// tow2coop-setup.exe: installs, updates or removes the co-op mod in the player's own The Outer Worlds 2 (Steam).
// The mod's two DLLs are inside it (resources). Same rules as install.ps1: the game is found through Steam, nothing of
// the game is changed or overwritten (a dsound.dll of another mod stops the install), tow2coop.ini holds the settings.
// Window by default; for scripts and tests: tow2coop-setup.exe --install|--uninstall|--status [--game <folder>]
// [--udp --role host|client --peer <address> --key <key> --port <n>] [--friend <Steam name>] (the result on stdout).
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>
#include "resource.h"
#include "version.h"

namespace {
constexpr const char* kAppId = "1449110";
constexpr const char* kBuild = "24225441";  // the game build the mod was made for (Steam build id)
constexpr const wchar_t* kExe = L"TheOuterWorlds2-Win64-Shipping.exe";

bool french() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH; }
const wchar_t* tr(const wchar_t* en, const wchar_t* fr) { return french() ? fr : en; }

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool read_file(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER n;
    bool ok = GetFileSizeEx(h, &n) && n.QuadPart < (512ll << 20);
    if (ok) {
        out.resize(static_cast<size_t>(n.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

DWORD write_file(const std::wstring& p, const void* data, size_t n) {  // 0 or the Windows error
    std::wstring tmp = p + L".tow2coop-new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD put = 0;
    bool ok = WriteFile(h, data, static_cast<DWORD>(n), &put, nullptr) && put == n;
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (ok && !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) err = GetLastError(), ok = false;
    if (!ok) DeleteFileW(tmp.c_str());
    return err;
}

struct Blob {
    const char* data = nullptr;
    size_t size = 0;
};

Blob payload(int id) {
    Blob b;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    if (g) b.data = static_cast<const char*>(LockResource(g)), b.size = SizeofResource(nullptr, r);
    return b;
}

// "key" "value" pairs of a Steam .vdf/.acf file (values unescaped).
std::vector<std::string> vdf_values(const std::string& text, const std::string& key) {
    std::vector<std::string> out;
    std::string k = "\"" + key + "\"";
    for (size_t at = text.find(k); at != std::string::npos; at = text.find(k, at + 1)) {
        size_t q = text.find('"', at + k.size());
        size_t line = text.find('\n', at + k.size());
        if (q == std::string::npos || (line != std::string::npos && q > line)) continue;
        std::string v;
        for (size_t i = q + 1; i < text.size() && text[i] != '"'; i++) {
            if (text[i] == '\\' && i + 1 < text.size()) i++;
            v += text[i];
        }
        out.push_back(v);
    }
    return out;
}

struct Game {
    std::wstring root, win64;
    std::string build;  // from the Steam manifest, empty when the folder was picked by hand
    std::wstring problem;
};

std::wstring steam_path() {
    wchar_t buf[1024];
    DWORD n = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return L"";
    std::wstring s = buf;
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

Game check_folder(const std::wstring& picked) {  // the game's own folder, or its Arkansas\Binaries\Win64
    Game g;
    std::wstring dir = picked;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    const std::wstring tail = L"\\Arkansas\\Binaries\\Win64";
    if (exists(dir + L"\\" + kExe)) {
        g.win64 = dir, g.root = dir;
        if (dir.size() > tail.size() && _wcsicmp(dir.c_str() + dir.size() - tail.size(), tail.c_str()) == 0) g.root = dir.substr(0, dir.size() - tail.size());
    } else if (exists(dir + L"\\Arkansas\\Binaries\\Win64\\" + kExe)) g.root = dir, g.win64 = dir + L"\\Arkansas\\Binaries\\Win64";
    else g.problem = tr(L"The Outer Worlds 2 is not in this folder.", L"The Outer Worlds 2 n'est pas dans ce dossier.");
    return g;
}

Game find_game() {
    std::wstring steam = steam_path();
    if (steam.empty()) {
        Game g;
        g.problem = tr(L"Steam was not found: pick the game folder.", L"Steam est introuvable : choisis le dossier du jeu.");
        return g;
    }
    std::vector<std::wstring> libs = {steam};
    std::string vdf;
    if (read_file(steam + L"\\steamapps\\libraryfolders.vdf", vdf))
        for (const auto& p : vdf_values(vdf, "path")) libs.push_back(widen(p));
    for (const auto& lib : libs) {
        std::string acf;
        if (!read_file(lib + L"\\steamapps\\appmanifest_" + widen(kAppId) + L".acf", acf)) continue;
        auto dirs = vdf_values(acf, "installdir");
        auto builds = vdf_values(acf, "buildid");
        if (dirs.empty()) continue;
        Game g = check_folder(lib + L"\\steamapps\\common\\" + widen(dirs[0]));
        if (!builds.empty()) g.build = builds[0];
        return g;
    }
    Game g;
    g.problem = tr(L"The Outer Worlds 2 was not found in your Steam libraries: pick the game folder.",
                   L"The Outer Worlds 2 est introuvable dans tes bibliothèques Steam : choisis le dossier du jeu.");
    return g;
}

bool game_running(const std::wstring& win64) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, kExe) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        wchar_t path[2048];
        DWORD n = 2048;
        if (h && QueryFullProcessImageNameW(h, 0, path, &n) && _wcsnicmp(path, win64.c_str(), win64.size()) == 0) found = true;
        if (h) CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

// A file of ours: absent, this version, or an older build of the mod (it names TOW2_SANDBOX_DIR: the loader in
// UTF-16, the mod in ASCII).
bool is_ours(const std::wstring& path, const Blob& mine) {
    std::string b;
    if (!exists(path)) return true;
    if (!read_file(path, b)) return false;
    if (b.size() == mine.size && memcmp(b.data(), mine.data, mine.size) == 0) return true;
    std::string a = "TOW2_SANDBOX_DIR", w;
    for (char c : a) w += c, w += '\0';
    return b.find(a) != std::string::npos || b.find(w) != std::string::npos;
}

bool same(const std::wstring& path, const Blob& mine) {
    std::string b;
    return read_file(path, b) && b.size() == mine.size && memcmp(b.data(), mine.data, mine.size) == 0;
}

enum class State { Absent, Current, Older, Foreign };

State installed(const Game& g) {
    Blob d = payload(IDR_DSOUND), c = payload(IDR_CORE);
    std::wstring dp = g.win64 + L"\\dsound.dll", cp = g.win64 + L"\\tow2coop.dll";
    if (!is_ours(dp, d) || !is_ours(cp, c)) return State::Foreign;
    if (!exists(dp) && !exists(cp)) return State::Absent;
    return same(dp, d) && same(cp, c) ? State::Current : State::Older;
}

std::wstring status_text(const Game& g) {
    if (!g.problem.empty()) return g.problem;
    std::wstring s;
    if (!g.build.empty() && g.build != kBuild)
        s = std::wstring(tr(L"Game build ", L"Version du jeu ")) + widen(g.build) +
            tr(L": the mod was made for 24225441 and stays off until it is updated. ",
               L" : le mod a été fait pour la 24225441 et reste éteint tant qu'il n'est pas mis à jour. ");
    switch (installed(g)) {
        case State::Absent: return s + tr(L"The co-op mod is not installed.", L"Le mod co-op n'est pas installé.");
        case State::Current: return s + tr(L"The co-op mod is installed (this version).", L"Le mod co-op est installé (cette version).");
        case State::Older: return s + tr(L"An older version of the co-op mod is installed: Install updates it.",
                                         L"Une ancienne version du mod co-op est installée : Installer la met à jour.");
        default: return s + tr(L"dsound.dll in the game folder belongs to another mod: remove it first.",
                               L"Le dsound.dll du dossier du jeu appartient à un autre mod : retire-le d'abord.");
    }
}

struct Options {
    bool udp = false;
    std::string role = "auto", peer, key, friend_name;
    int port = 27960;
};

bool key_ok(const std::string& k) {
    if (k.size() < 4 || k.size() > 64) return false;
    for (char c : k)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

std::string new_key() {  // a join password for a direct-IP host (keeps strangers who reach the port out)
    const char* abc = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string k;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t x = static_cast<uint64_t>(t.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
    for (int i = 0; i < 10; i++) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        k += abc[x % 36];
    }
    return k;
}

struct Result {
    bool ok = false, denied = false;
    std::wstring text;
};

Result install(const Game& g, Options o) {
    Result r;
    if (!g.problem.empty()) { r.text = g.problem; return r; }
    if (game_running(g.win64)) { r.text = tr(L"Close the game first.", L"Ferme d'abord le jeu."); return r; }
    Blob d = payload(IDR_DSOUND), c = payload(IDR_CORE);
    if (!d.size || !c.size) { r.text = L"setup: payload missing"; return r; }
    if (!is_ours(g.win64 + L"\\dsound.dll", d) || !is_ours(g.win64 + L"\\tow2coop.dll", c)) {
        r.text = tr(L"dsound.dll in the game folder belongs to another mod: remove it first (nothing was changed).",
                    L"Le dsound.dll du dossier du jeu appartient à un autre mod : retire-le d'abord (rien n'a été changé).");
        return r;
    }
    if (o.friend_name.find_first_of("\r\n") != std::string::npos) { r.text = L"the friend name is one line"; return r; }
    std::string ini = "[coop]\r\nrole=" + o.role + "\r\ntransport=" + (o.udp ? "udp" : "steam") + "\r\n";
    if (o.udp) {
        if (o.role != "host" && o.role != "client") { r.text = tr(L"Direct IP: pick host or join.", L"IP directe : choisis héberger ou rejoindre."); return r; }
        if (o.role == "client" && (o.peer.empty() || o.key.empty())) {
            r.text = tr(L"To join by IP, enter the host's address and the key the host got.",
                        L"Pour rejoindre par IP, entre l'adresse de l'hôte et la clé qu'il a reçue.");
            return r;
        }
        if (o.key.empty()) o.key = new_key();
        if (!key_ok(o.key)) { r.text = tr(L"The key is 4-64 letters, digits, _ or -.", L"La clé fait 4 à 64 lettres, chiffres, _ ou -."); return r; }
        if (!o.peer.empty() && o.peer.find(':') == std::string::npos) o.peer += ":" + std::to_string(o.port);
        if (o.peer.find_first_of(" \r\n\"") != std::string::npos) { r.text = L"the address is one word"; return r; }
        ini += "; direct IP: both players use the same key\r\nport=" + std::to_string(o.port) + "\r\nkey=" + o.key + "\r\n";
        if (o.role == "client") ini += "peer=" + o.peer + "\r\n";
    } else if (!o.friend_name.empty()) {
        ini += "host=" + o.friend_name + "\r\n";
    }
    struct File {
        const wchar_t* name;
        const char* data;
        size_t size;
    } files[] = {{L"dsound.dll", d.data, d.size}, {L"tow2coop.dll", c.data, c.size}, {L"tow2coop.ini", ini.data(), ini.size()}};
    for (const auto& f : files) {
        DWORD err = write_file(g.win64 + L"\\" + f.name, f.data, f.size);
        if (err) {
            r.denied = err == ERROR_ACCESS_DENIED;
            r.text = std::wstring(tr(L"Could not write ", L"Impossible d'écrire ")) + f.name + L" (" + std::to_wstring(err) + L")" +
                     (r.denied ? tr(L": run the setup as administrator.", L" : lance l'installation en administrateur.") : L".");
            return r;
        }
    }
    r.ok = true;
    if (!o.udp)
        r.text = tr(L"Installed. Start the game: in the main menu, pick \"Let's play together (co-op)\" to host your game or join a "
                    L"friend's (Steam friends and public games).",
                    L"Installé. Lance le jeu : dans le menu principal, choisis « Jouons ensemble (co-op) » pour héberger ta partie ou "
                    L"rejoindre celle d'un ami (amis Steam et parties publiques).");
    else if (o.role == "host")
        r.text = std::wstring(tr(L"Installed as host (direct IP). UDP ", L"Installé comme hôte (IP directe). L'UDP ")) +
                 std::to_wstring(o.port) + tr(L" and TCP ", L" et le TCP ") + std::to_wstring(o.port + 1) +
                 tr(L" must reach this PC. Give your friend your address and this key: ", L" doivent arriver sur ce PC. Donne à ton ami ton adresse et cette clé : ") +
                 widen(o.key);
    else
        r.text = std::wstring(tr(L"Installed to join ", L"Installé pour rejoindre ")) + widen(o.peer) +
                 tr(L" (direct IP). Start the game and wait at the main menu.", L" (IP directe). Lance le jeu et attends au menu principal.");
    return r;
}

Result uninstall(const Game& g) {
    Result r;
    if (!g.problem.empty()) { r.text = g.problem; return r; }
    if (game_running(g.win64)) { r.text = tr(L"Close the game first.", L"Ferme d'abord le jeu."); return r; }
    Blob d = payload(IDR_DSOUND), c = payload(IDR_CORE);
    std::wstring kept;
    for (auto f : {std::make_pair(L"dsound.dll", d), std::make_pair(L"tow2coop.dll", c)}) {
        std::wstring p = g.win64 + L"\\" + f.first;
        if (!exists(p)) continue;
        if (!is_ours(p, f.second)) { kept += std::wstring(f.first) + tr(L" is another mod's: left in place. ", L" appartient à un autre mod : laissé. "); continue; }
        if (!DeleteFileW(p.c_str())) {
            r.denied = GetLastError() == ERROR_ACCESS_DENIED;
            r.text = std::wstring(tr(L"Could not remove ", L"Impossible de retirer ")) + f.first +
                     (r.denied ? tr(L": run the setup as administrator.", L" : lance l'installation en administrateur.") : L".");
            return r;
        }
    }
    DeleteFileW((g.win64 + L"\\tow2coop.ini").c_str());
    DeleteFileW((g.win64 + L"\\tow2coop.log").c_str());
    r.ok = true;
    r.text = kept + tr(L"The co-op mod is removed. The game runs as before.", L"Le mod co-op est retiré. Le jeu tourne comme avant.");
    return r;
}

// --- the window ---
Game game;

void set(HWND d, int id, const wchar_t* text) { SetDlgItemTextW(d, id, text); }

std::string field(HWND d, int id) {
    wchar_t buf[512];
    GetDlgItemTextW(d, id, buf, 512);
    std::wstring w = buf;
    while (!w.empty() && iswspace(w.back())) w.pop_back();
    while (!w.empty() && iswspace(w.front())) w.erase(w.begin());
    return narrow(w);
}

void refresh(HWND d) {
    set(d, IDC_GAME, game.win64.empty() ? L"" : game.root.c_str());
    set(d, IDC_STATUS, status_text(game).c_str());
    bool direct = IsDlgButtonChecked(d, IDC_DIRECT) == BST_CHECKED;
    bool join = SendDlgItemMessageW(d, IDC_ROLE, CB_GETCURSEL, 0, 0) == 1;
    EnableWindow(GetDlgItem(d, IDC_ROLE), direct);
    EnableWindow(GetDlgItem(d, IDC_PEER), direct && join);
    EnableWindow(GetDlgItem(d, IDC_PEER_LABEL), direct && join);
    EnableWindow(GetDlgItem(d, IDC_KEY), direct);
    EnableWindow(GetDlgItem(d, IDC_KEY_LABEL), direct);
    bool there = game.problem.empty();
    State st = there ? installed(game) : State::Absent;
    EnableWindow(GetDlgItem(d, IDC_INSTALL), there && st != State::Foreign);
    EnableWindow(GetDlgItem(d, IDC_UNINSTALL), there && (st == State::Current || st == State::Older));
    set(d, IDC_INSTALL, st == State::Older ? tr(L"Update", L"Mettre à jour") : tr(L"Install", L"Installer"));
}

void pick_folder(HWND d) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(tr(L"The Outer Worlds 2 folder", L"Dossier de The Outer Worlds 2"));
    if (SUCCEEDED(dlg->Show(d))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                game = check_folder(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    refresh(d);
}

// Access denied (a game folder Windows protects): the same action again, as administrator.
bool run_elevated(const std::wstring& args) {
    wchar_t self[2048];
    GetModuleFileNameW(nullptr, self, 2048);
    SHELLEXECUTEINFOW e{};
    e.cbSize = sizeof e;
    e.fMask = SEE_MASK_NOCLOSEPROCESS;
    e.lpVerb = L"runas";
    e.lpFile = self;
    e.lpParameters = args.c_str();
    e.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&e)) return false;
    WaitForSingleObject(e.hProcess, INFINITE);
    CloseHandle(e.hProcess);
    return true;
}

std::wstring quote(const std::string& s) { return L"\"" + widen(s) + L"\""; }

void act(HWND d, bool install_it) {
    Options o;
    o.udp = IsDlgButtonChecked(d, IDC_DIRECT) == BST_CHECKED;
    if (o.udp) {
        o.role = SendDlgItemMessageW(d, IDC_ROLE, CB_GETCURSEL, 0, 0) == 1 ? "client" : "host";
        o.peer = field(d, IDC_PEER), o.key = field(d, IDC_KEY);
    }
    if (!install_it && MessageBoxW(d, tr(L"Remove the co-op mod from the game folder?", L"Retirer le mod co-op du dossier du jeu ?"),
                                   tr(L"The Outer Worlds 2 co-op", L"The Outer Worlds 2 co-op"), MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    Result r = install_it ? install(game, o) : uninstall(game);
    if (r.denied) {
        std::wstring args = (install_it ? L"--install --gui" : L"--uninstall --gui") + std::wstring(L" --game ") + quote(narrow(game.root));
        if (o.udp) args += L" --udp --role " + widen(o.role) + (o.peer.empty() ? L"" : L" --peer " + quote(o.peer)) + (o.key.empty() ? L"" : L" --key " + quote(o.key));
        if (run_elevated(args)) { refresh(d); set(d, IDC_RESULT, status_text(game).c_str()); return; }
    }
    set(d, IDC_RESULT, r.text.c_str());
    refresh(d);
}

INT_PTR CALLBACK proc(HWND d, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            std::wstring title = std::wstring(L"The Outer Worlds 2 co-op ") + widen(TOW2_VERSION);
            SetWindowTextW(d, title.c_str());
            set(d, IDC_TITLE, (std::wstring(tr(L"Co-op mod for The Outer Worlds 2, version ", L"Mod co-op pour The Outer Worlds 2, version ")) +
                               widen(TOW2_VERSION)).c_str());
            set(d, IDC_GAME_LABEL, tr(L"Game folder", L"Dossier du jeu"));
            set(d, IDC_BROWSE, tr(L"Change...", L"Changer..."));
            set(d, IDC_GROUP, tr(L"How you play together", L"Comment vous jouez ensemble"));
            set(d, IDC_STEAM, tr(L"Over Steam: host or join from the game's main menu (recommended)",
                                 L"Par Steam : héberger ou rejoindre depuis le menu principal du jeu (conseillé)"));
            set(d, IDC_DIRECT, tr(L"Direct IP (advanced: the host opens UDP 27960 and TCP 27961)",
                                  L"IP directe (avancé : l'hôte ouvre l'UDP 27960 et le TCP 27961)"));
            set(d, IDC_PEER_LABEL, tr(L"Address", L"Adresse"));
            set(d, IDC_KEY_LABEL, tr(L"Key", L"Clé"));
            set(d, IDC_UNINSTALL, tr(L"Uninstall", L"Désinstaller"));
            set(d, IDCANCEL, tr(L"Close", L"Fermer"));
            SendDlgItemMessageW(d, IDC_ROLE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(L"Host", L"Héberger")));
            SendDlgItemMessageW(d, IDC_ROLE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(L"Join", L"Rejoindre")));
            SendDlgItemMessageW(d, IDC_ROLE, CB_SETCURSEL, 0, 0);
            SendDlgItemMessageW(d, IDC_KEY, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(L"made for you", L"créée pour toi")));
            CheckRadioButton(d, IDC_STEAM, IDC_DIRECT, IDC_STEAM);
            refresh(d);
            set(d, IDC_RESULT, tr(L"Each player installs the mod in their own copy of the game. Nothing of the game is changed; "
                                  L"Uninstall removes every file the mod added.",
                                  L"Chaque joueur installe le mod dans sa propre copie du jeu. Rien du jeu n'est modifié ; "
                                  L"Désinstaller retire tous les fichiers ajoutés par le mod."));
            SetFocus(GetDlgItem(d, IDC_INSTALL));
            return FALSE;  // the focus set above
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_BROWSE: pick_folder(d); return TRUE;
                case IDC_STEAM:
                case IDC_DIRECT: refresh(d); return TRUE;
                case IDC_ROLE: if (HIWORD(wp) == CBN_SELCHANGE) refresh(d); return TRUE;
                case IDC_INSTALL: act(d, true); return TRUE;
                case IDC_UNINSTALL: act(d, false); return TRUE;
                case IDCANCEL: EndDialog(d, 0); return TRUE;
            }
            break;
    }
    return FALSE;
}

// --- command line (scripts, tests, and the elevated retry) ---
void say(const std::wstring& text) {
    std::string s = narrow(text) + "\r\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD put = 0;
    if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, s.data(), static_cast<DWORD>(s.size()), &put, nullptr);
}

int command_line(int argc, wchar_t** argv) {
    std::wstring action;
    std::wstring folder;
    bool gui = false;
    Options o;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? narrow(argv[++i]) : std::string(); };
        if (a == L"--install" || a == L"--uninstall" || a == L"--status") action = a;
        else if (a == L"--game" && i + 1 < argc) folder = argv[++i];
        else if (a == L"--udp") o.udp = true;
        else if (a == L"--role") o.role = next();
        else if (a == L"--peer") o.peer = next();
        else if (a == L"--key") o.key = next();
        else if (a == L"--port") o.port = atoi(next().c_str());
        else if (a == L"--friend") o.friend_name = next();
        else if (a == L"--gui") gui = true;
        else { say(L"unknown argument: " + a); return 64; }
    }
    if (o.udp && o.role == "auto") { say(L"direct IP: --role host or --role client"); return 64; }
    if (o.port <= 0 || o.port > 65534) { say(L"bad --port"); return 64; }
    game = folder.empty() ? find_game() : check_folder(folder);
    Result r;
    if (action == L"--status") r.ok = game.problem.empty(), r.text = (game.win64.empty() ? L"" : L"game: " + game.root + L"\r\n") + status_text(game);
    else r = action == L"--install" ? install(game, o) : uninstall(game);
    if (gui) MessageBoxW(nullptr, r.text.c_str(), L"The Outer Worlds 2 co-op", MB_OK | (r.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    else say(r.text);
    return r.ok ? 0 : r.denied ? 5 : 1;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) return command_line(argc, argv);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    game = find_game();
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, proc, 0);
    CoUninitialize();
    return 0;
}
