"""Automatic join: the host (copy 0) is in gameplay, the client (copy 1) sits at the main menu and was never told to
join; after its front end has run a minute it asks the host, receives the campaign and loads it.
Usage: python -I test_autojoin.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    w0 = call("eval", {"expr": "world"}, inst=0)["value"].split()[-1]
    w1 = safe(lambda: call("eval", {"expr": "world"}, inst=1)["value"].split()[-1], "?")
    check("ConversationalMainMenu" in w1, "client starts at the main menu", w1[-40:])
    t0 = time.time()
    while time.time() - t0 < 360:
        w1 = safe(lambda: call("eval", {"expr": "world"}, inst=1, timeout_s=10)["value"].split()[-1], "loading")
        if w1 == w0:
            break
        time.sleep(3)
    check(w1 == w0, "client joined the host's map by itself", f"after {time.time() - t0:.0f} s: {w1[-40:]}")
    t1 = time.time()
    while time.time() - t1 < 120:
        s0, s1 = safe(lambda: call("coop.status", {}, inst=0), {}), safe(lambda: call("coop.status", {}, inst=1), {})
        if s0.get("puppet", "none") != "none" and s1.get("puppet", "none") != "none":
            break
        time.sleep(3)
    check(s0.get("puppet", "none") != "none" and s1.get("puppet", "none") != "none", "both see each other", f"{s0.get('puppet')} / {s1.get('puppet')}")
    hp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
    cp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"]
    d = sum((hp[k] - cp[k]) ** 2 for k in "XYZ") ** 0.5
    check(d < 800, "client spawned near the host", f"{d:.0f} units apart")
    # The client plays a copy of the host's character, so in each world the stand-in must look like the local player.
    for inst, s, who in ((0, s0, "host"), (1, s1, "client")):
        if s.get("puppet", "none") == "none":
            continue
        look = {k: [safe(lambda r=r, k=k: call("eval", {"expr": f"{r}.AppearanceComponent.{k}"}, inst=inst)["value"]) for r in ("pawn", s["puppet"])]
                for k in ("AppearanceGender", "CurrentHead", "CurrentBody", "Customization")}
        diff = [k for k, (mine, theirs) in look.items() if mine is None or mine != theirs]
        check(not diff, f"the stand-in in the {who}'s world wears the player's look", f"differs: {diff}" if diff else "")
    print(f"client join requests {s1.get('joinRequests')}, fetches {s1.get('resyncFetches')}; host resyncs {s0.get('resyncs')} ({s0.get('resyncReason')})")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
