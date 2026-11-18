// tow2coop.dll: runtime core of the co-op mod for The Outer Worlds 2 (Steam 1.2.0.1, CL-2994920).
// Loaded by the dsound.dll proxy. It waits for the engine, checks that this is the one supported build (PE header and
// the bytes at every address it uses), checks the reflection layouts against live objects, then hooks
// UGameEngine::Tick (game-thread frame pump) and UObject::ProcessEvent (event tracing). In test copies
// (TOW2_BRIDGE_PIPE / TOW2_BRIDGE_PORT set) it serves the coop-anything bridge protocol for tests and tools.
// On any mismatch it does nothing and the game runs unmodified.
#include <winsock2.h>
#include <coopbridge/server.h>
#include "ue.hpp"
#include "coop.hpp"
#include "join.hpp"
#include "steamnet.hpp"
#include "character.hpp"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>

namespace hook { void* install(void* target, const uint8_t* expect, size_t len, void* detour, std::string& err); }

using coopbridge::json::Value;
namespace {
// ---------------------------------------------------------------- the supported build
constexpr uint32_t kTimeDateStamp = 0x89B6FE2C, kSizeOfImage = 0x0B137000, kCheckSum = 0x0ACBC2F0;
constexpr ue::Rvas kRvas = {0xA3F65C0, 0xA5DD780, 0xA436C40, 0x109EC84, 0x1027AEC};
constexpr uint32_t kTickRva = 0x14165E4;
constexpr uint8_t kPeBytes[19] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};
constexpr uint8_t kTickBytes[14] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kCtorBytes[10] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30};
constexpr uint8_t kPoolFnBytes[11] = {0x48, 0x83, 0xEC, 0x28, 0x80, 0x3D, 0x49, 0x7C, 0x39, 0x09, 0x00};  // GetNamePool: cmp [ready], 0
constexpr uint32_t kPoolFnRva = 0x109EFEC;
// UPlayerInfoComponent::AddXP(XP, EXPGainReason, TArray* details): every XP gain but the debug command's (inlined)
constexpr uint32_t kAddXpRva = 0x139E03C;
constexpr uint8_t kAddXpBytes[20] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57};

uint8_t* g_base;
std::string g_logpath, g_status = "starting", g_verify_report;
std::mutex g_log_mu;
std::atomic<bool> g_verified{false}, g_bridge_on{false}, g_trace_on{false};
std::atomic<uint64_t> g_frames{0};
std::atomic<DWORD> g_game_thread{0};
std::atomic<float> g_last_dt{0};
std::atomic<bool> g_coop_on{false};
int g_coop_errors;
coopbridge::Server* g_server = new coopbridge::Server;  // never destroyed: process exit order is not ours

void logmsg(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> g(g_log_mu);
    if (FILE* f = fopen(g_logpath.c_str(), "ab")) {
        fprintf(f, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
        fclose(f);
    }
}

// ---------------------------------------------------------------- fault guard for bridge requests
// An access violation inside this DLL while a bridge request runs becomes a C++ exception at the faulting frame, so
// destructors run and the request fails instead of the game. Faults in game code are left alone (they stay crashes).
thread_local int t_guard;
thread_local uintptr_t t_fault_at, t_fault_addr;  // where in this DLL (offset) and what address it read or wrote
uintptr_t g_self_lo, g_self_hi;
[[noreturn]] __attribute__((noinline, force_align_arg_pointer)) void throw_av() {
    char b[200];  // the offset maps to a function with llvm-symbolizer --obj=tow2coop.dll --adjust-vma=<image base>
    snprintf(b, sizeof b, "access violation inside the core at +0x%llx, address 0x%llx (bad pointer or layout); the request was aborted",
             static_cast<unsigned long long>(t_fault_at), static_cast<unsigned long long>(t_fault_addr));
    throw std::runtime_error(b);
}
LONG CALLBACK fault_guard(EXCEPTION_POINTERS* e) {
    if (!t_guard || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = e->ContextRecord->Rip;
    if (rip < g_self_lo || rip >= g_self_hi) return EXCEPTION_CONTINUE_SEARCH;
    t_fault_at = rip - g_self_lo;
    t_fault_addr = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    e->ContextRecord->Rsp -= 8;
    *reinterpret_cast<DWORD64*>(e->ContextRecord->Rsp) = rip + 1;  // a return address inside the faulting function
    e->ContextRecord->Rip = reinterpret_cast<DWORD64>(&throw_av);
    return EXCEPTION_CONTINUE_EXECUTION;
}
struct GuardScope {
    GuardScope() { ++t_guard; }
    ~GuardScope() { --t_guard; }
};
void reg(const char* name, std::function<Value(const Value&)> fn, bool main_thread = true) {
    g_server->Register(name, [fn](const Value& p) { GuardScope g; return fn(p); }, main_thread);
}

std::string env(const char* name) {
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n && n < sizeof buf ? std::string(buf, n) : std::string();
}

bool fingerprint(std::string& why) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    char b[200];
    snprintf(b, sizeof b, "timestamp 0x%08lX size 0x%08lX checksum 0x%08lX", (unsigned long)nt->FileHeader.TimeDateStamp,
             (unsigned long)nt->OptionalHeader.SizeOfImage, (unsigned long)nt->OptionalHeader.CheckSum);
    why = b;
    if (nt->FileHeader.TimeDateStamp != kTimeDateStamp || nt->OptionalHeader.SizeOfImage != kSizeOfImage || nt->OptionalHeader.CheckSum != kCheckSum)
        return why += " (not 1.2.0.1 CL-2994920)", false;
    if (memcmp(g_base + kRvas.process_event, kPeBytes, sizeof kPeBytes) || memcmp(g_base + kTickRva, kTickBytes, sizeof kTickBytes) ||
        memcmp(g_base + kRvas.fname_ctor, kCtorBytes, sizeof kCtorBytes) || memcmp(g_base + kPoolFnRva, kPoolFnBytes, sizeof kPoolFnBytes))
        return why += ", but code bytes differ", false;
    return true;
}

// ---------------------------------------------------------------- ProcessEvent trace
struct TraceRec { ue::UObject* cls; ue::UFunction* fn; };
std::mutex g_trace_mu;
std::vector<TraceRec> g_trace;

using PeFn = void (*)(ue::UObject*, ue::UFunction*, void*);
PeFn o_pe;
void hk_pe(ue::UObject* o, ue::UFunction* f, void* p) {
    if (g_trace_on.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> g(g_trace_mu);
        if (g_trace.size() < 4000000) g_trace.push_back({ue::class_of(o), f});
    }
    // Game events the co-op module mirrors (door state, container removals): a pointer compare per call.
    int n = coop::watch_count.load(std::memory_order_acquire);
    if (n) {  // by FName index (UObject::NamePrivate at 0x18): Blueprint overrides are separate UFunction objects
        uint32_t name = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(f) + 0x18);
        for (int i = 0; i < n; i++)
            if (coop::watch[i] == reinterpret_cast<void*>(static_cast<uintptr_t>(name))) { coop::observe(o, f, p); break; }
    }
    o_pe(o, f, p);
}

coop::AddXpFn o_add_xp;
bool hk_add_xp(void* self, int32_t xp, uint8_t reason, void* details) {
    if (coop::xp_blocked(self, reason)) return false;  // the client's own quest XP: the host's comes instead
    bool gained = o_add_xp(self, xp, reason, details);
    if (gained) coop::on_xp(self, xp, reason);
    return gained;
}

Value trace_stop(const Value& params) {
    g_trace_on = false;
    std::vector<TraceRec> recs;
    {
        std::lock_guard<std::mutex> g(g_trace_mu);
        recs.swap(g_trace);
    }
    std::string match = params.get("match").asString();
    size_t limit = static_cast<size_t>(params.get("limit").asInt(400));
    std::map<std::pair<ue::UObject*, ue::UFunction*>, std::pair<size_t, size_t>> agg;  // -> (first index, count)
    for (size_t i = 0; i < recs.size(); i++) {
        auto& e = agg[{recs[i].cls, recs[i].fn}];
        if (!e.second) e.first = i;
        e.second++;
    }
    std::vector<std::pair<size_t, Value>> rows;
    for (auto& kv : agg) {
        std::string fn = ue::name_of(kv.first.first) + ":" + ue::name_of(kv.first.second);
        if (!match.empty()) {
            std::string a = fn, b = match;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            if (a.find(b) == std::string::npos) continue;
        }
        Value r = Value::object();
        r.set("fn", fn).set("count", static_cast<double>(kv.second.second)).set("first", static_cast<double>(kv.second.first));
        rows.emplace_back(kv.second.first, r);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Value out = Value::object(), list = Value::array();
    for (size_t i = 0; i < rows.size() && i < limit; i++) list.push(rows[i].second);
    out.set("calls", static_cast<double>(recs.size())).set("distinct", static_cast<double>(rows.size())).set("rows", list);
    return out;
}

// ---------------------------------------------------------------- frame hook
using TickFn = void (*)(void*, float, bool);
TickFn o_tick;
void hk_tick(void* self, float dt, bool idle) {
    g_game_thread = GetCurrentThreadId();
    g_frames++;
    g_last_dt = dt;
    if (g_bridge_on) {
        try { g_server->Pump(); } catch (...) {}
    }
    if (g_coop_on) {
        try {
            GuardScope g;
            coop::frame(dt);
        } catch (const std::exception& e) {
            if (g_coop_errors++ < 20) logmsg("co-op frame error: %s", e.what());
        }
        try {
            GuardScope g;
            join::frame(g_frames.load());
        } catch (const std::exception& e) {
            if (g_coop_errors++ < 20) logmsg("join error: %s", e.what());
        }
    }
    o_tick(self, dt, idle);
}

// ---------------------------------------------------------------- bridge
ue::UObject* need(const std::string& ref) {
    std::string err;
    ue::UObject* o = ue::resolve(ref, &err);
    if (!o) {  // a class name stands for its default object (for static functions)
        ue::UClass* c = ue::find_class(ref);
        if (c) o = ue::cdo(c);
    }
    if (!o) throw coopbridge::Error(-32010, err.empty() ? "no object " + ref : err);
    return o;
}

class Adapter : public coopbridge::Adapter {
public:
    coopbridge::Capabilities capabilities() const override {
        coopbridge::Capabilities c;
        c.exec = c.eval = c.state = true;
        return c;
    }
    coopbridge::ExecResult Exec(const std::string& line, const std::string&) override {
        GuardScope g;
        ue::UObject* lib = need("/Script/Engine.Default__KismetSystemLibrary");
        ue::UFunction* f = ue::find_func(ue::class_of(lib), "ExecuteConsoleCommand");
        Value args = Value::object();
        args.set("WorldContextObject", ue::addr(need("world"))).set("Command", line);
        std::string err;
        if (ue::UObject* pc = ue::resolve("pc", &err)) args.set("SpecificPlayer", ue::addr(pc));
        ue::call(lib, f, args);
        return {true, ""};
    }
    Value Eval(const std::string& expr, const std::string&) override { GuardScope g; return ue::eval_path(expr); }
    Value State(const std::vector<std::string>&) override {
        GuardScope g;
        Value s = Value::object();
        std::string err;
        ue::UObject* world = ue::resolve("world", &err);
        s.set("world", world ? ue::path_of(world) : "none").set("frames", static_cast<double>(g_frames.load()));
        if (ue::resolve("pawn", &err)) {
            Value p = Value::object();
            p.set("ref", ue::eval_path("pawn"));
            try { p.set("location", ue::eval_path("pawn.K2_GetActorLocation()")); } catch (const std::exception& e) { p.set("location", e.what()); }
            s.set("pawn", p);
        }
        return s;
    }
};
Adapter* g_adapter = new Adapter;

void dump_types(const std::string& path, size_t& classes, size_t& structs, size_t& enums) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) throw coopbridge::Error(-32011, "cannot write " + path);
    int n = ue::num_objects();
    for (int i = 0; i < n; i++) {
        ue::UObject* o = ue::object_at(i);
        if (!o) continue;
        bool is_class = ue::is_a(o, ue::class_class()), is_struct = ue::is_a(o, ue::scriptstruct_class()), is_enum = ue::is_a(o, ue::enum_class());
        if (is_enum) {
            fprintf(f, "enum %s %s\n", ue::path_of(o).c_str(), ue::enum_values(o).dump().c_str());
            enums++;
            continue;
        }
        if (!is_class && !is_struct) continue;
        (is_class ? classes : structs)++;
        Value d = ue::describe_struct(o, is_class);
        fprintf(f, "%s %s : %s size 0x%x\n", is_class ? "class" : "struct", d.get("path").str().c_str(), d.get("super").str().c_str(),
                static_cast<unsigned>(d.get("size").asInt()));
        for (auto& p : d.get("props").items())
            fprintf(f, "  +0x%04x %-40s %s [%d] %s\n", static_cast<unsigned>(p.get("offset").asInt()), p.get("name").str().c_str(), p.get("type").str().c_str(),
                    static_cast<int>(p.get("size").asInt()), p.get("flags").str().c_str());
        for (auto& fn : d.get("functions").items()) {
            std::string ps;
            for (auto& p : fn.get("params").items())
                ps += (ps.empty() ? "" : ", ") + p.get("name").str() + ": " + p.get("type").str() + (p.get("ret").asBool() ? " ret" : p.get("out").asBool() ? " out" : "");
            fprintf(f, "  fn %s(%s) flags %s\n", fn.get("name").str().c_str(), ps.c_str(), fn.get("flags").str().c_str());
        }
    }
    fclose(f);
}

void register_methods() {
    reg("ue.find", [](const Value& p) {
        size_t limit = static_cast<size_t>(p.get("limit").asInt(50));
        bool with_cdo = p.get("cdo").asBool(false);
        std::vector<ue::UObject*> found;
        if (p.get("path").isString()) {
            if (ue::UObject* o = ue::find_path(p.get("path").str())) found.push_back(o);
        } else if (p.get("class").isString()) {
            ue::UClass* c = ue::find_class(p.get("class").str());
            if (!c) throw coopbridge::Error(-32010, "no class " + p.get("class").str());
            found = ue::instances(c, with_cdo, limit);
        } else {
            std::string want = p.get("name").asString();
            std::transform(want.begin(), want.end(), want.begin(), ::tolower);
            int n = ue::num_objects();
            for (int i = 0; i < n && found.size() < limit; i++) {
                ue::UObject* o = ue::object_at(i);
                if (!o || (!with_cdo && ue::is_cdo(o))) continue;
                std::string nm = ue::name_of(o);
                std::transform(nm.begin(), nm.end(), nm.begin(), ::tolower);
                if (nm.find(want) != std::string::npos && ue::alive(o)) found.push_back(o);
            }
        }
        Value out = Value::array();
        for (ue::UObject* o : found) out.push(ue::addr(o) + " " + ue::class_name(o) + " " + ue::path_of(o));
        return out;
    });
    reg("ue.get", [](const Value& p) {
        if (p.get("path").isString()) return ue::eval_path(p.get("path").str());
        return ue::read_object(need(p.get("ref").asString()), static_cast<int>(p.get("depth").asInt(0)), p.get("filter").asString());
    });
    reg("ue.set", [](const Value& p) { return ue::set_path(p.get("path").asString(), p.get("value")); });
    reg("ue.call", [](const Value& p) {
        ue::UObject* o = need(p.get("ref").asString());
        ue::UFunction* f = ue::find_func(ue::class_of(o), p.get("fn").asString());
        if (!f) throw coopbridge::Error(-32010, ue::class_name(o) + " has no function " + p.get("fn").asString());
        return ue::call(o, f, p.get("args"));
    });
    reg("ue.class", [](const Value& p) {
        std::string n = p.get("name").asString();
        ue::UObject* c = n.size() && n[0] == '/' ? ue::find_path(n) : ue::find_class(n);
        if (!c) throw coopbridge::Error(-32010, "no class or struct " + n);
        if (ue::is_a(c, ue::enum_class())) return ue::enum_values(c);
        return ue::describe_struct(c, true);
    });
    reg("ue.dump", [](const Value& p) {
        std::string path = p.get("path").asString();
        size_t c = 0, st = 0, e = 0;
        dump_types(path, c, st, e);
        Value out = Value::object();
        out.set("path", path).set("classes", static_cast<double>(c)).set("structs", static_cast<double>(st)).set("enums", static_cast<double>(e));
        return out;
    });
    reg("ue.trace.start", [](const Value&) {
        { std::lock_guard<std::mutex> g(g_trace_mu); g_trace.clear(); }
        g_trace_on = true;
        return Value(true);
    });
    reg("ue.trace.stop", trace_stop);
    // Self-check of the fault guard: a deliberate bad read inside the core must come back as an error reply.
    reg("diag.fault", [](const Value&) { return Value(*reinterpret_cast<volatile int*>(static_cast<uintptr_t>(8))); });
}

void start_bridge() {
    std::string pipe = env("TOW2_BRIDGE_PIPE"), port = env("TOW2_BRIDGE_PORT");
    if (pipe.empty() && port.empty()) return;
    coopbridge::Options o;
    o.game = "tow2";
    o.build = "tow2coop-dev " __DATE__ " " __TIME__;
    o.pipeName = pipe;
    o.tcpPort = port.empty() ? -1 : atoi(port.c_str());
    std::string dir = g_logpath.substr(0, g_logpath.find_last_of('\\'));
    o.endpointFile = dir + "\\bridge-endpoint.json";
    o.defaultTimeoutMs = 20000;
    g_server->Register("core.status", [](const Value&) {
        Value v = Value::object();
        v.set("status", g_status).set("verified", g_verified.load()).set("frames", static_cast<double>(g_frames.load()));
        v.set("gameThread", static_cast<double>(g_game_thread.load())).set("dt", g_last_dt.load()).set("report", g_verify_report);
        return v;
    }, false);
    // Read-only diagnostics, served on the transport thread (they work while the mod is off).
    g_server->Register("diag.path", [](const Value& p) { return Value(ue::addr(ue::find_path(p.get("path").asString()))); }, false);
    g_server->Register("diag.mem", [](const Value& p) {
        uintptr_t a = static_cast<uintptr_t>(strtoull(p.get("addr").asString().c_str(), nullptr, 16));
        int n = static_cast<int>(p.get("size").asInt(0x100));
        Value out = Value::array();
        for (int o = 0; o < n && o < 0x1000; o += 8) {
            const void* q = reinterpret_cast<const void*>(a + o);
            if (!ue::readable(q, 8)) { out.push("unreadable"); break; }
            uint64_t v = *static_cast<const uint64_t*>(q);
            char b[64];
            snprintf(b, sizeof b, "+0x%03x %016llx", o, static_cast<unsigned long long>(v));
            std::string s = b;
            auto* obj = reinterpret_cast<ue::UObject*>(static_cast<uintptr_t>(v));
            if (v > 0x10000 && v < 0x7fffffffffffull && ue::readable(obj, 0x30) && ue::alive(obj)) {
                s += " obj " + ue::class_name(obj) + " " + ue::path_of(obj);
            } else {
                uint32_t idx = static_cast<uint32_t>(v), num = static_cast<uint32_t>(v >> 32);
                if (idx && (idx >> 16) < 8192 && num < 64) {
                    std::string nm = ue::name(ue::FName{idx, num});
                    bool ok = !nm.empty() && nm.size() < 80 && std::all_of(nm.begin(), nm.end(), [](char c) { return c >= 32 && c < 127; });
                    if (ok && nm != "<badname>") s += " name? " + nm;
                }
            }
            out.push(s);
        }
        return out;
    }, false);
    if (g_server->Start(o, *g_adapter)) {
        g_bridge_on = true;
        logmsg("bridge on: pipe '%s' tcp %d, endpoint file %s", pipe.c_str(), o.tcpPort, o.endpointFile.c_str());
    } else logmsg("bridge failed: %s", g_server->lastError().c_str());
}

DWORD WINAPI init_thread(LPVOID) {
    std::string why;
    // Wait for the engine: the name pool and a filled object array (the start-up screens run long before this).
    DWORD t0 = GetTickCount();
    if (!ue::init(g_base, kRvas, why)) { logmsg("init failed: %s", why.c_str()); return 0; }
    while (!ue::ready()) {
        if (GetTickCount() - t0 > 600000) { g_status = "engine never became ready"; logmsg("%s", g_status.c_str()); return 0; }
        Sleep(250);
    }
    if (!fingerprint(why)) {
        g_status = "unsupported build: " + why;
        logmsg("%s; the mod stays off", g_status.c_str());
        start_bridge();
        return 0;
    }
    logmsg("build ok: %s; name pool and object array up after %lu ms, %d objects", why.c_str(), GetTickCount() - t0, ue::num_objects());
    // Script classes get their properties linked during engine init; GEngine exists only after that.
    for (;;) {
        ue::UObject* ge = ue::find_path("/Script/Engine.GameEngine");
        if (ge && !ue::instances(ge, false, 1).empty()) break;
        if (GetTickCount() - t0 > 600000) { g_status = "no GameEngine instance after 10 min"; logmsg("%s", g_status.c_str()); return 0; }
        Sleep(500);
    }
    logmsg("GameEngine instance up after %lu ms, %d objects", GetTickCount() - t0, ue::num_objects());
    bool ok = false;
    for (int attempt = 0; attempt < 6 && !(ok = ue::verify(g_verify_report)); attempt++) Sleep(5000);
    if (!ok) {
        g_status = "layout check failed";
        logmsg("layout check FAILED, the mod stays off:\n%s", g_verify_report.c_str());
        start_bridge();
        return 0;
    }
    logmsg("layouts ok:\n%s", g_verify_report.c_str());
    g_verified = true;
    register_methods();
    std::string err;
    o_tick = reinterpret_cast<TickFn>(hook::install(g_base + kTickRva, kTickBytes, sizeof kTickBytes, reinterpret_cast<void*>(hk_tick), err));
    if (!o_tick) { g_status = "tick hook failed: " + err; logmsg("%s", g_status.c_str()); return 0; }
    o_pe = reinterpret_cast<PeFn>(hook::install(g_base + kRvas.process_event, kPeBytes, sizeof kPeBytes, reinterpret_cast<void*>(hk_pe), err));
    if (!o_pe) { g_status = "ProcessEvent hook failed: " + err; logmsg("%s", g_status.c_str()); }
    o_add_xp = reinterpret_cast<coop::AddXpFn>(hook::install(g_base + kAddXpRva, kAddXpBytes, sizeof kAddXpBytes, reinterpret_cast<void*>(hk_add_xp), err));
    if (o_add_xp) coop::add_xp_original = o_add_xp, coop::add_xp_entry = reinterpret_cast<coop::AddXpFn>(g_base + kAddXpRva);
    else logmsg("XP sharing off: %s", err.c_str());
    reg("coop.xp", [](const Value& p) { return coop::xp_test(static_cast<int>(p.get("xp").asInt(10)), static_cast<int>(p.get("reason").asInt(0))); });  // test
    std::string coop_why;
    reg("coop.status", [](const Value&) { return coop::status(); });
    reg("coop.set", [](const Value& p) { return coop::set_options(p); });
    reg("coop.steamProbe", [](const Value&) { return coop::steam_probe(); }, false);  // waits: off the game thread
    reg("coop.face", [](const Value&) { return coop::face_debug(); });  // test: look texts
    reg("coop.charSave", [](const Value&) { character::save_now(); return character::status(); });  // test: keep the character now
    reg("coop.steamAccept", [](const Value&) {  // test: the host's accept pass over the friends list, on any copy
        Value r = Value::object();
        r.set("on", steamnet::on()).set("friends", static_cast<double>(steamnet::accept_friends()));
        return r;
    }, false);
    // Join: host saves and serves its campaign over TCP (co-op port + 1); the client downloads and loads it.
    reg("coop.offer", [](const Value& p) {
        std::string bind = p.get("bind").asString(env("TOW2_COOP_BIND"));
        // steamSelf (test): push the save over Steam to your own SteamID; coop.steamPump plays the client side
        uint64_t self = p.get("steamSelf").asBool(false) ? steamnet::me() : 0;
        return join::host_offer(static_cast<int>(p.get("port").asInt(27961)), bind.empty() ? "127.0.0.1" : bind, "", self, 0xABCDEF01u);
    });
    // Test (off the game thread): hand Steam channel-1 messages sent to yourself to the client side of the hand-over,
    // which writes CoopHost_ABCDEF01 and then loads it. Use with transport=udp (in Steam mode the game thread reads them).
    reg("coop.steamPump", [](const Value& p) {
        std::string why;
        if (!steamnet::start(why)) throw coopbridge::Error(-32012, why);
        for (ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(p.get("seconds").asNumber(60) * 1000); GetTickCount64() < end; Sleep(20)) {
            bool done = false;
            steamnet::receive(1, [&](uint64_t from, const char* d, int n) {
                if (from != steamnet::me()) return;
                join::on_steam_data(d, n);
                done = done || (n == 1 && d[0] == 'E');
            });
            if (done) break;
        }
        return join::status();
    }, false);
    reg("coop.join", [](const Value& p) {
        return join::client_fetch(p.get("host").asString("127.0.0.1"), static_cast<int>(p.get("port").asInt(27961)),
                                  static_cast<uint32_t>(p.get("hostId").asInt(0)));
    });
    reg("coop.joinStatus", [](const Value&) { return join::status(); }, false);
    reg("coop.refuseLoads", [](const Value& p) { join::refuse_loads(static_cast<int>(p.get("n").asInt(1))); return Value(true); }, false);  // test
    g_coop_on = coop::start(coop_why);
    logmsg("co-op: %s", coop_why.c_str());
    start_bridge();
    g_status = "running";
    logmsg("hooks on (tick %s, ProcessEvent %s)", ue::addr(reinterpret_cast<void*>(o_tick)).c_str(), ue::addr(reinterpret_cast<void*>(o_pe)).c_str());
    return 0;
}
}  // namespace

void core_log(const std::string& line) { logmsg("%s", line.c_str()); }

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    auto* me = reinterpret_cast<uint8_t*>(self);
    g_self_lo = reinterpret_cast<uintptr_t>(me);
    g_self_hi = g_self_lo + reinterpret_cast<IMAGE_NT_HEADERS64*>(me + reinterpret_cast<IMAGE_DOS_HEADER*>(me)->e_lfanew)->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, fault_guard);
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    std::string dir = env("TOW2_SANDBOX_DIR");
    if (dir.empty()) {
        char me[MAX_PATH];
        GetModuleFileNameA(self, me, MAX_PATH);
        dir = me;
        dir = dir.substr(0, dir.find_last_of('\\'));
    }
    g_logpath = dir + "\\tow2coop.log";
    HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
