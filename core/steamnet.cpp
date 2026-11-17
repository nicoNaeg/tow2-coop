// Steamworks flat API of the game's steam_api64.dll (SDK 1.57, folder Steamv157), resolved by name at run time.
#include "steamnet.hpp"
#include <windows.h>
#include <cstddef>
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
    msgs_ = msgs;
    set_session_cb(netutils_, on_session_request);
    why = "Steam link ready (app " + std::to_string(app_) + ")";
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

int session_state(uint64_t peer) { return msgs_ && peer ? state_(msgs_, id_of(peer), nullptr, nullptr) : 0; }
int relay_status() { return netutils_ ? relay_(netutils_, nullptr) : 0; }
void close(uint64_t peer) {
    if (msgs_ && peer) close_(msgs_, id_of(peer));
}
}  // namespace steamnet
