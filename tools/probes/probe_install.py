"""Read-only probe of a UE5 install: pak footer, utoc header, PE sections/imports, interesting strings.
Usage: python -I probe_install.py <game dir>"""
import mmap, os, struct, sys

game = sys.argv[1]
paks = os.path.join(game, "Arkansas", "Content", "Paks")
exe = os.path.join(game, "Arkansas", "Binaries", "Win64", "TheOuterWorlds2-Win64-Shipping.exe")

# --- pak footer (FPakInfo, UE 4.23+ layout: guid16, bEnc1, magic4, ver4, off8, size8, hash20, 5x32 names)
for name in ("Arkansas-Windows.pak",):
    p = os.path.join(paks, name)
    with open(p, "rb") as f:
        f.seek(-512, 2)
        tail = f.read()
    i = tail.rfind(struct.pack("<I", 0x5A6F12E1))
    if i < 0:
        print(name, "pak magic not found in last 512 bytes")
        continue
    guid = tail[i - 17:i - 1]
    enc = tail[i - 1]
    ver, off, size = struct.unpack_from("<iqq", tail, i + 4)
    names = [tail[i + 44 + k * 32:i + 44 + (k + 1) * 32].split(b"\0")[0].decode("ascii", "replace") for k in range(5)]
    print(f"{name}: pak version {ver}, index offset {off}, index size {size}, encrypted index {enc}, "
          f"key guid {'zero' if guid == bytes(16) else guid.hex()}, compression {[n for n in names if n]}")

# --- utoc header (FIoStoreTocHeader)
for name in ("Arkansas-Windows.utoc", "global.utoc"):
    with open(os.path.join(paks, name), "rb") as f:
        h = f.read(144)
    magic = h[:16]
    ver = h[16]
    (hdr_size, entry_count, cblk_count, cblk_size, cm_count, cm_len, cblock_size, dir_size, part_count) = struct.unpack_from("<9I", h, 20)
    container_id = struct.unpack_from("<Q", h, 56)[0]
    guid = h[64:80]
    flags = h[80]
    fl = [n for b, n in ((1, "Compressed"), (2, "Encrypted"), (4, "Signed"), (8, "Indexed"), (16, "OnDemand")) if flags & b]
    print(f"{name}: magic ok {magic == b'-==--==--==--==-'}, toc version {ver}, entries {entry_count}, "
          f"compression methods {cm_count}, block size {cblock_size}, dir index size {dir_size}, partitions {part_count}, "
          f"flags {fl or ['None']}, key guid {'zero' if guid == bytes(16) else guid.hex()}")

# --- PE sections and imports
with open(exe, "rb") as f:
    data = f.read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec, = struct.unpack_from("<H", data, pe + 6)
optsz, = struct.unpack_from("<H", data, pe + 20)
opt = pe + 24
entry_rva, = struct.unpack_from("<I", data, opt + 16)
image_base, = struct.unpack_from("<Q", data, opt + 24)
dllchar, = struct.unpack_from("<H", data, opt + 70)
ndirs, = struct.unpack_from("<I", data, opt + 108)
dirs = [struct.unpack_from("<II", data, opt + 112 + 8 * k) for k in range(ndirs)]
secs = []
so = opt + optsz
for k in range(nsec):
    nm = data[so + 40 * k:so + 40 * k + 8].split(b"\0")[0].decode()
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, so + 40 * k + 8)
    ch, = struct.unpack_from("<I", data, so + 40 * k + 36)
    secs.append((nm, va, vsize, raw, rsize, ch))
def rva2off(rva):
    for nm, va, vs, raw, rs, ch in secs:
        if va <= rva < va + max(vs, rs):
            return raw + rva - va
    return None
def entropy(b):
    import math
    if not b:
        return 0.0
    c = [0] * 256
    for x in b:
        c[x] += 1
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c if v)
print(f"\nexe: image base {image_base:#x}, entry rva {entry_rva:#x}, ASLR {bool(dllchar & 0x40)}")
for nm, va, vs, raw, rs, ch in secs:
    sample = data[raw:raw + min(rs, 1 << 20)]
    tag = " <- entry" if va <= entry_rva < va + vs else ""
    print(f"  {nm:9} rva {va:#010x} vsize {vs:>11,} raw {rs:>11,} chars {ch:#010x} entropy(1MB) {entropy(sample):.2f}{tag}")

imp_rva = dirs[1][0]
imports = {}
o = rva2off(imp_rva)
while o is not None:
    ilt, ts, fwd, name_rva, iat = struct.unpack_from("<IIIII", data, o)
    if name_rva == 0:
        break
    no = rva2off(name_rva)
    dll = data[no:data.index(b"\0", no)].decode()
    funcs = []
    t = rva2off(ilt or iat)
    while t is not None:
        v, = struct.unpack_from("<Q", data, t)
        if v == 0:
            break
        if v & (1 << 63):
            funcs.append(f"#{v & 0xFFFF}")
        else:
            ho = rva2off(v & 0x7FFFFFFF)
            funcs.append(data[ho + 2:data.index(b"\0", ho + 2)].decode() if ho else "?")
        t += 8
    imports[dll] = funcs
    o += 20
print(f"\nimports: {len(imports)} dlls")
for dll, funcs in imports.items():
    keep = [f for f in funcs if any(k in f for k in ("Mutex", "FindWindow", "KnownFolder", "SHGetFolder", "CreateFile", "Steam", "XGame", "XUser"))]
    print(f"  {dll}: {len(funcs)} funcs{'; e.g. ' + ', '.join(keep[:12]) if keep else ''}")

# --- strings (ASCII and UTF-16LE) outside the code section
needles = ["IpNetDriver", "GameNetDriver", "NetDriverDefinitions", "DemoNetDriver", "BeaconNetDriver",
           "SteamNetDriver", "SteamSocketsNetDriver", "OnlineSubsystemSteam", "OnlineSubsystemGDK", "OnlineSubsystemNull",
           "OnlineSubsystemUtils", "SocketSubsystemSteam", "ReplicationGraph", "IrisNetDriver", "?listen", "listen",
           "steam_appid.txt", "SteamAppId", "RestartAppIfNecessary", "Saved Games", "SaveGameSystem",
           "PlayFab", "PartyNetDriver", "GDKNetDriver", "Multiplayer", "bUseIris", "NetMode",
           "UE4SS", "Dumper", "-nosteam", "AllowMultipleInstances", "SingleInstance"]
mm = data
for nd in needles:
    a = nd.encode()
    w = nd.encode("utf-16-le")
    ca, cw = mm.count(a), mm.count(w)
    if ca or cw:
        # show up to 2 surrounding strings for context (wide first)
        ctx = []
        for pat, wide in ((w, True), (a, False)):
            start = 0
            for _ in range(2):
                j = mm.find(pat, start)
                if j < 0:
                    break
                start = j + 1
                if wide:
                    s = j
                    while s >= 2 and 32 <= mm[s - 2] < 127 and mm[s - 1] == 0:
                        s -= 2
                    e = j
                    while e + 1 < len(mm) and 32 <= mm[e] < 127 and mm[e + 1] == 0:
                        e += 2
                    ctx.append("W:" + mm[s:e].decode("utf-16-le")[:90])
                else:
                    s = j
                    while s >= 1 and 32 <= mm[s - 1] < 127:
                        s -= 1
                    e = j
                    while e < len(mm) and 32 <= mm[e] < 127:
                        e += 1
                    ctx.append("A:" + mm[s:e].decode("ascii")[:90])
        print(f"{nd!r}: ascii {ca}, utf16 {cw} | " + " | ".join(ctx))
    else:
        print(f"{nd!r}: none")
