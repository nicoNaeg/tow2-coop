// The joined player's own character: kept as Player.dat, spliced into every campaign the host hands over.
#include "character.hpp"
#include <windows.h>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "join.hpp"
#include "savefile.hpp"
#include "ue.hpp"

void core_log(const std::string& line);  // main.cpp: a line in tow2coop.log

namespace character {
namespace {
using json = ue::json;
std::mutex mu;  // status() runs on the bridge thread
std::string key = "direct", source = "none", note;
bool saving = false, strip_pending = false, protect = false;  // protect: the world holds the host's copy, never store it
double save_started = 0, last_check = 0, next_periodic = 0, last_saved = -1;
long long last_size = -1;
constexpr double kEvery = 180.0;  // a private save every 3 minutes keeps a crash from losing much

double now_s() { return GetTickCount64() / 1000.0; }

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::wstring store_path() {
    std::wstring dir = join::local_folder();
    if (dir.empty()) return L"";
    dir += L"\\tow2coop";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\characters";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + widen(key) + L".player";
}

std::wstring self_dir() { return join::saves_folder() + L"\\CoopSelf"; }

ue::UObject* save_manager() {
    std::vector<ue::UObject*> found = ue::instances(ue::find_class("/Script/Arkansas.SaveGameManager"), false, 1);
    return found.empty() ? nullptr : found[0];
}

ue::UObject* player_pawn() {
    std::string err;
    ue::UObject* p = ue::resolve("pawn", &err);
    return p && ue::class_name(p) == "IndianaPlayerCharacter_BP_C" ? p : nullptr;
}

long long file_size(const std::wstring& path, FILETIME* written = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return -1;
    if (written) *written = a.ftLastWriteTime;
    return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

// A private save of this world ("CoopSelf"): only its Player.dat is used. Saves stay off for the player in a joined
// game (their own Autosave/Quicksave slots are their own campaign's): lifted for this one call only.
bool start_save() {
    if (saving) return true;
    ue::UObject* sgm = save_manager();
    if (!sgm || !player_pawn()) return false;
    ue::UFunction* dis = ue::find_func(ue::class_of(sgm), "DisableSaves");
    json on = json::object(), off = json::object(), a = json::object();
    on.set("bDisable", true), off.set("bDisable", false);
    a.set("Filename", "CoopSelf").set("bIgnoreSuperNova", true).set("SaveGameType", "Standard");
    if (dis) ue::call(sgm, dis, off);
    std::string r;
    try { r = ue::call(sgm, ue::find_func(ue::class_of(sgm), "SaveGame"), a).get("ReturnValue").asString(); } catch (const std::exception& e) { r = e.what(); }
    if (dis && join::joined()) ue::call(sgm, dis, on);
    if (r != "Success" && r != "AsyncSaveTask_Begun") {
        std::lock_guard<std::mutex> g(mu);
        note = "the game refused a save now (" + r + "), the last kept character is used";
        return false;
    }
    saving = true, save_started = now_s(), last_size = -1, last_check = 0;
    return true;
}

// The private save is done when its SaveGame.dat is newer than the request and stopped growing; then its Player.dat
// becomes the kept character.
void poll_save() {
    if (!saving) return;
    double t = now_s();
    if (t - last_check < 0.5) return;
    last_check = t;
    FILETIME w;
    long long n = file_size(self_dir() + L"\\SaveGame.dat", &w);
    ULARGE_INTEGER u;
    u.LowPart = w.dwLowDateTime, u.HighPart = w.dwHighDateTime;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER nw;
    nw.LowPart = now_ft.dwLowDateTime, nw.HighPart = now_ft.dwHighDateTime;
    bool fresh = n > 0 && (nw.QuadPart - u.QuadPart) / 1e7 < t - save_started + 1.0;
    if (fresh && n == last_size) {
        saving = false;
        std::vector<uint8_t> p;
        std::string err;
        if (protect) { std::lock_guard<std::mutex> g(mu); note = "kept character not replaced (this world holds the host's copy)"; return; }
        if (savefile::read_player(self_dir(), p, err) && savefile::write_file(store_path(), p)) {
            std::lock_guard<std::mutex> g(mu);
            last_saved = t;
            source = "kept";
        } else {
            std::lock_guard<std::mutex> g(mu);
            note = "could not keep the character: " + err;
        }
        return;
    }
    last_size = n;
    if (t - save_started > 20.0) {
        saving = false;
        std::lock_guard<std::mutex> g(mu);
        note = "the private save did not finish in 20 s";
    }
}

// The player's own newest save (not a co-op one): their own character for a first join.
std::wstring newest_own_save() {
    std::wstring root = join::saves_folder(), best;
    FILETIME best_t{};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    do {
        std::wstring n = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == L"." || n == L".." || n.rfind(L"CoopHost", 0) == 0 || n.rfind(L"CoopSelf", 0) == 0)
            continue;
        FILETIME t;
        if (file_size(root + L"\\" + n + L"\\SaveGame.dat", &t) > 0 && CompareFileTime(&t, &best_t) > 0) best_t = t, best = n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? L"" : root + L"\\" + best;
}

bool splice(const std::wstring& dir) {
    std::vector<uint8_t> mine;
    std::string err, from;
    bool first = false;
    if (savefile::read_file(store_path(), mine) && !mine.empty()) {
        from = "kept";
    } else {
        std::wstring own = newest_own_save();
        if (own.empty() || !savefile::read_player(own, mine, err)) {
            std::lock_guard<std::mutex> g(mu);
            source = "a copy of the host's character, emptied (no save of your own found)";
            strip_pending = true, protect = false;
            core_log("character: no character of yours yet and no save of your own: you join as an emptied copy of the host's");
            return true;
        }
        from = "your own newest save";
        first = true;
    }
    if (!savefile::splice_player(dir, mine, err)) {
        std::lock_guard<std::mutex> g(mu);
        source = "the host's copy (splice failed: " + err + ")";
        protect = true;  // never store the host's copy over the kept character
        core_log("character: could not put your character into the campaign (" + err + "); this session plays the host's copy, your kept character is untouched");
        return true;
    }
    std::lock_guard<std::mutex> g(mu);
    source = from, protect = false;
    strip_pending = first;  // first join: the player's own character, without its items ("join with nothing")
    core_log("character: " + from + " goes into the host's campaign" + (first ? " (its inventory is emptied once loaded)" : ""));
    return true;
}

void strip() {
    ue::UObject* pawn = player_pawn();
    if (!pawn) return;
    std::string err;
    ue::UObject* inv = ue::resolve(ue::addr(pawn) + ".InventoryComponent", &err);
    ue::UFunction* rm = inv ? ue::find_func(ue::class_of(inv), "RemoveItemFromActor") : nullptr;
    if (!rm) return;
    json items = ue::eval_path(ue::addr(pawn) + ".InventoryComponent.Items");
    int removed = 0, kept = 0;
    for (const json& st : items.items()) {
        std::string ref = st.get("Item").asString();
        int count = static_cast<int>(st.get("Count").asInt(0));
        ue::UObject* item = ref.rfind("0x", 0) == 0 ? reinterpret_cast<ue::UObject*>(static_cast<uintptr_t>(strtoull(ref.c_str(), nullptr, 16))) : nullptr;
        if (!item || !ue::alive(item) || count <= 0) continue;
        json a = json::object();
        a.set("Actor", ue::addr(pawn)).set("ItemType", ue::addr(ue::class_of(item))).set("Count", count);
        try { (ue::call(inv, rm, a).get("ReturnValue").asBool(false) ? removed : kept)++; } catch (const std::exception&) { kept++; }
    }
    core_log("character: joined with nothing (" + std::to_string(removed) + " item stacks removed" + (kept ? ", " + std::to_string(kept) + " kept by the game" : "") + ")");
}
}  // namespace

void set_host(const std::string& k) {
    std::lock_guard<std::mutex> g(mu);
    key = k;
}

bool before_load(const std::wstring& dir) {
    // Leaving a joined world: keep the character first. The game may refuse a save for a moment (run 62: "Restricted"
    // right after another save): try again for up to 6 s, then the last kept character goes in.
    static double first_try = -1;
    if (!saving && player_pawn() && join::joined() && !protect) {
        double t = now_s();
        if (first_try < 0) first_try = t;
        if (!start_save() && t - first_try < 6.0) return false;
    }
    if (saving) {
        poll_save();
        if (saving) return false;
    }
    first_try = -1;
    return splice(dir);
}

void loaded() { next_periodic = now_s() + kEvery; }

void frame(double now, bool joined_gameplay) {
    poll_save();
    if (!joined_gameplay) return;
    if (strip_pending) {
        strip_pending = false;
        strip();
        next_periodic = now + 5.0;  // keep the emptied character soon
        return;
    }
    if (!saving && !protect && now >= next_periodic) {
        next_periodic = now + kEvery;
        start_save();
    }
}

void save_now() { start_save(); }

coopbridge::json::Value status() {
    std::lock_guard<std::mutex> g(mu);
    json s = json::object();
    s.set("host", key).set("source", source).set("saving", saving).set("stripPending", strip_pending).set("protect", protect);
    s.set("lastSaved", last_saved < 0 ? -1.0 : now_s() - last_saved).set("note", note);
    return s;
}
}  // namespace character
