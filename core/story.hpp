#pragma once
#include <cstdint>
#include <functional>
#include <coopbridge/json.h>

namespace story {
constexpr uint16_t kStory = 6, kStoryAck = 7;  // co-op packet types
using SendFn = std::function<void(uint16_t type, const void* body, int len)>;
bool ensure_loaded();                                           // game thread
void reset_peer();                                              // host: a client (re)joined
uint64_t forced_completions();  // quests set Completed here by the other side's outcome (this quest graph not there)
void host_frame(double now, const SendFn& send);                // host, game thread, settled gameplay only
void on_story(const char* body, int len, const SendFn& send);   // either side, game thread: the other side's changes
void on_ack(const char* body, int len);                         // either side: how far the other side applied ours
void client_reset();                                            // client: world reloaded from a save
void client_frame(double now, const SendFn& send);              // client, game thread, settled gameplay only (also
                                                                // watches the client's own changes and sends them)
bool take_resync_request();                                     // client: an outcome could not be applied
// The game's quest lookup (UQuestManager, found in main.cpp; nullptr when this build differs): the quest instance of a
// GUID, its state (EQuestState) at +0x1A4. With it the client applies quest outcomes itself instead of reloading.
using FindQuestFn = uint8_t* (*)(void* quest_manager, const void* guid);
extern FindQuestFn find_quest;
constexpr int kQuestStateOffset = 0x1A4;
coopbridge::json::Value status();
}  // namespace story
