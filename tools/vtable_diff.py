"""Compare the virtual-function lists of two UE4SS VTableLayout ini files for the UObject chain.
Usage: python -I vtable_diff.py <stock template.ini> <game layout.ini> [classes...]"""
import sys

def sections(path):
    out, cur = {}, None
    for line in open(path, encoding="utf-8-sig"):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            cur = s[1:-1]
            out[cur] = []
        elif cur and s and not s.startswith(";"):
            out[cur].append(s)
    return out

def index_of(sec, classes, name):
    """Slot of `name` counting each class's new virtuals after its bases (the dtor is shared, slot 0)."""
    slot = 0
    for c in classes:
        for fn in sec.get(c, []):
            if fn == "__vecDelDtor":
                continue
            slot += 1
            if fn == name:
                return slot
    return None

stock, game = sections(sys.argv[1]), sections(sys.argv[2])
chain = sys.argv[3:] or ["UObjectBase", "UObjectBaseUtility", "UObject"]
for c in chain:
    a, b = stock.get(c, []), game.get(c, [])
    print(f"[{c}] stock {len(a)} entries, game {len(b)} entries")
    for fn in b:
        if fn not in a:
            print(f"  + {fn}")
    for fn in a:
        if fn not in b:
            print(f"  - {fn}")
for fn in ("GetWorld", "ProcessEvent", "ProcessConsoleExec"):
    print(f"{fn}: stock slot {index_of(stock, chain, fn)}, game slot {index_of(game, chain, fn)}")
