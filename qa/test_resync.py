"""Resync checks on a running pair in gameplay (copy 0 host, copy 1 client, same campaign state).
A. Quest outcome: the host starts a quest (synced live), then drives it to an end state with DebugAdvanceQuest; the
   client cannot apply an end state directly, asks for a resync, reloads the host's save and ends in the same state.
B. Loading transition: the host travels to another map (TravelManager.BeginTravelToBP, the game's own travel); the
   client starts the same travel by itself and arrives within seconds, without a save hand-over. B2 also travels back
   with following switched off on the client: the host hands its campaign over (the fallback).
C. The client travels first (a door on its side): the host follows by itself, no hand-over.
S. The story both ways: the client starts a quest and drives it to an end state; the host's campaign follows.
Usage: python -I test_resync.py [A|B|AB|B2|AB2|BC]"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

Q = "/Game/Exported/BaseGame/Design/Quests/quests.quests#QuestBundles"
DEST = "/Game/Maps/TravelDestinationData/0601_Cloister/0605_OAHQHardLoad.0605_OAHQHardLoad"
DEST_BACK = "/Game/Maps/POI_Data/06_Cloister/0605_OAHQ_ExitHardLoad.0605_OAHQ_ExitHardLoad"  # out of the OA headquarters


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
            # The client applies the outcome itself (the quest's own state): no hand-over, no reload.
            w1, r0 = safe(lambda: call("eval", {"expr": "world"}, inst=1)["value"].split()[0]), safe(lambda: call("coop.status", {}, inst=0)["resyncs"], 0)
            live = wait(lambda: state(1, gid) == end, 20)
            time.sleep(8)  # the client's 5 s check that its quest really is in that state
            same = safe(lambda: call("eval", {"expr": "world"}, inst=1)["value"].split()[0]) == w1 and safe(lambda: call("coop.status", {}, inst=0)["resyncs"], 0) == r0
            check(live and same, "client reached the same end state live (no reload)", f"client {safe(lambda: state(1, gid))}, world kept {same}")
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
    if "S" in which:  # the story both ways: the client's own quest start and outcome reach the host's campaign
        n = len(call("eval", {"expr": Q}, inst=1)["value"])
        names = [call("eval", {"expr": f"{Q}[{i}].Name"}, inst=1)["value"] for i in range(n)]
        gid, name = None, None
        for i in sorted(range(n), key=lambda i: not names[i].startswith("06")):
            g = call("eval", {"expr": f"{Q}[{i}].ID"}, inst=1)["value"]
            if state(0, g) == "Invalid" and state(1, g) == "Invalid":
                call("ue.call", {"ref": "QuestScripts", "fn": "StartQuest", "args": {"QuestID": g}}, inst=1)
                time.sleep(0.5)
                if state(1, g) == "InProgress":
                    gid, name = g, names[i]
                    break
        check(gid is not None, "the client started a fresh quest", str(name))
        if gid:
            check(wait(lambda: state(0, gid) == "InProgress", 15), "the host's campaign started it too", safe(lambda: state(0, gid)))
            w0, r0 = safe(lambda: call("eval", {"expr": "world"}, inst=0)["value"].split()[0]), call("coop.status", {}, inst=0)["resyncs"]
            xp0 = (call("coop.status", {}, inst=0).get("xp"), call("coop.status", {}, inst=1).get("xp"))
            for _ in range(10):
                if state(1, gid) in ("Succeeded", "Failed"):
                    break
                call("ue.call", {"ref": "QuestScripts", "fn": "DebugAdvanceQuest", "args": {"QuestID": gid}}, inst=1)
                time.sleep(1.0)
            time.sleep(3)
            xp1 = (call("coop.status", {}, inst=0).get("xp"), call("coop.status", {}, inst=1).get("xp"))
            print(f"XP while the client drove its quest: host {xp0[0]} -> {xp1[0]}, client {xp0[1]} -> {xp1[1]}", flush=True)
            end = state(1, gid)
            check(end in ("Succeeded", "Failed"), "the client drove it to an end state", end)
            ok = wait(lambda: state(0, gid) == end, 20)
            same = safe(lambda: call("eval", {"expr": "world"}, inst=0)["value"].split()[0]) == w0 and call("coop.status", {}, inst=0)["resyncs"] == r0
            check(ok and same, "the host's campaign has the same outcome, without a reload", f"host {safe(lambda: state(0, gid))}, world kept {same}")
            if end == "Succeeded":  # the quest's XP: the client's game gave it (its own is dropped), the host takes it and shares it
                got = wait(lambda: (call("coop.status", {}, inst=0).get("xp") or 0) > (xp0[0] or 0)
                           and (call("coop.status", {}, inst=1).get("xp") or 0) > (xp0[1] or 0), 15)
                s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
                check(bool(got), "the quest's XP reached both players",
                      f"host {xp0[0]} -> {s0.get('xp')} (credited {s0.get('questXpCredited')}), client {xp0[1]} -> {s1.get('xp')} "
                      f"(its own dropped {s1.get('xpBlocked')}); {s0.get('lastXp')}")
    if "B" in which:
        # B1: the client starts the same travel as the host (no hand-over). B2 (following off on the client): the
        # fallback, the host hands its campaign over once in the new map.
        travel(check, follow=True)
        if "2" in which:
            travel(check, follow=False)
    if "C" in which:  # the client goes first (a door on its side): the host follows, no hand-over
        travel(check, follow=True, leader=1)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


def map_of(inst):
    return world(inst).split("/")[-1].split(".")[0]


def both_puppets():
    return call("coop.status", {}, inst=0)["puppet"] != "none" and call("coop.status", {}, inst=1)["puppet"] != "none"


def travel(check, follow, leader=0):
    """The leader travels (to 0605, or back to 0601 when already there). With follow, the other player starts the same
    travel by itself, arrives within seconds and no save is handed over; without, the host hands its campaign over."""
    other = 1 - leader
    w0 = world(0)
    dest, want = (DEST_BACK, "0601") if "0605" in w0 else (DEST, "0605")  # the map each destination leads to
    call("coop.set", {"follow": follow}, inst=other)
    tm = call("ue.find", {"class": "TravelManager"}, inst=leader)[0].split()[0]
    d = call("ue.find", {"path": dest}, inst=leader)
    if not d:  # streamed out (the players are elsewhere in the map): load it, as the mod does for the follower
        lib = call("ue.find", {"path": "/Script/Engine.Default__KismetSystemLibrary"}, inst=leader)[0].split()[0]
        safe(lambda: call("ue.call", {"ref": lib, "fn": "LoadAsset_Blocking", "args": {"Asset": dest}}, inst=leader))
        d = call("ue.find", {"path": dest}, inst=leader)
    check(bool(d), "travel destination loaded", dest.split(".")[-1])
    if not d:
        return
    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=other)
    before, followed, refused = s0["resyncs"], s1.get("travelsFollowed", 0), s1.get("travelsRefused", 0)
    t_start = time.time()
    print("travel:", safe(lambda: call("ue.call", {"ref": tm, "fn": "BeginTravelToBP", "args": {"Destination": d[0].split()[0],
          "bPassGametime": False, "bTriggerAutosave": False, "bForceBlocking": False}, "timeoutMs": 240000}, inst=leader, timeout_s=250)), flush=True)
    seen = {}
    while time.time() - t_start < 240 and len(seen) < 2:  # both arrival times (new world object), polled together
        for i in (0, 1):
            if i not in seen and map_of(i).startswith(want):
                seen[i] = time.time()
        time.sleep(0.25)
    check(0 in seen, "host arrived in the new map", world(0))
    check(1 in seen, "client arrived in the new map", world(1))
    lag = seen[other] - seen[leader] if len(seen) == 2 else 999
    names = ("host", "client")
    print(f"{names[other]} in the new map {lag:.1f} s after the {names[leader]} ({names[leader]}: {seen.get(leader, t_start) - t_start:.1f} s after its travel started)", flush=True)
    s1 = call("coop.status", {}, inst=other)
    if follow:
        check(s1.get("travelsFollowed", 0) > followed, f"the {names[other]} followed the {names[leader]}'s travel by itself", s1.get("lastTravel"))
        check(lag < 8, f"{names[other]} in the new map within 8 s of the {names[leader]}", f"{lag:.1f} s")
    else:
        check(s1.get("travelsRefused", 0) > refused, f"the {names[other]} did not follow (test switch)", s1.get("lastTravel"))
        pushed = wait(lambda: call("coop.status", {}, inst=0)["resyncs"] > before, 60)
        check(pushed, "the host handed its campaign over instead", f"resyncs {before} -> {call('coop.status', {}, inst=0)['resyncs']}")
    call("coop.set", {"follow": True}, inst=other)
    both = wait(both_puppets, 120)
    t_see = time.time()
    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    check(both, "both see each other in the new map", f"{s0['puppet']} / {s1['puppet']}, {t_see - max(seen.values(), default=t_see):.0f} s after both arrived")
    if follow:
        time.sleep(5)
        s0 = call("coop.status", {}, inst=0)
        check(s0["resyncs"] == before, "no hand-over (no save sent)", f"host resyncs {before} -> {s0['resyncs']} ({s0['resyncReason']})")
    hp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
    cp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"]
    dist = ((hp["X"] - cp["X"]) ** 2 + (hp["Y"] - cp["Y"]) ** 2) ** 0.5
    check(dist < 1500, "the client stands near the host", f"{dist:.0f} units apart")
    print("from", w0, "host at", hp, "client at", cp, flush=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "AB"))
