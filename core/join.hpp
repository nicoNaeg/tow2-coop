#pragma once
#include <cstdint>
#include <string>
#include <coopbridge/json.h>

namespace join {
// allow_ip: only a connection from this address gets the save ("" = the first one, test bridge only)
coopbridge::json::Value host_offer(int port, const std::string& bind_ip, const std::string& allow_ip = "");  // game thread
coopbridge::json::Value client_fetch(const std::string& host, int port, uint32_t host_id);    // game thread
void frame(uint64_t frames);                                                                                  // game thread
coopbridge::json::Value status();                                                              // any thread
}  // namespace join
