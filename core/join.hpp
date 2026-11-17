#pragma once
#include <cstdint>
#include <string>
#include <coopbridge/json.h>

namespace join {
// allow_ip: only a connection from this address gets the save ("" = the first one, test bridge only).
// steam_to: push the save to this SteamID over Steam instead of serving TCP (host_id names the client's folder).
coopbridge::json::Value host_offer(int port, const std::string& bind_ip, const std::string& allow_ip = "", uint64_t steam_to = 0,
                                   uint32_t host_id = 0);  // game thread
void block_saves(bool block);                  // game thread: SaveGameManager.DisableSaves (a joined player never saves)
bool loading();                                // game thread: a LoadGame this core started has not reached its new world yet
void client_expect(uint32_t host_id);          // Steam client, game thread: the host announced a hand-over
void on_steam_data(const char* data, int n);   // Steam client, game thread: one message of the hand-over (channel 1)
coopbridge::json::Value client_fetch(const std::string& host, int port, uint32_t host_id);    // game thread
void frame(uint64_t frames);                                                                                  // game thread
std::wstring saves_folder();  // <Saved Games>\TheOuterWorlds2 as the game sees it (a test copy: its sandbox)
std::wstring local_folder();  // <LocalAppData> as the game sees it
bool joined();                // the world came from a hand-over (cleared at the main menu): saves stay off for the player
void set_joined(bool on);
coopbridge::json::Value status();                                                              // any thread
}  // namespace join
