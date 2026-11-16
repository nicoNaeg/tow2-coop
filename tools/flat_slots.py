"""Print the vtable slot each Steamworks flat-API wrapper jumps through (mov rax,[rcx]; jmp [rax+disp]).
Usage: python -I flat_slots.py <steam_api64.dll> <export-prefix>..."""
import struct, sys

def pe(path):
    b = open(path, "rb").read()
    e = struct.unpack_from("<I", b, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", b, e + 6)[0], struct.unpack_from("<H", b, e + 20)[0]
    opt = e + 24
    exp_rva = struct.unpack_from("<I", b, opt + 112)[0]
    secs = [struct.unpack_from("<8sIIII", b, opt + optsz + 40 * i) for i in range(nsec)]
    def off(rva):
        for name, vsz, va, rsz, raw in secs:
            if va <= rva < va + max(vsz, rsz):
                return raw + rva - va
        raise ValueError(hex(rva))
    o = off(exp_rva)
    n_names, funcs, names, ords = struct.unpack_from("<IIII", b, o + 24)[0], *struct.unpack_from("<III", b, o + 28)
    out = {}
    for i in range(n_names):
        nm_rva = struct.unpack_from("<I", b, off(names) + 4 * i)[0]
        nm = b[off(nm_rva): b.index(b"\0", off(nm_rva))].decode()
        ordi = struct.unpack_from("<H", b, off(ords) + 2 * i)[0]
        out[nm] = struct.unpack_from("<I", b, off(funcs) + 4 * ordi)[0]
    return b, off, out

def slot(code):
    if code[:5] == b"\x48\x8b\x01\xff\x60":
        return code[5] // 8
    if code[:5] == b"\x48\x8b\x01\xff\xa0":
        return struct.unpack_from("<I", code, 5)[0] // 8
    return None

if __name__ == "__main__":
    b, off, exps = pe(sys.argv[1])
    for name in sorted(exps):
        if any(name.startswith(p) for p in sys.argv[2:]):
            code = b[off(exps[name]): off(exps[name]) + 12]
            print(f"{name:60s} slot {slot(code)}  {code[:9].hex(' ')}")
