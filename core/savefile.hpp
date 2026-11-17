// The game's save files (SaveGame.dat): one zlib stream holding an "SGDF" container of named entries (one per level,
// plus Player.dat, Quests.dat, ...). Player.dat is the character: inventory with each item's state, equipment, money,
// level, perks, appearance, position (probe 61: spliced into another save, the game loads that world with this
// character). Metadata.dat holds the container's unpacked size, which the loader checks.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace savefile {
bool inflate(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err);  // zlib stream
std::vector<uint8_t> deflate_stored(const std::vector<uint8_t>& in);  // a valid zlib stream of stored blocks
uint32_t adler32(const uint8_t* p, size_t n);

// The Player.dat entry of the save in `dir` (a save folder).
bool read_player(const std::wstring& dir, std::vector<uint8_t>& player, std::string& err);
// Puts `player` into the save in `dir`, with the save's own position and rotation (the character appears where the
// save's player stood), and updates the unpacked size in Metadata.dat.
bool splice_player(const std::wstring& dir, std::vector<uint8_t> player, std::string& err);

bool read_file(const std::wstring& path, std::vector<uint8_t>& out);
bool write_file(const std::wstring& path, const std::vector<uint8_t>& data);  // through a temporary file
}  // namespace savefile
