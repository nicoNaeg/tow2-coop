"""A large fight against armed humans, on a running pair in gameplay in 0601_CL_P (the test save): the Protectorate
squad (assault rifles, revolvers, a drone; ~9, neutral until attacked). The host attacks one of them, as a player
picking a fight would; then for 40 s:
- the squad fights in both worlds, each soldier on the target the deciding world gives (the players or the stand-ins);
- both players take hits in their own world (pseudo god mode: hit, not killed);
- each fighting soldier's two copies stay close;
- the client's own hits reach the host's copy;
then the host kills the squad: dead in both worlds, nothing left fighting. No caught fault meanwhile.
Usage: python -I test_bigfight.py"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_combat import dist, fights, health, loc, players, teleport, uc  # noqa: E402
from tow2 import call  # noqa: E402

SQUAD = {"X": -87000.0, "Y": 64800.0}  # 0601_CL_P: PR_T3 soldiers (probe 121), around Z -700..-150
FAILS = 0


def check(ok, what, detail=""):
    global FAILS
    print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
    FAILS += not ok
    return ok


def safe(f, d=None):
    try:
        return f()
    except Exception:  # noqa: BLE001
        return d


def squad():
    """The living Protectorate soldiers within 40 m of the squad's place: [(name, host ref, client ref)]."""
    out = []
    for c in call("ue.find", {"class": "Character", "limit": 900}, inst=0):
        a, cls, path = c.split(" ", 2)
        if not cls.startswith("PR_T") or "_UAID_" not in path or safe(lambda: health(0, a), 0) <= 0:
            continue
        if safe(lambda: call("eval", {"expr": a + ".bHidden"}, inst=0)["value"], True):
            continue  # one waiting hidden for another encounter's wave: not in this fight (it can't be hit or killed)
        p = safe(lambda: loc(0, a))
        if not p or math.dist((p["X"], p["Y"]), (SQUAD["X"], SQUAD["Y"])) > 4000:
            continue
        b = call("ue.find", {"path": path}, inst=1)
        if b:
            out.append((path.split(".")[-1], a, b[0].split()[0]))
    return out


def strike(inst, target, n=4):
    """The player swings its blade at the target from 1.6 m (as test_spawned.swings)."""
    b, me = loc(inst, target), loc(inst, "pawn")
    a = math.atan2(me["Y"] - b["Y"], me["X"] - b["X"])
    spot = {"X": b["X"] + 160 * math.cos(a), "Y": b["Y"] + 160 * math.sin(a), "Z": b["Z"] + 20}
    teleport(inst, spot, b)
    uc(inst, "pawn.EquipmentComponent", "EquipWeaponFromSlot", {"SlotIndex": 0, "bInstantEquip": True, "bStartingCombat": False})
    time.sleep(0.5)
    w = call("eval", {"expr": "pawn.GetCurrentWeapon()"}, inst=inst)["value"].split()[0]
    for _ in range(n):
        uc(inst, w, "BeginPrimaryTrigger")
        time.sleep(0.12)
        uc(inst, w, "EndPrimaryTrigger", {"bForceEnd": False})
        time.sleep(0.9)


def main():
    start = [loc(i, "pawn") for i in (0, 1)]
    errors0 = [call("coop.status", {}, inst=i).get("frameErrors", 0) for i in (0, 1)]
    # the squad's area streamed in for both
    teleport(0, {"X": SQUAD["X"] - 2500, "Y": SQUAD["Y"], "Z": 1200.0})
    teleport(1, {"X": SQUAD["X"] - 2700, "Y": SQUAD["Y"], "Z": 1200.0})
    time.sleep(5)
    sq = squad()
    if not check(len(sq) >= 5, "the Protectorate squad is there in both worlds", f"{len(sq)}: {', '.join(n[:28] for n, _, _ in sq)}"):
        return 1
    players("test")
    try:
        near = loc(0, sq[0][1])
        stand = {"X": near["X"] - 1200, "Y": near["Y"], "Z": near["Z"] + 150}
        teleport(0, stand, near)
        teleport(1, {"X": stand["X"], "Y": stand["Y"] - 300, "Z": stand["Z"]}, near)
        time.sleep(2)
        hp0 = [health(i) for i in (0, 1)]
        strike(0, sq[0][1])  # the host picks the fight
        teleport(0, stand, near)
        names = {n for n, _, _ in sq}
        fought_h = fought_c = 0
        hit = [False, False]
        apart = []
        for _ in range(20):  # 40 s
            time.sleep(2)
            fh = {k: v for k, v in fights(0).items() if k in names}
            fought_h = max(fought_h, sum(1 for v in fh.values() if v.get("word") in (1, 2)))
            fc = {k: v for k, v in fights(1).items() if k in names}
            fought_c = max(fought_c, sum(1 for v in fc.values() if v.get("here") in (1, 2) or v.get("word") in (1, 2)))
            for i in (0, 1):
                hit[i] = hit[i] or safe(lambda i=i: health(i), hp0[i]) < hp0[i] - 1
            for n, a, b in sq:
                if n in fh and fh[n].get("word") in (1, 2):
                    d = safe(lambda a=a, b=b: dist(loc(0, a), loc(1, b)))
                    if d is not None and safe(lambda a=a: health(0, a), 0) > 0:
                        apart.append(d)
        check(fought_h >= 3, "the squad fights the players in the host's world", f"up to {fought_h} at once")
        check(fought_c >= 3, "and in the client's world", f"up to {fought_c} at once")
        check(hit[0] and hit[1], "both players took hits in their own world", f"host {hit[0]}, client {hit[1]}")
        within = sum(1 for d in apart if d < 6)
        check(apart and within >= 0.9 * len(apart), "each fighting soldier's two copies stay within 6 m",
              f"{within}/{len(apart)} samples, max {max(apart) if apart else 0:.1f} m")
        alive = [(n, a, b) for n, a, b in sq if safe(lambda a=a: health(0, a), 0) > 0]
        if alive:
            n, a, b = alive[0]
            h0 = health(0, a)
            got = False
            for _ in range(3):  # a swing can miss (the soldier moves): again, as a player would
                strike(1, b)
                t0 = time.time()
                while time.time() - t0 < 4 and not got:
                    got = safe(lambda: health(0, a), h0) < h0 - 5
                    time.sleep(0.25)
                if got:
                    break
            check(got, "the client's own hits on a soldier reached the host's copy", f"{n[:30]}: {h0:.0f} -> {safe(lambda: health(0, a)):.0f}")
        for n, a, b in sq:
            safe(lambda a=a: uc(0, a + ".HealthComponent", "Suicide", {"bPreventRevival": True}))
        t0 = time.time()
        while time.time() - t0 < 8 and any(safe(lambda b=b: health(1, b), 0) > 0 for _, _, b in sq):
            time.sleep(0.5)
        left = [n for n, _, b in sq if safe(lambda b=b: health(1, b), 0) > 0]
        check(not left, "the host killed the squad: dead in the client's world too", str(left))
        time.sleep(3)
        still = [k for k, v in fights(1).items() if k in names and v.get("pinned")]
        check(not still, "nothing of the squad left pinned on a player", str(still))
        errors = [call("coop.status", {}, inst=i).get("frameErrors", 0) for i in (0, 1)]
        check(errors == errors0, "no caught fault in either game during the fight", f"{errors0} -> {errors}")
    finally:
        players("normal")
        for i in (0, 1):
            safe(lambda i=i: teleport(i, start[i]))
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
