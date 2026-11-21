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
#include <map>
#include <set>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <array>
#include <mutex>

void core_log(const std::string& line);  // main.cpp: a line in tow2coop.log (what a player sends when something fails)

namespace coop {
using json = coopbridge::json::Value;
std::atomic<int> watch_count{0};
void* watch[8];
// The local player's weapon events, counted from the calls of its weapon's visuals (TriggerEventBP, probe 92).
std::atomic<void*> my_weapon{nullptr};
std::atomic<uint32_t> ev_frame{1};
std::atomic<uint8_t> my_shots{0}, my_swings{0}, my_swing_kind{0}, my_reloads{0};
std::atomic<bool> my_aiming{false};
uint32_t trigger_event_name = 0;
uint32_t actor_destroyed_name = 0;  // SaveGameManager.OnActorDestroyed: the save notes an actor gone (a loose item taken)
uint32_t ev_seen[128];
uint32_t ev_log[32];            // test bridge: the last weapon events heard (frame << 8 | event), before de-duplication
std::atomic<uint32_t> ev_log_n{0};
// The co-op menu (main menu): its digit keys and clicks, from the window procedure.
std::atomic<int> menu_key{0}, menu_own{0};  // a digit pressed; ours: that digit (level 0) or -1 (all: our choices shown)
std::atomic<uint64_t> menu_click{0}, menu_client{0};  // a left click: client x << 32 | y; the client area's size
std::atomic<bool> menu_clicked{false}, menu_listen{false};
std::atomic<uint32_t> menu_held{0};      // digits whose key-down the menu took: their character and release too
extern bool damage_hooked;  // the game's damage function is hooked (below): the stand-in may fire for real
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
std::vector<uint64_t> picked_up;  // path ids of level-placed loose items taken here (obs_mu)
volatile bool applying_remote = false;  // set while we apply a remote change: its events must not echo back
#pragma pack(push, 1)
struct InteractMsg {
    uint32_t eid;   // sender-unique event id (dedupe of the 3 copies)
    uint8_t kind;   // 1 door state, 2 item taken, 3 item stored, 4 XP earned, 5 item given, 6 gift received (id: its eid),
                    // 7 quest XP, 8 a loose item picked up, 9 a quest item's count (path), 10 quest items asked for,
                    // 11 an interaction state (a switch, a radio)
    uint64_t id;    // fnv64 of the actor path (level-placed actors have the same path in both worlds); XP: EXPGainReason;
                    // gift: item level | rarity << 32
    int32_t a;      // door: EOCLState; items: count; XP: amount before the receiver's own multipliers
    uint16_t len;   // item class path length (kinds 2, 3, 5; a gift: "<item class>|<mod class>|...")
    char path[1024];
};
#pragma pack(pop)
struct Outgoing {  // sent until the other game acknowledges it (kInteractAck), 120 s at most
    InteractMsg m;
    int size;
    double next, until;
    int sent;
};
constexpr uint32_t kMagic = 0x37433254;  // "T2C7": the last character is the protocol version, bump it on any packet change
constexpr const char* kVersion = "0.5.4";
constexpr uint16_t kState = 1, kHello = 2, kBye = 3, kEnemies = 4, kDamage = 5, kResync = 8, kResyncRequest = 9, kInteract = 10, kDamageAck = 11, kLook = 12, kAppearance = 13, kTravel = 14, kClock = 15, kCombat = 16, kSpawns = 17, kSpawnOffers = 18, kWeaponMods = 19, kInteractAck = 20, kConversation = 21, kParty = 23, kWaves = 24;  // 22: 0.4.0's response pick
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
    uint8_t flags;  // bit 0: this player is dying or dead (its stand-in leaves the fights); 1: weapon drawn; 2: aiming; 3: crouched
    uint8_t shots, swings, swing_kind, reloads;  // wrapping counts of the player's weapon events: its stand-in plays them
    float ax, ay, az;                            // where the player's last shot ended: the stand-in's shot goes there
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
    char weapon[256];  // the class of the weapon in hand ("" = none)
};
struct FacePart { uint32_t version; uint8_t part, parts; uint16_t len; char text[1100]; };  // one piece of a player's look
struct TravelMsg {
    uint32_t id;     // the travel's sender-unique id
    uint8_t ack;     // 0 a travel starts (path: the destination data asset), 1 the receiver follows it, 2 it does not (path: why)
    uint16_t len;
    char path[256];
};
struct ConvMsg {             // the conversation its sender leads (shared conversations)
    uint8_t open;            // 1: it leads one; 0: it ended (sent a few times)
    uint8_t asset_len, owner_len, pad;
    uint32_t conv;           // the sender's conversation number
    int32_t node, prev;      // its node now, and the one before
    int32_t group;           // the bank its line was drawn from (-1: none)
    uint32_t picks;          // the responses picked in it so far
    int32_t recent[4];       // the last ones, newest first
    float pos[3], yaw;       // where the sender stands
    uint64_t owner_id;       // other_id() of whom it talks to (0: by its path)
    char text[512];          // the asset's path, then the owner's path
};
#pragma pack(pop)
constexpr int kConvHead = static_cast<int>(offsetof(ConvMsg, text));

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

struct Spawned {  // an NPC the game spawned during play, shared by its class and place
    std::string cls;
    float p[3] = {}, yaw = 0;
    double first = 0, seen = 0;               // first heard of / last announced (the other world's grace, its end)
    ue::UObject* actor = nullptr;             // this world's actor (identity: its class)
    ue::UClass* actor_cls = nullptr;
    bool copy = false;                        // spawned by the mod (a client's copy goes when the host's is gone)
    uint64_t offer = 0;                       // host: the client's offer it answers
};
struct SpawnOffer { std::string cls; float p[3] = {}, yaw = 0; double first = 0, seen = 0; };

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
    uint64_t npc_sent = 0, npc_applied = 0, npc_killed = 0, npc_snapped = 0, dmg_sent = 0, dmg_applied = 0, dmg_not_mine = 0, drops = 0, map_changes = 0;
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
    // The other player's weapon on the stand-in: what arrived, what the stand-in did with it (gestures)
    std::string look_weapon, standin_weapon_path, my_weapon_path;
    ue::UObject* standin_weapon = nullptr;  // the weapon given to the stand-in (identity: its class path)
    bool standin_drawn = true, standin_aiming = false, trigger_down = false, gestures_synced = false, my_drawn = false;
    uint8_t peer_flags = 0, peer_shots = 0, peer_swings = 0, peer_swing_kind = 0, peer_reloads = 0;
    uint8_t done_shots = 0, done_swings = 0, done_reloads = 0, sent_shots = 0;
    float peer_pitch = 0, peer_aim[3] = {}, my_aim[3] = {};
    double peer_shot_at = -10, next_orient = 0, next_weapon_try = 0, shot_wait_since = -1, aimed_at = -10;
    uint64_t standin_shots = 0, standin_swings = 0, standin_reloads = 0, standin_dmg_blocked = 0, weapon_changes = 0;
    std::string look_mods_for, standin_mods_applied;  // the other player's weapon (path) its mods below belong to; applied
    std::vector<std::string> look_weapon_mods;
    bool standin_crouched = false, my_crouched = false;
    double crouch_tick_until = 0;                     // the stand-in's movement component ticks until then (its crouch)
    double crouch_since = 0, next_crouch_ask = 0, next_crouch_retry = 0;
    float standin_half = 0, standin_half_crouched = 0;  // its capsule's half height standing (read at spawn), crouched
    uint64_t standin_mods_installed = 0, standin_crouches = 0, crouch_misses = 0, standin_respawns = 0;
    double standin_friction = -1, standin_braking = -1;  // its movement component's own, kept while a crouch zeroes them
    int crouch_miss_streak = 0;
    // Enemies the game spawned during play, shared (see "enemies the game spawns during play")
    std::unordered_map<uint64_t, Spawned> spawned;        // by shared id (the host's)
    std::unordered_map<ue::UObject*, uint64_t> spawn_ids; // client: this world's actor -> shared id (the scan registers it)
    std::unordered_map<ue::UObject*, double> native_first; // client: when this world's own spawned one was first seen
    std::vector<ue::UObject*> natives, natives_scan;      // this world's spawned NPCs (not placed, not companions or the stand-in)
    std::unordered_map<uint64_t, SpawnOffer> offers;      // host: the client's own ones, by its key
    double next_spawn_send = 0;
    // The co-op menu: the player picks host or join in the main menu (role "auto")
    bool menu_mode = false, role_chosen = true, start_host = false;
    uint64_t join_target = 0, menu_picks = 0;  // join_target: the Steam friend picked in the menu
    bool host_public = false, join_public = false;  // a public game (a Steam lobby): anyone may join / we join a stranger
    bool host_new = false;   // host: a new game together (the friend makes a character first)
    bool new_char = false;   // client: our own New Game makes our character, then we join
    int new_char_stage = 0;  // 0 making it, 1 keeping it (a private save), 2 asking for the host's game
    double new_char_since = 0;
    uint64_t pending_join = 0;                       // a host to join from Steam ("Join game", an invite, the command line)
    bool public_wanted = false;                      // a public game picked in the menu: joined once its lobby is entered
    uint64_t spawn_copies = 0, spawn_matched = 0, spawn_removed = 0;
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
    bool opt_no_pause = true, opt_party = true, opt_conv_follow = true;
    uint64_t pauses_lifted = 0;
    // shared conversations: the one the other player leads (open: they lead one now)
    ConvMsg peer_conv{};
    double peer_conv_at = -100;
    int32_t conv_lock_test = -1;  // test: a response the follower treats as locked (coop.set convLockTest)
    int pad_test = -1;            // test: the controller's buttons instead of XInput's (coop.set pad)
    uint16_t pad_now = 0, pad_prev = 0;
    bool pad_seen = false;        // a controller (or the test's) was used: the menu shows its buttons
    std::unordered_map<uint64_t, ue::UObject*> encounters;  // this world's encounters, by path hash
    std::map<std::pair<uint64_t, uint8_t>, uint8_t> wave_state;   // each wave's state last seen here (0: Inactive)
    std::map<std::pair<uint64_t, uint8_t>, double> wave_starts;   // waves that started here, when (sent for 5 s)
    std::map<std::pair<uint64_t, uint8_t>, double> waves_done;    // the other world's starts taken here, when
    double next_waves = 0, next_encounters = 0;
    uint64_t waves_started = 0;
    uint64_t conv_states_sent = 0, conv_leads = 0, conv_follows = 0, conv_picks_sent = 0, conv_picks_applied = 0, conv_jumps = 0, conv_advances = 0, conv_bypassed = 0;
    uint64_t dmg_resent = 0, join_requests = 0;
    uint32_t next_eid = 1;
    std::vector<Outgoing> outgoing;
    std::unordered_set<uint32_t> seen_eids;
    std::deque<uint32_t> seen_order;
    double opt_drop_interact = 0;  // test: this share of door/item/XP packets and their acknowledgements is not sent
    uint64_t interacts_resent = 0, interacts_lost = 0, interact_acks = 0;
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
    std::vector<std::pair<int32_t, double>> quest_xp_held;  // host: the friend's quest XP (kind 7), waiting for a forced completion
    std::vector<double> quest_credits;                      // host: quests completed here by force, waiting for that XP
    uint64_t forced_seen = 0, quest_xp_credited = 0;
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
    ue::UClass* pickup_class = nullptr;            // loose items in the world (/Script/Arkansas.Pickup)
    ue::UClass* icomp_class = nullptr;             // InteractableComponent: a switch's or a radio's interaction state
    std::unordered_map<uint64_t, ue::UObject*> states, states_scan;  // those components by path id (a scan pass)
    std::vector<uint64_t> state_ids;               // polled in turn
    size_t state_cursor = 0;
    std::unordered_map<uint64_t, int32_t> state_last, state_remote;  // last polled here; the other world's last word
    std::unordered_map<uint64_t, int32_t> state_check;  // a use replayed here: the state it must end in (next poll)
    uint64_t states_sent = 0, states_applied = 0;
    std::unordered_set<uint64_t> pickups_gone;     // level-placed loose items taken in either world: none here
    uint64_t pickups_sent = 0, pickups_applied = 0;
    std::map<ue::UClass*, int32_t> qitems;          // our player's quest items as last seen, for gains
    ue::UObject* qitems_pawn = nullptr;
    double next_qitems = 0;
    uint64_t qitems_sent = 0, qitems_given = 0;
    std::vector<std::string> qitems_dropped;      // quest items our game took away by itself (a quest step), newest last
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
    struct PartySnap { double t; float p[3], yaw, v[3]; };
    std::unordered_map<uint64_t, std::pair<PartySnap, PartySnap>> party_in;  // client: the host's companions (previous, latest)
    std::unordered_set<ue::UObject*> party_tamed;                            // client: companions driven from the host's
    bool party_calm_in = false;
    double party_in_at = 0, next_party_send = 0;
    uint64_t party_tames = 0, party_releases = 0;
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

// A loose item the other player picked up: destroyed here too, as the game does once it is taken (its save notes the
// actor destroyed, probe 132: CompleteInteract, the item added, SaveGameManager.OnActorDestroyed).
void remove_pickup(ue::UObject* o) {
    static Fn d;
    if (!base_fn(d, "/Script/Engine.Actor", "K2_DestroyActor")) return;
    alignas(16) uint8_t b[16] = {};
    bool was = applying_remote;
    applying_remote = true;  // not told back
    ue::process_event(o, d.f, b);
    applying_remote = was;
    g.pickups_applied++;
}

bool is_companion(ue::UObject* o);  // co-op combat, below

// One slice of the object array per frame; a full pass replaces the registry.
void scan_step(ue::UObject* pawn) {
    if (!g.character_class) g.character_class = ue::find_class("/Script/Engine.Character");
    if (!g.door_class) g.door_class = ue::find_class("/Script/Arkansas.Door");
    if (!g.container_class) g.container_class = ue::find_class("/Script/Arkansas.Container");
    if (!g.corpse_class) g.corpse_class = ue::find_class("/Script/Arkansas.Corpse");
    if (!g.pickup_class) g.pickup_class = ue::find_class("/Script/Arkansas.Pickup");
    if (!g.icomp_class) g.icomp_class = ue::find_class("/Script/Arkansas.InteractableComponent");
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
        if (g.pickup_class && ue::is_a(o, g.pickup_class)) {  // a loose item: level-placed ones have the same path in both
            std::string path = ue::alive(o) ? ue::path_of(o) : std::string();
            if (path.find("_UAID_") == std::string::npos) continue;  // dropped or spawned during play: this world's own
            uint64_t id = fnv64(path);
            if (g.pickups_gone.count(id)) remove_pickup(o);  // taken in the other world (maybe before it streamed in here)
            else g.inter_scan[id] = o;
            continue;
        }
        if (g.icomp_class && ue::is_a(o, g.icomp_class)) {  // a level-placed switch or radio (probe 139): its state
            ue::UObject* owner = ue::outer_of(o);
            if (!owner || !ue::alive(o) || !ue::alive(owner) || ue::is_a(owner, g.character_class) ||
                (g.door_class && ue::is_a(owner, g.door_class)) || (g.container_class && ue::is_a(owner, g.container_class)) ||
                (g.corpse_class && ue::is_a(owner, g.corpse_class)) || (g.pickup_class && ue::is_a(owner, g.pickup_class)))
                continue;  // NPCs talk; doors, containers and loose items have their own sharing
            std::string path = ue::path_of(o);
            if (path.find("_UAID_") != std::string::npos) g.states_scan[fnv64(path)] = o;
            continue;
        }
        if (!ue::is_a(o, g.character_class) || !ue::alive(o)) continue;
        std::string path = ue::path_of(o);
        static ue::UClass* ai_class = ue::find_class("/Script/Arkansas.IndianaAiCharacter");
        uint64_t id = 0;
        if (path.find("_UAID_") == std::string::npos) {  // only level-placed actors have the same name everywhere
            if (is_companion(o)) { g.party_scan[fnv64("party:" + ue::class_name(o))] = o; continue; }  // companions, by class
            if (!ai_class || !ue::is_a(o, ai_class)) continue;
            g.natives_scan.push_back(o);  // spawned during play: shared by class and place, under the host's id
            if (g.host) id = fnv64("spawn:" + path);
            else if (auto s = g.spawn_ids.find(o); s != g.spawn_ids.end()) id = s->second;
            if (!id) continue;
        } else {
            id = fnv64(path);
        }
        ue::UObject* hc = ue::resolve(ue::addr(o) + ".HealthComponent");
        if (!hc) continue;
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
        g.states.swap(g.states_scan), g.states_scan.clear(), g.state_ids.clear(), g.state_cursor = 0;
        for (auto& kv : g.states) g.state_ids.push_back(kv.first);
        g.party.swap(g.party_scan);
        g.party_scan.clear();
        g.natives.swap(g.natives_scan);
        g.natives_scan.clear();
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

bool hit_by(Npc& n, ue::UObject* pawn) {  // the NPC's last hit came from that pawn (HealthComponent's last instigator)
    static Fn last;
    if (!pawn || !base_fn(last, "/Script/Arkansas.HealthComponent", "GetLastHitInstigator")) return true;  // unknown: as before
    alignas(16) uint8_t b[32] = {};
    ue::process_event(n.health, last.f, b);
    ue::UObject* who = nullptr;
    memcpy(&who, b + last.off("ReturnValue"), 8);
    if (!who || who == pawn) return true;  // none recorded: counted, as before
    std::string err;
    if (who == ue::resolve(ue::addr(pawn) + ".Controller", &err)) return true;
    static ue::UClass* actor_k = ue::find_class("/Script/Engine.Actor");
    if (!actor_k || !ue::alive(who) || !ue::is_a(who, actor_k)) return false;
    for (const char* f : {".Owner", ".Instigator"})  // a projectile or a thrown thing of ours
        if (ue::resolve(ue::addr(who) + f, &err) == pawn) return true;
    return false;
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
        if (n.host_health >= 0 && local < n.host_health - 0.5f && local < r.health - 0.5f) {
            // Our player hit it since the last update: tell the host. A companion's or another NPC's hit is not ours:
            // the host's world has its own (run 118: 106 such reports in 20 s of a fight the client stayed out of).
            if (hit_by(n, pawn)) n.pending_damage += n.host_health - local;
            else g.dmg_not_mine++;
        }
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
    uint32_t t = ue::make_name("TriggerEventBP", false).index;  // a weapon's visuals hear its events: fire, swing, reload
    uint32_t x = ue::make_name("OnActorDestroyed", false).index;  // SaveGameManager: a loose item taken (probe 132)
    if (!b || !c || !d) return;
    int k = 0;
    for (uint32_t w : {b, c, d, t, x})
        if (w) watch[k++] = reinterpret_cast<void*>(static_cast<uintptr_t>(w));
    trigger_event_name = t, actor_destroyed_name = x;
    watch_count.store(k, std::memory_order_release);
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
    o.next = g.now, o.until = g.now + 120.0, o.sent = 0;
    if (kind == 1)  // a door's newer state replaces one still unacknowledged: an older one arriving last would win
        g.outgoing.erase(std::remove_if(g.outgoing.begin(), g.outgoing.end(), [&](const Outgoing& x) { return x.m.kind == 1 && x.m.id == id; }),
                         g.outgoing.end());
    if (g.outgoing.size() >= 4000) g.outgoing.erase(g.outgoing.begin()), g.interacts_lost++;  // the other game is gone
    g.outgoing.push_back(o);
}

// Game thread: turn observed events into messages.
void process_observed() {
    std::vector<Observed> evs;
    std::vector<uint64_t> picked;
    {
        std::lock_guard<std::mutex> lk(obs_mu);
        evs.swap(observed), picked.swap(picked_up);
    }
    for (uint64_t id : picked) {  // loose items our player took: gone in the other world too (not in our own list: a
        if (!g.have_peer || (!g.host && !join::joined())) break;  // reload brings them back here)
        queue_interact(8, id, 0, "");
        g.pickups_sent++;
        g.last_interact = "sent a loose item picked up";
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

bool test_drop() { return g.opt_drop_interact > 0 && (rand() % 1000) < g.opt_drop_interact * 1000; }

// Door, item and XP events: resent until the other game acknowledges them (0.2 s apart at first, then 1 s, then 3 s).
void send_outgoing() {
    for (size_t i = 0; i < g.outgoing.size();) {
        Outgoing& o = g.outgoing[i];
        if (g.now >= o.until) {
            if (g.interacts_lost++ % 50 == 0) core_log("interact: an event was not acknowledged in 120 s (kind " + std::to_string(o.m.kind) + ")");
            g.outgoing.erase(g.outgoing.begin() + i);
            continue;
        }
        if (g.now >= o.next) {
            if (!test_drop()) send_typed(kInteract, &o.m, o.size);
            if (o.sent > 0) g.interacts_resent++;
            o.sent++;
            o.next = g.now + (o.sent < 5 ? 0.2 : o.sent < 15 ? 1.0 : 3.0);
        }
        i++;
    }
}

void ack_interact(uint32_t eid) {  // taken (applied, or kept for later): the other game stops sending it
    if (test_drop()) return;
    uint8_t b[5];
    b[0] = 1;
    memcpy(b + 1, &eid, 4);
    send_typed(kInteractAck, b, sizeof b);
}

void on_interact_ack(const char* body, int len) {
    if (len < 1 || len != 1 + 4 * static_cast<uint8_t>(body[0])) { g.bad++; return; }
    for (int k = 0; k < static_cast<uint8_t>(body[0]); k++) {
        uint32_t eid;
        memcpy(&eid, body + 1 + 4 * k, 4);
        for (size_t i = 0; i < g.outgoing.size(); i++)
            if (g.outgoing[i].m.eid == eid) { g.outgoing.erase(g.outgoing.begin() + i); g.interact_acks++; break; }
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

// ---------------------------------------------------------------- switches and radios
// An interactable's interaction state (a switch on or off, a radio playing: probe 139, the game's own use changed it in
// that world only) is shared like a door's: polled in turn, a change sent, set in the other world the game's way
// (SetInteractionState, its own handlers run). One the other world changed before it streamed in here is set on sight.
int32_t interact_state(ue::UObject* comp) {
    static Fn f;
    if (!f.init(comp, "GetInteractState")) return -1;
    alignas(16) uint8_t b[16] = {};
    ue::process_event(comp, f.f, b);
    int32_t v;
    memcpy(&v, b + f.off("ReturnValue"), 4);
    return v;
}

// bForceExecute: the state is set, then the interaction runs from it (a two-state switch: on to the other state).
void set_interact_state(ue::UObject* comp, int32_t st, bool run) {
    static Fn f;
    if (!f.init(comp, "SetInteractionState")) return;
    alignas(16) uint8_t b[16] = {};
    memcpy(b + f.off("StateIn"), &st, 4);
    b[f.off("bForceExecute")] = run ? 1 : 0;
    bool was = applying_remote;
    applying_remote = true;
    ue::process_event(comp, f.f, b);
    applying_remote = was;
    g.states_applied++;
}

void poll_states() {
    for (int k = 0; k < 16 && !g.state_ids.empty(); k++) {
        uint64_t id = g.state_ids[g.state_cursor];
        g.state_cursor = (g.state_cursor + 1) % g.state_ids.size();
        auto it = g.states.find(id);
        if (it == g.states.end() || !ue::alive(it->second)) continue;
        int32_t st = interact_state(it->second);
        if (st < 0) continue;  // no interaction states (a ladder: -1)
        auto last = g.state_last.find(id);
        auto remote = g.state_remote.find(id);
        auto check = g.state_check.find(id);
        if (check != g.state_check.end()) {  // a replayed use that ended elsewhere: the other world's state, plainly
            if (st != check->second) set_interact_state(it->second, st = check->second, false);
            g.state_check.erase(check);
        } else if (last == g.state_last.end()) {  // first seen here: the other world's word, if it had one
            if (remote != g.state_remote.end() && remote->second != st) set_interact_state(it->second, st = remote->second, false);
        } else if (st != last->second && (remote == g.state_remote.end() || remote->second != st)) {
            queue_interact(11, id, st | last->second << 8, "");  // to | from << 8
            g.state_remote[id] = st;  // both worlds' word now
            g.states_sent++;
            g.last_interact = "sent " + ue::name_of(ue::outer_of(it->second)) + " -> state " + std::to_string(st);
        }
        g.state_last[id] = st;
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

bool first_time(uint32_t eid) {  // an event may arrive again (resent until acknowledged): applied once
    if (!g.seen_eids.insert(eid).second) return false;
    g.seen_order.push_back(eid);
    if (g.seen_order.size() > 16384) g.seen_eids.erase(g.seen_order.front()), g.seen_order.pop_front();
    return true;
}

// XP the other player earned (kind 4). Taken in any world state: it waits in the inbox for a player (it often comes
// with a map change, while this world loads).
bool take_xp(const char* body, int len) {
    InteractMsg m{};
    if (len != static_cast<int>(offsetof(InteractMsg, path))) return false;
    memcpy(&m, body, len);
    if (m.kind != 4 && m.kind != 7) return false;
    g.recv++;
    if (first_time(m.eid) && m.a > 0 && m.a <= 1000000 && m.id <= 13) {  // trust boundary
        if (m.kind == 4) g.xp_inbox.push_back({m.a, static_cast<uint8_t>(m.id)});
        else if (g.host) g.quest_xp_held.push_back({m.a, g.now});
    }
    ack_interact(m.eid);
    return true;
}

void take_quest_item(const InteractMsg& m);  // quest items to both, below
void send_quest_items();

void apply_interact(const InteractMsg& m) {
    if (!first_time(m.eid)) return;
    if (m.kind == 9) return take_quest_item(m);
    if (m.kind == 10) { if (g.host) send_quest_items(); return; }  // the client's new world: our quest items, the missing given
    ue::UObject* actor = interactable(m.id);
    if (m.kind == 11) {  // a switch or a radio the other player used: the same state here, now or when it streams in
        int32_t to = m.a & 0xFF, from = m.a >> 8;
        if (m.a < 0 || to > 64 || from > 64) return;  // trust boundary: interaction state indexes
        g.state_remote[m.id] = to;
        auto it = g.states.find(m.id);
        if (it == g.states.end() || !ue::alive(it->second)) return;  // set when it streams in
        int32_t cur = interact_state(it->second);
        if (cur == to) return;
        if (cur == from) set_interact_state(it->second, from, true), g.state_check[m.id] = to;  // the same use: its handlers
        else set_interact_state(it->second, to, false);
        g.state_last[m.id] = to;
        return;
    }
    if (m.kind == 8) {  // a loose item the other player picked up: gone here now, or when it streams in
        g.pickups_gone.insert(m.id);
        if (actor && g.pickup_class && ue::is_a(actor, g.pickup_class)) remove_pickup(actor);
        return;
    }
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
std::atomic<bool> window_input{false};  // the player clicked or typed in the game's window (keep_drawn_for_saves)
WNDPROC game_proc = nullptr;
HWND game_hwnd = nullptr;

LRESULT CALLBACK key_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_KEYDOWN && w == 'E' && !(l & (1 << 30))) give_key = true;  // a new press (bit 30: a repeat)
    if (msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || (msg == WM_ACTIVATE && LOWORD(w) == WA_CLICKACTIVE)) window_input = true;
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR) {  // the co-op menu's digits (main menu)
        int d = msg == WM_CHAR ? (w >= '1' && w <= '9' ? static_cast<int>(w - '0') : 0)
              : w >= '1' && w <= '9' ? static_cast<int>(w - '0') : w >= VK_NUMPAD1 && w <= VK_NUMPAD9 ? static_cast<int>(w - VK_NUMPAD0) : 0;
        int own = menu_own.load(std::memory_order_relaxed);
        uint32_t bit = d ? 1u << d : 0;
        if (d && msg == WM_KEYDOWN && menu_listen.load(std::memory_order_relaxed) && (own < 0 || d == own)) {
            if (!(l & (1 << 30))) menu_key = d;
            menu_held |= bit;
            return 0;  // ours: the game's (hidden) choices never see it
        }
        if (d && (menu_held.load() & bit)) {  // its character and its release too (the game picks a choice on release)
            if (msg == WM_KEYUP) menu_held &= ~bit;
            return 0;
        }
    }
    if (menu_listen.load(std::memory_order_relaxed)) {  // the co-op menu is shown: a click is checked against its choices
        if (msg == WM_LBUTTONUP) {
            int x = static_cast<short>(LOWORD(l)), y = static_cast<short>(HIWORD(l));  // client pixels (physical)
            RECT rc{};
            GetClientRect(h, &rc);  // the viewport renders at its own size into it (150 % display: 960 x 540 into 1440 x 810)
            menu_click = static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(y);
            menu_client = static_cast<uint64_t>(static_cast<uint32_t>(rc.right)) << 32 | static_cast<uint32_t>(rc.bottom);
            menu_clicked = true;
        }
    }
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
void give_to_player(ue::UClass* k, int32_t n, int32_t level, int32_t rarity, const std::vector<ue::UClass*>& mods = {}, bool notify = true) {
    ue::UObject* pawn = local_pawn();
    std::string err;
    ue::UObject* inv = pawn ? ue::resolve(ue::addr(pawn) + ".InventoryComponent", &err) : nullptr;
    ue::UFunction* f = inv ? ue::find_func(ue::class_of(inv), "GiveItemToPlayer") : nullptr;
    if (!f) throw std::runtime_error("no player inventory");
    std::vector<ue::UObject*> before;
    for (const auto& s : stacks_of(pawn)) if (ue::class_of(s.first) == k) before.push_back(s.first);
    json a = json::object();
    a.set("ItemType", ue::addr(k)).set("Count", n).set("bShowNotification", notify).set("bPlayItemPickupAudio", notify);
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

// ---------------------------------------------------------------- quest items to both
// A quest item either player gets (from the ground, a container, a gift) is given to the other player too: a quest's
// later step may ask either of them for it. The other game is told the new count and gives what its player lacks, so
// an item both games got (an NPC's gift in a shared conversation: each game runs it) is not given twice. After a load
// or a travel the client asks for the host's (a joined player's own character comes without the campaign's keys).
std::map<ue::UClass*, int32_t> quest_items_of(ue::UObject* pawn) {
    static ue::UClass* qk = nullptr;
    if (!qk) qk = ue::find_class("/Script/Arkansas.QuestItem");
    std::map<ue::UClass*, int32_t> out;
    if (!qk || !pawn) return out;
    for (const auto& st : stacks_of(pawn))
        if (ue::is_a(st.first, qk)) out[ue::class_of(st.first)] += st.second;
    return out;
}

void send_quest_items() {  // all of ours (asked for by the client): given quietly where missing
    for (const auto& kv : quest_items_of(local_pawn())) queue_interact(9, 1, kv.second, ue::path_of(kv.first)), g.qitems_sent++;
}

void take_quest_item(const InteractMsg& m) {
    // From the other player (trust boundary): a /Game/ quest item class, a sane count.
    std::string cls(m.path, m.len);
    static ue::UClass* qk = nullptr;
    if (!qk) qk = ue::find_class("/Script/Arkansas.QuestItem");
    ue::UClass* k = cls.rfind("/Game/", 0) == 0 && m.a > 0 && m.a <= 1000 ? load_class(cls) : nullptr;
    if (!k || !qk || !ue::cdo(k) || !ue::is_a(ue::cdo(k), qk)) { core_log("quest items: ignored one that is no quest item: " + cls); return; }
    ue::UObject* pawn = local_pawn();
    auto have = quest_items_of(pawn);
    int32_t mine = have.count(k) ? have[k] : 0;
    if (mine >= m.a) return;
    try {
        give_to_player(k, m.a - mine, 0, -1, {}, m.id == 0);  // a new one: the game's notification; the host's set: quietly
    } catch (const std::exception& e) {
        core_log(std::string("quest items: could not give ") + ue::name_of(k) + ": " + e.what());
        return;
    }
    g.qitems[k] = m.a;  // not told back as a gain of ours
    g.qitems_given++;
    if (m.id == 0) core_log("quest items: " + ue::name_of(k) + " from the other player's game");
}

void quest_items_frame(ue::UObject* pawn) {
    if (!g.have_peer || (!g.host && !join::joined()) || g.now < g.next_qitems) return;
    if (!g.host && character::stripping()) return;  // a first join empties our inventory: the host's set after it (run 189)
    g.next_qitems = g.now + 1.0;
    auto now = quest_items_of(pawn);
    if (pawn != g.qitems_pawn) {  // a new world or a load: gains count from here; the client asks for the host's set
        g.qitems_pawn = pawn, g.qitems = now;
        if (!g.host) queue_interact(10, 0, 0, "");
        return;
    }
    for (const auto& kv : g.qitems) {  // gone here, not given away: the game's own quest step (probe 145: the client's story
        auto it = now.find(kv.first);       // catch-up dropped two of the host's leftovers), kept as it is
        if (it != now.end() && it->second >= kv.second) continue;
        std::string n = ue::name_of(kv.first);
        if (std::find(g.qitems_dropped.begin(), g.qitems_dropped.end(), n) == g.qitems_dropped.end()) g.qitems_dropped.push_back(n);
        if (g.qitems_dropped.size() > 32) g.qitems_dropped.erase(g.qitems_dropped.begin());
        core_log("quest items: " + n + " left this player's inventory (" + std::to_string(kv.second) + " -> " + std::to_string(it == now.end() ? 0 : it->second) + ")");
    }
    for (const auto& kv : now) {
        auto it = g.qitems.find(kv.first);
        if (kv.second <= (it == g.qitems.end() ? 0 : it->second)) continue;
        queue_interact(9, 0, kv.second, ue::path_of(kv.first));
        g.qitems_sent++;
        core_log("quest items: " + ue::name_of(kv.first) + " goes to the other player too");
    }
    g.qitems = std::move(now);
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

// Co-op never pauses (owner, 2026-11-19: "c'est multi"): the game's menus, its conversations and screens pause the
// world with the engine's own pause, the persistent level's WorldSettings.PauserPlayerState (probe 105: the player's
// state while the pause menu or the inventory is open). While the other player is in the game it is lifted at once:
// the menu stays open, the world goes on, as in a multiplayer game.
ue::UObject* active_conversation();  // shared conversations, below

void keep_running() {
    if (active_conversation()) return;  // a pause the game itself sets in a conversation (a cinematic) stays, as in 0.4.0
    std::string err;
    ue::UObject* ws = ue::resolve("world.PersistentLevel.WorldSettings", &err);
    static ue::FProperty* prop = nullptr;
    static ue::UClass* cls = nullptr;
    if (!ws) return;
    if (ue::class_of(ws) != cls) cls = ue::class_of(ws), prop = ue::find_prop(cls, "PauserPlayerState");
    if (!prop) return;
    void** pauser = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ws) + ue::prop_offset(prop));
    if (!*pauser) return;
    *pauser = nullptr;
    if (g.pauses_lifted++ < 5) core_log("co-op: a pause of the game lifted (the other player plays on)");
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
void on_spawns(const char* b, int n);  // enemies the game spawns during play, below
void on_waves(const char* b, int n);   // encounter waves, below
void on_offers(const char* b, int n);

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
    if (h.type == kResync && !g.host) {
        g.last_recv_t = g.now;
        // New game together: the host's game comes once our new character is kept (new_char_frame asks for it then);
        // the host offers it earlier when it sees us in another map (the character creator): not taken (run 152).
        if (!g.new_char || g.new_char_stage >= 2) client_fetch_resync(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)));
        return;
    }
    if (h.type == kConversation) {  // the conversation the other player leads
        int len = n - static_cast<int>(sizeof(Header));
        ConvMsg m{};
        if (len < kConvHead || len > static_cast<int>(sizeof m)) { g.bad++; return; }
        memcpy(&m, buf + sizeof(Header), len);
        bool sane = kConvHead + m.asset_len + m.owner_len == len && std::isfinite(m.pos[0]) && std::isfinite(m.pos[1]) && std::isfinite(m.pos[2]) &&
                    std::isfinite(m.yaw);  // trust boundary
        if (!sane) { g.bad++; return; }
        g.peer_conv = m, g.peer_conv_at = g.now, g.last_recv_t = g.now;
        return;
    }
    if (h.type == kParty && !g.host) {  // the host's companions: calm flag, count, then id, position, yaw, velocity each
        int len = n - static_cast<int>(sizeof(Header));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(buf + sizeof(Header));
        constexpr int kRec = 8 + 7 * 4;
        if (len < 2 || len != 2 + b[1] * kRec || b[1] > 8) { g.bad++; return; }
        g.party_calm_in = b[0] != 0, g.party_in_at = g.now, g.last_recv_t = g.now;
        for (int i = 0; i < b[1]; i++) {
            const uint8_t* r = b + 2 + i * kRec;
            uint64_t id;
            Ctx::PartySnap s{};
            memcpy(&id, r, 8);
            memcpy(s.p, r + 8, 12), memcpy(&s.yaw, r + 20, 4), memcpy(s.v, r + 24, 12);
            s.t = g.now;
            bool sane = std::isfinite(s.p[0]) && std::isfinite(s.p[1]) && std::isfinite(s.p[2]) && std::isfinite(s.yaw);  // trust boundary
            if (!sane) continue;
            auto& e = g.party_in[id];
            e.first = e.second.t > 0 ? e.second : s, e.second = s;
        }
        return;
    }
    if (h.type == kInteractAck) { g.last_recv_t = g.now; on_interact_ack(buf + sizeof(Header), n - static_cast<int>(sizeof(Header))); return; }
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
        ack_interact(m.eid);
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
    if (h.type == kSpawns) { if (!g.host) on_spawns(buf + sizeof(Header), n - static_cast<int>(sizeof(Header))); return; }
    if (h.type == kWeaponMods) {  // the other player's weapon, then its mods, one class path per line
        std::string body(buf + sizeof(Header), n - static_cast<int>(sizeof(Header)));
        std::vector<std::string> lines;
        for (size_t at = 0; at <= body.size() && lines.size() < 16;) {
            size_t e = body.find('\n', at);
            if (e == std::string::npos) e = body.size();
            lines.push_back(body.substr(at, e - at));
            at = e + 1;
        }
        g.look_mods_for = lines.empty() ? std::string() : lines[0];
        g.look_weapon_mods.assign(lines.size() > 1 ? lines.begin() + 1 : lines.end(), lines.end());
        return;
    }
    if (h.type == kWaves) { on_waves(buf + sizeof(Header), n - static_cast<int>(sizeof(Header))); return; }
    if (h.type == kSpawnOffers) { if (g.host) on_offers(buf + sizeof(Header), n - static_cast<int>(sizeof(Header))); return; }
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
        g.look_weapon.assign(l.weapon, strnlen(l.weapon, sizeof l.weapon));
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
    g.peer_flags = st.flags, g.peer_pitch = st.pitch;
    if (!g.gestures_synced) g.done_shots = st.shots, g.done_swings = st.swings, g.done_reloads = st.reloads, g.gestures_synced = true;
    if (st.shots != g.peer_shots) {  // a new shot: aim there now
        g.peer_shot_at = g.now, g.next_orient = 0;
        g.peer_aim[0] = st.ax, g.peer_aim[1] = st.ay, g.peer_aim[2] = st.az;
    }
    g.peer_shots = st.shots, g.peer_swings = st.swings, g.peer_swing_kind = st.swing_kind, g.peer_reloads = st.reloads;
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
    if (g.my_weapon_path.size() < sizeof l.weapon) memcpy(l.weapon, g.my_weapon_path.data(), g.my_weapon_path.size());
    send_typed(kLook, &l, sizeof l);
    // The weapon's mods (a silencer, a scope: they change its look): their class paths, after the weapon's own.
    std::string mods = g.my_weapon_path;
    if (ue::UObject* w = static_cast<ue::UObject*>(my_weapon.load()))
        if (ue::alive(w))
            for (ue::UClass* m : mods_of(w)) {
                std::string p = ue::path_of(m);
                if (mods.size() + p.size() + 1 < 1300) mods += "\n" + p;
            }
    send_typed(kWeaponMods, mods.data(), static_cast<int>(mods.size()));
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
    if (g.now >= g.crouch_tick_until) call_quiet(p + ".CharacterMovement", "SetComponentTickEnabled", no);  // (a crouch: on a moment)
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
    g.standin_weapon = nullptr, g.standin_weapon_path.clear(), g.standin_drawn = true, g.standin_aiming = false, g.trigger_down = false;
    g.standin_mods_applied.clear(), g.standin_crouched = false, g.crouch_tick_until = 0, g.next_crouch_retry = 0;
    g.standin_half = 0, g.standin_half_crouched = 0, g.standin_friction = -1, g.standin_braking = -1, g.crouch_miss_streak = 0;
}

// A deferred spawn of `cls`, kept out of the saves: the flag only counts before the actor finishes spawning (run 32:
// set afterwards, the save still kept the stand-in).
ue::UObject* spawn_actor(ue::UClass* cls, const double p[3], double yaw_deg, const char* collision) {
    std::string err;
    ue::UObject* world_ctx = ue::resolve("pc", &err);
    ue::UObject* gs = ue::cdo(ue::find_class("/Script/Engine.GameplayStatics"));
    json xf = json::object(), tr = json::array(), rot = json::object(), sc = json::array();
    tr.push(p[0]).push(p[1]).push(p[2]);
    double yaw = yaw_deg * 3.14159265358979 / 180.0;
    rot.set("X", 0.0).set("Y", 0.0).set("Z", std::sin(yaw / 2)).set("W", std::cos(yaw / 2));
    sc.push(1.0).push(1.0).push(1.0);
    xf.set("Rotation", rot).set("Translation", tr).set("Scale3D", sc);
    json a = json::object();
    a.set("WorldContextObject", ue::addr(world_ctx)).set("ActorClass", ue::addr(cls)).set("SpawnTransform", xf);
    a.set("CollisionHandlingOverride", collision);
    json r = ue::call(gs, ue::find_func(ue::class_of(gs), "BeginDeferredActorSpawnFromClass"), a);
    std::string ref = r.get("ReturnValue").asString();
    ue::UObject* actor = ref.rfind("0x", 0) == 0 ? reinterpret_cast<ue::UObject*>(static_cast<uintptr_t>(strtoull(ref.c_str(), nullptr, 16))) : nullptr;
    if (!actor || !ue::alive(actor)) { g.last_error = "deferred spawn returned " + ref; return nullptr; }
    ue::set_path(ue::addr(actor) + ".bShouldSaveGameState", json(false));
    json f = json::object();
    f.set("Actor", ue::addr(actor)).set("SpawnTransform", xf);
    ue::call(gs, ue::find_func(ue::class_of(gs), "FinishSpawningActor"), f);
    return actor;
}

bool spawn_puppet(const Snapshot& s) {
    // A body of the player's gender (the stand-in takes the player's appearance, which needs the matching skeleton).
    std::string gerr;
    json gender = json();
    try { gender = ue::eval_path("pawn.AppearanceComponent.AppearanceGender.TagName"); } catch (...) {}
    const char* klass = gender.asString().find("Female") != std::string::npos ? kPuppetClassFemale : kPuppetClass;
    ue::UClass* cls = load_class(klass);
    if (!cls) { g.last_error = "puppet class not loaded"; return false; }
    double at[3] = {s.px, s.py, s.pz};
    ue::UObject* actor = spawn_actor(cls, at, s.yaw, "AlwaysSpawn");  // out of the host's saves: no extra dock worker in them
    if (!actor) return false;
    // The NPC keeps its own AI controller (game systems expect NPCs to have one; a controller-less puppet is a null
    // dereference waiting to happen), but its behaviour is stopped and it can't be talked to, targeted or killed.
    ue::set_path(ue::addr(actor) + ".CharacterMovement.MovementMode", "MOVE_Walking");
    g.puppet = actor;
    g.puppet_cls = ue::class_of(actor);
    g.puppet_in_air = false;
    g.standin_weapon = nullptr, g.standin_weapon_path.clear(), g.standin_drawn = true, g.standin_aiming = false, g.trigger_down = false;
    g.standin_mods_applied.clear(), g.standin_crouched = false, g.crouch_tick_until = 0, g.next_crouch_retry = 0;
    g.standin_half = 0, g.standin_half_crouched = 0, g.standin_friction = -1, g.standin_braking = -1, g.crouch_miss_streak = 0;
    tame_puppet(true);
    return true;
}

// ---------------------------------------------------------------- enemies the game spawns during play
// A level-placed NPC has the same name in both worlds; one the game spawns during play (a hive, a hatch, a door, a
// summon) does not. Each world tells the other about its own hostile ones (class, place); the host gives the shared
// id. The other world takes one of its own of that class within 15 m (both worlds spawned it), else spawns a copy
// after 1.5 s. Then it is in the NPC registry like a placed one: health and death from the host, fights shared.
constexpr float kSpawnMatch = 1500;
constexpr double kSpawnGrace = 1.5, kSpawnGone = 5.0;

struct SpawnRec { uint64_t id; std::string cls; float p[3]; float yaw; };

bool hostile_to(ue::UObject* npc, ue::UObject* pawn) {
    static Fn att;
    static ue::UObject* lib = nullptr;
    if (!lib)
        if (ue::UClass* k = ue::find_class("/Script/Arkansas.CharacterBlueprintFunctionLibrary")) lib = ue::cdo(k);
    if (!pawn || !lib || !base_fn(att, "/Script/Arkansas.CharacterBlueprintFunctionLibrary", "GetTeamAttitudeTowards")) return false;
    alignas(16) uint8_t b[64] = {};
    memcpy(b + att.off("Character"), &npc, 8);
    memcpy(b + att.off("Other"), &pawn, 8);
    ue::process_event(lib, att.f, b);
    return b[att.off("ReturnValue")] == 2;  // ETeamAttitude::Hostile
}

bool actor_place(ue::UObject* a, float p[3], float& yaw) {
    double pos[3], v[3];
    float pitch = 0;
    if (!read_transform(a, pos, yaw, pitch, v)) return false;
    for (int i = 0; i < 3; i++) p[i] = static_cast<float>(pos[i]);
    return true;
}

float dist_f(const float a[3], const float b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void send_spawn_recs(uint16_t type, const std::vector<SpawnRec>& recs) {  // [count] {id, place, yaw, len, class}...
    char body[1400];
    int n = 1, count = 0;
    auto flush = [&]() {
        if (count) body[0] = static_cast<char>(count), send_typed(type, body, n);
        n = 1, count = 0;
    };
    for (const SpawnRec& r : recs) {
        if (r.cls.size() > 200) continue;
        int need = 25 + static_cast<int>(r.cls.size());
        if (n + need > static_cast<int>(sizeof body) || count == 255) flush();
        memcpy(body + n, &r.id, 8);
        memcpy(body + n + 8, r.p, 12);
        memcpy(body + n + 20, &r.yaw, 4);
        body[n + 24] = static_cast<char>(r.cls.size());
        memcpy(body + n + 25, r.cls.data(), r.cls.size());
        n += need, count++;
    }
    flush();
}

std::vector<SpawnRec> parse_spawn_recs(const char* b, int n) {  // from the other player (trust boundary): bounds-checked
    std::vector<SpawnRec> out;
    int count = n >= 1 ? static_cast<uint8_t>(b[0]) : 0, at = 1;
    for (int i = 0; i < count && at + 25 <= n; i++) {
        SpawnRec r;
        memcpy(&r.id, b + at, 8);
        memcpy(r.p, b + at + 8, 12);
        memcpy(&r.yaw, b + at + 20, 4);
        int len = static_cast<uint8_t>(b[at + 24]);
        if (at + 25 + len > n) break;
        r.cls.assign(b + at + 25, len);
        at += 25 + len;
        if (std::isfinite(r.p[0]) && std::isfinite(r.p[1]) && std::isfinite(r.p[2]) && std::isfinite(r.yaw)) out.push_back(r);
    }
    return out;
}

// A copy of an NPC the other world spawned. Its class comes from the other player (trust boundary): a /Game/ NPC class.
ue::UObject* spawn_copy(const std::string& cls_path, const float p[3], float yaw) {
    if (cls_path.rfind("/Game/", 0) != 0 || cls_path.find(' ') != std::string::npos) return nullptr;
    static ue::UClass* ai_k = ue::find_class("/Script/Arkansas.IndianaAiCharacter");
    ue::UClass* cls = load_class(cls_path);
    if (!cls || !ai_k || !ue::cdo(cls) || !ue::is_a(ue::cdo(cls), ai_k)) return nullptr;
    double at[3] = {p[0], p[1], p[2]};
    ue::UObject* a = spawn_actor(cls, at, yaw, "AdjustIfPossibleButAlwaysSpawn");
    if (!a) return nullptr;
    std::string err;
    if (!ue::resolve(ue::addr(a) + ".Controller", &err)) call_quiet(ue::addr(a), "SpawnDefaultController", json::object());  // its AI
    return a;
}

void destroy_actor(ue::UObject* a) {
    try { ue::call(a, ue::find_func(ue::class_of(a), "K2_DestroyActor"), json::object()); } catch (...) {}
}

bool spawned_live(const Spawned& s) { return s.actor && ue::alive(s.actor) && ue::class_of(s.actor) == s.actor_cls; }

void on_spawns(const char* b, int n) {  // client: the host's list
    for (const SpawnRec& r : parse_spawn_recs(b, n)) {
        Spawned& s = g.spawned[r.id];
        if (s.cls.empty()) s.first = g.now;
        if (s.cls != r.cls && s.actor) g.spawn_ids.erase(s.actor), s.actor = nullptr;
        s.cls = r.cls, s.yaw = r.yaw, s.seen = g.now;
        memcpy(s.p, r.p, 12);
    }
}

void on_offers(const char* b, int n) {  // host: the client's own spawned enemies it has no shared id for yet
    for (const SpawnRec& r : parse_spawn_recs(b, n)) {
        SpawnOffer& o = g.offers[r.id];
        if (o.cls.empty()) o.first = g.now;
        o.cls = r.cls, o.yaw = r.yaw, o.seen = g.now;
        memcpy(o.p, r.p, 12);
    }
}

void host_spawned(ue::UObject* pawn) {
    for (ue::UObject* o : g.natives) {  // ours: the hostile ones get a shared id (a quest's friend comes with the story)
        if (!ue::alive(o)) continue;
        uint64_t id = fnv64("spawn:" + ue::path_of(o));
        auto it = g.spawned.find(id);
        if (it == g.spawned.end()) {
            if (!hostile_to(o, pawn)) continue;
            Spawned s;
            s.first = g.now, s.cls = ue::path_of(ue::class_of(o)), s.actor = o, s.actor_cls = ue::class_of(o);
            it = g.spawned.emplace(id, s).first;
            core_log("spawned: the game spawned a " + s.cls.substr(s.cls.rfind('.') + 1) + ", shared with the other player");
        }
        if (it->second.actor != o) continue;
        it->second.seen = g.now;
        actor_place(o, it->second.p, it->second.yaw);
    }
    for (auto it = g.offers.begin(); it != g.offers.end();) {  // the client's: one of ours of that class nearby, else a copy
        SpawnOffer& off = it->second;
        if (g.now - off.seen > kSpawnGone) { it = g.offers.erase(it); continue; }
        bool answered = false;
        for (auto& kv : g.spawned) answered = answered || kv.second.offer == it->first;
        if (!answered) {
            Spawned* mine = nullptr;
            for (auto& kv : g.spawned)
                if (!kv.second.offer && kv.second.cls == off.cls && dist_f(kv.second.p, off.p) < kSpawnMatch) { mine = &kv.second; break; }
            if (mine) {
                mine->offer = it->first, g.spawn_matched++;
            } else if (g.now - off.first >= kSpawnGrace) {
                if (ue::UObject* a = spawn_copy(off.cls, off.p, off.yaw)) {
                    Spawned s;
                    s.first = s.seen = g.now, s.cls = off.cls, s.yaw = off.yaw, s.actor = a, s.actor_cls = ue::class_of(a), s.copy = true, s.offer = it->first;
                    memcpy(s.p, off.p, 12);
                    g.spawned[fnv64("spawn:" + ue::path_of(a))] = s;
                    g.spawn_copies++;
                    core_log("spawned: a copy of the other player's " + off.cls.substr(off.cls.rfind('.') + 1));
                } else {
                    off.first = g.now + 30;  // not an NPC class here: not tried again for a while
                }
            }
        }
        ++it;
    }
    std::vector<SpawnRec> recs;
    for (auto it = g.spawned.begin(); it != g.spawned.end();) {
        if (!spawned_live(it->second)) { it = g.spawned.erase(it); g.spawn_removed++; continue; }  // the game removed it
        double p[3] = {it->second.p[0], it->second.p[1], it->second.p[2]};
        if (near_players(p, pawn)) recs.push_back({it->first, it->second.cls, {it->second.p[0], it->second.p[1], it->second.p[2]}, it->second.yaw});
        ++it;
    }
    send_spawn_recs(kSpawns, recs);
}

void client_spawned(ue::UObject* pawn) {
    for (auto it = g.spawned.begin(); it != g.spawned.end();) {
        Spawned& s = it->second;
        bool live = spawned_live(s);
        if (g.now - s.seen > kSpawnGone) {  // the host's is gone: our copy goes too (one of our own stays the game's)
            if (live && s.copy) destroy_actor(s.actor);
            if (s.actor) g.spawn_ids.erase(s.actor);
            it = g.spawned.erase(it), g.spawn_removed++;
            continue;
        }
        if (!live) {
            if (s.actor) g.spawn_ids.erase(s.actor);
            s.actor = nullptr;
            for (ue::UObject* o : g.natives) {  // one of ours of that class nearby: both worlds spawned it
                float p[3], y = 0;
                if (g.spawn_ids.count(o) || !ue::alive(o) || ue::path_of(ue::class_of(o)) != s.cls || !actor_place(o, p, y) || dist_f(p, s.p) >= kSpawnMatch) continue;
                s.actor = o, s.actor_cls = ue::class_of(o), s.copy = false;
                g.spawn_ids[o] = it->first, g.spawn_matched++;
                break;
            }
            if (!s.actor && g.now - s.first >= kSpawnGrace)
                if (ue::UObject* a = spawn_copy(s.cls, s.p, s.yaw)) {
                    s.actor = a, s.actor_cls = ue::class_of(a), s.copy = true;
                    g.spawn_ids[a] = it->first, g.spawn_copies++;
                    core_log("spawned: a copy of the host's " + s.cls.substr(s.cls.rfind('.') + 1));
                }
        }
        ++it;
    }
    std::vector<SpawnRec> offers;  // ours the host has no id for, after the grace
    std::unordered_map<ue::UObject*, double> first;
    for (ue::UObject* o : g.natives) {
        if (g.spawn_ids.count(o) || !ue::alive(o)) continue;
        auto f = g.native_first.find(o);
        double t = f != g.native_first.end() ? f->second : g.now;
        first[o] = t;
        SpawnRec r;
        r.id = fnv64(ue::path_of(o)), r.cls = ue::path_of(ue::class_of(o));
        double p[3];
        if (g.now - t < kSpawnGrace || !actor_place(o, r.p, r.yaw)) continue;
        p[0] = r.p[0], p[1] = r.p[1], p[2] = r.p[2];
        if (near_players(p, pawn) && hostile_to(o, pawn)) offers.push_back(r);
    }
    g.native_first.swap(first);
    send_spawn_recs(kSpawnOffers, offers);
}

void spawned_frame(ue::UObject* pawn) {
    if (!g.have_peer || !pawn || g.now < g.next_spawn_send) return;
    g.next_spawn_send = g.now + 1.0;
    if (g.host) host_spawned(pawn);
    else client_spawned(pawn);
}

// ---------------------------------------------------------------- encounter waves (hives, hatches, doors)
// A hive's or a hatch's enemies are placed in the level (the same names in both worlds: an encounter wave pairs each
// with its spawner) and wait hidden there until their wave starts. Each world starts waves by its own player's
// proximity or the story, so a wave the host's world started left the client's copies hidden in their hives while they
// fought in the host's world (probe 116: a minute, until the client's wave started by itself). Each world tells the
// other the waves that start there (a start, not the state: an encounter its player left goes back to Inactive in that
// world alone, and must stay so); the other starts the same wave with the game's Encounter.ActivateWave.
constexpr int kWaveSize = 0x340, kWaveStateOff = 0x30a;  // EncounterWaveInformation in this build (dump-types)

struct WaveList { uint8_t* data; int32_t num, max; };
WaveList* encounter_waves(ue::UObject* e) {
    static int off = -1;
    if (off < 0) {
        ue::UClass* k = ue::find_class("/Script/Arkansas.Encounter");
        ue::FProperty* pr = k ? ue::find_prop(k, "EncounterWaves") : nullptr;
        if (!pr) return nullptr;
        off = ue::prop_offset(pr);
    }
    auto* w = reinterpret_cast<WaveList*>(reinterpret_cast<uint8_t*>(e) + off);
    return w->data && w->num > 0 && w->num < 64 ? w : nullptr;
}

void waves_frame() {
    if (!g.have_peer || g.now < g.next_waves || g.now < g.settle_until) return;
    g.next_waves = g.now + 1.0;
    if (g.now >= g.next_encounters) {  // the list: every 5 s (levels stream in and out)
        g.next_encounters = g.now + 5.0;
        g.encounters.clear();
        if (ue::UClass* k = ue::find_class("/Script/Arkansas.Encounter"))
            for (ue::UObject* o : ue::instances(k, false, 1024)) g.encounters[fnv64("enc:" + ue::path_of(o))] = o;
    }
    for (auto& kv : g.encounters) {  // a wave Inactive before, not now: started here (a wave seen first is not a start)
        if (!ue::alive(kv.second)) continue;
        WaveList* w = encounter_waves(kv.second);
        for (int i = 0; w && i < w->num; i++) {
            uint8_t now = w->data[i * kWaveSize + kWaveStateOff];
            auto key = std::make_pair(kv.first, static_cast<uint8_t>(i));
            auto was = g.wave_state.find(key);
            auto taken = g.waves_done.find(key);  // started for the other world's start: not sent back
            bool echo = taken != g.waves_done.end() && g.now - taken->second < 10.0;
            if (was != g.wave_state.end() && was->second == 0 && now != 0 && !echo) g.wave_starts[key] = g.now;
            g.wave_state[key] = now;
        }
    }
    char body[1400];
    memcpy(body, &g.my_map, 4);
    int n = 5, count = 0;
    for (auto it = g.wave_starts.begin(); it != g.wave_starts.end();) {  // each start sent for 5 s (a lost packet)
        if (g.now - it->second > 5.0) { it = g.wave_starts.erase(it); continue; }
        if (n + 9 <= static_cast<int>(sizeof body) && count < 255) {
            memcpy(body + n, &it->first.first, 8);
            body[n + 8] = static_cast<char>(it->first.second);
            n += 9, count++;
        }
        ++it;
    }
    if (!count) return;
    body[4] = static_cast<char>(count);
    send_typed(kWaves, body, n);
}

void on_waves(const char* b, int n) {  // the other world's started waves (trust boundary): bounds-checked
    uint32_t map;
    if (n < 5) { g.bad++; return; }
    memcpy(&map, b, 4);
    int count = static_cast<uint8_t>(b[4]);
    if (n != 5 + count * 9) { g.bad++; return; }
    if (map != g.my_map || g.now < g.settle_until) return;
    static Fn act;
    if (!base_fn(act, "/Script/Arkansas.Encounter", "ActivateWave")) return;
    for (int i = 0; i < count; i++) {
        uint64_t id;
        memcpy(&id, b + 5 + i * 9, 8);
        uint8_t wave = static_cast<uint8_t>(b[5 + i * 9 + 8]);
        auto key = std::make_pair(id, wave);
        auto done = g.waves_done.find(key);
        if (done != g.waves_done.end() && g.now - done->second < 10.0) continue;  // this start, sent again
        auto it = g.encounters.find(id);
        if (it == g.encounters.end() || !ue::alive(it->second)) continue;  // not loaded here
        WaveList* w = encounter_waves(it->second);
        if (!w || wave >= w->num) continue;
        g.waves_done[key] = g.now;  // (our own start of it, caused by this, is not sent back)
        if (w->data[wave * kWaveSize + kWaveStateOff] != 0) continue;  // started here too
        alignas(16) uint8_t a[16] = {};
        int32_t idx = wave;
        memcpy(a + act.off("WaveIndex"), &idx, 4);
        ue::process_event(it->second, act.f, a);
        g.waves_started++;
        core_log("encounter: wave " + std::to_string(wave) + " of " + ue::path_of(it->second).substr(ue::path_of(it->second).rfind('.') + 1) +
                 " started here too (the other world started it)");
    }
}

json spawn_test(const json& p) {  // test bridge, game thread: an NPC of a class spawned here, as the game would
    std::string path = p.get("class").asString();
    ue::UClass* cls = path.rfind("/Game/", 0) == 0 ? load_class(path) : nullptr;
    if (!cls) throw std::runtime_error("no class " + path);
    double at[3] = {p.get("x").asNumber(0), p.get("y").asNumber(0), p.get("z").asNumber(0)};
    ue::UObject* a = spawn_actor(cls, at, p.get("yaw").asNumber(0), "AdjustIfPossibleButAlwaysSpawn");
    if (!a) throw std::runtime_error(g.last_error);
    std::string err;
    if (!ue::resolve(ue::addr(a) + ".Controller", &err)) call_quiet(ue::addr(a), "SpawnDefaultController", json::object());
    return json(ue::addr(a) + " " + ue::path_of(a));
}

json spawned_list() {  // test bridge, game thread: the shared spawned NPCs, by shared id
    json out = json::array();
    for (auto& kv : g.spawned) {
        json r = json::object();
        r.set("id", std::to_string(kv.first)).set("cls", kv.second.cls.substr(kv.second.cls.rfind('.') + 1)).set("copy", kv.second.copy);
        r.set("actor", spawned_live(kv.second) ? ue::addr(kv.second.actor) : std::string("none")).set("offer", kv.second.offer != 0);
        out.push(r);
    }
    return out;
}

// ---------------------------------------------------------------- gestures (probe 92)
// The local player's weapon in hand and its events go out with each state; the other world's stand-in plays them.
void gestures_out(ue::UObject* pawn, State& st) {
    static Fn cur, holstered, mode, end, crot;
    ue::UObject* w = nullptr;
    if (base_fn(cur, "/Script/Arkansas.OwBaseCharacter", "GetCurrentWeapon")) {
        alignas(16) uint8_t b[64] = {};
        ue::process_event(pawn, cur.f, b);
        memcpy(&w, b + cur.off("ReturnValue"), 8);
        if (w && !ue::alive(w)) w = nullptr;
    }
    my_weapon.store(w, std::memory_order_relaxed);
    std::string path = w ? ue::path_of(ue::class_of(w)) : std::string();
    if (path != g.my_weapon_path) g.my_weapon_path = path, g.next_look = 0;  // the other world's stand-in swaps at once
    bool drawn = false;
    if (w && base_fn(holstered, "/Script/Arkansas.Weapon", "IsHolstered")) {
        alignas(16) uint8_t b[16] = {};
        ue::process_event(w, holstered.f, b);
        drawn = !b[holstered.off("ReturnValue")];
    }
    if (base_fn(crot, "/Script/Engine.Pawn", "GetControlRotation")) {  // the view's pitch (the body's is always 0)
        alignas(16) uint8_t b[64] = {};
        ue::process_event(pawn, crot.f, b);
        double r[3];
        memcpy(r, b + crot.off("ReturnValue"), 24);
        st.pitch = static_cast<float>(std::remainder(r[0], 360.0));
    }
    g.my_drawn = drawn;
    try { g.my_crouched = ue::eval_path(ue::addr(pawn) + ".bIsCrouched").asBool(false); } catch (const std::exception&) {}
    st.flags |= (drawn ? 2 : 0) | (my_aiming.load() ? 4 : 0) | (g.my_crouched ? 8 : 0);
    st.shots = my_shots.load(), st.swings = my_swings.load(), st.swing_kind = my_swing_kind.load(), st.reloads = my_reloads.load();
    if (st.shots != g.sent_shots && w) {  // where that shot ended (the weapon's fire mode): the stand-in's goes there
        g.sent_shots = st.shots;
        static ue::UClass* fire_k = ue::find_class("/Script/Arkansas.FireMode");
        ue::UObject* m = nullptr;
        if (base_fn(mode, "/Script/Arkansas.Weapon", "GetCurrentMode")) {
            alignas(16) uint8_t b[16] = {};
            ue::process_event(w, mode.f, b);
            memcpy(&m, b + mode.off("ReturnValue"), 8);
        }
        if (m && fire_k && ue::alive(m) && ue::is_a(m, fire_k) && base_fn(end, "/Script/Arkansas.FireMode", "GetLastShotEndLocation")) {
            alignas(16) uint8_t b[32] = {};
            ue::process_event(m, end.f, b);
            double e[3];
            memcpy(e, b + end.off("ReturnValue"), 24);
            for (int i = 0; i < 3; i++) g.my_aim[i] = static_cast<float>(e[i]);
        }
    }
    st.ax = g.my_aim[0], st.ay = g.my_aim[1], st.az = g.my_aim[2];
}

// The stand-in's weapon: the other player's class, given by its equipment component (the NPC then has that weapon's
// NPC visuals and animations), with endless ammo. Identity: its class path (a freed object's slot is reused, run 106).
ue::UObject* standin_weapon_now() {
    if (g.standin_weapon && ue::alive(g.standin_weapon) && ue::path_of(ue::class_of(g.standin_weapon)) == g.standin_weapon_path) return g.standin_weapon;
    g.standin_weapon = nullptr;
    static Fn cur;
    if (g.standin_weapon_path.empty() || !base_fn(cur, "/Script/Arkansas.OwBaseCharacter", "GetCurrentWeapon")) return nullptr;
    alignas(16) uint8_t b[64] = {};
    ue::process_event(g.puppet, cur.f, b);
    ue::UObject* w = nullptr;
    memcpy(&w, b + cur.off("ReturnValue"), 8);
    if (!w || !ue::alive(w) || ue::path_of(ue::class_of(w)) != g.standin_weapon_path) return nullptr;
    try { ue::set_path(ue::addr(w) + ".bIgnoresAmmoConsumption", json(true)); } catch (const std::exception&) {}
    return g.standin_weapon = w;
}

ue::UObject* sequencer_lib() {  // the game's cinematic controls for an NPC (AISequencerBehaviors)
    static ue::UObject* cdo = nullptr;
    if (!cdo)
        if (ue::UClass* k = ue::find_class("/Script/Arkansas.AISequencerBehaviors")) cdo = ue::cdo(k);
    return cdo;
}

// The game's NPC attack animation for a swing (EWeaponEvent) of the stand-in's weapon kind: light or heavy melee,
// unarmed. A gun's quick melee has none (no NPC animation found for it).
ue::UObject* swing_montage(uint8_t kind) {
    static Fn anims;
    if (!base_fn(anims, "/Script/Arkansas.IndianaAiCharacter", "K2_GetCurrentWeaponAnimations")) return nullptr;
    alignas(16) uint8_t b[16] = {};
    ue::process_event(g.puppet, anims.f, b);
    ue::UObject* a = nullptr;
    memcpy(&a, b + anims.off("ReturnValue"), 8);
    if (!a || !ue::alive(a)) return nullptr;
    std::string set = ue::class_name(a);  // ARK_MeleeLt_NPC_WeaponAnimations_C, ...
    struct Swings { const char *key, *dir, *right, *left, *power, *sweep; };
    static const Swings kSets[] = {
        {"MeleeLt", "MeleeLt", "AS_NPC_MeleeLt_ATK_R2L_100", "AS_NPC_MeleeLt_ATK_L2R_100", "AS_NPC_MeleeLt_ATK_Lunge_L2R_500", "AS_NPC_MeleeLt_ATK_SWP_250"},
        {"MeleeHv", "MeleeHvy", "AS_NPC_MeleeHvy_ATK_R2L_100", "AS_NPC_MeleeHvy_ATK_L2R_100", "AS_NPC_MeleeHvy_ATK_Lunge_500", "AS_NPC_MeleeHvy_ATK_R2L"},
        {"Unarmed", "Unarmed", "AS_NPC_Una_ATK_R2L", "AS_NPC_Una_ATK_L2R", "AS_NPC_Una_ATK_PWR", "AS_NPC_Una_ATK_SWP"},
    };
    const Swings* s = nullptr;
    for (const Swings& c : kSets)
        if (set.find(c.key) != std::string::npos) { s = &c; break; }
    if (!s && (set.find("ME2H") != std::string::npos || set.find("MeleeHvy") != std::string::npos)) s = &kSets[1];  // two-handed melee
    if (!s && set.find("ME1H") != std::string::npos) s = &kSets[0];
    const char* dir = nullptr;
    const char* name = nullptr;
    if (s) {
        dir = s->dir;
        name = kind == 24 || kind == 73 ? s->left
             : kind == 26 || kind == 29 || kind == 54 || kind == 75 || kind == 82 ? s->power
             : kind == 28 || kind == 76 ? s->sweep : s->right;
    } else if (kind == 35 || kind == 78) {  // a gun's quick melee: the bash of its family (NPC_Weapon_Animations_<code>)
        auto has = [&](const char* k) { return set.find(k) != std::string::npos; };
        if (has("PI2h") || has("2h_Pi") || has("Hndgn2h")) dir = "Hndgn2h", name = "AS_NPC_Hndgn2h_QuickMelee_GunButt01";
        else if (has("PI_") || has("Pistol") || has("Revolver")) dir = "Hndgn1h", name = "AS_NPC_Hndgn1h_QuickMelee01";
        else if (has("LMG") || has("LNCH") || has("HvyArmor")) dir = "HvyWpn", name = "AS_NPC_HvyWpn_QuickMelee";
        else dir = "LngGn", name = "AS_NPC_LngGn_QuickMelee_Cross";  // rifles, shotguns, the rest
    }
    if (!dir || !name) return nullptr;
    std::string object = std::string(name) + "_Montage";
    static std::unordered_map<std::string, ue::UObject*> cache;
    ue::UObject*& m = cache[object];
    if (m && ue::alive(m) && ue::class_name(m) == "AnimMontage" && ue::name_of(m) == object) return m;
    m = nullptr;
    ue::UClass* k = ue::find_class("/Script/Engine.KismetSystemLibrary");
    json req = json::object();
    req.set("Asset", std::string("/Game/Art/ANIM/Character/NPC/") + dir + "/" + object + "." + object);
    try {
        std::string r = ue::call(ue::cdo(k), ue::find_func(k, "LoadAsset_Blocking"), req).get("ReturnValue").asString();
        std::string err;
        m = r.rfind("0x", 0) == 0 ? ue::resolve(r.substr(0, r.find(' ')), &err) : nullptr;
    } catch (const std::exception&) {}
    if (m && (ue::class_name(m) != "AnimMontage" || ue::name_of(m) != object)) m = nullptr;
    return m;
}

// The other player's weapon and its use, on the stand-in: the same weapon class in hand, drawn or holstered; shots
// fired for real by the stand-in's weapon (animation, flash, sound, tracer) where the player's shot ended, their damage
// dropped (standin_damage); melee swings as the game's NPC attack animations; reloads. Probe 92.
void drive_gestures() {
    ue::UObject* lib = sequencer_lib();
    std::string err;
    // Crouch: an NPC crouches when its AI controller asks for it and its movement component, ticking on the ground, runs
    // the game's crouch (probe 103: its crouched flag, bWantsToCrouch, the animation's fields did not; in Falling, the
    // mode its placement leaves it in, the game refuses). It ticks a moment per change: a ticking component zeroes the
    // velocity the walk animation reads (8a), so its mode goes back after.
    bool want_crouch = (g.peer_flags & 8) != 0;
    std::string pz = ue::addr(g.puppet);
    auto crouched_now = [&]() {
        try { return ue::eval_path(pz + ".bIsCrouched").asBool(want_crouch); } catch (const std::exception&) { return want_crouch; }
    };
    bool retry = g.next_crouch_retry > 0 && g.now >= g.next_crouch_retry && g.crouch_tick_until == 0;
    if (retry) g.next_crouch_retry = 0;
    if (want_crouch != g.standin_crouched || (retry && crouched_now() != want_crouch)) {
        if (want_crouch != g.standin_crouched) g.standin_crouches++;
        g.standin_crouched = want_crouch;
        g.crouch_since = g.now, g.crouch_tick_until = g.now + 3.0, g.next_crouch_ask = 0;  // until it matches, 3 s at most
        // No braking while its movement component ticks: the velocity written every frame keeps the walk animation going
        // (a ticking component braked it to 0: the walk stopped for the change, 8a).
        if (g.standin_braking < 0)
            try {
                g.standin_friction = ue::eval_path(pz + ".CharacterMovement.GroundFriction").asNumber(-1);
                g.standin_braking = ue::eval_path(pz + ".CharacterMovement.BrakingDecelerationWalking").asNumber(-1);
            } catch (const std::exception&) {}
        if (g.standin_braking >= 0 && g.standin_friction >= 0)
            for (const char* f : {".CharacterMovement.GroundFriction", ".CharacterMovement.BrakingDecelerationWalking"})
                try { ue::set_path(pz + f, json(0.0)); } catch (const std::exception&) {}
    }
    if (g.crouch_tick_until > 0) {
        if (g.now >= g.next_crouch_ask) {  // asked again while it differs: its mode may have changed under it
            g.next_crouch_ask = g.now + 0.25;
            json req = json::object();
            req.set("bCrouch", want_crouch);
            call_quiet(pz + ".Controller", "K2_SetWantCrouch", req);
            try { ue::set_path(pz + ".CharacterMovement.MovementMode", "MOVE_NavWalking"); } catch (const std::exception&) {}
            json on = json::object();
            on.set("bEnabled", true);
            call_quiet(pz + ".CharacterMovement", "SetComponentTickEnabled", on);
        }
        bool is = crouched_now();
        // its crouch took ~0.7 s, standing up 0.3 s (probe 103); standing up fails while its standing capsule wouldn't fit
        if ((is == want_crouch && g.now >= g.crouch_since + 1.0) || g.now >= g.crouch_tick_until) {
            g.crouch_tick_until = 0;
            json off = json::object();
            off.set("bEnabled", false);
            call_quiet(pz + ".CharacterMovement", "SetComponentTickEnabled", off);
            try { ue::set_path(pz + ".CharacterMovement.MovementMode", g.puppet_in_air ? "MOVE_Falling" : "MOVE_Walking"); } catch (const std::exception&) {}
            if (g.standin_braking >= 0 && g.standin_friction >= 0)
                try {
                    ue::set_path(pz + ".CharacterMovement.GroundFriction", json(g.standin_friction));
                    ue::set_path(pz + ".CharacterMovement.BrakingDecelerationWalking", json(g.standin_braking));
                } catch (const std::exception&) {}
            if (is == want_crouch) g.crouch_miss_streak = 0;
            else {  // standing up was refused once after a fight (run 129): what was around, for the log
                g.crouch_misses++, g.crouch_miss_streak++, g.next_crouch_retry = g.now + 5.0;
                double a[3], b[3], v[3];
                float yaw, pitch;
                std::string where;
                if (ue::UObject* me = local_pawn())
                    if (read_transform(me, a, yaw, pitch, v) && read_transform(g.puppet, b, yaw, pitch, v))
                        where = ", " + std::to_string(static_cast<int>(std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])))) +
                                " cm from you, " + std::to_string(static_cast<int>(b[2] - a[2])) + " cm above";
                core_log(std::string("stand-in: could not ") + (want_crouch ? "crouch" : "stand up") + " (try " + std::to_string(g.crouch_miss_streak) + where + ")");
                if (g.crouch_miss_streak >= 3) {  // still not after 3 tries (~15 s): a new stand-in, standing as spawned
                    core_log("stand-in: a new one after 3 tries");
                    g.standin_respawns++;
                    destroy_puppet();
                    return;
                }
            }
        }
    }
    if (g.look_weapon != g.standin_weapon_path && g.now >= g.next_weapon_try) {
        g.next_weapon_try = g.now + 1.0;
        ue::UObject* eq = ue::resolve(ue::addr(g.puppet) + ".EquipmentComponent", &err);
        if (!eq) return;
        if (ue::UObject* old = standin_weapon_now()) {
            json a = json::object();
            a.set("WeaponClass", ue::addr(ue::class_of(old)));
            call_quiet(ue::addr(eq), "RemoveWeapon_BP", a);
        }
        g.standin_weapon = nullptr;
        const std::string want = g.look_weapon;
        static ue::UClass* weapon_k = ue::find_class("/Script/Arkansas.Weapon");
        // A path from the other player (trust boundary): only a /Game/ class that is a weapon is given.
        ue::UClass* cls = want.rfind("/Game/", 0) == 0 && want.find(' ') == std::string::npos ? load_class(want) : nullptr;
        if (cls && weapon_k && ue::cdo(cls) && ue::is_a(ue::cdo(cls), weapon_k)) {
            json a = json::object();
            a.set("Weapon", ue::addr(cls));
            call_quiet(ue::addr(eq), "EquipWeaponBP", a);
        }
        g.standin_weapon_path = want, g.standin_drawn = true, g.weapon_changes++;
        core_log("gestures: the stand-in holds " + (want.empty() ? std::string("no weapon") : want.substr(want.rfind('.') + 1)));
    }
    ue::UObject* w = standin_weapon_now();
    if (!w || !lib) {  // nothing to play with: what arrived meanwhile is not replayed later
        g.done_shots = g.peer_shots, g.done_swings = g.peer_swings, g.done_reloads = g.peer_reloads;
        return;
    }
    if (g.look_mods_for == g.standin_weapon_path) {  // its mods, with the game's own install (as for given weapons)
        std::string key = g.look_mods_for;
        for (const std::string& m : g.look_weapon_mods) key += "|" + m;
        if (key != g.standin_mods_applied) {
            g.standin_mods_applied = key;
            std::vector<ue::UClass*> want, have = mods_of(w);
            for (const std::string& m : g.look_weapon_mods)  // from the other player (trust boundary): /Game/ classes;
                if (m.rfind("/Game/", 0) == 0 && m.find(' ') == std::string::npos)  // install_mod takes weapon mods only
                    if (ue::UClass* c = load_class(m)) want.push_back(c);
            bool extra = false;
            for (ue::UClass* c : have) extra = extra || std::find(want.begin(), want.end(), c) == want.end();
            if (extra) {  // a mod the player took off: a fresh weapon, then its mods
                g.standin_weapon_path.clear(), g.standin_mods_applied.clear(), g.next_weapon_try = 0;
                return;
            }
            for (ue::UClass* c : want)
                if (std::find(have.begin(), have.end(), c) == have.end() && install_mod(w, c)) g.standin_mods_installed++;
        }
    }
    bool want_drawn = (g.peer_flags & 2) != 0;
    if (want_drawn && g.standin_drawn && g.now >= g.next_weapon_try) {  // its own AI may have put it away (a fight's end)
        static Fn cur;
        ue::UObject* held = nullptr;
        if (base_fn(cur, "/Script/Arkansas.OwBaseCharacter", "GetCurrentWeapon")) {
            alignas(16) uint8_t b[64] = {};
            ue::process_event(g.puppet, cur.f, b);
            memcpy(&held, b + cur.off("ReturnValue"), 8);
        }
        if (!held) g.standin_drawn = false, g.next_weapon_try = g.now + 1.0;  // drawn again just below
    }
    if (want_drawn != g.standin_drawn) {
        json a = json::object();
        a.set("Character", ue::addr(g.puppet)).set("bInstant", false);
        if (want_drawn) a.set("Weapon", ue::addr(w));
        call_quiet(ue::addr(lib), want_drawn ? "EquipWeaponByInstance" : "HolsterWeapon", a);
        g.standin_drawn = want_drawn;
    }
    // Aim: at the player's last shot, else along the player's view, while they fire or aim down the sights.
    bool aim = g.standin_drawn && (g.now - g.peer_shot_at < 2.0 || (g.peer_flags & 4));
    if (aim && g.now >= g.next_orient) {
        g.next_orient = g.now + 0.1;
        double face[3], pos[3], v[3];
        float yaw = 0, pitch = 0;
        if (g.now - g.peer_shot_at < 0.5) {
            for (int i = 0; i < 3; i++) face[i] = g.peer_aim[i];
        } else if (read_transform(g.puppet, pos, yaw, pitch, v)) {
            double cy = yaw * 3.14159265 / 180, cp = g.peer_pitch * 3.14159265 / 180;
            face[0] = pos[0] + std::cos(cp) * std::cos(cy) * 3000;
            face[1] = pos[1] + std::cos(cp) * std::sin(cy) * 3000;
            face[2] = pos[2] + 60 + std::sin(cp) * 3000;
        } else {
            return;
        }
        json at = json::object();
        at.set("X", face[0]).set("Y", face[1]).set("Z", face[2]);
        json c = json::object();
        c.set("FaceTarget", "None").set("FaceTargetSubLocation", "Default").set("FaceLocation", at).set("FaceActor", "None");
        json a = json::object();
        a.set("Character", ue::addr(g.puppet)).set("TrackingRegion", "TR_WeaponAim").set("AIOrientationControl", c);
        call_quiet(ue::addr(lib), "SetOrientation", a);
        g.standin_aiming = true;
    } else if (!aim && g.standin_aiming) {
        json a = json::object();
        a.set("Character", ue::addr(g.puppet)).set("TrackingRegion", "TR_WeaponAim");
        call_quiet(ue::addr(lib), "ClearOrientation", a);
        g.standin_aiming = false;
    }
    // Shots: one trigger pull per shot, released the next frame; a pull the weapon refuses (busy) is tried again.
    uint8_t pending = static_cast<uint8_t>(g.peer_shots - g.done_shots);
    if (pending > 16) g.done_shots = g.peer_shots, pending = 0;  // a restarted player: skipped
    else if (pending > 4) g.done_shots = static_cast<uint8_t>(g.peer_shots - 4), pending = 4;  // far behind: the last 4
    if (g.trigger_down) {
        json a = json::object();
        a.set("bForceEnd", false);
        call_quiet(ue::addr(w), "EndPrimaryTrigger", a);
        g.trigger_down = false;
    } else if (pending && (!g.standin_drawn || !damage_hooked)) {
        g.done_shots = g.peer_shots;  // holstered, or its shots could hurt (no damage hook in this build): none fired
    } else if (pending && g.now - g.peer_shot_at >= 0.05) {  // the aim had a moment to turn
        bool fired = false;
        try { fired = ue::call(w, ue::find_func(ue::class_of(w), "BeginPrimaryTrigger"), json::object()).get("ReturnValue").asBool(false); } catch (const std::exception&) {}
        if (fired) {
            g.trigger_down = true, g.done_shots++, g.standin_shots++, g.shot_wait_since = -1;
        } else if (g.shot_wait_since < 0) {
            g.shot_wait_since = g.now;
        } else if (g.now - g.shot_wait_since > 1.2) {  // still refused (an NPC's rate of fire may be lower): skipped
            g.done_shots++, g.shot_wait_since = -1;
        }
    }
    uint8_t sp = static_cast<uint8_t>(g.peer_swings - g.done_swings);  // the newest swing only
    if (sp) {
        g.done_swings = g.peer_swings;
        if (sp <= 16 && g.standin_drawn)
            if (ue::UObject* m = swing_montage(g.peer_swing_kind)) {
                json a = json::object();
                a.set("AnimMontage", ue::addr(m)).set("InPlayRate", 1.0).set("StartSectionName", "None");
                call_quiet(ue::addr(g.puppet), "PlayAnimMontage", a);
                g.standin_swings++;
            }
    }
    uint8_t rp = static_cast<uint8_t>(g.peer_reloads - g.done_reloads);
    if (rp) {
        g.done_reloads = g.peer_reloads;
        if (rp <= 16 && g.standin_drawn)
            if (ue::UObject* c = ue::resolve(ue::addr(g.puppet) + ".Controller", &err)) {
                call_quiet(ue::addr(c), "K2_ReloadWeapon", json::object());
                g.standin_reloads++;
            }
    }
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
    // Its feet on the other player's: their capsule's center comes, and a stand-in in the middle of a change of stance
    // (or whose standing up failed) has the other height: it would sink or float 41 cm, and floating, not stand up.
    if (ue::UObject* cap = ue::resolve(ue::addr(g.puppet) + ".CapsuleComponent"))
        if (ue::FProperty* hp = ue::find_prop(ue::class_of(cap), "CapsuleHalfHeight")) {
            float half = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(cap) + ue::prop_offset(hp));
            if (g.standin_half <= 0 && !g.standin_crouched && g.crouch_tick_until == 0) g.standin_half = half;  // as spawned
            if (g.standin_half_crouched <= 0)
                try { g.standin_half_crouched = static_cast<float>(ue::eval_path(ue::addr(g.puppet) + ".CharacterMovement.CrouchedHalfHeight").asNumber(0)); }
                catch (const std::exception&) {}
            // shortcut: the other player's capsule taken as the stand-in's (96 / 55 both, probe 102); send it if one differs
            float peer_half = (g.peer_flags & 8) && g.standin_half_crouched > 0 ? g.standin_half_crouched : g.standin_half;
            if (g.standin_half > 0 && peer_half > 0) pos[2] += half - peer_half;
        }
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
    try { drive_gestures(); } catch (const std::exception& e) { g.last_error = std::string("gestures: ") + e.what(); }
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
        steamnet::set_presence("connect", "");  // two players only: no "Join game" while playing
        if (st != 3) send_typed(kHello, &none, 0);  // keeps asking until the session is up
        return;
    }
    steamnet::set_presence("tow2coop", g.host ? "host" : "");
    steamnet::set_presence("tow2coop_new", g.host && g.host_new ? "1" : "");
    if (g.host) {  // "Join game" for friends (rich presence connect); a public game: a lobby whose members are accepted
        steamnet::set_presence("connect", ("-tow2coop_join=" + std::to_string(steamnet::me())).c_str());
        if (g.host_public) {
            steamnet::lobby_host(kVersion, steamnet::name(steamnet::me()));
            steamnet::lobby_accept_members();
        }
    }
    std::string want = g.host ? "join:" + std::to_string(steamnet::me()) : std::string("host");
    std::string pick = g.host ? std::string() : env("TOW2_COOP_HOST");
    // A named host is contacted directly, presence or not (its game details may be private); the host accepts friends.
    uint64_t named = !g.host && g.join_target ? g.join_target : pick.empty() ? 0 : steamnet::find_friend(pick);
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
        if (named && !g.host && !g.join_public && steamnet::presence(f, "tow2coop") != "host") continue;  // the chosen host: once it hosts
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

bool xp_blocked(void* self, int32_t xp, uint8_t reason) {
    try {
        if (reason > 13 || !((kQuestXp >> reason) & 1) || !g.on || g.host || !g.remote_id || !join::joined() || self != player_info()) return false;
        g.xp_blocked++;
        g.last_xp = "own " + xp_reason(reason) + " XP dropped: the host's comes instead";
        // A quest this game completed may be set on the host's by force (its quest graph not there yet), and then the
        // host's game gives no XP for it: the host takes this one then, as its own, and shares it back (kind 7).
        if (reason == 1 && xp > 0) queue_interact(7, reason, xp, "");
        return true;
    } catch (const std::exception&) { return false; }  // called from game code: nothing may escape
}

void on_xp(void* self, int32_t xp, uint8_t reason) {
    try {
        if (!g.host && join::joined() && xp > 0 && self == player_info()) character::changed();  // kept soon
        if (reason > 13 || !(((kSharedXp | (g.host ? kQuestXp : 0)) >> reason) & 1) || !g.on || !g.remote_id || xp <= 0) return;
        if ((!g.host && !join::joined()) || self != player_info()) return;  // the friend's own campaign is not shared
        queue_interact(4, reason, xp, "");
        g.xp_sent++;
        g.last_xp = "sent " + std::to_string(xp) + " XP (" + xp_reason(reason) + ")";
    } catch (const std::exception&) {}
}

// Host: the friend's quest-completed XP goes in for a quest this game completed by force (the friend's outcome), one
// for one, within 30 s; through the game's AddXP (its XP notification shows, the friend gets it back as shared XP).
void match_quest_xp() {
    uint64_t f = story::forced_completions();
    for (; g.forced_seen < f; g.forced_seen++) g.quest_credits.push_back(g.now);
    auto old = [](double t) { return g.now - t > 30.0; };
    g.quest_credits.erase(std::remove_if(g.quest_credits.begin(), g.quest_credits.end(), old), g.quest_credits.end());
    g.quest_xp_held.erase(std::remove_if(g.quest_xp_held.begin(), g.quest_xp_held.end(), [&](const std::pair<int32_t, double>& x) { return old(x.second); }),
                          g.quest_xp_held.end());
    ue::UObject* pic = g.quest_credits.empty() || g.quest_xp_held.empty() ? nullptr : player_info();
    if (!pic || !add_xp_entry) return;
    alignas(16) uint8_t details[16] = {};
    int32_t xp = g.quest_xp_held.front().first;
    add_xp_entry(pic, xp, 1, details);
    g.quest_xp_held.erase(g.quest_xp_held.begin()), g.quest_credits.erase(g.quest_credits.begin());
    g.quest_xp_credited++;
    core_log("story: " + std::to_string(xp) + " quest XP for a quest the other player completed");
}

json xp_test(int xp, int reason) {  // test bridge: an XP gain through the game's AddXP, as gameplay makes one
    ue::UObject* pic = player_info();
    if (!pic || !add_xp_entry) throw std::runtime_error(!pic ? "no player" : "AddXP is not hooked");
    alignas(16) uint8_t details[16] = {};
    json r = json::object();
    r.set("gained", add_xp_entry(pic, xp, static_cast<uint8_t>(reason), details));
    return r;
}

// Self-test of public games on one Steam account (bridge coop.lobbyProbe, off the game thread): an invisible lobby (not
// shown to friends; no rich presence is set) is made, described, searched for (a search leaves out our own), then left.
json lobby_probe() {
    json r = json::object();
    std::string why;
    if (!steamnet::start(why) || !steamnet::lobbies_on()) return r.set("ok", false).set("why", why.empty() ? "no public games" : why);
    std::string tag = "probe-" + std::to_string(GetTickCount64() % 1000000);
    steamnet::lobby_host(tag, "probe");
    double t0 = GetTickCount64() / 1000.0;
    while (!steamnet::lobby_id() && GetTickCount64() / 1000.0 - t0 < 15.0) steamnet::lobby_poll(), Sleep(100);
    r.set("lobby", std::to_string(steamnet::lobby_id())).set("madeIn", GetTickCount64() / 1000.0 - t0);
    if (steamnet::lobby_id()) {
        steamnet::lobby_search(tag);
        double t1 = GetTickCount64() / 1000.0;
        while (GetTickCount64() / 1000.0 - t1 < 6.0) steamnet::lobby_poll(), Sleep(100);
        r.set("searched", true).set("found", static_cast<double>(steamnet::lobby_results().size()));
    }
    steamnet::lobby_leave();
    return r.set("ok", true).set("left", steamnet::lobby_id() == 0);
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
    bool menu = role == "auto" || env("TOW2_COOP_MENU") == "1";  // picked in the game's main menu (the co-op entry)
    if (role == "auto") role = env("GAMEPOOL_ROLE");                 // tests: the copy's set role keeps its direct-IP setup
    if (role.empty() && menu) role = "host";
    if (role != "host" && role != "client") { why = "no role (TOW2_COOP_ROLE or GAMEPOOL_ROLE): co-op off"; return false; }
    g.host = role == "host";
    g.start_host = g.host, g.menu_mode = menu, g.role_chosen = !menu;
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
    if (menu) why = g.steam ? "Steam; the player picks host or join in the main menu (co-op)" : why + "; picked in the main menu (co-op)";
    why = std::string("tow2coop ") + kVersion + ", " + why;
    return true;
}

// ---------------------------------------------------------------- the co-op menu (main menu)
// TOW2's main menu is a conversation with the Moon Man: its choices are response widgets in the conversation widget.
// The mod adds one ("Let's play together (co-op)."); chosen, the game's choices are hidden and the mod's take their
// place: host for Steam friends, join a friend who hosts, back. They are picked by their number key or a click, through
// the mod's window procedure (the game binds its own responses natively; the mod's are bound to nothing, so the game
// never acts on them). Probe 94.
namespace {
struct MenuState {
    ue::UObject* conv = nullptr;     // the conversation widget (the game instance's)
    int level = 0;                   // 0 the game's choices + ours; 1 co-op; 2 friends who host
    std::vector<ue::UObject*> mine;  // our response widgets, in order
    std::vector<std::string> texts;  // what each shows
    std::vector<int> actions;
    std::vector<std::pair<ue::UObject*, uint8_t>> hidden;  // the game's responses we collapsed, and their visibility
    std::vector<uint64_t> hosts;     // level 2: Steam friends who host
    std::vector<steamnet::Lobby> publics;  // level 2: public games (Steam lobbies of this version)
    double search_from = 0, next_refresh = 0;
    int lang = -1;                   // 0 English, 1 French (the game's language)
    int sel = 0;                     // levels 1-2: the entry the controller is on
} menu;

double menu_last[2] = {};  // the last click, in viewport pixels (test bridge)

// A controller (XInput, as the game reads it: XINPUT1_4.dll): read once a frame; tests set its buttons (coop.set pad).
// Connected slots are read each frame, the others looked for every 2 s (a missing pad's read is slow).
constexpr uint16_t kPadUp = 0x1, kPadDown = 0x2, kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;
void pad_poll() {
    struct PadState { DWORD packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
    using GetStateFn = DWORD(WINAPI*)(DWORD, PadState*);
    static GetStateFn get = nullptr;
    static bool tried = false;
    static unsigned connected = 0;
    static double next_scan = 0;
    g.pad_prev = g.pad_now;
    if (g.pad_test >= 0) {
        g.pad_now = static_cast<uint16_t>(g.pad_test), g.pad_seen = true;
        return;
    }
    if (!tried) {
        tried = true;
        HMODULE m = GetModuleHandleW(L"XINPUT1_4.dll");
        if (!m) m = LoadLibraryW(L"XINPUT1_4.dll");
        if (m) get = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
    }
    g.pad_now = 0;
    if (!get) return;
    bool scan = g.now >= next_scan;
    if (scan) next_scan = g.now + 2.0;
    for (DWORD i = 0; i < 4; i++) {
        if (!scan && !(connected & (1u << i))) continue;
        PadState st{};
        if (get(i, &st) == ERROR_SUCCESS) connected |= 1u << i, g.pad_now |= st.buttons, g.pad_seen = true;
        else connected &= ~(1u << i);
    }
}
bool pad_pressed(uint16_t button) { return (g.pad_now & button) && !(g.pad_prev & button); }

enum MenuAction { kOpen = 1, kHost, kStopHosting, kJoinList, kStopJoining, kBack, kSearch, kJoinDirect, kHostPublic, kHostNew, kJoinFriend = 100, kJoinPublic = 200 };

const char* tr(const char* en, const char* fr) {
    if (menu.lang < 0) {
        menu.lang = 0;
        if (ue::UClass* k = ue::find_class("/Script/Engine.KismetInternationalizationLibrary")) {
            try {
                std::string c = ue::call(ue::cdo(k), ue::find_func(k, "GetCurrentLanguage"), json::object()).get("ReturnValue").asString();
                menu.lang = c.rfind("fr", 0) == 0 ? 1 : 0;
            } catch (const std::exception&) {}
        }
    }
    return menu.lang == 1 ? fr : en;
}

std::string fill(std::string s, const std::string& name) {
    size_t at = s.find("{}");
    return at == std::string::npos ? s : s.replace(at, 2, name);
}

std::vector<ue::UObject*> game_responses() {  // ConversationBaseWidget::SelectableResponseWidgets
    std::vector<ue::UObject*> out;
    static ue::FProperty* arr = nullptr;
    if (!arr) arr = ue::find_prop(ue::class_of(menu.conv), "SelectableResponseWidgets");
    if (!arr) return out;
    struct TArr { ue::UObject** data; int32_t num, max; };
    TArr a;
    memcpy(&a, reinterpret_cast<uint8_t*>(menu.conv) + ue::prop_offset(arr), sizeof a);
    for (int i = 0; i < a.num && i < 16 && a.data; i++)
        if (a.data[i] && ue::alive(a.data[i])) out.push_back(a.data[i]);
    return out;
}

uint8_t widget_visibility(ue::UObject* w) {
    static Fn get;
    if (!base_fn(get, "/Script/UMG.Widget", "GetVisibility")) return 0;
    alignas(16) uint8_t b[16] = {};
    ue::process_event(w, get.f, b);
    return b[get.off("ReturnValue")];
}

void set_visibility(ue::UObject* w, uint8_t v) {  // ESlateVisibility: 0 Visible, 1 Collapsed, 4 SelfHitTestInvisible
    static Fn set;
    if (!base_fn(set, "/Script/UMG.Widget", "SetVisibility")) return;
    alignas(16) uint8_t b[16] = {};
    b[set.off("InVisibility")] = v;
    ue::process_event(w, set.f, b);
}

void set_enabled(ue::UObject* w, bool on) {
    static Fn set;
    if (!base_fn(set, "/Script/UMG.Widget", "SetIsEnabled")) return;
    alignas(16) uint8_t b[16] = {};
    b[set.off("bInIsEnabled")] = on ? 1 : 0;
    ue::process_event(w, set.f, b);
}

bool widget_rect(ue::UObject* w, double r[4]) {  // where a widget is, in the game viewport's pixels (= client pixels)
    static Fn geo, size, vp;
    static ue::UObject* lib = nullptr;
    if (!lib)
        if (ue::UClass* k = ue::find_class("/Script/UMG.SlateBlueprintLibrary")) lib = ue::cdo(k);
    std::string err;
    ue::UObject* pc = ue::resolve("pc", &err);
    if (!lib || !pc || !base_fn(geo, "/Script/UMG.Widget", "GetCachedGeometry") || !base_fn(size, "/Script/UMG.SlateBlueprintLibrary", "GetLocalSize") ||
        !base_fn(vp, "/Script/UMG.SlateBlueprintLibrary", "LocalToViewport") || geo.size > 200 || vp.size > 256 || size.size > 256) return false;
    alignas(16) uint8_t b[256] = {}, c[256] = {};
    ue::process_event(w, geo.f, b);
    int gsz = geo.size - geo.off("ReturnValue");
    memcpy(c + size.off("Geometry"), b + geo.off("ReturnValue"), gsz);
    ue::process_event(lib, size.f, c);
    double wh[2];
    memcpy(wh, c + size.off("ReturnValue"), 16);
    for (int corner = 0; corner < 2; corner++) {
        alignas(16) uint8_t d[256] = {};
        memcpy(d + vp.off("WorldContextObject"), &pc, 8);
        memcpy(d + vp.off("Geometry"), b + geo.off("ReturnValue"), gsz);
        double at[2] = {corner ? wh[0] : 0.0, corner ? wh[1] : 0.0};
        memcpy(d + vp.off("LocalCoordinate"), at, 16);
        ue::process_event(lib, vp.f, d);
        memcpy(r + corner * 2, d + vp.off("PixelPosition"), 16);
    }
    return true;
}

bool widget_under(ue::UObject* w, double x, double y) {  // a click at client (viewport) pixels
    double r[4];
    return widget_rect(w, r) && x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3];
}

bool mine_ok(ue::UObject* w) { return w && ue::alive(w) && ue::class_name(w) == "ConversationResponse_BP_C"; }

void entry_text(ue::UObject* w, int number, const std::string& text) {
    std::string err;
    if (ue::UObject* n = ue::resolve(ue::addr(w) + ".ResponseNumberTextBlock", &err)) {
        json a = json::object();
        a.set("InString", "[" + std::to_string(number) + "]");
        call_quiet(ue::addr(n), "SetText", a);
        set_visibility(n, 4);
    }
    if (ue::UObject* m = ue::resolve(ue::addr(w) + ".MessageTextBlock", &err)) {
        json a = json::object();
        a.set("InText", text);
        call_quiet(ue::addr(m), "SetText", a);
    }
}

ue::UObject* new_entry(ue::UObject* box) {  // a response widget of the conversation's own class, in its container
    std::string err;
    ue::UObject* pc = ue::resolve("pc", &err);
    ue::UObject* cls = ue::resolve(ue::addr(menu.conv) + ".ResponseWidgetClass", &err);
    ue::UClass* lib = ue::find_class("/Script/UMG.WidgetBlueprintLibrary");
    if (!pc || !cls || !lib) return nullptr;
    json a = json::object();
    a.set("WorldContextObject", ue::addr(pc)).set("WidgetType", ue::addr(cls)).set("OwningPlayer", ue::addr(pc));
    std::string r;
    try { r = ue::call(ue::cdo(lib), ue::find_func(lib, "Create"), a).get("ReturnValue").asString(); } catch (const std::exception& e) { g.last_error = std::string("menu: ") + e.what(); }
    ue::UObject* w = r.rfind("0x", 0) == 0 ? ue::resolve(r.substr(0, r.find(' ')), &err) : nullptr;
    if (!w) return nullptr;
    json c = json::object();
    c.set("Content", ue::addr(w));
    call_quiet(ue::addr(box), "AddChildToVerticalBox", c);
    return w;
}

void clear_mine() {
    for (ue::UObject* w : menu.mine)
        if (mine_ok(w)) call_quiet(ue::addr(w), "RemoveFromParent", json::object());
    menu.mine.clear(), menu.texts.clear(), menu.actions.clear();
}

void show_game(bool show) {
    if (show) {
        for (auto& h : menu.hidden)
            if (h.first && ue::alive(h.first)) set_visibility(h.first, h.second), set_enabled(h.first, true);
        menu.hidden.clear();
        return;
    }
    for (ue::UObject* w : game_responses()) {
        bool done = false;
        for (auto& h : menu.hidden) done = done || h.first == w;
        if (done) continue;
        menu.hidden.push_back({w, widget_visibility(w)});
        set_visibility(w, 1), set_enabled(w, false);
    }
}

// Our entries as wanted (text, action), numbered from `first`: widgets reused, made or removed; texts set on change.
void show_entries(const std::vector<std::pair<std::string, int>>& want, int first) {
    std::string err;
    ue::UObject* box = ue::resolve(ue::addr(menu.conv) + ".ResponsesContainer", &err);
    if (!box) return;
    for (size_t i = 0; i < menu.mine.size(); i++)
        if (!mine_ok(menu.mine[i])) { clear_mine(); break; }
    while (menu.mine.size() > want.size()) {
        call_quiet(ue::addr(menu.mine.back()), "RemoveFromParent", json::object());
        menu.mine.pop_back(), menu.texts.pop_back(), menu.actions.pop_back();
    }
    for (size_t i = 0; i < want.size(); i++) {
        if (i == menu.mine.size()) {
            ue::UObject* w = new_entry(box);
            if (!w) return;
            menu.mine.push_back(w), menu.texts.push_back(""), menu.actions.push_back(0);
        }
        std::string label = want[i].first + "#" + std::to_string(first + static_cast<int>(i));
        if (menu.texts[i] != label) entry_text(menu.mine[i], first + static_cast<int>(i), want[i].first), menu.texts[i] = label;
        menu.actions[i] = want[i].second;
    }
}

std::string peer_name() {
    if (g.steam && steamnet::on() && (g.have_peer || g.join_target)) return steamnet::name(g.have_peer ? g.peer_id : g.join_target);
    return tr("your friend", "ton ami");
}

bool steam_up() {  // the Steam link is only started by the co-op loop, which waits for a role: the menu starts it itself
    if (!g.steam) return false;
    if (!steamnet::on()) {
        std::string why;
        if (steamnet::start(why)) core_log("co-op menu: " + why);
    }
    return steamnet::on();
}

std::vector<uint64_t> hosting_friends() {
    std::vector<uint64_t> out;
    if (!steam_up()) return out;
    for (uint64_t f : steamnet::friends_in_game())
        if (steamnet::presence(f, "tow2coop") == "host") out.push_back(f);
    return out;
}

// The game's own New Game, from the main menu: Scripts.MainMenuNewGame (what the Moon Man's "New Game" runs) sets
// where his conversation leads; ending it starts the New Game (the character creator: CharacterCreation_Lite, then
// 0501_PS_Intro_P; probe 109).
bool main_menu_new_game() {
    ue::UClass* k = ue::find_class("/Script/Arkansas.Scripts");
    ue::UFunction* f = k ? ue::find_func(k, "MainMenuNewGame") : nullptr;
    ue::UObject* lib = nullptr;
    if (ue::UClass* c = ue::find_class("/Script/Arkansas.ConversationBlueprintLibrary")) lib = ue::cdo(c);
    ue::UObject* menu_talk = active_conversation();
    if (!f || !lib || !menu_talk) return false;
    try {
        ue::call(ue::cdo(k), f, json::object());
        json a = json::object();
        a.set("Instance", ue::addr(menu_talk));
        ue::call(lib, ue::find_func(ue::class_of(lib), "EndConversationInstance"), a);
    } catch (const std::exception& e) {
        g.last_error = std::string("new game: ") + e.what();
        return false;
    }
    core_log("new game together: the game's New Game starts (the character creator)");
    return true;
}

bool host_is_new(uint64_t host) { return g.steam && steamnet::on() && steamnet::presence(host, "tow2coop_new") == "1"; }

void choose_off() {
    if (g.steam && steamnet::on()) {
        steamnet::set_presence("tow2coop", "");
        steamnet::set_presence("connect", "");
        steamnet::lobby_leave();
        if (g.have_peer) steamnet::close(g.peer_id);
        g.have_peer = false;
    }
    g.role_chosen = false, g.join_target = 0, g.snaps.clear(), g.remote_id = 0, g.remote_last_seq = 0, g.gestures_synced = false;
    g.host_public = false, g.join_public = false, g.public_wanted = false, g.host_new = false, g.new_char = false, g.new_char_stage = 0;
    if (g.steam && steamnet::on()) steamnet::set_presence("tow2coop_new", "");
    destroy_puppet();
    core_log("co-op menu: co-op off");
}

void choose_role(bool host, uint64_t target) {
    if (g.role_chosen) choose_off();
    g.host = host, g.join_target = target, g.role_chosen = true;
    if (g.steam) g.have_peer = false;  // found again: a host takes the friend who joins, a client the chosen host
    core_log(host ? std::string("co-op menu: you host") : "co-op menu: you join " + (target ? steamnet::name(target) : std::string("the host at the configured address")));
}

void menu_act(int action) {
    if (action == kOpen) menu.level = 1;
    else if (action == kBack) menu.level = menu.level == 2 ? 1 : 0;
    else if (action == kHost) choose_role(true, 0), menu.level = 0;
    else if (action == kHostPublic) choose_role(true, 0), g.host_public = true, menu.level = 0;
    else if (action == kHostNew) {  // the game's New Game, hosted: the friend makes a character and joins it
        choose_role(true, 0), g.host_new = true, menu.level = 0;
        if (!main_menu_new_game()) core_log("new game together: the game's New Game did not start (" + g.last_error + ")");
    }
    else if (action == kStopHosting || action == kStopJoining) choose_off(), menu.level = 0;
    else if (action == kJoinList || action == kSearch) {
        menu.level = 2, menu.search_from = g.now, menu.next_refresh = 0, menu.hosts.clear(), menu.publics.clear();
        if (steam_up())
            for (uint64_t f : steamnet::friends_in_game()) steamnet::request_presence(f);
    } else if (action == kJoinDirect) {
        choose_role(false, 0), menu.level = 0;
        if (!character::has_own()) g.new_char = main_menu_new_game();  // a character of their own first
    }
    else if (action >= kJoinPublic && action - kJoinPublic < static_cast<int>(menu.publics.size())) {
        if (g.role_chosen) choose_off();
        steamnet::lobby_join(menu.publics[action - kJoinPublic].id);  // entered: its owner is the host (frame)
        g.public_wanted = true, menu.level = 0;
        core_log("co-op menu: joining the public game of " + menu.publics[action - kJoinPublic].host);
    } else if (action >= kJoinFriend && action - kJoinFriend < static_cast<int>(menu.hosts.size())) {
        uint64_t h = menu.hosts[action - kJoinFriend];
        choose_role(false, h), menu.level = 0;
        if (host_is_new(h) || !character::has_own()) g.new_char = main_menu_new_game();  // their new game: our character first
    }
    g.menu_picks++;
}

void menu_reset() {
    clear_mine();
    show_game(true);
    menu.level = 0;
    menu_own = 0;
}

// What is shown at each level, from the co-op state.
std::vector<std::pair<std::string, int>> menu_wanted() {
    std::vector<std::pair<std::string, int>> v;
    bool hosting = g.role_chosen && g.host, joining = g.role_chosen && !g.host;
    bool playing = g.role_chosen && g.have_peer && !g.snaps.empty() && g.now - g.snaps.back().t < 5.0;
    if (menu.level == 0) {
        std::string s = playing ? fill(tr("Co-op: playing with {}.", "Co-op : tu joues avec {}."), peer_name())
                      : hosting && g.host_public ? tr("Co-op: my game is open to everyone (public).", "Co-op : ma partie est ouverte à tous (publique).")
                      : hosting ? tr("Co-op: my game is open to my Steam friends.", "Co-op : ma partie est ouverte à mes amis Steam.")
                      : g.public_wanted ? tr("Co-op: joining a public game...", "Co-op : je rejoins une partie publique...")
                      : joining ? fill(tr("Co-op: joining {}...", "Co-op : je rejoins {}..."), peer_name())
                      : tr("Let's play together (co-op).", "Jouons ensemble (co-op).");
        v.push_back({s, kOpen});
    } else if (menu.level == 1) {
        bool can_host = g.steam || g.start_host, can_join = g.steam || !g.start_host;  // a direct-IP copy keeps its set role
        if (hosting) v.push_back({tr("I'll stop hosting.", "Je n'héberge plus."), kStopHosting});
        else if (can_host) {
            v.push_back({tr("I'll host: my Steam friends can join my game.", "J'héberge : mes amis Steam pourront me rejoindre."), kHost});
            v.push_back({tr("I'll host a new game: we both start from the very beginning.", "J'héberge une nouvelle partie : on commence tous les deux depuis le début."), kHostNew});
            if (g.steam && steamnet::lobbies_on())
                v.push_back({tr("I'll host: anyone can join (public game).", "J'héberge : tout le monde peut me rejoindre (partie publique)."), kHostPublic});
        }
        if (joining) v.push_back({tr("I'll stop joining.", "J'arrête de rejoindre."), kStopJoining});
        else if (can_join) v.push_back({tr("I'll join a friend's game.", "Je rejoins la partie d'un ami."), kJoinList});
        v.push_back({tr("Never mind.", "Laisse tomber."), kBack});
    } else {
        if (!g.steam) {
            v.push_back({tr("Join the host at the configured address.", "Rejoindre l'hôte à l'adresse configurée."), kJoinDirect});
        } else {
            // New game together: without a character of their own the player would join as an emptied copy of the
            // host's (its look too): their own new game makes one (the character creator), then they come back here.
            static double checked = -100;
            static bool own = true;
            if (g.now - checked > 5.0) checked = g.now, own = character::has_own();
            for (size_t i = 0; i < menu.hosts.size() && i < 7; i++) {  // a new game, or no character yet: ours is made first
                bool fresh = host_is_new(menu.hosts[i]);
                const char* label = fresh ? tr("Join {} (a new game: you make your character first).", "Rejoindre {} (nouvelle partie : tu crées d'abord ton personnage).")
                                    : !own ? tr("Join {} (you make your character first).", "Rejoindre {} (tu crées d'abord ton personnage).")
                                           : tr("Join {}.", "Rejoindre {}.");
                v.push_back({fill(label, steamnet::name(menu.hosts[i])), kJoinFriend + static_cast<int>(i)});
            }
            for (size_t i = 0; i < menu.publics.size() && v.size() < 7; i++)
                v.push_back({fill(tr("Join {} (public game).", "Rejoindre {} (partie publique)."), menu.publics[i].host.empty() ? std::string("?") : menu.publics[i].host),
                             kJoinPublic + static_cast<int>(i)});
            if (menu.hosts.empty() && menu.publics.empty())
                v.push_back({g.now - menu.search_from < 2.5 ? tr("Looking for friends who host...", "Je cherche des amis qui hébergent...")
                                                              : tr("Nobody is hosting right now. Look again.", "Personne n'héberge pour l'instant. Cherche encore."), kSearch});
            else
                v.push_back({tr("Look again.", "Cherche encore."), kSearch});
        }
        v.push_back({tr("Never mind.", "Laisse tomber."), kBack});
    }
    return v;
}

void menu_frame() {
    bool shown = false;
    if (g.menu_mode && g.at_menu) {
        if (!menu.conv || !ue::alive(menu.conv) || ue::class_name(menu.conv) != "ConversationWidget_BP_C") {
            menu.conv = nullptr;
            static ue::UClass* k = ue::find_class("/Script/Arkansas.ConversationWidget");
            if (k)
                for (ue::UObject* o : ue::instances(k, false, 2)) menu.conv = o;
        }
        static Fn vis;
        if (menu.conv && base_fn(vis, "/Script/UMG.Widget", "IsVisible")) {
            alignas(16) uint8_t b[16] = {};
            ue::process_event(menu.conv, vis.f, b);
            shown = b[vis.off("ReturnValue")] && (menu.level > 0 || game_responses().size() >= 2);
        }
    }
    menu_listen.store(shown);
    if (!shown) {
        if (menu.level || !menu.mine.empty() || !menu.hidden.empty()) menu_reset();
        return;
    }
    watch_keys();
    int key = menu_key.exchange(0), pick = -1;
    int first = menu.level == 0 ? static_cast<int>(game_responses().size()) + 1 : 1;
    if (key && key >= first && key - first < static_cast<int>(menu.mine.size())) pick = key - first;
    if (menu_clicked.exchange(false)) {
        uint64_t c = menu_click.load(), wh = menu_client.load();
        double x = static_cast<int32_t>(c >> 32), y = static_cast<int32_t>(c & 0xffffffffu);
        double cw = static_cast<int32_t>(wh >> 32), ch = static_cast<int32_t>(wh & 0xffffffffu), vp[2] = {};
        // Client pixels -> the UI's (the viewport's pixels, where LocalToViewport puts the choices): the same at 1:1; a
        // viewport drawn into a client area of another size scales (probe 98: both 960 x 540 in the test copies).
        static Fn size;
        static ue::UObject* lib = nullptr;
        if (!lib)
            if (ue::UClass* k = ue::find_class("/Script/UMG.WidgetLayoutLibrary")) lib = ue::cdo(k);
        std::string err;
        ue::UObject* pc = ue::resolve("pc", &err);
        if (lib && pc && base_fn(size, "/Script/UMG.WidgetLayoutLibrary", "GetViewportSize")) {
            alignas(16) uint8_t b[64] = {};
            memcpy(b + size.off("WorldContextObject"), &pc, 8);
            ue::process_event(lib, size.f, b);
            memcpy(vp, b + size.off("ReturnValue"), 16);
        }
        if (cw > 0 && ch > 0 && vp[0] > 0 && vp[1] > 0) x = x * vp[0] / cw, y = y * vp[1] / ch;
        menu_last[0] = x, menu_last[1] = y;
        for (size_t i = 0; i < menu.mine.size() && pick < 0; i++)
            if (mine_ok(menu.mine[i]) && widget_under(menu.mine[i], x, y)) pick = static_cast<int>(i);
    }
    if (pick < 0 && g.pad_seen && !menu.actions.empty()) {  // a controller: Y opens; then the D-pad moves, A picks, B goes back
        int n = static_cast<int>(menu.actions.size());
        if (menu.level == 0) pick = pad_pressed(kPadY) ? 0 : -1;
        else {
            menu.sel = std::min(menu.sel, n - 1);
            if (pad_pressed(kPadDown)) menu.sel = (menu.sel + 1) % n;
            if (pad_pressed(kPadUp)) menu.sel = (menu.sel + n - 1) % n;
            if (pad_pressed(kPadA)) pick = menu.sel;
            else if (pad_pressed(kPadB)) pick = static_cast<int>(std::find(menu.actions.begin(), menu.actions.end(), static_cast<int>(kBack)) - menu.actions.begin());
        }
    }
    if (pick >= 0 && pick < static_cast<int>(menu.actions.size())) {
        int before = menu.level;
        menu_act(menu.actions[pick]);
        if (menu.level != before) clear_mine(), menu.sel = 0;  // other entries now
    }
    if (menu.level == 2 && g.now >= menu.next_refresh) {
        menu.next_refresh = g.now + 1.0, menu.hosts = hosting_friends();
        if (g.steam && steamnet::on()) steamnet::lobby_search(kVersion), menu.publics = steamnet::lobby_results();
    }
    show_game(menu.level == 0);
    first = menu.level == 0 ? static_cast<int>(game_responses().size()) + 1 : 1;
    std::vector<std::pair<std::string, int>> want = menu_wanted();
    if (g.pad_seen && !want.empty()) {  // the controller's way: its button on the entry, the entry it is on marked
        if (menu.level == 0) want[0].first += " (Y)";
        else want[std::min<size_t>(menu.sel, want.size() - 1)].first = "\xC2\xBB " + want[std::min<size_t>(menu.sel, want.size() - 1)].first;
    }
    show_entries(want, first);
    menu_own = menu.level == 0 ? first : -1;  // the window procedure: the digit that is ours (-1: all of them)
}
}  // namespace

// Steam: lobby calls complete; "Join game" from the friends list (or an invite, or the command line) joins that host
// from the main menu (in a game of their own, the player goes back to the main menu first); a public game picked in the
// menu is joined once its lobby is entered.
void steam_requests() {
    static bool cmdline_read = false;
    if (!cmdline_read) {  // Steam started the game for "Join game": the connect string is on the command line
        cmdline_read = true;
        std::string c = GetCommandLineA();
        size_t at = c.find("-tow2coop_join=");
        if (at != std::string::npos) g.pending_join = strtoull(c.c_str() + at + 15, nullptr, 10), core_log("co-op: started to join a friend's game (Steam)");
    }
    if (!g.steam || !steamnet::on()) return;
    steamnet::lobby_poll();
    if (uint64_t h = steamnet::take_join_request()) g.pending_join = h;
    if (uint64_t owner = steamnet::lobby_joined()) {
        if (g.public_wanted) {
            choose_role(false, owner);
            g.join_public = true;  // a stranger's game: no rich presence to read, its lobby is the invitation
        }
        g.public_wanted = false;
    }
    if (g.pending_join && g.menu_mode && g.at_menu) {
        uint64_t h = g.pending_join;
        g.pending_join = 0;
        choose_role(false, h);
        core_log("co-op: joining " + steamnet::name(h) + " (Steam)");
    } else if (g.pending_join) {
        static uint64_t told = 0;
        if (told != g.pending_join) told = g.pending_join, core_log("co-op: go back to the main menu to join " + steamnet::name(g.pending_join));
    }
}

json menu_status() {
    json m = json::array(), rects = json::array();
    for (const std::string& t : menu.texts) m.push(t.substr(0, t.rfind('#')));
    for (ue::UObject* w : menu.mine) {
        double r[4] = {};
        json a = json::array();
        if (mine_ok(w) && widget_rect(w, r))
            for (double v : r) a.push(v);
        rects.push(a);
    }
    json click = json::array();
    click.push(menu_last[0]).push(menu_last[1]);
    json s = json::object();
    s.set("rects", rects).set("lastClick", click);
    s.set("level", static_cast<double>(menu.level)).set("entries", m).set("shown", menu_listen.load()).set("own", static_cast<double>(menu_own.load()));
    return s;
}

// ---------------------------------------------------------------- companions out of a fight
// Owner, 2026-11-19 (point 4): one party, the host's. Each world's companions follow their own player, so the two
// worlds disagreed on where they were. Out of a fight the client's world shows them where they are in the host's
// (driven like the stand-in: their AI paused, placed each frame, walking from the velocity); in a fight they get their
// own AI back in each world, as co-op combat wants.
namespace {
void host_send_party(ue::UObject* pawn) {
    if (g.now < g.next_party_send) return;
    g.next_party_send = g.now + 0.2;
    uint8_t buf[2 + 8 * 36];
    buf[0] = g.now >= g.party_fight_until && !pawn_in_combat(pawn) ? 1 : 0, buf[1] = 0;
    for (auto& kv : g.party) {
        if (buf[1] >= 8 || !ue::alive(kv.second)) continue;
        double pos[3], vel[3];
        float yaw, pitch;
        if (!read_transform(kv.second, pos, yaw, pitch, vel)) continue;
        uint8_t* r = buf + 2 + buf[1] * 36;
        float f[7] = {static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2]), yaw,
                      static_cast<float>(vel[0]), static_cast<float>(vel[1]), static_cast<float>(vel[2])};
        memcpy(r, &kv.first, 8), memcpy(r + 8, f, sizeof f);
        buf[1]++;
    }
    send_typed(kParty, buf, 2 + buf[1] * 36);
}

void party_ai(ue::UObject* c, bool on) {
    std::string a = ue::addr(c);
    json reason = json::object(), tick = json::object();
    reason.set("Reason", "coop party");
    tick.set("bEnabled", on);
    call_quiet(a + ".Controller.BrainComponent", on ? "RestartLogic" : "StopLogic", on ? json::object() : reason);
    call_quiet(a + ".CharacterMovement", "SetComponentTickEnabled", tick);
    if (!on) call_quiet(a + ".Controller", "StopMovement", json::object());
}

void client_drive_party(ue::UObject* pawn) {
    bool calm = g.party_calm_in && g.now - g.party_in_at < 2.0 && g.now >= g.party_fight_until && !pawn_in_combat(pawn);
    static Fn set;
    if (!base_fn(set, "/Script/Engine.Actor", "K2_SetActorLocationAndRotation")) return;
    for (auto& kv : g.party) {
        ue::UObject* c = kv.second;
        if (!c || !ue::alive(c)) continue;
        auto it = g.party_in.find(kv.first);
        bool drive = calm && it != g.party_in.end() && g.now - it->second.second.t < 2.0;
        bool tamed = g.party_tamed.count(c) != 0;
        if (!drive) {
            if (tamed) party_ai(c, true), g.party_tamed.erase(c), g.party_releases++;
            continue;
        }
        if (!tamed) party_ai(c, false), g.party_tamed.insert(c), g.party_tames++;
        const Ctx::PartySnap &a = it->second.first, &b = it->second.second;
        double k = std::min(1.0, std::max(0.0, (g.now - b.t) / 0.2));  // toward the newest over one send interval
        double pos[3] = {a.p[0] + (b.p[0] - a.p[0]) * k, a.p[1] + (b.p[1] - a.p[1]) * k, a.p[2] + (b.p[2] - a.p[2]) * k};
        double rot[3] = {0, b.yaw, 0}, v[3] = {b.v[0], b.v[1], b.v[2]};
        alignas(16) uint8_t buf[512] = {};
        memcpy(buf + set.off("NewLocation"), pos, 24);
        memcpy(buf + set.off("NewRotation"), rot, 24);
        buf[set.off("bTeleport")] = 1;
        ue::process_event(c, set.f, buf);
        if (ue::UObject* cmc = ue::resolve(ue::addr(c) + ".CharacterMovement")) {
            if (ue::FProperty* vp = ue::find_prop(ue::class_of(cmc), "Velocity")) memcpy(reinterpret_cast<uint8_t*>(cmc) + ue::prop_offset(vp), v, 24);
            if (ue::FProperty* vp = ue::find_prop(ue::class_of(cmc), "ComponentVelocity")) memcpy(reinterpret_cast<uint8_t*>(cmc) + ue::prop_offset(vp), v, 24);
        }
    }
    for (auto t = g.party_tamed.begin(); t != g.party_tamed.end();) {  // gone from the party (or the world): forget it
        bool still = false;
        for (auto& kv : g.party) still = still || kv.second == *t;
        if (!still || !ue::alive(*t)) t = g.party_tamed.erase(t);
        else ++t;
    }
}
}  // namespace

// ---------------------------------------------------------------- shared conversations
// Owner, 2026-11-20: the player who starts a conversation leads it; the other player's game plays the same conversation
// (its camera, voices and cinematics) next to the leader and can't choose: it shows the choices, and the leader's pick
// is made there too. The follower starts the leader's conversation asset at the leader's node (StartConversation's
// StartNode: any node, a response too; probe 108). A response the leader picks is picked there by its node (a response
// widget keeps its flowchart node at +0x378: id at +8, type at +0x10), else the conversation starts again at it. Lines
// run on their own in both games; a follower left behind skips to the leader's line. Both games run the conversation's
// scripts (items, quests, globals): the story sync then finds them applied. No game pauses for it (the base game's own
// conversations don't pause, and a paused one stops at its first line: run 139).
namespace {
constexpr int kRespNodeOff = 0x378;    // ConversationResponseWidget -> its FPlayerResponseNode (not reflected; probe 108)
constexpr int kRespParentOff = 0x388;  // -> the line it answers (its FTalkNode)

ue::UObject* conv_lib_cdo() {
    static ue::UObject* cdo = nullptr;
    if (!cdo)
        if (ue::UClass* k = ue::find_class("/Script/Arkansas.ConversationBlueprintLibrary")) cdo = ue::cdo(k);
    return cdo;
}

ue::UObject* conv_screen() {  // the conversation screen on show (ConversationWidget_BP_C)
    static ue::UClass* k = ue::find_class("/Script/Arkansas.ConversationWidget");
    static Fn vis;
    if (!k || !base_fn(vis, "/Script/UMG.Widget", "IsVisible")) return nullptr;
    for (ue::UObject* o : ue::instances(k, false, 8)) {
        alignas(16) uint8_t b[16] = {};
        ue::process_event(o, vis.f, b);
        if (b[vis.off("ReturnValue")]) return o;
    }
    return nullptr;
}

ue::UObject* active_conversation() {  // the local player's conversation (GetActivePlayerConversation), or nullptr
    static Fn f;
    ue::UObject* cdo = conv_lib_cdo();
    if (!cdo || !base_fn(f, "/Script/Arkansas.ConversationBlueprintLibrary", "GetActivePlayerConversation")) return nullptr;
    alignas(16) uint8_t b[16] = {};
    ue::process_event(cdo, f.f, b);
    ue::UObject* inst = nullptr;
    memcpy(&inst, b + f.off("ReturnValue"), 8);
    return inst && ue::alive(inst) ? inst : nullptr;
}

// OEICineConversationInstance.NavigatorState: its CurrentNodeID and Conversation.ConversationAsset.
struct NavOff { int node = -1, asset = -1; } nav_off;
bool nav_ready(ue::UObject* inst) {
    if (nav_off.node >= 0) return true;
    ue::FProperty* ns = ue::find_prop(ue::class_of(inst), "NavigatorState");
    ue::UStruct* st = ns ? ue::struct_of(ns) : nullptr;
    ue::FProperty* cur = st ? ue::find_prop(st, "CurrentNodeID") : nullptr;
    ue::FProperty* ref = st ? ue::find_prop(st, "Conversation") : nullptr;
    ue::UStruct* rs = ref ? ue::struct_of(ref) : nullptr;
    ue::FProperty* asset = rs ? ue::find_prop(rs, "ConversationAsset") : nullptr;
    if (!cur || !asset) return false;
    nav_off.node = ue::prop_offset(ns) + ue::prop_offset(cur);
    nav_off.asset = ue::prop_offset(ns) + ue::prop_offset(ref) + ue::prop_offset(asset);
    return true;
}
int32_t conv_node(ue::UObject* inst) {
    return inst && nav_ready(inst) ? *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(inst) + nav_off.node) : -1;
}
ue::UObject* conv_asset(ue::UObject* inst) {
    if (!inst || !nav_ready(inst)) return nullptr;
    ue::UObject* a = *reinterpret_cast<ue::UObject* const*>(reinterpret_cast<const uint8_t*>(inst) + nav_off.asset);
    return a && ue::alive(a) ? a : nullptr;
}

struct Shown { int32_t node, parent; bool selectable; const uint8_t* data; uint8_t* widget; };
std::vector<Shown> shown_responses(ue::UObject* screen) {  // the responses on a conversation screen, by node
    std::vector<Shown> out;
    if (!screen) return out;
    static ue::FProperty *sel = nullptr, *uns = nullptr;
    if (!sel) sel = ue::find_prop(ue::class_of(screen), "SelectableResponseWidgets"), uns = ue::find_prop(ue::class_of(screen), "UnselectableResponseWidgets");
    if (!sel || !uns) return out;
    struct TArr { ue::UObject** data; int32_t num, max; };
    for (int pass = 0; pass < 2; pass++) {
        TArr a;
        memcpy(&a, reinterpret_cast<uint8_t*>(screen) + ue::prop_offset(pass ? uns : sel), sizeof a);
        for (int i = 0; i < a.num && i < 16 && a.data; i++) {
            const uint8_t* w = reinterpret_cast<const uint8_t*>(a.data[i]);
            if (!w || !ue::alive(a.data[i]) || !ue::readable(w + kRespNodeOff, kRespParentOff + 8 - kRespNodeOff)) continue;
            const uint8_t* n = *reinterpret_cast<const uint8_t* const*>(w + kRespNodeOff);
            const uint8_t* up = *reinterpret_cast<const uint8_t* const*>(w + kRespParentOff);
            if (!n || !ue::readable(n, 0x28) || n[0x10] != 1) continue;  // EFlowChartNodeType::PlayerResponse
            int32_t parent = up && ue::readable(up, 0x10) ? *reinterpret_cast<const int32_t*>(up + 8) : -1;
            out.push_back({*reinterpret_cast<const int32_t*>(n + 8), parent, pass == 0, n, const_cast<uint8_t*>(w)});
        }
    }
    return out;
}

// The response `node` picked on our screen: the Nth selectable one is OnResponseN. One our character can't take (a skill
// check, a flaw: in UnselectableResponseWidgets) is put first for one OnResponse1 call: first in the selectable widgets,
// its selectable flag (+0x380, probe 108) set, and first in the screen's own list of the selectable responses' nodes
// (+0x5C0, not reflected: what OnResponseN picks from, probe 113), checked against the widgets first; all put back when
// the game left them as they were (a pick rebuilds the lists). 0: not on our screen, 1: picked, 2: picked past the lock.
constexpr int kRespSelectableOff = 0x380, kScreenSelectableNodesOff = 0x5C0;
volatile bool conv_applying = false;  // our own pick on the follower's screen: not blocked (conv_swallow)
int pick_response(int32_t node) {
    ue::UObject* s = conv_screen();
    static ue::FProperty* sel_prop = nullptr;
    if (!s) return 0;
    if (!sel_prop) sel_prop = ue::find_prop(ue::class_of(s), "SelectableResponseWidgets");
    int k = 0, at = 0;
    uint8_t* locked = nullptr;
    for (const Shown& r : shown_responses(s)) {
        if (r.selectable) k++;
        if (r.node != node) continue;
        if (r.selectable && node != g.conv_lock_test) at = k;
        else locked = r.widget;
    }
    auto press = [&](int n) {
        conv_applying = true;
        call_quiet(ue::addr(s), ("OnResponse" + std::to_string(n) + "InputAction").c_str(), json::object());
        conv_applying = false;
    };
    if (at && at <= 9) return press(at), 1;
    if (!locked || !sel_prop) return 0;
    struct TArr { ue::UObject** data; int32_t num, max; };
    auto* arr = reinterpret_cast<TArr*>(reinterpret_cast<uint8_t*>(s) + ue::prop_offset(sel_prop));
    if (!arr->data || arr->num < 1) return 0;  // shortcut: no selectable response to stand in for it (all locked), the follower starts again there
    struct NArr { const uint8_t** data; int32_t num, max; };
    auto* nodes = reinterpret_cast<NArr*>(reinterpret_cast<uint8_t*>(s) + kScreenSelectableNodesOff);
    if (!ue::readable(nodes, sizeof *nodes) || !nodes->data || nodes->num < arr->num || !ue::readable(nodes->data, static_cast<size_t>(nodes->num) * 8)) return 0;
    // All selectable (a lock the test simulates): the node list is the selectable widgets' own, in order; a real lock
    // (a skill check: probe 124) keeps every shown response there, locked ones too, in the conversation's order.
    bool full_list = nodes->num != arr->num;
    for (int i = 0; i < arr->num && !full_list; i++)  // the screen's own list, as the widgets show it (this build's layout)
        if (!arr->data[i] || !ue::readable(reinterpret_cast<uint8_t*>(arr->data[i]) + kRespNodeOff, 8) ||
            *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<uint8_t*>(arr->data[i]) + kRespNodeOff) != nodes->data[i])
            return 0;
    TArr was = *arr;
    NArr nwas = *nodes;
    ue::UObject* first = arr->data[0];
    const uint8_t* first_node = nodes->data[0];
    const uint8_t* node_ptr = *reinterpret_cast<const uint8_t* const*>(locked + kRespNodeOff);
    uint8_t flag = locked[kRespSelectableOff];
    arr->data[0] = reinterpret_cast<ue::UObject*>(locked), locked[kRespSelectableOff] = 1;
    if (!full_list) nodes->data[0] = node_ptr;
    press(1);  // (the game doesn't check its requirements again: the flag is enough, probe 124)
    if (arr->data == was.data && arr->num == was.num && arr->data[0] == reinterpret_cast<ue::UObject*>(locked)) arr->data[0] = first;
    if (!full_list && nodes->data == nwas.data && nodes->num == nwas.num && nodes->data[0] == node_ptr) nodes->data[0] = first_node;
    if (ue::alive(reinterpret_cast<ue::UObject*>(locked)) && *reinterpret_cast<const uint8_t* const*>(locked + kRespNodeOff) == node_ptr)
        locked[kRespSelectableOff] = flag;  // the same response still: its own flag back (a reused widget keeps its new one)
    return 2;
}

// A conversation asset's graph: its links by node (ConversationBundle.DialogueLinks: FromNodeID at +8, ToNodeID at
// +0xc, 0x20 each) and the bank each line belongs to (BankNodes: a random line drawn among ChildNodeIDs). Read once.
struct ConvGraph {
    std::unordered_map<int32_t, std::vector<int32_t>> next;
    std::unordered_map<int32_t, int32_t> bank;                         // line -> its bank
    std::unordered_map<int32_t, std::pair<int32_t*, int32_t>> lines;   // bank -> its ChildNodeIDs (in the asset)
};
const ConvGraph& conv_graph(ue::UObject* asset) {
    static ue::UObject* cached = nullptr;
    static int32_t cached_num = -1, links_off = -1, banks_off = -1, bank_size = 0, children_off = -1;
    static ConvGraph gr;
    struct TArr { const uint8_t* data; int32_t num, max; };
    if (asset && links_off < 0) {
        ue::FProperty* b = ue::find_prop(ue::class_of(asset), "ConversationBundle");
        ue::UStruct* bs = b ? ue::struct_of(b) : nullptr;
        ue::FProperty* l = bs ? ue::find_prop(bs, "DialogueLinks") : nullptr;
        ue::FProperty* k = bs ? ue::find_prop(bs, "BankNodes") : nullptr;
        ue::FProperty* in = k ? ue::array_inner(k) : nullptr;
        ue::FProperty* ch = in && ue::struct_of(in) ? ue::find_prop(ue::struct_of(in), "ChildNodeIDs") : nullptr;
        if (!l) return gr;
        links_off = ue::prop_offset(b) + ue::prop_offset(l);
        if (ch) banks_off = ue::prop_offset(b) + ue::prop_offset(k), bank_size = ue::prop_size(in), children_off = ue::prop_offset(ch);
    }
    TArr links{};
    if (asset && links_off >= 0) memcpy(&links, reinterpret_cast<const uint8_t*>(asset) + links_off, sizeof links);
    if (asset == cached && links.num == cached_num) return gr;
    cached = asset, cached_num = links.num, gr.next.clear(), gr.bank.clear(), gr.lines.clear();
    if (!links.data || links.num <= 0 || links.num > 100000 || !ue::readable(links.data, static_cast<size_t>(links.num) * 0x20)) return gr;
    for (int32_t k = 0; k < links.num; k++) {
        const uint8_t* e = links.data + static_cast<size_t>(k) * 0x20;
        gr.next[*reinterpret_cast<const int32_t*>(e + 8)].push_back(*reinterpret_cast<const int32_t*>(e + 0xc));
    }
    TArr banks{};
    if (banks_off >= 0) memcpy(&banks, reinterpret_cast<const uint8_t*>(asset) + banks_off, sizeof banks);
    if (banks.data && banks.num > 0 && banks.num < 10000 && bank_size >= children_off + 16 && ue::readable(banks.data, static_cast<size_t>(banks.num) * bank_size))
        for (int32_t k = 0; k < banks.num; k++) {
            const uint8_t* b = banks.data + static_cast<size_t>(k) * bank_size;
            TArr c;
            memcpy(&c, b + children_off, sizeof c);
            if (!c.data || c.num <= 0 || c.num > 256 || !ue::readable(c.data, static_cast<size_t>(c.num) * 4)) continue;
            int32_t id = *reinterpret_cast<const int32_t*>(b + 8);
            gr.lines[id] = {reinterpret_cast<int32_t*>(const_cast<uint8_t*>(c.data)), c.num};
            for (int32_t i = 0; i < c.num; i++) gr.bank[reinterpret_cast<const int32_t*>(c.data)[i]] = id;
        }
    return gr;
}
// The follower draws the leader's line: its copy of the bank lists that line only (the ids overwritten in place: the
// array stays the game's own), put back when following ends. The last 4 banks the leader went through stay pinned (the
// follower may still be a line behind).
struct Pin { ue::UObject* asset; int32_t bank; int32_t* ids; std::vector<int32_t> was; };
std::vector<Pin> pins;
void unpin_banks() {
    for (const Pin& p : pins)
        if (ue::alive(p.asset) && ue::readable(p.ids, p.was.size() * 4)) memcpy(p.ids, p.was.data(), p.was.size() * 4);
    pins.clear();
}
void pin_bank(ue::UObject* asset, int32_t bank, int32_t line) {
    for (const Pin& p : pins)
        if (p.asset == asset && p.bank == bank) {
            if (p.ids[0] == line) return;
            std::fill(p.ids, p.ids + p.was.size(), line);  // the leader went through it again: its new line
            return;
        }
    const ConvGraph& gr = conv_graph(asset);
    auto it = gr.lines.find(bank);
    if (it == gr.lines.end() || !ue::readable(it->second.first, static_cast<size_t>(it->second.second) * 4)) return;
    std::vector<int32_t> was(it->second.first, it->second.first + it->second.second);
    if (std::find(was.begin(), was.end(), line) == was.end()) return;  // not a line of that bank
    if (pins.size() >= 4) {  // the oldest one goes back
        const Pin& o = pins.front();
        if (ue::alive(o.asset) && ue::readable(o.ids, o.was.size() * 4)) memcpy(o.ids, o.was.data(), o.was.size() * 4);
        pins.erase(pins.begin());
    }
    std::fill(it->second.first, it->second.first + it->second.second, line);
    pins.push_back({asset, bank, it->second.first, std::move(was)});
}

int32_t conv_group(ue::UObject* inst) {  // the bank the current line was drawn from, or -1
    const ConvGraph& gr = conv_graph(conv_asset(inst));
    auto it = gr.bank.find(conv_node(inst));
    return it == gr.bank.end() ? -1 : it->second;
}

// The response picked, from the line it led to: the game names the next line, not the response (BP_OnConversation-
// NodeChanged: once per pick, for the line; run 145), and a response often goes there through hidden branch nodes
// and random banks (probe 110: 153 -> bank 160, which drew 88, 161 or 162). The shown response with the shortest path
// to the line or its bank (4 links at most).
int32_t response_to(ue::UObject* asset, const std::vector<Shown>& shown, int32_t to, int32_t group) {
    const auto& next = conv_graph(asset).next;
    int32_t best = -1;
    int best_d = 5;
    for (const Shown& r : shown) {
        std::vector<int32_t> front{r.node}, nx;
        std::unordered_set<int32_t> seen{r.node};
        for (int d = 1; d < best_d && !front.empty(); d++) {
            nx.clear();
            for (int32_t f : front) {
                auto it = next.find(f);
                if (it == next.end()) continue;
                for (int32_t t : it->second)
                    if (seen.insert(t).second) nx.push_back(t);
            }
            if (std::find(nx.begin(), nx.end(), to) != nx.end() || (group >= 0 && std::find(nx.begin(), nx.end(), group) != nx.end())) {
                best = r.node, best_d = d;
                break;
            }
            front.swap(nx);
        }
    }
    return best;
}

ue::UObject* ret_object(const json& r) {  // a call's ReturnValue object ("0x... Class path"), or nullptr
    std::string s = r.get("ReturnValue").asString(), err;
    return s.rfind("0x", 0) == 0 ? ue::resolve(s.substr(0, s.find(' ')), &err) : nullptr;
}

json conv_lib_call(const char* fn, const json& a) {
    ue::UObject* cdo = conv_lib_cdo();
    ue::UFunction* f = cdo ? ue::find_func(ue::class_of(cdo), fn) : nullptr;
    if (!f) throw std::runtime_error(std::string("no ") + fn);
    return ue::call(cdo, f, a);
}

void conv_call(const char* fn, ue::UObject* inst) {  // EndConversationInstance, AdvanceConversationInstance
    if (!inst) return;
    json a = json::object();
    a.set("Instance", ue::addr(inst));
    try { conv_lib_call(fn, a); } catch (const std::exception& e) { g.last_error = std::string("conversation: ") + e.what(); }
}

ue::UObject* conv_partner(ue::UObject* inst, ue::UObject* pawn) {  // whom the conversation is with: a speaker not us
    try {
        ue::UClass* k = ue::class_of(inst);
        ue::UFunction* cur = ue::find_func(k, "GetCurrentSpeaker");
        ue::UFunction* all = ue::find_func(k, "BP_GetAllSpeakers");
        ue::UObject* a = cur ? ret_object(ue::call(inst, cur, json::object())) : nullptr;
        if (a && a != pawn && a != g.puppet) return a;
        if (all)
            for (const json& v : ue::call(inst, all, json::object()).get("ReturnValue").items()) {
                std::string t = v.asString(), err;
                ue::UObject* o = t.rfind("0x", 0) == 0 ? ue::resolve(t.substr(0, t.find(' ')), &err) : nullptr;
                if (o && o != pawn && o != g.puppet) return o;
            }
    } catch (const std::exception&) {}
    return nullptr;
}

ue::UObject* load_asset(const std::string& path) {  // an asset by path, loaded if needed (blocking)
    if (ue::UObject* o = ue::find_path(path)) return o;
    ue::UClass* k = ue::find_class("/Script/Engine.KismetSystemLibrary");
    if (!k) return nullptr;
    json req = json::object();
    req.set("Asset", path);
    try { return ret_object(ue::call(ue::cdo(k), ue::find_func(k, "LoadAsset_Blocking"), req)); } catch (const std::exception&) { return nullptr; }
}

ue::UObject* start_conversation(ue::UObject* asset, ue::UObject* owner, int32_t node) {  // ours, at that node
    static ue::UClass* character = ue::find_class("/Script/Arkansas.IndianaCharacter");
    if (!owner) return nullptr;
    json a = json::object();
    a.set("StartNode", node).set("bAllowBarkConversationDuringCombat", false);
    const char* fn = "StartConversationWithActor";  // an owner that is no character (a terminal): its own conversation
    if (asset && character && ue::is_a(owner, character)) fn = "StartConversation", a.set("Conversation", ue::addr(asset)).set("Owner", ue::addr(owner));
    else a.set("Speaker", ue::addr(owner));
    try {
        json r = conv_lib_call(fn, a);
        return r.get("Result").asString() == "OnTrue" ? ret_object(r) : nullptr;
    } catch (const std::exception& e) { g.last_error = std::string("conversation: ") + e.what(); return nullptr; }
}

bool can_start(ue::UObject* asset) {  // CanStartConversation: not in a fight, not in another conversation
    std::string err;
    ue::UObject* pc = ue::resolve("pc", &err);
    if (!asset || !pc) return true;  // a terminal's: tried as it is
    json a = json::object();
    a.set("WorldContextObject", ue::addr(pc)).set("Conversation", ue::addr(asset)).set("bAllowBarkConversationDuringCombat", false);
    try { return conv_lib_call("CanStartConversation", a).get("ReturnValue").asBool(true); } catch (const std::exception&) { return true; }
}

ue::UObject* text_row() {  // a response line of the game's own, alone in the viewport, on one line
    std::string err;
    ue::UObject* pc = ue::resolve("pc", &err);
    static ue::UClass* cls = nullptr;
    if (!cls) cls = load_class("/Game/UI/Conversation/ConversationResponse_BP.ConversationResponse_BP_C");
    ue::UClass* lib = ue::find_class("/Script/UMG.WidgetBlueprintLibrary");
    if (!pc || !cls || !lib) return nullptr;
    json a = json::object();
    a.set("WorldContextObject", ue::addr(pc)).set("WidgetType", ue::addr(cls)).set("OwningPlayer", ue::addr(pc));
    ue::UObject* w = nullptr;
    try { w = ret_object(ue::call(ue::cdo(lib), ue::find_func(lib, "Create"), a)); } catch (const std::exception&) {}
    if (!w) return nullptr;
    json z = json::object();
    z.set("ZOrder", 60);
    call_quiet(ue::addr(w), "AddToViewport", z);
    if (ue::UObject* n = ue::resolve(ue::addr(w) + ".ResponseNumberTextBlock", &err)) set_visibility(n, 1);
    json nw = json::object(), nat = json::object();
    nw.set("InAutoTextWrap", false), nat.set("InWrapTextAt", 0.0);
    call_quiet(ue::addr(w) + ".MessageTextBlock", "SetAutoWrapText", nw);
    call_quiet(ue::addr(w) + ".MessageTextBlock", "SetWrapTextAt", nat);
    return w;
}

void viewport_size(double vp[2]) {
    vp[0] = 1920, vp[1] = 1080;
    static Fn size;
    static ue::UObject* lib = nullptr;
    std::string err;
    ue::UObject* pc = ue::resolve("pc", &err);
    if (!lib)
        if (ue::UClass* k = ue::find_class("/Script/UMG.WidgetLayoutLibrary")) lib = ue::cdo(k);
    if (!lib || !pc || !base_fn(size, "/Script/UMG.WidgetLayoutLibrary", "GetViewportSize")) return;
    alignas(16) uint8_t b[64] = {};
    memcpy(b + size.off("WorldContextObject"), &pc, 8);
    ue::process_event(lib, size.f, b);
    memcpy(vp, b + size.off("ReturnValue"), 16);
}

// ----- leading: our conversation, told to the other player
struct Lead {
    ue::UObject* inst = nullptr;  // our conversation, led by us
    uint32_t conv = 0, picks = 0;
    int32_t node = -1, prev = -1, group = -1, recent[4] = {-1, -1, -1, -1};
    std::vector<Shown> shown;     // the responses on our screen for the line we are on
    std::string asset, owner;
    uint64_t owner_id = 0;
    double next_send = 0, next_look = 0;
    int closing = 0;              // closing states still to send
} lead;
uint32_t lead_count = 0;

void send_conv(bool open) {
    ConvMsg m{};
    m.open = open ? 1 : 0;
    m.conv = lead.conv, m.node = lead.node, m.prev = lead.prev, m.group = lead.group, m.picks = lead.picks;
    memcpy(m.recent, lead.recent, sizeof m.recent);
    double p[3] = {}, v[3];
    float pitch = 0;
    if (ue::UObject* pawn = local_pawn()) read_transform(pawn, p, m.yaw, pitch, v);
    for (int i = 0; i < 3; i++) m.pos[i] = static_cast<float>(p[i]);
    m.owner_id = lead.owner_id;
    size_t al = std::min<size_t>(lead.asset.size(), 255), ol = std::min<size_t>(lead.owner.size(), std::min<size_t>(255, sizeof m.text - al));
    m.asset_len = static_cast<uint8_t>(al), m.owner_len = static_cast<uint8_t>(ol);
    memcpy(m.text, lead.asset.data(), al);
    memcpy(m.text + al, lead.owner.data(), ol);
    send_typed(kConversation, &m, kConvHead + static_cast<int>(al + ol));
    g.conv_states_sent++;
}

void lead_frame(ue::UObject* inst, ue::UObject* pawn) {
    if (inst != lead.inst) {  // a new conversation of ours
        if (!lead_count) lead_count = static_cast<uint32_t>(GetTickCount64() & 0xFFFFFF) << 8;  // a restarted game's count is new
        lead = Lead{};
        lead.inst = inst, lead.conv = ++lead_count;
        g.conv_leads++;
    }
    bool changed = false, picked = false;
    if (g.now >= lead.next_look) {  // the asset (a hub conversation goes on in another one) and whom it is with (known
        lead.next_look = g.now + 0.25;  // once a line is spoken)
        ue::UObject* asset = conv_asset(inst);
        std::string a = asset ? ue::path_of(asset) : std::string();
        bool first = lead.owner.empty();
        if (first)
            if (ue::UObject* who = conv_partner(inst, pawn)) lead.owner = ue::path_of(who), lead.owner_id = other_id(who), changed = true;
        if (a != lead.asset || (first && !lead.owner.empty())) {
            lead.asset = a, changed = true;
            if (!lead.owner.empty())
                core_log("conversation: we lead " + a.substr(a.rfind('/') + 1) + " with " + lead.owner.substr(lead.owner.rfind('.') + 1) + "; " + peer_name() +
                         "'s game plays it too");
        }
    }
    int32_t n = conv_node(inst);
    changed = changed || n != lead.node;
    if (n != lead.node) {
        lead.group = conv_group(inst);
        if (!lead.shown.empty() && lead.node >= 0 && n >= 0) {  // the line we answered moved on: we picked one of them
            int32_t r = response_to(conv_asset(inst), lead.shown, n, lead.group);
            std::memmove(lead.recent + 1, lead.recent, 3 * sizeof(int32_t));
            lead.recent[0] = r >= 0 ? r : n, lead.picks++, g.conv_picks_sent++, picked = true;  // not found: the line it led to
        }
        lead.prev = lead.node, lead.node = n;
    }
    lead.shown.clear();
    for (const Shown& r : shown_responses(conv_screen()))
        if (r.parent == n) lead.shown.push_back(r);
    if (changed || picked || g.now >= lead.next_send) send_conv(true), lead.next_send = g.now + 0.25;
}

// ----- following: the other player's conversation, played here
struct Follow {
    bool on = false;
    uint32_t conv = 0, picks_done = 0;
    ue::UObject* inst = nullptr;         // our conversation that plays the leader's
    int32_t restart = -1;                // started again at this node once ours is gone
    int32_t pick = -1, pick_from = -1;   // the leader's response being made here, and our node when tried
    int pick_tries = 0, starts = 0, restarts = 0, restart_fails = 0;
    double pick_since = 0, next_pick = 0, next_try = 0, mismatch_since = -1, next_advance = 0, gone_since = -1, asset_since = -1;
    std::vector<int32_t> mine;           // our nodes since the last pick (the leader's node among them: we are ahead)
    ue::UObject *screen = nullptr, *box = nullptr, *hint = nullptr;
    uint8_t box_vis = 4;
} follow;
ue::UObject* followed_inst = nullptr;    // the last conversation we played for the leader (never ours to lead)
uint32_t block_names[14];
std::atomic<uint64_t> conv_blocked{0};

void follow_input(bool on) {  // the follower's own choices and skips are dropped (conv_swallow), the responses not hit
    if (on && !block_names[9]) {
        for (int i = 0; i < 9; i++) block_names[i] = ue::make_name("OnResponse" + std::to_string(i + 1) + "InputAction", false).index;
        block_names[9] = ue::make_name("OnSkipInputAction", false).index;
        // The conversation's payments (Scripts: money, items handed over, a bounty, a purchase) are the leader's: a
        // follower's game would take them a second time from its own player. Its rewards stay (each player's own).
        int k = 10;
        for (const char* n : {"RemoveCurrency", "RemoveItemFromPlayer", "PayOffBounty", "BuyItemWithCurrency"}) block_names[k++] = ue::make_name(n, false).index;
    }
    conv_block.store(on, std::memory_order_release);
    ue::UObject* s = on ? conv_screen() : nullptr;
    if (follow.box && (!on || (s && s != follow.screen))) {  // the screen it was set on: as it was
        if (ue::alive(follow.box)) set_visibility(follow.box, follow.box_vis);
        follow.box = nullptr, follow.screen = nullptr;
    }
    if (!s) return;
    if (!follow.box) {
        std::string err;
        follow.screen = s;
        follow.box = ue::resolve(ue::addr(s) + ".ResponsesContainer", &err);
        if (!follow.box) return;
        follow.box_vis = widget_visibility(follow.box);
        if (follow.box_vis == 3) follow.box_vis = 4;
    }
    if (ue::alive(follow.box) && widget_visibility(follow.box) != 3) set_visibility(follow.box, 3);  // HitTestInvisible
}

void follow_hint(bool on) {  // one line at the top: who chooses
    if (!on || (follow.hint && !ue::alive(follow.hint))) {
        if (follow.hint && ue::alive(follow.hint)) call_quiet(ue::addr(follow.hint), "RemoveFromParent", json::object());
        follow.hint = nullptr;
        if (!on) return;
    }
    if (follow.hint) return;
    follow.hint = text_row();
    if (!follow.hint) return;
    double vp[2];
    viewport_size(vp);
    json t = json::object(), pos = json::object(), xy = json::object();
    t.set("InText", fill(tr("{} leads this conversation: you see their choices.", "{} mène cette conversation : tu vois ses choix."), peer_name()));
    call_quiet(ue::addr(follow.hint) + ".MessageTextBlock", "SetText", t);
    xy.set("X", vp[0] * 0.30).set("Y", vp[1] * 0.05);
    pos.set("Position", xy).set("bRemoveDPIScale", true);
    call_quiet(ue::addr(follow.hint), "SetPositionInViewport", pos);
}

void follow_stop(const char* why) {
    if (!follow.on) return;
    ue::UObject* inst = active_conversation();
    if (inst && (inst == follow.inst || follow.restart >= 0)) conv_call("EndConversationInstance", inst);
    follow_input(false);
    follow_hint(false);
    unpin_banks();
    follow.on = false, follow.restart = -1, follow.pick = -1;
    core_log(std::string("conversation: following ended (") + why + ")");
}

void join_leader(const ConvMsg& m) {  // the follower comes next to the leader: the conversation's camera is from here
    ue::UObject* pawn = local_pawn();
    double pos[3], vel[3];
    float yaw = 0, pitch = 0;
    if (!pawn || !read_transform(pawn, pos, yaw, pitch, vel) || g.puppet_map != g.my_map) return;
    double d = std::hypot(pos[0] - m.pos[0], pos[1] - m.pos[1]);
    if (d < 600.0) return;
    double a = m.yaw * 3.14159265 / 180 + 1.5708;
    json args = json::object(), to = json::object();
    to.set("X", m.pos[0] + 150.0 * std::cos(a)).set("Y", m.pos[1] + 150.0 * std::sin(a)).set("Z", m.pos[2] + 50.0);
    args.set("NewLocation", to).set("bSweep", false).set("bTeleport", true);
    call_quiet(ue::addr(pawn), "K2_SetActorLocation", args);
    core_log("conversation: " + std::to_string(static_cast<int>(d / 100)) + " m from " + peer_name() + ": moved next to them");
}

bool follow_targets(const ConvMsg& m, ue::UObject*& asset, ue::UObject*& owner) {  // the leader's conversation, in our world
    std::string asset_path(m.text, m.asset_len), owner_path(m.text + m.asset_len, m.owner_len);
    owner = m.owner_id ? other_actor(m.owner_id) : nullptr;
    if (!owner && !owner_path.empty()) owner = ue::find_path(owner_path);
    asset = asset_path.empty() ? nullptr : load_asset(asset_path);
    return owner != nullptr;
}

void follow_jump(ue::UObject* inst, int32_t node, const char* why) {  // ours starts again at the leader's node
    follow.restart = node, follow.restart_fails = 0, follow.mismatch_since = -1;
    g.conv_jumps++;
    conv_call("EndConversationInstance", inst);
    core_log(std::string("conversation: started again at the leader's node ") + std::to_string(node) + " (" + why + ")");
}

void follow_frame(ue::UObject* inst) {
    const ConvMsg& m = g.peer_conv;
    bool open = m.open && g.now - g.peer_conv_at < 3.0 && g.opt_conv_follow;
    if (!open) {
        follow_stop(m.open ? "no word from the leader" : "the leader's conversation ended");
        return;
    }
    if (follow.on && m.conv != follow.conv) follow_stop("the leader started another one");
    if (!follow.on) {
        if (inst || g.now < follow.next_try || g.puppet_map != g.my_map) return;  // our own conversation is ours to lead
        if (!m.owner_len && !m.owner_id) return;  // whom it is with: told once a line is spoken
        ue::UObject *asset = nullptr, *owner = nullptr;
        bool here = follow_targets(m, asset, owner);
        ue::UObject* mine = nullptr;
        if (here && can_start(asset)) {
            join_leader(m);
            mine = start_conversation(asset, owner, m.node);
        }
        if (!mine) {
            follow.next_try = g.now + 1.5;
            if (follow.starts++ < 3)
                core_log("conversation: " + peer_name() + "'s conversation can't be played here yet (" + (here ? "it would not start" : "whom they talk to is not here") + ")");
            return;
        }
        follow = Follow{};
        follow.on = true, follow.conv = m.conv, follow.inst = followed_inst = mine, follow.picks_done = m.picks;
        g.conv_follows++;
        std::string a(m.text, m.asset_len);
        core_log("conversation: " + peer_name() + " leads one (" + a.substr(a.rfind('/') + 1) + ", node " + std::to_string(m.node) + "): played here, they choose");
        return;
    }
    if (follow.restart >= 0) {  // started again once ours is gone
        if (inst) return;
        ue::UObject *asset = nullptr, *owner = nullptr;
        ue::UObject* mine = follow_targets(m, asset, owner) ? start_conversation(asset, owner, follow.restart) : nullptr;
        if (mine) follow.inst = followed_inst = mine, follow.restart = -1, follow.mine.clear();
        else if (++follow.restart_fails > 40) follow_stop("it would not start again");
        return;
    }
    if (!inst) {  // ours ended by itself while the leader's goes on: again at its node (it may end there too)
        if (follow.gone_since < 0) follow.gone_since = g.now;
        if (g.now - follow.gone_since > 2.0 && follow.restarts < 4) follow.restarts++, follow.gone_since = -1, follow_jump(nullptr, m.node, "ours had ended");
        return;
    }
    follow.gone_since = -1;
    follow.inst = followed_inst = inst;
    follow_input(true);
    follow_hint(true);
    std::string want(m.text, m.asset_len);
    ue::UObject* our_asset = conv_asset(inst);
    if (our_asset && !want.empty() && ue::path_of(our_asset) != want) {  // the leader's went on in another conversation
        if (follow.asset_since < 0) follow.asset_since = g.now;
        if (g.now - follow.asset_since > 1.5) follow.asset_since = -1, follow.pick = -1, follow.picks_done = m.picks, follow_jump(inst, m.node, "the leader's conversation went on in another one");
        return;
    }
    follow.asset_since = -1;
    if (m.group >= 0) pin_bank(our_asset, m.group, m.node);  // our game draws the leader's line from that bank
    int32_t ours = conv_node(inst);
    if (follow.mine.empty() || follow.mine.back() != ours) {
        follow.mine.push_back(ours);
        if (follow.mine.size() > 24) follow.mine.erase(follow.mine.begin());
    }
    if (follow.pick < 0 && m.picks > follow.picks_done) {  // the leader's picks, in order
        uint32_t missing = m.picks - follow.picks_done;
        if (missing > 4) follow.picks_done = m.picks - 1, missing = 1;  // more were lost than are told: the newest
        follow.pick = m.recent[missing - 1], follow.pick_since = g.now, follow.pick_tries = 0, follow.next_pick = g.now;
    }
    if (follow.pick >= 0) {
        if (follow.pick_tries > 0 && ours != follow.pick_from) {  // taken here
            follow.picks_done++, follow.pick = -1, follow.mine.clear(), g.conv_picks_applied++;
        } else if (g.now >= follow.next_pick) {
            std::vector<Shown> v = shown_responses(conv_screen());
            bool elsewhere = !v.empty() && v[0].parent == ours && follow.pick_tries == 0;  // our line's responses shown
            int how = follow.pick_tries < 4 ? pick_response(follow.pick) : 0;
            if (how) {
                if (how == 2) g.conv_bypassed++;
                follow.pick_tries++, follow.pick_from = ours, follow.next_pick = g.now + 0.6;
            } else if (elsewhere || follow.pick_tries >= 4 || g.now - follow.pick_since > 2.5) {  // not shown here: started at it
                int32_t r = follow.pick;
                follow.picks_done++, follow.pick = -1;
                follow_jump(inst, r, "their response can't be chosen here");
            } else follow.next_pick = g.now + 0.2;  // not on our screen yet
        }
        return;
    }
    int32_t our_group = conv_group(inst);  // a line of the leader's bank: in step (each game draws its own)
    if (m.node >= 0 && ours != m.node && !(our_group >= 0 && our_group == m.group)) {  // behind the leader: skip there; ahead: wait
        bool ahead = std::find(follow.mine.begin(), follow.mine.end(), m.node) != follow.mine.end();
        if (follow.mismatch_since < 0) follow.mismatch_since = g.now;
        if (!ahead && ours == m.prev && g.now >= follow.next_advance) {
            conv_call("AdvanceConversationInstance", inst);
            follow.next_advance = g.now + 1.0, g.conv_advances++;
        }
        if (g.now - follow.mismatch_since > (ahead ? 8.0 : 3.0)) follow_jump(inst, m.node, ahead ? "the leader stayed behind" : "left behind");
    } else follow.mismatch_since = -1;
}

void conversation_frame() {
    // Shared only with the other player in the same session: not alone, not at the main menu (the Moon Man's), not
    // before the friend is in the host's world (a new game's start). Leaving: our input is ours again.
    if (!g.have_peer || g.at_menu || (!g.host && !join::joined())) {
        follow_stop(g.have_peer ? "not in the same game" : "the other player left");
        if (conv_block.load(std::memory_order_relaxed)) follow_input(false);
        lead.inst = nullptr, lead.closing = 0;
        return;
    }
    ue::UObject* inst = active_conversation();
    if (!inst) followed_inst = nullptr;
    follow_frame(inst);
    bool leading = inst && !follow.on && inst != followed_inst;
    if (leading) lead_frame(inst, local_pawn());
    else if (lead.inst) lead.inst = nullptr, lead.closing = 5, lead.next_send = 0;
    if (!leading && lead.closing > 0 && g.now >= lead.next_send) send_conv(false), lead.closing--, lead.next_send = g.now + 0.2;
    if (!follow.on && conv_block.load(std::memory_order_relaxed)) follow_input(false);  // never left blocking
}

void conv_world_reset() {  // a load: the old world's conversations are gone (the screen is the game instance's: it stays)
    conv_block.store(false, std::memory_order_release);
    unpin_banks();
    if (follow.box && ue::alive(follow.box)) set_visibility(follow.box, follow.box_vis);
    follow = Follow{};
    lead.inst = nullptr, lead.closing = 0;
    followed_inst = nullptr;
}

// New game together, the friend's side: their own New Game made their character. Once in the world (the character
// creator closed), connected and settled, the character is kept (a private save), then the host's game is asked for as
// from the main menu: the hand-over puts the new character into it (character::splice).
bool creating_character() {
    for (const char* c : {"/Script/Arkansas.CharacterCreationMasterWidget", "/Script/Arkansas.ARCC_MasterWidget"}) {
        static Fn vis;
        ue::UClass* k = ue::find_class(c);
        if (!k || !base_fn(vis, "/Script/UMG.Widget", "IsVisible")) continue;
        for (ue::UObject* o : ue::instances(k, false, 4)) {
            alignas(16) uint8_t b[16] = {};
            ue::process_event(o, vis.f, b);
            if (b[vis.off("ReturnValue")]) return true;
        }
    }
    return false;
}

void new_char_frame(ue::UObject* pawn, bool settled) {
    if (!g.new_char || g.host) return;
    if (join::joined()) {
        g.new_char = false, g.new_char_stage = 0;
        core_log("new game together: you play in " + peer_name() + "'s game with your new character");
        return;
    }
    if (!pawn || !settled || !g.have_peer || creating_character()) return;
    if (g.new_char_stage == 0) {  // the host's key is known once connected: the character is kept under it
        if (g.now < g.new_char_since) return;
        character::save_now();  // refused during the intro's conversation and cinematics: again in 3 s
        if (!character::status().get("saving").asBool(false)) {
            if (g.new_char_since == 0) core_log("new game together: your game doesn't allow a save yet (the intro); your character is kept once it does");
            g.new_char_since = g.now + 3.0;
            return;
        }
        g.new_char_stage = 1;
        return;
    }
    if (g.new_char_stage == 1) {  // never joined without it: the host's game would come with an emptied copy of theirs
        json cs = character::status();
        if (cs.get("saving").asBool(false)) return;
        if (cs.get("lastSaved").asNumber(-1) < 0) {
            g.new_char_stage = 0, g.new_char_since = g.now + 3.0;
            return;
        }
        g.new_char_stage = 2;
        core_log("new game together: your new character is kept; " + peer_name() + "'s game comes now");
    }
    if (g.now >= g.next_join_request && g.now >= g.fetch_quiet_until) {  // asked every 5 s until the hand-over starts
        uint32_t reason = 2;
        send_typed(kResyncRequest, &reason, 4);
        g.next_join_request = g.now + 5.0, g.join_requests++;
    }
}

// A minimized game never finishes a save (it waits for a drawn frame for the save's picture: runs 34-36), so a join
// or a resync failed while the host's window was minimized. While one of the mod's saves runs (the host's hand-over,
// the friend's private save), a minimized window is shown again behind the other windows, not activated, then
// minimized back.
void keep_drawn_for_saves() {
    static double next = 0;
    static bool shown_by_us = false;
    if (g.now < next) return;
    next = g.now + 0.5;
    bool waiting = join::status().get("state").asString() == "saving" || (!g.host && character::status().get("saving").asBool(false));
    if (!game_hwnd) watch_keys();
    if (!game_hwnd || !IsWindow(game_hwnd)) return;
    if (waiting && !shown_by_us && IsIconic(game_hwnd)) {
        window_input = false;
        ShowWindow(game_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(game_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        shown_by_us = true;
        core_log("window: the game was minimized during a save: drawn again behind the other windows until it is done");
    } else if (!waiting && shown_by_us) {
        shown_by_us = false;
        // (the game activates its window when it comes back: only the player's own click or key says they came back)
        if (window_input) core_log("window: the save is done; the player came back to the game");
        else ShowWindow(game_hwnd, SW_SHOWMINNOACTIVE), core_log("window: the save is done, the game minimized again");
    }
}

}  // namespace

std::atomic<bool> conv_block{false};
bool conv_swallow(void*, void* f) {  // any thread (ProcessEvent): the follower's own response or skip input
    uint32_t name = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(f) + 0x18);
    for (uint32_t b : block_names)
        if (b && b == name) {
            if (conv_applying) return false;
            conv_blocked.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    return false;
}

void frame(float) {
    if (!g.on) return;
    g.now = clock_s();
    ev_frame.fetch_add(1, std::memory_order_relaxed);
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
        conv_world_reset();
        g.party_tamed.clear(), g.party_in.clear();
        g.npcs.clear(), g.scanning.clear(), g.scan_cursor = 0;
        g.encounters.clear(), g.waves_done.clear(), g.wave_state.clear(), g.wave_starts.clear(), g.next_encounters = 0;
        g.interactables.clear(), g.inter_scan.clear();  // old world's doors and containers
        g.states.clear(), g.states_scan.clear(), g.state_ids.clear(), g.state_cursor = 0, g.state_last.clear(), g.state_check.clear();
        g.box_pos.clear(), g.box_counts.clear();
        doors.clear(), door_cursor = 0;
        g.at_menu = world && ue::path_of(world).find("MainMenu") != std::string::npos;
        if (g.at_menu) join::set_joined(false), join::hold(false), g.died = false, g.campaign_wanted = false, g.pickups_gone.clear(), g.state_remote.clear();  // back at the menu: the player's own games save again
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
    pad_poll();  // the controller, once a frame (the menu and giving read its presses)
    try { menu_frame(); } catch (const std::exception& e) { g.last_error = std::string("menu: ") + e.what(); }
    try { steam_requests(); } catch (const std::exception& e) { g.last_error = std::string("steam requests: ") + e.what(); }
    if (!g.role_chosen) return;  // nothing goes out until the player picks host or join (the co-op menu)
    receive();  // after the world check: data for the old world's objects is ignored until the new one settles
    watch_travel();
    if (gameplay) follow_travel(pawn);
    send_travels();
    send_gifts();
    bool give_pressed = give_key.exchange(false) || pad_pressed(kPadX);  // E, or a controller's interact (X: PrimaryInteract)
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
            gestures_out(pawn, st);
            send_raw(&st, sizeof st);
        }
    }
    if (g.last_recv_t >= 0 && g.now - g.last_recv_t > 5.0) {  // peer silent: drop it, wait for a (re)join
        g.snaps.clear();
        destroy_puppet();
        g.last_recv_t = -1;
        g.remote_id = 0, g.remote_last_seq = 0;  // a restarted peer has a new id and starts its sequence again
        g.gestures_synced = false;               // and its own weapon event counts
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
        try { quest_items_frame(pawn); } catch (const std::exception& e) { g.last_error = std::string("quest items: ") + e.what(); }
        if (g.opt_doors) poll_doors(), poll_states();
        apply_xp_inbox();
        if (g.host) match_quest_xp();
        send_outgoing();
        if (g.have_peer && g.now >= g.next_look) {
            g.next_look = g.now + 2.0;
            send_look();
        }
        if (g.have_peer && g.now >= g.next_face) {
            g.next_face = g.now + 5.0;
            try { send_face(); } catch (const std::exception& e) { g.last_error = std::string("send face: ") + e.what(); }
        }
        try { conversation_frame(); } catch (const std::exception& e) { g.last_error = std::string("conversation: ") + e.what(); }
        try { keep_drawn_for_saves(); } catch (const std::exception& e) { g.last_error = std::string("window: ") + e.what(); }
        try { new_char_frame(pawn, settled); } catch (const std::exception& e) { g.last_error = std::string("new game: ") + e.what(); }
        if (g.have_peer && g.opt_party && pawn) {
            try { if (g.host) host_send_party(pawn); else client_drive_party(pawn); } catch (const std::exception& e) { g.last_error = std::string("party: ") + e.what(); }
        }
        if (g.have_peer && g.opt_no_pause) keep_running();
        drive_puppet();
        if (g.opt_scan) scan_step(pawn);
        if (g.opt_scan && g.opt_npcs) spawned_frame(pawn);
        if (g.opt_scan && g.opt_npcs) waves_frame();
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
    if (!g.host && g.at_menu && !g.new_char && g.now - g.map_since > 30.0 && g.now - g.start_time > 60.0 && g.now >= g.next_join_request &&
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

// The game's damage function (hooked in main.cpp, game thread): the stand-in's shots and swings hurt nobody. Its
// weapon fires for real for the look and sound; the other player's real hits arrive as damage messages, and a kill by
// the stand-in would give nobody its XP. A projectile counts by its owner or instigator, a weapon by its outer chain.
bool damage_hooked = false;
bool standin_damage(void* causer_, void* instigator_) {
    ue::UObject* p = g.puppet;
    if (!p) return false;
    auto* causer = static_cast<ue::UObject*>(causer_);
    auto* inst = static_cast<ue::UObject*>(instigator_);
    bool from = causer == p || inst == p;
    static ue::UClass* actor_k = ue::find_class("/Script/Engine.Actor");
    static int owner_off = -2, inst_off = -2;
    if (owner_off == -2) {
        ue::FProperty* o = actor_k ? ue::find_prop(actor_k, "Owner") : nullptr;
        ue::FProperty* i = actor_k ? ue::find_prop(actor_k, "Instigator") : nullptr;
        owner_off = o ? ue::prop_offset(o) : -1, inst_off = i ? ue::prop_offset(i) : -1;
    }
    for (ue::UObject* a : {causer, inst}) {
        if (from || !a) continue;
        if (actor_k && ue::is_a(a, actor_k)) {
            auto* b = reinterpret_cast<uint8_t*>(a);
            if (owner_off >= 0 && *reinterpret_cast<ue::UObject**>(b + owner_off) == p) from = true;
            if (inst_off >= 0 && *reinterpret_cast<ue::UObject**>(b + inst_off) == p) from = true;
        } else {
            const uint8_t* at = reinterpret_cast<const uint8_t*>(a);
            for (int up = 0; at && up < 4 && !from; up++) {
                at = *reinterpret_cast<const uint8_t* const*>(at + 0x20);  // UObject::OuterPrivate
                from = at == reinterpret_cast<const uint8_t*>(p);
            }
        }
    }
    if (from) g.standin_dmg_blocked++;
    return from;
}

// One of the local player's weapon events (EWeaponEvent), heard by every visuals object of the weapon in one call.
void note_weapon_event(uint8_t ev) {
    if (ev >= 128) return;
    uint32_t fr = ev_frame.load(std::memory_order_relaxed);
    ev_log[ev_log_n.fetch_add(1) % 32] = fr << 8 | ev;
    int cat = 0;  // 1 shot, 2 swing, 3 reload, 4 aim on, 5 aim off
    switch (ev) {
    case 7: case 8: case 9: case 10: case 11: case 17: case 37: case 70: case 79: cat = 1; break;  // Fire, hip, scoped, charged...
    case 23: case 24: case 26: case 28: case 29: case 35: case 54: case 66: case 73: case 74: case 75: case 76: case 78: case 82:
        cat = 2; break;  // melee: right, left, power, sweep, counter, quick, stealth (and their slow-time kin)
    case 19: case 67: cat = 3; break;  // reload, dry-fire reload
    case 42: case 44: cat = 4; break;  // fine aim start / fully aimed
    case 43: case 45: cat = 5; break;
    default: return;
    }
    // One shot sends Fire and FireHipfire, and every visuals object of the weapon (4-7) hears each, in one frame.
    if (ev_seen[cat] == fr) return;
    ev_seen[cat] = fr;
    if (cat == 1) my_shots.fetch_add(1);
    else if (cat == 2) my_swing_kind.store(ev), my_swings.fetch_add(1);
    else if (cat == 3) my_reloads.fetch_add(1);
    else my_aiming.store(cat == 4);
}

void observe(void* o, void* f, const void* p) {
    if (applying_remote || !p) return;  // our own apply (game thread): no echo
    if (trigger_event_name && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(f) + 0x18) == trigger_event_name) {
        void* w = my_weapon.load(std::memory_order_relaxed);  // only the local player's weapon: its visuals' outer chain
        const uint8_t* at = static_cast<const uint8_t*>(o);
        for (int up = 0; w && at && up < 3; up++) {
            at = *reinterpret_cast<const uint8_t* const*>(at + 0x20);  // UObject::OuterPrivate
            if (at == w) { note_weapon_event(*static_cast<const uint8_t*>(p)); break; }
        }
        return;
    }
    if (actor_destroyed_name && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(f) + 0x18) == actor_destroyed_name) {
        // Told here, while the actor still exists (it may be freed before the next frame): a level-placed loose item?
        int off = param_offset(static_cast<ue::UFunction*>(f), "Actor");  // OnActorDestroyed(Actor)
        ue::UObject* a = off >= 0 && off + 8 <= 64 ? *reinterpret_cast<ue::UObject* const*>(static_cast<const uint8_t*>(p) + off) : nullptr;
        if (!a || !g.pickup_class || !ue::readable(a, 0x30) || !ue::is_a(a, g.pickup_class)) return;
        std::string path = ue::path_of(a);
        if (path.find("_UAID_") == std::string::npos) return;
        std::lock_guard<std::mutex> lk(obs_mu);
        if (picked_up.size() < 4096) picked_up.push_back(fnv64(path));
        return;
    }
    Observed e{};
    e.obj = o, e.fn = f;
    uint16_t size = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(f) + 0xB6);  // UFunction::ParmsSize
    memcpy(e.parms, p, std::min<size_t>(size, sizeof e.parms));
    std::lock_guard<std::mutex> lk(obs_mu);
    if (observed.size() < 4096) observed.push_back(e);
}

json set_options(const json& p) {
    for (auto& kv : {std::make_pair("story", &g.opt_story), std::make_pair("doors", &g.opt_doors), std::make_pair("npcs", &g.opt_npcs), std::make_pair("scan", &g.opt_scan),
                     std::make_pair("follow", &g.opt_follow), std::make_pair("gifts", &g.opt_gifts), std::make_pair("giveHold", &g.opt_give_hold), std::make_pair("combat", &g.opt_combat), std::make_pair("fakeDead", &g.opt_fake_dead),
                     std::make_pair("noPause", &g.opt_no_pause), std::make_pair("party", &g.opt_party),
                     std::make_pair("convFollow", &g.opt_conv_follow)})
        if (p.get(kv.first).isBool()) *kv.second = p.get(kv.first).asBool(true);
    if (p.get("pad").isNumber()) {  // test: buttons pressed on a simulated controller; -1: the real one again
        g.pad_test = static_cast<int>(p.get("pad").asInt(-1));
        if (g.pad_test < 0) g.pad_seen = false;  // a connected controller sets it again
    }
    if (p.get("convLockTest").isNumber()) g.conv_lock_test = static_cast<int32_t>(p.get("convLockTest").asInt(-1));
    if (p.get("dropInteract").isNumber()) g.opt_drop_interact = std::clamp(p.get("dropInteract").asNumber(0), 0.0, 0.95);
    if (p.get("menu").isBool()) {  // test: the role picked in the main menu from now on (off: the set role again)
        if (p.get("menu").asBool()) { g.menu_mode = true; if (g.role_chosen) choose_off(); g.host = g.start_host; }
        else g.menu_mode = false, g.host = g.start_host, g.role_chosen = true;
    }
    json s = json::object();
    s.set("story", g.opt_story).set("doors", g.opt_doors).set("npcs", g.opt_npcs).set("scan", g.opt_scan).set("follow", g.opt_follow).set("gifts", g.opt_gifts).set("combat", g.opt_combat);
    return s;
}

// Test bridge, game thread: {class, x, y, z, yaw} spawns an NPC as the game would; {} lists the shared spawned NPCs.
json spawned(const json& p) { return p.get("class").isString() ? spawn_test(p) : spawned_list(); }

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
    s.set("pickupsSent", static_cast<double>(g.pickups_sent)).set("pickupsApplied", static_cast<double>(g.pickups_applied))
        .set("pickupsGone", static_cast<double>(g.pickups_gone.size())).set("questItemsSent", static_cast<double>(g.qitems_sent))
        .set("questItemsGiven", static_cast<double>(g.qitems_given)).set("statesSent", static_cast<double>(g.states_sent))
        .set("questItemsDropped", [] { json a = json::array(); for (const auto& n : g.qitems_dropped) a.push(n); return a; }())
        .set("statesApplied", static_cast<double>(g.states_applied)).set("statesWatched", static_cast<double>(g.states.size()));
    s.set("pausesLifted", static_cast<double>(g.pauses_lifted)).set("noPause", g.opt_no_pause);
    s.set("partyDriven", static_cast<double>(g.party_tamed.size())).set("partyTames", static_cast<double>(g.party_tames))
        .set("partyReleases", static_cast<double>(g.party_releases)).set("partyCalm", g.party_calm_in);
    {
        json shown = json::array();
        for (const Shown& r : shown_responses(conv_screen())) {
            json e = json::array();
            e.push(static_cast<double>(r.node)).push(r.selectable).push(static_cast<double>(r.parent));
            shown.push(e);
        }
        s.set("convShown", shown);
    }
    s.set("pad", static_cast<double>(g.pad_now)).set("padSeen", g.pad_seen).set("menuSel", static_cast<double>(menu.sel));
    s.set("hostNew", g.host_new).set("newChar", g.new_char).set("newCharStage", g.new_char_stage).set("joined", join::joined());
    s.set("convLead", lead.inst != nullptr).set("convFollow", follow.on).set("convLeads", static_cast<double>(g.conv_leads))
        .set("convFollows", static_cast<double>(g.conv_follows)).set("convStatesSent", static_cast<double>(g.conv_states_sent))
        .set("convPicksSent", static_cast<double>(g.conv_picks_sent)).set("convPicksApplied", static_cast<double>(g.conv_picks_applied))
        .set("convJumps", static_cast<double>(g.conv_jumps)).set("convAdvances", static_cast<double>(g.conv_advances))
        .set("convBypassed", static_cast<double>(g.conv_bypassed))
        .set("convBlocked", static_cast<double>(conv_blocked.load())).set("peerConvOpen", g.peer_conv.open != 0 && g.now - g.peer_conv_at < 3.0)
        .set("convNode", static_cast<double>(conv_node(active_conversation())));
    s.set("interactsPending", static_cast<double>(g.outgoing.size())).set("interactsResent", static_cast<double>(g.interacts_resent))
        .set("interactsLost", static_cast<double>(g.interacts_lost)).set("interactAcks", static_cast<double>(g.interact_acks));
    s.set("itemsSent", static_cast<double>(g.items_sent)).set("itemsApplied", static_cast<double>(g.items_applied)).set("lastInteract", g.last_interact);
    if (ue::UObject* pic = player_info()) s.set("xp", static_cast<double>(*reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(pic) + kXpOffset)));
    s.set("xpHook", add_xp_original != nullptr).set("xpSent", static_cast<double>(g.xp_sent)).set("xpApplied", static_cast<double>(g.xp_applied));
    s.set("questXpCredited", static_cast<double>(g.quest_xp_credited)).set("questXpHeld", static_cast<double>(g.quest_xp_held.size()))
        .set("questCredits", static_cast<double>(g.quest_credits.size()));
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
    s.set("myWeapon", g.my_weapon_path).set("myShots", static_cast<double>(my_shots.load())).set("mySwings", static_cast<double>(my_swings.load()));
    s.set("myReloads", static_cast<double>(my_reloads.load())).set("myAiming", my_aiming.load()).set("myDrawn", g.my_drawn).set("lookWeapon", g.look_weapon);
    s.set("standinWeapon", g.standin_weapon && ue::alive(g.standin_weapon) ? ue::path_of(ue::class_of(g.standin_weapon)) : std::string());
    s.set("standinDrawn", g.standin_drawn).set("standinAiming", g.standin_aiming).set("peerShots", static_cast<double>(g.peer_shots));
    s.set("standinShots", static_cast<double>(g.standin_shots)).set("standinSwings", static_cast<double>(g.standin_swings));
    s.set("standinReloads", static_cast<double>(g.standin_reloads)).set("standinDamageBlocked", static_cast<double>(g.standin_dmg_blocked));
    s.set("weaponChanges", static_cast<double>(g.weapon_changes));
    s.set("dmgNotMine", static_cast<double>(g.dmg_not_mine));
    s.set("myCrouched", g.my_crouched).set("standinCrouched", g.standin_crouched).set("standinCrouches", static_cast<double>(g.standin_crouches))
        .set("crouchMisses", static_cast<double>(g.crouch_misses)).set("crouchWindow", g.crouch_tick_until > 0)
        .set("standinRespawns", static_cast<double>(g.standin_respawns));
    s.set("standinModsInstalled", static_cast<double>(g.standin_mods_installed)).set("lookWeaponMods", static_cast<double>(g.look_weapon_mods.size()));
    s.set("menuMode", g.menu_mode).set("roleChosen", g.role_chosen).set("menuPicks", static_cast<double>(g.menu_picks)).set("menu", menu_status());
    s.set("encounters", static_cast<double>(g.encounters.size())).set("wavesStarted", static_cast<double>(g.waves_started));
    s.set("spawnedShared", static_cast<double>(g.spawned.size())).set("spawnCopies", static_cast<double>(g.spawn_copies));
    s.set("spawnMatched", static_cast<double>(g.spawn_matched)).set("spawnRemoved", static_cast<double>(g.spawn_removed));
    s.set("spawnNatives", static_cast<double>(g.natives.size())).set("spawnOffers", static_cast<double>(g.offers.size()));
    json evs = json::array();
    for (uint32_t i = ev_log_n.load() > 32 ? ev_log_n.load() - 32 : 0; i < ev_log_n.load(); i++)
        evs.push(std::to_string(ev_log[i % 32] >> 8) + ":" + std::to_string(ev_log[i % 32] & 255));
    s.set("myEvents", evs);
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
