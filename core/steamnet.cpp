// Steamworks flat API of the game's steam_api64.dll (SDK 1.57, folder Steamv157), resolved by name at run time.
#include "steamnet.hpp"
#include <windows.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

void core_log(const std::string& line);  // main.cpp: a line in tow2coop.log (thread-safe)

namespace steamnet {
namespace {
struct Identity {  // SteamNetworkingIdentity: type, size, 128-byte union (pack 8)
    int32_t type, size;
    uint8_t data[128];
};
static_assert(sizeof(Identity) == 136, "SteamNetworkingIdentity layout");
struct Message {  // head of SteamNetworkingMessage_t; released with SteamAPI_SteamNetworkingMessage_t_Release
    void* data;
    int32_t size;
    uint32_t conn;
    Identity peer;
};
static_assert(sizeof(void*) == 8 && offsetof(Message, peer) == 16, "SteamNetworkingMessage_t layout");
struct FriendGameInfo {  // FriendGameInfo_t
    uint64_t game_id;
    uint32_t ip;
    uint16_t game_port, query_port;
    uint64_t lobby;
};
static_assert(sizeof(FriendGameInfo) == 24, "FriendGameInfo_t layout");

void *msgs_, *friends_, *netutils_;
int rate_set_ = 0;  // send buffer and rates set (3 = all)
uint64_t me_;
uint32_t app_;
int (*send_)(void*, const Identity&, const void*, uint32_t, int, int);
int (*recv_)(void*, int, Message**, int);
bool (*close_)(void*, const Identity&);
int (*state_)(void*, const Identity&, void*, void*);
void (*release_)(Message*);
void (*set_id_)(Identity*, uint64_t);
uint64_t (*get_id_)(Identity*);
bool (*set_rp_)(void*, const char*, const char*);
const char* (*get_rp_)(void*, uint64_t, const char*);
void (*req_rp_)(void*, uint64_t);
int (*friend_count_)(void*, int);
uint64_t (*friend_at_)(void*, int, int);
bool (*friend_game_)(void*, uint64_t, FriendGameInfo*);
const char* (*friend_name_)(void*, uint64_t);
int (*relay_)(void*, void*);
bool (*accept_)(void*, const Identity&);
int (*relationship_)(void*, uint64_t);
constexpr int kImmediateFriends = 4;  // k_EFriendFlagImmediate

// A session request (someone sent first, we never wrote to them): accepted from Steam friends only. Meant to cover a
// host that cannot see the client's rich presence (private game details), but in the first two-PC test (beta.3) the
// host never accepted: accept_friends() polls instead; this logs whether the callback comes at all.
void on_session_request(void* request) {  // SteamNetworkingMessagesSessionRequest_t: the requester's identity first
    Identity* who = static_cast<Identity*>(request);
    uint64_t id = get_id_(who);
    bool ok = id && relationship_(friends_, id) == 3 && accept_(msgs_, *who);  // k_EFriendRelationshipFriend
    core_log("steam: session request from " + name(id) + (ok ? ", accepted" : ", not accepted (not a Steam friend)"));
}

template <class T>
bool get(HMODULE m, const char* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return out != nullptr;
}

Identity id_of(uint64_t user) {
    Identity i{};
    set_id_(&i, user);
    return i;
}
}  // namespace

bool start_lobbies(HMODULE m);  // below

bool start(std::string& why) {
    if (msgs_) return true;
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    if (!m) { why = "steam_api64.dll is not loaded (start the game through Steam)"; return false; }
    int (*hsteamuser)() = nullptr;
    void *(*acc_msgs)() = nullptr, *(*acc_friends)() = nullptr, *(*acc_user)() = nullptr, *(*acc_utils)() = nullptr, *(*acc_net)() = nullptr;
    uint64_t (*user_id)(void*) = nullptr;
    uint32_t (*appid)(void*) = nullptr;
    void (*init_relay)(void*) = nullptr;
    bool (*set_session_cb)(void*, void (*)(void*)) = nullptr;
    bool ok = get(m, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest", set_session_cb) &&
              get(m, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", accept_) &&
              get(m, "SteamAPI_ISteamFriends_GetFriendRelationship", relationship_) && get(m, "SteamAPI_GetHSteamUser", hsteamuser) && get(m, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002", acc_msgs) &&
              get(m, "SteamAPI_SteamFriends_v017", acc_friends) && get(m, "SteamAPI_SteamUser_v021", acc_user) &&
              get(m, "SteamAPI_SteamUtils_v010", acc_utils) && get(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", acc_net) &&
              get(m, "SteamAPI_ISteamUser_GetSteamID", user_id) && get(m, "SteamAPI_ISteamUtils_GetAppID", appid) &&
              get(m, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", init_relay) &&
              get(m, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus", relay_) &&
              get(m, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser", send_) &&
              get(m, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", recv_) &&
              get(m, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser", close_) &&
              get(m, "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo", state_) &&
              get(m, "SteamAPI_SteamNetworkingMessage_t_Release", release_) && get(m, "SteamAPI_SteamNetworkingIdentity_SetSteamID64", set_id_) &&
              get(m, "SteamAPI_SteamNetworkingIdentity_GetSteamID64", get_id_) && get(m, "SteamAPI_ISteamFriends_SetRichPresence", set_rp_) &&
              get(m, "SteamAPI_ISteamFriends_GetFriendRichPresence", get_rp_) && get(m, "SteamAPI_ISteamFriends_RequestFriendRichPresence", req_rp_) &&
              get(m, "SteamAPI_ISteamFriends_GetFriendCount", friend_count_) && get(m, "SteamAPI_ISteamFriends_GetFriendByIndex", friend_at_) &&
              get(m, "SteamAPI_ISteamFriends_GetFriendGamePlayed", friend_game_) && get(m, "SteamAPI_ISteamFriends_GetFriendPersonaName", friend_name_);
    if (!ok) { why = "this steam_api64.dll lacks a function the Steam link needs (expected SDK 1.57)"; return false; }
    if (!hsteamuser()) { why = "Steam is not initialised in this game"; return false; }
    void *msgs = acc_msgs(), *user = acc_user(), *utils = acc_utils();
    friends_ = acc_friends(), netutils_ = acc_net();
    if (!msgs || !friends_ || !user || !utils || !netutils_) { why = "a Steam interface is missing"; return false; }
    me_ = user_id(user), app_ = appid(utils);
    init_relay(netutils_);  // starts measuring the relay network now, so the first session connects faster
    // Faster hand-overs: the default send rate (256 KB/s) took 7 s for a 2 MB campaign in the two-PC test. Each value
    // is set only when the id has the expected name in this steam_api64.dll.
    const char* (*cfg_info)(void*, int, int*, int*) = nullptr;
    bool (*cfg_set)(void*, int, int32_t) = nullptr;
    if (get(m, "SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo", cfg_info) && get(m, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32", cfg_set)) {
        const std::pair<int, const char*> ids[] = {{9, "SendBufferSize"}, {10, "SendRateMin"}, {11, "SendRateMax"}};
        const int32_t values[] = {8 << 20, 1 << 20, 1 << 20};  // 8 MB buffer, 1 MB/s
        for (int i = 0; i < 3; i++) {
            int type = 0, scope = 0;
            const char* n = cfg_info(netutils_, ids[i].first, &type, &scope);
            if (n && std::string(n) == ids[i].second && cfg_set(netutils_, ids[i].first, values[i])) rate_set_++;
        }
    }
    msgs_ = msgs;
    set_session_cb(netutils_, on_session_request);
    bool lobbies = start_lobbies(m);
    why = "Steam link ready (app " + std::to_string(app_) + (rate_set_ == 3 ? ", 1 MB/s" : ", default send rate") +
          (lobbies ? ", public games" : ", no public games") + ")";
    return true;
}

bool on() { return msgs_ != nullptr; }
uint64_t me() { return me_; }
uint32_t app_id() { return app_; }

int send(uint64_t to, const void* data, int size, bool reliable, int channel) {
    if (!msgs_ || !to || size <= 0) return 0;
    // no Nagle delay; a broken session restarts by itself instead of failing the send
    return send_(msgs_, id_of(to), data, static_cast<uint32_t>(size), (reliable ? 8 : 0) | 1 | 32, channel);
}

void receive(int channel, const std::function<void(uint64_t, const char*, int)>& fn) {
    if (!msgs_) return;
    for (int round = 0; round < 16; round++) {
        Message* got[32];
        int n = recv_(msgs_, channel, got, 32);
        if (n <= 0) return;
        std::vector<std::pair<uint64_t, std::string>> batch;  // copied and released first: fn may throw
        for (int i = 0; i < n; i++) {
            if (got[i]->data && got[i]->size > 0)
                batch.emplace_back(get_id_(&got[i]->peer), std::string(static_cast<const char*>(got[i]->data), static_cast<size_t>(got[i]->size)));
            release_(got[i]);
        }
        for (auto& b : batch) fn(b.first, b.second.data(), static_cast<int>(b.second.size()));
        if (n < 32) return;
    }
}

void set_presence(const char* key, const char* value) {
    if (friends_) set_rp_(friends_, key, value);
}

std::string presence(uint64_t user, const char* key) {
    const char* v = friends_ ? get_rp_(friends_, user, key) : nullptr;
    return v ? v : "";
}

void request_presence(uint64_t user) {
    if (friends_) req_rp_(friends_, user);
}

std::vector<uint64_t> friends_in_game() {
    std::vector<uint64_t> out;
    if (!friends_) return out;
    int n = friend_count_(friends_, kImmediateFriends);
    for (int i = 0; i < n; i++) {
        uint64_t f = friend_at_(friends_, i, kImmediateFriends);
        FriendGameInfo gi{};
        if (friend_game_(friends_, f, &gi) && (gi.game_id & 0xFFFFFFu) == app_) out.push_back(f);  // CGameID: app id in the low 24 bits
    }
    return out;
}

uint64_t find_friend(const std::string& name_or_id) {
    if (!friends_ || name_or_id.empty()) return 0;
    int n = friend_count_(friends_, kImmediateFriends);
    for (int i = 0; i < n; i++) {
        uint64_t f = friend_at_(friends_, i, kImmediateFriends);
        const char* nm = friend_name_(friends_, f);
        if (name_or_id == std::to_string(f) || (nm && name_or_id == nm)) return f;
    }
    return 0;
}

std::string name(uint64_t user) {
    const char* v = friends_ ? friend_name_(friends_, user) : nullptr;
    return v ? v : "";
}

// A pending session from any immediate friend is accepted (AcceptSessionWithUser is a no-op without one). First two-PC
// test (beta.3): the friend's attempts timed out every 10 s, the host never saw its presence nor got the callback.
int accept_friends() {
    if (!msgs_ || !friends_) return 0;
    static std::set<uint64_t> told;
    int n = friend_count_(friends_, kImmediateFriends);
    for (int i = 0; i < n; i++) {
        uint64_t f = friend_at_(friends_, i, kImmediateFriends);
        if (accept_(msgs_, id_of(f)) && told.insert(f).second) core_log("steam: accepted a session from " + name(f));
    }
    return n;
}

// ---------------------------------------------------------------- public games and "Join game"
namespace {
#pragma pack(push, 8)
struct LobbyCreated { int32_t result; uint64_t lobby; };                                   // LobbyCreated_t 513
struct LobbyMatchList { uint32_t count; };                                                 // LobbyMatchList_t 510
struct LobbyEnter { uint64_t lobby; uint32_t perms; bool locked; uint32_t response; };     // LobbyEnter_t 504
struct LobbyJoinRequested { uint64_t lobby, friend_id; };                                  // GameLobbyJoinRequested_t 333
struct PresenceJoinRequested { uint64_t friend_id; char connect[256]; };                   // GameRichPresenceJoinRequested_t 337
#pragma pack(pop)
static_assert(sizeof(LobbyCreated) == 16 && sizeof(LobbyEnter) == 24 && sizeof(PresenceJoinRequested) == 264, "Steam callback layouts");

void *mm_, *utils_;
uint64_t (*create_lobby_)(void*, int, int);
uint64_t (*join_lobby_)(void*, uint64_t);
void (*leave_lobby_)(void*, uint64_t);
uint64_t (*request_list_)(void*);
void (*filter_string_)(void*, const char*, const char*, int);
void (*filter_distance_)(void*, int);
void (*filter_count_)(void*, int);
void (*filter_slots_)(void*, int);
uint64_t (*lobby_at_)(void*, int);
const char* (*lobby_get_)(void*, uint64_t, const char*);
bool (*lobby_put_)(void*, uint64_t, const char*, const char*);
uint64_t (*lobby_owner_)(void*, uint64_t);
int (*lobby_count_)(void*, uint64_t);
uint64_t (*lobby_member_)(void*, uint64_t, int);
bool (*call_done_)(void*, uint64_t, bool*);
bool (*call_result_)(void*, uint64_t, void*, int, int, bool*);
void (*register_cb_)(void*, int);
uint64_t creating_ = 0, searching_ = 0, joining_ = 0, lobby_ = 0, joined_owner_ = 0, join_request_ = 0, lobby_request_ = 0;
double last_search_ = -100;
std::vector<Lobby> found_;
std::string want_version_, want_name_;
bool hosting_lobby_ = false;

double now_s() { return GetTickCount64() / 1000.0; }

// SteamAPI_RegisterCallback takes a CCallbackBase of the SDK's (MSVC) layout: a vtable of Run(void*, bool, call) and
// Run(void*) (MSVC orders the two overloads in reverse) and GetCallbackSizeBytes(), then the flags and the callback id.
// Both Run slots go to one function: the callback's data is the first argument either way.
struct CallbackObj {
    void** vtable;
    uint8_t flags;
    int32_t id;
};
static_assert(offsetof(CallbackObj, flags) == 8 && offsetof(CallbackObj, id) == 12, "CCallbackBase layout");
void on_callback(CallbackObj* self, void* data) {  // the game's SteamAPI_RunCallbacks (game thread)
    if (self->id == 337) {
        auto* r = static_cast<PresenceJoinRequested*>(data);
        std::string c(r->connect, strnlen(r->connect, sizeof r->connect));
        size_t at = c.find("-tow2coop_join=");
        uint64_t host = at == std::string::npos ? 0 : strtoull(c.c_str() + at + 15, nullptr, 10);
        core_log("steam: \"Join game\" from the friends list for " + name(r->friend_id) + (host ? "" : " (not a co-op game)"));
        if (host) join_request_ = host;
    } else if (self->id == 333) {
        auto* r = static_cast<LobbyJoinRequested*>(data);
        core_log("steam: a lobby invite from " + name(r->friend_id) + " accepted");
        lobby_request_ = r->lobby;
    }
}
int callback_size(CallbackObj* self) { return self->id == 337 ? static_cast<int>(sizeof(PresenceJoinRequested)) : static_cast<int>(sizeof(LobbyJoinRequested)); }
void* callback_vtable[3] = {reinterpret_cast<void*>(on_callback), reinterpret_cast<void*>(on_callback), reinterpret_cast<void*>(callback_size)};
CallbackObj join_cb_ = {callback_vtable, 0, 337}, invite_cb_ = {callback_vtable, 0, 333};

template <class T>
bool done(uint64_t& call, int id, T& out) {  // a pending call's result, once
    bool failed = false;
    if (!call || !call_done_(utils_, call, &failed)) return false;
    bool ok = !failed && call_result_(utils_, call, &out, static_cast<int>(sizeof out), id, &failed) && !failed;
    call = 0;
    return ok;
}
}  // namespace

bool lobbies_on() { return mm_ != nullptr; }

bool start_lobbies(HMODULE m) {  // from start(): optional, the friends' co-op works without it
    void* (*acc_mm)() = nullptr;
    void* (*acc_utils)() = nullptr;
    bool ok = get(m, "SteamAPI_SteamMatchmaking_v009", acc_mm) && get(m, "SteamAPI_SteamUtils_v010", acc_utils) &&
              get(m, "SteamAPI_ISteamMatchmaking_CreateLobby", create_lobby_) && get(m, "SteamAPI_ISteamMatchmaking_JoinLobby", join_lobby_) &&
              get(m, "SteamAPI_ISteamMatchmaking_LeaveLobby", leave_lobby_) && get(m, "SteamAPI_ISteamMatchmaking_RequestLobbyList", request_list_) &&
              get(m, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter", filter_string_) &&
              get(m, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter", filter_distance_) &&
              get(m, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter", filter_count_) &&
              get(m, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable", filter_slots_) &&
              get(m, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex", lobby_at_) && get(m, "SteamAPI_ISteamMatchmaking_GetLobbyData", lobby_get_) &&
              get(m, "SteamAPI_ISteamMatchmaking_SetLobbyData", lobby_put_) && get(m, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", lobby_owner_) &&
              get(m, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", lobby_count_) &&
              get(m, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", lobby_member_) &&
              get(m, "SteamAPI_ISteamUtils_IsAPICallCompleted", call_done_) && get(m, "SteamAPI_ISteamUtils_GetAPICallResult", call_result_) &&
              get(m, "SteamAPI_RegisterCallback", register_cb_);
    if (!ok) return false;
    utils_ = acc_utils(), mm_ = acc_mm();
    if (!mm_ || !utils_) { mm_ = nullptr; return false; }
    register_cb_(&join_cb_, 337);
    register_cb_(&invite_cb_, 333);
    return true;
}

void lobby_poll() {
    if (!mm_) return;
    LobbyCreated c{};
    if (creating_ && done(creating_, 513, c)) {
        if (c.result == 1 && c.lobby) {
            lobby_ = c.lobby;
            if (!hosting_lobby_) { leave_lobby_(mm_, lobby_); lobby_ = 0; }  // hosting stopped meanwhile
            else {
                lobby_put_(mm_, lobby_, "tow2coop", want_version_.c_str());
                lobby_put_(mm_, lobby_, "host", want_name_.c_str());
                core_log("steam: public game open (lobby " + std::to_string(lobby_) + ")");
            }
        } else core_log("steam: the public game could not be made (result " + std::to_string(c.result) + ")");
    }
    LobbyMatchList l{};
    if (searching_ && done(searching_, 510, l)) {
        found_.clear();
        for (uint32_t i = 0; i < l.count && i < 20; i++) {
            uint64_t id = lobby_at_(mm_, static_cast<int>(i));
            uint64_t owner = lobby_owner_(mm_, id);
            if (!id || owner == me_) continue;
            const char* h = lobby_get_(mm_, id, "host");
            found_.push_back({id, owner, h ? std::string(h).substr(0, 48) : std::string()});
        }
    }
    LobbyEnter e{};
    if (joining_ && done(joining_, 504, e)) {
        if (e.response == 1) lobby_ = e.lobby, joined_owner_ = lobby_owner_(mm_, e.lobby);  // k_EChatRoomEnterResponseSuccess
        else core_log("steam: could not enter the public game (" + std::to_string(e.response) + ")");
    }
    if (lobby_request_) {  // an invite accepted from Steam: enter it, then its owner is the host to join
        uint64_t l2 = lobby_request_;
        lobby_request_ = 0;
        lobby_join(l2);
    }
}

void lobby_host(const std::string& version, const std::string& host_name) {
    want_version_ = version, want_name_ = host_name, hosting_lobby_ = true;
    if (!mm_ || lobby_ || creating_) return;
    creating_ = create_lobby_(mm_, 3, 2);  // k_ELobbyTypeInvisible, two players
}

void lobby_leave() {
    hosting_lobby_ = false, joined_owner_ = 0;
    if (mm_ && lobby_) leave_lobby_(mm_, lobby_);
    lobby_ = 0;
}

uint64_t lobby_id() { return lobby_; }

void lobby_set(const char* key, const char* value) {
    if (mm_ && lobby_ && hosting_lobby_) lobby_put_(mm_, lobby_, key, value);
}

void lobby_accept_members() {
    if (!mm_ || !lobby_ || !msgs_) return;
    int n = lobby_count_(mm_, lobby_);
    for (int i = 0; i < n && i < 8; i++) {
        uint64_t u = lobby_member_(mm_, lobby_, i);
        if (u && u != me_) accept_(msgs_, id_of(u));
    }
}

void lobby_search(const std::string& version) {
    if (!mm_ || searching_ || now_s() - last_search_ < 3.0) return;
    last_search_ = now_s();
    filter_string_(mm_, "tow2coop", version.c_str(), 0);  // k_ELobbyComparisonEqual: the same version only
    filter_slots_(mm_, 1);
    filter_distance_(mm_, 3);  // k_ELobbyDistanceFilterWorldwide
    filter_count_(mm_, 20);
    searching_ = request_list_(mm_);
}

std::vector<Lobby> lobby_results() { return found_; }

void lobby_join(uint64_t lobby) {
    if (!mm_ || !lobby) return;
    if (lobby_) leave_lobby_(mm_, lobby_), lobby_ = 0;
    hosting_lobby_ = false, joined_owner_ = 0;
    joining_ = join_lobby_(mm_, lobby);
}

uint64_t lobby_joined() {
    uint64_t o = joined_owner_;
    joined_owner_ = 0;
    return o;
}

uint64_t take_join_request() {
    uint64_t h = join_request_;
    join_request_ = 0;
    return h;
}

int session_state(uint64_t peer) { return msgs_ && peer ? state_(msgs_, id_of(peer), nullptr, nullptr) : 0; }
int relay_status() { return netutils_ ? relay_(netutils_, nullptr) : 0; }
void close(uint64_t peer) {
    if (msgs_ && peer) close_(msgs_, id_of(peer));
}
}  // namespace steamnet
