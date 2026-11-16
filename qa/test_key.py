"""Shared key (tow2coop.ini key=...): copies with different keys never pair up. Run with the pair up, the host in
gameplay and the client at the main menu, the two copies holding different keys (run_key.ps1 does all of it).
Usage: python -I test_key.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    time.sleep(150)  # the client asks every 5 s once its front end has run a minute
    h, c = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    check(c.get("joinRequests", 0) > 0, "the client asked to join", f"requests {c.get('joinRequests')}")
    check(h["bad"] > 0 and not h["peer"], "the host ignored it (other key)", f"bad {h['bad']}, peer {h['peer']}")
    w1 = call("eval", {"expr": "world"}, inst=1)["value"]
    check("MainMenu" in w1, "the client is still at the main menu", w1[-40:])
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
