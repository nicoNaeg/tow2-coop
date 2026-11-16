"""Write a minidump (thread stacks, modules, thread names) of a running process. Read it with mdump.py.
Usage: python -I dumpproc.py <pid> <out.dmp>"""
import ctypes
import sys
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
dbg.MiniDumpWriteDump.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
dbg.MiniDumpWriteDump.restype = wintypes.BOOL

PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, PROCESS_DUP_HANDLE = 0x400, 0x10, 0x40
MiniDumpWithThreadInfo, MiniDumpWithUnloadedModules = 0x1000, 0x20


def main(pid, out):
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, False, pid)
    if not h:
        sys.exit(f"OpenProcess failed: {ctypes.get_last_error()}")
    f = k32.CreateFileW(out, 0x40000000, 0, None, 2, 0x80, None)  # GENERIC_WRITE, CREATE_ALWAYS
    if f in (None, wintypes.HANDLE(-1).value):
        sys.exit(f"CreateFile failed: {ctypes.get_last_error()}")
    ok = dbg.MiniDumpWriteDump(h, pid, f, MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules, None, None, None)
    err = ctypes.get_last_error()
    k32.CloseHandle(f)
    k32.CloseHandle(h)
    if not ok:
        sys.exit(f"MiniDumpWriteDump failed: {err}")
    print(f"wrote {out}")


if __name__ == "__main__":
    main(int(sys.argv[1]), sys.argv[2])
