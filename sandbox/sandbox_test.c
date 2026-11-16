// Test for the dsound.dll proxy. Built next to it, it imports dsound.dll (so the proxy loads at start-up,
// as in the game) and calls SHGetKnownFolderPath / SHGetFolderPathW through its own import table.
// Usage: sandbox_test sandbox|passthrough|steamfail <fake steam_api64.dll>
//   (sandbox and steamfail expect TOW2_SANDBOX_DIR to be set; steamfail must end with exit code 0x5AFE0010)
// Prints one PASS/FAIL line per check; the exit code is the number of failures.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

__declspec(dllimport) HRESULT WINAPI DirectSoundEnumerateW(void *cb, void *ctx);

static int failures;
static void check(int ok, const char *what, const wchar_t *detail) {
    printf("%s %s %ls\n", ok ? "PASS" : "FAIL", what, detail ? detail : L"");
    failures += !ok;
}

static BOOL CALLBACK count_device(GUID *g, LPCWSTR desc, LPCWSTR mod, void *ctx) {
    (void)g, (void)desc, (void)mod;
    (*(int *)ctx)++;
    return TRUE;
}

// The real answer, bypassing this exe's import table.
static HRESULT real_known(REFKNOWNFOLDERID id, PWSTR *out) {
    typedef HRESULT(WINAPI * fn)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);
    fn f = (fn)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetKnownFolderPath");
    return f(id, 0, NULL, out);
}

static void expect_known(const char *what, REFKNOWNFOLDERID id, const wchar_t *want) {
    PWSTR got = NULL, real = NULL;
    HRESULT hr = SHGetKnownFolderPath(id, 0, NULL, &got);
    real_known(id, &real);
    wchar_t detail[1024];
    _snwprintf(detail, 1024, L"got=%ls want=%ls", SUCCEEDED(hr) && got ? got : L"(error)", want ? want : real);
    detail[1023] = 0;
    int ok = SUCCEEDED(hr) && got && _wcsicmp(got, want ? want : real) == 0;
    if (ok && want)
        ok = GetFileAttributesW(got) != INVALID_FILE_ATTRIBUTES;  // a redirected folder must exist
    check(ok, what, detail);
    CoTaskMemFree(got);
    CoTaskMemFree(real);
}

// Steam guard: with the fake steam_api64.dll loaded, stat/achievement/cloud writes must stop at the guard in sandbox
// mode and reach the "real" slots in pass-through mode.
typedef _Bool (*f_name)(void *, const char *);
typedef _Bool (*f_obj)(void *);
typedef _Bool (*f_write)(void *, const char *, const void *, int);
typedef int (*f_read)(void *, const char *, void *, int);
typedef void (*f_cloud)(void *, _Bool);
static void steam_checks(const wchar_t *fake_path, int sandbox, int breakit) {
    HMODULE f = LoadLibraryW(fake_path);
    check(f != NULL, "fake steam_api64.dll loaded", fake_path);
    if (!f)
        return;
#define FAKE(t, n) ((t)GetProcAddress(f, n))
    LONG (*real_calls)(void) = FAKE(LONG(*)(void), "fake_real_calls");
    if (breakit)
        FAKE(void (*)(void), "fake_break_wrappers")();
    Sleep(100);  // the guard must wait for SteamAPI_Init, not act on the bare module
    FAKE(void (*)(int), "fake_set_user")(1);
    if (breakit) {
        Sleep(3000);
        check(0, "steamfail: the guard should have stopped the process by now", NULL);
        return;
    }
    void *stats = FAKE(void *(*)(void), "SteamAPI_SteamUserStats_v012")();
    void *storage = FAKE(void *(*)(void), "SteamAPI_SteamRemoteStorage_v016")();
    f_name set_ach = FAKE(f_name, "SteamAPI_ISteamUserStats_SetAchievement");
    LONG before = real_calls();
    _Bool r = 0;
    for (int i = 0; i < 150; i++) {  // the guard patches within a few ms of init; allow 3 s
        before = real_calls();
        r = set_ach(stats, "ACH_GUARD_TEST");
        if (!sandbox || real_calls() == before)
            break;
        Sleep(20);
    }
    wchar_t d[128];
    _snwprintf(d, 128, L"returned %d, real slot calls %ld -> %ld", r, before, real_calls());
    check(r && (sandbox ? real_calls() == before : real_calls() == before + 1),
          sandbox ? "SetAchievement stops at the guard" : "SetAchievement reaches Steam (no guard without sandbox)", d);
    if (!sandbox)
        return;
    before = real_calls();
    r = FAKE(f_obj, "SteamAPI_ISteamUserStats_StoreStats")(stats);
    _Bool w = FAKE(f_write, "SteamAPI_ISteamRemoteStorage_FileWrite")(storage, "guard_test.sav", "x", 1);
    FAKE(f_cloud, "SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp")(storage, 0);
    _snwprintf(d, 128, L"StoreStats %d, FileWrite %d, real slot calls +%ld", r, w, real_calls() - before);
    check(r && !w && real_calls() == before, "StoreStats, FileWrite and SetCloudEnabledForApp stop at the guard", d);
    before = real_calls();
    char buf[4];
    FAKE(f_read, "SteamAPI_ISteamRemoteStorage_FileRead")(storage, "guard_test.sav", buf, 4);
    _snwprintf(d, 128, L"real slot calls +%ld", real_calls() - before);
    check(real_calls() == before + 1, "FileRead passes through the guard", d);
}

int main(int argc, char **argv) {
    int sandbox = argc > 1 && (strcmp(argv[1], "sandbox") == 0 || strcmp(argv[1], "steamfail") == 0);
    wchar_t fake[MAX_PATH] = L"";
    if (argc > 2)
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, fake, MAX_PATH);
    wchar_t root[MAX_PATH] = L"", want[MAX_PATH];
    GetEnvironmentVariableW(L"TOW2_SANDBOX_DIR", root, MAX_PATH);
    check(!sandbox || root[0], "mode: TOW2_SANDBOX_DIR set for sandbox mode", root);

    _snwprintf(want, MAX_PATH, L"%ls\\SavedGames", root);
    expect_known("saved games folder", &FOLDERID_SavedGames, sandbox ? want : NULL);
    _snwprintf(want, MAX_PATH, L"%ls\\LocalAppData", root);
    expect_known("local app data folder", &FOLDERID_LocalAppData, sandbox ? want : NULL);
    expect_known("other folders pass through (Program Files)", &FOLDERID_ProgramFiles, NULL);

    wchar_t got[MAX_PATH] = L"", real[MAX_PATH] = L"";
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, got);
    typedef HRESULT(WINAPI * fpw)(HWND, int, HANDLE, DWORD, LPWSTR);
    ((fpw)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetFolderPathW"))(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, real);
    _snwprintf(want, MAX_PATH, L"%ls\\LocalAppData", root);
    check(SUCCEEDED(hr) && _wcsicmp(got, sandbox ? want : real) == 0, "SHGetFolderPathW local app data", got);

    int devices = 0;
    hr = DirectSoundEnumerateW((void *)count_device, &devices);
    wchar_t d[64];
    _snwprintf(d, 64, L"hr=0x%08lx devices=%d", (unsigned long)hr, devices);
    check(hr == S_OK, "DirectSound forwarded to the system DLL", d);

    // The game imports dsound by ordinal. The loaded dsound.dll must be the proxy, and each ordinal must reach the
    // function of that name (build.ps1 checks separately that the name/ordinal table equals System32's).
    HMODULE mine = GetModuleHandleW(L"dsound.dll");
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(mine, path, MAX_PATH);
    const char *names[] = {"DirectSoundCreate", "DirectSoundEnumerateA", "DirectSoundEnumerateW", "DllCanUnloadNow",
                           "DllGetClassObject", "DirectSoundCaptureCreate", "DirectSoundCaptureEnumerateA",
                           "DirectSoundCaptureEnumerateW", "GetDeviceID", "DirectSoundFullDuplexCreate",
                           "DirectSoundCreate8", "DirectSoundCaptureCreate8"};
    int ord_ok = mine && GetProcAddress(mine, "DirectSoundCreate8") && !wcsstr(_wcslwr(path), L"\\system32\\");
    for (int o = 1; ord_ok && o <= 12; o++)
        ord_ok = GetProcAddress(mine, MAKEINTRESOURCEA(o)) == GetProcAddress(mine, names[o - 1]) && GetProcAddress(mine, names[o - 1]);
    check(ord_ok, "proxy loaded, ordinals 1-12 reach the named functions", path);

    // Named mutexes: renamed per sandbox (a second test copy must not see the first one's instance lock).
    HANDLE mx = CreateMutexW(NULL, FALSE, L"TOW2SandboxTestMutex");
    HANDLE seen = OpenMutexW(SYNCHRONIZE, FALSE, L"TOW2SandboxTestMutex");  // OpenMutexW is not redirected
    check(mx && (sandbox ? seen == NULL : seen != NULL), sandbox ? "named mutex renamed per sandbox" : "named mutex keeps its name", NULL);
    if (seen)
        CloseHandle(seen);
    if (mx)
        CloseHandle(mx);
    if (fake[0])
        steam_checks(fake, sandbox, argc > 1 && strcmp(argv[1], "steamfail") == 0);
    if (sandbox) {
        wchar_t log[MAX_PATH];
        _snwprintf(log, MAX_PATH, L"%ls\\sandbox.log", root);
        Sleep(200);  // the status line is written by a thread the proxy starts
        check(GetFileAttributesW(log) != INVALID_FILE_ATTRIBUTES, "sandbox.log written", log);
    }
    printf("RESULT: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures;
}
