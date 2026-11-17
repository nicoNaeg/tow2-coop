"""Outfit sync on a running pair in gameplay (copy 0 host, copy 1 client, each sees the other's stand-in): the client
takes its helmet off, puts another armor set on, then restores both; within a few seconds the stand-in in the host's
world shows the same armor set and helmet (a visual override on the stand-in, read back from the host's world).
Usage: python -I test_look.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

SLOTS = (("ArmorSet", "GetEquippedArmor"), ("Helmet", "GetEquippedHelmet"))


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def cls_of(ref):
    """Class name of an object reference ("0x... ClassName /Path"), None for no object."""
    return ref.split()[1] if str(ref).startswith("0x") else None


def worn(inst, slot):  # the client's own item in that slot
    return cls_of(call("ue.call", {"ref": "pawn.EquipmentComponent", "fn": dict(SLOTS)[slot]}, inst=inst)["ReturnValue"])


def shown(slot):  # what the stand-in in the host's world wears in that slot (None: no override)
    p = call("coop.status", {}, inst=0)["puppet"]
    r = call("ue.call", {"ref": f"{p.split()[0]}.EquipmentComponent", "fn": "GetArmorVisualOverride", "args": {"Slot": slot}}, inst=0)
    return cls_of(r["ReturnValue"])


def equip(slot, cls_name, inst=1):
    rows = call("ue.find", {"name": cls_name, "limit": 20}, inst=inst)
    c = next(r.split()[0] for r in rows if r.split()[2].endswith("." + cls_name) and r.split()[1].endswith("Class"))
    call("ue.call", {"ref": "pawn.EquipmentComponent", "fn": "EquipArmor_BP", "args": {"ArmorSlot": slot, "ArmorItem": c}}, inst=inst)


def helmet_flag():  # the stand-in's AppearanceComponent.bHelmetVisible in the host's world (False hides its own cap)
    p = call("coop.status", {}, inst=0)["puppet"]
    return call("eval", {"expr": f"{p.split()[0]}.AppearanceComponent.bHelmetVisible"}, inst=0)["value"]


def wait_flag(want, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(helmet_flag, "?") is want:
            return True
        time.sleep(0.5)
    return False


def wait_for(slot, want, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(lambda: shown(slot), "?") == want:
            return True
        time.sleep(0.5)
    return False


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    s0 = call("coop.status", {}, inst=0)
    check(s0["puppet"] != "none", "host sees the client's stand-in", s0["puppet"])
    if s0["puppet"] == "none":
        print(f"RESULT: FAIL ({fails} failure(s))")
        return fails
    # Hair (two-PC test: the stand-in was bald without a helmet): an NPC body applies Customization only when enabled.
    p0 = s0["puppet"].split()[0]
    enabled = call("ue.get", {"path": f"{p0}.AppearanceComponent.bEnableCustomization"}, inst=0)
    asset = lambda r: r.split()[2] if str(r).startswith("0x") else None  # noqa: E731  the hair asset's path
    hair = asset(call("ue.get", {"path": f"{p0}.AppearanceComponent.Customization.Hair"}, inst=0))
    mine = asset(call("ue.get", {"path": "pawn.AppearanceComponent.Customization.Hair"}, inst=0))
    check(enabled is True and hair == mine, "stand-in has the player's hair (customization enabled)", f"enabled {enabled}, hair {hair} / {mine}")
    # Each player has their own look (end goal 2): the client changes its own hair; the host's stand-in of the client
    # follows it (look message), the client's stand-in of the host keeps the host's hair.
    p1 = call("coop.status", {}, inst=1)["puppet"].split()[0]
    theirs = lambda: asset(call("ue.get", {"path": f"{p0}.AppearanceComponent.Customization.Hair"}, inst=0))  # noqa: E731
    host_hair = asset(call("ue.get", {"path": "pawn.AppearanceComponent.Customization.Hair"}, inst=0))
    other = next((r.split()[0], r.split()[2]) for r in call("ue.find", {"class": "HairAppearances", "limit": 80}, inst=1) if r.split()[2] != host_hair)
    old = asset(call("ue.get", {"path": "pawn.AppearanceComponent.Customization.Hair"}, inst=1))
    call("ue.call", {"ref": "pawn.AppearanceComponent", "fn": "SetHair", "args": {"NewHair": other[0]}}, inst=1)
    call("ue.call", {"ref": "pawn.AppearanceComponent", "fn": "RefreshFullAppearance", "args": {}}, inst=1)
    t0 = time.time()
    while time.time() - t0 < 15 and safe(theirs) != other[1]:
        time.sleep(0.5)
    check(safe(theirs) == other[1], "the host's stand-in shows the client's own new hair", f"{safe(theirs)} after {time.time() - t0:.0f} s")
    kept = asset(call("ue.get", {"path": f"{p1}.AppearanceComponent.Customization.Hair"}, inst=1))
    check(kept == host_hair, "the client's stand-in of the host keeps the host's hair", f"{kept}")
    back = next(r.split()[0] for r in call("ue.find", {"class": "HairAppearances", "limit": 80}, inst=1) if r.split()[2] == old)
    call("ue.call", {"ref": "pawn.AppearanceComponent", "fn": "SetHair", "args": {"NewHair": back}}, inst=1)
    call("ue.call", {"ref": "pawn.AppearanceComponent", "fn": "RefreshFullAppearance", "args": {}}, inst=1)
    a0, h0 = worn(1, "ArmorSet"), worn(1, "Helmet")
    print("client wears", a0, "/", h0, flush=True)
    check(wait_for("ArmorSet", a0) and wait_for("Helmet", h0), "stand-in wears the client's armor set and helmet",
          f"{safe(lambda: shown('ArmorSet'))} / {safe(lambda: shown('Helmet'))}")
    if h0:
        call("ue.call", {"ref": "pawn.EquipmentComponent", "fn": "RemoveArmor_BP", "args": {"ArmorSlot": "Helmet"}}, inst=1)
        check(worn(1, "Helmet") is None, "client took its helmet off")
        # The stand-in body has its own cap (probe 53): without the override it must be hidden, not shown.
        check(wait_for("Helmet", None) and wait_flag(False), "stand-in's helmet is gone and its own cap hidden",
              f"{safe(lambda: shown('Helmet'))}, bHelmetVisible {safe(helmet_flag)}")
        equip("Helmet", h0)
        check(wait_for("Helmet", h0) and wait_flag(True), "stand-in's helmet is back and shown",
              f"{safe(lambda: shown('Helmet'))}, bHelmetVisible {safe(helmet_flag)}")
    # Another armor set: any loaded one (items in inventories, vendors, NPCs).
    other = None
    for row in call("ue.find", {"class": "ArmorSet", "limit": 200}, inst=1):
        if cls_of(row) != a0 and cls_of(row).endswith("_C"):  # a Blueprint armor set (a /Game/ class)
            other = cls_of(row)
            break
    check(other is not None, "found another armor set", str(other))
    if other:
        equip("ArmorSet", other)
        check(worn(1, "ArmorSet") == other, "client put the other armor set on", str(worn(1, "ArmorSet")))
        check(wait_for("ArmorSet", other), "stand-in wears the new armor set", str(safe(lambda: shown("ArmorSet"))))
        if a0:
            equip("ArmorSet", a0)
            check(wait_for("ArmorSet", a0), "stand-in back in the first armor set", str(safe(lambda: shown("ArmorSet"))))
    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    print(f"host: lookApplied {s0.get('lookApplied')!r}, error {s0.get('error')!r}; client error {s1.get('error')!r}")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
