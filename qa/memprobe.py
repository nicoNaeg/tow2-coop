"""Memory bisection on a running copy: samples its private bytes every 10 s while sync parts are switched off and on
(bridge method coop.set), and prints each phase's growth in MB per minute.
Usage: python -I memprobe.py <pid> <inst> [minutes per phase]"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + [(n, ctypes.c_size_t) for n in (
        "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
        "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]


def mem(pid):
    k = ctypes.windll.kernel32
    k.OpenProcess.restype = wt.HANDLE
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    try:
        c = PMC()
        c.cb = ctypes.sizeof(c)
        if not k.K32GetProcessMemoryInfo(wt.HANDLE(h), ctypes.byref(c), c.cb):
            return None
        return c.PrivateUsage / 2**20, c.WorkingSetSize / 2**20
    finally:
        k.CloseHandle(wt.HANDLE(h))


def slope(pts):  # least squares, MB per minute
    n = len(pts)
    if n < 2:
        return 0.0
    mt, mv = sum(t for t, _ in pts) / n, sum(v for _, v in pts) / n
    den = sum((t - mt) ** 2 for t, _ in pts)
    return sum((t - mt) * (v - mv) for t, v in pts) / den * 60 if den else 0.0


def frames(inst):
    return call("core.status", {}, inst=inst)["frames"]


def main(pid, inst, minutes):
    on = {"story": True, "doors": True, "scan": True, "npcs": True}
    phases = [("all on", on), ("story off", {"story": False}), ("story+npcs off", {"npcs": False}),
              ("everything off", {"doors": False, "scan": False}), ("all on again", on), ("all on, window restored", on)]
    assert abs(slope([(0, 0), (60, 10)]) - 10) < 1e-9
    for name, opts in phases:
        if "restored" in name:  # behind the other windows, not activated
            subprocess.run(["powershell", "-NoProfile", "-File", str(Path(__file__).resolve().parent / "unminimize.ps1"), "-ProcessId", str(pid)], check=False)
        print(f"== {name}: {call('coop.set', opts, inst=inst)}", flush=True)
        pts, t0, f0 = [], time.time(), frames(inst)
        while time.time() - t0 < minutes * 60:
            m = mem(pid)
            if m is None:
                print("process gone")
                return 1
            pts.append((time.time() - t0, m[0]))
            time.sleep(10)
        fps = (frames(inst) - f0) / (time.time() - t0)
        print(f"   private {pts[0][1]:.0f} -> {pts[-1][1]:.0f} MB, slope {slope(pts):+.1f} MB/min, working set {mem(pid)[1]:.0f} MB, {fps:.0f} frames/s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]) if len(sys.argv) > 3 else 2))
