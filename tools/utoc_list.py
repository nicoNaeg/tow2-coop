"""List every file path in an unencrypted IoStore container (.utoc, TOC version 5-8) from its directory index.
Read-only, no Oodle needed (the index is not compressed).
Usage: python -I utoc_list.py <file.utoc> <out.txt>"""
import struct, sys

INVALID = 0xFFFFFFFF


def fstring(b, p):
    n, = struct.unpack_from("<i", b, p)
    p += 4
    if n == 0:
        return "", p
    if n < 0:
        return b[p:p - 2 * n - 2].decode("utf-16-le"), p - 2 * n
    return b[p:p + n - 1].decode("utf-8", "replace"), p + n


def main(utoc, out):
    b = open(utoc, "rb").read()
    assert b[:16] == b"-==--==--==--==-", "not a utoc"
    ver = b[16]
    hdr_size, entries, cblocks, cblock_size, cm_count, cm_len, _bs, dir_size, _parts = struct.unpack_from("<9I", b, 20)
    flags = b[80]
    seeds, = struct.unpack_from("<I", b, 84)
    no_hash, = struct.unpack_from("<I", b, 96)
    assert flags & 8 and not flags & 2, f"needs Indexed and not Encrypted (flags {flags})"
    p = hdr_size + entries * 12 + entries * 10
    if ver >= 4:
        p += seeds * 4
    if ver >= 5:
        p += no_hash * 4
    p += cblocks * cblock_size + cm_count * cm_len
    assert not flags & 4, "signed containers not handled"
    d = b[p:p + dir_size]
    mount, q = fstring(d, 0)
    nd, = struct.unpack_from("<i", d, q); q += 4
    dirs = [struct.unpack_from("<4I", d, q + 16 * i) for i in range(nd)]; q += 16 * nd
    nf, = struct.unpack_from("<i", d, q); q += 4
    files = [struct.unpack_from("<3I", d, q + 12 * i) for i in range(nf)]; q += 12 * nf
    ns, = struct.unpack_from("<i", d, q); q += 4
    strings = []
    for _ in range(ns):
        s, q = fstring(d, q)
        strings.append(s)
    paths = []
    stack = [(0, mount.rstrip("/"))]
    while stack:
        i, base = stack.pop()
        name, child, _sib, f = dirs[i]
        here = base if name == INVALID else f"{base}/{strings[name]}"
        while f != INVALID:
            paths.append(f"{here}/{strings[files[f][0]]}")
            f = files[f][1]
        while child != INVALID:
            stack.append((child, here))
            child = dirs[child][2]
    paths.sort()
    with open(out, "w", encoding="utf-8") as o:
        o.write("\n".join(paths) + "\n")
    print(f"toc version {ver}, mount {mount!r}, {nd} dirs, {nf} files, {ns} strings -> {len(paths)} paths in {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
