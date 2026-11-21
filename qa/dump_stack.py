"""A game crash's minidump (UEMinidump.dmp): the faulting thread's stack, scanned for values that point into the game
exe or the mod, deepest call last (no debugger needed). A scan, not an unwind: a value can be a stale return address
(hk_tick tail-calls the game's tick, so `tow2coop.dll+0x...` right after a `join::frame` call is left over, run 178).
Usage: python -I dump_stack.py <UEMinidump.dmp> [max values]"""
import struct
import sys


def main(path, most=120):
    d = open(path, "rb").read()
    n, dirrva = struct.unpack_from("<II", d, 8)
    streams = {}
    for i in range(n):
        t, size, rva = struct.unpack_from("<III", d, dirrva + 12 * i)
        streams.setdefault(t, (size, rva))

    def text(rva):
        ln, = struct.unpack_from("<I", d, rva)
        return d[rva + 4: rva + 4 + ln].decode("utf-16le")

    mods = []
    rva = streams[4][1]  # ModuleListStream: MINIDUMP_MODULE is 108 bytes
    for i in range(struct.unpack_from("<I", d, rva)[0]):
        o = rva + 4 + 108 * i
        base, size = struct.unpack_from("<QI", d, o)
        mods.append((base, size, text(struct.unpack_from("<I", d, o + 20)[0]).split("\\")[-1]))

    def sym(a):
        return next((f"{nm}+0x{a - b:x}" for b, s, nm in mods if b <= a < b + s), None)

    rva = streams[6][1]  # ExceptionStream: thread id, EXCEPTION_RECORD64 (152 bytes), then the CONTEXT's location
    tid, = struct.unpack_from("<I", d, rva)
    code, _, _, at = struct.unpack_from("<IIQQ", d, rva + 8)
    nparams, = struct.unpack_from("<I", d, rva + 8 + 24)
    addr = struct.unpack_from("<Q", d, rva + 8 + 32 + 8)[0] if nparams >= 2 else 0
    crva = struct.unpack_from("<II", d, rva + 8 + 152)[1]
    rsp, = struct.unpack_from("<Q", d, crva + 0x98)
    print(f"thread {tid}: exception 0x{code:x} at {sym(at) or hex(at)}, address 0x{addr:x}")
    rva = streams[3][1]  # ThreadListStream: MINIDUMP_THREAD is 48 bytes, its stack's descriptor at +24
    for i in range(struct.unpack_from("<I", d, rva)[0]):
        o = rva + 4 + 48 * i
        if struct.unpack_from("<I", d, o)[0] != tid:
            continue
        start, size, mrva = struct.unpack_from("<QII", d, o + 24)
        vals = [sym(struct.unpack_from("<Q", d, mrva + off)[0]) for off in range((max(rsp, start) - start) & ~7, size - 7, 8)]
        vals = [v for v in vals if v and v.lower().startswith(("theouterworlds2", "tow2coop"))]
        print(f"{len(vals)} code values on the stack:", " ".join(v.replace("TheOuterWorlds2-Win64-Shipping.exe", "game") for v in vals[:most]))


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 120)
