// Two-world co-op, slice 1: each copy sends its player's transform and velocity over UDP; the other copy shows a
// puppet (an NPC body with AI possession disabled) that follows it with interpolation.
// Config (environment): TOW2_COOP_ROLE=host|client, TOW2_COOP_PORT (host: listen port, default 27960),
// TOW2_COOP_PEER=ip:port (client: the host), TOW2_COOP_BIND, TOW2_COOP_KEY (optional shared secret: players with another
// key are ignored). A player's install sets the same keys without the prefix in
// tow2coop.ini next to this DLL, section [coop]; the environment wins (test copies).
// Everything here runs on the game thread (coop_frame from the tick hook).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "coop.hpp"
#include "ue.hpp"
#include "story.hpp"
#include "join.hpp"
#include "steamnet.hpp"
#include "character.hpp"
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <array>
#include <mutex>

void core_log(const std::string& line);  // main.cpp: a line in tow2coop.log (what a player sends when something fails)

namespace coop {
using json = coopbridge::json::Value;
std::atomic<int> watch_count{0};
void* watch[8];
namespace {
// ---------------------------------------------------------------- interactions (doors, containers): shared state
// Rules: door state is shared (either player opens, closes or locks; the other world follows); container contents are
// shared (an item taken by one player disappears from that container in the other world; the taker keeps it).
struct Observed {
    void* obj;
    void* fn;
    uint8_t parms[64];
};
std::mutex obs_mu;
std::vector<Observed> observed;
volatile bool applying_remote = false;  // set while we apply a remote change: its events must not echo back
#pragma pack(push, 1)
struct InteractMsg {
    uint32_t eid;   // sender-unique event id (dedupe of the 3 copies)
    uint8_t kind;   // 1 door state, 2 item taken, 3 item stored, 4 XP earned, 5 item given, 6 gift received (id: its eid)
    uint64_t id;    // fnv64 of the actor path (level-placed actors have the same path in both worlds); XP: EXPGainReason;
                    // gift: item level | rarity << 32
    int32_t a;      // door: EOCLState; items: count; XP: amount before the receiver's own multipliers
    uint16_t len;   // item class path length (kinds 2, 3, 5; a gift: "<item class>|<mod class>|...")
    char path[1024];
};
#pragma pack(pop)
struct Outgoing {
    InteractMsg m;
    int size;
    double at[3];
    int sent;
};
constexpr uint32_t kMagic = 0x33433254;  // "T2C3": the last character is the protocol version, bump it on any packet change
constexpr const char* kVersion = "0.2.0-beta.6";
constexpr uint16_t kState = 1, kHello = 2, kBye = 3, kEnemies = 4, kDamage = 5, kResync = 8, kResyncRequest = 9, kInteract = 10, kDamageAck = 11, kLook = 12, kAppearance = 13, kTravel = 14, kClock = 15, kCombat = 16;
constexpr const char* kPuppetClass = "/Game/Blueprints/NPC/Townies/Base_Townie_Variants/Townie_DockWorker_Male.Townie_DockWorker_Male_C";
constexpr const char* kPuppetClassFemale = "/Game/Blueprints/NPC/Townies/Base_Townie_Variants/Townie_DockWorker_Female.Townie_DockWorker_Female_C";

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t type, size;  // size of the whole packet
    uint32_t seq, sender, t_ms;
};
struct State {
    Header h;
    double px, py, pz;
    float yaw, pitch, vx, vy, vz;
    uint32_t map_hash;
    uint8_t flags;  // bit 0: this player is dying or dead (its stand-in leaves the fights)
};
// Host -> client: authoritative NPC state. Ids are a 64-bit hash of the actor's path, which is the same in every
// world for level-placed (World Partition, "_UAID_") actors.
struct EnemyRec {
    uint64_t id;
    float px, py, pz, yaw, health;
    uint8_t dead;
    uint8_t target;  // whom it fights in the world that decides its fight: a Fight value
    uint8_t flags;   // bit 0: the client's world decides its fight (the client is much nearer than the host)
    uint64_t other;  // kSomeoneElse: that NPC's registry id, or a companion's ("party:" + its class)
};
constexpr int kMaxEnemyRecs = 32;  // 26 + 32 * 43 bytes: one packet fits the 1500-byte receive buffer (48 did not)
struct Enemies {
    Header h;
    uint32_t map_hash;
    uint16_t count;
    uint8_t fight;  // the party fights (either player): the stand-ins are on the players' team
    EnemyRec rec[kMaxEnemyRecs];
};
// Client -> host: the fights the client's world decides (kind 1: NPCs much nearer the client than the host) and hints
// (kind 2: the client's copy of an NPC fights the client while the host's copy fights nobody: the client was seen first).
struct CombatRec {
    uint64_t id;
    float px, py, pz;
    uint8_t target;  // a Fight value, as seen in the client's world
    uint8_t kind;
    uint64_t other;  // as EnemyRec.other
};
constexpr int kMaxCombatRecs = 40;
struct CombatMsg {
    Header h;
    uint32_t map_hash;
    uint16_t count;
    uint8_t fight;  // the client's player is in combat in its world
    CombatRec rec[kMaxCombatRecs];
};
// Client -> host: damage the client's player dealt to an NPC in the client's world.
struct Damage {
    Header h;
    uint32_t eid;  // client-unique; the host acknowledges it and applies each eid once
    uint64_t id;
    float amount;
};
// Either way, every 2 s (UDP may drop one): the armor set and helmet the player wears, class paths, empty = none,
// and whether the helmet shows (none worn, or hidden: the stand-in body would show its own cap, probe 53).
struct Look {
    char item[2][256];
    uint8_t helmet_shown;
};
struct FacePart { uint32_t version; uint8_t part, parts; uint16_t len; char text[1100]; };  // one piece of a player's look
struct TravelMsg {
    uint32_t id;     // the travel's sender-unique id
    uint8_t ack;     // 0 a travel starts (path: the destination data asset), 1 the receiver follows it, 2 it does not (path: why)
    uint16_t len;
    char path[256];
};
#pragma pack(pop)

// Whom an NPC fights, in both players' terms (the same value in both worlds).
enum Fight : uint8_t { kNobody = 0, kHostPlayer = 1, kClientPlayer = 2, kSomeoneElse = 3 };

struct Npc {
    ue::UObject *actor = nullptr, *health = nullptr;
    float host_health = -1;  // client: last value applied from the host; -1 = none yet
    float pending_damage = 0;
    // co-op combat
    bool ai = false;                 // an IndianaAiCharacter (it can fight)
    ue::UClass *cls = nullptr, *health_cls = nullptr, *ctrl_cls = nullptr;  // what actor, health and controller were
    ue::UObject* ctrl = nullptr;     // its AI controller
    bool client_owned = false;       // the client's world decides its fight
    uint8_t target = kNobody;        // the deciding world's last word on whom it fights
    uint64_t other = 0;              // and who, when it is neither player (EnemyRec.other)
    ue::UObject* forced = nullptr;   // the player (or stand-in) our copy is pinned on, in the world that follows
    uint8_t handle[0x18] = {};       // that pin: the PriorityNodeHandle of ForceOverridePrimaryTarget
    bool has_handle = false;
    double next_force = 0, next_hint = 0, rec_at = -100, owner_at = -100;
};

struct Snapshot {
    double t;  // receiver time, seconds
    double px, py, pz, vx, vy, vz;
    float yaw;
};

struct Ctx {
    bool on = false, host = false;
    SOCKET s = INVALID_SOCKET;
    sockaddr_in peer{};
    bool have_peer = false;
    bool steam = false;            // transport=steam: the peer is a SteamID (peer_id), found through rich presence
    uint64_t peer_id = 0;
    double next_discovery = 0;
    int session = 0;               // last Steam session state with the peer (logged when it changes)
    uint32_t me = 0, seq = 0, remote_id = 0, remote_last_seq = 0;
    uint32_t magic = kMagic;  // kMagic mixed with the hash of the optional shared key: other keys never become the peer
    uint64_t sent = 0, recv = 0, stale = 0, bad = 0;
    double next_send = 0, last_recv_t = -1;
    std::deque<Snapshot> snaps;
    ue::UObject* puppet = nullptr;
    uint32_t puppet_map = 0;
    std::string last_error;
    double now = 0;
    uint32_t my_map = 0;
    // NPC registry, rebuilt by an incremental scan of the object array (a slice per frame, no hitch).
    std::unordered_map<uint64_t, Npc> npcs, scanning;
    int scan_cursor = 0;
    ue::UClass* character_class = nullptr;
    double next_npc_send = 0;
    uint64_t npc_eased = 0, npc_fight_snapped = 0;
    uint64_t npc_sent = 0, npc_applied = 0, npc_killed = 0, npc_snapped = 0, dmg_sent = 0, dmg_applied = 0, drops = 0, map_changes = 0;
    double settle_until = 0, next_tame = 0;
    bool arrived_by_travel = false;  // this world came with a travel (the TravelManager still had a destination)
    // resync: the host hands its campaign over again (save + TCP) when the client cannot follow (map, quest outcome)
    uintptr_t world_ptr = 0, saves_blocked_world = 0;
    int port = 27960;
    double resync_cooldown = 0, mismatch_since = -1, next_resync_send = 0, fetch_quiet_until = 0;
    bool push_pending = false;  // host: its map changed; the client follows as soon as the host can save
    double next_push_try = 0, push_ready_at = 0;
    std::vector<std::pair<ue::UClass*, double>> my_removals;  // items that just left the local player's inventory  // push_ready_at: gameplay seen + 2 s
    uintptr_t snapped_world = 0;  // client: the world where it was put next to the host after a hand-over
    bool puppet_in_air = false;
    int resync_sends = 0;
    uint64_t resyncs = 0, resync_requests = 0, resync_fetches = 0;
    bool watches_set = false;
    struct PendingDamage { Damage d; double next; int tries; };
    std::vector<PendingDamage> dmg_pending;   // client: until the host acknowledges
    std::vector<uint32_t> dmg_seen;           // host: eids already applied
    uint32_t next_dmg_eid = 1;
    double start_time = 0, next_join_request = 0, map_since = 0;
    bool at_menu = false;
    bool opt_story = true, opt_doors = true, opt_npcs = true, opt_scan = true, opt_follow = true;  // coop.set
    uint64_t dmg_resent = 0, join_requests = 0;
    uint32_t next_eid = 1;
    std::vector<Outgoing> outgoing;
    std::vector<uint32_t> seen_eids;
    std::unordered_map<uint64_t, ue::UObject*> interactables, inter_scan;  // doors + containers by path id
    ue::UClass *door_class = nullptr, *container_class = nullptr, *corpse_class = nullptr;
    double interactables_built = -100;
    uint64_t doors_sent = 0, doors_applied = 0, items_sent = 0, items_applied = 0;
    std::string last_interact;
    std::string resync_reason;
    bool in_gameplay = false;
    uint64_t brain_stops = 0;
    std::string look[2], look_applied;  // the other player's armor set and helmet; what the stand-in shows
    bool have_look = false, look_helmet_shown = true;
    double next_look = 0;
    std::string face_in, face_applied;  // the other player's look (text), and the one on the stand-in
    uint32_t face_version = 0;          // the look being assembled from its pieces
    std::vector<std::string> face_parts;
    double next_face = 0;
    std::unordered_map<uint64_t, std::array<double, 3>> box_pos;                        // containers don't move
    std::unordered_map<uint64_t, std::unordered_map<ue::UClass*, int32_t>> box_counts;  // what those near the player hold
    double next_box_check = 0;
    std::vector<std::pair<int32_t, uint8_t>> xp_inbox;  // XP the other player earned, until there is a player to give it to
    uint64_t xp_sent = 0, xp_applied = 0, xp_blocked = 0, xp_details_filled = 0;
    std::string last_xp;
    ue::UObject *travel_seen = nullptr, *travel_following = nullptr;  // our travel destination last frame; the one we follow
    uint32_t travel_id = 0, travel_acked = 0, travel_in_id = 0;       // our last travel, its answer; the other player's
    uint64_t travel_in_key = 0;
    std::string travel_in, last_travel;                              // the other player's destination, until we follow
    double travel_in_at = 0, follow_until = 0;                       // host: the client comes by itself until then
    std::vector<std::pair<TravelMsg, double>> travel_out;            // copies to send, and when
    uint64_t travels_sent = 0, travels_followed = 0, travels_refused = 0;
    struct Gift { InteractMsg m; int size; double next, until; ue::UClass* item; int32_t count, level, rarity; std::vector<ue::UClass*> mods; };
    std::vector<Gift> gifts_out;                   // given to the other player, until they confirm
    std::vector<InteractMsg> gift_inbox;           // given to us, until there is a player to give them to
    ue::UObject *transfer_widget = nullptr, *give_box = nullptr;  // the transfer screen we opened, on its hidden chest
    double next_give_check = 0, next_close = 0;
    uint64_t gifts_sent = 0, gifts_received = 0, gifts_returned = 0;
    std::string last_gift;
    bool opt_gifts = true;  // test switch: false = take no gift and answer none (the giver gets it back)
    bool opt_give_hold = false;  // test switch: the chest is not emptied (a test puts a modded item in it first)
    double reload_handed_at = -1e9;   // host: when it sent the save its game is loading (no hand-over once loaded)
    bool died = false, campaign_wanted = false;  // client: dead now; the host's current game is asked for
    double next_life_check = 0, next_campaign_ask = 0;
    uint64_t deaths = 0, loads_redirected = 0, reloads_handed = 0;
    double next_clock = 0, host_hour = -1, host_hour_at = 0;  // the host's calendar hour, when it came
    bool clock_held = false;                                  // client: its calendar paused, the host's is behind
    uint32_t resync_kind = 0;                                 // host: 1 = the hand-over is a save its game loads
    uint64_t clock_jumps = 0, clock_holds = 0;
    bool host_paused = false;
    bool opt_combat = true;  // coop.set combat: false = each world fights on its own (as before)
    double next_combat_send = 0;
    uint64_t combat_pins = 0, combat_detections = 0, combat_hints_sent = 0, combat_hints_in = 0, combat_recs_in = 0, combat_handoffs = 0;
    int combat_fights = 0, combat_owned = 0;  // host: NPCs fighting a player now; those the client's world decides
    std::unordered_map<uint64_t, ue::UObject*> party, party_scan;  // this world's companions, by "party:" + class
    ue::UObject* standin_team_for = nullptr;  // the stand-in whose team was last set, and to what
    ue::UClass* puppet_cls = nullptr;         // the stand-in's class (identity check, see standin_ok)
    bool standin_fighting = false, peer_fight = false;
    double party_fight_until = 0, hint_fight_until = 0, peer_fight_at = -100;
    uint64_t standin_team_changes = 0;
    std::string standin_spell;  // the last CastSpell answer for the stand-in (ESpellCastResult), or "removed"
    bool me_dead = false, peer_dead = false, opt_fake_dead = false;  // opt_fake_dead: test switch, we say we are dead
    bool party_fighting = false;
    double npcs_sent_at = -100;
    double next_dead_check = 0;
} g;

uint64_t fnv64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (char c : s) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    return h;
}

uint32_t fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return h;
}

double clock_s() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / f.QuadPart;
}

std::string ini_path() {  // tow2coop.ini next to this DLL; empty if the DLL path is unknown (never a relative name:
    char p[MAX_PATH];         // GetPrivateProfileString would look for it in the Windows folder)
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&ini_path), &m);
    DWORD n = m ? GetModuleFileNameA(m, p, MAX_PATH) : 0;
    std::string s(p, n && n < MAX_PATH ? n : 0);
    size_t slash = s.find_last_of('\\');
    return slash == std::string::npos ? std::string() : s.substr(0, slash + 1) + "tow2coop.ini";
}

std::string env(const char* n) {
    char b[256];
    DWORD k = GetEnvironmentVariableA(n, b, sizeof b);
    if (k && k < sizeof b) return std::string(b, k);
    static const std::string ini = ini_path();
    if (ini.empty() || strncmp(n, "TOW2_COOP_", 10) != 0) return std::string();
    std::string key(n + 10);  // TOW2_COOP_PEER -> peer
    for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    k = GetPrivateProfileStringA("coop", key.c_str(), "", b, sizeof b, ini.c_str());
    return std::string(b, k);
}

// A UFunction with its parameter offsets, looked up once.
struct Fn {
    ue::UObject* obj_class = nullptr;
    ue::UFunction* f = nullptr;
    int size = 0;
    bool init(ue::UObject* on, const char* name) {
        if (f && obj_class == ue::class_of(on)) return true;
        obj_class = ue::class_of(on);
        f = ue::find_func(obj_class, name);
        if (!f) return false;
        size = 0;
        for (ue::FField* p = ue::first_prop(f); p; p = ue::next_prop(p))
            size = std::max(size, ue::prop_offset(p) + ue::prop_size(p));
        return true;
    }
    int off(const char* param) const {
        for (ue::FField* p = ue::first_prop(f); p; p = ue::next_prop(p))
            if (ue::field_name(p) == param) return ue::prop_offset(p);
        return -1;
    }
};

bool base_fn(Fn& f, const char* cls, const char* name);  // co-op combat, below

ue::UObject* local_pawn() {
    std::string err;
    return ue::resolve("pawn", &err);
}

bool read_transform(ue::UObject* pawn, double pos[3], float& yaw, float& pitch, double vel[3]) {
    static Fn loc, rot, vel_fn;
    if (!base_fn(loc, "/Script/Engine.Actor", "K2_GetActorLocation") || !base_fn(rot, "/Script/Engine.Actor", "K2_GetActorRotation") || !base_fn(vel_fn, "/Script/Engine.Actor", "GetVelocity")) return false;
    alignas(16) uint8_t b[256] = {};
    ue::process_event(pawn, loc.f, b);
    memcpy(pos, b + loc.off("ReturnValue"), 24);
    memset(b, 0, sizeof b);
    ue::process_event(pawn, rot.f, b);
    double r[3];
    memcpy(r, b + rot.off("ReturnValue"), 24);  // Pitch, Yaw, Roll
    pitch = static_cast<float>(r[0]);
    yaw = static_cast<float>(r[1]);
    memset(b, 0, sizeof b);
    ue::process_event(pawn, vel_fn.f, b);
    memcpy(vel, b + vel_fn.off("ReturnValue"), 24);
    return true;
}

void send_raw(const void* p, int n) {
    if (!g.have_peer) return;
    if (g.steam) {
        if (steamnet::send(g.peer_id, p, n, false, 0) == 1) g.sent++;
        return;
    }
    if (sendto(g.s, static_cast<const char*>(p), n, 0, reinterpret_cast<const sockaddr*>(&g.peer), sizeof g.peer) == n) g.sent++;
}

void fill_header(Header& h, uint16_t type, uint16_t size) {
    h.magic = g.magic, h.type = type, h.size = size, h.seq = ++g.seq, h.sender = g.me, h.t_ms = static_cast<uint32_t>(g.now * 1000);
}

// ---------------------------------------------------------------- NPCs (host authority over health and death)
float npc_health(Npc& n) {
    static Fn get;
    if (!base_fn(get, "/Script/Arkansas.HealthComponent", "GetHealth")) return -1;
    alignas(16) uint8_t b[32] = {};
    ue::process_event(n.health, get.f, b);
    float v;
    memcpy(&v, b + get.off("ReturnValue"), 4);
    return v;
}

void npc_set_health(Npc& n, float hp) {
    static Fn set;
    if (!base_fn(set, "/Script/Arkansas.HealthComponent", "SetHealth")) return;
    alignas(16) uint8_t b[64] = {};
    memcpy(b + set.off("Health"), &hp, 4);
    ue::process_event(n.health, set.f, b);
}

void npc_kill(Npc& n) {
    static Fn die;
    if (!base_fn(die, "/Script/Arkansas.HealthComponent", "Suicide")) return;
    alignas(16) uint8_t b[64] = {};
    b[die.off("bPreventRevival")] = 1;
    // A death applied from the other world: its own inventory changes happen there too, they are not loot to mirror.
    struct Applying {
        bool was = applying_remote;
        Applying() { applying_remote = true; }
        ~Applying() { applying_remote = was; }
    } guard;
    ue::process_event(n.health, die.f, b);
}

bool is_companion(ue::UObject* o);  // co-op combat, below

// One slice of the object array per frame; a full pass replaces the registry.
void scan_step(ue::UObject* pawn) {
    if (!g.character_class) g.character_class = ue::find_class("/Script/Engine.Character");
    if (!g.door_class) g.door_class = ue::find_class("/Script/Arkansas.Door");
    if (!g.container_class) g.container_class = ue::find_class("/Script/Arkansas.Container");
    if (!g.corpse_class) g.corpse_class = ue::find_class("/Script/Arkansas.Corpse");
    if (!g.character_class) return;
    int total = ue::num_objects(), end = std::min(total, g.scan_cursor + 20000);
    for (int i = g.scan_cursor; i < end; i++) {
        ue::UObject* o = ue::object_at(i);
        if (!o || o == pawn || o == g.puppet || ue::is_cdo(o)) continue;
        if ((g.door_class && ue::is_a(o, g.door_class)) || (g.container_class && ue::is_a(o, g.container_class)) ||
            (g.corpse_class && ue::is_a(o, g.corpse_class))) {
            if (ue::alive(o)) g.inter_scan[fnv64(ue::path_of(o))] = o;
            continue;
        }
        if (!ue::is_a(o, g.character_class) || !ue::alive(o)) continue;
        std::string path = ue::path_of(o);
        if (path.find("_UAID_") == std::string::npos) {  // only level-placed actors have the same name everywhere
            if (o != g.puppet && is_companion(o)) g.party_scan[fnv64("party:" + ue::class_name(o))] = o;  // and companions, by class
            continue;
        }
        ue::UObject* hc = ue::resolve(ue::addr(o) + ".HealthComponent");
        if (!hc) continue;
        uint64_t id = fnv64(path);
        static ue::UClass* ai_class = ue::find_class("/Script/Arkansas.IndianaAiCharacter");
        Npc n;  // its live state (health, damage, fight, pin) joins it when the registries swap
        n.actor = o, n.health = hc;
        n.cls = ue::class_of(o), n.health_cls = ue::class_of(hc);
        n.ai = ai_class && ue::is_a(o, ai_class);
        g.scanning[id] = n;
    }
    g.scan_cursor = end;
    if (end >= total) {
        // The live state goes over now, not when the scan reached the NPC: a pin taken or released since then came
        // back stale and was released twice (run 99, the client's game crashed). A pin moves, never two copies.
        for (auto& kv : g.scanning) {
            auto old = g.npcs.find(kv.first);
            if (old == g.npcs.end() || old->second.actor != kv.second.actor) continue;
            Npc fresh = kv.second;
            kv.second = old->second;
            kv.second.health = fresh.health, kv.second.ai = fresh.ai, kv.second.cls = fresh.cls, kv.second.health_cls = fresh.health_cls;
            old->second.has_handle = false;
        }
        // An NPC that did not come back went away (streamed out, destroyed): its pin went with its controller.
        g.npcs.swap(g.scanning);
        g.scanning.clear();
        g.party.swap(g.party_scan);
        g.party_scan.clear();
        g.interactables.swap(g.inter_scan);
        g.inter_scan.clear();
        g.scan_cursor = 0;
        g.interactables_built = g.now;
    }
}

bool near_players(const double p[3], ue::UObject* pawn) {
    double me[3], v[3];
    float y, pi;
    if (!read_transform(pawn, me, y, pi, v)) return false;
    auto d2 = [&](double x, double yy, double z) { return (p[0] - x) * (p[0] - x) + (p[1] - yy) * (p[1] - yy) + (p[2] - z) * (p[2] - z); };
    const double r2 = 8000.0 * 8000.0;  // 80 m
    if (d2(me[0], me[1], me[2]) < r2) return true;
    return !g.snaps.empty() && d2(g.snaps.back().px, g.snaps.back().py, g.snaps.back().pz) < r2;
}

// ---------------------------------------------------------------- Co-op combat
// Both worlds run the game's own AI. One world decides each NPC's fight: the host's, or the client's when the client is
// much nearer (an NPC far from a world's player has its AI asleep there: probe 90, no detection 200 m from the host).
// That world says whom the NPC fights; in the other world our copy of the NPC is made to detect that player
// (ForceFullDetection) and is pinned on them (ForceOverridePrimaryTarget, activated). Each player takes the hits of
// the copies that fight them in their own world; the stand-ins take the others (god mode).

// A held pointer can be alive and be another object: an unloaded object's memory and slot get reused (run 106: the
// spell class pointer led to a StaticMeshActor). Identity is checked with the class too.
bool npc_live(const Npc& n) {
    return ue::alive(n.actor) && ue::class_of(n.actor) == n.cls && ue::alive(n.health) && ue::class_of(n.health) == n.health_cls;
}

bool standin_ok() { ue::UObject* p = g.puppet; return p && ue::alive(p) && ue::class_of(p) == g.puppet_cls; }

// A function declared on a base class, found once: callable on any object of that class.
bool base_fn(Fn& f, const char* cls, const char* name) {
    if (f.f) return true;
    ue::UClass* c = ue::find_class(cls);
    return c && f.init(ue::cdo(c), name);
}

ue::UObject* npc_ctrl(Npc& n) {
    if (n.ctrl && ue::alive(n.ctrl) && ue::class_of(n.ctrl) == n.ctrl_cls) return n.ctrl;
    static ue::UClass* ai_ctrl = ue::find_class("/Script/Arkansas.IndianaAiController");
    std::string err;
    n.ctrl = ue::resolve(ue::addr(n.actor) + ".Controller", &err);
    if (n.ctrl && (!ai_ctrl || !ue::is_a(n.ctrl, ai_ctrl))) n.ctrl = nullptr;
    n.ctrl_cls = n.ctrl ? ue::class_of(n.ctrl) : nullptr;
    return n.ctrl;
}

// The actor this NPC fights in this world now (none when it is not in combat).
ue::UObject* fighting(Npc& n) {
    static Fn combat, tgt;
    ue::UObject* c = npc_ctrl(n);
    if (!c || !base_fn(combat, "/Script/Arkansas.OwBaseCharacter", "IsInCombat") || !base_fn(tgt, "/Script/Arkansas.IndianaAiController", "K2_GetCurrentTarget")) return nullptr;
    alignas(16) uint8_t b[64] = {};
    ue::process_event(n.actor, combat.f, b);
    if (!b[combat.off("ReturnValue")]) return nullptr;
    memset(b, 0, sizeof b);
    ue::process_event(c, tgt.f, b);
    ue::UObject* t = nullptr;
    memcpy(&t, b + tgt.off("ReturnValue"), sizeof t);
    return t && ue::alive(t) ? t : nullptr;
}

uint8_t fight_of(ue::UObject* t, ue::UObject* pawn) {  // an actor of this world as a Fight value
    if (!t) return kNobody;
    if (t == pawn) return g.host ? kHostPlayer : kClientPlayer;
    if (t == g.puppet) return g.host ? kClientPlayer : kHostPlayer;
    return kSomeoneElse;
}

ue::UClass* load_class(const std::string& path);                          // below
void call_quiet(const std::string& ref, const char* fn, const json& args);  // below

bool pawn_in_combat(ue::UObject* pawn) {
    static Fn f;
    if (!pawn || !base_fn(f, "/Script/Arkansas.OwBaseCharacter", "IsInCombat")) return false;
    alignas(16) uint8_t b[32] = {};
    ue::process_event(pawn, f.f, b);
    return b[f.off("ReturnValue")] != 0;
}

void unpin(Npc& n);  // below

// A Blueprint class kept between frames: loaded again when the game unloaded it (nothing roots it; run 102: the
// spell class was gone a minute later).
ue::UClass* kept_class(ue::UClass*& cache, const char* path) {
    if (!cache || !ue::alive(cache) || ue::path_of(cache) != path) cache = load_class(path);
    return cache;
}

// The stand-in is on its player's team while the party fights (enemies may pick it, the fight splits), neutral to all
// otherwise: out of a fight each player is only seen in their own world, with their own stealth (the stand-in never
// crouches; creatures attacked the client's stand-in in the host's world on their own, probe 90).
void standin_team(bool fight) {
    if (!g.puppet || !standin_ok()) return;
    if (g.standin_team_for == g.puppet && g.standin_fighting == fight) return;
    ue::UObject* team = nullptr;
    std::string err;
    if (fight) team = ue::resolve("pawn.TeamComponent.CurrentTeamData", &err);
    else {
        static ue::UClass* rest = nullptr;
        team = kept_class(rest, "/Game/Blueprints/AI/Teams/Teams/T_Generic_NeutralToAll.T_Generic_NeutralToAll_C");
    }
    if (!team) return;
    json t = json::object();
    t.set("InTeamData", ue::addr(team));
    call_quiet(ue::addr(g.puppet) + ".TeamComponent", "SetTeam", t);
    // A team alone is not enough: creatures went for a stand-in on T_Generic_NeutralToAll too (run 100). Out of a
    // fight it is untargetable by AI, with the game's own spell for that.
    static ue::UClass* spell_cache = nullptr;
    ue::UClass* spell = kept_class(spell_cache, "/Game/Blueprints/Spells/AI/Spell_MakeNpcUntargetable.Spell_MakeNpcUntargetable_C");
    ue::UFunction* f = spell ? ue::find_func(ue::class_of(g.puppet), fight ? "RemoveSpell" : "CastSpell") : nullptr;
    if (f) {
        json a = json::object();
        a.set("SpellType", ue::addr(spell));
        try {
            json r = ue::call(g.puppet, f, a);
            g.standin_spell = fight ? std::string("removed") : r.get("ReturnValue").asString();
        } catch (const std::exception& e) { g.standin_spell = std::string("error: ") + e.what(); }
    } else {
        g.standin_spell = "no spell";
    }
    g.standin_team_for = g.puppet, g.standin_fighting = fight;
    g.standin_team_changes++;
    core_log(std::string("combat: the stand-in ") + (fight ? "is in the fight" : "is out of the fight, untargetable: " + g.standin_spell));
}

// Either player fights (lasts 5 s after the last sign): the party's fight.
bool party_fight(ue::UObject* pawn) {
    bool sign = pawn_in_combat(pawn) || (g.peer_fight && g.now - g.peer_fight_at < 1.0);
    if (g.host) sign = sign || g.combat_fights > 0 || g.now < g.hint_fight_until;
    if (sign) g.party_fight_until = g.now + 5.0;
    bool on = g.now < g.party_fight_until;
    if (on != g.party_fighting) g.party_fighting = on, core_log(on ? "combat: the party fights" : "combat: the party's fight is over");
    return on;
}

bool is_companion(ue::UObject* o) {
    static ue::UClass* ai_class = ue::find_class("/Script/Arkansas.IndianaAiCharacter");
    static ue::UClass* ai_ctrl = ue::find_class("/Script/Arkansas.IndianaAiController");
    static Fn f;
    if (!ai_class || !ai_ctrl || !ue::is_a(o, ai_class) || !base_fn(f, "/Script/Arkansas.IndianaAiController", "K2_IsCompanion")) return false;
    std::string err;
    ue::UObject* c = ue::resolve(ue::addr(o) + ".Controller", &err);
    if (!c || !ue::is_a(c, ai_ctrl)) return false;
    alignas(16) uint8_t b[32] = {};
    ue::process_event(c, f.f, b);
    return b[f.off("ReturnValue")] != 0;
}

uint64_t other_id(ue::UObject* t) {  // the other side of a fight that is neither player, as the other world finds it
    if (!t) return 0;
    std::string path = ue::path_of(t);
    if (path.find("_UAID_") != std::string::npos) return fnv64(path);
    return fnv64("party:" + ue::class_name(t));
}

ue::UObject* other_actor(uint64_t id) {
    auto it = g.npcs.find(id);
    if (it != g.npcs.end() && npc_live(it->second)) return it->second.actor;
    auto p = g.party.find(id);  // a companion: its class is in its key
    return p != g.party.end() && ue::alive(p->second) && fnv64("party:" + ue::class_name(p->second)) == id ? p->second : nullptr;
}

void force_detect(Npc& n, ue::UObject* who) {
    static Fn f, loc;
    ue::UObject* c = npc_ctrl(n);
    std::string err;
    ue::UObject* det = c ? ue::resolve(ue::addr(c) + ".AIDetectionComponent", &err) : nullptr;
    if (!det || !base_fn(f, "/Script/Arkansas.AIDetectionComponent", "ForceFullDetection") || !base_fn(loc, "/Script/Engine.Actor", "K2_GetActorLocation")) return;
    alignas(16) uint8_t b[128] = {};
    if (f.size > static_cast<int>(sizeof b)) return;
    ue::process_event(who, loc.f, b);
    double p[3];
    memcpy(p, b + loc.off("ReturnValue"), 24);
    memset(b, 0, sizeof b);
    memcpy(b + f.off("DetectedActor"), &who, sizeof who);
    memcpy(b + f.off("ExternalStimulusLocation"), p, 24);
    b[f.off("bPermitArkansasAI")] = 1;
    ue::process_event(det, f.f, b);
    g.combat_detections++;
}

const wchar_t kPinFrom[] = L"tow2coop";
ue::UObject* priority_lib(const char* cls) {
    ue::UClass* c = ue::find_class(cls);
    return c ? ue::cdo(c) : nullptr;
}

bool pin_target(Npc& n, ue::UObject* who) {
    static Fn over, act;
    static ue::UObject* lib = priority_lib("/Script/Arkansas.AITargetInfoPriorityFunctionLibrary");
    static ue::UObject* nodes = priority_lib("/Script/Arkansas.PriorityNodeFunctionLibrary");
    ue::UObject* c = npc_ctrl(n);
    if (!c || !lib || !nodes || !over.init(lib, "ForceOverridePrimaryTarget") || !act.init(nodes, "Activate")) return false;
    alignas(16) uint8_t b[128] = {};
    if (over.size > static_cast<int>(sizeof b) || act.size > static_cast<int>(sizeof b)) return false;
    memcpy(b + over.off("Controller"), &c, sizeof c);
    b[over.off("Priority")] = 175;  // EPriorityNodeValue::High
    memcpy(b + over.off("ForcedTargetActor"), &who, sizeof who);
    ue::process_event(lib, over.f, b);
    uint8_t h[sizeof n.handle];
    memcpy(h, b + over.off("ReturnValue"), sizeof h);
    memset(b, 0, sizeof b);  // Activate(PriorityNodeHandle&, Owner, FromWhere): without it the pin does nothing (probe 90)
    memcpy(b + act.off("PriorityNodeHandle"), h, sizeof h);
    memcpy(b + act.off("Owner"), &c, sizeof c);
    ue::FString from{const_cast<wchar_t*>(kPinFrom), 9, 9};
    memcpy(b + act.off("FromWhere"), &from, sizeof from);
    ue::process_event(nodes, act.f, b);
    if (!b[act.off("ReturnValue")]) return false;
    memcpy(n.handle, b + act.off("PriorityNodeHandle"), sizeof n.handle);
    n.has_handle = true;
    g.combat_pins++;
    return true;
}

void unpin(Npc& n) {
    if (!n.has_handle) return;
    n.has_handle = false;
    static Fn de;
    static ue::UObject* nodes = priority_lib("/Script/Arkansas.PriorityNodeFunctionLibrary");
    if (!n.ctrl || !ue::alive(n.ctrl) || ue::class_of(n.ctrl) != n.ctrl_cls || !nodes || !de.init(nodes, "Deactivate")) return;  // the pin went with its controller
    alignas(16) uint8_t b[64] = {};
    if (de.size > static_cast<int>(sizeof b)) return;
    memcpy(b + de.off("PriorityNodeHandle"), n.handle, sizeof n.handle);
    ue::process_event(nodes, de.f, b);
}

// The deciding world's word on whom this NPC fights, applied to our copy: it detects that player (or their
// stand-in) and is pinned on them; looked at again every second (our copy may lose sight of them here).
void follow_fight(Npc& n, uint8_t word, ue::UObject* pawn) {
    ue::UObject* want = word == kHostPlayer ? (g.host ? pawn : g.puppet) : word == kClientPlayer ? (g.host ? g.puppet : pawn) : nullptr;
    if (word == kSomeoneElse && n.other) want = other_actor(n.other);  // the same companion or NPC in this world
    if (want && !ue::alive(want)) want = nullptr;
    if (want == g.puppet && g.peer_dead) want = nullptr;  // their stand-in left the fight
    if (want == n.forced && g.now < n.next_force) return;
    n.next_force = g.now + 1.0;
    if (want != n.forced) {
        unpin(n);
        n.forced = nullptr;
    }
    if (!want) return;
    if (want == g.puppet) standin_team(true), g.party_fight_until = std::max(g.party_fight_until, g.now + 5.0);
    if (fighting(n) != want) force_detect(n, want);
    if (!n.has_handle) pin_target(n, want);
    n.forced = want;
}

// Our copy drifted from the deciding world's: snap it there past 3 m, or, in a fight (both copies run their own
// fight and move a lot), ease it back by 15 % of the gap per update past 1.5 m and snap only past 6 m (run 98: 49
// snaps of 3 m and more in about 35 s of fighting).
void snap_npc(Npc& n, double x, double y, double z) {
    static Fn loc, set, slide;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    if (!base_fn(loc, "/Script/Engine.Actor", "K2_GetActorLocation") || !base_fn(set, "/Script/Engine.Actor", "K2_SetActorLocationAndRotation") ||
        !base_fn(slide, "/Script/Engine.Actor", "K2_SetActorLocation")) return;
    alignas(16) uint8_t b[512] = {};
    if (set.size > static_cast<int>(sizeof b) || slide.size > static_cast<int>(sizeof b)) return;
    ue::process_event(n.actor, loc.f, b);
    double p[3];
    memcpy(p, b + loc.off("ReturnValue"), 24);
    double dx = x - p[0], dy = y - p[1], dz = z - p[2];
    double d2 = dx * dx + dy * dy + dz * dz;
    bool fight = n.target != kNobody;
    double hard = fight ? 600.0 : 300.0;
    memset(b, 0, sizeof b);
    if (d2 > hard * hard) {
        double np[3] = {x, y, z};
        memcpy(b + set.off("NewLocation"), np, 24);
        b[set.off("bTeleport")] = 1;
        ue::process_event(n.actor, set.f, b);
        g.npc_snapped++;
        if (fight) g.npc_fight_snapped++;
    } else if (fight && d2 > 150.0 * 150.0) {
        double np[3] = {p[0] + dx * 0.15, p[1] + dy * 0.15, p[2] + dz * 0.15};
        memcpy(b + slide.off("NewLocation"), np, 24);
        b[slide.off("bSweep")] = 1;  // a wall or another body stops it
        ue::process_event(n.actor, slide.f, b);
        g.npc_eased++;
    }
}

double dist3(const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

// Host: which world decides this NPC's fight, and (when it is ours) whom it fights. p: its position, me: the host
// player's (null when unknown).
void host_fight(Npc& n, const double p[3], const double* me, ue::UObject* pawn) {
    bool fresh = !g.snaps.empty() && g.now - g.snaps.back().t < 2.0 && g.puppet_map == g.my_map;
    double dh = me ? dist3(p, me) : 1e12, dc = 1e12;
    if (fresh) {
        const Snapshot& s = g.snaps.back();
        double c[3] = {s.px, s.py, s.pz};
        dc = dist3(p, c);
    }
    bool own = fresh && (n.client_owned ? dh > 4000 && dc < dh - 1000 : dh > 5000 && dc < dh - 2000);  // 50 m out, 40 m back
    if (own != n.client_owned) {
        n.client_owned = own;
        unpin(n);
        n.forced = nullptr;
        n.target = kNobody, n.other = 0;
        n.owner_at = g.now;
        g.combat_handoffs++;
    }
    if (n.client_owned) {
        if (g.now - n.owner_at > 2.0) n.target = kNobody;  // no word from the client lately
        follow_fight(n, n.target, pawn);
        return;
    }
    ue::UObject* t = fighting(n);
    n.target = fight_of(t, pawn);
    n.other = n.target == kSomeoneElse ? other_id(t) : 0;
}

void host_send_npcs(ue::UObject* pawn) {
    Enemies e{};
    e.map_hash = g.my_map;
    static Fn loc;
    double me[3], v[3];
    float yaw, pitch;
    bool have_me = read_transform(pawn, me, yaw, pitch, v);
    bool sent = false;
    e.fight = g.opt_combat && party_fight(pawn) ? 1 : 0;
    if (g.opt_combat) standin_team(e.fight != 0);
    auto flush = [&] {
        uint16_t size = static_cast<uint16_t>(offsetof(Enemies, rec) + e.count * sizeof(EnemyRec));
        fill_header(e.h, kEnemies, size);
        send_raw(&e, size);
        g.npc_sent++;
        e.count = 0, sent = true;
    };
    int fights = 0, owned = 0;
    for (auto& kv : g.npcs) {
        Npc& n = kv.second;
        if (!npc_live(n) || !base_fn(loc, "/Script/Engine.Actor", "K2_GetActorLocation")) continue;
        alignas(16) uint8_t b[64] = {};
        ue::process_event(n.actor, loc.f, b);
        double p[3];
        memcpy(p, b + loc.off("ReturnValue"), 24);
        if (!near_players(p, pawn)) continue;
        EnemyRec& r = e.rec[e.count];
        r.id = kv.first, r.px = static_cast<float>(p[0]), r.py = static_cast<float>(p[1]), r.pz = static_cast<float>(p[2]);
        r.health = npc_health(n);
        r.dead = r.health <= 0;
        r.target = kNobody, r.flags = 0, r.other = 0;
        if (r.dead) {  // its last word must not stay (run 106: a dead Ursopod still "fought" the client)
            n.target = kNobody, n.other = 0;
            if (n.has_handle) unpin(n), n.forced = nullptr;
        }
        if (!r.dead && n.ai && g.opt_combat) {
            host_fight(n, p, have_me ? me : nullptr, pawn);
            r.target = n.target, r.flags = n.client_owned ? 1 : 0, r.other = n.other;
            fights += n.target == kHostPlayer || n.target == kClientPlayer;
            owned += n.client_owned;
        }
        if (++e.count == kMaxEnemyRecs) flush();  // more NPCs near: another packet
    }
    if (e.count || !sent) flush();
    g.combat_fights = fights, g.combat_owned = owned;
    g.npcs_sent_at = g.now;
}

// Client, 5 times a second: the fights its world decides, and hints.
void client_send_combat(ue::UObject* pawn) {
    CombatMsg m{};
    m.map_hash = g.my_map;
    static Fn loc;
    bool sent = false;
    m.fight = pawn_in_combat(pawn) ? 1 : 0;
    standin_team(party_fight(pawn));
    auto flush = [&] {
        uint16_t size = static_cast<uint16_t>(offsetof(CombatMsg, rec) + m.count * sizeof(CombatRec));
        fill_header(m.h, kCombat, size);
        send_raw(&m, size);
        m.count = 0, sent = true;
    };
    for (auto& kv : g.npcs) {
        Npc& n = kv.second;
        if (!n.ai || g.now - n.rec_at > 1.0 || !npc_live(n)) continue;  // only NPCs the host streams now
        if (!n.client_owned && (n.target != kNobody || g.now < n.next_hint)) continue;
        ue::UObject* t = fighting(n);
        if (!n.client_owned) {
            if (!t || t != pawn) continue;  // a hint: our copy fights our player, the host's copy fights nobody
            n.next_hint = g.now + 1.0;
            g.combat_hints_sent++;
        }
        if (!base_fn(loc, "/Script/Engine.Actor", "K2_GetActorLocation")) continue;
        alignas(16) uint8_t b[64] = {};
        ue::process_event(n.actor, loc.f, b);
        double p[3];
        memcpy(p, b + loc.off("ReturnValue"), 24);
        CombatRec& r = m.rec[m.count];
        r.id = kv.first, r.px = static_cast<float>(p[0]), r.py = static_cast<float>(p[1]), r.pz = static_cast<float>(p[2]);
        r.target = fight_of(t, pawn);
        r.other = r.target == kSomeoneElse ? other_id(t) : 0;
        r.kind = n.client_owned ? 1 : 2;
        if (++m.count == kMaxCombatRecs) flush();
    }
    if (m.count || !sent) flush();
}

// Host: the client's fights (NPCs its world decides) and hints (the client was seen first).
void host_apply_combat(const CombatMsg& m) {
    if (m.map_hash != g.my_map || !g.opt_combat) return;
    g.peer_fight = m.fight != 0, g.peer_fight_at = g.now;
    ue::UObject* pawn = local_pawn();
    for (int i = 0; i < m.count; i++) {
        const CombatRec& r = m.rec[i];
        auto it = g.npcs.find(r.id);
        if (it == g.npcs.end()) continue;
        Npc& n = it->second;
        if (!n.ai || !npc_live(n) || npc_health(n) <= 0) continue;
        g.combat_recs_in++;
        if (r.kind == 1 && n.client_owned) {
            n.target = r.target <= kSomeoneElse ? r.target : kNobody;
            n.other = r.other;
            n.owner_at = g.now;
            if (pawn) follow_fight(n, n.target, pawn);
            snap_npc(n, r.px, r.py, r.pz);
        } else if (r.kind == 2 && !n.client_owned && n.target == kNobody && g.now >= n.next_force && g.puppet && standin_ok() && !g.peer_dead) {
            g.hint_fight_until = g.now + 5.0;
            standin_team(true);
            force_detect(n, g.puppet);  // the client's copy saw the client first: ours notices the client's stand-in
            n.next_force = g.now + 1.0;
            g.combat_hints_in++;
        }
    }
}

void apply_npcs(const Enemies& e) {
    if (e.map_hash != g.my_map) return;
    g.peer_fight = e.fight != 0, g.peer_fight_at = g.now;
    ue::UObject* pawn = local_pawn();
    for (int i = 0; i < e.count; i++) {
        const EnemyRec& r = e.rec[i];
        auto it = g.npcs.find(r.id);
        if (it == g.npcs.end() || !npc_live(it->second)) continue;
        Npc& n = it->second;
        float local = npc_health(n);
        if (n.host_health >= 0 && local < n.host_health - 0.5f && local < r.health - 0.5f)
            n.pending_damage += n.host_health - local;  // our player hit it since the last update: tell the host
        if (r.dead && local > 0) { npc_kill(n); g.npc_killed++; }
        else if (!r.dead && std::fabs(local - r.health) > 0.5f) npc_set_health(n, r.health);
        n.host_health = r.health;
        g.npc_applied++;
        n.client_owned = (r.flags & 1) != 0;
        n.rec_at = g.now;
        n.target = r.target <= kSomeoneElse ? r.target : kNobody;
        n.other = r.other;
        if (r.dead) {  // its pin goes with it
            unpin(n);
            n.forced = nullptr;
            continue;
        }
        if (!n.client_owned) snap_npc(n, r.px, r.py, r.pz);  // a fight our world decides moves here, not there
        if (!n.ai || !g.opt_combat || !pawn) continue;
        if (n.client_owned) {
            unpin(n);
            n.forced = nullptr;
        } else {
            follow_fight(n, n.target, pawn);
        }
    }
}

void client_report_damage() {
    for (auto& kv : g.npcs) {
        Npc& n = kv.second;
        if (n.pending_damage <= 0.5f) continue;
        Damage d{};
        d.eid = (g.me << 16) ^ g.next_dmg_eid++;
        d.id = kv.first, d.amount = n.pending_damage;
        g.dmg_pending.push_back({d, 0.0, 0});
        n.pending_damage = 0;
        g.dmg_sent++;
    }
}

void resend_damage() {
    for (size_t i = 0; i < g.dmg_pending.size();) {
        auto& p = g.dmg_pending[i];
        if (g.now >= p.next) {
            if (p.tries >= 25) { g.dmg_pending.erase(g.dmg_pending.begin() + i); continue; }  // 5 s: the host is gone
            fill_header(p.d.h, kDamage, sizeof p.d);
            send_raw(&p.d, sizeof p.d);
            if (p.tries) g.dmg_resent++;
            p.tries++, p.next = g.now + 0.2;
        }
        i++;
    }
}

void apply_damage(const Damage& d) {
    auto it = g.npcs.find(d.id);
    if (it == g.npcs.end() || !npc_live(it->second) || d.amount <= 0 || d.amount > 1e6f) return;
    float hp = npc_health(it->second);
    if (hp <= 0) return;
    if (hp - d.amount <= 0) npc_kill(it->second);
    else npc_set_health(it->second, hp - d.amount);
    g.dmg_applied++;
}

std::string bind_ip() {
    std::string b = env("TOW2_COOP_BIND");
    return b.empty() ? std::string("0.0.0.0") : b;
}

bool request_resync(const char* reason, bool host_moved = false, const std::wstring& existing = L"") {
    if (!g.host || !g.have_peer || (!host_moved && g.now < g.resync_cooldown)) return false;
    try {
        char peer_ip[64] = "";
        inet_ntop(AF_INET, &g.peer.sin_addr, peer_ip, sizeof peer_ip);
        // the campaign goes to the joined player only: its address (TCP), or its SteamID (pushed over Steam)
        join::host_offer(g.port + 1, bind_ip(), peer_ip, g.steam ? g.peer_id : 0, g.me, existing);
    } catch (const std::exception& e) {
        g.last_error = std::string("resync offer: ") + e.what();
        static std::string logged;  // once per reason: the client asks again every few seconds
        if (logged != e.what()) core_log(std::string("hand-over waits: ") + (logged = e.what()));
        return false;
    }
    g.resync_cooldown = g.now + 60.0;  // one hand-over at a time; the client needs ~30-60 s to reload
    g.push_pending = false;
    g.resync_sends = 6, g.next_resync_send = g.now + (existing.empty() ? 2.0 : 0.0);  // a loaded save: no save to wait for
    g.resync_kind = existing.empty() ? 0 : 1;
    g.resyncs++;
    g.resync_reason = reason;
    return true;
}

void client_fetch_resync(const char* body, int len) {
    if (g.host || len != 16) return;
    uint32_t port, host_id, kind, id;  // kind 1: a save the host's game loads (a reload); id: which hand-over
    memcpy(&port, body, 4);
    memcpy(&host_id, body + 4, 4);
    memcpy(&kind, body + 8, 4);
    memcpy(&id, body + 12, 4);
    char ip[64] = "";
    inet_ntop(AF_INET, &g.peer.sin_addr, ip, sizeof ip);
    // The host repeats each notice: one fetch per hand-over (run 96: a 30 s quiet window after a death's fetch made
    // the next reload's first notice wait for the host's load to end, 9 s late).
    static uint64_t fetched = 0;
    uint64_t key = static_cast<uint64_t>(host_id) << 32 | id;
    if (key == fetched) return;
    try {
        if (g.steam) join::client_expect(host_id);  // the host pushes the files on Steam channel 1
        else join::client_fetch(ip, static_cast<int>(port), host_id);
        g.resync_fetches++;
        g.fetch_quiet_until = g.now + 30.0;
        fetched = key;
        g.campaign_wanted = false;
        if (kind == 1) character::use_kept_next();      // a reload: the character as last kept, no save to wait for
        else if (join::joined()) character::save_now();  // keep the character while the files come (not when dead)
    } catch (const std::exception&) {}  // already fetching
}

// Header + body, for the story channel (and later event types).
void send_typed(uint16_t type, const void* body, int len) {
    char buf[1500];
    if (len < 0 || len + static_cast<int>(sizeof(Header)) > static_cast<int>(sizeof buf)) return;
    Header h;
    fill_header(h, type, static_cast<uint16_t>(sizeof(Header) + len));
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, body, len);
    send_raw(buf, static_cast<int>(sizeof h) + len);
}

// Watch the game's own door/container events by name (see hk_pe in main.cpp).
void setup_watches() {
    if (g.watches_set) return;
    uint32_t b = ue::make_name("HandleItemRemoved", false).index;  // door state changes go through a native delegate: polled
    uint32_t c = ue::make_name("ItemRemoved", false).index;          // IndianaCharacter.ItemRemoved: a dead NPC looted
    uint32_t d = ue::make_name("OnItemCountChangedInInventory", false).index;  // part of the player's stack left (run 70)
    if (!b || !c || !d) return;
    watch[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(b));
    watch[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(c));
    watch[2] = reinterpret_cast<void*>(static_cast<uintptr_t>(d));
    watch_count.store(3, std::memory_order_release);
    g.watches_set = true;
}

int param_offset(ue::UFunction* f, const char* name) {
    for (ue::FField* p = ue::first_prop(f); p; p = ue::next_prop(p))
        if (ue::field_name(p) == name) return ue::prop_offset(p);
    return -1;
}

void queue_interact(uint8_t kind, uint64_t id, int32_t a, const std::string& path) {
    Outgoing o{};
    o.m.eid = (g.me & 0xFFFF0000u) | (g.next_eid++ & 0xFFFFu);
    o.m.kind = kind, o.m.id = id, o.m.a = a;
    o.m.len = static_cast<uint16_t>(std::min<size_t>(path.size(), sizeof o.m.path));
    memcpy(o.m.path, path.data(), o.m.len);
    o.size = static_cast<int>(offsetof(InteractMsg, path)) + o.m.len;
    o.at[0] = g.now, o.at[1] = g.now + 0.1, o.at[2] = g.now + 0.3;  // three copies: UDP
    g.outgoing.push_back(o);
}

// Game thread: turn observed events into messages.
void process_observed() {
    std::vector<Observed> evs;
    {
        std::lock_guard<std::mutex> lk(obs_mu);
        evs.swap(observed);
    }
    g.my_removals.erase(std::remove_if(g.my_removals.begin(), g.my_removals.end(), [](const std::pair<ue::UClass*, double>& r) { return g.now - r.second > 1.0; }),
                        g.my_removals.end());
    ue::UObject* me = local_pawn();
    std::string err;
    ue::UObject* my_inv = me ? ue::resolve(ue::addr(me) + ".InventoryComponent", &err) : nullptr;
    for (auto& e : evs) {  // items that left the local player (an item stored in a container may come first in the batch)
        auto* f = static_cast<ue::UFunction*>(e.fn);
        std::string fname = ue::name_of(f);
        bool removed = me && e.obj == me && fname == "ItemRemoved";  // a whole stack
        // Part of a stack: only its count changes (EquipmentComponent / InventoryAmmoPool handlers). A gain changes it
        // too; it only counts with a container add of the same class within 1 s.
        bool changed = false;
        if (my_inv && fname == "OnItemCountChangedInInventory") {
            int io = param_offset(f, "InInventoryComponent");
            ue::UObject* inv = nullptr;
            if (io >= 0 && io + 8 <= 64) memcpy(&inv, e.parms + io, 8);
            changed = inv == my_inv;
        }
        if (!removed && !changed) continue;
        int off = param_offset(f, removed ? "Item" : "ItemStack");
        ue::UObject* item = nullptr;
        if (off >= 0 && off + 8 <= 64) memcpy(&item, e.parms + off, 8);
        if (item && ue::alive(item)) g.my_removals.push_back({ue::class_of(item), g.now});
    }
    for (auto& e : evs) {
        auto* o = static_cast<ue::UObject*>(e.obj);
        auto* f = static_cast<ue::UFunction*>(e.fn);
        if (!ue::alive(o)) continue;
        std::string fname = ue::name_of(f), path = ue::path_of(o);
        if (fname == "OnOCLStateChange") {
            int off = param_offset(f, "CurrentState");
            if (off < 0 || off >= 64) continue;
            int32_t st = e.parms[off];
            if (st > 2) continue;  // Closed 0, Open 1, Locked 2; transitions and seals are not mirrored
            queue_interact(1, fnv64(path), st, "");
            g.doors_sent++;
            g.last_interact = "sent door " + path.substr(path.rfind('.') + 1) + " -> " + std::to_string(st);
        } else if (fname == "ItemRemoved") {  // containers: watch_boxes sees every take, whole stack or part of one
            int off = param_offset(f, fname == "ItemRemoved" ? "Item" : "ItemStack");
            if (off < 0 || off + 12 > 64) continue;
            ue::UObject* item;
            int32_t count;
            memcpy(&item, e.parms + off, 8);
            memcpy(&count, e.parms + off + 8, 4);
            if (!item || !ue::alive(item) || count <= 0) continue;
            ue::UClass* cls = ue::class_of(item);
            // ItemRemoved is any character's inventory: only a dead level-placed NPC counts (looted; both worlds know
            // it by name). A living NPC uses its own items, and each world runs its own AI (two-PC test: corpse loot).
            if (fname == "ItemRemoved") {
                if (o == g.puppet || o == local_pawn() || path.find("_UAID_") == std::string::npos) continue;
                std::string err;
                ue::UObject* hc = ue::resolve(ue::addr(o) + ".HealthComponent", &err);
                if (!hc || ue::call(hc, ue::find_func(ue::class_of(hc), "GetHealth"), json::object()).get("ReturnValue").asNumber(1) > 0) continue;
            }
            queue_interact(2, fnv64(path), count, ue::path_of(cls));
            g.items_sent++;
            g.last_interact = "sent take " + std::to_string(count) + "x " + ue::name_of(ue::class_of(item));
        }
    }
}

// Trading: what the containers near the player hold (every 0.25 s). A container that gained items of a class the
// player just lost got them from the player: a new stack, or added to one it had (the game fires no event for that,
// run 71). The other world's same container gets them too.
std::vector<std::pair<ue::UObject*, int32_t>> stacks_of(ue::UObject* actor) {  // its inventory's item stacks
    std::vector<std::pair<ue::UObject*, int32_t>> out;
    std::string err;
    ue::UObject* inv = ue::resolve(ue::addr(actor) + ".InventoryComponent", &err);
    static int items_off = -2;
    if (!inv) return out;
    if (items_off == -2) {
        ue::FProperty* p = ue::find_prop(ue::class_of(inv), "Items");  // InventoryComponent.Items: TArray<FItemStack>
        items_off = p ? ue::prop_offset(p) : -1;
    }
    if (items_off < 0) return out;
    struct Stack { ue::UObject* item; int32_t count; uint8_t rest[12]; };  // FItemStack: Item, Count, bLootable (0x18)
    static_assert(sizeof(Stack) == 0x18, "FItemStack size");
    const Stack* data;
    int32_t num;
    memcpy(&data, reinterpret_cast<const uint8_t*>(inv) + items_off, 8);
    memcpy(&num, reinterpret_cast<const uint8_t*>(inv) + items_off + 8, 4);
    if (!data || num <= 0 || num > 4096 || !ue::readable(data, static_cast<size_t>(num) * sizeof(Stack))) return out;
    for (int i = 0; i < num; i++)
        if (data[i].item && data[i].count > 0 && ue::alive(data[i].item)) out.push_back({data[i].item, data[i].count});
    return out;
}

std::unordered_map<ue::UClass*, int32_t> box_items(ue::UObject* box) {
    std::unordered_map<ue::UClass*, int32_t> out;
    for (const auto& s : stacks_of(box)) out[ue::class_of(s.first)] += s.second;
    return out;
}

void watch_boxes(ue::UObject* pawn) {
    if (g.now < g.next_box_check || !g.container_class) return;
    g.next_box_check = g.now + 0.25;
    double me[3], vel[3];
    float yaw, pitch;
    if (!read_transform(pawn, me, yaw, pitch, vel)) return;
    static Fn loc;
    int budget = 64;  // new containers located per pass
    std::unordered_map<uint64_t, std::unordered_map<ue::UClass*, int32_t>> close_by;  // ('near' is a Windows macro)
    for (auto& kv : g.interactables) {
        ue::UObject* box = kv.second;
        if (!ue::alive(box) || !ue::is_a(box, g.container_class) || box == g.give_box) continue;  // (the giving chest: gifts)
        auto at = g.box_pos.find(kv.first);
        if (at == g.box_pos.end()) {
            if (budget-- <= 0 || !base_fn(loc, "/Script/Engine.Actor", "K2_GetActorLocation")) continue;
            alignas(16) uint8_t b[64] = {};
            ue::process_event(box, loc.f, b);
            std::array<double, 3> p;
            memcpy(p.data(), b + loc.off("ReturnValue"), 24);
            at = g.box_pos.emplace(kv.first, p).first;
        }
        double dx = at->second[0] - me[0], dy = at->second[1] - me[1], dz = at->second[2] - me[2];
        if (dx * dx + dy * dy + dz * dz > 500.0 * 500.0) continue;
        auto items = box_items(box);
        auto was = g.box_counts.find(kv.first);
        if (was != g.box_counts.end()) {
            for (const auto& it : items) {
                auto w = was->second.find(it.first);
                int32_t gained = it.second - (w == was->second.end() ? 0 : w->second);
                if (gained <= 0) continue;
                auto mine = std::find_if(g.my_removals.begin(), g.my_removals.end(), [&](const std::pair<ue::UClass*, double>& r) { return r.first == it.first; });
                if (mine == g.my_removals.end()) continue;  // the game's own loot, not the player's
                g.my_removals.erase(mine);
                queue_interact(3, kv.first, gained, ue::path_of(it.first));
                g.items_sent++;
                g.last_interact = "sent store " + std::to_string(gained) + "x " + ue::name_of(it.first);
            }
            for (const auto& w : was->second) {  // taken out: a whole stack or part of one (the other player's takes
                auto now = items.find(w.first);   // applied here already lowered the snapshot: no echo)
                int32_t lost = w.second - (now == items.end() ? 0 : now->second);
                if (lost <= 0) continue;
                queue_interact(2, kv.first, lost, ue::path_of(w.first));
                g.items_sent++;
                g.last_interact = "sent take " + std::to_string(lost) + "x " + ue::name_of(w.first);
            }
        }
        close_by[kv.first] = std::move(items);
    }
    g.box_counts.swap(close_by);  // a container the player left is looked at afresh when they come back
}

void send_outgoing() {
    for (size_t i = 0; i < g.outgoing.size();) {
        Outgoing& o = g.outgoing[i];
        while (o.sent < 3 && g.now >= o.at[o.sent]) {
            send_typed(kInteract, &o.m, o.size);
            o.sent++;
        }
        if (o.sent >= 3) g.outgoing.erase(g.outgoing.begin() + i);
        else i++;
    }
}

ue::UObject* interactable(uint64_t id) {  // filled by the incremental scan (scan_step)
    auto it = g.interactables.find(id);
    return it != g.interactables.end() && ue::alive(it->second) ? it->second : nullptr;
}

struct DoorRec {
    ue::UObject *actor = nullptr, *ocl = nullptr;
    uint64_t id = 0;
    int last = -1;            // last polled settled state
    int remote = -1;          // state we just applied from the other world (its poll must not echo back)
};
std::vector<DoorRec> doors;
size_t door_cursor = 0;
double doors_built = -100;

int door_state(ue::UObject* ocl) {
    static ue::UFunction* f;
    static ue::UObject* cls;
    if (!f || cls != ue::class_of(ocl)) {
        cls = ue::class_of(ocl);
        f = ue::find_func(cls, "GetCurrentState");
        if (!f) return -1;
    }
    alignas(16) uint8_t b[16] = {};
    ue::process_event(ocl, f, b);
    return b[0];  // EOCLState is the only (return) parameter
}

void poll_doors() {
    if (g.interactables_built > doors_built) {  // a scan pass finished: World Partition streams doors in and out
        std::vector<DoorRec> fresh;
        if (g.door_class) {
            for (auto& kv : g.interactables) {
                ue::UObject* o = kv.second;
                if (!ue::alive(o) || !ue::is_a(o, g.door_class)) continue;
                std::string err;
                ue::UObject* ocl = ue::resolve(ue::addr(o) + ".OCLComponent", &err);
                if (!ocl) continue;
                DoorRec r;
                r.actor = o, r.ocl = ocl, r.id = fnv64(ue::path_of(o));
                for (auto& old : doors)
                    if (old.actor == o) { r.last = old.last, r.remote = old.remote; break; }
                fresh.push_back(r);
            }
        }
        doors.swap(fresh);
        door_cursor = 0;
        doors_built = g.interactables_built;
    }
    for (int k = 0; k < 16 && !doors.empty(); k++) {
        DoorRec& d = doors[door_cursor];
        door_cursor = (door_cursor + 1) % doors.size();
        if (!ue::alive(d.actor) || !ue::alive(d.ocl)) continue;
        int st = door_state(d.ocl);
        if (st < 0 || st > 2) continue;  // only settled Closed/Open/Locked
        if (d.last >= 0 && st != d.last) {
            if (st == d.remote) d.remote = -1;  // our own apply arriving: no echo
            else {
                queue_interact(1, d.id, st, "");
                g.doors_sent++;
                g.last_interact = "sent door " + ue::name_of(d.actor) + " -> " + std::to_string(st);
            }
        }
        d.last = st;
    }
}

void note_remote_door(ue::UObject* actor, int state) {
    for (auto& d : doors)
        if (d.actor == actor) d.remote = state;
}

void call_on(ue::UObject* o, const char* fn, const json& args) {
    ue::UFunction* f = ue::find_func(ue::class_of(o), fn);
    if (f) ue::call(o, f, args);
}

ue::UClass* load_class(const std::string& path);  // below: a Blueprint class by path, loaded if needed

bool first_time(uint32_t eid) {  // each event goes out three times
    if (std::find(g.seen_eids.begin(), g.seen_eids.end(), eid) != g.seen_eids.end()) return false;
    g.seen_eids.push_back(eid);
    if (g.seen_eids.size() > 512) g.seen_eids.erase(g.seen_eids.begin(), g.seen_eids.begin() + 256);
    return true;
}

// XP the other player earned (kind 4). Taken in any world state: it waits in the inbox for a player (it often comes
// with a map change, while this world loads).
bool take_xp(const char* body, int len) {
    InteractMsg m{};
    if (len != static_cast<int>(offsetof(InteractMsg, path))) return false;
    memcpy(&m, body, len);
    if (m.kind != 4) return false;
    g.recv++;
    if (first_time(m.eid) && m.a > 0 && m.a <= 1000000 && m.id <= 13) g.xp_inbox.push_back({m.a, static_cast<uint8_t>(m.id)});  // trust boundary
    return true;
}

void apply_interact(const InteractMsg& m) {
    if (!first_time(m.eid)) return;
    ue::UObject* actor = interactable(m.id);
    if (!actor && m.kind == 2) {  // a looted dead NPC: the NPC registry knows it by the same path id
        auto it = g.npcs.find(m.id);
        if (it != g.npcs.end() && npc_live(it->second)) actor = it->second.actor;
    }
    if (!actor) return;
    std::string err;
    applying_remote = true;
    try {
        if (m.kind == 1) {
            // Doors keep their state machine in OCLComponent (AttemptOpen/AttemptClose); other interactables in the
            // interaction component (OpenOCL/CloseOCL).
            ue::UObject* ocl = ue::resolve(ue::addr(actor) + ".OCLComponent", &err);
            ue::UObject* ic = ocl ? ocl : ue::resolve(ue::addr(actor) + ".Interactable", &err);
            if (ic) {
                json cur = ue::call(ic, ue::find_func(ue::class_of(ic), ocl ? "GetCurrentState" : "GetCurrentStateOCL"), json::object()).get("ReturnValue");
                std::string want = m.a == 0 ? "Closed" : m.a == 1 ? "Open" : "Locked";
                if (cur.asString() != want) {
                    note_remote_door(actor, m.a);
                    json a = json::object();
                    a.set("bIgnoreLockStatus", true).set("bForce", true);
                    if (cur.asString() == "Locked" && m.a != 2) {
                        json u = json::object();
                        u.set("bFireOCLUnlockedEvent", true);
                        call_on(ic, "UnlockOCL", u);
                    }
                    const char* open_fn = ocl ? "AttemptOpen" : "OpenOCL";
                    const char* close_fn = ocl ? "AttemptClose" : "CloseOCL";
                    if (m.a == 1) call_on(ic, open_fn, a);
                    else if (m.a == 0) call_on(ic, close_fn, a);
                    else {
                        if (cur.asString() == "Open") call_on(ic, close_fn, a);
                        json l = json::object();
                        l.set("bFireOCLLockedEvent", true);
                        call_on(ic, "LockOCL", l);
                    }
                    g.doors_applied++;
                    g.last_interact = "applied door " + ue::name_of(actor) + " -> " + want;
                }
            }
        } else if (m.kind == 3 && m.len > 0) {
            // The other player stored an item in this container (trading). A class from the other player (trust
            // boundary): a /Game/ item class only, a sane count.
            std::string cls(m.path, m.len);
            ue::UClass* item = cls.rfind("/Game/", 0) == 0 && m.a > 0 && m.a <= 1000000 ? load_class(cls) : nullptr;
            ue::UClass* base = ue::find_class("/Script/Arkansas.Item");
            ue::UObject* inv = ue::resolve(ue::addr(actor) + ".InventoryComponent", &err);
            if (item && base && ue::cdo(item) && ue::is_a(ue::cdo(item), base) && inv) {
                json q = json::object();
                q.set("Actor", ue::addr(actor)).set("ItemType", ue::addr(item)).set("Count", static_cast<int>(m.a)).set("bLootable", true);
                ue::call(inv, ue::find_func(ue::class_of(inv), "GiveItemToActor"), q);
                auto snap = g.box_counts.find(m.id);
                if (snap != g.box_counts.end()) snap->second[item] += m.a;
                g.items_applied++;
                g.last_interact = "applied store " + std::to_string(m.a) + "x " + ue::name_of(item) + " in " + ue::name_of(actor);
            }
        } else if (m.kind == 2 && m.len > 0) {
            std::string cls(m.path, m.len);
            ue::UClass* item = ue::find_class(cls);
            ue::UObject* inv = ue::resolve(ue::addr(actor) + ".InventoryComponent", &err);
            if (item && inv) {
                json q = json::object();
                q.set("Actor", ue::addr(actor)).set("ItemType", ue::addr(item));
                int have = static_cast<int>(ue::call(inv, ue::find_func(ue::class_of(inv), "GetItemCountOnActor"), q).get("ReturnValue").asInt(0));
                if (have > 0) {
                    q.set("Count", std::min(have, static_cast<int>(m.a)));
                    ue::call(inv, ue::find_func(ue::class_of(inv), "RemoveItemFromActor"), q);
                    auto snap = g.box_counts.find(m.id);  // the container watch must not send this take back
                    if (snap != g.box_counts.end()) snap->second[item] = std::max(0, snap->second[item] - std::min(have, static_cast<int>(m.a)));
                    g.items_applied++;
                    g.last_interact = "applied take " + std::to_string(m.a) + "x " + ue::name_of(item) + " from " + ue::name_of(actor);
                }
            }
        }
    } catch (const std::exception& e) {
        g.last_error = std::string("interact: ") + e.what();
    }
    applying_remote = false;
}

void on_face(const char* body, int len);  // below: the other player's look, in pieces

void close_transfer();  // below (giving items): the transfer screen we opened, closed as the player would
void call_quiet(const std::string& ref, const char* fn, const json& args);  // below

// ---------------------------------------------------------------- following a travel
// A travel (a door into an interior, the ship, a fast travel: the TravelManager's ActiveTravelDestination gets set)
// starts the same travel in the other world at once, so both players load the new map together; before, the client
// waited for the host's save of the new map (run 81: 22 s behind the host). The client follows the host's travels and
// the host the client's. The save hand-over stays the fallback: a travel refused or not answered, and loads that are no
// travel (a save loaded, a death).
ue::UObject* travel_manager() {
    static ue::UObject* tm = nullptr;
    static double next_look = 0;
    if (tm && ue::alive(tm)) return tm;
    if (g.now < next_look) return nullptr;  // none (the main menu): a search walks every object, not each frame
    next_look = g.now + 2.0;
    std::vector<ue::UObject*> v = ue::instances(ue::find_class("/Script/Arkansas.TravelManager"), false, 1);
    return tm = v.empty() ? nullptr : v[0];
}

ue::UObject* travel_destination(ue::UObject* tm) {  // the travel under way, nullptr if none
    static int off = -1;
    if (off < 0) {
        ue::FProperty* p = ue::find_prop(ue::class_of(tm), "ActiveTravelDestination");
        off = p ? ue::prop_offset(p) : 0;
    }
    ue::UObject* d = off ? *reinterpret_cast<ue::UObject**>(reinterpret_cast<uint8_t*>(tm) + off) : nullptr;
    return d && ue::alive(d) ? d : nullptr;
}

void queue_travel(uint32_t id, uint8_t ack, const std::string& text) {  // 3 copies, 0.15 s apart (UDP may drop one)
    TravelMsg m{};
    m.id = id, m.ack = ack;
    m.len = static_cast<uint16_t>(std::min(text.size(), sizeof m.path));
    memcpy(m.path, text.data(), m.len);
    for (int i = 0; i < 3; i++) g.travel_out.push_back({m, g.now + 0.15 * i});
}

void send_travels() {
    for (size_t i = 0; i < g.travel_out.size();) {
        if (g.now < g.travel_out[i].second) { i++; continue; }
        const TravelMsg& m = g.travel_out[i].first;
        send_typed(kTravel, &m, static_cast<int>(offsetof(TravelMsg, path)) + m.len);
        g.travel_out.erase(g.travel_out.begin() + i);
    }
}

void on_travel(const char* body, int len) {
    TravelMsg m{};
    if (len < static_cast<int>(offsetof(TravelMsg, path)) || len > static_cast<int>(sizeof m)) { g.bad++; return; }
    memcpy(&m, body, len);
    if (m.len > sizeof m.path || len != static_cast<int>(offsetof(TravelMsg, path)) + m.len) { g.bad++; return; }
    std::string text(m.path, m.len);
    if (m.ack) {  // the answer to our travel
        if (m.id != g.travel_id || g.travel_acked == m.id) return;
        g.travel_acked = m.id;
        if (g.host) g.follow_until = m.ack == 1 ? g.now + 90.0 : 0;  // the client comes by itself: no hand-over meanwhile
        g.last_travel = m.ack == 1 ? "the other player follows" : "the other player stays (" + text + "): the save hand-over brings them";
        core_log("travel: " + g.last_travel);
        return;
    }
    uint64_t key = static_cast<uint64_t>(g.remote_id) << 32 | m.id;  // a restarted game counts its travels from 1 again
    if (key == g.travel_in_key) return;                                  // a copy
    g.travel_in_key = key, g.travel_in_id = m.id;
    g.travel_in = text, g.travel_in_at = g.now;
}

// Our own travel starting: ask the other player to follow.
void watch_travel() {
    ue::UObject* tm = travel_manager();
    ue::UObject* d = tm ? travel_destination(tm) : nullptr;
    if (d == g.travel_seen) return;
    g.travel_seen = d;
    if (!d) { g.travel_following = nullptr; return; }  // a travel ended
    if (d == g.travel_following) return;               // the one we started to follow the other player's
    if (!g.have_peer || !g.remote_id || (!g.host && !join::joined())) return;  // nobody to bring, or the friend's own campaign
    queue_travel(++g.travel_id, 0, ue::path_of(d));
    g.travels_sent++;
    g.last_travel = "to " + ue::name_of(d) + ": the other player is asked to follow";
    core_log("travel: " + g.last_travel);
}

bool in_combat(ue::UObject* pawn) {
    ue::UFunction* f = ue::find_func(ue::class_of(pawn), "IsInCombat");
    try { return f && ue::call(pawn, f, json::object()).get("ReturnValue").asBool(false); } catch (const std::exception&) { return false; }
}

// The other player's travel: start the same one here (game thread, in gameplay).
void follow_travel(ue::UObject* pawn) {
    if (g.travel_in.empty() || join::loading()) return;
    if (g.transfer_widget) { close_transfer(); return; }  // its chest goes away with this world
    std::string path = g.travel_in;
    uint32_t id = g.travel_in_id;
    g.travel_in.clear();
    auto answer = [&](bool ok, const std::string& why) {
        queue_travel(id, ok ? 1 : 2, why);
        if (ok) g.travels_followed++;
        else g.travels_refused++;
        g.last_travel = (ok ? "following the other player to " : "not following the other player to ") + path.substr(path.rfind('.') + 1) + (ok ? "" : " (" + why + ")");
        core_log("travel: " + g.last_travel);
    };
    if (!g.opt_follow) return answer(false, "following is off (test)");
    if (g.died) return answer(false, "dead: the fetched game brings you there");
    if (g.now - g.travel_in_at > 20.0) return answer(false, "asked too long ago");
    if (!g.host && !join::joined()) return answer(false, "playing your own campaign");
    if (g.host && in_combat(pawn)) return answer(false, "the host is in combat");  // the host's campaign: never cut a fight short
    ue::UObject* tm = travel_manager();
    if (!tm) return answer(false, "no TravelManager");
    ue::UObject* d = ue::find_path(path);
    if (!d && path.rfind("/Game/", 0) == 0) {  // not loaded yet in this world
        ue::UClass* k = ue::find_class("/Script/Engine.KismetSystemLibrary");
        json a = json::object();
        a.set("Asset", path);
        try { ue::call(ue::cdo(k), ue::find_func(k, "LoadAsset_Blocking"), a); } catch (const std::exception&) {}
        d = ue::find_path(path);
    }
    if (!d) return answer(false, "destination not found");
    if (!ue::is_a(d, ue::find_class("/Script/Arkansas.PointOfInterestData"))) return answer(false, "not a travel destination");  // from the other player
    if (travel_destination(tm) != d) {
        ue::UFunction* f = ue::find_func(ue::class_of(tm), "BeginTravelToBP");
        if (!f) return answer(false, "no BeginTravelToBP");
        json a = json::object();
        a.set("Destination", ue::addr(d)).set("bPassGametime", false).set("bTriggerAutosave", false).set("bForceBlocking", false);
        try { ue::call(tm, f, a); } catch (const std::exception& e) { return answer(false, e.what()); }
        if (travel_destination(tm) != d) return answer(false, "the game did not start the travel");
    }
    g.travel_following = g.travel_seen = d;  // not announced back
    if (g.host) g.follow_until = g.now + 90.0;  // the client leads: it will be in the new map, no hand-over meanwhile
    answer(true, "");
}

// ---------------------------------------------------------------- giving items (the other player's stand-in)
// Look at the other player's stand-in (within 3.5 m) and press E: the game's own transfer screen opens between your
// inventory and a hidden, empty chest at the stand-in (on the stand-in itself, the screen crashed reading its NPC
// clothes, probe 83). What you move into it leaves at once and goes into the other player's inventory (resent until
// they confirm it; back to you after 20 s without an answer). A given weapon or armor arrives as a new copy with the
// same level and rarity, without its mods.
std::atomic<bool> give_key{false};
WNDPROC game_proc = nullptr;
HWND game_hwnd = nullptr;

LRESULT CALLBACK key_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_KEYDOWN && w == 'E' && !(l & (1 << 30))) give_key = true;  // a new press (bit 30: a repeat)
    return game_proc ? CallWindowProcW(game_proc, h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

BOOL CALLBACK find_game_window(HWND h, LPARAM out) {
    DWORD pid = 0;
    wchar_t cls[32] = L"";
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || !GetClassNameW(h, cls, 32) || wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    *reinterpret_cast<HWND*>(out) = h;
    return FALSE;
}

void watch_keys() {  // E presses reach the game's window procedure; the window may be made again (display mode change)
    static double next = 0;
    if (g.now < next) return;
    next = g.now + 5.0;
    if (game_hwnd && IsWindow(game_hwnd)) return;  // ours already (never twice: an overlay chained after us would loop)
    HWND h = nullptr;
    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&h));
    if (!h) return;
    LONG_PTR old = SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(key_proc));
    if (old && old != reinterpret_cast<LONG_PTR>(key_proc)) game_proc = reinterpret_cast<WNDPROC>(old), game_hwnd = h;
}

void close_transfer() {  // as the player would: Escape (its chest must not go away with a world under an open screen)
    if (!g.transfer_widget || !game_hwnd || g.now < g.next_close) return;
    g.next_close = g.now + 0.5;
    PostMessageW(game_hwnd, WM_KEYDOWN, VK_ESCAPE, 1 | (MapVirtualKeyW(VK_ESCAPE, 0) << 16));
    PostMessageW(game_hwnd, WM_KEYUP, VK_ESCAPE, 1 | (MapVirtualKeyW(VK_ESCAPE, 0) << 16) | (1u << 30) | (1u << 31));
}

int item_prop(const char* name) {  // offset of an Item property (ItemLevel, ItemRarity), -1 if missing
    ue::UClass* k = ue::find_class("/Script/Arkansas.Item");
    ue::FProperty* p = k ? ue::find_prop(k, name) : nullptr;
    return p && ue::prop_size(p) == 4 ? ue::prop_offset(p) : -1;
}

int32_t read_i32(ue::UObject* o, int off) {
    int32_t v = 0;
    if (o && off >= 0) memcpy(&v, reinterpret_cast<const uint8_t*>(o) + off, 4);
    return v;
}

// The mods on a weapon or armor item (probe 85). A weapon keeps them in WeaponModContainer.ModSlots, a map of slot ->
// FWeaponModSlot (CurrentMod: the mod installed, DefaultMod: the slot's stock one); an armor in ArmorModSlots (a fixed
// array) and BonusArmoringSlot (CurrentModClass).
std::vector<ue::UClass*> mods_of(ue::UObject* item) {
    std::vector<ue::UClass*> out;
    static ue::UClass* const weapon_k = ue::find_class("/Script/Arkansas.Weapon");
    static ue::UClass* const armor_k = ue::find_class("/Script/Arkansas.ArmorItem");
    if (!item || !ue::alive(item)) return out;
    if (weapon_k && ue::is_a(item, weapon_k)) {
        static int cont_off = -2, slots_off = -1, cur_off = -1, def_off = -1, slot_size = 0;
        if (cont_off == -2) {
            ue::FProperty* c = ue::find_prop(weapon_k, "WeaponModContainer");
            ue::UClass* ck = ue::find_class("/Script/Arkansas.WeaponModContainer");
            ue::FProperty* s = ck ? ue::find_prop(ck, "ModSlots") : nullptr;
            ue::UStruct* st = ue::find_path("/Script/Arkansas.WeaponModSlot");
            ue::FProperty* cm = st ? ue::find_prop(st, "CurrentMod") : nullptr;
            ue::FProperty* dm = st ? ue::find_prop(st, "DefaultMod") : nullptr;
            cont_off = c ? ue::prop_offset(c) : -1, slots_off = s ? ue::prop_offset(s) : -1;
            cur_off = cm ? ue::prop_offset(cm) : -1, def_off = dm ? ue::prop_offset(dm) : -1, slot_size = st ? ue::struct_size(st) : 0;
        }
        if (cont_off < 0 || slots_off < 0 || cur_off < 0 || def_off < 0 || slot_size <= 0) return out;
        ue::UObject* cont = nullptr;
        memcpy(&cont, reinterpret_cast<const uint8_t*>(item) + cont_off, 8);
        if (!cont || !ue::alive(cont)) return out;
        // TMap<EModSlot, FWeaponModSlot>: a sparse array of {key byte, padding to 8, value, two hash ints}, its
        // allocation bits inline at +0x10 (or behind the pointer at +0x20 when they don't fit)
        const uint8_t* map = reinterpret_cast<const uint8_t*>(cont) + slots_off;
        const uint8_t* data = nullptr;
        const uint32_t* heap_bits = nullptr;
        int32_t num = 0;
        memcpy(&data, map, 8), memcpy(&num, map + 8, 4), memcpy(&heap_bits, map + 0x20, 8);
        const uint32_t* bits = heap_bits ? heap_bits : reinterpret_cast<const uint32_t*>(map + 0x10);
        size_t elem = (8 + static_cast<size_t>(slot_size) + 8 + 7) & ~static_cast<size_t>(7);
        if (!data || num <= 0 || num > 64 || !ue::readable(data, elem * num) || !ue::readable(bits, 8)) return out;
        for (int i = 0; i < num; i++) {
            if (!((bits[i / 32] >> (i % 32)) & 1)) continue;  // a free entry
            const uint8_t* v = data + elem * i + 8;
            ue::UObject* cur = nullptr;
            ue::UClass* def = nullptr;
            memcpy(&cur, v + cur_off, 8), memcpy(&def, v + def_off, 8);
            if (cur && ue::alive(cur) && ue::class_of(cur) != def) out.push_back(ue::class_of(cur));
        }
    } else if (armor_k && ue::is_a(item, armor_k)) {
        static int slots_off = -2, bonus_off = -1, dim = 0, cls_off = -1, slot_size = 0;
        if (slots_off == -2) {
            ue::FProperty* s = ue::find_prop(armor_k, "ArmorModSlots");
            ue::FProperty* b = ue::find_prop(armor_k, "BonusArmoringSlot");
            ue::UStruct* st = ue::find_path("/Script/Arkansas.ArmorSetModSlot");
            ue::FProperty* c = st ? ue::find_prop(st, "CurrentModClass") : nullptr;
            slots_off = s ? ue::prop_offset(s) : -1, bonus_off = b ? ue::prop_offset(b) : -1;
            dim = s ? ue::prop_dim(s) : 0;
            cls_off = c ? ue::prop_offset(c) : -1, slot_size = st ? ue::struct_size(st) : 0;
        }
        if (slots_off < 0 || cls_off < 0 || slot_size <= 0 || dim < 1 || dim > 16) return out;
        std::vector<int> at;
        for (int i = 0; i < dim; i++) at.push_back(slots_off + i * slot_size);
        if (bonus_off >= 0) at.push_back(bonus_off);
        const uint8_t* stock = reinterpret_cast<const uint8_t*>(ue::cdo(ue::class_of(item)));  // a unique's built-in mods
        for (int o : at) {
            ue::UClass *k = nullptr, *def = nullptr;
            memcpy(&k, reinterpret_cast<const uint8_t*>(item) + o + cls_off, 8);
            if (stock) memcpy(&def, stock + o + cls_off, 8);
            if (k && k != def && ue::alive(k)) out.push_back(k);
        }
    }
    return out;
}

bool install_mod(ue::UObject* item, ue::UClass* mod) {  // the game's own install, as its debug commands do it
    static ue::UClass* const weapon_k = ue::find_class("/Script/Arkansas.Weapon");
    static ue::UClass* const armor_k = ue::find_class("/Script/Arkansas.ArmorItem");
    static ue::UClass* const wmod_k = ue::find_class("/Script/Arkansas.WeaponMod");
    static ue::UClass* const amod_k = ue::find_class("/Script/Arkansas.ArmorModItem");
    ue::UObject* cdo = mod ? ue::cdo(mod) : nullptr;
    void* ref = mod;  // TSubclassOf<>: the class pointer, passed by reference
    if (!cdo || !item || !ue::alive(item)) return false;
    if (weapon_equip_mod && weapon_k && wmod_k && ue::is_a(item, weapon_k) && ue::is_a(cdo, wmod_k)) return weapon_equip_mod(item, &ref, true);
    if (armor_equip_mod && armor_k && amod_k && ue::is_a(item, armor_k) && ue::is_a(cdo, amod_k)) {
        armor_equip_mod(item, &ref, false, true, true, true);
        auto now = mods_of(item);
        return std::find(now.begin(), now.end(), mod) != now.end();
    }
    return false;
}

// Into the local player's inventory, with the game's own notification; a new single item gets the level, rarity and
// mods of the given one.
void give_to_player(ue::UClass* k, int32_t n, int32_t level, int32_t rarity, const std::vector<ue::UClass*>& mods = {}) {
    ue::UObject* pawn = local_pawn();
    std::string err;
    ue::UObject* inv = pawn ? ue::resolve(ue::addr(pawn) + ".InventoryComponent", &err) : nullptr;
    ue::UFunction* f = inv ? ue::find_func(ue::class_of(inv), "GiveItemToPlayer") : nullptr;
    if (!f) throw std::runtime_error("no player inventory");
    std::vector<ue::UObject*> before;
    for (const auto& s : stacks_of(pawn)) if (ue::class_of(s.first) == k) before.push_back(s.first);
    json a = json::object();
    a.set("ItemType", ue::addr(k)).set("Count", n).set("bShowNotification", true).set("bPlayItemPickupAudio", true);
    ue::call(inv, f, a);
    static const int lv = item_prop("ItemLevel"), ra = item_prop("ItemRarity");
    for (const auto& s : stacks_of(pawn)) {
        if (ue::class_of(s.first) != k || std::find(before.begin(), before.end(), s.first) != before.end()) continue;
        if (lv >= 0 && level > 0 && level < 10000) memcpy(reinterpret_cast<uint8_t*>(s.first) + lv, &level, 4);
        if (ra >= 0 && rarity >= 0 && rarity < 16) memcpy(reinterpret_cast<uint8_t*>(s.first) + ra, &rarity, 4);
        for (ue::UClass* m : mods)
            if (!install_mod(s.first, m)) core_log("give: could not put the mod " + ue::name_of(m) + " on " + ue::name_of(s.first));
    }
}

constexpr const char* kGiveBoxClass = "/Game/Blueprints/World/Interactable/Container/BP_Container_Chest_Small.BP_Container_Chest_Small_C";

ue::UObject* spawn_give_box(ue::UObject* standin) {  // an empty, hidden chest 2 m under the stand-in, never saved
    ue::UClass* cls = load_class(kGiveBoxClass);
    double at[3], v[3];
    float yaw, pitch;
    if (!cls || !read_transform(standin, at, yaw, pitch, v)) return nullptr;
    std::string err;
    ue::UObject* gs = ue::cdo(ue::find_class("/Script/Engine.GameplayStatics"));
    json xf = json::object(), tr = json::array(), rot = json::object(), sc = json::array();
    tr.push(at[0]).push(at[1]).push(at[2] - 200.0);
    rot.set("X", 0.0).set("Y", 0.0).set("Z", 0.0).set("W", 1.0);
    sc.push(1.0).push(1.0).push(1.0);
    xf.set("Rotation", rot).set("Translation", tr).set("Scale3D", sc);
    json a = json::object();
    a.set("WorldContextObject", ue::addr(ue::resolve("pc", &err))).set("ActorClass", ue::addr(cls)).set("SpawnTransform", xf);
    a.set("CollisionHandlingOverride", "AlwaysSpawn");
    std::string ref = ue::call(gs, ue::find_func(ue::class_of(gs), "BeginDeferredActorSpawnFromClass"), a).get("ReturnValue").asString();
    ue::UObject* box = ref.rfind("0x", 0) == 0 ? reinterpret_cast<ue::UObject*>(static_cast<uintptr_t>(strtoull(ref.c_str(), nullptr, 16))) : nullptr;
    if (!box || !ue::alive(box)) return nullptr;
    ue::set_path(ue::addr(box) + ".bShouldSaveGameState", json(false));    // as the stand-in: out of the host's saves
    ue::set_path(ue::addr(box) + ".bLootManagerLootApplied", json(true));  // no loot of its own
    json f = json::object();
    f.set("Actor", ue::addr(box)).set("SpawnTransform", xf);
    ue::call(gs, ue::find_func(ue::class_of(gs), "FinishSpawningActor"), f);
    json hide = json::object(), nocol = json::object(), noint = json::object();
    hide.set("bNewHidden", true), nocol.set("bNewActorEnableCollision", false), noint.set("bNewInteractionsEnabled", false);
    call_on(box, "SetActorHiddenInGame", hide);
    call_on(box, "SetActorEnableCollision", nocol);
    call_quiet(ue::addr(box) + ".InteractableComponent", "SetInteractionsEnabled", noint);
    std::string ierr;
    ue::UObject* inv = ue::resolve(ue::addr(box) + ".InventoryComponent", &ierr);
    for (const auto& st : stacks_of(box)) {  // in case the chest came with something anyway
        json q = json::object();
        q.set("Actor", ue::addr(box)).set("ItemType", ue::addr(ue::class_of(st.first))).set("Count", st.second);
        if (inv) call_on(inv, "RemoveItemFromActor", q);
    }
    return box;
}

void drop_give_box() {
    if (g.give_box && ue::alive(g.give_box)) call_on(g.give_box, "K2_DestroyActor", json::object());
    g.give_box = nullptr;
}

bool open_give(std::string& why) {
    if (!open_transfer_fn) return why = "the game's transfer screen was not found in this build", false;
    if (!g.puppet || !standin_ok()) return why = "no stand-in of the other player here", false;
    if (!g.host && !join::joined()) return why = "playing your own campaign", false;
    std::string err;
    ue::UObject* hud = ue::resolve("pc.MyHUD", &err);
    if (!hud || !ue::is_a(hud, ue::find_class("/Script/Arkansas.IndianaUI"))) return why = "no game HUD", false;
    drop_give_box();
    g.give_box = spawn_give_box(g.puppet);
    if (!g.give_box) return why = "could not place the hidden chest", false;
    void* w = open_transfer_fn(hud, g.give_box);
    if (!w) { drop_give_box(); return why = "the game did not open its transfer screen", false; }
    g.transfer_widget = static_cast<ue::UObject*>(w), g.next_give_check = 0;
    return true;
}

bool looking_at(ue::UObject* pawn, ue::UObject* target) {  // within 3.5 m, the view crossing its body
    double me[3], at[3], v[3];
    float yaw, pitch;
    std::string err;
    ue::UObject* pc = ue::resolve("pc", &err);
    ue::UFunction* rot = pc ? ue::find_func(ue::class_of(pc), "GetControlRotation") : nullptr;
    if (!rot || !read_transform(pawn, me, yaw, pitch, v) || !read_transform(target, at, yaw, pitch, v)) return false;
    json r = ue::call(pc, rot, json::object()).get("ReturnValue");
    double cy = r.get("Yaw").asNumber(0) * 3.14159265 / 180, cp = r.get("Pitch").asNumber(0) * 3.14159265 / 180;
    double dx = at[0] - me[0], dy = at[1] - me[1], h = std::hypot(dx, dy);
    if (h > 350 || h < 1) return false;
    double off = std::fabs(std::remainder(std::atan2(dy, dx) - cy, 2 * 3.14159265));  // sideways angle
    double z = me[2] + 60 + h * std::tan(cp);                                                // view height at the stand-in
    return off < std::atan2(45.0, h) + 0.1 && z > at[2] - 100 && z < at[2] + 100;
}

void try_give(ue::UObject* pawn) {  // E was pressed
    if (g.transfer_widget || !g.puppet || !standin_ok() || g.puppet_map != g.my_map) return;
    json cursor;
    try { cursor = ue::eval_path("pc.bShowMouseCursor"); } catch (const std::exception&) {}
    if (cursor.asBool(false) || !looking_at(pawn, g.puppet)) return;  // a menu is open, or E was for something else
    std::string why;
    bool ok = false;
    try { ok = open_give(why); } catch (const std::exception& e) { why = e.what(), drop_give_box(); }
    g.last_gift = ok ? "transfer screen open for the other player" : "could not open the transfer screen: " + why;
    core_log("give: " + g.last_gift);
}

// The open transfer screen: what landed in the hidden chest goes to the other player, stack by stack (an item leaves
// by class, which takes the first stack of that class: the one read; shortcut: two copies of one weapon with different
// mods moved together rely on that order).
void watch_gifts() {
    if (!g.transfer_widget || g.now < g.next_give_check || g.opt_give_hold) return;
    g.next_give_check = g.now + 0.2;
    ue::UObject* s = g.give_box;
    bool shown = false;
    if (ue::alive(g.transfer_widget)) {
        ue::UFunction* f = ue::find_func(ue::class_of(g.transfer_widget), "IsInViewport");
        try { shown = f && ue::call(g.transfer_widget, f, json::object()).get("ReturnValue").asBool(false); } catch (const std::exception&) {}
    }
    if (s && ue::alive(s)) {
        static const int lv = item_prop("ItemLevel"), ra = item_prop("ItemRarity");
        std::string err;
        ue::UObject* inv = ue::resolve(ue::addr(s) + ".InventoryComponent", &err);
        ue::UFunction* rm = inv ? ue::find_func(ue::class_of(inv), "RemoveItemFromActor") : nullptr;
        for (int guard = 0; rm && guard < 64; guard++) {
            auto stacks = stacks_of(s);
            if (stacks.empty()) break;
            ue::UObject* one = stacks.front().first;
            int32_t n = stacks.front().second;
            ue::UClass* k = ue::class_of(one);
            Ctx::Gift x{};
            x.item = k, x.count = n, x.level = read_i32(one, lv), x.rarity = read_i32(one, ra), x.mods = mods_of(one);
            json q = json::object();
            q.set("Actor", ue::addr(s)).set("ItemType", ue::addr(k)).set("Count", n);
            bool removed = false;
            try { removed = ue::call(inv, rm, q).get("ReturnValue").asBool(false); } catch (const std::exception&) {}
            if (!removed) { g.last_gift = "could not take " + ue::name_of(k) + " out of the chest"; break; }
            x.m.eid = (g.me & 0xFFFF0000u) | (g.next_eid++ & 0xFFFFu);
            x.m.kind = 5, x.m.a = n, x.m.id = static_cast<uint32_t>(x.level) | static_cast<uint64_t>(static_cast<uint32_t>(x.rarity)) << 32;
            std::string path = ue::path_of(k);
            for (ue::UClass* m : x.mods) {
                std::string mp = ue::path_of(m);
                if (path.size() + 1 + mp.size() <= sizeof x.m.path) path += "|" + mp;
            }
            x.m.len = static_cast<uint16_t>(std::min(path.size(), sizeof x.m.path));
            memcpy(x.m.path, path.data(), x.m.len);
            x.size = static_cast<int>(offsetof(InteractMsg, path)) + x.m.len;
            x.next = g.now, x.until = g.now + 20.0;
            g.gifts_out.push_back(x);
            g.gifts_sent++;
            g.last_gift = "giving " + std::to_string(n) + "x " + ue::name_of(k) + (x.mods.empty() ? "" : " with " + std::to_string(x.mods.size()) + " mod(s)") + " to the other player";
            core_log("give: " + g.last_gift);
        }
    }
    if (!shown || !s || !ue::alive(s)) g.transfer_widget = nullptr, drop_give_box();  // closed (a last pass ran)
}

void send_gifts() {  // resent every 0.5 s until confirmed; back to the giver after 20 s
    for (size_t i = 0; i < g.gifts_out.size();) {
        Ctx::Gift& x = g.gifts_out[i];
        if (g.now >= x.until) {
            std::string what = std::to_string(x.count) + "x " + ue::name_of(x.item);
            try {
                give_to_player(x.item, x.count, x.level, x.rarity, x.mods);
                g.last_gift = "the other player did not answer: " + what + " given back to you";
            } catch (const std::exception& e) {
                if (g.now < x.until + 60.0) { i++; continue; }  // no player now (a load): try again for a minute
                g.last_gift = "the other player did not answer and " + what + " could not be given back: " + e.what();
            }
            g.gifts_returned++;
            core_log("give: " + g.last_gift);
            g.gifts_out.erase(g.gifts_out.begin() + i);
            continue;
        }
        if (g.now >= x.next) send_typed(kInteract, &x.m, x.size), x.next = g.now + 0.5;
        i++;
    }
}

// A gift (kind 5) or the answer to ours (kind 6), in any world state: a gift waits in the inbox for a player.
bool take_gift(const char* body, int len) {
    InteractMsg m{};
    if (len < static_cast<int>(offsetof(InteractMsg, path)) || len > static_cast<int>(sizeof m)) return false;
    memcpy(&m, body, len);
    if (m.kind == 6) {
        for (size_t i = 0; i < g.gifts_out.size(); i++) {
            if (g.gifts_out[i].m.eid != static_cast<uint32_t>(m.id)) continue;
            g.last_gift = "gave " + std::to_string(g.gifts_out[i].count) + "x " + ue::name_of(g.gifts_out[i].item) + " to the other player";
            core_log("give: " + g.last_gift);
            g.gifts_out.erase(g.gifts_out.begin() + i);
            break;
        }
        return true;
    }
    if (m.kind != 5) return false;
    g.recv++;
    if (m.len > sizeof m.path || len != static_cast<int>(offsetof(InteractMsg, path)) + m.len) { g.bad++; return true; }
    if (!g.opt_gifts || (!g.host && !join::joined())) return true;  // no answer: the giver gets it back
    if (first_time(m.eid)) g.gift_inbox.push_back(m);
    InteractMsg ack{};
    ack.eid = (g.me & 0xFFFF0000u) | (g.next_eid++ & 0xFFFFu), ack.kind = 6, ack.id = m.eid;
    send_typed(kInteract, &ack, static_cast<int>(offsetof(InteractMsg, path)));
    return true;
}

void apply_gifts() {
    if (g.gift_inbox.empty()) return;
    std::vector<InteractMsg> todo;
    todo.swap(g.gift_inbox);
    ue::UClass* base = ue::find_class("/Script/Arkansas.Item");
    static ue::UClass* const mod_base = ue::find_class("/Script/Arkansas.ItemMod");
    for (const InteractMsg& m : todo) {
        std::string all(m.path, m.len), cls = all.substr(0, all.find('|'));  // "<item class>|<mod class>|..."
        // From the other player (trust boundary): /Game/ classes of items and of item mods, a sane count.
        ue::UClass* item = cls.rfind("/Game/", 0) == 0 && m.a > 0 && m.a <= 1000000 ? load_class(cls) : nullptr;
        if (!item || !base || !ue::cdo(item) || !ue::is_a(ue::cdo(item), base)) { core_log("give: ignored a gift that is no item: " + cls); continue; }
        std::vector<ue::UClass*> mods;
        for (size_t at = all.find('|'); at != std::string::npos;) {
            size_t next = all.find('|', at + 1);
            std::string mp = all.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
            ue::UClass* mk = mp.rfind("/Game/", 0) == 0 ? load_class(mp) : nullptr;
            if (mk && mod_base && ue::cdo(mk) && ue::is_a(ue::cdo(mk), mod_base)) mods.push_back(mk);
            else core_log("give: ignored a mod that is no item mod: " + mp);
            at = next;
        }
        try {
            give_to_player(item, m.a, static_cast<int32_t>(m.id & 0xFFFFFFFFu), static_cast<int32_t>(m.id >> 32), mods);
        } catch (const std::exception&) {
            g.gift_inbox.push_back(m);  // no player right now: next frame
            continue;
        }
        g.gifts_received++;
        g.last_gift = "got " + std::to_string(m.a) + "x " + ue::name_of(item) + (mods.empty() ? "" : " with " + std::to_string(mods.size()) + " mod(s)") + " from the other player";
        core_log("give: " + g.last_gift);
    }
}

// ---------------------------------------------------------------- one clock
// The game's calendar runs in each world: the host waiting, sleeping or flying the ship moves its time, a menu stops
// it. The client follows the host's hour: behind by more than 6 game minutes, its calendar jumps ahead; ahead, it
// waits (RequestPauseTime) until the host's catches up.
ue::UObject* calendar() {
    static ue::UObject* c = nullptr;
    static double next_look = 0;
    if (c && ue::alive(c)) return c;
    if (g.now < next_look) return nullptr;
    next_look = g.now + 2.0;
    std::vector<ue::UObject*> v = ue::instances(ue::find_class("/Script/Arkansas.GameCalendar"), false, 1);
    return c = v.empty() ? nullptr : v[0];
}

float raw_hour(ue::UObject* c) {
    ue::UFunction* f = ue::find_func(ue::class_of(c), "K2_GetRawHour");
    try { return f ? static_cast<float>(ue::call(c, f, json::object()).get("ReturnValue").asNumber(-1)) : -1.0f; } catch (const std::exception&) { return -1.0f; }
}

bool game_paused() {
    static ue::UObject* gs = ue::cdo(ue::find_class("/Script/Engine.GameplayStatics"));
    ue::UFunction* f = gs ? ue::find_func(ue::class_of(gs), "IsGamePaused") : nullptr;
    std::string err;
    ue::UObject* pc = ue::resolve("pc", &err);
    if (!f || !pc) return false;
    json a = json::object();
    a.set("WorldContextObject", ue::addr(pc));
    try { return ue::call(gs, f, a).get("ReturnValue").asBool(false); } catch (const std::exception&) { return false; }
}

void send_clock() {
    ue::UObject* c = calendar();
    float h = c ? raw_hour(c) : -1.0f;
    if (h >= 0) send_typed(kClock, &h, 4);
}

void hold_clock(ue::UObject* c, bool on) {
    if (g.clock_held == on) return;
    json a = json::object();
    a.set("RequestContext", "tow2coop");
    call_on(c, on ? "RequestPauseTime" : "RequestUnpauseTime", a);
    g.clock_held = on;
    if (on) g.clock_holds++;
}

void keep_clock() {
    ue::UObject* c = calendar();
    if (!c) return;
    bool fresh = g.host_hour >= 0 && g.now - g.host_hour_at < 5.0 && g.in_gameplay && join::joined();
    if (!fresh) { hold_clock(c, false); return; }  // no host to follow: the client's own time runs
    float mine = raw_hour(c);
    if (mine < 0) return;
    float d = std::remainder(g.host_hour - mine, 24.0f);  // hours, -12..12
    if (d > 0.1f) {  // behind by more than 6 game minutes: jump to the host's time
        hold_clock(c, false);
        json a = json::object();
        a.set("Days", 0.0).set("Hours", 0.0).set("Minutes", d * 60.0).set("Seconds", 0.0);
        call_on(c, "K2_AdvanceTime", a);
        g.clock_jumps++;
        g.host_hour = -1;  // the next report tells where we are
    } else {
        hold_clock(c, d < -0.02f);  // ahead: wait for the host (a menu stopped its clock)
    }
}

// ---------------------------------------------------------------- death and reload
// The friend's own game must never load one of its own saves into the joined game: it would put a stale or foreign
// world under the host's (probe 85: "Continue" after a death loads the newest local save). When the friend dies, the
// host's current game is fetched at once and held; "Continue" (or Load) then loads it with the friend's last kept
// character. A trauma kit revive drops it.
void watch_life(ue::UObject* pawn) {
    g.next_life_check = g.now + 0.5;
    std::string err;
    ue::UObject* hc = ue::resolve(ue::addr(pawn) + ".HealthComponent", &err);
    ue::UFunction* f = hc ? ue::find_func(ue::class_of(hc), "GetStateOfBeing") : nullptr;
    if (!f) return;
    std::string st;
    try { st = ue::call(hc, f, json::object()).get("ReturnValue").asString(); } catch (const std::exception&) { return; }
    bool dead = st == "Dying" || st == "Dead";
    if (dead && !g.died) {
        g.died = true, g.deaths++;
        join::hold(true);
        g.campaign_wanted = true, g.next_campaign_ask = 0;
        core_log("death: the host's current game is fetched; Continue loads it with your last kept character");
    } else if (!dead && g.died) {
        g.died = false, g.campaign_wanted = false;
        join::hold(false);
        join::discard();
        core_log("death: revived, no reload");
    }
}

}  // namespace

int on_load_request(const std::wstring& name) {
    if (!g.on || name.empty() || name.rfind(L"CoopHost", 0) == 0) return -1;  // the mod's own hand-over loads
    std::string n(name.begin(), name.end());
    if (g.host) {
        // The host's game loads a save (death Continue, Load, Quickload): that file, complete on disk, goes to the
        // friend before the load starts. LoadGame only returns once the new world is up (probe 88: 16 s, ticking the
        // engine inside), so the friend loads it at the same time instead of after the host.
        if (!g.have_peer) return -1;
        if (request_resync("the host loaded a save", true, name)) {
            g.reload_handed_at = g.now, g.reloads_handed++;
            uint32_t body[4] = {static_cast<uint32_t>(g.port + 1), g.me, 1, static_cast<uint32_t>(g.resyncs)};
            send_typed(kResync, body, 16);  // the first notice now: the frames that send the others wait for the load
            core_log("reload: " + n + " goes to the other player as the host's game loads it");
        } else {
            g.reload_handed_at = -1e9;  // an earlier reload's timestamp must not skip this one's hand-over
            core_log("reload: " + n + " could not go to the other player now (" + g.last_error + "); the hand-over follows once loaded");
        }
        return -1;
    }
    if (!join::joined()) return -1;
    // The joined friend's game loads one of its own saves: the host's current game comes instead.
    join::hold(false);
    if (!g.campaign_wanted && g.now >= g.fetch_quiet_until) g.campaign_wanted = true, g.next_campaign_ask = 0;
    g.loads_redirected++;
    core_log("reload: your game's own load (" + n + ") is replaced by the host's current game");
    return 21;  // ELoadGameResult::LoadCanceledByUser
}

void on_load_started(const std::wstring& name, uint8_t result) {
    if (!g.on || !g.host || result == 0 || name.empty() || name.rfind(L"CoopHost", 0) == 0) return;
    if (g.now - g.reload_handed_at < 60.0) g.reload_handed_at = -1e9;  // the friend got a save the host did not load
    core_log("reload: the game did not load " + std::string(name.begin(), name.end()) + " (result " + std::to_string(result) + ")");
}

namespace {
// One datagram from the other player: UDP gives its address (from), Steam its SteamID (from_id).
void handle_packet(const char* buf, int n, const sockaddr_in* from, uint64_t from_id) {
    if (n < static_cast<int>(sizeof(Header))) { g.bad++; return; }
    Header h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != g.magic || h.size != n || h.sender == g.me) {
        static bool warned = false;  // "T2C" + protocol digit: another digit is another version of the mod
        if (!warned && g.magic == kMagic && h.magic != kMagic && (h.magic & 0x00FFFFFFu) == (kMagic & 0x00FFFFFFu)) {
            warned = true;
            core_log(std::string("the other player runs another version of the mod (protocol ") + static_cast<char>(h.magic >> 24) + ", this one " +
                     static_cast<char>(kMagic >> 24) + "): both players need the same version");
        }
        g.bad++;
        return;
    }
    if (g.host && !g.have_peer) {  // first client becomes the peer
        if (from) g.peer = *from;
        g.peer_id = from_id;
        g.have_peer = true;
        if (g.steam) g.session = -1, core_log("steam: a friend joins: " + steamnet::name(from_id));
    }
    if (g.remote_id && h.sender != g.remote_id) { g.bad++; return; }  // shortcut: one remote player only
    if (h.type == kHello) { g.remote_id = h.sender; return; }
    if (h.type == kBye) { g.remote_id = 0; g.snaps.clear(); return; }
    // One sequence for every packet type, newest wins: a reordered packet is dropped. Damage and story updates are
    // resent until acknowledged, looks every 2 s; door and container changes go out three times.
    if (g.remote_id == h.sender && h.seq <= g.remote_last_seq) { g.stale++; return; }  // reordered or duplicate
    if (g.remote_id != h.sender && g.host) story::reset_peer();  // a (re)joined client gets the story log replayed
    g.remote_id = h.sender;
    g.remote_last_seq = h.seq;
    if (h.type == kResync && !g.host) { g.last_recv_t = g.now; client_fetch_resync(buf + sizeof(Header), n - static_cast<int>(sizeof(Header))); return; }
    if (h.type == kInteract && take_xp(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)))) { g.last_recv_t = g.now; return; }
    if (h.type == kTravel) { g.last_recv_t = g.now; on_travel(buf + sizeof(Header), n - static_cast<int>(sizeof(Header))); return; }  // even while settling
    if (h.type == kInteract && take_gift(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)))) { g.last_recv_t = g.now; return; }
    if (!g.in_gameplay || g.now < g.settle_until) { g.last_recv_t = g.now; g.stale++; return; }  // not in a settled world: data ignored
    if (h.type == kResync && !g.host) {  // handled even while not settled: the client may be anywhere
        client_fetch_resync(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)));
        return;
    }
    if (h.type == kInteract) {
        int body = n - static_cast<int>(sizeof(Header));
        InteractMsg m{};
        if (body < static_cast<int>(offsetof(InteractMsg, path)) || body > static_cast<int>(sizeof m)) { g.bad++; return; }
        memcpy(&m, buf + sizeof(Header), body);
        if (m.len > sizeof m.path || body != static_cast<int>(offsetof(InteractMsg, path)) + m.len) { g.bad++; return; }
        g.recv++;
        apply_interact(m);
        return;
    }
    if (h.type == kResyncRequest && g.host) {
        uint32_t reason = 1;
        if (n == static_cast<int>(sizeof(Header)) + 4) memcpy(&reason, buf + sizeof(Header), 4);
        g.resync_requests++;
        request_resync(reason == 2 ? "client joins from the main menu" : reason == 3 ? "the client loads (its game's Continue or Load)"
                       : reason == 4 ? "the client died" : "client could not apply a quest outcome", reason >= 3);
        return;
    }
    if (h.type == story::kStory) {
        g.recv++;
        story::on_story(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)), send_typed);
        return;
    }
    if (h.type == story::kStoryAck) {
        story::on_ack(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)));
        return;
    }
    if (h.type == kEnemies && !g.host) {
        Enemies e;
        int head = static_cast<int>(offsetof(Enemies, rec));
        memcpy(&e, buf, std::min(n, static_cast<int>(sizeof e)));
        if (n < head || e.count > kMaxEnemyRecs || n != head + e.count * static_cast<int>(sizeof(EnemyRec))) { g.bad++; return; }
        g.recv++;
        apply_npcs(e);
        return;
    }
    if (h.type == kCombat && g.host) {
        CombatMsg m;
        int head = static_cast<int>(offsetof(CombatMsg, rec));
        memcpy(&m, buf, std::min(n, static_cast<int>(sizeof m)));
        if (n < head || m.count > kMaxCombatRecs || n != head + m.count * static_cast<int>(sizeof(CombatRec))) { g.bad++; return; }
        g.recv++;
        host_apply_combat(m);
        return;
    }
    if (h.type == kDamage && g.host) {
        if (n != static_cast<int>(sizeof(Damage))) { g.bad++; return; }
        Damage d;
        memcpy(&d, buf, sizeof d);
        g.recv++;
        if (std::find(g.dmg_seen.begin(), g.dmg_seen.end(), d.eid) == g.dmg_seen.end()) {
            apply_damage(d);
            g.dmg_seen.push_back(d.eid);
            if (g.dmg_seen.size() > 1024) g.dmg_seen.erase(g.dmg_seen.begin(), g.dmg_seen.begin() + 512);
        }
        send_typed(kDamageAck, &d.eid, 4);
        return;
    }
    if (h.type == kAppearance) { on_face(buf + sizeof(Header), n - static_cast<int>(sizeof(Header))); return; }
    if (h.type == kClock && !g.host) {
        float hour;
        if (n != static_cast<int>(sizeof(Header)) + 4) { g.bad++; return; }
        memcpy(&hour, buf + sizeof(Header), 4);
        if (hour >= 0 && hour < 24.5f) g.host_hour = hour, g.host_hour_at = g.now;
        return;
    }
    if (h.type == kLook) {
        if (n != static_cast<int>(sizeof(Header) + sizeof(Look))) { g.bad++; return; }
        Look l;
        memcpy(&l, buf + sizeof(Header), sizeof l);
        for (int i = 0; i < 2; i++) g.look[i].assign(l.item[i], strnlen(l.item[i], sizeof l.item[i]));
        g.look_helmet_shown = l.helmet_shown != 0;
        g.have_look = true;
        g.recv++;
        return;
    }
    if (h.type == kDamageAck && !g.host) {
        uint32_t eid;
        if (n != static_cast<int>(sizeof(Header)) + 4) { g.bad++; return; }
        memcpy(&eid, buf + sizeof(Header), 4);
        for (size_t i = 0; i < g.dmg_pending.size(); i++)
            if (g.dmg_pending[i].d.eid == eid) { g.dmg_pending.erase(g.dmg_pending.begin() + i); break; }
        return;
    }
    if (h.type != kState || n != static_cast<int>(sizeof(State))) { g.bad++; return; }
    State st;
    memcpy(&st, buf, sizeof st);
    g.recv++;
    g.last_recv_t = g.now;
    g.snaps.push_back({g.now, st.px, st.py, st.pz, st.vx, st.vy, st.vz, st.yaw});
    g.puppet_map = st.map_hash;
    g.peer_dead = (st.flags & 1) != 0;
    while (g.snaps.size() > 64) g.snaps.pop_front();
}

void receive() {
    if (g.steam) {
        steamnet::receive(0, [](uint64_t from_id, const char* data, int n) {
            if (n > 1500 || (g.have_peer && from_id != g.peer_id)) { g.bad++; return; }  // only the player found by presence
            handle_packet(data, n, nullptr, from_id);
        });
        steamnet::receive(1, [](uint64_t from_id, const char* data, int n) {  // the campaign hand-over (reliable, ordered)
            if (!g.host && g.have_peer && from_id == g.peer_id) join::on_steam_data(data, n);
        });
        return;
    }
    char buf[1500];
    for (;;) {
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(g.s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        handle_packet(buf, n, &from, 0);
    }
}

// Puppet hygiene; `full` once after spawning, then a cheap re-check of the AI brain every 2 s (something may restart it).
void call_quiet(const std::string& ref, const char* fn, const json& args) {
    std::string err;
    ue::UObject* o = ue::resolve(ref, &err);
    if (!o) return;
    ue::UFunction* f = ue::find_func(ue::class_of(o), fn);
    if (!f) return;
    try { ue::call(o, f, args); } catch (const std::exception& e) { g.last_error = std::string(fn) + ": " + e.what(); }
}

const char* const kArmorSlot[2] = {"ArmorSet", "Helmet"};

ue::UObject* worn(ue::UObject* equipment, int slot) {  // the armor set (0) or helmet (1) worn, nullptr if none
    ue::UFunction* f = ue::find_func(ue::class_of(equipment), slot == 0 ? "GetEquippedArmor" : "GetEquippedHelmet");
    std::string r = f ? ue::call(equipment, f, json::object()).get("ReturnValue").asString() : std::string();
    std::string err;
    return r.rfind("0x", 0) == 0 ? ue::resolve(r.substr(0, r.find(' ')), &err) : nullptr;
}

ue::UClass* load_class(const std::string& path) {  // a Blueprint class by path, loaded if needed (blocking)
    if (ue::UClass* c = ue::find_class(path)) return c;
    ue::UClass* k = ue::find_class("/Script/Engine.KismetSystemLibrary");
    json a = json::object();
    a.set("AssetClass", path);
    try { ue::call(ue::cdo(k), ue::find_func(k, "LoadClassAsset_Blocking"), a); } catch (const std::exception& e) { g.last_error = e.what(); }
    return ue::find_class(path);
}

// The stand-in wears the other player's armor set and helmet as a visual override (nothing is equipped or given to the
// NPC). Until their first look message arrives: the local player's (the joined client plays a copy of the host's
// character). Same piece as ours: our item (its mods show); another one: the class defaults (unmodded).
std::string look_key() { return g.look[0] + "|" + g.look[1] + (g.look_helmet_shown ? "|shown" : "|hidden"); }

bool helmet_shown(ue::UObject* equipment) {  // a helmet worn and not hidden (the stand-in mirrors it)
    if (!worn(equipment, 1)) return false;
    try { return ue::eval_path("pawn.AppearanceComponent.bHelmetVisible").asBool(true); } catch (const std::exception&) { return true; }
}

void dress_outfit() {
    g.look_applied = g.have_look ? look_key() : std::string();  // first: a failure is not retried every frame
    std::string err;
    ue::UObject* my_eq = ue::resolve("pawn.EquipmentComponent", &err);
    ue::UObject* their_eq = g.puppet ? ue::resolve(ue::addr(g.puppet) + ".EquipmentComponent", &err) : nullptr;
    if (!my_eq || !their_eq) return;
    for (int i = 0; i < 2; i++) {
        ue::UObject* mine = worn(my_eq, i);
        std::string want = g.have_look ? g.look[i] : mine ? ue::path_of(ue::class_of(mine)) : std::string();
        ue::UObject* item = mine && ue::path_of(ue::class_of(mine)) == want ? mine : nullptr;
        // A path from the other player (trust boundary): only a /Game/ class of this slot's item type is used.
        ue::UClass* base = ue::find_class(i == 0 ? "/Script/Arkansas.ArmorSet" : "/Script/Arkansas.Helmet");
        if (!item && base && want.rfind("/Game/", 0) == 0)
            if (ue::UClass* c = load_class(want))
                if (ue::cdo(c) && ue::is_a(ue::cdo(c), base)) item = ue::cdo(c);
        json a = json::object();
        a.set("Slot", kArmorSlot[i]);
        if (!item) { call_quiet(ue::addr(their_eq), "ClearArmorVisualOverride", a); continue; }
        a.set("OverrideItem", ue::addr(item));
        call_quiet(ue::addr(their_eq), "SetArmorVisualOverride", a);
    }
    // Without a helmet override the stand-in body shows its own headgear (a dock worker's cap): hide it then.
    bool shown = g.have_look ? g.look_helmet_shown : helmet_shown(my_eq);
    try { ue::set_path(ue::addr(g.puppet) + ".AppearanceComponent.bHelmetVisible", json(shown)); } catch (const std::exception& e) { g.last_error = std::string("helmet: ") + e.what(); }
    call_quiet(ue::addr(g.puppet) + ".AppearanceComponent", "RefreshFullAppearance", json::object());
}

const char kFaceRoot[] = "/Game/Art/CHAR/AppearanceDataAssets/";

std::string asset_path(const ue::json& v) {  // "0x... Class /Path.Name" -> "/Path.Name"
    std::string s = v.asString();
    size_t a = s.find(' '), b = a == std::string::npos ? a : s.find(' ', a + 1);
    return s.rfind("0x", 0) == 0 && b != std::string::npos ? s.substr(b + 1) : std::string();
}

std::string face_text() {
    std::string out;
    auto put = [&](const std::string& k, std::string path) {
        if (path.empty()) return;
        if (path.rfind(kFaceRoot, 0) == 0) path = "~" + path.substr(sizeof kFaceRoot - 1);
        out += k + "=" + path + "\n";
    };
    ue::json c = ue::eval_path("pawn.AppearanceComponent.Customization");
    for (const auto& kv : c.members()) {
        if (kv.second.isNumber()) {
            char b[32];
            snprintf(b, sizeof b, "%.5g", kv.second.asNumber());
            out += "f." + kv.first + "=" + b + "\n";
        } else put("c." + kv.first, asset_path(kv.second));
    }
    put("head", asset_path(ue::eval_path("pawn.AppearanceComponent.DefaultHead")));
    std::string err;
    for (const char* body : {"CurrentBody", "DefaultBody"}) {
        try { std::string b = asset_path(ue::eval_path(std::string("pawn.AppearanceComponent.") + body)); if (!b.empty()) { put("body", b); break; } } catch (const std::exception&) {}
    }
    try { out += "gender=" + ue::eval_path("pawn.AppearanceComponent.AppearanceGender").get("TagName").asString() +"\n"; } catch (const std::exception&) {}
    return out;
}

// Our look to the other player, in pieces (it is ~2 KB); sent every 5 s (UDP: a lost piece comes again).
void send_face() {
    std::string t = face_text();
    uint32_t v = static_cast<uint32_t>(fnv(t)) | 1;
    size_t n = (t.size() + sizeof(FacePart::text) - 1) / sizeof(FacePart::text);
    if (n == 0 || n > 8) return;
    for (size_t i = 0; i < n; i++) {
        FacePart f{};
        f.version = v, f.part = static_cast<uint8_t>(i), f.parts = static_cast<uint8_t>(n);
        std::string piece = t.substr(i * sizeof f.text, sizeof f.text);
        f.len = static_cast<uint16_t>(piece.size());
        memcpy(f.text, piece.data(), piece.size());
        send_typed(kAppearance, &f, static_cast<int>(offsetof(FacePart, text) + piece.size()));
    }
}

void on_face(const char* body, int len) {
    if (len < static_cast<int>(offsetof(FacePart, text))) return;
    FacePart f{};
    memcpy(&f, body, std::min<size_t>(len, sizeof f));
    if (!f.parts || f.parts > 8 || f.part >= f.parts || f.len > sizeof f.text || offsetof(FacePart, text) + f.len > static_cast<size_t>(len)) return;
    if (f.version != g.face_version) g.face_version = f.version, g.face_parts.assign(f.parts, std::string()), g.face_parts.shrink_to_fit();
    if (g.face_parts.size() != f.parts) return;
    g.face_parts[f.part] = std::string(f.text, f.len) + "\x01";  // marks the piece as received (a piece may be empty)
    std::string all;
    for (const std::string& piece : g.face_parts) {
        if (piece.empty()) return;
        all += piece.substr(0, piece.size() - 1);
    }
    g.face_in = all;
}

// The other player's look on the stand-in. Every asset comes from the other player (trust boundary): only a /Game/
// appearance asset of the field's own type is used; a float only for a float field.
void dress_face() {
    g.face_applied = g.face_in;
    std::string p = ue::addr(g.puppet) + ".AppearanceComponent";
    std::string err;
    ue::UObject* comp = ue::resolve(p, &err);
    ue::FProperty* cp = comp ? ue::find_prop(ue::class_of(comp), "Customization") : nullptr;
    ue::UStruct* cs = cp ? ue::struct_of(cp) : nullptr;
    if (!cs) return;
    static std::unordered_map<std::string, std::string> types;  // field -> its type ("ObjectProperty<HairAppearances>")
    if (types.empty()) {  // the description is kept: a range-for over a temporary's member dangles
        json d = ue::describe_struct(cs, false);
        for (const auto& f : d.get("props").items()) types[f.get("name").asString()] = f.get("type").asString();
    }
    auto load = [&](std::string path, const std::string& want) -> ue::UObject* {
        if (!path.empty() && path[0] == '~') path = kFaceRoot + path.substr(1);
        if (path.rfind("/Game/", 0) != 0 || path.find(' ') != std::string::npos) return nullptr;
        ue::UObject* o = ue::find_path(path);
        if (!o) {
            ue::UClass* k = ue::find_class("/Script/Engine.KismetSystemLibrary");
            json a = json::object();
            a.set("Asset", path);
            try { ue::call(ue::cdo(k), ue::find_func(k, "LoadAsset_Blocking"), a); } catch (const std::exception&) {}
            o = ue::find_path(path);
        }
        return o && (want.empty() || ue::class_name(o) == want) ? o : nullptr;
    };
    int set = 0, skipped = 0;
    std::string why;
    std::vector<std::pair<std::string, std::string>> lines;  // head and body first: a new head may reset the rest
    for (size_t at = 0; at < g.face_in.size();) {
        size_t nl = g.face_in.find('\n', at);
        std::string line = g.face_in.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? g.face_in.size() : nl + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        auto kv = std::make_pair(k, line.substr(eq + 1));
        if (k == "head" || k == "body" || k == "gender") lines.insert(lines.begin(), kv);
        else lines.push_back(kv);
    }
    for (const auto& kv : lines) {
        const std::string &k = kv.first, &v = kv.second;
        try {
            if (k.rfind("c.", 0) == 0 && types.count(k.substr(2))) {
                const std::string& t = types[k.substr(2)];
                size_t lt = t.find('<');
                if (t.rfind("ObjectProperty<", 0) != 0 || lt == std::string::npos) continue;
                if (ue::UObject* o = load(v, t.substr(lt + 1, t.size() - lt - 2))) ue::set_path(p + ".Customization." + k.substr(2), json(ue::addr(o))), set++;
                else if (skipped++ == 0) why = k + " " + v;
            } else if (k.rfind("f.", 0) == 0 && types.count(k.substr(2)) && types[k.substr(2)] == "FloatProperty") {
                ue::set_path(p + ".Customization." + k.substr(2), json(atof(v.c_str()))), set++;
            } else if (k == "head") {
                if (ue::UObject* o = load(v, "")) { json a = json::object(); a.set("Head", ue::addr(o)); call_quiet(p, "SetDefaultHead", a); }
            } else if (k == "body") {
                if (ue::UObject* o = load(v, "")) { json a = json::object(); a.set("Body", ue::addr(o)); call_quiet(p, "SetDefaultBody", a); }
            } else if (k == "gender" && (v == "Appearance.Gender.Male" || v == "Appearance.Gender.Female")) {
                ue::set_path(p + ".AppearanceGender.TagName", json(v));
            }
        } catch (const std::exception& e) {
            g.last_error = "face " + k + ": " + e.what();
            if (skipped++ == 0) why = k + ": " + e.what();
        }
    }
    ue::set_path(p + ".bEnableCustomization", json(true));
    core_log("look: the other player's look on the stand-in, " + std::to_string(set) + " fields set" +
             (skipped ? ", " + std::to_string(skipped) + " skipped (first: " + why + ")" : ""));
    dress_outfit();  // refreshes the whole appearance
}

void send_look() {
    std::string err;
    ue::UObject* eq = ue::resolve("pawn.EquipmentComponent", &err);
    if (!eq) return;
    Look l{};
    for (int i = 0; i < 2; i++) {
        ue::UObject* item = worn(eq, i);
        std::string p = item ? ue::path_of(ue::class_of(item)) : std::string();
        if (p.size() < sizeof l.item[i]) memcpy(l.item[i], p.data(), p.size());  // never cut: a cut path names no class
    }
    l.helmet_shown = helmet_shown(eq) ? 1 : 0;
    send_typed(kLook, &l, sizeof l);
}

// The joined client plays a copy of the host's character, so the stand-in takes the local player's look: the
// Customization struct (floats and shared appearance-asset pointers only, safe to copy), head and body, then a refresh.
void dress_puppet() {
    if (!g.face_in.empty()) { dress_face(); return; }  // the other player's own look (each player has their own character)
    std::string err;
    ue::UObject* mine = ue::resolve("pawn.AppearanceComponent", &err);
    ue::UObject* theirs = g.puppet ? ue::resolve(ue::addr(g.puppet) + ".AppearanceComponent", &err) : nullptr;
    if (!mine || !theirs) return;
    ue::FProperty* p = ue::find_prop(ue::class_of(mine), "Customization");
    if (!p || ue::field_type(p) != "StructProperty" || ue::find_prop(ue::class_of(theirs), "Customization") != p) return;  // same layout only
    // An NPC body ignores Customization unless enabled: the stand-in was bald without a helmet (probe 56, pictures).
    ue::set_path(ue::addr(theirs) + ".bEnableCustomization", json(true));
    memcpy(reinterpret_cast<uint8_t*>(theirs) + ue::prop_offset(p), reinterpret_cast<uint8_t*>(mine) + ue::prop_offset(p), ue::prop_size(p));
    json body = json::object(), head = json::object();
    ue::UObject* b = ue::resolve("pawn.AppearanceComponent.CurrentBody", &err);
    if (!b) b = ue::resolve("pawn.AppearanceComponent.DefaultBody", &err);
    ue::UObject* h = ue::resolve("pawn.AppearanceComponent.DefaultHead", &err);
    if (b) { body.set("Body", ue::addr(b)); call_quiet(ue::addr(theirs), "SetDefaultBody", body); }
    if (h) { head.set("Head", ue::addr(h)); call_quiet(ue::addr(theirs), "SetDefaultHead", head); }
    dress_outfit();
}

void tame_puppet(bool full) {
    if (!g.puppet || !standin_ok()) return;
    std::string p = ue::addr(g.puppet);
    json reason = json::object();
    reason.set("Reason", "coop puppet");
    std::string err;
    ue::UObject* brain = ue::resolve(p + ".Controller.BrainComponent", &err);
    if (brain) {
        json r = ue::call(brain, ue::find_func(ue::class_of(brain), "IsRunning"), json::object());
        if (r.get("ReturnValue").asBool(false)) {
            call_quiet(ue::addr(brain), "StopLogic", reason);
            g.brain_stops++;
        }
    }
    // Its movement component would zero the velocity we write every frame (the AI controller is kept): the stand-in
    // slid in a frozen pose in the two-PC test. Off, the written velocity drives the walk (probe 56: speed 0 -> 400).
    json no = json::object();
    no.set("bEnabled", false);
    call_quiet(p + ".CharacterMovement", "SetComponentTickEnabled", no);
    if (!full) return;
    try { dress_puppet(); } catch (const std::exception& e) { g.last_error = std::string("dress: ") + e.what(); }
    call_quiet(p + ".Controller", "StopMovement", json::object());
    json on = json::object(), off = json::object();
    on.set("bEnabled", true);
    call_quiet(p + ".HealthComponent", "SetGod", on);
    off.set("bNewInteractionsEnabled", false);
    call_quiet(p + ".InteractableComponent", "SetInteractionsEnabled", off);
    json nt = json::object();
    nt.set("bNewTargetable", false);
    call_quiet(p + ".TargetableComponent", "SetTargetable", nt);
    call_quiet(p + ".ChatterComponent", "Deactivate", json::object());
    standin_team(false);  // neutral to all until the party fights (co-op combat)
}

void destroy_puppet() {
    if (g.puppet && standin_ok()) {
        try { ue::call(g.puppet, ue::find_func(ue::class_of(g.puppet), "K2_DestroyActor"), json::object()); } catch (...) {}
    }
    g.puppet = nullptr;
}

bool spawn_puppet(const Snapshot& s) {
    // A body of the player's gender (the stand-in takes the player's appearance, which needs the matching skeleton).
    std::string gerr;
    json gender = json();
    try { gender = ue::eval_path("pawn.AppearanceComponent.AppearanceGender.TagName"); } catch (...) {}
    const char* klass = gender.asString().find("Female") != std::string::npos ? kPuppetClassFemale : kPuppetClass;
    ue::UClass* cls = load_class(klass);
    if (!cls) { g.last_error = "puppet class not loaded"; return false; }
    std::string err;
    ue::UObject* world_ctx = ue::resolve("pc", &err);
    ue::UObject* gs = ue::cdo(ue::find_class("/Script/Engine.GameplayStatics"));
    json xf = json::object(), tr = json::array(), rot = json::object(), sc = json::array();
    tr.push(s.px).push(s.py).push(s.pz);
    double yaw = s.yaw * 3.14159265358979 / 180.0;
    rot.set("X", 0.0).set("Y", 0.0).set("Z", std::sin(yaw / 2)).set("W", std::cos(yaw / 2));
    sc.push(1.0).push(1.0).push(1.0);
    xf.set("Rotation", rot).set("Translation", tr).set("Scale3D", sc);
    json a = json::object();
    a.set("WorldContextObject", ue::addr(world_ctx)).set("ActorClass", ue::addr(cls)).set("SpawnTransform", xf);
    a.set("CollisionHandlingOverride", "AlwaysSpawn");
    json r = ue::call(gs, ue::find_func(ue::class_of(gs), "BeginDeferredActorSpawnFromClass"), a);
    std::string ref = r.get("ReturnValue").asString();
    ue::UObject* actor = ref.rfind("0x", 0) == 0 ? reinterpret_cast<ue::UObject*>(static_cast<uintptr_t>(strtoull(ref.c_str(), nullptr, 16))) : nullptr;
    if (!actor || !ue::alive(actor)) { g.last_error = "deferred spawn returned " + ref; return false; }
    // Keep the stand-in out of the host's saves (else every save made during co-op holds an extra dock worker). The
    // flag only counts before the actor finishes spawning (run 32: off afterwards, the save still kept it).
    ue::set_path(ue::addr(actor) + ".bShouldSaveGameState", json(false));
    json f = json::object();
    f.set("Actor", ue::addr(actor)).set("SpawnTransform", xf);
    ue::call(gs, ue::find_func(ue::class_of(gs), "FinishSpawningActor"), f);
    // The NPC keeps its own AI controller (game systems expect NPCs to have one; a controller-less puppet is a null
    // dereference waiting to happen), but its behaviour is stopped and it can't be talked to, targeted or killed.
    ue::set_path(ue::addr(actor) + ".CharacterMovement.MovementMode", "MOVE_Walking");
    g.puppet = actor;
    g.puppet_cls = ue::class_of(actor);
    g.puppet_in_air = false;
    tame_puppet(true);
    return true;
}

void drive_puppet() {
    if (g.snaps.empty()) return;
    if (g.puppet_map != g.my_map) { destroy_puppet(); return; }  // the other player is in another map
    // Render 100 ms in the past between two snapshots; extrapolate at most 200 ms past the newest one.
    double t = g.now - 0.1;
    const Snapshot* a = &g.snaps.front();
    const Snapshot* b = &g.snaps.back();
    for (size_t i = 0; i + 1 < g.snaps.size(); i++)
        if (g.snaps[i].t <= t && g.snaps[i + 1].t >= t) { a = &g.snaps[i]; b = &g.snaps[i + 1]; break; }
    double k = b->t > a->t ? (t - a->t) / (b->t - a->t) : 1.0;
    double ext = 0;
    if (k > 1) { ext = std::min(t - b->t, 0.2); k = 1; a = b; }
    k = std::max(0.0, k);
    double pos[3] = {a->px + (b->px - a->px) * k + b->vx * ext, a->py + (b->py - a->py) * k + b->vy * ext, a->pz + (b->pz - a->pz) * k + b->vz * ext};
    float dyaw = std::remainder(b->yaw - a->yaw, 360.0f);
    double yaw = a->yaw + dyaw * k;
    if (g.peer_dead) {  // a dead player has no stand-in: the enemies on it turn to the living (probe 91: they keep it)
        if (g.puppet) destroy_puppet(), core_log("combat: the other player died, their stand-in leaves until they live again");
        return;
    }
    if (!g.puppet || !standin_ok()) {
        g.puppet = nullptr;
        Snapshot s = *b;
        s.px = pos[0], s.py = pos[1], s.pz = pos[2];
        if (!spawn_puppet(s)) return;
    }
    if (g.now >= g.next_tame) {
        g.next_tame = g.now + 2.0;
        tame_puppet(false);
    }
    if (!g.face_in.empty() && g.face_in != g.face_applied) {  // the other player's look arrived or changed
        try { dress_face(); } catch (const std::exception& e) { g.last_error = std::string("face: ") + e.what(); }
    }
    if (g.have_look && look_key() != g.look_applied) {  // the other player changed armor or helmet
        try { dress_outfit(); } catch (const std::exception& e) { g.last_error = std::string("outfit: ") + e.what(); }
    }
    static Fn set;
    if (!base_fn(set, "/Script/Engine.Actor", "K2_SetActorLocationAndRotation")) return;
    alignas(16) uint8_t buf[512] = {};
    double rot[3] = {0, yaw, 0};
    memcpy(buf + set.off("NewLocation"), pos, 24);
    memcpy(buf + set.off("NewRotation"), rot, 24);
    buf[set.off("bTeleport")] = 1;
    ue::process_event(g.puppet, set.f, buf);
    // Animation reads the movement component's velocity, and its mode for the in-air state (bMovementStateIsInAir).
    double v[3] = {b->vx, b->vy, b->vz};
    bool in_air = std::fabs(b->vz) > 50.0;
    if (in_air != g.puppet_in_air) {
        g.puppet_in_air = in_air;
        try { ue::set_path(ue::addr(g.puppet) + ".CharacterMovement.MovementMode", in_air ? "MOVE_Falling" : "MOVE_Walking"); } catch (const std::exception&) {}
    }
    if (ue::UObject* cmc = ue::resolve(ue::addr(g.puppet) + ".CharacterMovement")) {
        if (ue::FProperty* p = ue::find_prop(ue::class_of(cmc), "Velocity")) memcpy(reinterpret_cast<uint8_t*>(cmc) + ue::prop_offset(p), v, 24);
        if (ue::FProperty* p = ue::find_prop(ue::class_of(cmc), "ComponentVelocity")) memcpy(reinterpret_cast<uint8_t*>(cmc) + ue::prop_offset(p), v, 24);
    }
}
}  // namespace

// Steam: find the other player through rich presence (every 2 s). A host shows "host" and takes the first friend that
// shows "join:<host's SteamID>"; a client takes a friend that shows "host" (TOW2_COOP_HOST / ini host= narrows it to a
// Steam name or SteamID) and shows "join:<that friend>". Each side sends a hello first: that accepts the other's
// session, so no Steam callback is needed. The keys are re-set every pass (the game may clear rich presence).
void steam_discovery() {
    if (g.now < g.next_discovery) return;
    g.next_discovery = g.now + 2.0;
    if (!steamnet::on()) {  // a game started by Steam sets up its Steam session after the mod's start (two-PC test, beta.2)
        static const double test_delay = atof(env("TOW2_COOP_STEAM_DELAY").c_str());  // tests only: Steam shows up this late
        static bool told = false;
        std::string why = "test delay";
        double t = g.now - g.start_time;
        if (t >= test_delay && steamnet::start(why)) core_log("co-op: " + why + ", " + std::to_string(static_cast<int>(t)) + " s after the mod started");
        else if (!told) told = true, core_log("co-op: waiting for the game's Steam session (" + why + ")");
        return;
    }
    const uint8_t none = 0;
    if (g.have_peer) {
        int st = steamnet::session_state(g.peer_id);
        if (st != g.session) {
            core_log("steam: session with " + steamnet::name(g.peer_id) + " state " + std::to_string(st) + " (3 = connected), relay " +
                     std::to_string(steamnet::relay_status()) + " (100 = ready)");
            g.session = st;
        }
        static double next_log = 0;  // a status line every 30 s: what a player sends when something looks wrong
        if (g.now >= next_log) {
            next_log = g.now + 30.0;
            core_log("steam: " + steamnet::name(g.peer_id) + " state " + std::to_string(st) + ", sent " + std::to_string(g.sent) + ", received " +
                     std::to_string(g.recv) + ", rejected " + std::to_string(g.bad) + (g.puppet ? ", stand-in shown" : ", no stand-in"));
        }
        steamnet::set_presence("tow2coop", g.host ? "host" : ("join:" + std::to_string(g.peer_id)).c_str());
        if (st != 3) send_typed(kHello, &none, 0);  // keeps asking until the session is up
        return;
    }
    steamnet::set_presence("tow2coop", g.host ? "host" : "");
    std::string want = g.host ? "join:" + std::to_string(steamnet::me()) : std::string("host");
    std::string pick = g.host ? std::string() : env("TOW2_COOP_HOST");
    // A named host is contacted directly, presence or not (its game details may be private); the host accepts friends.
    uint64_t named = pick.empty() ? 0 : steamnet::find_friend(pick);
    std::vector<uint64_t> candidates = named ? std::vector<uint64_t>{named} : steamnet::friends_in_game();
    if (g.host) {  // a client with private game details never shows its presence: take its pending session (two-PC test)
        steamnet::accept_friends();
        static double next_wait_log = 0;
        if (g.now >= next_wait_log) {  // what the host sees while nobody has joined, every 30 s
            next_wait_log = g.now + 30.0;
            std::string seen;
            for (uint64_t f : candidates) seen += " " + steamnet::name(f) + " '" + steamnet::presence(f, "tow2coop") + "'";
            core_log("steam: no client yet; friends playing:" + (seen.empty() ? std::string(" none") : seen) + "; relay " +
                     std::to_string(steamnet::relay_status()));
        }
    }
    for (uint64_t f : candidates) {
        steamnet::request_presence(f);
        if (!named && steamnet::presence(f, "tow2coop") != want) continue;
        if (!named && !pick.empty()) continue;  // a name was given but no such friend: wait for it to show up
        g.peer_id = f, g.have_peer = true, g.session = -1;
        if (!g.host) character::set_host("steam-" + std::to_string(f));  // the joined player's character for this host
        if (!g.host) steamnet::set_presence("tow2coop", ("join:" + std::to_string(f)).c_str());
        send_typed(kHello, &none, 0);
        core_log(std::string(g.host ? "steam: a friend joins: " : "steam: joining the host ") + steamnet::name(f));
        return;
    }
}

json face_debug() {
    json r = json::object();
    std::string local;
    try { local = face_text(); } catch (const std::exception& e) { local = std::string("error: ") + e.what(); }
    r.set("local", local).set("in", g.face_in).set("applied", !g.face_in.empty() && g.face_in == g.face_applied).set("error", g.last_error);
    return r;
}

// ---------------------------------------------------------------- party XP
// The game's AddXP (hooked in main.cpp) sees every XP gain with its reason. Kills, lock picks, hacks, conversations,
// interactions, automechs and taming are shared both ways; quest XP comes from the host's campaign only (the client's
// own quest XP is dropped: its quest graph follows the host's); discoveries, collectibles, bits and debug XP stay with
// the player who earned them (each player discovers and picks up on their own).
AddXpFn add_xp_original = nullptr, add_xp_entry = nullptr;
OpenTransferFn open_transfer_fn = nullptr;
WeaponModFn weapon_equip_mod = nullptr;
ArmorModFn armor_equip_mod = nullptr;

json mods_test(const json& p) {  // test bridge: the mods on an item, after installing "add" (a class path) if given
    std::string err;
    ue::UObject* item = ue::resolve(p.get("item").asString(), &err);
    if (!item) throw std::runtime_error("item: " + err);
    json r = json::object();
    if (p.get("add").isString()) {
        ue::UClass* k = load_class(p.get("add").asString());
        r.set("installed", k && install_mod(item, k));
    }
    json list = json::array();
    for (ue::UClass* m : mods_of(item)) list.push(ue::path_of(m));
    r.set("mods", list).set("weaponInstall", weapon_equip_mod != nullptr).set("armorInstall", armor_equip_mod != nullptr);
    return r;
}

json give_open() {  // test bridge: the transfer screen on the stand-in, without the key and the aim
    std::string why;
    bool ok = false;
    try { ok = open_give(why); } catch (const std::exception& e) { why = e.what(), drop_give_box(); }
    json r = json::object();
    r.set("open", ok).set("why", why).set("widget", ok ? ue::addr(g.transfer_widget) : std::string());
    return r;
}

bool close_screens() {
    if (!g.transfer_widget) return false;
    close_transfer();
    return true;
}
namespace {
constexpr int kXpOffset = 0x93C;  // UPlayerInfoComponent's XP (int32), from AddXP's code in build CL-2994920
constexpr uint16_t kSharedXp = 1 << 0 | 1 << 4 | 1 << 5 | 1 << 6 | 1 << 10 | 1 << 11 | 1 << 12, kQuestXp = 1 << 1 | 1 << 2;

ue::UObject* player_info() {
    std::string err;
    return ue::resolve("pawn.PlayerInfoComponent", &err);
}

std::string xp_reason(uint8_t r) {
    static const char* n[] = {"enemy killed", "quest", "quest objective", "discovery", "hack", "lock pick", "conversation",
                              "collectible", "debug", "travel destination", "interaction", "automech", "creature tamed", "bits"};
    return r < 14 ? n[r] : "?";
}

void apply_xp_inbox() {
    if (g.xp_inbox.empty() || !add_xp_original) return;
    if (!g.host && !join::joined()) { g.xp_inbox.clear(); return; }  // the friend's own campaign: not the co-op character
    ue::UObject* pic = player_info();
    if (!pic) return;
    for (const auto& x : g.xp_inbox) {
        alignas(16) uint8_t details[16] = {};  // the game's own callers pass an empty TArray here
        bool gained = add_xp_original(pic, x.first, x.second, details);  // the original: no echo back
        void* data;
        memcpy(&data, details, sizeof data);
        if (data) g.xp_details_filled++;  // shortcut: entries a listener adds are not freed (none seen in tests); free them if this counts up
        if (gained) g.xp_applied++;
        g.last_xp = std::string(gained ? "got " : "at max level, no ") + std::to_string(x.first) + " XP (" + xp_reason(x.second) + ") from the other player";
    }
    g.xp_inbox.clear();
}
}  // namespace

bool xp_blocked(void* self, uint8_t reason) {
    try {
        if (reason > 13 || !((kQuestXp >> reason) & 1) || !g.on || g.host || !g.remote_id || !join::joined() || self != player_info()) return false;
        g.xp_blocked++;
        g.last_xp = "own " + xp_reason(reason) + " XP dropped: the host's comes instead";
        return true;
    } catch (const std::exception&) { return false; }  // called from game code: nothing may escape
}

void on_xp(void* self, int32_t xp, uint8_t reason) {
    try {
        if (reason > 13 || !(((kSharedXp | (g.host ? kQuestXp : 0)) >> reason) & 1) || !g.on || !g.remote_id || xp <= 0) return;
        if ((!g.host && !join::joined()) || self != player_info()) return;  // the friend's own campaign is not shared
        queue_interact(4, reason, xp, "");
        g.xp_sent++;
        g.last_xp = "sent " + std::to_string(xp) + " XP (" + xp_reason(reason) + ")";
    } catch (const std::exception&) {}
}

json xp_test(int xp, int reason) {  // test bridge: an XP gain through the game's AddXP, as gameplay makes one
    ue::UObject* pic = player_info();
    if (!pic || !add_xp_entry) throw std::runtime_error(!pic ? "no player" : "AddXP is not hooked");
    alignas(16) uint8_t details[16] = {};
    json r = json::object();
    r.set("gained", add_xp_entry(pic, xp, static_cast<uint8_t>(reason), details));
    return r;
}

// Self-test of the Steam link without another player (bridge coop.steamProbe, any thread): the functions resolve, the
// relay network comes up, and a message addressed to yourself comes back on channel 9.
json steam_probe() {
    std::string why;
    json r = json::object();
    bool ok = steamnet::start(why);
    r.set("available", ok).set("why", why);
    if (!ok) return r;
    r.set("app", static_cast<double>(steamnet::app_id())).set("friendsInGame", static_cast<double>(steamnet::friends_in_game().size()));
    int relay = 0;
    for (int i = 0; i < 100 && (relay = steamnet::relay_status()) != 100 && relay > -100; i++) Sleep(100);  // up to 10 s
    r.set("relay", static_cast<double>(relay));
    const char ping[] = "tow2coop self-test";
    r.set("selfSend", static_cast<double>(steamnet::send(steamnet::me(), ping, sizeof ping, true, 9)));
    bool back = false;
    for (int i = 0; i < 50 && !back; i++) {  // up to 5 s
        steamnet::receive(9, [&](uint64_t from, const char* d, int n) {
            back = back || (from == steamnet::me() && n == static_cast<int>(sizeof ping) && !memcmp(d, ping, sizeof ping));
        });
        if (!back) Sleep(100);
    }
    r.set("selfReceived", back);
    return r;
}

// UDP: the host listens on the port; a client knows the host's address (TOW2_COOP_PEER).
bool start_udp(int port, std::string& why) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    g.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(g.s, FIONBIO, &nb);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons(g.host ? static_cast<u_short>(port) : 0);
    std::string bind_ip = env("TOW2_COOP_BIND");  // default: all interfaces for a host, loopback is enough for tests
    inet_pton(AF_INET, bind_ip.empty() ? "0.0.0.0" : bind_ip.c_str(), &me.sin_addr);
    if (bind(g.s, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) { why = "bind failed " + std::to_string(WSAGetLastError()); return false; }
    if (!g.host) {
        std::string peer = env("TOW2_COOP_PEER");
        if (peer.empty()) peer = "127.0.0.1:" + std::to_string(port);
        size_t c = peer.rfind(':');
        if (peer.empty() || c == std::string::npos) { why = "client needs TOW2_COOP_PEER=ip:port"; return false; }
        g.peer.sin_family = AF_INET;
        g.peer.sin_port = htons(static_cast<u_short>(atoi(peer.c_str() + c + 1)));
        if (inet_pton(AF_INET, peer.substr(0, c).c_str(), &g.peer.sin_addr) != 1) { why = "bad peer address " + peer; return false; }
        g.have_peer = true;
    }
    return true;
}

bool start(std::string& why) {
    // TOW2_COOP_* wins; else the hub's gamepool pair variables (up --pairs N) give role and port.
    std::string role = env("TOW2_COOP_ROLE");
    if (role.empty()) role = env("GAMEPOOL_ROLE");
    if (role != "host" && role != "client") { why = "no role (TOW2_COOP_ROLE or GAMEPOOL_ROLE): co-op off"; return false; }
    g.host = role == "host";
    std::string key = env("TOW2_COOP_KEY");  // optional shared secret; both players set the same
    g.magic = key.empty() ? kMagic : kMagic ^ fnv(key) ^ 0x9E3779B9u;
    int port = atoi(env("TOW2_COOP_PORT").c_str());
    if (!port) port = atoi(env("GAMEPOOL_PORT").c_str());
    if (!port) port = 27960;
    g.port = port;
    g.steam = env("TOW2_COOP_TRANSPORT") == "steam";  // no socket, no open port: the peer is found through Steam (frame)
    if (!g.steam && !start_udp(port, why)) return false;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g.me = static_cast<uint32_t>(GetCurrentProcessId() * 2654435761u ^ q.LowPart) | 1;
    g.on = true;
    g.start_time = clock_s();
    if (g.steam) why = g.host ? "Steam; host: Steam friends running the mod as client join" : "Steam; client: joins a Steam friend hosting";
    else why = std::string(g.host ? "host on port " : "client, host port ") + std::to_string(g.host ? port : ntohs(g.peer.sin_port));
    why = std::string("tow2coop ") + kVersion + ", " + why;
    return true;
}

void frame(float) {
    if (!g.on) return;
    g.now = clock_s();
    // A hand-over load tears the old world down over the next frames: touching its objects then faulted every frame
    // (runs 42, 44, 50) and an aborted game call mid-load may leave it half done. Stay away until the new world exists.
    if (join::loading()) return;
    ue::UObject* pawn = local_pawn();
    std::string err;
    ue::UObject* world = ue::resolve("world", &err);
    uint32_t map = world ? fnv(ue::path_of(world)) : 0;
    if (map != g.my_map || reinterpret_cast<uintptr_t>(world) != g.world_ptr) {  // a load or reload happened
        g.my_map = map;
        g.world_ptr = reinterpret_cast<uintptr_t>(world);
        if (!g.host) story::client_reset();  // the client's world now holds a save's state: replay the story log
        g.settle_until = g.now + 10.0;
        g.arrived_by_travel = false;
        g.puppet = nullptr;  // it belonged to the old world
        g.npcs.clear(), g.scanning.clear(), g.scan_cursor = 0;
        g.interactables.clear(), g.inter_scan.clear();  // old world's doors and containers
        g.box_pos.clear(), g.box_counts.clear();
        doors.clear(), door_cursor = 0;
        g.at_menu = world && ue::path_of(world).find("MainMenu") != std::string::npos;
        if (g.at_menu) join::set_joined(false), join::hold(false), g.died = false, g.campaign_wanted = false;  // back at the menu: the player's own games save again
        bool same_save = g.now - g.reload_handed_at < 60.0 && join::status().get("state").asString() != "failed";
        if (g.host && !same_save) g.push_pending = true, g.push_ready_at = 0;  // the client follows the host's new map (or reload)
        else if (g.host) core_log("reload: the other player loads the same save, no hand-over");
        g.map_since = g.now;
        core_log("world: " + (world ? ue::name_of(world) : std::string("none")));  // timings of loads and travels
        g.map_changes++;
    }
    // Only a gameplay world counts (the main menu has its own pawn class); nothing touches the world before that.
    bool gameplay = pawn && ue::class_name(pawn) == "IndianaPlayerCharacter_BP_C";
    if (gameplay && g.now < g.settle_until) {  // after a travel, the game's own loading screen ends it: settled 1 s later
        ue::UObject* tm = travel_manager();       // (probe 82: ~7 s after the world appears, instead of a fixed 10 s)
        bool travelling = tm && travel_destination(tm);
        if (travelling) g.arrived_by_travel = true;
        else if (g.arrived_by_travel) g.settle_until = std::min(g.settle_until, g.now + 1.0), g.arrived_by_travel = false;
    }
    bool settled = gameplay && g.now >= g.settle_until;
    g.in_gameplay = settled;
    receive();  // after the world check: data for the old world's objects is ignored until the new one settles
    watch_travel();
    if (gameplay) follow_travel(pawn);
    send_travels();
    send_gifts();
    bool give_pressed = give_key.exchange(false);
    // The host's map changed: hand the campaign over as soon as it can save, not after the map settles and the client
    // reports another map (two-PC test: 15 s, then 7 s of transfer). Not while the client follows a travel by itself.
    bool followed = g.host && g.follow_until > g.now;
    if (followed && g.push_pending && g.puppet_map == g.my_map && !g.snaps.empty() && g.now - g.snaps.back().t < 2.0) {
        g.follow_until = 0, g.push_pending = false, followed = false;  // (only once the host is in its new map: run 84)
        core_log("travel: the other player arrived by itself, no hand-over");
    }
    if (g.host && g.push_pending && gameplay && !g.push_ready_at) g.push_ready_at = g.now + 2.0;  // placed at its saved spot
    if (g.host && g.push_pending && !followed && g.have_peer && gameplay && g.push_ready_at && g.now >= g.push_ready_at && g.now >= g.next_push_try) {
        g.next_push_try = g.now + 0.5;
        request_resync("the host changed map", true);
    }
    if (g.steam) steam_discovery();
    if (g.now >= g.next_send && pawn && g.have_peer && settled) {
        g.next_send = g.now + 1.0 / 30;
        State st{};
        double pos[3], vel[3];
        float yaw = 0, pitch = 0;
        if (read_transform(pawn, pos, yaw, pitch, vel)) {
            fill_header(st.h, kState, sizeof st);
            st.px = pos[0], st.py = pos[1], st.pz = pos[2];
            st.yaw = yaw, st.pitch = pitch;
            st.vx = static_cast<float>(vel[0]), st.vy = static_cast<float>(vel[1]), st.vz = static_cast<float>(vel[2]);
            st.map_hash = g.my_map;
            if (g.now >= g.next_dead_check) {
                g.next_dead_check = g.now + 0.5;
                g.me_dead = character::dead();
            }
            st.flags = g.me_dead || g.opt_fake_dead ? 1 : 0;
            send_raw(&st, sizeof st);
        }
    }
    if (g.last_recv_t >= 0 && g.now - g.last_recv_t > 5.0) {  // peer silent: drop it, wait for a (re)join
        g.snaps.clear();
        destroy_puppet();
        g.last_recv_t = -1;
        g.remote_id = 0, g.remote_last_seq = 0;  // a restarted peer has a new id and starts its sequence again
        if (g.host) g.have_peer = false;         // the next client to send becomes the peer (Steam: to show up again)
        if (g.steam) core_log("steam: no data from " + steamnet::name(g.peer_id) + " for 5 s" + (g.host ? ", looking for a client again" : ""));
        g.drops++;
    }
    if (pawn && settled && !g.host && g.saves_blocked_world != g.world_ptr) {  // again once settled: the game may lift it
        g.saves_blocked_world = g.world_ptr;
        join::block_saves(join::joined());  // only a joined game: the player's own campaign saves normally
        if (join::joined()) core_log("saves are off in this joined game: the host keeps the campaign, your own saves stay untouched");
    }
    if (!g.host) character::frame(g.now, pawn && settled && gameplay && join::joined());
    if (!g.host && gameplay && join::joined() && g.now >= g.next_life_check) watch_life(pawn);
    if (!g.host && g.campaign_wanted && g.now >= g.next_campaign_ask && g.now >= g.fetch_quiet_until) {
        uint32_t reason = g.died ? 4 : 3;  // asked again every 5 s: the host can't save in a fight
        send_typed(kResyncRequest, &reason, 4);
        g.next_campaign_ask = g.now + 5.0;
    }
    if (!g.host && pawn && settled && join::joined() && g.snapped_world != g.world_ptr && g.puppet && !g.snaps.empty() &&
        g.now - g.snaps.back().t < 1.0 && g.puppet_map == g.my_map) {
        g.snapped_world = g.world_ptr;
        double pos[3], vel[3];
        float yaw = 0, pitch = 0;
        const Snapshot& h = g.snaps.back();
        if (read_transform(pawn, pos, yaw, pitch, vel) && std::hypot(pos[0] - h.px, pos[1] - h.py) > 1000.0) {
            json a = json::object(), to = json::object();
            to.set("X", h.px + 150.0 * std::cos(h.yaw * 3.14159265 / 180 + 1.5708)).set("Y", h.py + 150.0 * std::sin(h.yaw * 3.14159265 / 180 + 1.5708)).set("Z", h.pz + 50.0);
            a.set("NewLocation", to).set("bSweep", false).set("bTeleport", true);
            call_quiet(ue::addr(pawn), "K2_SetActorLocation", a);
            core_log("joined " + std::to_string(static_cast<int>(std::hypot(pos[0] - h.px, pos[1] - h.py) / 100)) + " m from the host: moved next to it");
        }
    }
    if (pawn && settled) {
        watch_keys();
        if (give_pressed) try_give(pawn);
        watch_gifts();
        apply_gifts();
        setup_watches();
        process_observed();
        watch_boxes(pawn);
        if (g.opt_doors) poll_doors();
        apply_xp_inbox();
        send_outgoing();
        if (g.have_peer && g.now >= g.next_look) {
            g.next_look = g.now + 2.0;
            send_look();
        }
        if (g.have_peer && g.now >= g.next_face) {
            g.next_face = g.now + 5.0;
            try { send_face(); } catch (const std::exception& e) { g.last_error = std::string("send face: ") + e.what(); }
        }
        drive_puppet();
        if (g.opt_scan) scan_step(pawn);
        if (g.host && g.opt_story) story::host_frame(g.now, send_typed);  // polls even without a peer: the log covers the whole session
        if (g.host && g.have_peer && g.now >= g.next_clock) {
            g.next_clock = g.now + 2.0;
            g.host_paused = game_paused();
            send_clock();
        }
        if (g.host && g.opt_npcs && g.have_peer && g.now >= g.next_npc_send) {  // a menu pauses the host's NPCs, not
            g.next_npc_send = g.now + 0.1;                                       // the client's: no corrections then
            bool paused = game_paused();                                         // (looked at each time: run 96)
            if (paused != g.host_paused) core_log(paused ? "the host's game is paused (a menu, a conversation)" : "the host's game runs again");
            g.host_paused = paused;
            if (!g.host_paused) host_send_npcs(pawn);
        }
        if (!g.host) keep_clock();
        if (!g.host && g.opt_combat && g.have_peer && g.now >= g.next_combat_send) {
            g.next_combat_send = g.now + 0.2;
            client_send_combat(pawn);
        }
        if (!g.host) {
            client_report_damage();
            resend_damage();
            story::client_frame(g.now, send_typed);
            if (story::take_resync_request()) {
                uint32_t reason = 1;
                send_typed(kResyncRequest, &reason, 4);
            }
        }
        if (g.host && g.have_peer) {  // the client is in another map (the host travelled): hand the campaign over
            bool other = g.puppet_map != 0 && g.puppet_map != g.my_map && !g.snaps.empty() && g.now - g.snaps.back().t < 2.0 && g.follow_until <= g.now;
            if (!other) g.mismatch_since = -1;
            else if (g.mismatch_since < 0) g.mismatch_since = g.now;
            else if (g.now - g.mismatch_since > 5.0) request_resync("the client is in another map");
        }
    }
    // Automatic join: a client at the main menu (front end up for a minute: an earlier load crashes the engine) asks
    // the host for its campaign every 5 s until a hand-over starts.
    if (!g.host && g.at_menu && g.now - g.map_since > 30.0 && g.now - g.start_time > 60.0 && g.now >= g.next_join_request &&
        g.now >= g.fetch_quiet_until + 90.0) {  // never from a loading screen: the client may be travelling
        uint32_t reason = 2;
        send_typed(kResyncRequest, &reason, 4);
        g.next_join_request = g.now + 5.0;
        g.join_requests++;
    }
    if (g.host && g.resync_sends > 0 && g.now >= g.next_resync_send) {  // tell the client (repeated: UDP)
        uint32_t body[4] = {static_cast<uint32_t>(g.port + 1), g.me, g.resync_kind, static_cast<uint32_t>(g.resyncs)};
        send_typed(kResync, body, 16);
        g.resync_sends--;
        g.next_resync_send = g.now + 1.0;
    }
}

void observe(void* o, void* f, const void* p) {
    if (applying_remote || !p) return;  // our own apply (game thread): no echo
    Observed e{};
    e.obj = o, e.fn = f;
    uint16_t size = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(f) + 0xB6);  // UFunction::ParmsSize
    memcpy(e.parms, p, std::min<size_t>(size, sizeof e.parms));
    std::lock_guard<std::mutex> lk(obs_mu);
    if (observed.size() < 4096) observed.push_back(e);
}

json set_options(const json& p) {
    for (auto& kv : {std::make_pair("story", &g.opt_story), std::make_pair("doors", &g.opt_doors), std::make_pair("npcs", &g.opt_npcs), std::make_pair("scan", &g.opt_scan),
                     std::make_pair("follow", &g.opt_follow), std::make_pair("gifts", &g.opt_gifts), std::make_pair("giveHold", &g.opt_give_hold), std::make_pair("combat", &g.opt_combat), std::make_pair("fakeDead", &g.opt_fake_dead)})
        if (p.get(kv.first).isBool()) *kv.second = p.get(kv.first).asBool(true);
    json s = json::object();
    s.set("story", g.opt_story).set("doors", g.opt_doors).set("npcs", g.opt_npcs).set("scan", g.opt_scan).set("follow", g.opt_follow).set("gifts", g.opt_gifts).set("combat", g.opt_combat);
    return s;
}

// Test bridge, game thread: the NPCs fighting in this world, or pinned, with the deciding world's word.
json fights() {
    json out = json::array();
    ue::UObject* pawn = local_pawn();
    double me[3] = {}, v[3];
    float yaw, pitch;
    bool have_me = pawn && read_transform(pawn, me, yaw, pitch, v);
    static Fn loc;
    base_fn(loc, "/Script/Engine.Actor", "K2_GetActorLocation");
    for (auto& kv : g.npcs) {
        Npc& n = kv.second;
        if (!n.ai || !npc_live(n)) continue;
        uint8_t here = fight_of(fighting(n), pawn);
        if (here == kNobody && n.target == kNobody && !n.has_handle) continue;
        std::string path = ue::path_of(n.actor);
        json f = json::object();
        f.set("npc", path.substr(path.rfind('.') + 1)).set("owner", n.client_owned ? "client" : "host");
        f.set("word", static_cast<double>(n.target)).set("here", static_cast<double>(here)).set("pinned", n.has_handle);
        if (n.target == kSomeoneElse) f.set("otherHere", other_actor(n.other) != nullptr);
        if (have_me && loc.f) {
            alignas(16) uint8_t b[64] = {};
            ue::process_event(n.actor, loc.f, b);
            double p[3];
            memcpy(p, b + loc.off("ReturnValue"), 24);
            f.set("dist", dist3(p, me) / 100.0);
        }
        out.push(f);
    }
    return out;
}

json status() {
    json s = json::object();
    s.set("on", g.on).set("host", g.host).set("me", static_cast<double>(g.me)).set("remote", static_cast<double>(g.remote_id));
    s.set("transport", g.steam ? "steam" : "udp");
    if (g.steam)
        s.set("steamPeer", g.peer_id ? steamnet::name(g.peer_id) : std::string()).set("session", static_cast<double>(steamnet::session_state(g.peer_id)))
            .set("relay", static_cast<double>(steamnet::relay_status())).set("steamUp", steamnet::on());
    s.set("sent", static_cast<double>(g.sent)).set("recv", static_cast<double>(g.recv)).set("stale", static_cast<double>(g.stale)).set("bad", static_cast<double>(g.bad));
    s.set("peer", g.have_peer).set("puppet", g.puppet ? ue::addr(g.puppet) : std::string("none")).set("error", g.last_error);
    s.set("sameMap", g.puppet_map == g.my_map);
    s.set("faceIn", static_cast<double>(g.face_in.size())).set("faceApplied", !g.face_in.empty() && g.face_in == g.face_applied);
    s.set("remoteArmor", g.look[0]).set("remoteHelmet", g.look[1]).set("remoteHelmetShown", g.look_helmet_shown).set("lookApplied", g.look_applied);
    s.set("story", story::status());
    if (!g.host) s.set("character", character::status());
    s.set("doorsSent", static_cast<double>(g.doors_sent)).set("doorsApplied", static_cast<double>(g.doors_applied));
    s.set("itemsSent", static_cast<double>(g.items_sent)).set("itemsApplied", static_cast<double>(g.items_applied)).set("lastInteract", g.last_interact);
    if (ue::UObject* pic = player_info()) s.set("xp", static_cast<double>(*reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(pic) + kXpOffset)));
    s.set("xpHook", add_xp_original != nullptr).set("xpSent", static_cast<double>(g.xp_sent)).set("xpApplied", static_cast<double>(g.xp_applied));
    s.set("xpBlocked", static_cast<double>(g.xp_blocked)).set("xpPending", static_cast<double>(g.xp_inbox.size())).set("xpDetails", static_cast<double>(g.xp_details_filled)).set("lastXp", g.last_xp);
    s.set("resyncs", static_cast<double>(g.resyncs)).set("resyncRequests", static_cast<double>(g.resync_requests)).set("resyncFetches", static_cast<double>(g.resync_fetches)).set("resyncReason", g.resync_reason);
    s.set("travelsSent", static_cast<double>(g.travels_sent)).set("travelsFollowed", static_cast<double>(g.travels_followed)).set("travelsRefused", static_cast<double>(g.travels_refused));
    s.set("lastTravel", g.last_travel).set("followWait", std::max(0.0, g.follow_until - g.now));
    s.set("giftsSent", static_cast<double>(g.gifts_sent)).set("giftsReceived", static_cast<double>(g.gifts_received)).set("giftsReturned", static_cast<double>(g.gifts_returned));
    s.set("giftsPending", static_cast<double>(g.gifts_out.size())).set("lastGift", g.last_gift).set("transferOpen", g.transfer_widget != nullptr);
    s.set("giveReady", open_transfer_fn != nullptr && game_proc != nullptr).set("giveBox", g.give_box ? ue::addr(g.give_box) : std::string());
    s.set("died", g.died).set("deaths", static_cast<double>(g.deaths)).set("campaignWanted", g.campaign_wanted).set("loadsRedirected", static_cast<double>(g.loads_redirected));
    s.set("reloadsHanded", static_cast<double>(g.reloads_handed)).set("joinHeld", join::held());
    s.set("worldAge", g.now - g.map_since);  // seconds since this core saw its world change (timings, test bridge)
    s.set("clockJumps", static_cast<double>(g.clock_jumps)).set("clockHolds", static_cast<double>(g.clock_holds)).set("clockHeld", g.clock_held).set("hostPaused", g.host_paused);
    s.set("npcs", static_cast<double>(g.npcs.size())).set("npcSent", static_cast<double>(g.npc_sent)).set("npcApplied", static_cast<double>(g.npc_applied));
    s.set("npcKilled", static_cast<double>(g.npc_killed)).set("npcSnapped", static_cast<double>(g.npc_snapped)).set("npcEased", static_cast<double>(g.npc_eased)).set("npcFightSnapped", static_cast<double>(g.npc_fight_snapped));
    s.set("combatPins", static_cast<double>(g.combat_pins)).set("combatDetections", static_cast<double>(g.combat_detections));
    s.set("combatHintsSent", static_cast<double>(g.combat_hints_sent)).set("combatHintsIn", static_cast<double>(g.combat_hints_in));
    s.set("combatRecsIn", static_cast<double>(g.combat_recs_in)).set("combatHandoffs", static_cast<double>(g.combat_handoffs));
    s.set("combatFights", static_cast<double>(g.combat_fights)).set("combatOwned", static_cast<double>(g.combat_owned));
    s.set("partyFight", g.now < g.party_fight_until).set("standinFighting", g.standin_fighting).set("standinTeamChanges", static_cast<double>(g.standin_team_changes));
    s.set("meDead", g.me_dead).set("peerDead", g.peer_dead).set("standinSpell", g.standin_spell);
    if (g.host) s.set("npcsSentAgo", g.now - g.npcs_sent_at);
    if (g.puppet && standin_ok()) {  // the stand-in's untargetable effect, as the game sees it
        static ue::UClass* effect = ue::find_class("/Script/Arkansas.UntargetableByAIStatusEffect");
        std::string err;
        ue::UObject* sem = ue::resolve(ue::addr(g.puppet) + ".StatusEffectManagerComponent", &err);
        ue::UFunction* has = sem ? ue::find_func(ue::class_of(sem), "BPHasStatusEffect") : nullptr;
        if (effect && has) {
            json a = json::object();
            a.set("StatusEffect", ue::addr(effect));
            try { s.set("standinUntargetable", ue::call(sem, has, a).get("ReturnValue").asBool(false)); } catch (const std::exception&) {}
        }
    }
    s.set("dmgSent", static_cast<double>(g.dmg_sent)).set("dmgApplied", static_cast<double>(g.dmg_applied)).set("drops", static_cast<double>(g.drops)).set("dmgResent", static_cast<double>(g.dmg_resent)).set("dmgPending", static_cast<double>(g.dmg_pending.size())).set("joinRequests", static_cast<double>(g.join_requests)).set("atMenu", g.at_menu).set("mapChanges", static_cast<double>(g.map_changes)).set("settled", g.now >= g.settle_until).set("brainStops", static_cast<double>(g.brain_stops));
    if (!g.snaps.empty()) {
        const Snapshot& b = g.snaps.back();
        json r = json::array();
        r.push(b.px).push(b.py).push(b.pz);
        s.set("remotePos", r).set("remoteAgeMs", (g.now - b.t) * 1000);
    }
    return s;
}
}  // namespace coop
