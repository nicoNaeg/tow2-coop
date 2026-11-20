"""Crash hunt (runs 161, 164: a crash on arriving in 0605_OAHQ, one at a copy's start, each seen once): on a running pair
in gameplay, the host travels to the Order's headquarters and back N times; the client follows each time. After each
arrival both games must still run (their bridges answer) and 20 s pass in the new map (run 161 crashed 20 s after
arriving). The test copies' logs note any access violation ("fault: ..."); they are listed at the end.
Usage: python -I stress_travel.py [rounds]"""
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

DEST = "/Game/Maps/TravelDestinationData/0601_Cloister/0605_OAHQHardLoad.0605_OAHQHardLoad"
DEST_BACK = "/Game/Maps/POI_Data/06_Cloister/0605_OAHQ_ExitHardLoad.0605_OAHQ_ExitHardLoad"
LOGS = [Path(rf"C:\coop-anything\local\gamepool\instances\tow2\{i}\sandbox\tow2coop.log") for i in (0, 1)]


def safe(f, d=None):
    try:
        return f()
    except Exception:  # noqa: BLE001
        return d


def world(i):
    return str(call("eval", {"expr": "world"}, inst=i, timeout_s=10)["value"]).split(".")[-1]


def alive(i):
    cmd = r"@(Get-Process -Name TheOuterWorlds2-Win64-Shipping -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'C:\coop-anything\local\gamepool\instances\tow2\%d\*' }).Count" % i
    return subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.strip() == "1"


def main(rounds):
    sizes = [p.stat().st_size if p.exists() else 0 for p in LOGS]
    fails = 0
    for k in range(rounds * 2):
        w = safe(lambda: world(0), "")
        dest, want = (DEST_BACK, "0601") if "0605" in w else (DEST, "0605")
        tm = call("ue.find", {"class": "TravelManager"}, inst=0)[0].split()[0]
        d = call("ue.find", {"path": dest}, inst=0)
        if not d:
            lib = call("ue.find", {"path": "/Script/Engine.Default__KismetSystemLibrary"}, inst=0)[0].split()[0]
            safe(lambda: call("ue.call", {"ref": lib, "fn": "LoadAsset_Blocking", "args": {"Asset": dest}}, inst=0))
            d = call("ue.find", {"path": dest}, inst=0)
        call("ue.call", {"ref": tm, "fn": "BeginTravelToBP", "args": {"Destination": d[0].split()[0], "bPassGametime": False,
                                                                      "bTriggerAutosave": False, "bForceBlocking": False}}, inst=0)
        t0 = time.time()
        while time.time() - t0 < 120 and not all(want in safe(lambda i=i: world(i), "") for i in (0, 1)):
            time.sleep(2)
        arrived = all(want in safe(lambda i=i: world(i), "") for i in (0, 1))
        time.sleep(25)  # run 161: the crash came ~20 s after arriving
        ok = arrived and all(alive(i) and safe(lambda i=i: world(i)) for i in (0, 1))
        print(f"{'PASS' if ok else 'FAIL'} travel {k + 1}/{rounds * 2} to {want}: both arrived {arrived}, both still running after 25 s "
              f"{[alive(i) for i in (0, 1)]}", flush=True)
        fails += not ok
        if not ok:
            break
    for p, n in zip(LOGS, sizes):
        if p.exists():
            with open(p, "rb") as f:
                f.seek(n)
                for line in f.read().decode("utf-8", "replace").splitlines():
                    if "fault:" in line or "frame error" in line:
                        print(f"info: {p.parent.parent.name}: {line[:300]}")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]) if len(sys.argv) > 1 else 5))
