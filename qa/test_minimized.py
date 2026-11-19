"""A minimized host still finishes the mod's saves (owner, 2026-11-19), on a running pair: the host's window is
minimized, the host starts a hand-over save (coop.offer, on a spare port): the save finishes (its window is drawn
again behind the other windows meanwhile), then the window is minimized again. Usage: python -I test_minimized.py"""
import ctypes
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

user32 = ctypes.windll.user32


def pid(i):
    cmd = r"(Get-Process -Name TheOuterWorlds2-Win64-Shipping | Where-Object { $_.Path -like 'C:\coop-anything\local\gamepool\instances\tow2\%d\*' }).Id" % i
    return int(subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.split()[0])


def wait(pred, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return True
        time.sleep(0.25)
    return pred()


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok
        return ok

    hwnd = Game(pid(0)).hwnd()
    user32.ShowWindow(hwnd, 6)  # SW_MINIMIZE
    try:
        check(wait(lambda: user32.IsIconic(hwnd), 3), "the host's window is minimized")
        r = call("coop.offer", {"port": 27991}, inst=0)
        state = lambda: call("coop.joinStatus", {}, inst=0).get("state", "")  # noqa: E731
        done = wait(lambda: state() not in ("saving", ""), 30)
        check(done and state() in ("serving", "sent"), "its hand-over save finished all the same", f"{state()} ({r})")
        check(wait(lambda: user32.IsIconic(hwnd), 5), "and its window is minimized again")
    finally:
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE: the next steps need it drawn
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
