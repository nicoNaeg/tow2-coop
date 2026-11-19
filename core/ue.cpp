// Unreal Engine 5.4 reflection access (see ue.hpp).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ue.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace ue {
void* g_process_event;
static uint8_t *g_pool, *g_pool_ready, *g_objarr;
static void (*g_fname_ctor)(FName*, const wchar_t*, int);
static int g_item = 0x18;     // sizeof(FUObjectItem), checked in verify()
static int g_cdo_off = -1;    // UClass::ClassDefaultObject, found in verify()
static UClass *g_c_class, *g_c_package, *g_c_function, *g_c_scriptstruct, *g_c_enum, *g_c_engine;
static UFunction *g_f_text_to_string;
static UObject* g_text_lib;

// FField/FProperty member offsets: 16-byte FFieldVariant (stock 5.4) or 8-byte; verify() picks the one that reads.
namespace lay {
int ff_next = 0x20, ff_name = 0x28, prop_dim = 0x38, prop_size = 0x3C, prop_flags = 0x40, prop_offset = 0x4C, prop_extra = 0x78;
int array_inner = 0;  // FArrayProperty::Inner after prop_extra: 0 (stock) or 8 (EArrayPropertyFlags first), found in verify()
}
template <class T> static T at(const void* p, int off) { return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(p) + off); }

static std::string utf8(const wchar_t* w, int n) {
    if (!w || n <= 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, &s[0], len, nullptr, nullptr);
    return s;
}
static std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], len);
    return w;
}
static bool ieq(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
}
static bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
    return it != hay.end();
}
std::string addr(const void* p) {
    char b[32];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)(uintptr_t)p);
    return b;
}

// ------------------------------------------------------------------ setup
bool init(uint8_t* base, const Rvas& r, std::string& err) {
    g_objarr = base + r.objarray;
    g_pool = base + r.namepool;
    g_pool_ready = base + r.namepool_ready;
    g_fname_ctor = reinterpret_cast<void (*)(FName*, const wchar_t*, int)>(base + r.fname_ctor);
    g_process_event = base + r.process_event;
    err.clear();
    return true;
}

bool ready() {
    if (!*g_pool_ready) return false;
    int n = num_objects();
    return n > 50000 && object_at(0) != nullptr;
}

// ------------------------------------------------------------------ names
static std::mutex g_name_mu;
static std::unordered_map<uint32_t, std::string> g_name_cache;

static std::string entry_string(uint32_t idx) {
    uint8_t** blocks = reinterpret_cast<uint8_t**>(g_pool + 0x10);
    uint32_t block = idx >> 16, offs = (idx & 0xFFFF) * 2;
    if (block >= 8192 || !blocks[block]) return "<badname>";
    const uint8_t* e = blocks[block] + offs;
    uint16_t hdr = *reinterpret_cast<const uint16_t*>(e);
    int len = hdr >> 6;
    if (offs + 2 + len * ((hdr & 1) ? 2 : 1) > 0x20000) return "<badname>";  // 128 KiB blocks
    if (hdr & 1) return utf8(reinterpret_cast<const wchar_t*>(e + 2), len);
    return std::string(reinterpret_cast<const char*>(e + 2), len);
}

std::string name(FName n) {
    std::string s;
    {
        std::lock_guard<std::mutex> g(g_name_mu);
        auto it = g_name_cache.find(n.index);
        if (it != g_name_cache.end()) s = it->second;
        else s = g_name_cache.emplace(n.index, entry_string(n.index)).first->second;
    }
    if (n.number) s += "_" + std::to_string(n.number - 1);
    return s;
}

std::string name_of(const UObject* o) { return o ? name(at<FName>(o, off::obj_name)) : "None"; }

FName make_name(const std::string& s, bool add) {
    std::wstring w = wide(s);
    FName n;
    g_fname_ctor(&n, w.c_str(), add ? 1 : 0);
    return n;
}

// ------------------------------------------------------------------ objects
int num_objects() { return at<int32_t>(g_objarr, 0x10 + 0x14); }

static uint8_t* item(int i) {
    if (i < 0 || i >= num_objects()) return nullptr;
    uint8_t** chunks = at<uint8_t**>(g_objarr, 0x10);
    if (!chunks || !chunks[i >> 16]) return nullptr;
    return chunks[i >> 16] + static_cast<size_t>(i & 0xFFFF) * g_item;
}

UObject* object_at(int i) {
    uint8_t* it = item(i);
    return it ? *reinterpret_cast<UObject**>(it) : nullptr;
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t* q = static_cast<const uint8_t*>(p);
    while (n) {
        if (!VirtualQuery(q, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD) || !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        size_t left = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - q;
        if (left >= n) return true;
        q += left;
        n -= left;
    }
    return true;
}

// Memory known readable this frame: committed regions from VirtualQuery. The engine frees and decommits memory in its
// own tick (garbage collection, streaming), never while co-op code runs, so a region stays good until the next epoch.
namespace {
struct Region { uintptr_t lo = 0, hi = 0; uint32_t epoch = 0; };
Region g_regions[32];
uint32_t g_epoch = 1;
int g_next_region = 0;
}  // namespace

void new_epoch() { g_epoch++; }

static bool held_readable(const void* p, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p), e = a + n;
    for (const Region& r : g_regions)
        if (r.epoch == g_epoch && a >= r.lo && e <= r.hi) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD) || !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
    if (e > hi) return readable(p, n);  // across two regions
    g_regions[g_next_region] = {lo, hi, g_epoch};
    g_next_region = (g_next_region + 1) % 32;
    return true;
}

bool alive(const UObject* o) {
    if (!o) return false;
    if (!held_readable(o, 0x28)) return false;  // a held pointer whose memory went (run 99: an NPC streamed out)
    uint8_t* it = item(at<int32_t>(o, off::obj_index));
    if (!it || *reinterpret_cast<UObject**>(it) != o) return false;
    if (at<int32_t>(it, 8) & (1 << 21)) return false;                     // EInternalObjectFlags::Garbage
    return !(at<uint32_t>(o, off::obj_flags) & (0x8000 | 0x10000));        // RF_BeginDestroyed | RF_FinishDestroyed
}

UClass* class_of(const UObject* o) { return o ? at<UClass*>(o, off::obj_class) : nullptr; }
UObject* outer_of(const UObject* o) { return o ? at<UObject*>(o, off::obj_outer) : nullptr; }
std::string class_name(const UObject* o) { return name_of(class_of(o)); }
bool is_cdo(const UObject* o) { return o && (at<uint32_t>(o, off::obj_flags) & 0x10); }  // RF_ClassDefaultObject
UStruct* super_of(const UStruct* s) { return s ? at<UStruct*>(s, off::struct_super) : nullptr; }

bool is_a(const UObject* o, const UClass* c) {
    for (const UStruct* s = class_of(o); s; s = super_of(s))
        if (s == c) return true;
    return false;
}

std::string path_of(const UObject* o) {
    if (!o) return "None";
    std::vector<const UObject*> chain;
    for (const UObject* p = o; p && chain.size() < 64; p = outer_of(p)) chain.push_back(p);
    std::string s = name_of(chain.back());
    for (int i = static_cast<int>(chain.size()) - 2; i >= 0; i--) {
        const UObject* outer = chain[i + 1];
        const UObject* outer2 = outer_of(outer);
        bool sub = class_of(outer) != g_c_package && outer2 && class_of(outer2) == g_c_package;
        s += sub ? ":" : ".";
        s += name_of(chain[i]);
    }
    return s;
}

static bool same_name(const UObject* o, FName n) {
    FName m = at<FName>(o, off::obj_name);
    return m.index == n.index && m.number == n.number;
}

UObject* find_path(const std::string& path) {
    size_t cut = path.find_last_of(".:");
    std::string shortname = cut == std::string::npos ? path : path.substr(cut + 1);
    FName n = make_name(shortname, false);
    if (!n.index && !ieq(shortname, "None")) return nullptr;
    int count = num_objects();
    for (int i = 0; i < count; i++) {
        UObject* o = object_at(i);
        if (o && same_name(o, n) && ieq(path_of(o), path)) return o;
    }
    return nullptr;
}

UClass* find_class(const std::string& s) {
    if (!s.empty() && s[0] == '/') {
        UObject* o = find_path(s);
        return o && is_a(o, g_c_class) ? o : nullptr;
    }
    FName n = make_name(s, false);
    if (!n.index) return nullptr;
    UClass* best = nullptr;
    int count = num_objects();
    for (int i = 0; i < count; i++) {
        UObject* o = object_at(i);
        if (!o || !same_name(o, n) || !is_a(o, g_c_class)) continue;
        if (path_of(o).rfind("/Script/", 0) == 0) return o;  // prefer native classes
        if (!best) best = o;
    }
    return best;
}

std::vector<UObject*> instances(const UClass* c, bool include_cdo, size_t limit) {
    std::vector<UObject*> out;
    int count = num_objects();
    for (int i = 0; i < count && out.size() < limit; i++) {
        UObject* o = object_at(i);
        if (o && is_a(o, c) && alive(o) && (include_cdo || !is_cdo(o))) out.push_back(o);
    }
    return out;
}

UObject* cdo(const UClass* c) { return c && g_cdo_off > 0 ? at<UObject*>(c, g_cdo_off) : nullptr; }

// ------------------------------------------------------------------ reflection
FField* first_prop(const UStruct* s) { return s ? at<FField*>(s, off::struct_props) : nullptr; }
FField* next_prop(const FField* f) { return at<FField*>(f, lay::ff_next); }
std::string field_name(const FField* f) { return name(at<FName>(f, lay::ff_name)); }
std::string field_type(const FField* f) { return name(*reinterpret_cast<const FName*>(at<const uint8_t*>(f, off::ff_class))); }
int prop_offset(const FProperty* p) { return at<int32_t>(p, lay::prop_offset); }
int prop_dim(const FProperty* p) { return at<int32_t>(p, lay::prop_dim); }
int prop_size(const FProperty* p) { return at<int32_t>(p, lay::prop_size); }
uint64_t prop_flags(const FProperty* p) { return at<uint64_t>(p, lay::prop_flags); }
int struct_size(const UStruct* s) { return at<int32_t>(s, off::struct_size); }
template <class T> static T* extra(const FProperty* p, int n = 0) { return at<T*>(p, lay::prop_extra + 8 * n); }

FProperty* find_prop(const UStruct* s, const std::string& nm) {
    FName n = make_name(nm, false);
    for (; s; s = super_of(s))
        for (FField* f = first_prop(s); f; f = next_prop(f)) {
            FName m = at<FName>(f, lay::ff_name);
            if (n.index ? (m.index == n.index && m.number == n.number) : ieq(name(m), nm)) return f;
        }
    return nullptr;
}

UFunction* find_func(const UStruct* s, const std::string& nm) {
    FName n = make_name(nm, false);
    if (!n.index) return nullptr;
    for (; s; s = super_of(s))
        for (UObject* f = at<UObject*>(s, off::struct_children); f; f = at<UObject*>(f, off::field_next))
            if (same_name(f, n) && is_a(f, g_c_function)) return f;
    return nullptr;
}

static const FProperty* inner_of(const FProperty* p) { return at<FField*>(p, lay::prop_extra + lay::array_inner); }

UStruct* struct_of(const FProperty* p) { return p && field_type(p) == "StructProperty" ? extra<UObject>(p) : nullptr; }
FProperty* array_inner(const FProperty* p) { return p && field_type(p) == "ArrayProperty" ? const_cast<FProperty*>(inner_of(p)) : nullptr; }

static std::string type_detail(const FProperty* p) {
    if (!p || !readable(p, 0x40)) return "?";
    std::string t = field_type(p);
    if (t == "ArrayProperty") return t + "<" + type_detail(inner_of(p)) + ">";
    if (t == "StructProperty") return t + "<" + name_of(extra<UObject>(p)) + ">";
    if (t == "ObjectProperty" || t == "WeakObjectProperty" || t == "SoftObjectProperty" || t == "LazyObjectProperty" || t == "InterfaceProperty")
        return t + "<" + name_of(extra<UObject>(p)) + ">";
    if (t == "ClassProperty" || t == "SoftClassProperty") return t + "<" + name_of(extra<UObject>(p, 1)) + ">";
    if (t == "ByteProperty" && extra<UObject>(p)) return t + "<" + name_of(extra<UObject>(p)) + ">";
    if (t == "EnumProperty") return t + "<" + name_of(extra<UObject>(p, 1)) + ">";
    if (t == "SetProperty") return t + "<" + type_detail(extra<FField>(p)) + ">";
    if (t == "MapProperty") return t + "<" + type_detail(extra<FField>(p)) + "," + type_detail(extra<FField>(p, 1)) + ">";
    return t;
}

json describe_struct(const UStruct* s, bool with_functions) {
    json d = json::object();
    d.set("name", name_of(s)).set("path", path_of(s)).set("class", class_name(s));
    d.set("super", path_of(super_of(s))).set("size", struct_size(s));
    json props = json::array();
    for (FField* f = first_prop(s); f; f = next_prop(f)) {
        json p = json::object();
        char fl[24];
        snprintf(fl, sizeof fl, "0x%llx", (unsigned long long)prop_flags(f));
        p.set("name", field_name(f)).set("type", type_detail(f)).set("offset", prop_offset(f)).set("size", prop_size(f)).set("flags", fl);
        props.push(p);
    }
    d.set("props", props);
    if (with_functions) {
        json fns = json::array();
        for (UObject* f = at<UObject*>(s, off::struct_children); f; f = at<UObject*>(f, off::field_next)) {
            if (!is_a(f, g_c_function)) continue;
            json fn = json::object();
            char fl[16];
            snprintf(fl, sizeof fl, "0x%x", at<uint32_t>(f, off::func_flags));
            fn.set("name", name_of(f)).set("flags", fl).set("parmsSize", at<uint16_t>(f, off::func_parmssize));
            json params = json::array();
            for (FField* p = first_prop(f); p; p = next_prop(p)) {
                uint64_t pf = prop_flags(p);
                if (!(pf & CPF_Parm)) continue;
                json pj = json::object();
                pj.set("name", field_name(p)).set("type", type_detail(p)).set("offset", prop_offset(p));
                pj.set("out", (pf & CPF_OutParm) && !(pf & CPF_ConstParm)).set("ret", (pf & CPF_ReturnParm) != 0);
                params.push(pj);
            }
            fn.set("params", params);
            fns.push(fn);
        }
        d.set("functions", fns);
    }
    return d;
}

// ------------------------------------------------------------------ values
static std::string enum_name(const UObject* e, int64_t v) {
    if (!e) return std::to_string(v);
    auto names = at<TArray<uint8_t>>(e, off::enum_names);  // TPair<FName, int64>, 16 bytes each
    for (int i = 0; i < names.num && i < 4096; i++) {
        const uint8_t* pair = names.data + 16 * i;
        if (at<int64_t>(pair, 8) == v) {
            std::string s = name(at<FName>(pair, 0));
            size_t c = s.rfind("::");
            return c == std::string::npos ? s : s.substr(c + 2);
        }
    }
    return std::to_string(v);
}

static bool enum_value(const UObject* e, const std::string& s, int64_t& v) {
    auto names = at<TArray<uint8_t>>(e, off::enum_names);
    for (int i = 0; i < names.num && i < 4096; i++) {
        const uint8_t* pair = names.data + 16 * i;
        std::string n = name(at<FName>(pair, 0));
        size_t c = n.rfind("::");
        if (ieq(n, s) || (c != std::string::npos && ieq(n.substr(c + 2), s))) {
            v = at<int64_t>(pair, 8);
            return true;
        }
    }
    return false;
}

static std::string object_ref(const UObject* o) {
    if (!o) return "None";
    if (!readable(o, 0x30) || !alive(o)) return addr(o) + " <dead>";
    return addr(o) + " " + class_name(o) + " " + path_of(o);
}

static UObject* weak_get(const uint8_t* a) {
    int32_t idx = at<int32_t>(a, 0), serial = at<int32_t>(a, 4);
    uint8_t* it = item(idx);
    if (!it || !serial || at<int32_t>(it, 0x10) != serial) return nullptr;
    return *reinterpret_cast<UObject**>(it);
}

static std::string text_to_string(const uint8_t* text) {
    if (!g_f_text_to_string || !g_text_lib) return "<text>";
    alignas(16) uint8_t buf[96] = {};
    int size = at<uint16_t>(g_f_text_to_string, off::func_parmssize), ret = at<uint16_t>(g_f_text_to_string, off::func_retoffset);
    if (size > 96 || ret + 16 > size) return "<text?>";
    memcpy(buf, text, ret);  // FText by value: a bitwise view, never destroyed by the call
    process_event(g_text_lib, g_f_text_to_string, buf);
    FString* s = reinterpret_cast<FString*>(buf + ret);
    // shortcut: the returned FString is leaked (no FMemory::Free yet); fine for a debug read, not for per-frame use
    return utf8(s->data, s->num > 0 ? s->num - 1 : 0);
}

static json read_one(const FProperty* p, const std::string& t, const uint8_t* a, int depth);

json read_value(const FProperty* p, const uint8_t* a, int depth) {
    std::string t = field_type(p);
    int dim = at<int32_t>(p, lay::prop_dim);
    if (dim <= 1) return read_one(p, t, a, depth);
    json arr = json::array();
    for (int i = 0; i < dim && i < 64; i++) arr.push(read_one(p, t, a + static_cast<size_t>(i) * prop_size(p), depth));
    return arr;
}

static json read_struct(const UStruct* s, const uint8_t* a, int depth) {
    if (depth < 0) return "<" + name_of(s) + ">";
    json o = json::object();
    for (const UStruct* k = s; k; k = super_of(k))
        for (FField* f = first_prop(k); f; f = next_prop(f)) o.set(field_name(f), read_value(f, a + prop_offset(f), depth - 1));
    return o;
}

static json read_one(const FProperty* p, const std::string& t, const uint8_t* a, int depth) {
    if (t == "BoolProperty") return (a[at<uint8_t>(p, lay::prop_extra + 1)] & at<uint8_t>(p, lay::prop_extra + 3)) != 0;
    if (t == "ByteProperty") {
        const UObject* e = extra<UObject>(p);
        return e ? json(enum_name(e, a[0])) : json(a[0]);
    }
    if (t == "Int8Property") return at<int8_t>(a, 0);
    if (t == "Int16Property") return at<int16_t>(a, 0);
    if (t == "UInt16Property") return at<uint16_t>(a, 0);
    if (t == "IntProperty") return at<int32_t>(a, 0);
    if (t == "UInt32Property") return at<uint32_t>(a, 0);
    if (t == "Int64Property") return static_cast<double>(at<int64_t>(a, 0));
    if (t == "UInt64Property") return static_cast<double>(at<uint64_t>(a, 0));
    if (t == "FloatProperty") return at<float>(a, 0);
    if (t == "DoubleProperty") return at<double>(a, 0);
    if (t == "NameProperty") return name(at<FName>(a, 0));
    if (t == "StrProperty") {
        const FString& s = *reinterpret_cast<const FString*>(a);
        return utf8(s.data, s.num > 0 ? s.num - 1 : 0);
    }
    if (t == "TextProperty") return text_to_string(a);
    if (t == "ObjectProperty" || t == "ClassProperty" || t == "InterfaceProperty") return object_ref(at<UObject*>(a, 0));
    if (t == "WeakObjectProperty" || t == "LazyObjectProperty") return object_ref(weak_get(a));
    if (t == "SoftObjectProperty" || t == "SoftClassProperty") {
        std::string s = name(at<FName>(a, 8)) + "." + name(at<FName>(a, 16));
        const FString& sub = *reinterpret_cast<const FString*>(a + 24);
        if (sub.num > 1) s += ":" + utf8(sub.data, sub.num - 1);
        UObject* live = weak_get(a);
        return live ? s + " (loaded " + addr(live) + ")" : s;
    }
    if (t == "StructProperty") return read_struct(extra<UObject>(p), a, depth);
    if (t == "EnumProperty") {
        const FProperty* under = extra<FField>(p);
        int sz = prop_size(under);
        int64_t v = sz == 1 ? a[0] : sz == 2 ? at<int16_t>(a, 0) : sz == 4 ? at<int32_t>(a, 0) : at<int64_t>(a, 0);
        return enum_name(extra<UObject>(p, 1), v);
    }
    if (t == "ArrayProperty") {
        const FProperty* inner = inner_of(p);
        auto arr = at<TArray<uint8_t>>(a, 0);
        json out = json::array();
        if (depth < 0) return "<array n=" + std::to_string(arr.num) + ">";
        int n = std::min(arr.num, 256);
        for (int i = 0; i < n; i++) out.push(read_value(inner, arr.data + static_cast<size_t>(i) * prop_size(inner), depth - 1));
        if (arr.num > n) out.push("... " + std::to_string(arr.num - n) + " more");
        return out;
    }
    return "<" + t + ">";
}

json read_object(const UObject* o, int depth, const std::string& filter) {
    json out = json::object();
    out.set("$ref", object_ref(o));
    for (const UStruct* s = class_of(o); s; s = super_of(s))
        for (FField* f = first_prop(s); f; f = next_prop(f)) {
            std::string n = field_name(f);
            if (icontains(n, filter)) out.set(n, read_value(f, reinterpret_cast<const uint8_t*>(o) + prop_offset(f), depth));
        }
    return out;
}

struct Keep {
    std::vector<std::wstring> strings;
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
};

static void write_one(const FProperty* p, const std::string& t, uint8_t* a, const json& v, Keep& keep, bool param);

static void write_struct(const UStruct* s, uint8_t* a, const json& v, Keep& keep, bool param) {
    if (v.isObject()) {
        for (const auto& kv : v.members()) {
            FProperty* f = find_prop(s, kv.first);
            if (!f) throw std::runtime_error("struct " + name_of(s) + " has no member " + kv.first);
            write_one(f, field_type(f), a + prop_offset(f), kv.second, keep, param);
        }
        return;
    }
    if (v.isArray()) {  // members in declaration order, e.g. [x, y, z] for a vector
        std::vector<FField*> fields;
        for (FField* f = first_prop(s); f; f = next_prop(f)) fields.push_back(f);
        if (v.items().size() > fields.size()) throw std::runtime_error("too many values for struct " + name_of(s));
        for (size_t i = 0; i < v.items().size(); i++) write_one(fields[i], field_type(fields[i]), a + prop_offset(fields[i]), v.items()[i], keep, param);
        return;
    }
    throw std::runtime_error("struct " + name_of(s) + " needs an object or an array");
}

static void write_one(const FProperty* p, const std::string& t, uint8_t* a, const json& v, Keep& keep, bool param) {
    auto num = [&]() -> double {
        if (v.isNumber() || v.isBool()) return v.asNumber();
        if (v.isString()) return strtod(v.str().c_str(), nullptr);
        throw std::runtime_error(field_name(p) + ": expected a number");
    };
    if (t == "BoolProperty") {
        uint8_t off = at<uint8_t>(p, lay::prop_extra + 1), mask = at<uint8_t>(p, lay::prop_extra + 3);
        bool b = v.isBool() ? v.asBool() : num() != 0;
        a[off] = b ? (a[off] | mask) : (a[off] & ~mask);
        return;
    }
    if (t == "ByteProperty" || t == "EnumProperty") {
        const UObject* e = t == "ByteProperty" ? extra<UObject>(p) : extra<UObject>(p, 1);
        int64_t x = 0;
        if (v.isString() && e) {
            if (!enum_value(e, v.str(), x)) throw std::runtime_error(field_name(p) + ": no enum value " + v.str());
        } else x = static_cast<int64_t>(num());
        int sz = t == "ByteProperty" ? 1 : prop_size(extra<FField>(p));
        memcpy(a, &x, sz);  // little endian: the low bytes
        return;
    }
    if (t == "Int8Property") { int8_t x = (int8_t)num(); memcpy(a, &x, 1); return; }
    if (t == "Int16Property" || t == "UInt16Property") { int16_t x = (int16_t)num(); memcpy(a, &x, 2); return; }
    if (t == "IntProperty" || t == "UInt32Property") { int32_t x = (int32_t)(int64_t)num(); memcpy(a, &x, 4); return; }
    if (t == "Int64Property" || t == "UInt64Property") { int64_t x = (int64_t)num(); memcpy(a, &x, 8); return; }
    if (t == "FloatProperty") { float x = (float)num(); memcpy(a, &x, 4); return; }
    if (t == "DoubleProperty") { double x = num(); memcpy(a, &x, 8); return; }
    if (t == "NameProperty") { FName n = make_name(v.asString(), true); memcpy(a, &n, 8); return; }
    if (t == "StrProperty") {
        if (!param) throw std::runtime_error(field_name(p) + ": strings can only be passed as call arguments");
        keep.strings.push_back(wide(v.asString()));
        std::wstring& w = keep.strings.back();
        FString s{&w[0], static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
        if (w.empty()) s = FString{};
        memcpy(a, &s, sizeof s);
        return;
    }
    if (t == "ObjectProperty" || t == "ClassProperty") {
        UObject* o = nullptr;
        if (!v.isNull() && !(v.isString() && (v.str().empty() || ieq(v.str(), "None") || ieq(v.str(), "null")))) {
            std::string err;
            o = resolve(v.asString(), &err);
            if (!o) throw std::runtime_error(field_name(p) + ": " + err);
            const UObject* want = extra<UObject>(p);
            if (want && !is_a(o, want)) throw std::runtime_error(field_name(p) + ": " + path_of(o) + " is not a " + name_of(want));
            if (t == "ClassProperty") {
                const UObject* meta = extra<UObject>(p, 1);
                for (const UStruct* s = o; meta; s = super_of(s))
                    if (!s) throw std::runtime_error(field_name(p) + ": class " + path_of(o) + " is not a " + name_of(meta));
                    else if (s == meta) break;
            }
        }
        memcpy(a, &o, 8);
        return;
    }
    if (t == "SoftObjectProperty" || t == "SoftClassProperty") {
        std::string s = v.asString();  // /Game/Pkg/Asset.Asset
        size_t dot = s.find('.');
        FName pkg = make_name(dot == std::string::npos ? s : s.substr(0, dot), true);
        FName asset = make_name(dot == std::string::npos ? "" : s.substr(dot + 1), true);
        memset(a, 0, 0x28);
        memcpy(a + 8, &pkg, 8);
        memcpy(a + 16, &asset, 8);
        return;
    }
    if (t == "StructProperty") { write_struct(extra<UObject>(p), a, v, keep, param); return; }
    if (t == "ArrayProperty") {
        if (!param) throw std::runtime_error(field_name(p) + ": arrays can only be passed as call arguments");
        if (!v.isArray()) throw std::runtime_error(field_name(p) + ": expected an array");
        const FProperty* inner = inner_of(p);
        size_t n = v.items().size(), es = prop_size(inner);
        keep.blocks.emplace_back(new uint8_t[n * es + 16]());
        uint8_t* data = keep.blocks.back().get();
        for (size_t i = 0; i < n; i++) write_one(inner, field_type(inner), data + i * es, v.items()[i], keep, param);
        TArray<uint8_t> arr{n ? data : nullptr, static_cast<int32_t>(n), static_cast<int32_t>(n)};
        memcpy(a, &arr, sizeof arr);
        return;
    }
    throw std::runtime_error(field_name(p) + ": cannot write a " + t);
}

void write_value(const FProperty* p, uint8_t* a, const json& v, std::vector<std::wstring>& strings) {
    Keep keep;
    write_one(p, field_type(p), a, v, keep, false);
    for (auto& s : keep.strings) strings.push_back(std::move(s));
}

// ------------------------------------------------------------------ calls
void process_event(UObject* o, UFunction* f, void* parms) {
    using PE = void (*)(UObject*, UFunction*, void*);
    reinterpret_cast<PE>((*reinterpret_cast<void***>(o))[off::pe_slot])(o, f, parms);
}

static const json* find_ci(const json& obj, const std::string& key) {
    for (const auto& kv : obj.members())
        if (ieq(kv.first, key)) return &kv.second;
    return nullptr;
}

json call(UObject* o, UFunction* f, const json& args) {
    int psize = at<uint16_t>(f, off::func_parmssize);
    std::unique_ptr<uint8_t[]> raw(new uint8_t[psize + 48]());
    uint8_t* buf = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw.get()) + 15) & ~static_cast<uintptr_t>(15));
    Keep keep;
    size_t positional = 0;
    for (FField* p = first_prop(f); p; p = next_prop(p)) {
        uint64_t fl = prop_flags(p);
        if (!(fl & CPF_Parm) || (fl & CPF_ReturnParm)) continue;
        const json* v = nullptr;
        if (args.isObject()) v = find_ci(args, field_name(p));
        else if (args.isArray() && positional < args.items().size()) v = &args.items()[positional++];
        if (v) write_one(p, field_type(p), buf + prop_offset(p), *v, keep, true);
    }
    process_event(o, f, buf);
    json out = json::object();
    for (FField* p = first_prop(f); p; p = next_prop(p)) {
        uint64_t fl = prop_flags(p);
        if ((fl & CPF_ReturnParm) || ((fl & CPF_OutParm) && !(fl & CPF_ConstParm)))
            out.set(field_name(p), read_value(p, buf + prop_offset(p), 3));
    }
    return out;
}

// ------------------------------------------------------------------ references and paths
static UObject* engine() {
    static UObject* cached;
    if (cached && alive(cached)) return cached;
    auto v = g_c_engine ? instances(g_c_engine, false, 1) : std::vector<UObject*>();
    cached = v.empty() ? nullptr : v[0];
    return cached;
}

static UObject* obj_prop(UObject* o, const char* prop) {
    if (!o) return nullptr;
    FProperty* p = find_prop(class_of(o), prop);
    if (!p) return nullptr;
    UObject* v = at<UObject*>(o, prop_offset(p));
    return alive(v) ? v : nullptr;
}

static UObject* root(const std::string& r) {
    if (r == "engine") return engine();
    if (r == "viewport") return obj_prop(engine(), "GameViewport");
    if (r == "world") return obj_prop(obj_prop(engine(), "GameViewport"), "World");
    UObject* world = obj_prop(obj_prop(engine(), "GameViewport"), "World");
    if (r == "gi") return obj_prop(world, "OwningGameInstance");
    if (r == "gm") return obj_prop(world, "AuthorityGameMode");
    if (r == "gs") return obj_prop(world, "GameState");
    if (r == "pc" || r == "pawn" || r == "lp") {
        UObject* gi = obj_prop(world, "OwningGameInstance");
        FProperty* lp = gi ? find_prop(class_of(gi), "LocalPlayers") : nullptr;
        if (!lp) return nullptr;
        auto arr = at<TArray<UObject*>>(gi, prop_offset(lp));
        if (arr.num < 1 || !alive(arr.data[0])) return nullptr;
        if (r == "lp") return arr.data[0];
        UObject* pc = obj_prop(arr.data[0], "PlayerController");
        return r == "pc" ? pc : obj_prop(pc, "Pawn");
    }
    return nullptr;
}

// Walks root.Prop[2].Sub.Fn() and hands the final (property, address) or object to `last`.
struct Place {
    const FProperty* prop = nullptr;  // null when the place is a whole object
    uint8_t* addr = nullptr;
    UObject* obj = nullptr;
    json value;                       // set when the last step was a function call
    bool has_value = false;
};

static Place navigate(const std::string& expr) {
    // "/Path/To.Object#Prop.Sub" for object paths (they contain dots themselves), "root.Prop.Sub" otherwise
    std::vector<std::string> steps;
    std::string rest = expr;
    if (!expr.empty() && expr[0] == '/') {
        size_t h = expr.find('#');
        steps.push_back(expr.substr(0, h));
        rest = h == std::string::npos ? std::string() : expr.substr(h + 1);
    }
    if (!rest.empty() || steps.empty()) {
        std::string cur;
        int paren = 0;
        for (char c : rest) {
            if (c == '(') paren++;
            if (c == ')') paren--;
            if (c == '.' && paren == 0) { steps.push_back(cur); cur.clear(); }
            else cur += c;
        }
        steps.push_back(cur);
    }
    Place pl;
    const std::string& r = steps[0];
    if (r.rfind("0x", 0) == 0) {
        UObject* o = reinterpret_cast<UObject*>(static_cast<uintptr_t>(strtoull(r.c_str(), nullptr, 16)));
        if (!readable(o, 0x30) || !alive(o)) throw std::runtime_error("not a live object: " + r);
        pl.obj = o;
    } else if (!r.empty() && r[0] == '/') {
        pl.obj = find_path(r);
        if (!pl.obj) throw std::runtime_error("no object " + r);
    } else {
        pl.obj = root(r);
        if (!pl.obj) throw std::runtime_error("root " + r + " is not available now");
    }
    const UStruct* type = class_of(pl.obj);
    uint8_t* base = reinterpret_cast<uint8_t*>(pl.obj);
    for (size_t i = 1; i < steps.size(); i++) {
        std::string s = steps[i];
        if (pl.prop) {  // step into the previous property's value
            std::string t = field_type(pl.prop);
            if (t == "ObjectProperty" || t == "ClassProperty" || t == "InterfaceProperty" || t == "WeakObjectProperty") {
                UObject* o = t == "WeakObjectProperty" ? weak_get(pl.addr) : at<UObject*>(pl.addr, 0);
                if (!o || !alive(o)) throw std::runtime_error("null or dead object at " + steps[i - 1]);
                pl = Place();
                pl.obj = o;
                type = class_of(o);
                base = reinterpret_cast<uint8_t*>(o);
            } else if (t == "StructProperty") {
                type = extra<UObject>(pl.prop);
                base = pl.addr;
                pl.prop = nullptr;
            } else throw std::runtime_error("cannot step into " + steps[i - 1] + " (" + t + ")");
        } else if (pl.has_value) throw std::runtime_error("cannot step into a call result: " + steps[i - 1]);
        if (s.size() > 2 && s.compare(s.size() - 2, 2, "()") == 0) {  // zero-argument function call
            if (!pl.obj || base != reinterpret_cast<uint8_t*>(pl.obj)) throw std::runtime_error(s + ": calls need an object");
            UFunction* f = find_func(type, s.substr(0, s.size() - 2));
            if (!f) throw std::runtime_error("no function " + s);
            json res = call(pl.obj, f, json::object());
            pl.value = res.get("ReturnValue");
            pl.has_value = true;
            if (i + 1 < steps.size()) {  // an object result can be stepped into
                std::string rv = pl.value.asString();
                UObject* o = rv.rfind("0x", 0) == 0 ? reinterpret_cast<UObject*>(static_cast<uintptr_t>(strtoull(rv.c_str(), nullptr, 16))) : nullptr;
                if (!o || !alive(o)) throw std::runtime_error(s + " did not return a live object");
                pl = Place();
                pl.obj = o;
                type = class_of(o);
                base = reinterpret_cast<uint8_t*>(o);
            }
            continue;
        }
        int index = -1;
        size_t br = s.find('[');
        if (br != std::string::npos) {
            index = atoi(s.c_str() + br + 1);
            s = s.substr(0, br);
        }
        const FProperty* p = find_prop(type, s);
        if (!p) throw std::runtime_error(name_of(type) + " has no property " + s);
        uint8_t* a = base + prop_offset(p);
        if (index >= 0) {
            if (field_type(p) == "ArrayProperty") {
                auto arr = at<TArray<uint8_t>>(a, 0);
                if (index >= arr.num) throw std::runtime_error(s + "[" + std::to_string(index) + "] out of range (" + std::to_string(arr.num) + ")");
                p = inner_of(p);
                a = arr.data + static_cast<size_t>(index) * prop_size(p);
            } else {
                if (index >= at<int32_t>(p, lay::prop_dim)) throw std::runtime_error(s + " index out of range");
                a += static_cast<size_t>(index) * prop_size(p);
            }
        }
        pl.prop = p;
        pl.addr = a;
    }
    return pl;
}

UObject* resolve(const std::string& ref, std::string* err) {
    try {
        Place pl = navigate(ref);
        if (pl.has_value) {
            std::string rv = pl.value.asString();
            UObject* o = rv.rfind("0x", 0) == 0 ? reinterpret_cast<UObject*>(static_cast<uintptr_t>(strtoull(rv.c_str(), nullptr, 16))) : nullptr;
            if (o && alive(o)) return o;
            throw std::runtime_error(ref + " is not an object");
        }
        if (!pl.prop) return pl.obj;
        std::string t = field_type(pl.prop);
        UObject* o = t == "WeakObjectProperty" ? weak_get(pl.addr)
                     : (t == "ObjectProperty" || t == "ClassProperty" || t == "InterfaceProperty") ? at<UObject*>(pl.addr, 0) : nullptr;
        if (!o || !alive(o)) throw std::runtime_error(ref + " is not a live object");
        return o;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return nullptr;
    }
}

json eval_path(const std::string& expr) {
    Place pl = navigate(expr);
    if (pl.has_value) return pl.value;
    if (!pl.prop) return object_ref(pl.obj);
    return read_value(pl.prop, pl.addr, 3);
}

json set_path(const std::string& expr, const json& v) {
    Place pl = navigate(expr);
    if (!pl.prop) throw std::runtime_error("set needs a property path, not an object");
    Keep keep;
    write_one(pl.prop, field_type(pl.prop), pl.addr, v, keep, false);
    return read_value(pl.prop, pl.addr, 3);
}

// ------------------------------------------------------------------ verification
bool verify(std::string& report) {
    char line[256];
    report.clear();
    // FUObjectItem stride: every object must sit at its own InternalIndex.
    int n = num_objects();
    int good = 0;
    for (int stride : {0x18, 0x10, 0x20}) {
        g_item = stride;
        good = 0;
        for (int i = 0; i < n && i < 2000; i++) {
            UObject* o = object_at(i);
            if (o && at<int32_t>(o, off::obj_index) == i) good++;
        }
        if (good > 1900) break;
    }
    snprintf(line, sizeof line, "objects %d, item stride 0x%x (%d/2000 at their index)\n", n, g_item, good);
    report += line;
    if (good <= 1900) return false;
    // Core classes by path.
    UObject* obj_class = nullptr;
    for (int i = 0; i < 4000 && i < n; i++) {
        UObject* o = object_at(i);
        if (o && name_of(o) == "Object" && name_of(outer_of(o)) == "/Script/CoreUObject") { obj_class = o; break; }
    }
    if (!obj_class) { report += "no /Script/CoreUObject.Object\n"; return false; }
    g_c_class = class_of(obj_class);  // the class of a class is UClass
    if (name_of(g_c_class) != "Class") { report += "class of Object is " + name_of(g_c_class) + "\n"; return false; }
    g_c_package = class_of(outer_of(obj_class));
    g_c_function = find_class("/Script/CoreUObject.Function");
    g_c_scriptstruct = find_class("/Script/CoreUObject.ScriptStruct");
    g_c_enum = find_class("/Script/CoreUObject.Enum");
    g_c_engine = find_class("/Script/Engine.GameEngine");
    report += "classes: Class " + addr(g_c_class) + ", Package " + name_of(g_c_package) + ", Function " + addr(g_c_function) + ", GameEngine " + addr(g_c_engine) + "\n";
    if (!g_c_function || !g_c_scriptstruct || !g_c_enum || !g_c_engine || name_of(g_c_package) != "Package") return false;
    // FField layout: walk AActor's own properties with each candidate (next, name) pair; readable() guards every step.
    UClass* actor = find_class("/Script/Engine.Actor");
    if (!actor) { report += "no /Script/Engine.Actor\n"; return false; }
    struct Variant { int next, name, delta; const char* what; } variants[] = {{0x20, 0x28, 0, "16-byte FFieldVariant"}, {0x18, 0x20, -8, "8-byte FFieldVariant"}};
    bool picked = false;
    for (const Variant& v : variants) {
        lay::ff_next = v.next;
        lay::ff_name = v.name;
        int hits = 0, steps = 0;
        std::string seen;
        for (FField* f = first_prop(actor); f && steps < 400 && readable(f, 0x40); f = next_prop(f), steps++) {
            std::string n = field_name(f);
            if (steps < 4) seen += n + " ";
            hits += n == "RootComponent" || n == "PrimaryActorTick";
        }
        report += std::string(v.what) + ": " + std::to_string(steps) + " fields, first: " + seen + "\n";
        if (hits == 2) {
            lay::prop_dim = 0x38 + v.delta, lay::prop_size = 0x3C + v.delta, lay::prop_flags = 0x40 + v.delta;
            lay::prop_offset = 0x4C + v.delta, lay::prop_extra = 0x78 + v.delta;
            report += std::string("using ") + v.what + "\n";
            picked = true;
            break;
        }
    }
    if (!picked) return false;
    FProperty* root = actor ? find_prop(actor, "RootComponent") : nullptr;
    FProperty* tick = actor ? find_prop(actor, "PrimaryActorTick") : nullptr;
    if (!actor || !root || !tick) { report += "Actor/RootComponent/PrimaryActorTick not found\n"; return false; }
    snprintf(line, sizeof line, "Actor size 0x%x super %s; RootComponent %s at 0x%x size %d -> %s; PrimaryActorTick %s\n", struct_size(actor),
             name_of(super_of(actor)).c_str(), field_type(root).c_str(), prop_offset(root), prop_size(root), name_of(extra<UObject>(root)).c_str(),
             type_detail(tick).c_str());
    report += line;
    if (name_of(super_of(actor)) != "Object" || field_type(root) != "ObjectProperty" || prop_size(root) != 8 || name_of(extra<UObject>(root)) != "SceneComponent")
        return false;
    // FArrayProperty::Inner: AActor::Tags is TArray<FName>.
    FProperty* tags = find_prop(actor, "Tags");
    lay::array_inner = -1;
    for (int cand : {0, 8}) {
        const FField* in = tags ? at<FField*>(tags, lay::prop_extra + cand) : nullptr;
        if (in && readable(in, 0x40) && field_type(in) == "NameProperty") { lay::array_inner = cand; break; }
    }
    snprintf(line, sizeof line, "ArrayProperty inner at +%d\n", lay::array_inner);
    report += line;
    if (lay::array_inner < 0) return false;
    // UFunction layout: K2_GetActorLocation returns a Vector (24 bytes in UE5).
    UFunction* loc = find_func(actor, "K2_GetActorLocation");
    if (!loc) { report += "no K2_GetActorLocation\n"; return false; }
    snprintf(line, sizeof line, "K2_GetActorLocation flags 0x%x parms %d retoff %d\n", at<uint32_t>(loc, off::func_flags), at<uint16_t>(loc, off::func_parmssize),
             at<uint16_t>(loc, off::func_retoffset));
    report += line;
    if (at<uint16_t>(loc, off::func_parmssize) != 24 || !(at<uint32_t>(loc, off::func_flags) & 0x400)) return false;
    // UClass::ClassDefaultObject: the slot of UObject's class that points at Default__Object.
    UObject* def = find_path("/Script/CoreUObject.Default__Object");
    for (int o = 0x80; o < 0x200 && def; o += 8)
        if (at<UObject*>(obj_class, o) == def) { g_cdo_off = o; break; }
    snprintf(line, sizeof line, "ClassDefaultObject at 0x%x\n", g_cdo_off);
    report += line;
    if (g_cdo_off < 0) return false;
    // ProcessEvent slot: Default__Object's vtable entry must be the function we expect.
    void* pe = (*reinterpret_cast<void***>(def))[off::pe_slot];
    report += "vtable[79] " + addr(pe) + " expected " + addr(g_process_event) + "\n";
    if (pe != g_process_event) return false;
    // Enum layout: ENetRole has ROLE_Authority = 3.
    UObject* role = find_path("/Script/Engine.ENetRole");
    report += "ENetRole 3 = " + (role ? enum_name(role, 3) : std::string("?")) + "\n";
    if (!role || enum_name(role, 3) != "ROLE_Authority") return false;
    g_text_lib = find_path("/Script/Engine.Default__KismetTextLibrary");
    g_f_text_to_string = g_text_lib ? find_func(class_of(g_text_lib), "Conv_TextToString") : nullptr;
    return true;
}

UClass* function_class() { return g_c_function; }
UClass* class_class() { return g_c_class; }
UClass* scriptstruct_class() { return g_c_scriptstruct; }
UClass* enum_class() { return g_c_enum; }
json enum_values(const UObject* e) {
    json out = json::object();
    auto names = at<TArray<uint8_t>>(e, off::enum_names);
    for (int i = 0; i < names.num && i < 4096; i++) out.set(name(at<FName>(names.data + 16 * i, 0)), static_cast<double>(at<int64_t>(names.data + 16 * i, 8)));
    return out;
}
}  // namespace ue
