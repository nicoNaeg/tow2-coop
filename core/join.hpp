#pragma once
#include <cstdint>
#include <string>
#include <coopbridge/json.h>

namespace join {
// allow_ip: only a connection from this address gets the save ("" = the first one, test bridge only).
// steam_to: push the save to this SteamID over Steam instead of serving TCP (host_id names the client's folder).
// existing: a save folder the host's game is loading right now (complete on disk): sent as it is, no new save.
coopbridge::json::Value host_offer(int port, const std::string& bind_ip, const std::string& allow_ip = "", uint64_t steam_to = 0,
                                   uint32_t host_id = 0, const std::wstring& existing = L"");  // game thread
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
void refuse_loads(int n);     // test: the next n loads of a received campaign count as refused by the game
void hold(bool on);           // a received campaign waits (the friend died: it loads when they continue)
bool held();
void discard();               // a held campaign is dropped (the friend was revived)
coopbridge::json::Value status();                                                              // any thread
}  // namespace join
