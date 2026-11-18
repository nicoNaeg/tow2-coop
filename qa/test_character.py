"""Each player keeps their own character (end goal 2) on a running pair in gameplay (copy 0 host, copy 1 client that
just joined for the first time, with no kept character): the client joined with nothing; it gains money, ammo and
two levels; the host reloads its campaign, which hands it over again; the client still has its own money, ammo and
level (its Player.dat went into the host's campaign), not the host's, and stands next to the host.
Usage: python -I test_character.py"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

HOST_SAVE = "B9F05C364F38D4A68B3AB39CCB0910F8"


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def cls(inst, name):
    rows = call("ue.find", {"name": name, "limit": 20}, inst=inst)
    return next((r.split()[0] for r in rows if r.split()[2].endswith("." + name) and r.split()[1].endswith("Class")), None)


def count(inst, name):
    c = cls(inst, name)
    if not c:
        return 0
    return call("ue.call", {"ref": "pawn.InventoryComponent", "fn": "GetItemCountOnActor", "args": {"Actor": "pawn", "ItemType": c}}, inst=inst)["ReturnValue"]


def stacks(inst):
    return len(call("ue.get", {"path": "pawn.InventoryComponent.Items"}, inst=inst) or [])


def level(inst):
    return call("ue.call", {"ref": "pawn.PlayerInfoComponent", "fn": "GetLevel"}, inst=inst)["ReturnValue"]


def reloaded(changes):
    s = call("coop.status", {}, inst=1)
    return s["mapChanges"] > changes and s["settled"] and s["puppet"] != "none" and level(1) > 0


def wait(pred, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(1)
    return False


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    ch = call("coop.status", {}, inst=1).get("character", {})
    print("client character:", ch)
    check(ch.get("source") in ("your own newest save", "kept") or "emptied" in str(ch.get("source")), "the client's character came from its own save", str(ch.get("source")))
    wait(lambda: not call("coop.status", {}, inst=1)["character"]["stripPending"], 30)
    time.sleep(3)
    h_bits, c_bits, c_stacks = count(0, "Bit_Cartridge_C"), count(1, "Bit_Cartridge_C"), stacks(1)
    h_rockets, c_rockets = count(0, "Ammo_Rocket_C"), count(1, "Ammo_Rocket_C")
    # other tests may have equipped armor on the client since (their items count): money and the host's ammo tell
    check(c_bits == 0 and c_rockets == 0 and h_rockets > 0, "the client joined with nothing", f"bits {c_bits}, rockets {c_rockets} (host {h_rockets}), item stacks {c_stacks}")
    check(h_bits > 0, "the host keeps its own inventory", f"bits {h_bits}")
    # The client gains its own things.
    for name, n in (("Bit_Cartridge_C", 123), ("Ammo_Bullets_Short_C", 50)):
        call("ue.call", {"ref": "pawn.InventoryComponent", "fn": "GiveItemToActor", "args": {"Actor": "pawn", "ItemType": cls(1, name), "Count": n}}, inst=1)
    lvl0 = level(1)
    call("ue.call", {"ref": "pawn.PlayerInfoComponent", "fn": "SetLevel", "args": {"NewLevel": lvl0 + 2}}, inst=1)
    mine = {"bits": count(1, "Bit_Cartridge_C"), "ammo": count(1, "Ammo_Bullets_Short_C"), "level": level(1)}
    host = {"bits": count(0, "Bit_Cartridge_C"), "level": level(0)}
    print("client now:", mine, "host:", host)
    check(mine == {"bits": 123, "ammo": 50, "level": lvl0 + 2}, "the client gained money, ammo and two levels")
    call("coop.charSave", {}, inst=1)
    check(wait(lambda: 0 <= call("coop.status", {}, inst=1)["character"]["lastSaved"] < 30, 40), "the client kept its character (private save)",
          str(safe(lambda: call("coop.status", {}, inst=1)["character"])))
    # A new hand-over: the host reloads its campaign; the client follows with its own character.
    st = call("coop.status", {}, inst=1)
    before, changes = st["resyncFetches"], st["mapChanges"]
    refused = call("coop.joinStatus", {}, inst=1).get("refused", 0)
    call("coop.refuseLoads", {"n": 3}, inst=1)  # the game refuses the client's next loads (a fall, combat): it tries again
    sgm = call("ue.find", {"class": "SaveGameManager"}, inst=0)[0].split()[0]
    safe(lambda: call("ue.call", {"ref": sgm, "fn": "LoadGame", "args": {"Filename": HOST_SAVE, "bPreValidateMetadata": True}, "timeoutMs": 240000}, inst=0, timeout_s=260))
    check(wait(lambda: call("coop.status", {}, inst=1)["resyncFetches"] > before, 180), "the host handed its reloaded campaign over")
    # the client loads in a new world (run 70: the old world still answered right after the fetch started)
    check(wait(lambda: reloaded(changes), 300), "the client reloaded and sees the host")
    js = call("coop.joinStatus", {}, inst=1)
    check(js.get("refused", 0) - refused == 3 and "Success" in js.get("state", ""), "the client loaded after the game refused 3 times", str(js))
    # one private save at the hand-over (runs 70-76: a second one, refused while the first was finishing, cost 6 s)
    ch = call("coop.status", {}, inst=1)["character"]
    check("refused" not in ch.get("note", "") and ch.get("lastSaved", -1) >= 0, "the client kept its character at the hand-over", str(ch))
    time.sleep(5)
    after = {"bits": count(1, "Bit_Cartridge_C"), "ammo": count(1, "Ammo_Bullets_Short_C"), "level": level(1)}
    print("client after the hand-over:", after, "character:", call("coop.status", {}, inst=1)["character"])
    check(after == mine, "the client still has its own money, ammo and level (not the host's)", f"{after} (host has {host})")
    hp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
    cp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"]
    d = math.dist([hp[k] for k in "XYZ"], [cp[k] for k in "XYZ"])
    check(d < 500, "the client stands next to the host", f"{d:.0f} units apart")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
