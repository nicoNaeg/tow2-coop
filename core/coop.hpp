#pragma once
#include <string>
#include <coopbridge/json.h>

#include <atomic>

namespace coop {
// UFunctions whose ProcessEvent calls the hook hands to observe() (door state changes, container item removals).
extern std::atomic<int> watch_count;
extern void* watch[8];
void observe(void* object, void* function, const void* parms);  // any thread; only copies, frame() does the work
bool start(std::string& why);  // reads TOW2_COOP_* from the environment; false = co-op stays off
void frame(float dt);          // game thread, once per frame
coopbridge::json::Value status();
coopbridge::json::Value set_options(const coopbridge::json::Value& p);  // {story,doors,npcs,scan: bool}
coopbridge::json::Value steam_probe();  // any thread, blocks up to ~15 s: Steam link self-test without another player
}  // namespace coop
