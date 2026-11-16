"""One-off source patch for core/coop.cpp: auto-join only from the main menu; drop door/container pointers on a map change."""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")


def rep(old, new):
    global s
    assert s.count(old) == 1, old[:90]
    s = s.replace(old, new, 1)


rep("    double start_time = 0, next_join_request = 0;", "    double start_time = 0, next_join_request = 0, map_since = 0;\n    bool at_menu = false;")
rep("        g.npcs.clear(), g.scanning.clear(), g.scan_cursor = 0;\n        g.map_changes++;",
    "        g.npcs.clear(), g.scanning.clear(), g.scan_cursor = 0;\n"
    "        g.interactables.clear(), g.inter_scan.clear();  // old world's doors and containers\n"
    "        doors.clear(), door_cursor = 0;\n"
    "        g.at_menu = world && ue::path_of(world).find(\"MainMenu\") != std::string::npos;\n"
    "        g.map_since = g.now;\n"
    "        g.map_changes++;")
rep("    if (!g.host && !gameplay && g.now - g.start_time > 60.0 && g.now >= g.next_join_request && g.now >= g.fetch_quiet_until + 90.0) {",
    "    if (!g.host && g.at_menu && g.now - g.map_since > 30.0 && g.now - g.start_time > 60.0 && g.now >= g.next_join_request &&\n"
    "        g.now >= g.fetch_quiet_until + 90.0) {  // never from a loading screen: the client may be travelling")
rep('.set("joinRequests", static_cast<double>(g.join_requests))', '.set("joinRequests", static_cast<double>(g.join_requests)).set("atMenu", g.at_menu)')
p.write_text(s, encoding="utf-8", newline="\n")
print("patched", p)
