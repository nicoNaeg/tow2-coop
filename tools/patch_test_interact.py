"""One-off: make test_interact.py pick a door that actually opens on the host."""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8-sig")
start = s.index('    doors = [d for d in both("Door")')
end = s.index('    count = lambda inst, cont, inv_cls:')
new = '''    doors = [d for d in both("Door") if ocl(0, d[1]) == "Closed" and ocl(1, d[2]) == "Closed"]
    check(bool(doors), "closed doors present in both worlds", f"{len(doors)} candidates")
    fn = {"Open": "AttemptOpen", "Closed": "AttemptClose"}
    used = None
    for path, h, c in doors:  # travel doors (cargo) do not open in place: take the first that does
        call("ue.call", {"ref": h + ".OCLComponent", "fn": "AttemptOpen", "args": {"bForce": True, "bIgnoreLockStatus": True}}, inst=0)
        if wait(lambda: ocl(0, h) in ("Open", "Opening"), 3):
            used = (path, h, c)
            break
    check(used is not None, "host opened a door", used and used[0].split(".")[-1])
    if used:
        path, h, c = used
        check(wait(lambda: ocl(1, c) in ("Open", "Opening"), 8), "client's door followed -> Open", ocl(1, c))
        time.sleep(3)
        call("ue.call", {"ref": c + ".OCLComponent", "fn": "AttemptClose", "args": {"bForce": True, "bIgnoreLockStatus": True}}, inst=1)
        check(wait(lambda: ocl(1, c) in ("Closed", "Closing"), 3), "client closed it", ocl(1, c))
        check(wait(lambda: ocl(0, h) in ("Closed", "Closing"), 8), "host's door followed -> Closed", ocl(0, h))
'''
s = s[:start] + new + s[end:]
p.write_text(s, encoding="utf-8", newline="\n")
print("patched", p)
