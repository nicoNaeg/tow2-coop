// Story sync (host-owned campaign): the host watches quest states, quest addendums (objective updates) and the global
// variables quest objectives test, and sends every change through a small reliable channel; the client applies them
// in order with the game's own quest scripts. Quest data comes from the game's QuestBundleSet
// (/Game/Exported/BaseGame/Design/Quests/quests), so ids and names are the game's own.
#include "story.hpp"
#include "ue.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

void core_log(const std::string& line);  // main.cpp: a line in tow2coop.log

namespace story {
using json = coopbridge::json::Value;
FindQuestFn find_quest = nullptr;
namespace {
struct Guid { uint8_t b[16]; bool operator<(const Guid& o) const { return memcmp(b, o.b, 16) < 0; } };
struct Quest {
    Guid id;
    std::string name;
    std::vector<int32_t> addendums;
};
struct Probe {
    uint8_t kind;   // 1 quest state, 2 addendum, 3 global value
    int index;      // quest index (kind 1, 2) or global index (kind 3)
    int32_t arg;    // addendum id (kind 2)
    int32_t last = 0;
    bool known = false;
};
#pragma pack(push, 1)
struct Event {
    uint32_t seq;
    uint8_t kind;
    uint8_t guid[16];
    int32_t a;
};
#pragma pack(pop)
constexpr int kMaxEvents = 40;

// A UFunction on an object with its parameter offsets.
struct Call {
    ue::UObject* obj = nullptr;
    ue::UFunction* fn = nullptr;
    int size = 0;
    std::map<std::string, int> off;
    bool init(ue::UObject* o, const char* name) {
        obj = o;
        fn = o ? ue::find_func(ue::class_of(o), name) : nullptr;
        if (!fn) return false;
        for (ue::FField* p = ue::first_prop(fn); p; p = ue::next_prop(p)) {
            off[ue::field_name(p)] = ue::prop_offset(p);
            size = std::max(size, ue::prop_offset(p) + ue::prop_size(p));
        }
        return true;
    }
    template <class T> void put(uint8_t* b, const char* p, const T& v) const { memcpy(b + off.at(p), &v, sizeof v); }
    void run(uint8_t* b) const { ue::process_event(obj, fn, b); }
};

struct State {
    bool loaded = false;
    std::string load_error;
    std::vector<Quest> quests;
    std::map<Guid, int> quest_by_id;
    std::vector<Guid> globals;
    std::vector<Probe> probes;
    std::map<std::pair<int, int32_t>, size_t> quest_probe;  // (quest index, 0 = state | addendum id) -> probe
    std::map<Guid, size_t> global_probe;                    // global -> probe
    size_t cursor = 0;
    Call get_state, addendum_triggered, is_global, start, mention, trigger_addendum, set_global, debug_advance;
    ue::UObject* qm = nullptr;
    // reliable channel, each way
    std::vector<Event> log;     // our changes since this session started (seq = index + 1)
    uint32_t peer_acked = 0;    // highest seq of ours the other side applied
    uint32_t applied = 0;       // highest seq of the other side's that we applied
    uint32_t dropped_to = 0;    // client: its changes up to here were made before a reload, never sent
    double next_send = 0;
    uint64_t applied_count = 0, skipped_count = 0, sent_packets = 0, forced = 0;
    std::string last_applied;
    // client: outcomes the client could not apply directly (complete/fail): checked again after a grace period
    struct Pending { Guid id; int32_t target; double at; };
    std::vector<Pending> pending;
    // outcomes the other world reached, made here through the game's own path (advance_frame)
    struct Advance { Guid id; int32_t target; double start, next; int tries; std::string name; };
    std::vector<Advance> advancing;
    uint64_t advanced = 0;
    bool reset_ack = false, resync_wanted = false;
    double now = 0;
} g;

std::string fstr(const uint8_t* p) {
    const ue::FString& s = *reinterpret_cast<const ue::FString*>(p);
    std::string out;
    for (int i = 0; i + 1 < s.num; i++) out += static_cast<char>(s.data[i] < 128 ? s.data[i] : '?');
    return out;
}

const uint8_t* arr_data(const uint8_t* base, const ue::FProperty* p, int& num) {
    auto a = *reinterpret_cast<const ue::TArray<uint8_t>*>(base + ue::prop_offset(p));
    num = a.num;
    return a.data;
}

bool load(std::string& err) {
    ue::UObject* set = ue::find_path("/Game/Exported/BaseGame/Design/Quests/quests.quests");
    if (!set) return err = "QuestBundleSet not loaded", false;
    ue::FProperty* bundles = ue::find_prop(ue::class_of(set), "QuestBundles");
    ue::FProperty* inner = ue::array_inner(bundles);
    ue::UStruct* qb = ue::struct_of(inner);
    if (!qb) return err = "QuestBundles layout", false;
    int esz = ue::prop_size(inner), n = 0;
    const uint8_t* data = arr_data(reinterpret_cast<const uint8_t*>(set), bundles, n);
    ue::FProperty *p_name = ue::find_prop(qb, "Name"), *p_id = ue::find_prop(qb, "ID");
    if (!p_name || !p_id || n <= 0 || n > 5000) return err = "QuestBundle fields", false;
    static const char* kObjectiveArrays[] = {"GenericObjectiveNodes", "GlobalVariableObjectiveNodes", "TalkObjectiveNodes", "KillObjectiveNodes",
                                             "AcquireItemObjectiveNodes", "ObjectiveBankNodes", "BranchCompleteObjectiveNodes"};
    std::map<Guid, bool> seen_globals;
    for (int i = 0; i < n; i++) {
        const uint8_t* b = data + static_cast<size_t>(i) * esz;
        Quest q;
        memcpy(q.id.b, b + ue::prop_offset(p_id), 16);
        q.name = fstr(b + ue::prop_offset(p_name));
        for (const char* an : kObjectiveArrays) {
            ue::FProperty* ap = ue::find_prop(qb, an);
            ue::FProperty* ain = ue::array_inner(ap);
            ue::UStruct* node = ue::struct_of(ain);
            if (!node) continue;
            ue::FProperty* p_add = ue::find_prop(node, "AddendumIDs");
            ue::FProperty* p_cond = ue::find_prop(node, "Conditional");
            ue::FProperty* p_fail = ue::find_prop(node, "ChildFailedConditional");
            int nn = 0;
            const uint8_t* nodes = arr_data(b, ap, nn);
            for (int k = 0; k < nn && k < 4096; k++) {
                const uint8_t* nb = nodes + static_cast<size_t>(k) * ue::prop_size(ain);
                if (p_add) {
                    int na = 0;
                    const int32_t* ids = reinterpret_cast<const int32_t*>(arr_data(nb, p_add, na));
                    for (int j = 0; j < na && j < 64; j++)
                        if (std::find(q.addendums.begin(), q.addendums.end(), ids[j]) == q.addendums.end()) q.addendums.push_back(ids[j]);
                }
                for (ue::FProperty* c : {p_cond, p_fail}) {
                    if (!c) continue;
                    Guid gv;
                    memcpy(gv.b, nb + ue::prop_offset(c), 16);  // GlobalVariableConditional.ID.ID at +0
                    bool zero = std::all_of(gv.b, gv.b + 16, [](uint8_t x) { return x == 0; });
                    if (!zero && !seen_globals[gv]) { seen_globals[gv] = true; g.globals.push_back(gv); }
                }
            }
        }
        g.quest_by_id[q.id] = static_cast<int>(g.quests.size());
        g.quests.push_back(q);
    }
    for (int i = 0; i < static_cast<int>(g.quests.size()); i++) {
        g.quest_probe[{i, 0}] = g.probes.size();
        g.probes.push_back({1, i, 0});
        for (int32_t a : g.quests[i].addendums) g.quest_probe[{i, a}] = g.probes.size(), g.probes.push_back({2, i, a});
    }
    for (int i = 0; i < static_cast<int>(g.globals.size()); i++) g.global_probe[g.globals[i]] = g.probes.size(), g.probes.push_back({3, i, 0});
    std::vector<ue::UObject*> qms = ue::instances(ue::find_class("/Script/Arkansas.OwQuestManager"), false, 1);
    g.qm = qms.empty() ? nullptr : qms[0];
    ue::UObject* qc = ue::cdo(ue::find_class("/Script/OEIFlowCharts.QuestConditionals"));
    ue::UObject* qs = ue::cdo(ue::find_class("/Script/OEIFlowCharts.QuestScripts"));
    ue::UObject* gc = ue::cdo(ue::find_class("/Script/OEIGlobalVariables.GlobalVariableConditionals"));
    ue::UObject* gs = ue::cdo(ue::find_class("/Script/OEIGlobalVariables.GlobalVariableScripts"));
    if (!g.get_state.init(g.qm, "GetQuestState_BP") || !g.addendum_triggered.init(qc, "IsQuestAddendumTriggered") || !g.is_global.init(gc, "IsGlobalValue") ||
        !g.start.init(qs, "StartQuest") || !g.mention.init(qs, "MentionQuest") || !g.trigger_addendum.init(qs, "TriggerQuestAddendum") ||
        !g.set_global.init(gs, "SetGlobalValue"))
        return err = "quest/global script functions missing", false;
    g.debug_advance.init(qs, "DebugAdvanceQuest");  // optional: without it, outcomes are written (advance_frame)
    return true;
}

int32_t quest_state(const Guid& id) {
    alignas(16) uint8_t b[64] = {};
    g.get_state.put(b, "QuestID", id);
    g.get_state.run(b);
    return b[g.get_state.off.at("ReturnValue")];
}
bool addendum_on(const Guid& id, int32_t a) {
    alignas(16) uint8_t b[64] = {};
    g.addendum_triggered.put(b, "QuestID", id);
    g.addendum_triggered.put(b, "AddendumID", a);
    g.addendum_triggered.run(b);
    return b[g.addendum_triggered.off.at("ReturnValue")] != 0;
}
bool global_is(const Guid& id, uint8_t op, int32_t v) {
    alignas(16) uint8_t b[64] = {};
    g.is_global.put(b, "Variable", id);
    g.is_global.put(b, "Operator", op);
    g.is_global.put(b, "Value", v);
    g.is_global.run(b);
    return b[g.is_global.off.at("ReturnValue")] != 0;
}
int32_t global_value(const Guid& id, int32_t hint) {
    if (global_is(id, 0, hint)) return hint;
    for (int32_t v = 0; v < 16; v++)
        if (global_is(id, 0, v)) return v;
    int64_t lo = INT32_MIN, hi = INT32_MAX;  // largest v with value >= v
    while (lo < hi) {
        int64_t mid = lo + (hi - lo + 1) / 2;
        if (global_is(id, 4, static_cast<int32_t>(mid))) lo = mid;  // GreaterThanOrEquals
        else hi = mid - 1;
    }
    return static_cast<int32_t>(lo);
}

void emit(uint8_t kind, const Guid& id, int32_t a) {
    Event e{};
    e.seq = static_cast<uint32_t>(g.log.size() + 1), e.kind = kind, e.a = a;
    memcpy(e.guid, id.b, 16);
    g.log.push_back(e);
}

void poll_some(int budget) {
    for (int k = 0; k < budget && !g.probes.empty(); k++) {
        Probe& p = g.probes[g.cursor];
        g.cursor = (g.cursor + 1) % g.probes.size();
        int32_t v = 0;
        if (p.kind == 1) v = quest_state(g.quests[p.index].id);
        else if (p.kind == 2) {
            if (g.probes.empty()) break;
            v = addendum_on(g.quests[p.index].id, p.arg) ? 1 : 0;
        } else v = p.known ? (global_is(g.globals[p.index], 1, p.last) ? global_value(g.globals[p.index], p.last) : p.last) : global_value(g.globals[p.index], 0);
        if (p.known && v != p.last) {
            if (p.kind == 1) emit(1, g.quests[p.index].id, v);
            else if (p.kind == 2 && v == 1) emit(2, g.quests[p.index].id, p.arg);
            else if (p.kind == 3) emit(3, g.globals[p.index], v);
        }
        p.last = v, p.known = true;  // first look at each probe is the baseline: the client loaded the same state
    }
}

void baseline(size_t probe, int32_t v) {  // a change we apply is not ours to send back
    if (probe < g.probes.size()) g.probes[probe].last = v, g.probes[probe].known = true;
}

void apply(const Event& e) {
    Guid id;
    memcpy(id.b, e.guid, 16);
    char what[96];
    if (e.kind == 1) {
        auto it = g.quest_by_id.find(id);
        if (it == g.quest_by_id.end() || e.a < 0 || e.a > 4) { g.skipped_count++; return; }
        auto qp = g.quest_probe.find({it->second, 0});
        if (qp != g.quest_probe.end()) baseline(qp->second, e.a);
        const Quest& q = g.quests[it->second];
        int32_t local = quest_state(id);
        alignas(16) uint8_t b[64] = {};
        if (e.a == 1 && (local == 0 || local == 4)) { g.start.put(b, "QuestID", id); g.start.run(b); }
        else if (e.a == 4 && local == 0) { g.mention.put(b, "QuestID", id); g.mention.run(b); }
        else if ((e.a == 2 || e.a == 3) && local != e.a) {
            // QuestComplete/QuestFail do nothing in shipping; the quest's own path is walked instead (advance_frame),
            // its state written only if that path doesn't get there.
            bool queued = false;
            for (const auto& a : g.advancing) queued = queued || !memcmp(a.id.b, id.b, 16);
            if (!queued) g.advancing.push_back({id, e.a, g.now, g.now + 1.5, 0, q.name});  // 1.5 s for the synced inputs
            g.pending.push_back({id, e.a, g.now + 40.0});
        } else { g.skipped_count++; return; }
        snprintf(what, sizeof what, "quest %s -> %d", q.name.c_str(), e.a);
    } else if (e.kind == 2) {
        auto it0 = g.quest_by_id.find(id);
        auto ap = it0 == g.quest_by_id.end() ? g.quest_probe.end() : g.quest_probe.find({it0->second, e.a});
        if (ap == g.quest_probe.end()) { g.skipped_count++; return; }  // an addendum the quest data does not have
        baseline(ap->second, 1);
        if (addendum_on(id, e.a)) { g.skipped_count++; return; }
        alignas(16) uint8_t b[64] = {};
        g.trigger_addendum.put(b, "QuestID", id);
        g.trigger_addendum.put(b, "AddendumID", e.a);
        b[g.trigger_addendum.off.at("DisplayNotification")] = 1;
        g.trigger_addendum.run(b);
        auto it = g.quest_by_id.find(id);
        snprintf(what, sizeof what, "addendum %s #%d", it != g.quest_by_id.end() ? g.quests[it->second].name.c_str() : "?", e.a);
    } else if (e.kind == 3) {
        auto gp = g.global_probe.find(id);
        if (gp == g.global_probe.end()) { g.skipped_count++; return; }  // only the objective globals are shared
        baseline(gp->second, e.a);
        if (global_is(id, 0, e.a)) { g.skipped_count++; return; }
        alignas(16) uint8_t b[64] = {};
        g.set_global.put(b, "Variable", id);
        g.set_global.put(b, "Value", e.a);
        g.set_global.run(b);
        snprintf(what, sizeof what, "global -> %d", e.a);
    } else { g.skipped_count++; return; }
    g.applied_count++;
    g.last_applied = what;
}
}  // namespace

bool ensure_loaded() {
    if (g.loaded) return true;
    std::string err;
    if (!load(err)) {
        g.load_error = err;
        g.quests.clear(), g.quest_by_id.clear(), g.globals.clear(), g.probes.clear();
        return false;
    }
    g.loaded = true;
    return true;
}

uint64_t forced_completions() { return g.forced; }

namespace {
void write_outcome(const Guid& id, int32_t target) {  // the old way: the state written (no banner, no XP here)
    uint8_t* quest = find_quest && g.qm ? find_quest(g.qm, id.b) : nullptr;
    if (!quest || !ue::readable(quest + kQuestStateOffset, 1)) return;
    quest[kQuestStateOffset] = static_cast<uint8_t>(target);
    if (target == 2) g.forced++;  // no quest XP from this game for it: the other side's comes (coop.cpp)
}

// The other world's outcome, walked here: DebugAdvanceQuest completes the quest's current objectives the game's way
// (the banner, the journal, the quest's XP), a step a second. The state is written instead when the quest ends the
// other way, needs more than 15 steps or 30 s, or the script is missing.
void advance_frame() {
    for (size_t i = 0; i < g.advancing.size();) {
        State::Advance& a = g.advancing[i];
        int32_t local = quest_state(a.id);
        bool done = local == a.target;
        if (done && a.tries) g.advanced++, core_log("story: quest " + a.name + " completed here too, the game's way (" + std::to_string(a.tries) + " steps)");
        if (!done && ((local == 2 || local == 3) || a.tries >= 15 || g.now - a.start > 30.0 || !g.debug_advance.fn)) {
            write_outcome(a.id, a.target);
            core_log("story: quest " + a.name + " set to the other world's outcome (" + std::to_string(a.target) + "; it went to " + std::to_string(local) + ")");
            done = true;
        } else if (!done && g.now >= a.next) {
            alignas(16) uint8_t b[64] = {};
            g.debug_advance.put(b, "QuestID", a.id);
            g.debug_advance.run(b);
            a.tries++, a.next = g.now + 1.0;
        }
        if (done) g.advancing.erase(g.advancing.begin() + static_cast<long>(i));
        else i++;
    }
}
}  // namespace

void reset_peer() { g.peer_acked = 0, g.applied = 0; }  // a (re)joined client: replay ours (idempotent), take its anew

void send_log(double now, const SendFn& send) {  // our changes the other side has not applied yet
    uint32_t from = std::max(g.peer_acked, g.dropped_to);
    if (from >= g.log.size() || now < g.next_send) return;
    g.next_send = now + 0.1;
    uint8_t body[6 + kMaxEvents * sizeof(Event)];
    uint16_t count = 0;
    uint32_t base = g.dropped_to + 1;  // the first of our changes still valid (older ones were dropped by a reload)
    for (uint32_t s = from; s < g.log.size() && count < kMaxEvents; s++, count++)
        memcpy(body + 6 + count * sizeof(Event), &g.log[s], sizeof(Event));
    memcpy(body, &base, 4), memcpy(body + 4, &count, 2);
    send(kStory, body, 6 + count * static_cast<int>(sizeof(Event)));
    g.sent_packets++;
}

void host_frame(double now, const SendFn& send) {
    g.now = now;
    if (!ensure_loaded()) return;
    g.pending.clear();  // the host has no one to resync from
    advance_frame();
    poll_some(80);
    send_log(now, send);
}

void on_story(const char* body, int len, const SendFn& send) {
    if (len < 6 || !ensure_loaded() || g.reset_ack) return;  // a reset is announced first
    uint32_t base;
    uint16_t count;
    memcpy(&base, body, 4), memcpy(&count, body + 4, 2);
    if (count > kMaxEvents || len != 6 + count * static_cast<int>(sizeof(Event))) return;
    if (base > g.applied + 1) g.applied = base - 1;  // the sender dropped its changes made before a reload
    for (int i = 0; i < count; i++) {
        Event e;
        memcpy(&e, body + 6 + i * sizeof(Event), sizeof e);
        if (e.seq != g.applied + 1) continue;  // in order only; the sender resends from our ack
        apply(e);
        g.applied = e.seq;
    }
    send(kStoryAck, &g.applied, 4);
}

void on_ack(const char* body, int len) {
    if (len != 4 && len != 8) return;
    uint32_t a, flags = 0;
    memcpy(&a, body, 4);
    if (len == 8) memcpy(&flags, body + 4, 4);
    if (flags & 1) g.peer_acked = std::min<uint32_t>(a, static_cast<uint32_t>(g.log.size()));  // client reloaded: replay
    else if (a > g.peer_acked && a <= g.log.size()) g.peer_acked = a;
}

void client_reset() {  // the client's world was reloaded from a save: ask for a full (idempotent) replay
    g.applied = 0;
    g.pending.clear(), g.advancing.clear();
    g.reset_ack = true;
    g.dropped_to = static_cast<uint32_t>(g.log.size());  // changes made before the reload are not sent any more
    for (Probe& p : g.probes) p.known = false;          // the reloaded world is the new baseline
}

void client_frame(double now, const SendFn& send) {
    g.now = now;
    if (g.reset_ack) {
        uint32_t body[2] = {0, 1};
        send(kStoryAck, body, 8);
        g.reset_ack = false;
    }
    if (ensure_loaded()) {  // our own changes go to the host too (the first look at each probe is the baseline)
        advance_frame();
        poll_some(80);
        send_log(now, send);
    }
    for (size_t i = 0; i < g.pending.size();) {
        if (now < g.pending[i].at) { i++; continue; }
        if (ensure_loaded() && quest_state(g.pending[i].id) != g.pending[i].target) g.resync_wanted = true;
        g.pending.erase(g.pending.begin() + i);
    }
}

bool take_resync_request() {
    bool r = g.resync_wanted;
    g.resync_wanted = false;
    return r;
}

json status() {
    json s = json::object();
    s.set("loaded", g.loaded).set("error", g.load_error).set("quests", static_cast<double>(g.quests.size()));
    s.set("globals", static_cast<double>(g.globals.size())).set("probes", static_cast<double>(g.probes.size()));
    s.set("events", static_cast<double>(g.log.size())).set("peerAcked", static_cast<double>(g.peer_acked)).set("applied", static_cast<double>(g.applied));
    s.set("appliedCount", static_cast<double>(g.applied_count)).set("skipped", static_cast<double>(g.skipped_count)).set("lastApplied", g.last_applied);
    s.set("pendingOutcomes", static_cast<double>(g.pending.size())).set("ownEvents", static_cast<double>(g.log.size()));
    s.set("advancing", static_cast<double>(g.advancing.size())).set("advanced", static_cast<double>(g.advanced)).set("forced", static_cast<double>(g.forced));
    return s;
}
}  // namespace story
