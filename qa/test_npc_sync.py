"""Host-authority checks for NPC health and death on a running pair (copy 0 host, copy 1 client, same save loaded).

1. host damages an NPC  -> the client's copy of that NPC gets the same health
2. client damages an NPC -> the damage reaches the host and comes back as the authoritative health
3. host kills an NPC     -> the client's copy dies too
Picks NPCs from the client's registry that exist in both worlds. Prints PASS/FAIL lines; exit code = failures."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def health(inst, actor):
    return call("ue.call", {"ref": actor + ".HealthComponent", "fn": "GetHealth"}, inst=inst)["ReturnValue"]


def set_health(inst, actor, hp):
    call("ue.call", {"ref": actor + ".HealthComponent", "fn": "SetHealth", "args": {"Health": hp}}, inst=inst)


def actor_by_path(inst, path):
    r = call("ue.find", {"path": path}, inst=inst)
    return r[0].split()[0] if r else None


def wait(pred, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = pred()
        if v:
            return v
        time.sleep(0.2)
    return pred()


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    check(s0["npcs"] > 0 and s1["npcs"] > 0 and s0["npcSent"] > 0 and s1["npcApplied"] > 0, "registries and NPC stream",
          f"host npcs {s0['npcs']} sent {s0['npcSent']}, client npcs {s1['npcs']} applied {s1['npcApplied']}")
    # NPCs the host streams (within 80 m of a player), with health on both sides, nearest first
    me = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
    near = []
    for c in call("ue.find", {"class": "Character", "limit": 400}, inst=0):
        a, cls, path = c.split(" ", 2)
        if "_UAID_" not in path:
            continue
        p = call("eval", {"expr": a + ".K2_GetActorLocation()"}, inst=0)["value"]
        d = ((p["X"] - me["X"]) ** 2 + (p["Y"] - me["Y"]) ** 2 + (p["Z"] - me["Z"]) ** 2) ** 0.5
        if d < 7000:
            near.append((d, path, a))
    cands = []
    for d, path, a in sorted(near):
        b = actor_by_path(1, path)
        if b and health(0, a) > 20 and health(1, b) > 20:
            cands.append((path, a, b))
        if len(cands) >= 3:
            break
    print(f"NPCs within 70 m of the host: {len(near)}", flush=True)
    check(len(cands) >= 3, "three NPCs present in both worlds", str(len(cands)))
    if len(cands) < 3:
        return fails
    (p1, h1, c1), (p2, h2, c2), (p3, h3, c3) = cands[:3]
    # 1. host-side damage
    hp = health(0, h1)
    set_health(0, h1, hp * 0.5)
    got = wait(lambda: abs(health(1, c1) - hp * 0.5) < 1.0 and health(1, c1))
    check(bool(got), "host damage reaches the client", f"host {hp:.0f}->{hp*0.5:.0f}, client now {health(1, c1):.0f}  ({p1.split('.')[-1]})")
    # 2. client-side damage goes through the host
    hp2 = health(0, h2)
    set_health(1, c2, hp2 - 37)
    got = wait(lambda: abs(health(0, h2) - (hp2 - 37)) < 1.0)
    check(bool(got), "client damage applied on the host", f"host {hp2:.0f} -> {health(0, h2):.0f} (expected {hp2-37:.0f})")
    time.sleep(0.5)
    check(abs(health(1, c2) - health(0, h2)) < 1.0, "client agrees with the host afterwards", f"client {health(1, c2):.0f} host {health(0, h2):.0f}")
    # 3. host kill
    call("ue.call", {"ref": h3 + ".HealthComponent", "fn": "Suicide", "args": {"bPreventRevival": True}}, inst=0)
    got = wait(lambda: health(1, c3) <= 0, timeout=6)
    check(bool(got), "host kill kills the client's copy", f"client health {health(1, c3):.0f}  ({p3.split('.')[-1]})")
    s1 = call("coop.status", {}, inst=1)
    print(f"client counters: applied {s1['npcApplied']} killed {s1['npcKilled']} snapped {s1['npcSnapped']} dmgSent {s1['dmgSent']}; "
          f"host dmgApplied {call('coop.status', {}, inst=0)['dmgApplied']}")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
