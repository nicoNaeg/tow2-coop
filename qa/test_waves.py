"""The game's own spawners (hives, hatches), on a running pair in gameplay in 0601_CL_P: an encounter wave started in
one world is started in the other. The test starts a wave in one world only (the game's Encounter.ActivateWave, as a
player's approach or the story would) and checks the other world's wave starts and its enemies come out of their
hives there too (probe 116: they stayed hidden for a minute); then a death in the host's world is one in the client's.
Both directions: a wave the host's world starts, then one the client's starts.
Usage: python -I test_waves.py"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

SPOT = {"X": -81600.0, "Y": 41000.0, "Z": 1300.0}  # 0601_CL_P, the mantisaur nest (hives and floor hatches)


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def wait(pred, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(0.5)
    return safe(pred, False)


def tp(i, p):
    call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": p, "bSweep": False, "bTeleport": True}}, inst=i)


def encounters(i):  # name -> ref
    return {r.split()[-1].split(".")[-1]: r.split()[0] for r in call("ue.find", {"class": "/Script/Arkansas.Encounter", "limit": 400}, inst=i) if "Default__" not in r}


def waves(i, ref):  # [(state, [character refs], all of them waiting in a spawner)]
    out = []
    for w in call("eval", {"expr": ref + ".EncounterWaves"}, inst=i)["value"] or []:
        pairs = [p for p in (w.get("AICharacters") or []) if p.get("Character")]
        out.append((w.get("State"), [p["Character"].split()[0] for p in pairs],
                    bool(pairs) and all(str(p.get("Spawner") or "None").split()[0] not in ("None", "None.None") for p in pairs)))
    return out


def out(i, ch):  # the character is out of its spawner: shown
    return not call("eval", {"expr": ch + ".bHidden"}, inst=i)["value"]


def health(i, ch):
    return call("ue.call", {"ref": ch + ".HealthComponent", "fn": "GetHealth"}, inst=i)["ReturnValue"]


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok
        return ok

    start = [call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=i)["value"] for i in (0, 1)]
    if not check("0601_CL_P" in str(call("eval", {"expr": "world"}, inst=0)["value"]), "the host is in 0601_CL_P"):
        return 1
    tp(0, SPOT)
    time.sleep(0.5)
    tp(1, {"X": SPOT["X"] - 200, "Y": SPOT["Y"], "Z": SPOT["Z"]})
    time.sleep(4)
    try:
        h, c = encounters(0), encounters(1)
        # waves not started in either world, all of whose enemies wait hidden and alive in both
        fresh = []
        for name in sorted(set(h) & set(c)):  # (a listed one may be going away with its level: skipped)
            loc = safe(lambda: call("eval", {"expr": h[name] + ".K2_GetActorLocation()"}, inst=0)["value"])
            if not loc or math.dist((loc["X"], loc["Y"]), (SPOT["X"], SPOT["Y"])) > 25000:
                continue
            wh, wc = safe(lambda: waves(0, h[name]), []), safe(lambda: waves(1, c[name]), [])
            for k, (a, b) in enumerate(zip(wh, wc)):
                if a[0] == b[0] == "Inactive" and a[2] and len(a[1]) == len(b[1]) and safe(lambda: all(
                        not out(0, x) and health(0, x) > 0 for x in a[1]) and all(not out(1, x) and health(1, x) > 0 for x in b[1]), False):
                    fresh.append((name, k))
        print(f"info: {len(fresh)} waves waiting in both worlds near the nest: {fresh[:4]}", flush=True)
        if not check(len(fresh) >= 2, "waves waiting in their spawners in both worlds near the nest", str(len(fresh))):
            return fails
        s0 = [call("coop.status", {}, inst=i) for i in (0, 1)]
        used = []
        for starter, other, who in ((0, 1, "host"), (1, 0, "client")):
            refs = (h, c)
            for name, k in fresh:  # a wave whose encounter is off (its conditions) does not start: the next one
                if (name, k) in used:
                    continue
                # both players next to the wave's spawners (NPCs are shared within 80 m of a player)
                at = call("eval", {"expr": waves(0, h[name])[k][1][0] + ".K2_GetActorLocation()"}, inst=0)["value"]
                tp(0, {"X": at["X"] - 1500, "Y": at["Y"], "Z": at["Z"] + 300})
                time.sleep(0.5)
                tp(1, {"X": at["X"] - 1700, "Y": at["Y"], "Z": at["Z"] + 300})
                time.sleep(3)
                if waves(0, h[name])[k][0] != "Inactive" or waves(1, c[name])[k][0] != "Inactive":
                    used.append((name, k))  # it started by itself as the players came: not this one
                    continue
                call("ue.call", {"ref": refs[starter][name], "fn": "ActivateWave", "args": {"WaveIndex": k}}, inst=starter)
                if wait(lambda: waves(starter, refs[starter][name])[k][0] != "Inactive", 2):
                    used.append((name, k))
                    break
                print(f"info: wave {k} of {name[-24:]} did not start (its encounter is off)", flush=True)
                used.append((name, k))
            else:
                check(False, f"a wave the {who}'s world can start")
                continue
            mine = waves(starter, refs[starter][name])[k]
            check(mine[0] != "Inactive" and wait(lambda: all(out(starter, x) for x in mine[1]), 8),
                  f"the {who}'s world started a wave of {name[-24:]}: its {len(mine[1])} enemies came out", mine[0])
            started = wait(lambda: waves(other, refs[other][name])[k][0] != "Inactive", 6)
            theirs = waves(other, refs[other][name])[k]
            check(started, "the other world started the same wave", f"{theirs[0]}, waves started there {call('coop.status', {}, inst=other).get('wavesStarted')}")
            check(wait(lambda: all(out(other, x) for x in theirs[1]), 8), "and its enemies came out of their spawners there too",
                  str([safe(lambda x=x: out(other, x)) for x in theirs[1]]))
        # a death in the host's world is one in the client's (the NPC registry, now that they are out)
        name, k = used[-1]
        hx, cx = waves(0, h[name])[k][1][0], waves(1, c[name])[k][1][0]
        call("ue.call", {"ref": hx + ".HealthComponent", "fn": "Suicide"}, inst=0)
        check(wait(lambda: health(1, cx) <= 0, 6), "an enemy of the wave killed in the host's world died in the client's", f"health {safe(lambda: health(1, cx))}")
        s1 = [call("coop.status", {}, inst=i) for i in (0, 1)]
        check(s1[1].get("wavesStarted", 0) > s0[1].get("wavesStarted", 0) and s1[0].get("wavesStarted", 0) > s0[0].get("wavesStarted", 0),
              "each world started one wave for the other", f"host {s1[0].get('wavesStarted')}, client {s1[1].get('wavesStarted')}")
        # the rest of the woken nest is put down (the steps after this one start calm)
        for i, refs in ((0, h),):
            for name2, k2 in used:
                for x in waves(i, refs[name2])[k2][1]:
                    safe(lambda x=x: call("ue.call", {"ref": x + ".HealthComponent", "fn": "Suicide"}, inst=i))
    finally:
        for i in (0, 1):
            safe(lambda i=i: tp(i, start[i]))
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
