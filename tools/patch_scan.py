"""One-off source patch for core/coop.cpp: doors and containers come from the incremental object scan (no full scans)."""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")


def rep(old, new):
    global s
    assert old in s, old[:90]
    s = s.replace(old, new, 1)


# state for the incremental interactable scan
rep('''    std::unordered_map<uint64_t, ue::UObject*> interactables;''', '''    std::unordered_map<uint64_t, ue::UObject*> interactables, inter_scan;  // doors + containers by path id
    ue::UClass *door_class = nullptr, *container_class = nullptr;''')

# scan_step: collect doors/containers too; swap at pass end
rep('''        if (!o || o == pawn || o == g.puppet || ue::is_cdo(o) || !ue::is_a(o, g.character_class) || !ue::alive(o)) continue;''',
    '''        if (!o || o == pawn || o == g.puppet || ue::is_cdo(o)) continue;
        if ((g.door_class && ue::is_a(o, g.door_class)) || (g.container_class && ue::is_a(o, g.container_class))) {
            if (ue::alive(o)) g.inter_scan[fnv64(ue::path_of(o))] = o;
            continue;
        }
        if (!ue::is_a(o, g.character_class) || !ue::alive(o)) continue;''')
rep('''    if (!g.character_class) g.character_class = ue::find_class("/Script/Engine.Character");''',
    '''    if (!g.character_class) g.character_class = ue::find_class("/Script/Engine.Character");
    if (!g.door_class) g.door_class = ue::find_class("/Script/Arkansas.Door");
    if (!g.container_class) g.container_class = ue::find_class("/Script/Arkansas.Container");''')
rep('''    if (end >= total) {
        g.npcs.swap(g.scanning);
        g.scanning.clear();
        g.scan_cursor = 0;
    }''', '''    if (end >= total) {
        g.npcs.swap(g.scanning);
        g.scanning.clear();
        g.interactables.swap(g.inter_scan);
        g.inter_scan.clear();
        g.scan_cursor = 0;
        g.interactables_built = g.now;
    }''')
s_before = s
# interactable(): no rebuild, just lookup
start = s.index("ue::UObject* interactable(uint64_t id) {")
end = s.index("\n}\n", start) + 3
s = s[:start] + '''ue::UObject* interactable(uint64_t id) {  // filled by the incremental scan (scan_step)
    auto it = g.interactables.find(id);
    return it != g.interactables.end() && ue::alive(it->second) ? it->second : nullptr;
}
''' + s[end:]
# poll_doors: rebuild door list from interactables after each scan pass instead of instances()
rep('''    if (g.now - doors_built > 5.0) {  // rebuild: World Partition streams doors in and out
        std::vector<DoorRec> fresh;
        if (ue::UClass* c = ue::find_class("/Script/Arkansas.Door")) {
            for (ue::UObject* o : ue::instances(c, false, 5000)) {''', '''    if (g.interactables_built > doors_built) {  // a scan pass finished: World Partition streams doors in and out
        std::vector<DoorRec> fresh;
        if (g.door_class) {
            for (auto& kv : g.interactables) {
                ue::UObject* o = kv.second;
                if (!ue::alive(o) || !ue::is_a(o, g.door_class)) continue;''')
rep('''        doors.swap(fresh);
        door_cursor = 0;
        doors_built = g.now;''', '''        doors.swap(fresh);
        door_cursor = 0;
        doors_built = g.interactables_built;''')
p.write_text(s, encoding="utf-8", newline="\n")
print("patched", p)
