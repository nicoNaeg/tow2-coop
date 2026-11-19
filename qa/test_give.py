"""Giving items through the other player's stand-in, on a joined pair (copy 0 host, copy 1 client, both in gameplay).
For each direction: the giver stands in front of the other player's stand-in and presses E (a key posted to its game
window): the game's own transfer screen opens on a hidden chest at the stand-in. What lands in that chest must leave it
and reach the other player's inventory. Then the receiver refuses gifts (test switch): the giver gets the item back
after 20 s. The transfer itself is done the way the screen does it (items leave the giver, appear in the chest).
Usage: python -I test_give.py <host pid> <client pid>"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

SHELLS = "Ammo_Shotgun_C"
OUT = Path(__file__).resolve().parents[2] / "the-outer-worlds-2" / "evidence" / "give-last"


def safe(f, d=None):
    try:
        return f()
    except Exception:  # noqa: BLE001
        return d


def wait(pred, timeout, step=0.25):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(step)
    return bool(safe(pred, False))


def st(i):
    return call("coop.status", {}, inst=i)


def shell_class(i):
    rows = call("ue.find", {"name": SHELLS, "limit": 20}, inst=i)
    return next(r.split()[0] for r in rows if r.split()[2].endswith("." + SHELLS) and r.split()[1].endswith("Class"))


def count(i, actor="pawn"):
    inv = "pawn.InventoryComponent" if actor == "pawn" else actor + ".InventoryComponent"
    return call("ue.call", {"ref": inv, "fn": "GetItemCountOnActor", "args": {"Actor": actor, "ItemType": shell_class(i)}}, inst=i)["ReturnValue"]


def face_standin(i):
    """Stand 1.5 m in front of the stand-in, looking at its chest."""
    s = st(i)["puppet"]
    p = call("ue.call", {"ref": s, "fn": "K2_GetActorLocation"}, inst=i)["ReturnValue"]
    r = call("ue.call", {"ref": s, "fn": "K2_GetActorRotation"}, inst=i)["ReturnValue"]
    a = math.radians(r["Yaw"])
    me = {"X": p["X"] + 150 * math.cos(a), "Y": p["Y"] + 150 * math.sin(a), "Z": p["Z"] + 10}
    call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": me, "bSweep": False, "bTeleport": True}}, inst=i)
    time.sleep(0.4)
    me = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=i)["value"]
    yaw = math.degrees(math.atan2(p["Y"] - me["Y"], p["X"] - me["X"]))
    pitch = math.degrees(math.atan2(p["Z"] + 20 - (me["Z"] + 60), math.dist((p["X"], p["Y"]), (me["X"], me["Y"]))))
    call("ue.call", {"ref": "pc", "fn": "SetControlRotation", "args": {"NewRotation": {"Pitch": pitch, "Yaw": yaw, "Roll": 0}}}, inst=i)
    return s


def transfer(i, box, n):
    """What the screen does when the player moves n shells into the chest."""
    k = shell_class(i)
    call("ue.call", {"ref": "pawn.InventoryComponent", "fn": "RemoveItemFromActor", "args": {"Actor": "pawn", "ItemType": k, "Count": n}}, inst=i)
    call("ue.call", {"ref": box + ".InventoryComponent", "fn": "GiveItemToActor", "args": {"Actor": box, "ItemType": k, "Count": n, "bLootable": True}}, inst=i)


def class_path(i, name):
    for r in call("ue.find", {"name": name, "limit": 10}, inst=i):
        a, k, p = r.split(" ", 2)
        if k.endswith("Class") and p.endswith("." + name):
            return a, p
    return None, None


def instances(i, cls, owner_path):  # live objects of that class inside an actor (an inventory's items)
    return [r.split()[0] for r in call("ue.find", {"class": cls, "limit": 5000}, inst=i) if r.split(" ", 2)[2].startswith(owner_path + ".")]


def modded_gift(game, kind, item_name, mod_name):
    """The host puts a modded item in the chest (held while the test mods it, as the screen moves the player's own
    modded item in); the client's copy must carry the same mod. Returns the number of failures."""
    k, _ = class_path(0, item_name)
    _, mpath = class_path(0, mod_name)
    if not k or not mpath:
        print(f"FAIL {kind}: {item_name} or {mod_name} not loaded in the host's game", flush=True)
        return 1
    fails = 0
    client_pawn = call("eval", {"expr": "pawn"}, inst=1)["value"].split()[-1]
    before = set(instances(1, kind, client_pawn))
    call("coop.set", {"giveHold": True}, inst=0)
    try:
        face_standin(0)
        call("coop.give", {}, inst=0)
        if not wait(lambda: st(0)["transferOpen"], 3):
            print(f"FAIL {kind}: the transfer screen did not open", flush=True)
            return 1
        box = st(0)["giveBox"]
        box_path = call("eval", {"expr": box}, inst=0)["value"].split()[-1]
        call("ue.call", {"ref": box + ".InventoryComponent", "fn": "GiveItemToActor", "args": {"Actor": box, "ItemType": k, "Count": 1, "bLootable": True}}, inst=0)
        item = next(iter(instances(0, kind, box_path)), None)
        r = call("coop.mods", {"item": item, "add": mpath}, inst=0) if item else {}
        ok = mpath in r.get("mods", [])
        print(f"{'PASS' if ok else 'FAIL'} {kind}: the chest holds a {item_name} with {mod_name.split('_C')[0]} {r.get('mods')}", flush=True)
        fails += not ok
    finally:
        call("coop.set", {"giveHold": False}, inst=0)
    got = wait(lambda: len(set(instances(1, kind, client_pawn)) - before) >= 1, 10)
    new = list(set(instances(1, kind, client_pawn)) - before)
    mods = call("coop.mods", {"item": new[0]}, inst=1).get("mods", []) if new else []
    ok = got and mpath in mods
    print(f"{'PASS' if ok else 'FAIL'} {kind}: the client's copy carries the mod {mods}; client: {st(1)['lastGift']!r}", flush=True)
    fails += not ok
    game.key("ESCAPE")
    wait(lambda: not st(0)["transferOpen"], 4)
    return fails


def main(pids):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    OUT.mkdir(parents=True, exist_ok=True)
    games = [Game(int(p)) for p in pids]
    for i in (0, 1):
        call("coop.set", {"gifts": True}, inst=i)
        s = st(i)
        check(s.get("giveReady") is True, f"copy {i}: the transfer screen and the E key are ready", f"giveReady {s.get('giveReady')}, stand-in {s['puppet']}")
    for giver, taker in ((0, 1), (1, 0)):
        who = "host" if giver == 0 else "client"
        call("ue.call", {"ref": "pawn.InventoryComponent", "fn": "GiveItemToActor", "args": {"Actor": "pawn", "ItemType": shell_class(giver), "Count": 7}}, inst=giver)
        g0, t0 = count(giver), count(taker)
        s0, r0 = st(giver)["giftsSent"], st(taker)["giftsReceived"]
        face_standin(giver)
        games[giver].key("E")
        opened = wait(lambda: st(giver)["transferOpen"], 3)
        check(opened, f"{who}: E in front of the other player's stand-in opens the transfer screen", st(giver).get("lastGift"))
        time.sleep(1.0)
        print(games[giver].shot(str(OUT / f"transfer-{who}.png")), flush=True)
        if not opened:
            continue
        box = st(giver)["giveBox"]
        check(bool(box), f"{who}: a hidden chest holds the transfer", box)
        base = count(giver, box)
        transfer(giver, box, 3)
        left = wait(lambda: count(giver, box) == base, 3)
        check(left, f"{who}: the given shells left the chest", f"chest {safe(lambda: count(giver, box))} (before {base})")
        got = wait(lambda: count(taker) == t0 + 3, 8)
        check(got, f"{who}: the other player got 3 shells", f"{t0} -> {count(taker)}; giver {g0} -> {count(giver)}")
        a, b = st(giver), st(taker)
        check(a["giftsSent"] == s0 + 1 and b["giftsReceived"] == r0 + 1 and wait(lambda: st(giver)["giftsPending"] == 0, 3),
              f"{who}: one gift sent, received and confirmed", f"{a['lastGift']!r} / {b['lastGift']!r}")
        games[giver].key("ESCAPE")
        check(wait(lambda: not st(giver)["transferOpen"], 4), f"{who}: Escape closed the transfer screen")
        check(wait(lambda: not call("ue.find", {"name": box.split()[0], "limit": 1}, inst=giver) or not st(giver)["giveBox"], 3), f"{who}: the hidden chest is gone")
        time.sleep(1)
    # A weapon and an armor with a mod each, host to client: the client's copies carry the same mods.
    for kind, item_name, mod_name in (("Weapon", "Weapon_PI_Standard_Unique_ZanesPistol_C", "WeaponMod_Barrel_Silencer_PI_Standard_C"),
                                      ("ArmorItem", "Armor_ED_Zane_C", "ArmorMod_Armoring_DefensivePlates_C")):
        fails += modded_gift(games[0], kind, item_name, mod_name)
    # The receiver takes no gift: the giver gets it back.
    call("coop.set", {"gifts": False}, inst=0)
    g0, t0, back0 = count(1), count(0), st(1)["giftsReturned"]
    face_standin(1)
    call("coop.give", {}, inst=1)
    if wait(lambda: st(1)["transferOpen"], 3):
        transfer(1, st(1)["giveBox"], 2)
        check(wait(lambda: count(1) == g0 - 2, 3), "client gave 2 shells to a host that takes none", f"{g0} -> {count(1)}")
        back = wait(lambda: st(1)["giftsReturned"] > back0, 30, step=1)
        check(back and count(1) == g0 and count(0) == t0, "no answer: the shells came back to the client after 20 s",
              f"client {count(1)} (was {g0}), host {count(0)} (was {t0}); {st(1)['lastGift']!r}")
        games[1].key("ESCAPE")
        wait(lambda: not st(1)["transferOpen"], 4)
    else:
        check(False, "client: transfer screen for the no-answer case", st(1).get("lastGift"))
    call("coop.set", {"gifts": True}, inst=0)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:3]))
