"""One-off source patch: runtime switches for each sync part (bridge method coop.set), for memory/CPU bisection."""
import sys
from pathlib import Path

core = Path(sys.argv[1])


def patch(name, pairs):
    p = core / name
    s = p.read_text(encoding="utf-8")
    for old, new in pairs:
        assert s.count(old) == 1, (name, old[:80])
        s = s.replace(old, new, 1)
    p.write_text(s, encoding="utf-8", newline="\n")


patch("coop.cpp", [
    ("    bool at_menu = false;", "    bool at_menu = false;\n    bool opt_story = true, opt_doors = true, opt_npcs = true, opt_scan = true;  // coop.set"),
    ("        poll_doors();\n", "        if (g.opt_doors) poll_doors();\n"),
    ("        scan_step(pawn);\n", "        if (g.opt_scan) scan_step(pawn);\n"),
    ("        if (g.host) story::host_frame(g.now, send_typed);", "        if (g.host && g.opt_story) story::host_frame(g.now, send_typed);"),
    ("        if (g.host && g.have_peer && g.now >= g.next_npc_send) {", "        if (g.host && g.opt_npcs && g.have_peer && g.now >= g.next_npc_send) {"),
    ("json status() {\n    json s = json::object();",
     "json set_options(const json& p) {\n"
     "    for (auto& kv : {std::make_pair(\"story\", &g.opt_story), std::make_pair(\"doors\", &g.opt_doors), std::make_pair(\"npcs\", &g.opt_npcs), std::make_pair(\"scan\", &g.opt_scan)})\n"
     "        if (p.get(kv.first).isBool()) *kv.second = p.get(kv.first).asBool(true);\n"
     "    json s = json::object();\n"
     "    s.set(\"story\", g.opt_story).set(\"doors\", g.opt_doors).set(\"npcs\", g.opt_npcs).set(\"scan\", g.opt_scan);\n"
     "    return s;\n}\n\n"
     "json status() {\n    json s = json::object();"),
])
patch("coop.hpp", [("coopbridge::json::Value status();", "coopbridge::json::Value status();\ncoopbridge::json::Value set_options(const coopbridge::json::Value& p);  // {story,doors,npcs,scan: bool}")])
patch("main.cpp", [('    reg("coop.status", [](const Value&) { return coop::status(); });',
                    '    reg("coop.status", [](const Value&) { return coop::status(); });\n    reg("coop.set", [](const Value& p) { return coop::set_options(p); });')])
print("patched")
