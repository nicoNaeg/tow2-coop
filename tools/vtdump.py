"""Read an object's vtable from a running test copy (read-only) and print each slot as module+RVA.
Usage: python -I vtdump.py <pid> <object address hex> [first] [count]
Only for test copies we started; it opens the process with read rights only."""
import ctypes, ctypes.wintypes as wt, sys

PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x0010, 0x0400
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD, ctypes.POINTER(wt.DWORD)]
psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD]


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
        raise OSError(f"cannot read {n} bytes at {addr:#x}")
    return buf.raw


def main_module(h):
    mods = (ctypes.c_void_p * 1)()
    need = wt.DWORD()
    psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need))
    info = (ctypes.c_void_p * 3)()  # MODULEINFO: base, size (low 32 bits), entry
    psapi.GetModuleInformation(h, mods[0], info, ctypes.sizeof(info))
    return info[0], ctypes.c_uint32(info[1] or 0).value


def main(argv):
    pid, obj = int(argv[1]), int(argv[2], 16)
    first = int(argv[3]) if len(argv) > 3 else 0
    count = int(argv[4]) if len(argv) > 4 else 120
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        sys.exit(f"OpenProcess failed: {ctypes.get_last_error()}")
    base, size = main_module(h)
    vt = int.from_bytes(read(h, obj, 8), "little")
    print(f"module base {base:#x} size {size:#x}; object {obj:#x} vtable {vt:#x} (rva {vt - base:#x})")
    raw = read(h, vt + 8 * first, 8 * count)
    for i in range(count):
        f = int.from_bytes(raw[8 * i:8 * i + 8], "little")
        inside = base <= f < base + size
        print(f"[{first + i:3d}] +{8 * (first + i):#05x}  {f:#x}  " + (f"rva {f - base:#x}" if inside else "outside module"))


if __name__ == "__main__":
    main(sys.argv)
