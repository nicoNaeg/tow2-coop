// Detours for known prologues: the first `len` bytes of the target must equal `expect` (whole, position-independent
// instructions, at least 14 bytes). They are copied into a trampoline that jumps back, and replaced by an absolute
// jump to the hook. Other threads are suspended while the bytes change and none may be executing them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hook {
static void abs_jump(uint8_t* at, const void* to) {
    at[0] = 0xFF, at[1] = 0x25;  // jmp qword ptr [rip+0]
    memset(at + 2, 0, 4);
    memcpy(at + 6, &to, 8);
}

static std::vector<HANDLE> suspend_others() {
    std::vector<HANDLE> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
        out.push_back(h);
    }
    CloseHandle(snap);
    return out;
}

static void resume(std::vector<HANDLE>& hs) {
    for (HANDLE h : hs) { ResumeThread(h); CloseHandle(h); }
    hs.clear();
}

static bool any_inside(const std::vector<HANDLE>& hs, const uint8_t* lo, const uint8_t* hi) {
    for (HANDLE h : hs) {
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &c) && c.Rip >= (DWORD64)(uintptr_t)lo && c.Rip < (DWORD64)(uintptr_t)hi) return true;
    }
    return false;
}

// Returns the trampoline (call it to run the original), or nullptr with `err` set.
void* install(void* target_, const uint8_t* expect, size_t len, void* detour, std::string& err) {
    uint8_t* target = static_cast<uint8_t*>(target_);
    if (len < 14 || len > 32) { err = "prologue length must be 14..32"; return nullptr; }
    if (memcmp(target, expect, len) != 0) { err = "unexpected bytes at the hook site (unsupported build?)"; return nullptr; }
    uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) { err = "VirtualAlloc failed"; return nullptr; }
    memcpy(tramp, target, len);
    abs_jump(tramp + len, target + len);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    uint8_t patch[32];
    abs_jump(patch, detour);
    memset(patch + 14, 0x90, len - 14);
    for (int attempt = 0; attempt < 50; attempt++) {
        std::vector<HANDLE> hs = suspend_others();
        if (any_inside(hs, target, target + len)) { resume(hs); Sleep(1); continue; }
        DWORD old;
        if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old)) { resume(hs); err = "VirtualProtect failed"; return nullptr; }
        memcpy(target, patch, len);
        VirtualProtect(target, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, len);
        resume(hs);
        return tramp;
    }
    err = "a thread kept executing the hook site";
    return nullptr;
}
}  // namespace hook
