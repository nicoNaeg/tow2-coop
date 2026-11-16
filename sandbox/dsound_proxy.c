// dsound.dll proxy for The Outer Worlds 2 test copies.
//
// With TOW2_SANDBOX_DIR set (an absolute folder), the game exe's calls to SHGetKnownFolderPath and
// SHGetFolderPathW for the user folders (Saved Games, LocalAppData, Documents, ...) return folders under
// that directory, so a test copy never reads or writes the owner's saves, settings or logs. If the
// redirect cannot be installed the process is terminated before the game runs (fail closed).
// It also answers the Steam stat, achievement and Steam Cloud writes of the copy locally (steam guard below).
// Without the variable it only forwards DirectSound to the system DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define EXIT_NO_KNOWNFOLDER_SLOT 0x5AFE0001u
#define EXIT_NO_FOLDERPATH_SLOT 0x5AFE0002u
#define EXIT_BAD_SANDBOX_DIR 0x5AFE0003u

// ---------------------------------------------------------------- DirectSound forwarding
static HMODULE real_dsound(void) {
    static HMODULE cached;
    HMODULE m = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
    if (!m) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16)
            return NULL;
        wcscat(path, L"\\dsound.dll");
        m = LoadLibraryW(path);  // a second load in a race only bumps the ref count
        __atomic_store_n(&cached, m, __ATOMIC_RELEASE);
    }
    return m;
}

static FARPROC real_proc(const char *name) {
    HMODULE m = real_dsound();
    return m ? GetProcAddress(m, name) : NULL;
}

#define DSERR_NODRIVER_ ((HRESULT)0x88780078L)
typedef HRESULT(WINAPI *fn0)(void);
typedef HRESULT(WINAPI *fn2)(void *, void *);
typedef HRESULT(WINAPI *fn3)(void *, void *, void *);
typedef HRESULT(WINAPI *fn10)(void *, void *, void *, void *, void *, void *, void *, void *, void *, void *);

#define FWD2(name) \
    HRESULT WINAPI name(void *a, void *b) { fn2 f = (fn2)real_proc(#name); return f ? f(a, b) : DSERR_NODRIVER_; }
#define FWD3(name) \
    HRESULT WINAPI name(void *a, void *b, void *c) { fn3 f = (fn3)real_proc(#name); return f ? f(a, b, c) : DSERR_NODRIVER_; }

FWD3(DirectSoundCreate)
FWD2(DirectSoundEnumerateA)
FWD2(DirectSoundEnumerateW)
FWD3(DirectSoundCaptureCreate)
FWD2(DirectSoundCaptureEnumerateA)
FWD2(DirectSoundCaptureEnumerateW)
FWD2(GetDeviceID)
FWD3(DirectSoundCreate8)
FWD3(DirectSoundCaptureCreate8)

HRESULT WINAPI DirectSoundFullDuplexCreate(void *a, void *b, void *c, void *d, void *e, void *f_, void *g, void *h, void *i, void *j) {
    fn10 f = (fn10)real_proc("DirectSoundFullDuplexCreate");
    return f ? f(a, b, c, d, e, f_, g, h, i, j) : DSERR_NODRIVER_;
}

HRESULT WINAPI DllCanUnloadNow(void) {
    fn0 f = (fn0)real_proc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *out) {
    fn3 f = (fn3)real_proc("DllGetClassObject");
    return f ? f((void *)clsid, (void *)iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

// ---------------------------------------------------------------- sandbox
static wchar_t g_root[MAX_PATH];  // absolute, no trailing backslash; empty = pass-through
static CRITICAL_SECTION g_log_lock;
static DWORD g_start_ms;
static HRESULT(WINAPI *real_SHGetKnownFolderPath)(const GUID *, DWORD, HANDLE, PWSTR *);
static HRESULT(WINAPI *real_SHGetFolderPathW)(HWND, int, HANDLE, DWORD, LPWSTR);
static void *g_slot_known, *g_slot_folder;  // patched IAT slots, for the status file

static const struct {
    GUID id;
    const wchar_t *name;
} k_known[] = {
    {{0x4C5C32FF, 0xBB9D, 0x43B0, {0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4}}, L"SavedGames"},
    {{0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}}, L"LocalAppData"},
    {{0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}}, L"RoamingAppData"},
    {{0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}}, L"Documents"},
    {{0xA520A1A4, 0x1780, 0x4FF6, {0xBD, 0x18, 0x16, 0x73, 0x43, 0xC5, 0xAF, 0x16}}, L"LocalAppDataLow"},
};

static const wchar_t *known_name(const GUID *id) {
    for (size_t i = 0; i < sizeof k_known / sizeof k_known[0]; i++)
        if (IsEqualGUID(id, &k_known[i].id))
            return k_known[i].name;
    return NULL;
}

static const wchar_t *csidl_name(int csidl) {
    switch (csidl & 0xFF) {  // the high byte holds CSIDL_FLAG_* bits
    case 0x05: return L"Documents";       // CSIDL_PERSONAL
    case 0x1A: return L"RoamingAppData";  // CSIDL_APPDATA
    case 0x1C: return L"LocalAppData";    // CSIDL_LOCAL_APPDATA
    default: return NULL;
    }
}

// <root>\<name>, created if missing. FALSE when it does not fit or cannot be created.
static BOOL sandbox_path(const wchar_t *name, wchar_t *out, size_t cap) {
    int n = _snwprintf(out, cap, L"%ls\\%ls", g_root, name);
    if (n < 0 || (size_t)n >= cap)
        return FALSE;
    CreateDirectoryW(g_root, NULL);
    return CreateDirectoryW(out, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void log_line(const wchar_t *fmt, ...) {
    wchar_t w[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(w, 1023, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 1023;
    w[n] = 0;
    char u[3072];
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u - 2, NULL, NULL);
    if (len <= 0)
        return;
    u[len - 1] = '\n';
    wchar_t path[MAX_PATH];
    if (_snwprintf(path, MAX_PATH, L"%ls\\sandbox.log", g_root) >= MAX_PATH)
        return;
    EnterCriticalSection(&g_log_lock);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        DWORD wrote;
        if (GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20))  // shortcut: stops at 1 MB, enough for start-up tracing
            WriteFile(h, u, (DWORD)len, &wrote, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_lock);
}

static void describe_caller(void *ret, wchar_t *out, size_t cap) {
    HMODULE m = NULL;
    wchar_t mod[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m) && m)
        GetModuleFileNameW(m, mod, MAX_PATH);
    const wchar_t *base = wcsrchr(mod, L'\\');
    _snwprintf(out, cap, L"%ls+0x%llx", base ? base + 1 : mod, (unsigned long long)((char *)ret - (char *)m));
    out[cap - 1] = 0;
}

static HRESULT WINAPI hk_SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, PWSTR *out) {
    wchar_t caller[MAX_PATH + 32];
    describe_caller(__builtin_return_address(0), caller, sizeof caller / sizeof caller[0]);
    const wchar_t *name = id ? known_name(id) : NULL;
    if (!name || !out) {
        HRESULT hr = real_SHGetKnownFolderPath(id, flags, token, out);
        wchar_t g[64] = L"null";
        if (id)
            StringFromGUID2(id, g, 64);
        log_line(L"%lu SHGetKnownFolderPath(%ls, 0x%lx) pass -> 0x%08lx %ls  caller=%ls", GetTickCount() - g_start_ms, g, flags,
                 (unsigned long)hr, SUCCEEDED(hr) && out && *out ? *out : L"", caller);
        return hr;
    }
    wchar_t path[MAX_PATH];
    if (!sandbox_path(name, path, MAX_PATH)) {
        log_line(L"%lu SHGetKnownFolderPath(%ls) REFUSED: cannot create sandbox folder  caller=%ls", GetTickCount() - g_start_ms, name, caller);
        *out = NULL;
        return E_FAIL;  // fail closed: never fall back to the real folder
    }
    size_t bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    PWSTR p = (PWSTR)CoTaskMemAlloc(bytes);  // the caller frees it with CoTaskMemFree
    if (!p) {
        *out = NULL;
        return E_OUTOFMEMORY;
    }
    memcpy(p, path, bytes);
    *out = p;
    log_line(L"%lu SHGetKnownFolderPath(%ls, 0x%lx) -> %ls  caller=%ls", GetTickCount() - g_start_ms, name, flags, path, caller);
    return S_OK;
}

static HRESULT WINAPI hk_SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR out) {
    wchar_t caller[MAX_PATH + 32];
    describe_caller(__builtin_return_address(0), caller, sizeof caller / sizeof caller[0]);
    const wchar_t *name = csidl_name(csidl);
    if (!name || !out) {
        HRESULT hr = real_SHGetFolderPathW(hwnd, csidl, token, flags, out);
        log_line(L"%lu SHGetFolderPathW(csidl 0x%x) pass -> 0x%08lx %ls  caller=%ls", GetTickCount() - g_start_ms, csidl, (unsigned long)hr,
                 SUCCEEDED(hr) && out ? out : L"", caller);
        return hr;
    }
    wchar_t path[MAX_PATH];
    if (!sandbox_path(name, path, MAX_PATH)) {
        log_line(L"%lu SHGetFolderPathW(%ls) REFUSED: cannot create sandbox folder  caller=%ls", GetTickCount() - g_start_ms, name, caller);
        return E_FAIL;
    }
    wcscpy(out, path);  // fits: sandbox_path checked < MAX_PATH, the caller's buffer is MAX_PATH by contract
    log_line(L"%lu SHGetFolderPathW(%ls) -> %ls  caller=%ls", GetTickCount() - g_start_ms, name, path, caller);
    return S_OK;
}

// Named mutexes of the exe get a per-sandbox suffix, so two test copies on one PC do not see each other's
// single-instance lock ("There is already an instance of the game running."). Unnamed mutexes are untouched.
static wchar_t g_tag[16];  // "-sbx" + 8 hex digits of a hash of the sandbox path
static HANDLE(WINAPI *real_CreateMutexW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
static HANDLE(WINAPI *real_CreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static void *g_slot_mutexw, *g_slot_mutexa;

static HANDLE WINAPI hk_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR name) {
    wchar_t buf[MAX_PATH];
    if (!name || _snwprintf(buf, MAX_PATH, L"%ls%ls", name, g_tag) < 0 || wcslen(name) + wcslen(g_tag) >= MAX_PATH)
        return real_CreateMutexW(sa, own, name);
    log_line(L"%lu CreateMutexW(%ls) -> %ls", GetTickCount() - g_start_ms, name, buf);
    return real_CreateMutexW(sa, own, buf);
}

static HANDLE WINAPI hk_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) {
    char buf[MAX_PATH];
    if (!name || strlen(name) + 13 >= MAX_PATH || _snprintf(buf, MAX_PATH, "%s%ls", name, g_tag) < 0)
        return real_CreateMutexA(sa, own, name);
    log_line(L"%lu CreateMutexA(%hs) -> %hs", GetTickCount() - g_start_ms, name, buf);
    return real_CreateMutexA(sa, own, buf);
}

// Point every import slot of dll!func in module mod at hook. Returns the number of slots patched.
static int patch_import(HMODULE mod, const char *dll, const char *func, void *hook, void **orig, void **slot_out) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk)
            continue;  // names come from the lookup table; the address table already holds resolved pointers
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) != 0 || (void *)iat->u1.Function == hook)
                continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old))
                return -1;
            *orig = (void *)iat->u1.Function;
            iat->u1.Function = (ULONGLONG)hook;
            VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), old, &old);
            *slot_out = &iat->u1.Function;
            patched++;
        }
    }
    return patched;
}

// ---------------------------------------------------------------- Steam guard (test copies only)
// A test copy runs under the owner's Steam account with the real app id, so its stat and achievement writes
// (ISteamUserStats) and its Steam Cloud writes (ISteamRemoteStorage) would reach that account. With the sandbox on,
// those vtable slots are answered here instead; cloud reads are logged and passed on. The slot of each method is read
// from steam_api64.dll's own flat wrapper (mov rax,[rcx]; jmp [rax+disp]); if anything does not match, the process
// is stopped (fail closed) instead of running unguarded.
#define EXIT_STEAM_GUARD 0x5AFE0010u
typedef unsigned long long u64;
static volatile LONG g_guard_logged;

static void guard_log(const char *verb, const char *what, const char *name) {
    if (InterlockedIncrement(&g_guard_logged) <= 500)  // shortcut: first 500 calls only, enough to see what the game does
        log_line(L"%lu steam guard: %hs %hs(%hs)", GetTickCount() - g_start_ms, verb, what, name ? name : "");
}
static _Bool us_set_stat_i(void *s, const char *n, int v) { (void)s, (void)v; guard_log("blocked", "SetStatInt32", n); return 1; }
static _Bool us_set_stat_f(void *s, const char *n, float v) { (void)s, (void)v; guard_log("blocked", "SetStatFloat", n); return 1; }
static _Bool us_avg(void *s, const char *n, float a, double b) { (void)s, (void)a, (void)b; guard_log("blocked", "UpdateAvgRateStat", n); return 1; }
static _Bool us_set_ach(void *s, const char *n) { (void)s; guard_log("blocked", "SetAchievement", n); return 1; }
static _Bool us_clear_ach(void *s, const char *n) { (void)s; guard_log("blocked", "ClearAchievement", n); return 1; }
static _Bool us_store(void *s) { (void)s; guard_log("blocked", "StoreStats", NULL); return 1; }
static _Bool us_progress(void *s, const char *n, unsigned c, unsigned m) { (void)s, (void)c, (void)m; guard_log("blocked", "IndicateAchievementProgress", n); return 1; }
static _Bool us_reset(void *s, _Bool a) { (void)s, (void)a; guard_log("blocked", "ResetAllStats", NULL); return 1; }
static _Bool rs_write(void *s, const char *n, const void *d, int l) { (void)s, (void)d, (void)l; guard_log("blocked", "FileWrite", n); return 0; }
static u64 rs_write_async(void *s, const char *n, const void *d, unsigned l) { (void)s, (void)d, (void)l; guard_log("blocked", "FileWriteAsync", n); return 0; }
static _Bool rs_forget(void *s, const char *n) { (void)s; guard_log("blocked", "FileForget", n); return 0; }
static _Bool rs_delete(void *s, const char *n) { (void)s; guard_log("blocked", "FileDelete", n); return 0; }
static u64 rs_share(void *s, const char *n) { (void)s; guard_log("blocked", "FileShare", n); return 0; }
static _Bool rs_set_sync(void *s, const char *n, int p) { (void)s, (void)p; guard_log("blocked", "SetSyncPlatforms", n); return 0; }
static u64 rs_stream_open(void *s, const char *n) { (void)s; guard_log("blocked", "FileWriteStreamOpen", n); return ~0ull; }
static _Bool rs_stream_chunk(void *s, u64 h, const void *d, int l) { (void)s, (void)h, (void)d, (void)l; guard_log("blocked", "FileWriteStreamWriteChunk", NULL); return 0; }
static _Bool rs_stream_close(void *s, u64 h) { (void)s, (void)h; guard_log("blocked", "FileWriteStreamClose", NULL); return 0; }
static _Bool rs_stream_cancel(void *s, u64 h) { (void)s, (void)h; guard_log("blocked", "FileWriteStreamCancel", NULL); return 0; }
static void rs_set_cloud(void *s, _Bool on) { (void)s, (void)on; guard_log("blocked", "SetCloudEnabledForApp", NULL); }
static _Bool rs_begin_batch(void *s) { (void)s; guard_log("blocked", "BeginFileWriteBatch", NULL); return 0; }
static _Bool rs_end_batch(void *s) { (void)s; guard_log("blocked", "EndFileWriteBatch", NULL); return 0; }
static int (*o_rs_read)(void *, const char *, void *, int);
static u64 (*o_rs_read_async)(void *, const char *, unsigned, unsigned);
static int (*o_rs_count)(void *);
static int rs_read(void *s, const char *n, void *d, int l) { guard_log("seen", "FileRead", n); return o_rs_read(s, n, d, l); }
static u64 rs_read_async(void *s, const char *n, unsigned o, unsigned l) { guard_log("seen", "FileReadAsync", n); return o_rs_read_async(s, n, o, l); }
static int rs_count(void *s) { guard_log("seen", "GetFileCount", NULL); return o_rs_count(s); }

static const struct {
    const char *flat;  // steam_api64.dll export whose jump gives the slot
    void *stub;
    void **orig;       // non-NULL: the stub calls the original
    int iface;         // 0 user stats v012, 1 remote storage v016
} k_guard[] = {
    {"SteamAPI_ISteamUserStats_SetStatInt32", (void *)us_set_stat_i, NULL, 0},
    {"SteamAPI_ISteamUserStats_SetStatFloat", (void *)us_set_stat_f, NULL, 0},
    {"SteamAPI_ISteamUserStats_UpdateAvgRateStat", (void *)us_avg, NULL, 0},
    {"SteamAPI_ISteamUserStats_SetAchievement", (void *)us_set_ach, NULL, 0},
    {"SteamAPI_ISteamUserStats_ClearAchievement", (void *)us_clear_ach, NULL, 0},
    {"SteamAPI_ISteamUserStats_StoreStats", (void *)us_store, NULL, 0},
    {"SteamAPI_ISteamUserStats_IndicateAchievementProgress", (void *)us_progress, NULL, 0},
    {"SteamAPI_ISteamUserStats_ResetAllStats", (void *)us_reset, NULL, 0},
    {"SteamAPI_ISteamRemoteStorage_FileWrite", (void *)rs_write, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileWriteAsync", (void *)rs_write_async, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileForget", (void *)rs_forget, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileDelete", (void *)rs_delete, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileShare", (void *)rs_share, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_SetSyncPlatforms", (void *)rs_set_sync, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen", (void *)rs_stream_open, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk", (void *)rs_stream_chunk, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamClose", (void *)rs_stream_close, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel", (void *)rs_stream_cancel, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp", (void *)rs_set_cloud, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch", (void *)rs_begin_batch, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_EndFileWriteBatch", (void *)rs_end_batch, NULL, 1},
    {"SteamAPI_ISteamRemoteStorage_FileRead", (void *)rs_read, (void **)&o_rs_read, 1},
    {"SteamAPI_ISteamRemoteStorage_FileReadAsync", (void *)rs_read_async, (void **)&o_rs_read_async, 1},
    {"SteamAPI_ISteamRemoteStorage_GetFileCount", (void *)rs_count, (void **)&o_rs_count, 1},
};
#define GUARD_N (int)(sizeof k_guard / sizeof k_guard[0])

// Slot used by a flat wrapper: 48 8B 01 [48] FF 20 | FF 60 d8 | FF A0 d32. -1 for anything else.
static int flat_slot(FARPROC f) {
    const BYTE *c = (const BYTE *)f;
    if (!c || c[0] != 0x48 || c[1] != 0x8B || c[2] != 0x01)
        return -1;
    c += c[3] == 0x48 ? 4 : 3;
    if (c[0] != 0xFF)
        return -1;
    if (c[1] == 0x20)
        return 0;
    if (c[1] == 0x60 && c[2] < 0x80 && c[2] % 8 == 0)
        return c[2] / 8;
    if (c[1] == 0xA0 && *(const DWORD *)(c + 2) % 8 == 0 && *(const DWORD *)(c + 2) < 0x1000)
        return (int)(*(const DWORD *)(c + 2) / 8);
    return -1;
}

static void guard_fail(const wchar_t *why, const char *detail) {
    log_line(L"%lu steam guard FAILED: %ls %hs; stopping the test copy", GetTickCount() - g_start_ms, why, detail ? detail : "");
    TerminateProcess(GetCurrentProcess(), EXIT_STEAM_GUARD);
}

static DWORD WINAPI steam_guard(LPVOID unused) {
    (void)unused;
    HMODULE api;
    while (!(api = GetModuleHandleW(L"steam_api64.dll")))
        Sleep(10);
    int (*user)(void) = (int (*)(void))GetProcAddress(api, "SteamAPI_GetHSteamUser");
    void *(*get[2])(void) = {(void *(*)(void))GetProcAddress(api, "SteamAPI_SteamUserStats_v012"),
                             (void *(*)(void))GetProcAddress(api, "SteamAPI_SteamRemoteStorage_v016")};
    if (!user || !get[0] || !get[1])
        guard_fail(L"steam_api64.dll lacks an export the guard needs", NULL);
    log_line(L"%lu steam guard: steam_api64.dll loaded, waiting for SteamAPI_Init", GetTickCount() - g_start_ms);
    while (!user())
        Sleep(2);
    int slot[GUARD_N];
    for (int i = 0; i < GUARD_N; i++)  // read after init: the wrappers are final by then
        if ((slot[i] = flat_slot(GetProcAddress(api, k_guard[i].flat))) < 0)
            guard_fail(L"unexpected flat wrapper", k_guard[i].flat);
    void *obj[2] = {NULL, NULL};
    for (DWORD t0 = GetTickCount(); !(obj[0] = get[0]()) || !(obj[1] = get[1]()); Sleep(2))
        if (GetTickCount() - t0 > 10000)
            guard_fail(L"no ISteamUserStats/ISteamRemoteStorage interface 10 s after init", NULL);
    for (int i = 0; i < GUARD_N; i++) {
        void **entry = *(void ***)obj[k_guard[i].iface] + slot[i];
        DWORD old;
        if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &old))
            guard_fail(L"VirtualProtect failed for", k_guard[i].flat);
        if (k_guard[i].orig)
            *k_guard[i].orig = *entry;
        __atomic_store_n(entry, k_guard[i].stub, __ATOMIC_RELEASE);
        VirtualProtect(entry, sizeof(void *), old, &old);
        if (*entry != k_guard[i].stub)
            guard_fail(L"patch did not stick for", k_guard[i].flat);
    }
    log_line(L"%lu steam guard on: %d slots (user %d, stats %p, storage %p)", GetTickCount() - g_start_ms, GUARD_N, user(), obj[0], obj[1]);
    return 0;
}

static DWORD WINAPI write_status(LPVOID unused) {
    (void)unused;
    wchar_t exe[MAX_PATH] = L"?";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    HMODULE m = GetModuleHandleW(NULL);
    log_line(L"0 sandbox on: pid %lu exe %ls root %ls; SHGetKnownFolderPath slot rva 0x%llx, SHGetFolderPathW slot rva 0x%llx",
             GetCurrentProcessId(), exe, g_root, (unsigned long long)((char *)g_slot_known - (char *)m),
             (unsigned long long)((char *)g_slot_folder - (char *)m));
    return 0;
}

// ---------------------------------------------------------------- co-op core
// tow2coop.dll next to the exe is loaded from a thread (never inside DllMain). Without the file nothing happens.
static DWORD WINAPI load_core(LPVOID unused) {
    (void)unused;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : NULL;
    if (!slash || (size_t)(slash - path) + 14 >= MAX_PATH)
        return 0;
    wcscpy(slash + 1, L"tow2coop.dll");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        LoadLibraryW(path);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(self);
    HANDLE c = CreateThread(NULL, 0, load_core, NULL, 0, NULL);
    if (c)
        CloseHandle(c);
    DWORD n = GetEnvironmentVariableW(L"TOW2_SANDBOX_DIR", g_root, MAX_PATH);
    if (n == 0)
        return TRUE;  // pass-through: a player's install, or a copy without isolation
    if (n >= MAX_PATH - 64 || !(g_root[1] == L':' && g_root[2] == L'\\'))
        TerminateProcess(GetCurrentProcess(), EXIT_BAD_SANDBOX_DIR);  // only absolute drive paths, with room for sub-folders
    while (n > 3 && g_root[n - 1] == L'\\')
        g_root[--n] = 0;
    InitializeCriticalSection(&g_log_lock);
    g_start_ms = GetTickCount();
#ifndef TWIN_NO_PATCH  // test twin: build without the patch and the redirect test must fail
    HMODULE exe = GetModuleHandleW(NULL);
    if (patch_import(exe, "SHELL32.dll", "SHGetKnownFolderPath", (void *)hk_SHGetKnownFolderPath, (void **)&real_SHGetKnownFolderPath, &g_slot_known) <= 0)
        TerminateProcess(GetCurrentProcess(), EXIT_NO_KNOWNFOLDER_SLOT);
    if (patch_import(exe, "SHELL32.dll", "SHGetFolderPathW", (void *)hk_SHGetFolderPathW, (void **)&real_SHGetFolderPathW, &g_slot_folder) <= 0)
        TerminateProcess(GetCurrentProcess(), EXIT_NO_FOLDERPATH_SLOT);
    {
        unsigned h = 2166136261u;  // FNV-1a over the lower-cased sandbox path
        for (const wchar_t *c = g_root; *c; c++)
            h = (h ^ (unsigned)towlower(*c)) * 16777619u;
        _snwprintf(g_tag, sizeof g_tag / sizeof g_tag[0], L"-sbx%08x", h);
    }
    // Not fail-closed: without it a second copy only refuses to start.
    patch_import(exe, "KERNEL32.dll", "CreateMutexW", (void *)hk_CreateMutexW, (void **)&real_CreateMutexW, &g_slot_mutexw);
    patch_import(exe, "KERNEL32.dll", "CreateMutexA", (void *)hk_CreateMutexA, (void **)&real_CreateMutexA, &g_slot_mutexa);
    HANDLE g = CreateThread(NULL, 0, steam_guard, NULL, 0, NULL);
    if (!g)
        TerminateProcess(GetCurrentProcess(), EXIT_STEAM_GUARD);
    CloseHandle(g);
#endif
    HANDLE t = CreateThread(NULL, 0, write_status, NULL, 0, NULL);  // file I/O after the loader lock is released
    if (t)
        CloseHandle(t);
    return TRUE;
}
