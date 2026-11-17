"""Resync checks on a running pair in gameplay (copy 0 host, copy 1 client, same campaign state).
A. Quest outcome: the host starts a quest (synced live), then drives it to an end state with DebugAdvanceQuest; the
   client cannot apply an end state directly, asks for a resync, reloads the host's save and ends in the same state.
B. Loading transition: the host travels to another map (TravelManager.BeginTravelToBP, the game's own travel);
   the host sees the client in the old map, hands its campaign over, and the client follows into the new map.
Usage: python -I test_resync.py [A|B|AB]"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

Q = "/Game/Exported/BaseGame/Design/Quests/quests.quests#QuestBundles"
DEST = "/Game/Maps/TravelDestinationData/0601_Cloister/0605_OAHQHardLoad.0605_OAHQHardLoad"


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def state(inst, gid):
    qm = call("ue.find", {"class": "OwQuestManager"}, inst=inst)[0].split()[0]
    return call("ue.call", {"ref": qm, "fn": "GetQuestState_BP", "args": {"QuestID": gid}}, inst=inst, timeout_s=15)["ReturnValue"]


def world(inst):
    return safe(lambda: call("eval", {"expr": "world"}, inst=inst, timeout_s=15)["value"].split()[-1], "loading")


def wait(pred, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(2)
    return safe(pred, False)


def in_combat(inst):
    return call("ue.call", {"ref": "pawn", "fn": "IsInCombat"}, inst=inst)["ReturnValue"]


def end_combat(inst=0, patience=60):
    """A hand-over needs a save, which the game refuses in combat; the bot host stands still (and can't die), so after
    the melee tests hostile NPCs may keep it in combat for minutes (runs 44-46, 52). Wait, then defeat the hostile ones."""
    if wait(lambda: not in_combat(inst), patience):
        return True
    lib = call("ue.find", {"path": "/Script/Arkansas.Default__CharacterBlueprintFunctionLibrary"}, inst=inst)[0].split()[0]
    pawn = call("eval", {"expr": "pawn"}, inst=inst)["value"].split()[0]
    for c in call("ue.find", {"class": "Character", "limit": 400}, inst=inst):
        a = c.split()[0]
        att = safe(lambda: call("ue.call", {"ref": lib, "fn": "GetTeamAttitudeTowards", "args": {"Character": a, "Other": pawn}}, inst=inst)["ReturnValue"])
        if str(att).split(":")[-1] in ("Hostile", "2"):
            safe(lambda: call("ue.call", {"ref": a + ".HealthComponent", "fn": "Suicide"}, inst=inst))
    return wait(lambda: not in_combat(inst), 30)


def main(which):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    if "A" in which:
        hp = safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "GetHealth"}, inst=0)["ReturnValue"])
        check(hp is not None and hp > 0, "host is alive (a dead or downed player cannot save)", f"health {hp}")
        check(end_combat(), "host out of combat (the game refuses to save in combat)", f"in combat: {safe(lambda: in_combat(0))}")
        n = len(call("eval", {"expr": Q}, inst=0)["value"])
        names = [call("eval", {"expr": f"{Q}[{i}].Name"}, inst=0)["value"] for i in range(n)]
        gid, name = None, None
        for i in sorted(range(n), key=lambda i: not names[i].startswith("06")):
            g = call("eval", {"expr": f"{Q}[{i}].ID"}, inst=0)["value"]
            if state(0, g) == "Invalid" and state(1, g) == "Invalid":
                call("ue.call", {"ref": "QuestScripts", "fn": "StartQuest", "args": {"QuestID": g}}, inst=0)
                time.sleep(0.5)
                if state(0, g) == "InProgress":
                    gid, name = g, names[i]
                    break
        check(gid is not None, "host started a fresh quest", str(name))
        if gid:
            check(wait(lambda: state(1, gid) == "InProgress", 10), "client's copy started (live story sync)")
            for _ in range(10):
                if state(0, gid) in ("Succeeded", "Failed"):
                    break
                call("ue.call", {"ref": "QuestScripts", "fn": "DebugAdvanceQuest", "args": {"QuestID": gid}}, inst=0)
                time.sleep(1.0)
            end = state(0, gid)
            check(end in ("Succeeded", "Failed"), "host drove it to an end state", end)
            # The hand-over waits while the host can't save: a player would finish a fight first, so the bot does too.
            t0, fights, ok = time.time(), 0, False
            while time.time() - t0 < 240 and not ok:
                ok = safe(lambda: state(1, gid)) == end
                if not ok and safe(lambda: in_combat(0), False):
                    fights += 1
                    end_combat(patience=10)
                elif not ok:
                    time.sleep(2)
            s0, s1 = safe(lambda: call("coop.status", {}, inst=0), {}), safe(lambda: call("coop.status", {}, inst=1), {})
            check(ok, "client reached the same end state", f"client {safe(lambda: state(1, gid))} after {time.time() - t0:.0f} s; host resyncs "
                  f"{s0.get('resyncs')} (reason: {s0.get('resyncReason')}), client fetches {s1.get('resyncFetches')}; host fights ended: {fights}")
            wait(lambda: call("coop.status", {}, inst=0)["puppet"] != "none" and call("coop.status", {}, inst=1)["puppet"] != "none", 120)
            s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
            check(s0["puppet"] != "none" and s1["puppet"] != "none", "both see each other after the resync", f"{s0['puppet']} / {s1['puppet']}")
    if "B" in which:
        w0 = world(0)
        tm = call("ue.find", {"class": "TravelManager"}, inst=0)[0].split()[0]
        d = call("ue.find", {"path": DEST}, inst=0)
        check(bool(d), "travel destination loaded", DEST.split(".")[-1])
        print("travel:", safe(lambda: call("ue.call", {"ref": tm, "fn": "BeginTravelToBP", "args": {"Destination": d[0].split()[0],
              "bPassGametime": False, "bTriggerAutosave": False, "bForceBlocking": False}, "timeoutMs": 240000}, inst=0, timeout_s=250)), flush=True)
        before = safe(lambda: call("coop.status", {}, inst=0)["resyncs"], 0)
        check(wait(lambda: "0605" in world(0), 240), "host arrived in the new map", world(0))
        t_host = time.time()
        # Two-PC test: the hand-over started ~15 s after the host's arrival (map settle + 5 s of "client elsewhere").
        pushed = wait(lambda: safe(lambda: call("coop.status", {}, inst=0)["resyncs"], before) > before, 60)
        check(pushed and time.time() - t_host < 10, "host handed the campaign over right after arriving", f"after {time.time() - t_host:.1f} s")
        check(wait(lambda: "0605" in world(1), 300), "client followed into the new map", world(1))
        print(f"client in the new map {time.time() - t_host:.0f} s after the host", flush=True)
        wait(lambda: call("coop.status", {}, inst=0)["puppet"] != "none" and call("coop.status", {}, inst=1)["puppet"] != "none", 120)
        s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
        check(s0["puppet"] != "none" and s1["puppet"] != "none", "both see each other in the new map",
              f"{s0['puppet']} / {s1['puppet']}; host resyncs {s0['resyncs']} ({s0['resyncReason']})")
        hp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
        cp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"]
        print("from", w0, "host at", hp, "client at", cp)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "AB"))
