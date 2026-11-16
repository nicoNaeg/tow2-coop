"""Minimal minidump reader: exception, modules, and per-thread RIP plus a heuristic stack scan
(return-address candidates that fall inside a loaded module). No symbols needed.
Usage: python -I mdump.py <file.dmp> [--threads all|crashed] [--scan N]"""
import struct, sys

def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]

def utf16_at(b, rva):
    n = u32(b, rva)
    return b[rva + 4: rva + 4 + n].decode("utf-16-le", "replace")

def load(path):
    b = open(path, "rb").read()
    assert b[:4] == b"MDMP", "not a minidump"
    nstreams, dir_rva = u32(b, 8), u32(b, 12)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from("<III", b, dir_rva + 12 * i)
        streams[t] = (size, rva)
    return b, streams

def modules(b, streams):
    size, rva = streams[4]
    n = u32(b, rva)
    out = []
    for i in range(n):
        o = rva + 4 + 108 * i
        base, sz = u64(b, o), u32(b, o + 8)
        name = utf16_at(b, u32(b, o + 20)).rsplit("\\", 1)[-1]
        out.append((base, sz, name))
    return sorted(out)

def where(mods, a):
    for base, sz, name in mods:
        if base <= a < base + sz:
            return f"{name}+{a - base:x}"
    return None

def memory(b, streams):
    """List of (start, bytes) from Memory64ListStream (9) or MemoryListStream (5)."""
    regions = []
    if 9 in streams:
        size, rva = streams[9]
        n, base_rva = u64(b, rva), u64(b, rva + 8)
        off = base_rva
        for i in range(n):
            start, sz = struct.unpack_from("<QQ", b, rva + 16 + 16 * i)
            regions.append((start, off, sz))
            off += sz
    elif 5 in streams:
        size, rva = streams[5]
        n = u32(b, rva)
        for i in range(n):
            start, dsz, drva = struct.unpack_from("<QII", b, rva + 4 + 16 * i)
            regions.append((start, drva, dsz))
    return regions

def read(b, regions, addr, n):
    for start, off, sz in regions:
        if start <= addr and addr + n <= start + sz:
            return b[off + addr - start: off + addr - start + n]
    return None

def main(argv):
    path = argv[0]
    scan = int(argv[argv.index("--scan") + 1]) if "--scan" in argv else 48
    b, streams = load(path)
    mods = modules(b, streams)
    regions = memory(b, streams)
    crashed_tid = None
    if 6 in streams:
        size, rva = streams[6]
        crashed_tid = u32(b, rva)
        code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", b, rva + 8)
        params = struct.unpack_from("<2Q", b, rva + 8 + 32) if nparams >= 2 else ()
        print(f"exception 0x{code:08x} in thread {crashed_tid} at {where(mods, addr) or hex(addr)} params {[hex(p) for p in params]}")
    names = {}
    if 24 in streams:  # ThreadNamesStream
        size, rva = streams[24]
        n = u32(b, rva)
        for i in range(n):
            tid, name_rva = struct.unpack_from("<IQ", b, rva + 4 + 12 * i)
            names[tid] = utf16_at(b, name_rva)
    size, rva = streams[3]
    n = u32(b, rva)
    want = "all" if "--threads" in argv and argv[argv.index("--threads") + 1] == "all" else "interesting"
    for i in range(n):
        o = rva + 4 + 48 * i
        tid = u32(b, o)
        stack_start = u64(b, o + 24)
        ctx_size, ctx_rva = u32(b, o + 40), u32(b, o + 44)
        rip, rsp = u64(b, ctx_rva + 0xF8), u64(b, ctx_rva + 0x98)
        w = where(mods, rip) or hex(rip)
        frames = []
        for k in range(0, 8 * 4096, 8):
            v = read(b, regions, rsp + k, 8)
            if v is None:
                break
            a = u64(v, 0)
            m = where(mods, a)
            if m and not m.startswith(("ntdll", "KERNELBASE")):
                frames.append(m)
            if len(frames) >= scan:
                break
        boring = w.startswith(("ntdll+", "win32u+")) and all(f.startswith(("KERNEL32", "TheOuterWorlds2-Win64-Shipping.exe+ff7c43", "TheOuterWorlds2-Win64-Shipping.exe+147a257")) for f in frames[:2])
        interesting = tid == crashed_tid or any("UE4SS" in f or "dwmapi" in f or "dsound" in f for f in [w] + frames)
        if want == "all" or interesting:
            print(f"--- thread {tid} {names.get(tid, '')} rip {w}")
            print("   " + "\n   ".join(frames[:scan]))

if __name__ == "__main__":
    main(sys.argv[1:])
