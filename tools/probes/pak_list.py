"""List an unencrypted UE pak (version 11) from its full directory index. Read-only.
Usage: python -I pak_list.py <pak> <out.tsv>
Writes path, offset, uncompressed size, compressed size, compression method index per file."""
import struct, sys

pak, out = sys.argv[1], sys.argv[2]
f = open(pak, "rb")
f.seek(-512, 2)
tail = f.read()
i = tail.rfind(struct.pack("<I", 0x5A6F12E1))
ver, idx_off, idx_size = struct.unpack_from("<iqq", tail, i + 4)
methods = [""] + [tail[i + 44 + k * 32:i + 44 + (k + 1) * 32].split(b"\0")[0].decode() for k in range(5)]
assert ver == 11 and tail[i - 1] == 0, (ver, tail[i - 1])

class R:
    def __init__(s, b): s.b, s.p = b, 0
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def i32(s): v, = struct.unpack_from("<i", s.b, s.p); s.p += 4; return v
    def u32(s): v, = struct.unpack_from("<I", s.b, s.p); s.p += 4; return v
    def u64(s): v, = struct.unpack_from("<Q", s.b, s.p); s.p += 8; return v
    def raw(s, n): v = s.b[s.p:s.p + n]; s.p += n; return v
    def fstr(s):
        n = s.i32()
        if n == 0: return ""
        if n < 0: return s.raw(-n * 2)[:-2].decode("utf-16-le")
        return s.raw(n)[:-1].decode("utf-8", "replace")

f.seek(idx_off)
r = R(f.read(idx_size))
mount = r.fstr()
num = r.i32()
seed = r.u64()
if r.u32():  # path hash index
    r.u64(); r.u64(); r.raw(20)
has_full = r.u32()
assert has_full, "no full directory index"
fdi_off, fdi_size = r.u64(), r.u64(); r.raw(20)
enc_size = r.i32()
encoded = r.raw(enc_size)
nonenc = r.i32()

f.seek(fdi_off)
d = R(f.read(fdi_size))
rows = []
for _ in range(d.i32()):
    dname = d.fstr()
    for _ in range(d.i32()):
        fname = d.fstr()
        loc = d.i32()
        if loc < 0:
            rows.append((mount + dname + fname, -1, -1, -1, "nonencoded"))
            continue
        e = R(encoded); e.p = loc
        flags = e.u32()
        off = e.u32() if flags & (1 << 31) else e.u64()
        usize = e.u32() if flags & (1 << 30) else e.u64()
        cm = (flags >> 23) & 0x3F
        csize = (e.u32() if flags & (1 << 29) else e.u64()) if cm else usize
        rows.append((mount + dname + fname, off, usize, csize, methods[cm] if cm < len(methods) else f"#{cm}"))
with open(out, "w", encoding="utf-8") as o:
    o.write("path\toffset\tsize\tcompressed\tmethod\n")
    for row in sorted(rows):
        o.write("\t".join(map(str, row)) + "\n")
print(f"mount {mount!r}, entries {num}, listed {len(rows)}, written to {out}")
