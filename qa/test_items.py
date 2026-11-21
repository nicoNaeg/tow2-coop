"""Quest items to both players, loose items gone in both worlds, on a running joined pair in gameplay (0601_CL_P, the
test save):
- after the join the client holds the host's quest items (the campaign's keys and passes; its own character came
  without them);
- a quest item the host gets is given to the client, and one the client gets to the host;
- one both games get at once (as an NPC's gift in a shared conversation) is not given twice;
- a loose item the host picks up (E, real input) is gone in the client's world, and one the client picks up in the
  host's world;
- the host's quicksave keeps them gone: after its quickload (both games load it) neither item is back in either world.
Usage: python -I test_items.py"""
import math
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from test_combat import loc, teleport, uc  # noqa: E402
from tow2 import call  # noqa: E402

QK = "/Script/Arkansas.QuestItem"
CANDIDATES = [  # quest items of later planets (type dump): none of them in the test save's inventory
    "/Game/Blueprints/Item/QuestItem/02_ParadiseIsland/0212_Seed/0212_QI_EnragingAntidote.0212_QI_EnragingAntidote_C",
    "/Game/Blueprints/Item/QuestItem/02_ParadiseIsland/0220_Forward/0220_Scrabbles_Item.0220_Scrabbles_Item_C",
    "/Game/Blueprints/Item/QuestItem/08_Praetor/0802_RiftLab/0802_QI_ExperimentKey.0802_QI_ExperimentKey_C",
    "/Game/Blueprints/Item/QuestItem/02_ParadiseIsland/0203_Vox/0203_QI_RootKit.0203_QI_RootKit_C",
    "/Game/Blueprints/Item/QuestItem/02_ParadiseIsland/0202_WindTower/0202_DogTag.0202_DogTag_C",
    "/Game/Blueprints/Item/QuestItem/03_GoldenRidge/0312_MantisaurNest/0312_MantisaurPupa.0312_MantisaurPupa_C",
    "/Game/Blueprints/Item/QuestItem/02_ParadiseIsland/0203_Vox/0203_QI_WestportOrders.0203_QI_WestportOrders_C",
    "/Game/Blueprints/Item/QuestItem/08_Praetor/0810_ChemicalFactory/0810_QI_RuthsChemical.0810_QI_RuthsChemical_C",
]
FAILS = 0


def check(ok, what, detail=""):
    global FAILS
    print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
    FAILS += not ok
    return ok


def safe(f, d=None):
    try:
        return f()
    except Exception:  # noqa: BLE001
        return d


def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        if safe(cond, False):
            return True
        time.sleep(0.5)
    return bool(safe(cond, False))


def pid(i):
    want = rf"C:\coop-anything\local\gamepool\instances\tow2\{i}\*"
    cmd = "(Get-Process -Name TheOuterWorlds2-Win64-Shipping | Where-Object { $_.Path -like '%s' }).Id" % want
    return int(subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.split()[0])


def quest_items(i):
    """The class names of the player's quest items."""
    out = set()
    for r in call("ue.find", {"class": QK, "limit": 2000}, inst=i):
        _, k, path = r.split(" ", 2)
        if "Default__" not in r and "IndianaPlayerCharacter" in path and ".Inventory." in path:
            out.add(k)
    return out


def klass(i, path):
    r = call("ue.find", {"path": path}, inst=i)
    if not r:
        lib = call("ue.find", {"path": "/Script/Engine.Default__KismetSystemLibrary"}, inst=i)[0].split()[0]
        safe(lambda: call("ue.call", {"ref": lib, "fn": "LoadClassAsset_Blocking", "args": {"AssetClass": path}}, inst=i))
        r = call("ue.find", {"path": path}, inst=i)
    return r[0].split()[0] if r else None


def count(i, path):
    k = klass(i, path)
    return call("ue.call", {"ref": "pawn.InventoryComponent", "fn": "GetItemCountOnActor", "args": {"Actor": "pawn", "ItemType": k}}, inst=i)["ReturnValue"]


def give(i, path):
    """As a pickup would: the item into the player's inventory (the game's own call)."""
    uc(i, "pawn.InventoryComponent", "GiveItemToPlayer", {"ItemType": klass(i, path), "Count": 1, "bShowNotification": True, "bPlayItemPickupAudio": False})


def there(i, path):
    """A level-placed actor by path, alive in that world (a taken one can still be found, no longer live)."""
    r = call("ue.find", {"path": path}, inst=i)
    return bool(r) and safe(lambda: call("eval", {"expr": r[0].split()[0] + ".bHidden"}, inst=i)["value"] is False, False)


def pick_up(i, skip):
    """The player picks up the nearest reachable level-placed loose item with E (real input): (class, path) or None."""
    game = Game(pid(i))
    me = loc(i, "pawn")
    cands = []
    for r in call("ue.find", {"class": "/Script/Arkansas.Pickup", "limit": 3000}, inst=i):
        ref, k, path = r.split(" ", 2)
        if "Default__" in r or "_UAID_" not in path or path in skip or not there(1 - i, path):
            continue
        p = safe(lambda ref=ref: loc(i, ref))
        if p:
            cands.append((math.dist((p["X"], p["Y"]), (me["X"], me["Y"])), ref, k, path, p))
    cands.sort()
    for _, ref, k, path, p in cands[:10]:
        for ang in (0, 90, 180, 270):
            a = math.radians(ang)
            spot = {"X": p["X"] + 110 * math.cos(a), "Y": p["Y"] + 110 * math.sin(a), "Z": p["Z"] + 100}
            teleport(i, spot)
            yaw = math.degrees(math.atan2(p["Y"] - spot["Y"], p["X"] - spot["X"]))
            uc(i, "pc", "SetControlRotation", {"NewRotation": {"Pitch": -35, "Yaw": yaw, "Roll": 0}})
            time.sleep(0.8)
            game.key("E")
            time.sleep(1.2)
            if not there(i, path):
                return k, path
        skip.add(path)  # out of reach (behind a grate, in a case): the next one
    return None


def sgm(i):
    return call("ue.find", {"class": "SaveGameManager", "limit": 2}, inst=i)[0].split()[0]


def world(i):
    return str(call("eval", {"expr": "world"}, inst=i, timeout_s=10)["value"]).split(".")[-1]


def in_play(i, w):
    return world(i) == w and call("coop.status", {}, inst=i).get("puppet") not in (None, "none")


def main():
    host = quest_items(0)
    # the client's game may drop some later by itself, catching up on the story (probe 145: two leftovers of later quests)
    dropped = lambda: set(call("coop.status", {}, inst=1).get("questItemsDropped") or [])
    check(wait(lambda: not (host - quest_items(1) - dropped()), 20), "after the join the client holds the host's quest items, but those its game dropped",
          f"host {len(host)}, client {len(quest_items(1))}, missing {sorted(host - quest_items(1))[:5]}, its game dropped {sorted(dropped())}")

    free = [c for c in CANDIDATES if safe(lambda c=c: count(0, c), 1) == 0 and safe(lambda c=c: count(1, c), 1) == 0]
    if not check(len(free) >= 3, "three quest items neither player has", str(len(free))):
        return 1
    a, b, both = free[:3]
    give(0, a)
    check(wait(lambda: count(1, a) == 1, 8), "a quest item the host got is given to the client", f"{a.split('.')[-1]}: client {count(1, a)}, host {count(0, a)}")
    give(1, b)
    check(wait(lambda: count(0, b) == 1, 8), "a quest item the client got is given to the host", f"{b.split('.')[-1]}: host {count(0, b)}, client {count(1, b)}")
    give(0, both), give(1, both)
    time.sleep(6)
    check(count(0, both) == 1 and count(1, both) == 1, "one both games got at once is not given twice", f"host {count(0, both)}, client {count(1, both)}")
    check(count(0, a) == 1 and count(1, b) == 1, "nothing given back to the first one", f"host {count(0, a)}, client {count(1, b)}")

    skip = set()
    start = [loc(i, "pawn") for i in (0, 1)]
    got = pick_up(0, skip)
    if check(got is not None, "the host picked up a loose item with E", str(got)):
        check(wait(lambda: not there(1, got[1]), 5), "gone in the client's world too", got[0])
        skip.add(got[1])
    teleport(1, loc(0, "pawn"))
    time.sleep(2)
    got2 = pick_up(1, skip)
    if check(got2 is not None, "the client picked up a loose item with E", str(got2)):
        check(wait(lambda: not there(0, got2[1]), 5), "gone in the host's world too", got2[0])
    st = [call("coop.status", {}, inst=i) for i in (0, 1)]
    print(f"info: loose items told host {st[0].get('pickupsSent')}, client {st[1].get('pickupsSent')}; "
          f"removed host {st[0].get('pickupsApplied')}, client {st[1].get('pickupsApplied')}")

    if got and got2:  # the host's save keeps them gone (the client's one was destroyed by the mod in the host's world)
        w = world(0)
        r = safe(lambda: uc(0, sgm(0), "Quicksave")["ReturnValue"], "?")
        time.sleep(3)
        check("Success" in str(r) or "Begun" in str(r), "the host quicksaved", str(r))
        safe(lambda: uc(0, sgm(0), "Quickload"))
        time.sleep(15)
        back = wait(lambda: in_play(0, w) and in_play(1, w), 120)
        check(back, "both games loaded the host's quicksave", "")
        time.sleep(5)
        left = [(i, k) for i in (0, 1) for k, p in (got, got2) if there(i, p)]
        st2 = [call("coop.status", {}, inst=i).get("pickupsApplied") for i in (0, 1)]
        # removed again after the load would mean only the mod's list keeps them gone, not the save (probe 134: 1 -> 1)
        check(not left and st2 == [st[0].get("pickupsApplied"), st[1].get("pickupsApplied")],
              "after it neither loose item is back in either world, by the save itself", f"{left}, removed {st2}")
    for i in (0, 1):
        safe(lambda i=i: teleport(i, start[i]))
    # the quickload made new pawns: god mode and the rest of the suite's setup again (run 199: the host died in the next fight)
    subprocess.run([sys.executable, "-I", str(Path(__file__).resolve().parent / "god.py")], capture_output=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    r = main()
    print(f"RESULT: {'PASS' if not r else 'FAIL'} ({FAILS} failure(s))")
    sys.exit(r)
