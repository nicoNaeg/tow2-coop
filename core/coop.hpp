#pragma once
#include <string>
#include <coopbridge/json.h>

#include <atomic>
#include <cstdint>

namespace coop {
// The game's UPlayerInfoComponent::AddXP(XP, EXPGainReason, TArray* details): hooked in main.cpp (party XP).
using AddXpFn = bool (*)(void* self, int32_t xp, uint8_t reason, void* details);
extern AddXpFn add_xp_original, add_xp_entry;  // the original (no hook) and the hooked entry (as the game calls it)
bool xp_blocked(void* self, uint8_t reason);          // game thread, before an XP gain: drop it?
void on_xp(void* self, int32_t xp, uint8_t reason);   // game thread, after a gain: share it?
coopbridge::json::Value xp_test(int xp, int reason);  // test bridge: an XP gain through the hooked AddXP
// UFunctions whose ProcessEvent calls the hook hands to observe() (door state changes, container item removals).
extern std::atomic<int> watch_count;
extern void* watch[8];
void observe(void* object, void* function, const void* parms);  // any thread; only copies, frame() does the work
bool start(std::string& why);  // reads TOW2_COOP_* from the environment; false = co-op stays off
void frame(float dt);          // game thread, once per frame
coopbridge::json::Value status();
coopbridge::json::Value set_options(const coopbridge::json::Value& p);  // {story,doors,npcs,scan: bool}
coopbridge::json::Value steam_probe();
coopbridge::json::Value face_debug();  // game thread: the look we send, the one we got, the last error (test bridge)  // any thread, blocks up to ~15 s: Steam link self-test without another player
}  // namespace coop
