#pragma once
#include <cstdint>
#include <functional>
#include <coopbridge/json.h>

namespace story {
constexpr uint16_t kStory = 6, kStoryAck = 7;  // co-op packet types
using SendFn = std::function<void(uint16_t type, const void* body, int len)>;
bool ensure_loaded();                                           // game thread
void reset_peer();                                              // host: a client (re)joined
void host_frame(double now, const SendFn& send);                // host, game thread, settled gameplay only
void on_story(const char* body, int len, const SendFn& send);   // client, game thread
void on_ack(const char* body, int len);                         // host
void client_reset();                                            // client: world reloaded from a save
void client_frame(double now, const SendFn& send);              // client, game thread
bool take_resync_request();                                     // client: an outcome could not be applied
coopbridge::json::Value status();
}  // namespace story
