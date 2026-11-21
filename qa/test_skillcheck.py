"""A real skill check in a shared conversation, on a running pair in gameplay (the test save): both travel to the Order's
headquarters (0605_OAHQ); the host's skills are set high and the client's to 0 (the game's SetPlayerSkillDebug); the
host talks to the light worker (0605_LightWorker) and walks the conversation until a response is open on the host's
screen and locked on the client's (probe 122: "An electrical short ... I could take a look", node 18). The host picks
it: the client's game takes it too, past its own lock, without starting the conversation again (no camera cut).
The skills are put back at the end. Usage: python -I test_skillcheck.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from test_conversation import active, conv, lib, node, pid, responses, screen, status, uc, wait, safe  # noqa: E402
from tow2 import call  # noqa: E402

DEST = "/Game/Maps/TravelDestinationData/0601_Cloister/0605_OAHQHardLoad.0605_OAHQHardLoad"
SKILLS = ["Melee", "Guns", "Sneak", "Lockpick", "Engineering", "Explosives", "Hack", "Medical", "Science", "Observation", "Speech",
          "Leadership", "Persuade", "Lie", "Intimidate"]
LEAVE = ("Leave", "Quitter", "later", "Goodbye", "bye", "Bye", "[Open", "Vendor", "[Trade", "Never mind")
FAILS = 0


def check(ok, what, detail=""):
    global FAILS
    print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
    FAILS += not ok
    return ok


def world(i):
    return str(call("eval", {"expr": "world"}, inst=i)["value"])


def info(i):
    return [x for x in call("ue.find", {"class": "/Script/Arkansas.PlayerInfoComponent", "limit": 5}, inst=i) if "Default__" not in x][0].split()[0]


def skill(i, name):
    lib_ = call("ue.find", {"path": "/Script/Arkansas.Default__CharacterLibrary"}, inst=i)[0].split()[0]
    return uc(i, lib_, "GetSkillValue", {"Actor": call("eval", {"expr": "pawn"}, inst=i)["value"].split()[0], "Skill": name})["ReturnValue"]


def shown(i):
    return [tuple(e[:2]) for e in status(i).get("convShown", [])]


def main():
    if "0605_OAHQ" not in world(0):
        tm = call("ue.find", {"class": "TravelManager"}, inst=0)[0].split()[0]
        d = call("ue.find", {"path": DEST}, inst=0)
        if not d:
            call("ue.call", {"ref": "/Script/Engine.Default__KismetSystemLibrary", "fn": "LoadAsset_Blocking", "args": {"Asset": DEST}}, inst=0)
            d = call("ue.find", {"path": DEST}, inst=0)
        uc(0, tm, "BeginTravelToBP", {"Destination": d[0].split()[0], "bPassGametime": False, "bTriggerAutosave": False, "bForceBlocking": False})
        t0 = time.time()
        while time.time() - t0 < 120 and not all("0605_OAHQ" in safe(lambda i=i: world(i), "") for i in (0, 1)):
            time.sleep(2)
        time.sleep(8)
    if not check(all("0605_OAHQ" in world(i) for i in (0, 1)), "both are in the Order's headquarters"):
        return 1
    was = {i: {s: skill(i, s) for s in SKILLS} for i in (0, 1)}
    try:
        for i, v in ((0, 20), (1, 0)):
            ci = info(i)
            for s in SKILLS:
                uc(i, ci, "SetPlayerSkillDebug", {"Skill": s, "SkillValue": v})
        check(skill(0, "Engineering") >= 20 and skill(1, "Engineering") == 0, "the host's skills high, the client's 0",
              f"Engineering {skill(0, 'Engineering')} / {skill(1, 'Engineering')}")
        worker = [r for r in call("ue.find", {"class": "Character", "limit": 900}, inst=0) if r.split()[1] == "0605_LightWorker_C"]
        if not check(bool(worker), "the light worker is there"):
            return 1
        a = worker[0].split()[0]
        p = call("eval", {"expr": a + ".K2_GetActorLocation()"}, inst=0)["value"]
        uc(0, "pawn", "K2_SetActorLocation", {"NewLocation": {"X": p["X"] - 200, "Y": p["Y"], "Z": p["Z"] + 50}, "bSweep": False, "bTeleport": True})
        for i in (0, 1):
            safe(lambda i=i: Game(pid(i)).key("W", hold_s=0.05))
        time.sleep(1.5)
        r = uc(0, lib(0), "StartConversationWithActor", {"Speaker": a, "StartNode": -1, "Stage": None, "bAllowBarkConversationDuringCombat": False})
        if not check(r.get("Result") == "OnTrue" and wait(lambda: status(1).get("convFollow") and active(1), 10), "the host talks to him, the client follows"):
            return 1
        lock = None
        for step in range(7):
            for _ in range(30):  # the lines play; the responses come (on both screens)
                if (shown(0) and shown(1)) or not active(0):
                    break
                safe(lambda: uc(0, screen(0), "OnSkipInputAction"))
                time.sleep(0.5)
            time.sleep(1.5)
            h, c = shown(0), shown(1)
            locked = [n for n, s in h if s and (n, False) in c]
            if locked:
                lock = locked[0]
                break
            opts = [k for k, t in enumerate(responses(0)) if not any(w in t for w in LEAVE)]
            if not opts or not active(0):
                break
            before = node(0)
            uc(0, screen(0), f"OnResponse{opts[0] + 1}InputAction")
            wait(lambda: node(0) != before, 4)
        if not check(lock is not None, "a response open to the host and locked for the client (a real skill check)", f"{shown(0)} / {shown(1)}"):
            return 1
        s0, h0 = status(1), status(0)
        raw = h0.get("convShown")  # [node, open, the line it answers]
        k = [n for n, s in shown(0) if s].index(lock) + 1
        text = responses(0)[k - 1]
        before = node(0)
        for _ in range(6):
            uc(0, screen(0), f"OnResponse{k}InputAction")
            if wait(lambda: node(0) != before, 1.0):
                break
        after = node(0)
        took = wait(lambda: status(1).get("convBypassed", 0) > s0.get("convBypassed", 0)
                    or status(1).get("convPicksApplied", 0) > s0.get("convPicksApplied", 0), 6)
        time.sleep(2)
        s1, h1 = status(1), status(0)
        # the leader's pick is told by the line it led to: when another open response leads to the same line, the client
        # may take that one (the same line, no camera cut; run 177), else it takes the locked one past its lock
        check(took, f"the client took a response to the host's next line ({text[:50]!r})",
              f"past the lock {s0.get('convBypassed')} -> {s1.get('convBypassed')}, an open one {s0.get('convPicksApplied')} -> {s1.get('convPicksApplied')}"
              f" (picks the host told {h0.get('convPicksSent')} -> {h1.get('convPicksSent')}, the client's own steps {s0.get('convAdvances')} -> {s1.get('convAdvances')};"
              f" the host's line {before} -> {after}, shown {raw}, the host's own input dropped {h0.get('convBlocked')} -> {h1.get('convBlocked')})")
        check(s1.get("convJumps", 0) == s0.get("convJumps", 0), "the same response, no restart (no camera cut)",
              f"restarts {s0.get('convJumps')} -> {s1.get('convJumps')}")
        check(wait(lambda: node(1) == node(0) or (not active(0) and not active(1)), 8), "both at the host's next line",
              f"{safe(lambda: node(0))} / {safe(lambda: node(1))}")
    finally:
        if active(0):
            safe(lambda: uc(0, lib(0), "EndConversationInstance", {"Instance": conv(0)}))
        for i in (0, 1):
            ci = safe(lambda i=i: info(i))
            for s, v in was[i].items():
                safe(lambda ci=ci, s=s, v=v: uc(i, ci, "SetPlayerSkillDebug", {"Skill": s, "SkillValue": v}))
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
