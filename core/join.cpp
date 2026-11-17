// Join: the host hands its current campaign to the client. The host saves (SaveGameManager.SaveGame "CoopHost"),
// a worker thread serves that save folder over TCP (port = co-op port + 1); the client's worker downloads it into
// its own save folder as "CoopHost_<host id>" (never over one of the client's saves) and the game thread then loads
// it. Save folders are resolved through the exe's own SHGetKnownFolderPath import slot, so a test copy gets its
// sandbox and a player's install gets the real folder, exactly as the game itself does.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include "join.hpp"
#include "character.hpp"
#include "steamnet.hpp"
#include "ue.hpp"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void core_log(const std::string& line);  // main.cpp: a line in tow2coop.log

namespace join {
using json = coopbridge::json::Value;
json status();
namespace {
std::mutex mu;
std::string state = "idle", error, folder;  // folder: the save folder name the client will load
std::atomic<bool> busy{false}, ready_to_load{false};
uint64_t bytes = 0;
double started = 0, load_t = -1e9;  // load_t: when this core last started a LoadGame (game thread)
bool load_ok = false;               // the game accepted that LoadGame
std::atomic<bool> joined_world{false};
uintptr_t load_world = 0;           // the world it was started from

double now_s() { return GetTickCount64() / 1000.0; }
void set_state(const std::string& s, const std::string& e = "") {
    std::lock_guard<std::mutex> g(mu);
    state = s;
    if (!e.empty()) error = e;
}

// The function the exe would call (the proxy's hook in a test copy), found in the exe's import table.
using KnownFolderFn = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
KnownFolderFn exe_known_folder() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp(reinterpret_cast<char*>(base + d->Name), "SHELL32.dll") != 0 || !d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++)
            if (!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) &&
                strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, "SHGetKnownFolderPath") == 0)
                return reinterpret_cast<KnownFolderFn>(iat->u1.Function);
    }
    return nullptr;
}

std::wstring saves_dir() {
    KnownFolderFn f = exe_known_folder();
    PWSTR p = nullptr;
    if (!f || FAILED(f(FOLDERID_SavedGames, 0, nullptr, &p)) || !p) return L"";
    std::wstring d = std::wstring(p) + L"\\TheOuterWorlds2";
    CoTaskMemFree(p);
    return d;
}

std::vector<std::wstring> files_of(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool send_all(SOCKET s, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        int k = send(s, c, static_cast<int>(std::min<size_t>(n, 1 << 16)), 0);
        if (k <= 0) return false;
        c += k, n -= k;
    }
    return true;
}
bool recv_all(SOCKET s, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n) {
        int k = recv(s, c, static_cast<int>(std::min<size_t>(n, 1 << 16)), 0);
        if (k <= 0) return false;
        c += k, n -= k;
    }
    return true;
}

std::vector<char> read_file(const std::wstring& path, bool& ok) {
    std::vector<char> data;
    FILE* f = _wfopen(path.c_str(), L"rb");
    ok = f != nullptr;
    if (!f) return data;
    char buf[1 << 16];
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + k);
    fclose(f);
    return data;
}

// Steam: the save files go to the joined player on channel 1 (reliable, ordered): 'B' host id + file count, per file
// 'F' size + UTF-16 name and 'D' chunks of 64 KB, then 'E'. A full send buffer (EResult 25) is waited out.
bool steam_push(uint64_t to, const std::wstring& dir, const std::vector<std::wstring>& names, uint32_t host_id) {
    auto put = [&](const std::string& m) {
        for (int i = 0; i < 6000; i++) {  // up to 60 s of back-pressure
            int r = steamnet::send(to, m.data(), static_cast<int>(m.size()), true, 1);
            if (r == 1) return true;
            if (r != 25) { set_state("failed", "Steam send error " + std::to_string(r)); return false; }
            Sleep(10);
        }
        set_state("failed", "the Steam send buffer stayed full for 60 s");
        return false;
    };
    auto u32 = [](std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); };
    std::string b(1, 'B');
    u32(b, host_id), u32(b, static_cast<uint32_t>(names.size()));
    if (!put(b)) return false;
    for (auto& n : names) {
        bool ok;
        std::vector<char> data = read_file(dir + L"\\" + n, ok);
        if (!ok) { set_state("failed", "cannot read the host save"); return false; }
        uint64_t sz = data.size();
        std::string f(1, 'F');
        f.append(reinterpret_cast<const char*>(&sz), 8);
        u32(f, static_cast<uint32_t>(n.size() * 2));
        f.append(reinterpret_cast<const char*>(n.data()), n.size() * 2);
        if (!put(f)) return false;
        for (size_t off = 0; off < data.size(); off += 1 << 16) {
            std::string d(1, 'D');
            d.append(data.data() + off, std::min<size_t>(1 << 16, data.size() - off));
            if (!put(d)) return false;
        }
        bytes += sz;
    }
    return put(std::string(1, 'E'));
}

// Client side of the Steam push (game thread).
FILE* rx_file = nullptr;
uint64_t rx_left = 0;
std::wstring rx_dir;

void rx_fail(const std::string& why) {
    if (rx_file) fclose(rx_file), rx_file = nullptr;
    rx_left = 0;
    set_state("failed", why);
    core_log("hand-over failed: " + why);
}

void rx_begin(uint32_t host_id) {
    char name[48];
    snprintf(name, sizeof name, "CoopHost_%08X", host_id);
    if (rx_file) fclose(rx_file), rx_file = nullptr;
    rx_left = 0;
    rx_dir = saves_dir();
    if (!rx_dir.empty()) rx_dir += L"\\" + std::wstring(name, name + strlen(name));
    { std::lock_guard<std::mutex> g(mu); folder = name, error.clear(); }
    bytes = 0, started = now_s(), ready_to_load = false;
    set_state("receiving");
}

// Host worker: wait until the save folder stops changing, then serve it to one client. Framing per file:
// u32 name bytes, UTF-16 name, u64 size, data; a zero name length ends the stream.
void host_worker(std::wstring dir, int port, std::string bind_ip, std::string allow_ip, uint64_t steam_to, uint32_t host_id) {
    DWORD64 last_total = ~0ull;
    for (int stable = 0, i = 0; stable < 3 && i < 120; i++) {  // 3 quiet half-seconds, at most 60 s
        Sleep(500);
        DWORD64 total = 0;
        for (auto& f : files_of(dir)) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW((dir + L"\\" + f).c_str(), GetFileExInfoStandard, &a)) total += (DWORD64(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        }
        stable = total && total == last_total ? stable + 1 : 0;
        last_total = total;
    }
    auto names = files_of(dir);
    if (names.empty()) { set_state("failed", "the host save was not written"); busy = false; return; }
    if (steam_to) {  // pushed to the joined player's SteamID: no listening socket
        set_state("serving");
        core_log("hand-over: sending the campaign over Steam (" + std::to_string(names.size()) + " files)");
        bool ok = steam_push(steam_to, dir, names, host_id);
        if (ok) set_state("sent");
        core_log(ok ? "hand-over: sent " + std::to_string(bytes) + " bytes" : "hand-over failed: " + status().get("error").asString());
        busy = false;
        return;
    }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, bind_ip.c_str(), &a.sin_addr);
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) || listen(ls, 1)) {
        set_state("failed", "cannot listen on TCP " + std::to_string(port));
        closesocket(ls);
        busy = false;
        return;
    }
    set_state("serving");
    SOCKET c = INVALID_SOCKET;
    int refused = 0;
    for (ULONGLONG t0 = GetTickCount64(); c == INVALID_SOCKET && GetTickCount64() - t0 < 120000;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ls, &r);
        timeval tv{1, 0};
        if (select(0, &r, nullptr, nullptr, &tv) != 1) continue;
        sockaddr_in from{};
        int len = sizeof from;
        SOCKET k = accept(ls, reinterpret_cast<sockaddr*>(&from), &len);
        char ip[64] = "";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        if (k == INVALID_SOCKET) continue;
        if (allow_ip.empty() || allow_ip == ip) {
            c = k;
        } else {  // someone else reached the port: not our player
            closesocket(k);
            refused++;
        }
    }
    closesocket(ls);
    if (c == INVALID_SOCKET) {
        set_state("failed", "no client connected within 120 s" + (refused ? " (refused " + std::to_string(refused) + " other address(es))" : std::string()));
        busy = false;
        return;
    }
    bool ok = true;
    for (auto& n : names) {
        FILE* f = _wfopen((dir + L"\\" + n).c_str(), L"rb");
        if (!f) { ok = false; break; }
        std::vector<char> data;
        char buf[1 << 16];
        for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + k);
        fclose(f);
        uint32_t nl = static_cast<uint32_t>(n.size() * 2);
        uint64_t sz = data.size();
        ok = send_all(c, &nl, 4) && send_all(c, n.data(), nl) && send_all(c, &sz, 8) && send_all(c, data.data(), data.size());
        if (!ok) break;
        bytes += sz;
    }
    uint32_t end = 0;
    ok = ok && send_all(c, &end, 4);
    closesocket(c);
    set_state(ok ? "sent" : "failed", ok ? "" : "connection lost while sending");
    busy = false;
}

void client_worker(std::string host, int port, std::wstring dir) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) { set_state("failed", "bad host address"); busy = false; return; }
    bool connected = false;
    for (int i = 0; i < 60 && !connected; i++) {  // the host may still be writing its save
        connected = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        if (!connected) {
            closesocket(s);
            Sleep(1000);
            s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        }
    }
    if (!connected) { closesocket(s); set_state("failed", "cannot reach the host's save server"); busy = false; return; }
    CreateDirectoryW(dir.c_str(), nullptr);
    bool ok = true;
    for (;;) {
        uint32_t nl = 0;
        if (!recv_all(s, &nl, 4)) { ok = false; break; }
        if (!nl) break;
        if (nl > 512) { ok = false; break; }
        std::wstring name(nl / 2, L'\0');
        uint64_t sz = 0;
        if (!recv_all(s, &name[0], nl) || !recv_all(s, &sz, 8) || sz > (256ull << 20) || name.find_first_of(L"\\/:") != std::wstring::npos) { ok = false; break; }
        std::vector<char> data(sz);
        if (!recv_all(s, data.data(), sz)) { ok = false; break; }
        FILE* f = _wfopen((dir + L"\\" + name).c_str(), L"wb");
        if (!f || fwrite(data.data(), 1, sz, f) != sz) { if (f) fclose(f); ok = false; break; }
        fclose(f);
        bytes += sz;
    }
    closesocket(s);
    set_state(ok ? "received" : "failed", ok ? "" : "transfer broken");
    if (ok) ready_to_load = true;
    busy = false;
}
}  // namespace

std::wstring saves_folder() { return saves_dir(); }

std::wstring local_folder() {
    KnownFolderFn f = exe_known_folder();
    PWSTR p = nullptr;
    if (!f || FAILED(f(FOLDERID_LocalAppData, 0, nullptr, &p)) || !p) return L"";
    std::wstring d = p;
    CoTaskMemFree(p);
    return d;
}

bool joined() { return joined_world.load(); }
void set_joined(bool on) { joined_world = on; }

json host_offer(int port, const std::string& bind_ip, const std::string& allow_ip, uint64_t steam_to, uint32_t host_id) {
    if (busy.exchange(true)) throw std::runtime_error("a transfer is already running");
    std::wstring dir = saves_dir();
    if (dir.empty()) { busy = false; throw std::runtime_error("cannot resolve the save folder"); }
    std::vector<ue::UObject*> found = ue::instances(ue::find_class("/Script/Arkansas.SaveGameManager"), false, 1);
    ue::UObject* sgm = found.empty() ? nullptr : found[0];
    if (!sgm) { busy = false; throw std::runtime_error("no SaveGameManager"); }
    // The previous hand-over's files go first: if the game refuses to save now (combat, a conversation, a menu...), the
    // worker finds nothing instead of sending the old campaign (run 44-45: the client got a stale save mid-combat).
    for (auto& f : files_of(dir + L"\\CoopHost")) DeleteFileW((dir + L"\\CoopHost\\" + f).c_str());
    json a = json::object();
    a.set("Filename", "CoopHost").set("bIgnoreSuperNova", true).set("SaveGameType", "Standard");
    json r = ue::call(sgm, ue::find_func(ue::class_of(sgm), "SaveGame"), a);
    std::string saved = r.get("ReturnValue").asString();
    if (saved != "Success" && saved != "AsyncSaveTask_Begun") {  // the client asks again in a few seconds
        busy = false;
        throw std::runtime_error("the host cannot save right now (" + saved + ")");
    }
    bytes = 0, started = now_s(), error.clear();
    set_state("saving");
    std::thread(host_worker, dir + L"\\CoopHost", port, bind_ip, allow_ip, steam_to, host_id).detach();
    json out = json::object();
    out.set("save", r.get("ReturnValue")).set("port", port);
    return out;
}

json client_fetch(const std::string& host, int port, uint32_t host_id) {
    if (busy.exchange(true)) throw std::runtime_error("a transfer is already running");
    std::wstring dir = saves_dir();
    if (dir.empty()) { busy = false; throw std::runtime_error("cannot resolve the save folder"); }
    char name[48];
    snprintf(name, sizeof name, "CoopHost_%08X", host_id);
    { std::lock_guard<std::mutex> g(mu); folder = name; }
    bytes = 0, started = now_s(), error.clear(), ready_to_load = false;
    set_state("receiving");
    std::wstring wname(name, name + strlen(name));
    std::thread(client_worker, host, port, dir + L"\\" + wname).detach();
    json out = json::object();
    out.set("folder", name);
    return out;
}

void client_expect(uint32_t host_id) {
    std::string f;
    { std::lock_guard<std::mutex> g(mu); f = state == "receiving" ? folder : std::string(); }
    char name[48];
    snprintf(name, sizeof name, "CoopHost_%08X", host_id);
    if (f != name) rx_begin(host_id);  // the 'B' message may already have started this hand-over
}

void on_steam_data(const char* d, int n) {
    if (n < 1) return;
    uint32_t a = 0, b = 0;
    switch (d[0]) {
    case 'B':
        if (n != 9) return rx_fail("bad hand-over start");
        memcpy(&a, d + 1, 4);
        rx_begin(a);
        if (rx_dir.empty()) return rx_fail("cannot resolve the save folder");
        CreateDirectoryW(rx_dir.c_str(), nullptr);
        return;
    case 'F': {
        uint64_t sz = 0;
        if (rx_dir.empty() || rx_file || rx_left || n < 13) return rx_fail("bad file header");
        memcpy(&sz, d + 1, 8);
        memcpy(&b, d + 9, 4);
        if (b == 0 || b > 512 || b % 2 || n != 13 + static_cast<int>(b) || sz > (256ull << 20)) return rx_fail("bad file header");
        std::wstring name(b / 2, L'\0');
        memcpy(&name[0], d + 13, b);
        if (name.find_first_of(L"\\/:") != std::wstring::npos || name == L"." || name == L"..") return rx_fail("bad file name");
        rx_file = _wfopen((rx_dir + L"\\" + name).c_str(), L"wb");
        if (!rx_file) return rx_fail("cannot write the received save");
        rx_left = sz;
        if (!rx_left) fclose(rx_file), rx_file = nullptr;
        return;
    }
    case 'D': {
        size_t k = static_cast<size_t>(n - 1);
        if (!rx_file || k > rx_left) return rx_fail("unexpected data");
        if (fwrite(d + 1, 1, k, rx_file) != k) return rx_fail("cannot write the received save");
        rx_left -= k, bytes += k;
        if (!rx_left) fclose(rx_file), rx_file = nullptr;
        return;
    }
    case 'E':
        if (rx_dir.empty() || rx_file || rx_left) return rx_fail("transfer broken");
        set_state("received");
        ready_to_load = true;
        core_log("hand-over: received " + std::to_string(bytes) + " bytes, loading the host's campaign");
        return;
    }
}

// Game thread. The old world is torn down over several frames after LoadGame (run 50: 7 s after the call) and the
// world pointer changes only when the new one is there; a refused load keeps the old world, so no wait then.
bool loading() {
    double t = now_s() - load_t;
    if (t < 5.0) return true;
    if (t >= 60.0 || !load_ok) return false;
    std::string err;
    return reinterpret_cast<uintptr_t>(ue::resolve("world", &err)) == load_world;
}

// A joined player's game must not save: its autosave and quicksave slots ("Autosave02", ...) are the same files as in
// the player's own campaign, so a co-op autosave would overwrite them; the host keeps the campaign (test_client_saves.py).
void block_saves(bool block) {
    std::vector<ue::UObject*> found = ue::instances(ue::find_class("/Script/Arkansas.SaveGameManager"), false, 1);
    ue::UFunction* f = found.empty() ? nullptr : ue::find_func(ue::class_of(found[0]), "DisableSaves");
    if (!f) return;
    json a = json::object();
    a.set("bDisable", block);
    ue::call(found[0], f, a);
}

// Game thread: load the received save once (the front end must have run for a while, see session.py).
void frame(uint64_t frames) {
    if (frames < 3600 || !ready_to_load.load()) return;  // LoadGame in the first ~15 s after start crashed the engine
    std::string f;
    { std::lock_guard<std::mutex> g(mu); f = folder; }
    // The joined player's own character goes into the received campaign first (it may save it for a few seconds).
    if (!character::before_load(saves_dir() + L"\\" + std::wstring(f.begin(), f.end()))) { set_state("keeping your character"); return; }
    ready_to_load = false;
    std::vector<ue::UObject*> found = ue::instances(ue::find_class("/Script/Arkansas.SaveGameManager"), false, 1);
    if (found.empty()) { set_state("failed", "no SaveGameManager to load with"); return; }
    json a = json::object();
    a.set("Filename", f).set("bPreValidateMetadata", true);
    set_state("loading");
    block_saves(false);  // a joined game has saves off, which may refuse the load too; the new world blocks them again
    std::string err;
    load_world = reinterpret_cast<uintptr_t>(ue::resolve("world", &err));
    load_ok = false;
    load_t = now_s();
    json r = ue::call(found[0], ue::find_func(ue::class_of(found[0]), "LoadGame"), a);
    load_ok = r.get("ReturnValue").asString().find("Success") != std::string::npos;
    if (load_ok) joined_world = true, character::loaded();
    set_state("loaded:" + r.get("ReturnValue").asString());
}

json status() {
    std::lock_guard<std::mutex> g(mu);
    json s = json::object();
    s.set("state", state).set("error", error).set("bytes", static_cast<double>(bytes)).set("folder", folder);
    s.set("seconds", started ? now_s() - started : 0.0);
    return s;
}
}  // namespace join
