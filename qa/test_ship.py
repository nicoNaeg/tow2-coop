"""Ship travel, on a running pair in gameplay: the host boards the player ship (0502_Playership_POIData) and flies it
to another landing (a ShipDestinationData: the ship's location set, then the game's travel, as the ship's console
does); the client follows both legs by itself: the same map, the ship at the same place in both games, both see each
other. Usage: python -I test_ship.py [landing]   (default: back to SHIP_0601_CL_LP1 when elsewhere, else SHIP_0301_Landing_Pad)"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

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


def tm(i):
    return call("ue.find", {"class": "TravelManager"}, inst=i)[0].split()[0]


def world(i):
    return str(call("eval", {"expr": "world"}, inst=i)["value"]).split(".")[-1]


def ship_at(i):
    return str(call("ue.call", {"ref": tm(i), "fn": "GetCurrentShipLocationBP"}, inst=i)["ReturnValue"]).split(".")[-1]


def dest(cls, name):
    rows = [r for r in call("ue.find", {"class": cls, "limit": 600}, inst=0) if r.split(".")[-1] == name]
    return rows[0].split()[0] if rows else None


def leg(ref, label, set_ship):
    s0 = call("coop.status", {}, inst=1).get("travelsFollowed", 0)
    start = [world(0), world(1)]
    t = tm(0)
    if set_ship:
        call("ue.call", {"ref": t, "fn": "SetCurrentShipLocationBP", "args": {"Destination": ref}}, inst=0)
    call("ue.call", {"ref": t, "fn": "BeginTravelToBP", "args": {"Destination": ref, "bPassGametime": True, "bTriggerAutosave": False,
                                                                 "bForceBlocking": False}}, inst=0)
    t0, seen = time.time(), {}
    while time.time() - t0 < 240 and len(seen) < 2:
        for i in (0, 1):
            w = safe(lambda i=i: world(i), start[i])
            if i not in seen and w != start[i] and "Transition" not in w and "Loading" not in w:
                seen[i] = (time.time() - t0, w)
        time.sleep(1)
    print(f"info: {label}: host in {seen.get(0)}, client in {seen.get(1)} (s after the travel started)", flush=True)
    ok = len(seen) == 2 and seen[0][1] == seen[1][1]
    check(ok, f"{label}: both arrived in the same map", str(seen))
    check(call("coop.status", {}, inst=1).get("travelsFollowed", 0) > s0, f"{label}: the client followed the host's travel by itself",
          str(call("coop.status", {}, inst=1).get("lastTravel")))
    t1 = time.time()
    both = False
    while time.time() - t1 < 60 and not both:
        both = all(call("coop.status", {}, inst=i).get("puppet", "none") != "none" for i in (0, 1))
        time.sleep(2)
    check(both, f"{label}: both see each other", f"after {time.time() - t1:.0f} s")
    return ok


def main():
    ship = dest("/Script/Arkansas.PointOfInterestData", "0502_Playership_POIData")
    if not check(bool(ship), "the player ship's destination is loaded"):
        return 1
    here = ship_at(0)
    target = sys.argv[1] if len(sys.argv) > 1 else ("SHIP_0601_CL_LP1" if here != "SHIP_0601_CL_LP1" else "SHIP_0301_Landing_Pad")
    print(f"info: the ship is at {here} in both? {here} / {ship_at(1)}; flying to {target}", flush=True)
    if "PlayerShip" not in world(0) and not leg(ship, "boarding the ship", False):
        return 1
    land = dest("/Script/Arkansas.ShipDestinationData", target)
    if not check(bool(land), f"the landing {target} is loaded"):
        return 1
    leg(land, f"flying to {target}", True)
    check(ship_at(0) == ship_at(1) == target, "the ship is at the same place in both games", f"{ship_at(0)} / {ship_at(1)}")
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
