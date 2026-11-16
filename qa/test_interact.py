"""Interaction sync on a running pair in gameplay (copy 0 host, copy 1 client, same campaign state):
doors: the host opens a door (the game's own OpenOCL on the door's InteractableComponent), the client's copy opens;
then the client closes it and the host's copy closes. Containers: an item taken out of a container in one world
disappears from the same container in the other world. Usage: python -I test_interact.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def wait(pred, timeout=6.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return True
        time.sleep(0.25)
    return pred()


def both(cls):
    out = []
    for h in call("ue.find", {"class": cls, "limit": 500}, inst=0):
        a, _, path = h.split(" ", 2)
        b = call("ue.find", {"path": path}, inst=1)
        if b:
            out.append((path, a, b[0].split()[0]))
    return out


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    ocl = lambda inst, ref: call("ue.call", {"ref": ref + ".OCLComponent", "fn": "GetCurrentState"}, inst=inst)["ReturnValue"]
    doors = [d for d in both("Door") if ocl(0, d[1]) == "Closed" and ocl(1, d[2]) == "Closed"]
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
    count = lambda inst, cont, inv_cls: call("ue.call", {"ref": cont + ".InventoryComponent", "fn": "GetItemCountOnActor", "args": {"Actor": cont, "ItemType": inv_cls}}, inst=inst)["ReturnValue"]
    picked = None
    for path, h, c in both("Container"):
        items = call("ue.call", {"ref": h, "fn": "GetItemsInContainer"}, inst=0).get("OutItems") or []
        for it in items:
            ref = it.split()[0] if isinstance(it, str) and it.startswith("0x") else None
            if not ref:
                continue
            cls_path = call("eval", {"expr": ref + ".Class"}, inst=0)["value"].split()[2] if False else None
            klass = call("ue.find", {"path": call("eval", {"expr": ref}, inst=0)["value"].split()[2]}, inst=0)
            item_cls = it.split()[1]
            cls = [x for x in call("ue.find", {"name": item_cls, "limit": 5, "cdo": True}, inst=0) if x.split()[1].endswith("Class")]
            if not cls:
                continue
            cls_path = cls[0].split()[2]
            cc = [x for x in call("ue.find", {"path": cls_path}, inst=1)]
            if cc and count(0, h, cls[0].split()[0]) > 0 and count(1, c, cc[0].split()[0]) > 0:
                picked = (path, h, c, cls[0].split()[0], cc[0].split()[0], item_cls)
                break
        if picked:
            break
    check(picked is not None, "a container with the same item in both worlds", picked and picked[5])
    if picked:
        path, h, c, hc, cc, name = picked
        n0, n1 = count(0, h, hc), count(1, c, cc)
        call("ue.call", {"ref": h + ".InventoryComponent", "fn": "RemoveItemFromActor", "args": {"Actor": h, "ItemType": hc, "Count": 1}}, inst=0)
        check(count(0, h, hc) == n0 - 1, f"host took one {name}", f"{n0} -> {count(0, h, hc)}")
        check(wait(lambda: count(1, c, cc) == n1 - 1), "the client's container lost it too", f"{n1} -> {count(1, c, cc)}")
    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    print("host:", {k: s0[k] for k in ("doorsSent", "doorsApplied", "itemsSent", "itemsApplied", "lastInteract")})
    print("client:", {k: s1[k] for k in ("doorsSent", "doorsApplied", "itemsSent", "itemsApplied", "lastInteract")})
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
