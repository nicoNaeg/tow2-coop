"""Campaign hand-over over Steam on one copy, no second player needed: the copy saves its campaign and pushes it over
Steam to its own SteamID; the client side writes CoopHost_ABCDEF01 and loads it. Checks the Steam link, byte
equality of every file and the load. Run with one copy in gameplay, co-op on over direct IP (tow2coop.ini role=host;
in Steam mode the game thread would read the messages itself). Usage: python -I test_steam_self.py <sandbox dir>"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def main(sandbox):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    p = call("coop.steamProbe", {}, inst=0, timeout_s=60)
    check(p.get("available") and p.get("relay") == 100 and p.get("selfReceived"), "Steam link up (relay ready, self message back)", str(p))
    r = call("coop.offer", {"steamSelf": True}, inst=0)
    check(r.get("save") in ("Success", "AsyncSaveTask_Begun"), "host saved its campaign", str(r))
    t0 = time.time()
    s = call("coop.steamPump", {"seconds": 90}, inst=0, timeout_s=120)
    check(s.get("state") in ("received", "sent") or str(s.get("state")).startswith("load"), "the campaign came back over Steam",
          f"{s.get('state')} {s.get('bytes'):.0f} bytes in {time.time() - t0:.1f} s {s.get('error')}")
    saves = Path(sandbox) / "SavedGames" / "TheOuterWorlds2"
    src, dst = saves / "CoopHost", saves / "CoopHost_ABCDEF01"
    a = {f.name: f.read_bytes() for f in src.iterdir()} if src.is_dir() else {}
    b = {f.name: f.read_bytes() for f in dst.iterdir()} if dst.is_dir() else {}
    check(bool(a) and a == b, "every received file equals the host's save byte for byte", f"{len(a)} files, {sum(map(len, a.values()))} bytes")
    st, t0 = "", time.time()
    while time.time() - t0 < 120:
        try:
            st = call("coop.joinStatus", {}, inst=0, timeout_s=10).get("state", "")
        except Exception:
            st = "busy"  # the game is loading
        if st.startswith("loaded"):
            break
        time.sleep(3)
    check(st == "loaded:Success", "the received campaign loads", st)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
