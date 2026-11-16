// Fake steam_api64.dll for the steam guard test: the flat wrappers are byte-identical in shape to the real
// Steamworks 1.57 ones (mov rax,[rcx]; rex.w jmp [rax+disp]), and every vtable slot counts the calls that reach it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static volatile LONG g_user, g_real_calls;
static _Bool real_slot(void) { InterlockedIncrement(&g_real_calls); return 1; }  // any signature: arguments ignored
static void *vt_stats[64], *vt_storage[64];
static struct { void **vt; } stats = {vt_stats}, storage = {vt_storage};

__declspec(dllexport) void fake_set_user(int u) { g_user = u; }
__declspec(dllexport) LONG fake_real_calls(void) { return g_real_calls; }
__declspec(dllexport) int SteamAPI_GetHSteamUser(void) { return g_user; }
__declspec(dllexport) void *SteamAPI_SteamUserStats_v012(void) { return g_user ? &stats : NULL; }
__declspec(dllexport) void *SteamAPI_SteamRemoteStorage_v016(void) { return g_user ? &storage : NULL; }
// Break one wrapper so the guard cannot read its slot (fail-closed test).
__declspec(dllexport) void fake_break_wrappers(void);

#define WRAP8(name, disp) ".globl " #name "\n" #name ": .byte 0x48,0x8b,0x01,0x48,0xff,0x60," #disp "\n int3\n"
#define WRAP32(name, disp) ".globl " #name "\n" #name ": .byte 0x48,0x8b,0x01,0x48,0xff,0xa0\n .long " #disp "\n int3\n"
__asm__(".text\n"
        WRAP8(SteamAPI_ISteamUserStats_SetStatFloat, 0x18) WRAP8(SteamAPI_ISteamUserStats_SetStatInt32, 0x20)
        WRAP8(SteamAPI_ISteamUserStats_UpdateAvgRateStat, 0x28) WRAP8(SteamAPI_ISteamUserStats_SetAchievement, 0x38)
        WRAP8(SteamAPI_ISteamUserStats_ClearAchievement, 0x40) WRAP8(SteamAPI_ISteamUserStats_StoreStats, 0x50)
        WRAP8(SteamAPI_ISteamUserStats_IndicateAchievementProgress, 0x68) WRAP32(SteamAPI_ISteamUserStats_ResetAllStats, 0xa8)
        ".globl SteamAPI_ISteamRemoteStorage_FileWrite\nSteamAPI_ISteamRemoteStorage_FileWrite: .byte 0x48,0x8b,0x01,0x48,0xff,0x20\n int3\n"
        WRAP8(SteamAPI_ISteamRemoteStorage_FileRead, 0x08) WRAP8(SteamAPI_ISteamRemoteStorage_FileWriteAsync, 0x10)
        WRAP8(SteamAPI_ISteamRemoteStorage_FileReadAsync, 0x18) WRAP8(SteamAPI_ISteamRemoteStorage_FileForget, 0x28)
        WRAP8(SteamAPI_ISteamRemoteStorage_FileDelete, 0x30) WRAP8(SteamAPI_ISteamRemoteStorage_FileShare, 0x38)
        WRAP8(SteamAPI_ISteamRemoteStorage_SetSyncPlatforms, 0x40) WRAP8(SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen, 0x48)
        WRAP8(SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk, 0x50) WRAP8(SteamAPI_ISteamRemoteStorage_FileWriteStreamClose, 0x58)
        WRAP8(SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel, 0x60) WRAP32(SteamAPI_ISteamRemoteStorage_GetFileCount, 0x90)
        WRAP32(SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp, 0xb8) WRAP32(SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch, 0x1c8)
        WRAP32(SteamAPI_ISteamRemoteStorage_EndFileWriteBatch, 0x1d0));

void fake_break_wrappers(void) {
    extern char SteamAPI_ISteamUserStats_SetAchievement[];
    DWORD old;
    VirtualProtect(SteamAPI_ISteamUserStats_SetAchievement, 8, PAGE_EXECUTE_READWRITE, &old);
    SteamAPI_ISteamUserStats_SetAchievement[0] = (char)0xC3;  // ret: no longer a vtable jump
    VirtualProtect(SteamAPI_ISteamUserStats_SetAchievement, 8, old, &old);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    (void)self, (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        for (int i = 0; i < 64; i++)
            vt_stats[i] = vt_storage[i] = (void *)real_slot;
    return TRUE;
}
