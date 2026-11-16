"""30-minute soak of a running pair in gameplay (copy 0 host, copy 1 client): both players move and attack with posted
input; every 30 s the run checks liveness, packet flow, puppet-vs-player distance and synced NPC health.
Writes one JSON line per check to the output file and a summary at the end.
Usage: python -I soak.py <host pid> <client pid> <out.jsonl> [minutes]"""
import json
import math
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

import ctypes  # noqa: E402

from ctypes import wintypes  # noqa: E402

k32 = ctypes.WinDLL("kernel32")
k32.OpenProcess.restype, k32.OpenProcess.argtypes = wintypes.HANDLE, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
_handles = {}  # kept open so the exit code stays readable after the process ends


def exit_code(pid):
    """None while running, else the process's exit code (0 = it quit normally, e.g. its window was closed)."""
    h = _handles.get(pid) or k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return -1
    _handles[pid] = h
    code = wintypes.DWORD()
    k32.GetExitCodeProcess(h, ctypes.byref(code))
    return None if code.value == 259 else code.value  # 259 = STILL_ACTIVE


def alive(pid):
    return exit_code(pid) is None


CRASH_DIRS = [Path(rf"C:\coop-anything\local\gamepool\instances\tow2\{i}\sandbox\LocalAppData\Arkansas\Saved\Crashes") for i in (0, 1)]


def wander(game, inst, rng):
    """Turn to a random yaw, walk 1.2 s, walk back 1.2 s (stays near the start), sometimes swing the weapon."""
    yaw = rng.uniform(-180, 180)
    try:
        call("ue.call", {"ref": "pc", "fn": "SetControlRotation", "args": {"NewRotation": {"Pitch": -5, "Yaw": yaw, "Roll": 0}}}, inst=inst, timeout_s=10)
    except Exception:
        pass
    game.key("W", hold_s=1.2)
    game.key("S", hold_s=1.2)
    if rng.random() < 0.3:
        game.click("left")


def dist(a, b):
    return math.dist((a["X"], a["Y"], a["Z"]), (b["X"], b["Y"], b["Z"]))


def check(t0):
    row = {"t": round(time.time() - t0)}
    s0, s1 = call("coop.status", {}, inst=0, timeout_s=20), call("coop.status", {}, inst=1, timeout_s=20)
    row.update(hostRecv=s0["recv"], clientRecv=s1["recv"], hostPuppet=s0["puppet"] != "none", clientPuppet=s1["puppet"] != "none",
               drops=(s0["drops"], s1["drops"]), dmg=(s1["dmgSent"], s0["dmgApplied"]), killed=s1["npcKilled"])
    # puppet in the client's world vs the host's real pawn (and the reverse); 100 ms interpolation delay
    for a, b, key in ((0, 1, "hostVsClientPuppet"), (1, 0, "clientVsHostPuppet")):
        try:
            real = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=a)["value"]
            pup = call("coop.status", {}, inst=b)["puppet"]
            seen = call("eval", {"expr": pup + ".K2_GetActorLocation()"}, inst=b)["value"]
            row[key] = round(dist(real, seen), 1)
        except Exception as e:
            row[key] = f"n/a: {str(e)[:60]}"
    # synced NPC health: up to 6 NPCs near the host
    me = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
    worst, n = 0.0, 0
    for c in call("ue.find", {"class": "Character", "limit": 400}, inst=0):
        a, cls, path = c.split(" ", 2)
        if "_UAID_" not in path or n >= 6:
            continue
        try:
            p = call("eval", {"expr": a + ".K2_GetActorLocation()"}, inst=0)["value"]
            if dist(p, me) > 7000:
                continue
            b = call("ue.find", {"path": path}, inst=1)
            if not b:
                continue
            h0 = call("ue.call", {"ref": a + ".HealthComponent", "fn": "GetHealth"}, inst=0)["ReturnValue"]
            h1 = call("ue.call", {"ref": b[0].split()[0] + ".HealthComponent", "fn": "GetHealth"}, inst=1)["ReturnValue"]
            worst, n = max(worst, abs(h0 - h1)), n + 1
        except Exception:
            pass
    row.update(npcChecked=n, npcHealthMaxDiff=round(worst, 1))
    return row


def main(host_pid, client_pid, out, minutes=30):
    rng = random.Random(1449110)
    games = (Game(host_pid), Game(client_pid))
    t0, next_check = time.time(), time.time() + 30
    rows, problems = [], []
    with open(out, "w", encoding="utf-8") as f:
        while time.time() - t0 < minutes * 60:
            crashes = [str(p) for i in (0, 1) for p in CRASH_DIRS[i].glob("UECC-*")] if any(d.exists() for d in CRASH_DIRS) else []
            if crashes:  # a process in the crash handler still looks alive: the crash folder tells
                problems.append({"t": round(time.time() - t0), "crashReports": crashes})
                break
            if not (alive(host_pid) and alive(client_pid)):
                problems.append({"t": round(time.time() - t0), "exited": {p: hex(exit_code(p) or 0) for p in (host_pid, client_pid) if not alive(p)}})
                break
            for inst, g in enumerate(games):
                try:
                    wander(g, inst, rng)
                except Exception as e:
                    problems.append({"t": round(time.time() - t0), "input": str(e)[:80]})
            if time.time() >= next_check:
                next_check += 30
                try:
                    row = check(t0)
                except Exception as e:
                    row = {"t": round(time.time() - t0), "checkError": str(e)[:120]}
                rows.append(row)
                f.write(json.dumps(row) + "\n")
                f.flush()
            time.sleep(1)
        recv_ok = len(rows) >= 2 and all(isinstance(r.get("hostRecv"), (int, float)) for r in rows[-2:]) and rows[-1]["hostRecv"] > rows[0]["hostRecv"]
        dists = [r[k] for r in rows for k in ("hostVsClientPuppet", "clientVsHostPuppet") if isinstance(r.get(k), (int, float))]
        diffs = [r["npcHealthMaxDiff"] for r in rows if "npcHealthMaxDiff" in r]
        summary = {"summary": True, "minutes": round((time.time() - t0) / 60, 1), "checks": len(rows), "problems": problems,
                   "bothAlive": alive(host_pid) and alive(client_pid), "packetsKeptFlowing": recv_ok,
                   "puppetDistanceMax": max(dists) if dists else None, "puppetDistanceMedian": sorted(dists)[len(dists) // 2] if dists else None,
                   "npcHealthDiffMax": max(diffs) if diffs else None, "checkErrors": sum(1 for r in rows if "checkError" in r)}
        f.write(json.dumps(summary) + "\n")
        print(json.dumps(summary))


if __name__ == "__main__":
    main(int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 30)
