"""Stand-in animation on a running pair in gameplay (copy 0 host, copy 1 client, each sees the other's stand-in): the
client walks (held W) and jumps (SPACE); the host's stand-in plays the walk (anim SmoothedSpeed up, then back down)
and the in-air state (bMovementStateIsInAir). Two-PC test 2026-11-17: the stand-in slid in a frozen pose, because
its movement component (the AI controller is kept) zeroed the velocity the mod writes.
Usage: python -I test_anim.py <client pid>"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def main(client_pid):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    p = call("coop.status", {}, inst=0)["puppet"]
    check(p != "none", "host sees the client's stand-in", p)
    if p == "none":
        print(f"RESULT: FAIL ({fails} failure(s))")
        return fails
    ref = p.split()[0]
    anim = call("eval", {"expr": f"{ref}.Mesh.AnimScriptInstance"}, inst=0)["value"].split()[0]
    ticking = call("ue.call", {"ref": f"{ref}.CharacterMovement", "fn": "IsComponentTickEnabled"}, inst=0)["ReturnValue"]
    check(ticking is False, "the stand-in's movement component does not tick (it would zero the velocity)", f"ticking {ticking}")
    speed = lambda: call("ue.get", {"path": f"{anim}.SmoothedSpeed"}, inst=0)  # noqa: E731
    g = Game(client_pid)
    g.key("W", hold_s=0.05)  # wake the window's input
    time.sleep(0.5)
    samples = []
    import threading
    walker = threading.Thread(target=lambda: g.key("W", hold_s=3.0))
    walker.start()
    t0 = time.time()
    while time.time() - t0 < 3.0:
        samples.append(safe(speed, 0) or 0)
        time.sleep(0.1)
    walker.join()
    check(max(samples) > 150, "the stand-in walks while the client walks", f"anim speed max {max(samples):.0f}")
    time.sleep(2.5)
    still = safe(speed, -1)
    check(0 <= still < 60, "and stands when the client stops", f"anim speed {still:.0f}" if still is not None else "")
    air = False
    jumper = threading.Thread(target=lambda: g.key("SPACE", hold_s=0.15))
    jumper.start()
    t0 = time.time()
    while time.time() - t0 < 2.0 and not air:
        air = safe(lambda: call("ue.get", {"path": f"{anim}.bMovementStateIsInAir"}, inst=0), False) is True
        time.sleep(0.05)
    jumper.join()
    check(air, "the stand-in is in the air while the client jumps")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1])))
