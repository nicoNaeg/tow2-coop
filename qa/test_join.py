"""Join check on a running pair: the host (copy 0) is in gameplay, the client (copy 1) sits at the main menu.
The host saves and serves its campaign, the client downloads it into its own "CoopHost_<id>" folder and loads it;
then both are in the same map at the same spot and see each other. The client's own saves must be untouched.
Usage: python -I test_join.py"""
import hashlib
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

CLIENT_SAVES = Path(r"C:\coop-anything\local\gamepool\instances\tow2\1\sandbox\SavedGames\TheOuterWorlds2")
OWN = CLIENT_SAVES / "B9F05C364F38D4A68B3AB39CCB0910F8" / "SaveGame.dat"


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    own_before = hashlib.sha256(OWN.read_bytes()).hexdigest()
    host = call("coop.status", {}, inst=0)
    w1 = call("eval", {"expr": "world"}, inst=1)["value"]
    check("ConversationalMainMenu" in w1, "client starts at the main menu", w1.split()[-1][-40:])
    print("offer:", call("coop.offer", {}, inst=0))
    for _ in range(40):
        st = call("coop.joinStatus", {}, inst=0)
        if st["state"] in ("serving", "failed"):
            break
        time.sleep(0.5)
    check(st["state"] == "serving", "host saved and serves its campaign", str(st))
    print("join:", call("coop.join", {"hostId": host["me"]}, inst=1))
    t0 = time.time()
    while time.time() - t0 < 240:
        try:
            st1 = call("coop.joinStatus", {}, inst=1, timeout_s=10)
        except Exception:
            st1 = {"state": "busy"}
        if st1["state"].startswith(("loaded", "failed")):
            break
        time.sleep(2)
    check(st1["state"] == "loaded:Success", "client received and loaded the host's save", str(st1))
    time.sleep(20)  # map load + settle period
    h_world, c_world = call("eval", {"expr": "world"}, inst=0)["value"].split()[-1], call("eval", {"expr": "world"}, inst=1)["value"].split()[-1]
    check(h_world == c_world, "same map on both", c_world[-40:])
    hp, cp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"], call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"]
    d = sum((hp[k] - cp[k]) ** 2 for k in "XYZ") ** 0.5
    check(d < 500, "client spawned where the host saved", f"{d:.0f} units apart")
    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    check(s0["puppet"] != "none" and s1["puppet"] != "none", "both see each other after the join", f"{s0['puppet']} / {s1['puppet']}")
    got = CLIENT_SAVES / st1.get("folder", "?")
    check(got.is_dir() and (got / "SaveGame.dat").exists(), "host save stored in its own folder on the client", got.name)
    check(hashlib.sha256(OWN.read_bytes()).hexdigest() == own_before, "client's own save untouched")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
