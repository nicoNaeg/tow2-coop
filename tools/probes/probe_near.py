"""Print the UTF-16 strings stored near each hit of a UTF-16 needle (same source file often means same region).
Usage: python -I probe_near.py <exe> <needle> [radius]"""
import re, sys

data = open(sys.argv[1], "rb").read()
needle = sys.argv[2].encode("utf-16-le")
radius = int(sys.argv[3]) if len(sys.argv) > 3 else 1536
wide = re.compile(rb"(?:[\x20-\x7e]\x00){4,}")
s = 0
while True:
    j = data.find(needle, s)
    if j < 0:
        break
    s = j + 1
    lo, hi = max(0, j - radius), j + radius
    print(f"== hit at {j:#x}")
    for m in wide.finditer(data, lo, hi):
        print(f"  {m.start():#x}: {m.group().decode('utf-16-le')[:140]}")
