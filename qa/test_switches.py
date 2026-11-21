"""Switches shared, on a running joined pair in gameplay: in the Order's headquarters (0605_OAHQ, travelled to if needed)
a switch (Switch_1_Blueprint, its interaction state) used by the host through the game's own interaction calls (what E
calls: StartInteract, CompleteInteract; probe 139) takes the same state in the client's world, and one the client uses
in the host's.
Usage: python -I test_switches.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_items import check, safe, wait  # noqa: E402
import test_items  # noqa: E402
from tow2 import call  # noqa: E402

HQ = "/Game/Maps/TravelDestinationData/0601_Cloister/0605_OAHQHardLoad.0605_OAHQHardLoad"


def world(i):
    return str(call("eval", {"expr": "world"}, inst=i, timeout_s=10)["value"]).split(".")[-1]


def to_hq():
    tm = call("ue.find", {"class": "TravelManager"}, inst=0)[0].split()[0]
    d = call("ue.find", {"path": HQ}, inst=0)
    if not d:
        lib = call("ue.find", {"path": "/Script/Engine.Default__KismetSystemLibrary"}, inst=0)[0].split()[0]
        safe(lambda: call("ue.call", {"ref": lib, "fn": "LoadAsset_Blocking", "args": {"Asset": HQ}}, inst=0))
        d = call("ue.find", {"path": HQ}, inst=0)
    call("ue.call", {"ref": tm, "fn": "BeginTravelToBP", "args": {"Destination": d[0].split()[0], "bPassGametime": False,
                                                                  "bTriggerAutosave": False, "bForceBlocking": False}}, inst=0)
    wait(lambda: "0605" in world(0) and "0605" in world(1), 120)
    time.sleep(15)  # both settled, the scan has been round


def state(i, comp):
    return safe(lambda: call("ue.call", {"ref": comp, "fn": "GetInteractState"}, inst=i)["ReturnValue"])


def use(i, comp, owner):
    d = {"Interactee": owner, "Initiator": "pawn", "Interaction": 15, "InteractionTypes": [15]}  # Generic, as GetInteractions says
    call("ue.call", {"ref": comp, "fn": "StartInteract", "args": {"Description": d}}, inst=i)
    time.sleep(0.5)
    call("ue.call", {"ref": comp, "fn": "CompleteInteract", "args": {"Description": d}}, inst=i)


def main():
    if "0605" not in world(0):
        to_hq()
    if not check("0605" in world(0) and "0605" in world(1), "both in the Order's headquarters", world(0)):
        return 1
    rows = [r for r in call("ue.find", {"class": "/Script/Arkansas.InteractableComponent", "limit": 5000}, inst=0)
            if "Switch_1_Blueprint_C_UAID_" in r and "Default__" not in r]
    if not check(len(rows) >= 2, "switches there", str(len(rows))):
        return 1
    for n, (r, who) in enumerate(((rows[0], 0), (rows[1], 1))):
        _, _, path = r.split(" ", 2)
        comp = [call("ue.find", {"path": path}, inst=i)[0].split()[0] for i in (0, 1)]
        owner = call("ue.find", {"path": path.rsplit(".", 1)[0]}, inst=who)[0].split()[0]
        before = [state(i, comp[i]) for i in (0, 1)]
        use(who, comp[who], owner)
        other = 1 - who
        changed = wait(lambda: state(who, comp[who]) != before[who], 3)
        check(changed, f"the {'host' if who == 0 else 'client'} switched a switch", f"{before[who]} -> {state(who, comp[who])}")
        check(changed and wait(lambda: state(other, comp[other]) == state(who, comp[who]), 6),
              f"the same in the {'client' if who == 0 else 'host'}'s world", f"{before[other]} -> {state(other, comp[other])}")
    sent = [call("coop.status", {}, inst=i).get("statesSent") for i in (0, 1)]
    time.sleep(10)
    st = [call("coop.status", {}, inst=i) for i in (0, 1)]
    check([x.get("statesSent") for x in st] == sent, "then nothing goes back and forth (10 s)", f"sent {sent} -> {[x.get('statesSent') for x in st]}")
    print(f"info: switches watched {st[0].get('statesWatched')} / {st[1].get('statesWatched')}, sent {st[0].get('statesSent')} / "
          f"{st[1].get('statesSent')}, set {st[0].get('statesApplied')} / {st[1].get('statesApplied')}")
    return 1 if test_items.FAILS else 0


if __name__ == "__main__":
    r = main()
    print(f"RESULT: {'PASS' if not r else 'FAIL'} ({test_items.FAILS} failure(s))")
    sys.exit(r)
