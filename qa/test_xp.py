"""Party XP (end goal 2, own progression) on a joined pair in gameplay (copy 0 host, copy 1 client). Every XP gain goes
through the game's AddXP, which the mod hooks; coop.xp makes one the way gameplay does. A kill, a lock pick or a
conversation earns XP for both players; quest XP comes from the host's campaign only (the client's own quest XP is
dropped); a discovery stays with the player who made it.
Usage: python -I test_xp.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

# (who earns, EXPGainReason, what, host gains, client gains)
CASES = [
    (0, 0, "the host killed an enemy", True, True),
    (0, 6, "the host passed a conversation check", True, True),
    (0, 1, "the host completed a quest", True, True),
    (0, 3, "the host discovered a place", True, False),
    (1, 0, "the client killed an enemy", True, True),
    (1, 5, "the client picked a lock", True, True),
    (1, 2, "the client's own quest objective (its quest graph follows the host's)", False, False),
    (1, 3, "the client discovered a place", False, True),
]


def progress(inst):  # (level, XP): a gain may also be a level-up
    lvl = call("ue.call", {"ref": "pawn.PlayerInfoComponent", "fn": "GetLevel"}, inst=inst)["ReturnValue"]
    return (lvl, call("coop.status", {}, inst=inst).get("xp", -1))


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    st = [call("coop.status", {}, inst=i) for i in (0, 1)]
    check(st[0].get("xpHook") and st[1].get("xpHook"), "the game's AddXP is hooked in both copies")
    check(st[0].get("xp", -1) >= 0 and st[1].get("xp", -1) >= 0, "both players' XP is readable", f"host {st[0].get('xp')}, client {st[1].get('xp')}")
    for who, reason, what, host_gains, client_gains in CASES:
        before = [progress(0), progress(1)]
        r = call("coop.xp", {"xp": 20, "reason": reason}, inst=who)
        want = [host_gains, client_gains]
        t0 = time.time()
        while time.time() - t0 < 8:
            now = [progress(0), progress(1)]
            if all(now[i] > before[i] for i in (0, 1) if want[i]):
                break
            time.sleep(0.5)
        time.sleep(2)  # what must not change has had time to
        now = [progress(0), progress(1)]
        got = [now[i] > before[i] for i in (0, 1)]
        detail = f"host {before[0]} -> {now[0]}, client {before[1]} -> {now[1]}; local gain {r.get('gained')}; " \
                 f"host: {call('coop.status', {}, inst=0).get('lastXp')!r}, client: {call('coop.status', {}, inst=1).get('lastXp')!r}"
        expect = ("both gain XP" if all(want) else "only the host gains XP" if want[0] else "only the client gains XP" if want[1] else "nobody gains XP")
        check(got == want, f"{what}: {expect}", detail)
    st = [call("coop.status", {}, inst=i) for i in (0, 1)]
    print("xp counters: host sent %s applied %s; client sent %s applied %s blocked %s; details filled %s/%s" % (
        st[0].get("xpSent"), st[0].get("xpApplied"), st[1].get("xpSent"), st[1].get("xpApplied"), st[1].get("xpBlocked"),
        st[0].get("xpDetails"), st[1].get("xpDetails")))
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
