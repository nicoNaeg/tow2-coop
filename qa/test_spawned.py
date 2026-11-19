"""Enemies the game spawns during play, shared (copy 0 host, copy 1 client, the test save loaded). The test spawns
Ursopods (Bear_T3_Soldier) the way a hive or a door would (coop.spawned), 20 m from the host, each part elsewhere.

A. The host's world only: a copy appears in the client's world, near the host's; killed in the host's world, it dies
   in the client's world too.
B. Both worlds at once (the same spawner fired in both): no copy, each world keeps its own, paired under one id.
C. The client's world only: the host's world gets a copy, the client's own one is paired with it; killed in the host's
   world, the client's dies too.
D. The host's one is removed: the client's copy goes too.
The spawned ones are killed at the end. Usage: python -I test_spawned.py [ABCD]"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

FAILS = 0
MADE = []  # (inst, ref) of every NPC the test spawned


def check(ok, what, detail=""):
    global FAILS
    print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
    FAILS += not ok
    return ok


def safe(f, d=None):
    try:
        return f()
    except Exception as e:  # noqa: BLE001
        return d if d is not None else "err " + str(e)[:160]


def ev(inst, expr):
    return call("eval", {"expr": expr}, inst=inst)["value"]


def uc(inst, ref, fn, args=None):
    return call("ue.call", {"ref": ref, "fn": fn, "args": args or {}}, inst=inst)


def loc(inst, ref):
    return ev(inst, ref + ".K2_GetActorLocation()")


def dist(a, b):
    return math.dist((a["X"], a["Y"], a["Z"]), (b["X"], b["Y"], b["Z"])) / 100


def wait_for(cond, secs, step=0.25):
    t0 = time.time()
    while time.time() - t0 < secs:
        v = safe(cond, None)
        if v and not (isinstance(v, str) and v.startswith("err")):
            return v
        time.sleep(step)
    return None


def shared(inst):
    return {r["id"]: r for r in call("coop.spawned", {}, inst=inst)}


def health(inst, ref):
    return uc(inst, ref + ".HealthComponent", "GetHealth")["ReturnValue"]


def spawn(inst, cls, p, yaw):
    r = call("coop.spawned", {"class": cls, "x": p["X"], "y": p["Y"], "z": p["Z"], "yaw": yaw}, inst=inst)
    ref = str(r).split()[0]
    MADE.append((inst, ref))
    return ref


def near_of_class(inst, short, p, radius=25.0):  # living ones (a spawn on a slope may slide a few meters)
    out = []
    for c in call("ue.find", {"class": "Character", "limit": 800}, inst=inst):
        a, cls, path = c.split(" ", 2)
        if cls == short and "_UAID_" not in path and safe(lambda: dist(loc(inst, a), p), 999) < radius and safe(lambda: health(inst, a), 0) > 0:
            out.append(a)
    return out


def bear_class():
    for c in call("ue.find", {"name": "Bear_T3_Soldier_C", "limit": 20}, inst=0):
        a, k, path = c.split(" ", 2)
        if k == "BlueprintGeneratedClass":
            return path
    return None


def swings(inst, target, n=4):
    """The player swings its blade (its weapon's trigger) at the target, from 1.6 m in front of it (probe 101: the
    game records the player as the last hitter, the host's copy loses the same)."""
    for _ in range(2):
        b, me = loc(inst, target), loc(inst, "pawn")
        a = math.atan2(me["Y"] - b["Y"], me["X"] - b["X"])
        spot = {"X": b["X"] + 160 * math.cos(a), "Y": b["Y"] + 160 * math.sin(a), "Z": b["Z"] + 20}
        uc(inst, "pawn", "K2_SetActorLocation", {"NewLocation": spot, "bSweep": False, "bTeleport": True})
        pc = ev(inst, "pawn.Controller").split()[0]
        uc(inst, pc, "SetControlRotation", {"NewRotation": {"Pitch": -5.0, "Yaw": math.degrees(math.atan2(b["Y"] - spot["Y"], b["X"] - spot["X"])), "Roll": 0.0}})
        time.sleep(0.4)
    uc(inst, "pawn.EquipmentComponent", "EquipWeaponFromSlot", {"SlotIndex": 0, "bInstantEquip": True, "bStartingCombat": False})
    time.sleep(0.5)
    w = ev(inst, "pawn.GetCurrentWeapon()").split()[0]
    for _ in range(n):
        uc(inst, w, "BeginPrimaryTrigger")
        time.sleep(0.12)
        uc(inst, w, "EndPrimaryTrigger", {"bForceEnd": False})
        time.sleep(0.9)


def unpause_all():
    """A game paused by one of its own screens (a flaw offer after a fight: "Hold Esc Reject") is answered first."""
    import subprocess
    from gamewin import Game
    for i in (0, 1):
        for _ in range(3):
            pc = safe(lambda: call("eval", {"expr": "pc"}, inst=i)["value"].split()[0], None)
            gs = safe(lambda: call("ue.find", {"path": "/Script/Engine.Default__GameplayStatics"}, inst=i)[0].split()[0], None)
            paused = pc and gs and safe(lambda: call("ue.call", {"ref": gs, "fn": "IsGamePaused", "args": {"WorldContextObject": pc}}, inst=i)["ReturnValue"], False) is True
            if not paused:
                break
            want = rf"C:\coop-anything\local\gamepool\instances\tow2\{i}\*"
            cmd = "(Get-Process -Name TheOuterWorlds2-Win64-Shipping | Where-Object { $_.Path -like '%s' }).Id" % want
            pid = int(subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.split()[0])
            print(f"info: copy {i}'s game is paused (a screen of its own): Escape held", flush=True)
            safe(lambda: Game(pid).key("ESCAPE", hold_s=2.0))
            time.sleep(1.0)


def revive_if_down():
    """A player left down by the step before (run 121: the client died in the fight test) is revived, god mode again."""
    for i in (0, 1):
        life = str(safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "GetStateOfBeing"}, inst=i)["ReturnValue"], "")).split(":")[-1]
        if life and life != "Healthy":
            safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "Resurrect", "args": {"bDebug": False, "HealthPercentOverride": 1.0}}, inst=i))
            safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "SetGod", "args": {"bEnabled": True}}, inst=i))
            print(f"info: copy {i}'s player was {life} after the step before: revived", flush=True)
            time.sleep(8)  # its stand-in comes back in the other world


def main(parts):
    unpause_all()
    revive_if_down()
    for i in (0, 1):  # a hand-over earlier makes a new pawn, without god mode (run 136: the bears killed the client)
        safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "SetGod", "args": {"bEnabled": True}}, inst=i))
    cls = bear_class()
    if not check(bool(cls), "the Ursopod class is loaded", str(cls)):
        return 1
    short = cls.rsplit(".", 1)[-1]
    me = loc(0, "pawn")
    yaw = ev(0, "pawn.K2_GetActorRotation()")["Yaw"]

    nav = safe(lambda: call("ue.find", {"path": "/Script/NavigationSystem.Default__NavigationSystemV1"}, inst=0)[0].split()[0], None)
    host_pawn = ev(0, "pawn").split()[0]

    def place(turn):  # 20 m away, on the ground: a point past a ledge drops the bear out of the test (run 131)
        for d, t in ((2000, 0), (1700, 0), (2000, 20), (2000, -20), (2300, 0), (1700, 25), (1700, -25)):
            a = math.radians(yaw + turn + t)
            q = {"X": me["X"] + d * math.cos(a), "Y": me["Y"] + d * math.sin(a), "Z": me["Z"]}
            r = safe(lambda: call("ue.call", {"ref": nav, "fn": "K2_ProjectPointToNavigation", "args": {
                "WorldContextObject": host_pawn, "Point": q, "QueryExtent": {"X": 300.0, "Y": 300.0, "Z": 400.0}}}, inst=0), {}) if nav else {}
            g = r.get("ProjectedLocation") if r.get("ReturnValue") else None
            if g and abs(g["Z"] - me["Z"]) < 400:
                return {"X": g["X"], "Y": g["Y"], "Z": g["Z"] + 100}
        print(f"info: no ground found {turn} degrees from the host: the first point", flush=True)
        a = math.radians(yaw + turn)
        return {"X": me["X"] + 2000 * math.cos(a), "Y": me["Y"] + 2000 * math.sin(a), "Z": me["Z"] + 60}

    try:
        if "A" in parts:
            p = place(0)
            before = set(shared(1))
            mine = spawn(0, cls, p, yaw + 180)
            rec = wait_for(lambda: next((r for i, r in shared(1).items() if i not in before and r["actor"] != "none"), None), 8)
            check(bool(rec) and rec["copy"], "the host's spawned Ursopod got a copy in the client's world", str(rec))
            if rec:
                d = safe(lambda: dist(loc(0, mine), loc(1, rec["actor"])), 99)
                check(d < 6, "next to the host's", f"{d:.1f} m apart")
                # Only the client's own hits are reported to the host (a companion's: the host's game has its own; the
                # fight test checks that none are reported while the client stays out). Its blade, swung at its copy.
                time.sleep(1.5)  # in both registries
                h0 = health(0, mine)
                swings(1, rec["actor"])
                down = wait_for(lambda: health(0, mine) < h0 - 5, 4)
                check(bool(down), "the client's swings at its copy reached the host's", f"host {h0:.0f} -> {health(0, mine):.0f}")
                uc(0, mine + ".HealthComponent", "Suicide", {"bPreventRevival": True})
                died = wait_for(lambda: health(1, rec["actor"]) <= 0, 6)
                check(bool(died), "killed in the host's world, it died in the client's world", f"health {safe(lambda: health(1, rec['actor']))}")
        if "B" in parts:
            p = place(90)
            before0, before1 = set(shared(0)), set(shared(1))
            c0 = call("coop.status", {}, inst=1)["spawnCopies"]
            h = spawn(0, cls, p, yaw)
            c = spawn(1, cls, p, yaw)
            new1 = wait_for(lambda: [r for i, r in shared(1).items() if i not in before1 and r["actor"] != "none"], 8)
            time.sleep(2.0)
            new1 = [r for i, r in shared(1).items() if i not in before1]
            check(len(new1) == 1 and not new1[0]["copy"] and new1[0]["actor"] == c, "both spawned one: the client's own is paired, no copy",
                  str(new1))
            check(call("coop.status", {}, inst=1)["spawnCopies"] == c0, "the client made no copy")
            new0 = [r for i, r in shared(0).items() if i not in before0]
            check(len(new0) == 1 and new0[0]["actor"] == h and not new0[0]["copy"], "under the host's own one's id", str(new0))
        if "C" in parts:
            p = place(-90)
            before0, before1 = set(shared(0)), set(shared(1))
            mine = spawn(1, cls, p, yaw)
            host_copy = wait_for(lambda: next((r for i, r in shared(0).items() if i not in before0 and r["copy"]), None), 10)
            check(bool(host_copy) and host_copy["offer"], "the client's spawned Ursopod got a copy in the host's world", str(host_copy))
            paired = wait_for(lambda: next((r for i, r in shared(1).items() if i not in before1 and r["actor"] == mine), None), 6)
            check(bool(paired) and not paired["copy"], "the client's own one is paired with it", str(paired))
            if host_copy:
                d = safe(lambda: dist(loc(0, host_copy["actor"]), loc(1, mine)), 99)
                check(d < 6, "next to the client's", f"{d:.1f} m apart")
                check(len(near_of_class(1, short, p)) == 1, "one there in the client's world: its own (no copy of it)",
                      f"{len(near_of_class(1, short, p))} living")
                uc(0, host_copy["actor"] + ".HealthComponent", "Suicide", {"bPreventRevival": True})
                died = wait_for(lambda: health(1, mine) <= 0, 6)
                check(bool(died), "killed in the host's world, the client's died too", f"health {safe(lambda: health(1, mine))}")
        if "D" in parts:
            p = place(180)
            before = set(shared(1))
            mine = spawn(0, cls, p, yaw)
            rec = wait_for(lambda: next((r for i, r in shared(1).items() if i not in before and r["actor"] != "none"), None), 8)
            check(bool(rec) and rec["copy"], "a copy in the client's world")
            uc(0, mine, "K2_DestroyActor")
            gone = wait_for(lambda: rec and rec["id"] not in shared(1) and not near_of_class(1, short, p), 10) if rec else None
            check(bool(gone), "the host's one removed: the client's copy went too")
    finally:
        for inst, ref in MADE:  # nothing left fighting the players
            safe(lambda: uc(inst, ref + ".HealthComponent", "Suicide", {"bPreventRevival": True}))
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "ABCD"))
