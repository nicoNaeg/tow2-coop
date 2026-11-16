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
#include "ue.hpp"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace join {
using json = coopbridge::json::Value;
namespace {
std::mutex mu;
std::string state = "idle", error, folder;  // folder: the save folder name the client will load
std::atomic<bool> busy{false}, ready_to_load{false};
uint64_t bytes = 0;
double started = 0;

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

// Host worker: wait until the save folder stops changing, then serve it to one client. Framing per file:
// u32 name bytes, UTF-16 name, u64 size, data; a zero name length ends the stream.
void host_worker(std::wstring dir, int port, std::string bind_ip, std::string allow_ip) {
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

json host_offer(int port, const std::string& bind_ip, const std::string& allow_ip) {
    if (busy.exchange(true)) throw std::runtime_error("a transfer is already running");
    std::wstring dir = saves_dir();
    if (dir.empty()) { busy = false; throw std::runtime_error("cannot resolve the save folder"); }
    std::vector<ue::UObject*> found = ue::instances(ue::find_class("/Script/Arkansas.SaveGameManager"), false, 1);
    ue::UObject* sgm = found.empty() ? nullptr : found[0];
    if (!sgm) { busy = false; throw std::runtime_error("no SaveGameManager"); }
    json a = json::object();
    a.set("Filename", "CoopHost").set("bIgnoreSuperNova", true).set("SaveGameType", "Standard");
    json r = ue::call(sgm, ue::find_func(ue::class_of(sgm), "SaveGame"), a);
    bytes = 0, started = now_s(), error.clear();
    set_state("saving");
    std::thread(host_worker, dir + L"\\CoopHost", port, bind_ip, allow_ip).detach();
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

// Game thread: load the received save once (the front end must have run for a while, see session.py).
void frame(uint64_t frames) {
    if (frames < 3600 || !ready_to_load.load()) return;  // LoadGame in the first ~15 s after start crashed the engine
    ready_to_load = false;
    std::string f;
    { std::lock_guard<std::mutex> g(mu); f = folder; }
    std::vector<ue::UObject*> found = ue::instances(ue::find_class("/Script/Arkansas.SaveGameManager"), false, 1);
    if (found.empty()) { set_state("failed", "no SaveGameManager to load with"); return; }
    json a = json::object();
    a.set("Filename", f).set("bPreValidateMetadata", true);
    set_state("loading");
    json r = ue::call(found[0], ue::find_func(ue::class_of(found[0]), "LoadGame"), a);
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
