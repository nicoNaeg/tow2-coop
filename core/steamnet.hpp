// The co-op link over Steam, through the game's own Steam session (the game loads and initialises steam_api64.dll).
// Messages go by SteamID through Valve's relay (ISteamNetworkingMessages): no IP address, no open port, encrypted.
// Players find each other with rich presence (key "tow2coop"): a host shows "host", a client that picked a host shows
// "join:<host SteamID>". Each side sends to the other first, which accepts the other's session; a host without a client
// also accepts any pending session from a friend (a client with private game details never shows its presence).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace steamnet {
bool start(std::string& why);  // resolve steam_api64.dll's flat functions, warm up the relay; false: not available
bool on();
uint64_t me();
uint32_t app_id();
int send(uint64_t to, const void* data, int size, bool reliable, int channel);  // EResult: 1 OK, 25 send buffer full
void receive(int channel, const std::function<void(uint64_t from, const char* data, int size)>& fn);
void set_presence(const char* key, const char* value);
std::string presence(uint64_t user, const char* key);
void request_presence(uint64_t user);
std::vector<uint64_t> friends_in_game();  // immediate friends running this app now
uint64_t find_friend(const std::string& name_or_id);  // an immediate friend by Steam name or SteamID64; 0 = none
std::string name(uint64_t user);
int accept_friends();              // accept a pending session from any immediate friend; returns the friends checked
int session_state(uint64_t peer);  // ESteamNetworkingConnectionState: 1 connecting, 2 finding route, 3 connected
int relay_status();                // ESteamNetworkingAvailability: 100 current, 3 attempting, <0 failed
void close(uint64_t peer);
}  // namespace steamnet
