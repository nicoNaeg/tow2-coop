"""Bring running test copies into gameplay: wait for each core, load a save, wait for the pawn.

    python -I session.py [--instances 0,1] [--save B9F05C364F38D4A68B3AB39CCB0910F8]"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

DEFAULT_SAVE = "B9F05C364F38D4A68B3AB39CCB0910F8"  # copied owner save, 0601 Cloister overlands


def wait_core(i, timeout_s=600):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            if call("core.status", {}, inst=i, timeout_s=10).get("status") == "running":
                return True
        except Exception:
            pass
        time.sleep(3)
    raise TimeoutError(f"core {i} not running after {timeout_s}s")


def load(i, save):
    # LoadGame within the first ~15 s of the front end crashed the engine in LoadMap (exe+0x1429567) 5 times out of 5;
    # after 60 s at the menu it loads cleanly. Frames count from the core's hooks (menu capped at 60 fps).
    while call("core.status", {}, inst=i, timeout_s=15).get("frames", 0) < 3600:
        time.sleep(2)
    sgm = call("ue.find", {"class": "SaveGameManager"}, inst=i)[0].split()[0]
    try:
        r = call("ue.call", {"ref": sgm, "fn": "LoadGame", "args": {"Filename": save, "bPreValidateMetadata": True}, "timeoutMs": 240000},
                 inst=i, timeout_s=260)
    except Exception as e:  # the call blocks for the whole map load; the reply can come late
        r = {"late": str(e)[:80]}
    for _ in range(100):
        try:
            s = call("state", {}, inst=i, timeout_s=15)
            if "pawn" in s and "ConversationalMainMenu" not in s["world"]:
                return r, s
        except Exception:
            pass
        time.sleep(3)
    raise TimeoutError(f"copy {i}: no pawn after loading {save}")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--instances", default="0,1")
    ap.add_argument("--save", default=DEFAULT_SAVE)
    a = ap.parse_args(argv)
    for i in [int(x) for x in a.instances.split(",")]:
        wait_core(i)
        r, s = load(i, a.save)
        print(f"copy {i}: LoadGame {r} -> {s['world'].split('/')[-1]} pawn at {s['pawn']['location']}", flush=True)


if __name__ == "__main__":
    main(sys.argv[1:])
