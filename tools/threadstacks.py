"""Sample a live process: per thread, its name, RIP and the return-address candidates found by scanning its stack
(values that fall inside a loaded module). Threads are suspended only while their context and stack are read.
Usage: python -I threadstacks.py <pid> [--match NAME] [--scan N]"""
import ctypes
import struct
import sys
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
for fn, res, args in [
    ("OpenProcess", wintypes.HANDLE, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]),
    ("OpenThread", wintypes.HANDLE, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]),
    ("SuspendThread", wintypes.DWORD, [wintypes.HANDLE]),
    ("ResumeThread", wintypes.DWORD, [wintypes.HANDLE]),
    ("GetThreadContext", wintypes.BOOL, [wintypes.HANDLE, ctypes.c_void_p]),
    ("ReadProcessMemory", wintypes.BOOL, [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]),
    ("CloseHandle", wintypes.BOOL, [wintypes.HANDLE]),
    ("CreateToolhelp32Snapshot", wintypes.HANDLE, [wintypes.DWORD, wintypes.DWORD]),
    ("GetThreadDescription", ctypes.c_long, [wintypes.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]),
]:
    f = getattr(k32, fn)
    f.restype, f.argtypes = res, args
psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("size", wintypes.DWORD), ("entry", ctypes.c_void_p)]


psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODULEINFO), wintypes.DWORD]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long), ("dwFlags", wintypes.DWORD)]


k32.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(THREADENTRY32)]


def modules(hp):
    arr = (ctypes.c_void_p * 2048)()
    need = wintypes.DWORD()
    psapi.EnumProcessModulesEx(hp, arr, ctypes.sizeof(arr), ctypes.byref(need), 3)
    out = []
    for i in range(need.value // 8):
        mi = MODULEINFO()
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleInformation(hp, arr[i], ctypes.byref(mi), ctypes.sizeof(mi))
        psapi.GetModuleBaseNameW(hp, arr[i], name, 260)
        out.append((mi.base or 0, mi.size, name.value))
    return sorted(out)


def where(mods, a):
    for base, size, name in mods:
        if base <= a < base + size:
            return f"{name}+{a - base:x}"
    return None


def main(argv):
    pid = int(argv[0])
    match = argv[argv.index("--match") + 1].lower() if "--match" in argv else ""
    scan = int(argv[argv.index("--scan") + 1]) if "--scan" in argv else 40
    hp = k32.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
    mods = modules(hp)
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(snap, ctypes.byref(te))
    tids = []
    while ok:
        if te.th32OwnerProcessID == pid:
            tids.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    raw = ctypes.create_string_buffer(1232 + 16)
    ctx = (ctypes.addressof(raw) + 15) & ~15
    for tid in tids:
        ht = k32.OpenThread(0x0002 | 0x0008 | 0x0400 | 0x0800, False, tid)  # SUSPEND_RESUME, GET_CONTEXT, QUERY_INFO, QUERY_LIMITED
        if not ht:
            continue
        desc = ctypes.c_wchar_p()
        name = desc.value if k32.GetThreadDescription(ht, ctypes.byref(desc)) >= 0 and desc.value else ""
        if match and match not in name.lower():
            k32.CloseHandle(ht)
            continue
        k32.SuspendThread(ht)
        ctypes.memset(ctx, 0, 1232)
        struct.pack_into("<I", (ctypes.c_char * 1232).from_address(ctx), 0x30, 0x100003)
        got = k32.GetThreadContext(ht, ctx)
        rip, rsp = struct.unpack_from("<Q", (ctypes.c_char * 1232).from_address(ctx), 0xF8)[0], struct.unpack_from("<Q", (ctypes.c_char * 1232).from_address(ctx), 0x98)[0]
        stack = ctypes.create_string_buffer(0x10000)
        n = ctypes.c_size_t()
        k32.ReadProcessMemory(hp, rsp, stack, len(stack), ctypes.byref(n))
        k32.ResumeThread(ht)
        k32.CloseHandle(ht)
        frames = []
        for off in range(0, n.value - 8, 8):
            v = struct.unpack_from("<Q", stack.raw, off)[0]
            w = where(mods, v)
            if w and not w.startswith(("ntdll.dll+", "KERNELBASE.dll+")):
                frames.append(w)
            if len(frames) >= scan:
                break
        print(f"--- {tid} {name!r} ctx {bool(got)} rip {where(mods, rip) or hex(rip)}")
        print("   " + "\n   ".join(frames))


if __name__ == "__main__":
    main(sys.argv[1:])
