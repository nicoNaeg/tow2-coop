// The joined player's own character (end goal 2). Every hand-over loads the host's campaign, which holds the host's
// character; the client first puts its own Player.dat into it (savefile::splice_player), so it keeps its own items,
// money, level, perks and look, and appears where the host stands. The character is kept per host in
// <LocalAppData>\tow2coop\characters\<host>.player, refreshed by a private "CoopSelf" save (before every hand-over and
// every few minutes). First join: the player's own newest save (their own character), with the inventory emptied.
#pragma once
#include <string>
#include <coopbridge/json.h>

namespace character {
void set_host(const std::string& key);  // "steam-<SteamID>" or "direct"; game thread
// Game thread, before loading a received campaign in save folder `dir`: false while the player's own character is
// still being saved (a few seconds); then splices it in and returns true (also on failure: the host's copy loads).
bool before_load(const std::wstring& dir);
void loaded();                           // game thread: the hand-over LoadGame succeeded
void frame(double now, bool joined_gameplay);  // game thread, every frame; joined_gameplay: settled in a joined world
void save_now();                         // game thread: start a private save now (test bridge)
coopbridge::json::Value status();        // any thread
}  // namespace character
