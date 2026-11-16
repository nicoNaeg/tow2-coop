"""One-off source patch: door sync by polling OCLComponent state (the state-change delegate is native, not ProcessEvent)."""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")

# 1. only container removals are watched by name now
old = '''    uint32_t a = ue::make_name("OnOCLStateChange", false).index, b = ue::make_name("HandleItemRemoved", false).index;
    if (!a || !b) return;
    watch[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(a));
    watch[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(b));
    watch_count.store(2, std::memory_order_release);'''
new = '''    uint32_t b = ue::make_name("HandleItemRemoved", false).index;  // door state changes go through a native delegate: polled
    if (!b) return;
    watch[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(b));
    watch_count.store(1, std::memory_order_release);'''
assert old in s
s = s.replace(old, new, 1)

# 2. door registry + polling, before apply_interact
old = "void call_on(ue::UObject* o, const char* fn, const json& args) {"
new = r'''struct DoorRec {
    ue::UObject *actor = nullptr, *ocl = nullptr;
    uint64_t id = 0;
    int last = -1;            // last polled settled state
    int remote = -1;          // state we just applied from the other world (its poll must not echo back)
};
std::vector<DoorRec> doors;
size_t door_cursor = 0;
double doors_built = -100;

int door_state(ue::UObject* ocl) {
    static ue::UFunction* f;
    static ue::UObject* cls;
    if (!f || cls != ue::class_of(ocl)) {
        cls = ue::class_of(ocl);
        f = ue::find_func(cls, "GetCurrentState");
        if (!f) return -1;
    }
    alignas(16) uint8_t b[16] = {};
    ue::process_event(ocl, f, b);
    return b[0];  // EOCLState is the only (return) parameter
}

void poll_doors() {
    if (g.now - doors_built > 5.0) {  // rebuild: World Partition streams doors in and out
        std::vector<DoorRec> fresh;
        if (ue::UClass* c = ue::find_class("/Script/Arkansas.Door")) {
            for (ue::UObject* o : ue::instances(c, false, 5000)) {
                std::string err;
                ue::UObject* ocl = ue::resolve(ue::addr(o) + ".OCLComponent", &err);
                if (!ocl) continue;
                DoorRec r;
                r.actor = o, r.ocl = ocl, r.id = fnv64(ue::path_of(o));
                for (auto& old : doors)
                    if (old.actor == o) { r.last = old.last, r.remote = old.remote; break; }
                fresh.push_back(r);
            }
        }
        doors.swap(fresh);
        door_cursor = 0;
        doors_built = g.now;
    }
    for (int k = 0; k < 16 && !doors.empty(); k++) {
        DoorRec& d = doors[door_cursor];
        door_cursor = (door_cursor + 1) % doors.size();
        if (!ue::alive(d.actor) || !ue::alive(d.ocl)) continue;
        int st = door_state(d.ocl);
        if (st < 0 || st > 2) continue;  // only settled Closed/Open/Locked
        if (d.last >= 0 && st != d.last) {
            if (st == d.remote) d.remote = -1;  // our own apply arriving: no echo
            else {
                queue_interact(1, d.id, st, "");
                g.doors_sent++;
                g.last_interact = "sent door " + ue::name_of(d.actor) + " -> " + std::to_string(st);
            }
        }
        d.last = st;
    }
}

void note_remote_door(ue::UObject* actor, int state) {
    for (auto& d : doors)
        if (d.actor == actor) d.remote = state;
}

void call_on(ue::UObject* o, const char* fn, const json& args) {'''
assert old in s
s = s.replace(old, new, 1)

# 3. remember applied door states
old = '''                if (cur.asString() != want) {
                    json a = json::object();
                    a.set("bIgnoreLockStatus", true).set("bForce", true);'''
new = '''                if (cur.asString() != want) {
                    note_remote_door(actor, m.a);
                    json a = json::object();
                    a.set("bIgnoreLockStatus", true).set("bForce", true);'''
assert old in s
s = s.replace(old, new, 1)

# 4. poll in the frame
old = "        setup_watches();\n        process_observed();"
new = "        setup_watches();\n        process_observed();\n        poll_doors();"
assert old in s
s = s.replace(old, new, 1)
p.write_text(s, encoding="utf-8", newline="\n")
print("patched", p)
