"""One world for both players, on a joined pair (copy 0 host, copy 1 client, both in gameplay):
A. Clock: the host's calendar jumps 3 hours (as waiting or sleeping does): the client's follows within seconds.
B. Menus: the host opens its inventory (the game pauses): the client's NPCs are not pulled back to the host's frozen
   ones, and the client's clock waits for the host's.
C. Fast travel: the host fast-travels across the map: the client travels with it and lands next to it.
Usage: python -I test_world.py <host pid> [ABC]"""
import math
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

FAST = ["/Game/Maps/POI_Data/06_Cloister/0601_ChapelSE_FastTravel.0601_ChapelSE_FastTravel",
        "/Game/Maps/POI_Data/06_Cloister/0601_LP1_FastTravel.0601_LP1_FastTravel"]


def safe(f, d=None):
    try:
        return f()
    except Exception:  # noqa: BLE001
        return d


def wait(pred, timeout, step=0.5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(step)
    return bool(safe(pred, False))


def st(i):
    return call("coop.status", {}, inst=i)


def cal(i):
    return call("ue.find", {"class": "GameCalendar", "limit": 2}, inst=i)[0].split()[0]


def hour(i):
    return call("ue.call", {"ref": cal(i), "fn": "K2_GetRawHour"}, inst=i)["ReturnValue"]


def gap():
    d = (hour(0) - hour(1)) % 24.0
    return min(d, 24.0 - d)


def pos(i):
    return call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=i)["value"]


def main(host_pid, which):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    if "A" in which:
        print(f"hours: host {hour(0):.2f}, client {hour(1):.2f}", flush=True)
        call("ue.call", {"ref": cal(0), "fn": "K2_AdvanceTime", "args": {"Days": 0, "Hours": 3, "Minutes": 0, "Seconds": 0}}, inst=0)
        t0 = time.time()
        ok = wait(lambda: gap() < 0.1, 15)
        check(ok, "the client's clock followed the host's 3-hour jump", f"after {time.time() - t0:.0f} s: host {hour(0):.2f}, client {hour(1):.2f}, jumps {st(1).get('clockJumps')}")
    if "B" in which:
        game = Game(int(host_pid))
        s0 = st(1)["npcSnapped"]
        game.key("I")
        time.sleep(2)
        paused = st(0).get("hostPaused")
        time.sleep(13)
        s1 = st(1)["npcSnapped"]
        held, g = st(1).get("clockHeld"), gap()
        game.key("ESCAPE")
        check(paused is True, "the host's inventory pauses its game", str(paused))
        check(s1 - s0 <= 2, "the client's NPCs were not pulled back while the host's game was paused", f"snaps {s0} -> {s1} in 15 s (before the fix: 45)")
        check(g < 0.05, "the client's clock did not run away from the host's paused one", f"gap {g:.3f} h after 15 s (unchecked it grows ~0.1 h), held {held}")
        check(wait(lambda: st(1).get("clockHeld") is False, 10), "the client's clock runs freely once the host's does", str(st(1).get("clockHeld")))
    if "C" in which:
        tm = call("ue.find", {"class": "TravelManager"}, inst=0)[0].split()[0]
        p0 = pos(0)
        dest = None
        for path in FAST:  # the one farther from the host
            d = safe(lambda: call("ue.find", {"path": path}, inst=0))
            if not d:
                lib = call("ue.find", {"path": "/Script/Engine.Default__KismetSystemLibrary"}, inst=0)[0].split()[0]
                safe(lambda: call("ue.call", {"ref": lib, "fn": "LoadAsset_Blocking", "args": {"Asset": path}}, inst=0))
                d = safe(lambda: call("ue.find", {"path": path}, inst=0))
            if d:
                dest = d[0].split()[0]
                break
        check(dest is not None, "a fast travel point in the map", FAST[0].split(".")[-1])
        if dest:
            f0 = st(1).get("travelsFollowed", 0)
            threading.Thread(target=lambda: safe(lambda: call("ue.call", {"ref": tm, "fn": "BeginTravelToBP", "args": {"Destination": dest, "bPassGametime": False,
                             "bTriggerAutosave": False, "bForceBlocking": False}, "timeoutMs": 120000}, inst=0, timeout_s=130)), daemon=True).start()
            moved = wait(lambda: math.dist((pos(0)["X"], pos(0)["Y"]), (p0["X"], p0["Y"])) > 5000, 60, step=1)
            check(moved, "the host fast-travelled", f"{p0} -> {safe(lambda: pos(0))}")
            together = wait(lambda: math.dist((pos(0)["X"], pos(0)["Y"]), (pos(1)["X"], pos(1)["Y"])) < 1500, 60, step=1)
            d = safe(lambda: math.dist((pos(0)["X"], pos(0)["Y"]), (pos(1)["X"], pos(1)["Y"])), -1)
            check(together and st(1).get("travelsFollowed", 0) > f0, "the client fast-travelled with it", f"{d:.0f} units apart; {st(1).get('lastTravel')!r}")
            check(wait(lambda: st(0)["puppet"] != "none" and st(1)["puppet"] != "none", 60), "both see each other after the fast travel")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "ABC"))
