"""Disconnect/rejoin check on a running pair in gameplay (copy 0 host, copy 1 client):
kill the client process -> host drops it within 8 s (puppet removed, peer free); relaunch the client with the same
environment, load the save -> host and client see each other again (puppets on both). No crash on either side.
Usage: python -I test_rejoin.py <client pid>"""
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from session import load, wait_core  # noqa: E402
from tow2 import call  # noqa: E402


def ps(script, *args):
    return subprocess.run(["powershell", "-NoProfile", "-File", str(HERE / script), *args], capture_output=True, text=True).stdout


def main(client_pid):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    s = call("coop.status", {}, inst=0)
    check(s["remote"] != 0 and s["puppet"] != "none", "linked before the disconnect", str({k: s[k] for k in ("remote", "puppet")}))
    subprocess.run(["taskkill", "/F", "/PID", str(client_pid)], capture_output=True)
    time.sleep(8)
    s = call("coop.status", {}, inst=0)
    check(s["remote"] == 0 and s["puppet"] == "none" and not s["peer"], "host dropped the lost client", str({k: s[k] for k in ("remote", "puppet", "peer", "drops")}))
    out = subprocess.run([sys.executable, "-I", str(HERE / "relaunch.py"), "1", "client"], capture_output=True, text=True).stdout
    pid = int(out.strip().split()[-1])
    for _ in range(60):  # window, then restore it behind other windows (minimized copies load slowly)
        time.sleep(3)
        if "restored" in ps("unminimize.ps1", "-ProcessId", str(pid)):
            break
    wait_core(1)
    load(1, "B9F05C364F38D4A68B3AB39CCB0910F8")
    time.sleep(15)  # settle period after the load plus a few updates
    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    check(s0["remote"] != 0 and s0["puppet"] != "none" and s1["puppet"] != "none", "rejoined: puppets on both sides",
          f"host {({k: s0[k] for k in ('remote', 'puppet', 'drops')})} client {({k: s1[k] for k in ('remote', 'puppet', 'drops')})}")
    time.sleep(20)
    alive = subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-Process -Id {pid} -ErrorAction SilentlyContinue).Responding"],
                           capture_output=True, text=True).stdout.strip()
    check(alive == "True", "relaunched client still running 35 s after the load", alive)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))  client pid {pid}")
    return fails


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1])))
