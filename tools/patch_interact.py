"""One-off source patch: add door/container interaction sync to core/coop.cpp (kept for the record)."""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")
anchor = "void receive() {\n    char buf[1500];"
code = r'''// Watch the game's own door/container events by name (see hk_pe in main.cpp).
void setup_watches() {
    if (g.watches_set) return;
    uint32_t a = ue::make_name("OnOCLStateChange", false).index, b = ue::make_name("HandleItemRemoved", false).index;
    if (!a || !b) return;
    watch[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(a));
    watch[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(b));
    watch_count.store(2, std::memory_order_release);
    g.watches_set = true;
}

int param_offset(ue::UFunction* f, const char* name) {
    for (ue::FField* p = ue::first_prop(f); p; p = ue::next_prop(p))
        if (ue::field_name(p) == name) return ue::prop_offset(p);
    return -1;
}

void queue_interact(uint8_t kind, uint64_t id, int32_t a, const std::string& path) {
    Outgoing o{};
    o.m.eid = (g.me & 0xFFFF0000u) | (g.next_eid++ & 0xFFFFu);
    o.m.kind = kind, o.m.id = id, o.m.a = a;
    o.m.len = static_cast<uint16_t>(std::min<size_t>(path.size(), sizeof o.m.path));
    memcpy(o.m.path, path.data(), o.m.len);
    o.size = static_cast<int>(offsetof(InteractMsg, path)) + o.m.len;
    o.at[0] = g.now, o.at[1] = g.now + 0.1, o.at[2] = g.now + 0.3;  // three copies: UDP
    g.outgoing.push_back(o);
}

// Game thread: turn observed events into messages.
void process_observed() {
    std::vector<Observed> evs;
    {
        std::lock_guard<std::mutex> lk(obs_mu);
        evs.swap(observed);
    }
    for (auto& e : evs) {
        auto* o = static_cast<ue::UObject*>(e.obj);
        auto* f = static_cast<ue::UFunction*>(e.fn);
        if (!ue::alive(o)) continue;
        std::string fname = ue::name_of(f), path = ue::path_of(o);
        if (fname == "OnOCLStateChange") {
            int off = param_offset(f, "CurrentState");
            if (off < 0 || off >= 64) continue;
            int32_t st = e.parms[off];
            if (st > 2) continue;  // Closed 0, Open 1, Locked 2; transitions and seals are not mirrored
            queue_interact(1, fnv64(path), st, "");
            g.doors_sent++;
            g.last_interact = "sent door " + path.substr(path.rfind('.') + 1) + " -> " + std::to_string(st);
        } else if (fname == "HandleItemRemoved") {
            int off = param_offset(f, "ItemStack");
            if (off < 0 || off + 12 > 64) continue;
            ue::UObject* item;
            int32_t count;
            memcpy(&item, e.parms + off, 8);
            memcpy(&count, e.parms + off + 8, 4);
            if (!item || !ue::alive(item) || count <= 0) continue;
            queue_interact(2, fnv64(path), count, ue::path_of(ue::class_of(item)));
            g.items_sent++;
            g.last_interact = "sent take " + std::to_string(count) + "x " + ue::name_of(ue::class_of(item));
        }
    }
}

void send_outgoing() {
    for (size_t i = 0; i < g.outgoing.size();) {
        Outgoing& o = g.outgoing[i];
        while (o.sent < 3 && g.now >= o.at[o.sent]) {
            send_typed(kInteract, &o.m, o.size);
            o.sent++;
        }
        if (o.sent >= 3) g.outgoing.erase(g.outgoing.begin() + i);
        else i++;
    }
}

ue::UObject* interactable(uint64_t id) {
    auto it = g.interactables.find(id);
    if (it != g.interactables.end() && ue::alive(it->second)) return it->second;
    if (g.now - g.interactables_built < 1.0) return nullptr;  // rebuild at most once a second
    g.interactables.clear();
    g.interactables_built = g.now;
    for (const char* cls : {"/Script/Arkansas.Door", "/Script/Arkansas.Container"}) {
        ue::UClass* c = ue::find_class(cls);
        if (!c) continue;
        for (ue::UObject* o : ue::instances(c, false, 20000)) g.interactables[fnv64(ue::path_of(o))] = o;
    }
    it = g.interactables.find(id);
    return it != g.interactables.end() ? it->second : nullptr;
}

void call_on(ue::UObject* o, const char* fn, const json& args) {
    ue::UFunction* f = ue::find_func(ue::class_of(o), fn);
    if (f) ue::call(o, f, args);
}

void apply_interact(const InteractMsg& m) {
    if (std::find(g.seen_eids.begin(), g.seen_eids.end(), m.eid) != g.seen_eids.end()) return;
    g.seen_eids.push_back(m.eid);
    if (g.seen_eids.size() > 512) g.seen_eids.erase(g.seen_eids.begin(), g.seen_eids.begin() + 256);
    ue::UObject* actor = interactable(m.id);
    if (!actor) return;
    std::string err;
    applying_remote = true;
    try {
        if (m.kind == 1) {
            ue::UObject* ic = ue::resolve(ue::addr(actor) + ".InteractableComponent", &err);
            if (ic) {
                json cur = ue::call(ic, ue::find_func(ue::class_of(ic), "GetCurrentStateOCL"), json::object()).get("ReturnValue");
                std::string want = m.a == 0 ? "Closed" : m.a == 1 ? "Open" : "Locked";
                if (cur.asString() != want) {
                    json a = json::object();
                    a.set("bIgnoreLockStatus", true);
                    if (cur.asString() == "Locked" && m.a != 2) {
                        json u = json::object();
                        u.set("bFireOCLUnlockedEvent", true);
                        call_on(ic, "UnlockOCL", u);
                    }
                    if (m.a == 1) call_on(ic, "OpenOCL", a);
                    else if (m.a == 0) call_on(ic, "CloseOCL", a);
                    else {
                        if (cur.asString() == "Open") call_on(ic, "CloseOCL", a);
                        json l = json::object();
                        l.set("bFireOCLLockedEvent", true);
                        call_on(ic, "LockOCL", l);
                    }
                    g.doors_applied++;
                    g.last_interact = "applied door " + ue::name_of(actor) + " -> " + want;
                }
            }
        } else if (m.kind == 2 && m.len > 0) {
            std::string cls(m.path, m.len);
            ue::UClass* item = ue::find_class(cls);
            ue::UObject* inv = ue::resolve(ue::addr(actor) + ".InventoryComponent", &err);
            if (item && inv) {
                json q = json::object();
                q.set("Actor", ue::addr(actor)).set("ItemType", ue::addr(item));
                int have = static_cast<int>(ue::call(inv, ue::find_func(ue::class_of(inv), "GetItemCountOnActor"), q).get("ReturnValue").asInt(0));
                if (have > 0) {
                    q.set("Count", std::min(have, static_cast<int>(m.a)));
                    ue::call(inv, ue::find_func(ue::class_of(inv), "RemoveItemFromActor"), q);
                    g.items_applied++;
                    g.last_interact = "applied take " + std::to_string(m.a) + "x " + ue::name_of(item) + " from " + ue::name_of(actor);
                }
            }
        }
    } catch (const std::exception& e) {
        g.last_error = std::string("interact: ") + e.what();
    }
    applying_remote = false;
}

'''
assert anchor in s
s = s.replace(anchor, code + anchor, 1)
old = "        if (h.type == kResyncRequest && g.host) {"
new = '''        if (h.type == kInteract) {
            int body = n - static_cast<int>(sizeof(Header));
            InteractMsg m{};
            if (body < static_cast<int>(offsetof(InteractMsg, path)) || body > static_cast<int>(sizeof m)) { g.bad++; continue; }
            memcpy(&m, buf + sizeof(Header), body);
            if (m.len > sizeof m.path || body != static_cast<int>(offsetof(InteractMsg, path)) + m.len) { g.bad++; continue; }
            g.recv++;
            apply_interact(m);
            continue;
        }
        if (h.type == kResyncRequest && g.host) {'''
assert old in s
s = s.replace(old, new, 1)
old = "    if (pawn && settled) {\n        drive_puppet();"
new = "    if (pawn && settled) {\n        setup_watches();\n        process_observed();\n        send_outgoing();\n        drive_puppet();"
assert old in s
s = s.replace(old, new, 1)
old = "json status() {\n    json s = json::object();"
new = '''void observe(void* o, void* f, const void* p) {
    if (applying_remote || !p) return;  // our own apply (game thread): no echo
    Observed e{};
    e.obj = o, e.fn = f;
    uint16_t size = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(f) + 0xB6);  // UFunction::ParmsSize
    memcpy(e.parms, p, std::min<size_t>(size, sizeof e.parms));
    std::lock_guard<std::mutex> lk(obs_mu);
    if (observed.size() < 4096) observed.push_back(e);
}

json status() {
    json s = json::object();'''
assert old in s
s = s.replace(old, new, 1)
old = '    s.set("story", story::status());'
new = '''    s.set("story", story::status());
    s.set("doorsSent", static_cast<double>(g.doors_sent)).set("doorsApplied", static_cast<double>(g.doors_applied));
    s.set("itemsSent", static_cast<double>(g.items_sent)).set("itemsApplied", static_cast<double>(g.items_applied)).set("lastInteract", g.last_interact);'''
assert old in s
s = s.replace(old, new, 1)
p.write_text(s, encoding="utf-8", newline="\n")
print("patched", p)
