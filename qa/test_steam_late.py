"""Steam that comes up after the mod (a game started by Steam sets up its Steam session after the mod's start; beta.2
checked once and stayed off, two-PC test 2026-11-17). One copy at the menu with tow2coop.ini role=client,
transport=steam, steam_delay=<s> (the mod ignores Steam for that long): the mod waits, then links up by itself.
Usage: python -I test_steam_late.py <copy's tow2coop.log> <delay s>"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def main(log, delay):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    t0, s = time.time(), {}
    while time.time() - t0 < 600:
        try:
            s = call("coop.status", {}, inst=0, timeout_s=10)
            break
        except Exception:
            time.sleep(3)
    check(s.get("on") and s.get("transport") == "steam", "co-op on over Steam", f"on {s.get('on')}, transport {s.get('transport')}")
    while time.time() - t0 < 600 + delay and not s.get("steamUp"):
        time.sleep(2)
        s = call("coop.status", {}, inst=0, timeout_s=10)
    check(s.get("steamUp") is True, "the Steam link came up by itself", f"after {time.time() - t0:.0f} s of polling")
    text = Path(log).read_text(encoding="utf-8", errors="replace")
    run = text[text.rfind("build ok:"):]  # this start only
    lines = [ln for ln in run.splitlines() if "co-op:" in ln]
    print("\n".join(lines[:4]))
    order = [any("Steam; client" in ln for ln in lines[:1]),
             any("waiting for the game's Steam session (test delay)" in ln for ln in lines[1:2]),
             any("Steam link ready (app 1449110, 1 MB/s)" in ln for ln in lines[2:3])]  # beta.5: send rate set
    check(all(order), "log: start, waiting, ready (in this order)", str(order))
    m = re.search(r"Steam link ready \(app \d+[^)]*\), (\d+) s after", run)
    check(m is not None and int(m.group(1)) >= delay, "the link waited for Steam", m.group(0) if m else "no ready line")
    # The host's accept pass (beta.4) over this account's friends: runs on real Steam without a second account.
    a = call("coop.steamAccept", {}, inst=0, timeout_s=30)
    check(a.get("on") is True and a.get("friends", -1) >= 0, "accept pass over the friends list", str(a))
    text = Path(log).read_text(encoding="utf-8", errors="replace")
    run = text[text.rfind("build ok:"):]
    errors = [ln for ln in run.splitlines() if "co-op frame error" in ln or "join error" in ln]
    check(not errors, "no caught faults", "; ".join(errors[:3]))
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], int(sys.argv[2])))
