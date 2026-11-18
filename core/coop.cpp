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
    uint8_t kind;   // 1 door state, 2 item taken, 3 item stored, 4 XP earned
    uint64_t id;    // fnv64 of the actor path (level-placed actors have the same path in both worlds); XP: EXPGainReason
    int32_t a;      // door: EOCLState; items: count; XP: amount before the receiver's own multipliers
    uint16_t len;   // item class path length (kinds 2, 3)
    char path[256];
};
#pragma pack(pop)
struct Outgoing {
    InteractMsg m;
    int size;
    double at[3];
    int sent;
};
constexpr uint32_t kMagic = 0x32433254;  // "T2C2": the last character is the protocol version, bump it on any packet change
constexpr const char* kVersion = "0.2.0-beta.6";
constexpr uint16_t kState = 1, kHello = 2, kBye = 3, kEnemies = 4, kDamage = 5, kResync = 8, kResyncRequest = 9, kInteract = 10, kDamageAck = 11, kLook = 12, kAppearance = 13;
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
};
// Host -> client: authoritative NPC state. Ids are a 64-bit hash of the actor's path, which is the same in every
// world for level-placed (World Partition, "_UAID_") actors.
struct EnemyRec {
    uint64_t id;
    float px, py, pz, yaw, health;
    uint8_t dead;
};
constexpr int kMaxEnemyRecs = 48;
struct Enemies {
    Header h;
    uint32_t map_hash;
    uint16_t count;
    EnemyRec rec[kMaxEnemyRecs];
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
#pragma pack(pop)

struct Npc {
    ue::UObject *actor = nullptr, *health = nullptr;
    float host_health = -1;  // client: last value applied from the host; -1 = none yet
    float pending_damage = 0;
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
    uint64_t npc_sent = 0, npc_applied = 0, npc_killed = 0, npc_snapped = 0, dmg_sent = 0, dmg_applied = 0, drops = 0, map_changes = 0;
    double settle_until = 0, next_tame = 0;
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
    bool opt_story = true, opt_doors = true, opt_npcs = true, opt_scan = true;  // coop.set
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

ue::UObject* local_pawn() {
    std::string err;
    return ue::resolve("pawn", &err);
}

bool read_transform(ue::UObject* pawn, double pos[3], float& yaw, float& pitch, double vel[3]) {
    static Fn loc, rot, vel_fn;
    if (!loc.init(pawn, "K2_GetActorLocation") || !rot.init(pawn, "K2_GetActorRotation") || !vel_fn.init(pawn, "GetVelocity")) return false;
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
    if (!get.init(n.health, "GetHealth")) return -1;
    alignas(16) uint8_t b[32] = {};
    ue::process_event(n.health, get.f, b);
    float v;
    memcpy(&v, b + get.off("ReturnValue"), 4);
    return v;
}

void npc_set_health(Npc& n, float hp) {
    static Fn set;
    if (!set.init(n.health, "SetHealth")) return;
    alignas(16) uint8_t b[64] = {};
    memcpy(b + set.off("Health"), &hp, 4);
    ue::process_event(n.health, set.f, b);
}

void npc_kill(Npc& n) {
    static Fn die;
    if (!die.init(n.health, "Suicide")) return;
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
        if (path.find("_UAID_") == std::string::npos) continue;  // only level-placed actors have the same name everywhere
        ue::UObject* hc = ue::resolve(ue::addr(o) + ".HealthComponent");
        if (!hc) continue;
        uint64_t id = fnv64(path);
        Npc n;
        n.actor = o, n.health = hc;
        auto old = g.npcs.find(id);
        if (old != g.npcs.end() && old->second.actor == o) n.host_health = old->second.host_health, n.pending_damage = old->second.pending_damage;
        g.scanning[id] = n;
    }
    g.scan_cursor = end;
    if (end >= total) {
        g.npcs.swap(g.scanning);
        g.scanning.clear();
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

void host_send_npcs(ue::UObject* pawn) {
    Enemies e{};
    e.map_hash = g.my_map;
    static Fn loc;
    for (auto& kv : g.npcs) {
        Npc& n = kv.second;
        if (!ue::alive(n.actor) || !ue::alive(n.health) || !loc.init(n.actor, "K2_GetActorLocation")) continue;
        alignas(16) uint8_t b[64] = {};
        ue::process_event(n.actor, loc.f, b);
        double p[3];
        memcpy(p, b + loc.off("ReturnValue"), 24);
        if (!near_players(p, pawn)) continue;
        EnemyRec& r = e.rec[e.count];
        r.id = kv.first, r.px = static_cast<float>(p[0]), r.py = static_cast<float>(p[1]), r.pz = static_cast<float>(p[2]);
        r.health = npc_health(n);
        r.dead = r.health <= 0;
        if (++e.count == kMaxEnemyRecs) break;  // shortcut: nearest-first ordering and several packets when more fight at once
    }
    uint16_t size = static_cast<uint16_t>(offsetof(Enemies, rec) + e.count * sizeof(EnemyRec));
    fill_header(e.h, kEnemies, size);
    send_raw(&e, size);
    g.npc_sent++;
}

void apply_npcs(const Enemies& e) {
    if (e.map_hash != g.my_map) return;
    static Fn loc, set;
    for (int i = 0; i < e.count; i++) {
        const EnemyRec& r = e.rec[i];
        auto it = g.npcs.find(r.id);
        if (it == g.npcs.end() || !ue::alive(it->second.actor) || !ue::alive(it->second.health)) continue;
        Npc& n = it->second;
        float local = npc_health(n);
        if (n.host_health >= 0 && local < n.host_health - 0.5f && local < r.health - 0.5f)
            n.pending_damage += n.host_health - local;  // our player hit it since the last update: tell the host
        if (r.dead && local > 0) { npc_kill(n); g.npc_killed++; }
        else if (!r.dead && std::fabs(local - r.health) > 0.5f) npc_set_health(n, r.health);
        n.host_health = r.health;
        g.npc_applied++;
        if (!r.dead && loc.init(n.actor, "K2_GetActorLocation") && set.init(n.actor, "K2_SetActorLocationAndRotation")) {
            alignas(16) uint8_t b[512] = {};
            ue::process_event(n.actor, loc.f, b);
            double p[3];
            memcpy(p, b + loc.off("ReturnValue"), 24);
            double dx = p[0] - r.px, dy = p[1] - r.py, dz = p[2] - r.pz;
            if (dx * dx + dy * dy + dz * dz > 300.0 * 300.0) {  // drifted more than 3 m: snap to the host's position
                memset(b, 0, sizeof b);
                double np[3] = {r.px, r.py, r.pz};
                memcpy(b + set.off("NewLocation"), np, 24);
                b[set.off("bTeleport")] = 1;
                ue::process_event(n.actor, set.f, b);
                g.npc_snapped++;
            }
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
    if (it == g.npcs.end() || !ue::alive(it->second.actor) || !ue::alive(it->second.health) || d.amount <= 0 || d.amount > 1e6f) return;
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

void request_resync(const char* reason, bool host_moved = false) {
    if (!g.host || !g.have_peer || (!host_moved && g.now < g.resync_cooldown)) return;
    try {
        char peer_ip[64] = "";
        inet_ntop(AF_INET, &g.peer.sin_addr, peer_ip, sizeof peer_ip);
        // the campaign goes to the joined player only: its address (TCP), or its SteamID (pushed over Steam)
        join::host_offer(g.port + 1, bind_ip(), peer_ip, g.steam ? g.peer_id : 0, g.me);
    } catch (const std::exception& e) {
        g.last_error = std::string("resync offer: ") + e.what();
        static std::string logged;  // once per reason: the client asks again every few seconds
        if (logged != e.what()) core_log(std::string("hand-over waits: ") + (logged = e.what()));
        return;
    }
    g.resync_cooldown = g.now + 60.0;  // one hand-over at a time; the client needs ~30-60 s to reload
    g.push_pending = false;
    g.resync_sends = 6, g.next_resync_send = g.now + 2.0;
    g.resyncs++;
    g.resync_reason = reason;
}

void client_fetch_resync(const char* body, int len) {
    if (g.host || len != 8) return;
    uint32_t port, host_id;
    memcpy(&port, body, 4);
    memcpy(&host_id, body + 4, 4);
    char ip[64] = "";
    inet_ntop(AF_INET, &g.peer.sin_addr, ip, sizeof ip);
    if (g.now < g.fetch_quiet_until) return;  // the host repeats its notice; one hand-over per resync
    try {
        if (g.steam) join::client_expect(host_id);  // the host pushes the files on Steam channel 1
        else join::client_fetch(ip, static_cast<int>(port), host_id);
        g.resync_fetches++;
        g.fetch_quiet_until = g.now + 30.0;
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
        } else if (fname == "HandleItemRemoved" || fname == "ItemRemoved") {
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
std::unordered_map<ue::UClass*, int32_t> box_items(ue::UObject* box) {
    std::unordered_map<ue::UClass*, int32_t> out;
    std::string err;
    ue::UObject* inv = ue::resolve(ue::addr(box) + ".InventoryComponent", &err);
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
        if (data[i].item && data[i].count > 0 && ue::alive(data[i].item)) out[ue::class_of(data[i].item)] += data[i].count;
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
        if (!ue::alive(box) || !ue::is_a(box, g.container_class)) continue;
        auto at = g.box_pos.find(kv.first);
        if (at == g.box_pos.end()) {
            if (budget-- <= 0 || !loc.init(box, "K2_GetActorLocation")) continue;
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
        if (it != g.npcs.end() && ue::alive(it->second.actor)) actor = it->second.actor;
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
        request_resync(reason == 2 ? "client joins from the main menu" : "client could not apply a quest outcome");
        return;
    }
    if (h.type == story::kStory && !g.host) {
        g.recv++;
        story::on_story(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)), send_typed);
        return;
    }
    if (h.type == story::kStoryAck && g.host) {
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
    if (!g.puppet || !ue::alive(g.puppet)) return;
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
}

void destroy_puppet() {
    if (g.puppet && ue::alive(g.puppet)) {
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
    if (!g.puppet || !ue::alive(g.puppet)) {
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
    if (!set.init(g.puppet, "K2_SetActorLocationAndRotation")) return;
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
        g.puppet = nullptr;  // it belonged to the old world
        g.npcs.clear(), g.scanning.clear(), g.scan_cursor = 0;
        g.interactables.clear(), g.inter_scan.clear();  // old world's doors and containers
        g.box_pos.clear(), g.box_counts.clear();
        doors.clear(), door_cursor = 0;
        g.at_menu = world && ue::path_of(world).find("MainMenu") != std::string::npos;
        if (g.at_menu) join::set_joined(false);  // back at the menu: the player's own games save again
        if (g.host) g.push_pending = true, g.push_ready_at = 0;  // the client follows the host's new map (or its reload)
        g.map_since = g.now;
        g.map_changes++;
    }
    // Only a gameplay world counts (the main menu has its own pawn class); nothing touches the world before that.
    bool gameplay = pawn && ue::class_name(pawn) == "IndianaPlayerCharacter_BP_C";
    bool settled = gameplay && g.now >= g.settle_until;
    g.in_gameplay = settled;
    receive();  // after the world check: data for the old world's objects is ignored until the new one settles
    // The host's map changed: hand the campaign over as soon as it can save, not after the map settles and the client
    // reports another map (two-PC test: 15 s, then 7 s of transfer).
    if (g.host && g.push_pending && gameplay && !g.push_ready_at) g.push_ready_at = g.now + 2.0;  // placed at its saved spot
    if (g.host && g.push_pending && g.have_peer && gameplay && g.push_ready_at && g.now >= g.push_ready_at && g.now >= g.next_push_try) {
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
        if (g.host && g.opt_npcs && g.have_peer && g.now >= g.next_npc_send) {
            g.next_npc_send = g.now + 0.1;
            host_send_npcs(pawn);
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
            bool other = g.puppet_map != 0 && g.puppet_map != g.my_map && !g.snaps.empty() && g.now - g.snaps.back().t < 2.0;
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
        uint32_t body[2] = {static_cast<uint32_t>(g.port + 1), g.me};
        send_typed(kResync, body, 8);
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
    for (auto& kv : {std::make_pair("story", &g.opt_story), std::make_pair("doors", &g.opt_doors), std::make_pair("npcs", &g.opt_npcs), std::make_pair("scan", &g.opt_scan)})
        if (p.get(kv.first).isBool()) *kv.second = p.get(kv.first).asBool(true);
    json s = json::object();
    s.set("story", g.opt_story).set("doors", g.opt_doors).set("npcs", g.opt_npcs).set("scan", g.opt_scan);
    return s;
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
    s.set("npcs", static_cast<double>(g.npcs.size())).set("npcSent", static_cast<double>(g.npc_sent)).set("npcApplied", static_cast<double>(g.npc_applied));
    s.set("npcKilled", static_cast<double>(g.npc_killed)).set("npcSnapped", static_cast<double>(g.npc_snapped));
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
