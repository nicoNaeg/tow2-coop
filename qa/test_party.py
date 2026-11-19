"""Companions out of a fight (owner, 2026-11-19, point 4) on a running pair in gameplay: one party, the host's. The
host walks off (teleported 12 m): its companions follow it in its world, and the client's world shows them there too
(the client's companions are driven from the host's, their AI paused), within 2 m. Usage: python -I test_party.py"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

COMPANIONS = ("Marisol_C", "Azahelia_C", "Niles_C", "Inez_C", "Tristan_C", "Valerie_C", "Aza_C")


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def loc(inst, ref):
    return call("eval", {"expr": ref + ".K2_GetActorLocation()"}, inst=inst)["value"]


def companions(inst):
    return {r.split()[1]: r.split()[0] for r in call("ue.find", {"class": "Character", "limit": 900}, inst=inst) if r.split()[1] in COMPANIONS}


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok
        return ok

    h, c = companions(0), companions(1)
    both = sorted(set(h) & set(c))
    if not check(bool(both), "companions in both worlds", ", ".join(both)):
        print(f"RESULT: FAIL ({fails} failure(s))")
        return fails
    here = loc(0, "pawn")
    yaw = call("eval", {"expr": "pawn.K2_GetActorRotation()"}, inst=0)["value"]["Yaw"]
    to = {"X": here["X"] + 1200 * math.cos(math.radians(yaw)), "Y": here["Y"] + 1200 * math.sin(math.radians(yaw)), "Z": here["Z"] + 50}
    call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": to, "bSweep": False, "bTeleport": True}}, inst=0)
    time.sleep(8.0)  # the host's companions walk after it
    gaps = []
    for name in both:
        a, b = safe(lambda: loc(0, h[name])), safe(lambda: loc(1, c[name]))
        if a and b:
            gaps.append((name, math.dist((a["X"], a["Y"], a["Z"]), (b["X"], b["Y"], b["Z"])) / 100))
    st = call("coop.status", {}, inst=1)
    check(st.get("partyDriven", 0) > 0, "the client's world drives its companions from the host's", f"{st.get('partyDriven')} driven, calm {st.get('partyCalm')}")
    check(bool(gaps) and max(g for _, g in gaps) < 2.0, "each companion is where it is in the host's world", ", ".join(f"{n} {g:.1f} m" for n, g in gaps))
    call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": here, "bSweep": False, "bTeleport": True}}, inst=0)
    time.sleep(2.0)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
