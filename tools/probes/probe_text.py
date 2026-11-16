"""Is the exe's code section plain on disk? Checks .pdata entries and common x64 byte patterns.
Usage: python -I probe_text.py <exe>"""
import struct, sys

data = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec, = struct.unpack_from("<H", data, pe + 6)
optsz, = struct.unpack_from("<H", data, pe + 20)
opt = pe + 24
secs = {}
so = opt + optsz
for k in range(nsec):
    nm = data[so + 40 * k:so + 40 * k + 8].split(b"\0")[0].decode()
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, so + 40 * k + 8)
    secs[nm] = (va, vsize, raw, rsize)
exc_rva, exc_size = struct.unpack_from("<II", data, opt + 112 + 8 * 3)
tva, tvs, traw, trs = secs[".text"]
pva, pvs, praw, prs = secs[".pdata"]
print(f"exception dir rva {exc_rva:#x} size {exc_size:,} (.pdata at {pva:#x})")

# .pdata: RUNTIME_FUNCTION {begin, end, unwind} must be sorted, inside .text, begin < end
n = exc_size // 12
ok = sorted_ok = 0
prev = 0
for i in range(n):
    b, e, u = struct.unpack_from("<III", data, praw + (exc_rva - pva) + 12 * i)
    if tva <= b < e <= tva + tvs:
        ok += 1
    if b >= prev:
        sorted_ok += 1
    prev = b
print(f".pdata entries {n:,}: inside .text and begin<end {ok:,}, sorted {sorted_ok:,}")

text = data[traw:traw + trs]
# A plain x64 code section is full of int3 padding and common prologue bytes; ciphertext is not.
pats = {"cc cc cc cc": b"\xcc" * 4, "48 89 5c 24 (mov [rsp+x],rbx)": b"\x48\x89\x5c\x24",
        "48 83 ec (sub rsp,imm8)": b"\x48\x83\xec", "c3 cc (ret;int3)": b"\xc3\xcc", "e8 (call rel32) per MB": None}
for name, p in pats.items():
    if p is None:
        print(f"  {name}: {text.count(b'\xe8') / (len(text) / 2**20):,.0f}")
    else:
        c = text.count(p)
        expect = len(text) / (256 ** len(p))
        print(f"  {name}: {c:,} (random data would give ~{expect:,.0f})")

# Check that function starts named by .pdata look like code (first byte histogram)
from collections import Counter
first = Counter()
for i in range(0, n, max(1, n // 5000)):
    b, e, u = struct.unpack_from("<III", data, praw + (exc_rva - pva) + 12 * i)
    first[data[traw + b - tva]] += 1
print("most common first bytes at .pdata function starts:", [(f"{k:02x}", v) for k, v in first.most_common(6)])
