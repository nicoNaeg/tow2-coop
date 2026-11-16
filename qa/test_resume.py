"""Co-op resume across a full restart. Phase "save" (pair in gameplay): start a fresh quest and open a door on the host
(both reach the client), then the host saves "CoopResume" and the facts are written to a JSON file. Phase "check"
(after both games were restarted, the host loaded "CoopResume", the client auto-joined): the host and the client both
have the quest in progress, the door open and stand where the host saved.
Phase "stray": the host copy alone loaded "CoopResume"; no stand-in body stands where it stood at the save.
Usage: python -I test_resume.py save|check|stray <facts.json>"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

Q = "/Game/Exported/BaseGame/Design/Quests/quests.quests#QuestBundles"


def qstate(inst, gid):
    qm = call("ue.find", {"class": "OwQuestManager"}, inst=inst)[0].split()[0]
    return call("ue.call", {"ref": qm, "fn": "GetQuestState_BP", "args": {"QuestID": gid}}, inst=inst)["ReturnValue"]


def door_state(inst, path):
    d = call("ue.find", {"path": path}, inst=inst)
    return call("ue.call", {"ref": d[0].split()[0] + ".OCLComponent", "fn": "GetCurrentState"}, inst=inst)["ReturnValue"] if d else "missing"


def strays(f, live):
    """Stand-in bodies (spawned DockWorker NPCs) standing where the stand-in stood when the host saved, other than the
    live one: the save kept a copy of it. A class that is not loaded in the map has no such actor."""
    out = []
    for cls in ("Townie_DockWorker_Male_C", "Townie_DockWorker_Female_C"):
        try:
            found = call("ue.find", {"class": cls, "limit": 200}, inst=0)
        except Exception as e:
            if "no class" not in str(e):
                raise
            found = []
        for h in found:
            a = h.split()[0]
            if a == live:
                continue
            p = call("ue.call", {"ref": a, "fn": "K2_GetActorLocation"}, inst=0)["ReturnValue"]
            if sum((p[k] - f["puppet_pos"][k]) ** 2 for k in "XYZ") ** 0.5 < 300:
                out.append(h)
    return out


def main(phase, facts_path):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    if phase == "stray":  # the host copy alone (co-op off) after loading CoopResume: no stand-in may exist at all
        f = json.loads(Path(facts_path).read_text(encoding="utf-8"))
        stray = strays(f, None)
        check(not stray, "the save holds no leftover stand-in", "; ".join(stray)[:200])
    elif phase == "save":
        n = len(call("eval", {"expr": Q}, inst=0)["value"])
        names = [call("eval", {"expr": f"{Q}[{i}].Name"}, inst=0)["value"] for i in range(n)]
        gid = None
        for i in sorted(range(n), key=lambda i: not names[i].startswith("06")):
            g = call("eval", {"expr": f"{Q}[{i}].ID"}, inst=0)["value"]
            if qstate(0, g) == "Invalid" and qstate(1, g) == "Invalid":
                call("ue.call", {"ref": "QuestScripts", "fn": "StartQuest", "args": {"QuestID": g}}, inst=0)
                time.sleep(0.5)
                if qstate(0, g) == "InProgress":
                    gid, qname = g, names[i]
                    break
        check(gid is not None, "host started a quest", qname if gid else "")
        door = None
        for h in call("ue.find", {"class": "Door", "limit": 50}, inst=0):
            a, _, path = h.split(" ", 2)
            if door_state(0, path) == "Closed":
                call("ue.call", {"ref": a + ".OCLComponent", "fn": "AttemptOpen", "args": {"bForce": True, "bIgnoreLockStatus": True}}, inst=0)
                time.sleep(2.5)
                if door_state(0, path) == "Open":
                    door = path
                    break
        check(door is not None, "host opened a door", door and door.split(".")[-1])
        time.sleep(3)
        check(qstate(1, gid) == "InProgress" and door_state(1, door) == "Open", "both reached the client before saving")
        sgm = call("ue.find", {"class": "SaveGameManager"}, inst=0)[0].split()[0]
        r = call("ue.call", {"ref": sgm, "fn": "SaveGame", "args": {"Filename": "CoopResume", "bIgnoreSuperNova": True, "SaveGameType": "Standard"}}, inst=0)
        time.sleep(6)
        pos = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
        world = call("eval", {"expr": "world"}, inst=0)["value"].split()[-1]
        puppet = call("coop.status", {}, inst=0)["puppet"]
        ppos = call("ue.call", {"ref": puppet, "fn": "K2_GetActorLocation"}, inst=0)["ReturnValue"] if puppet != "none" else None
        check(ppos is not None, "host shows the stand-in while saving")
        Path(facts_path).write_text(json.dumps({"quest": gid, "questName": qname, "door": door, "pos": pos, "world": world, "save": r,
                                                "puppet_pos": ppos}), encoding="utf-8")
        check(r.get("ReturnValue") in ("Success", "AsyncSaveTask_Begun"), "host saved CoopResume", str(r))
    else:
        f = json.loads(Path(facts_path).read_text(encoding="utf-8"))
        for inst, who in ((0, "host"), (1, "client")):
            w = call("eval", {"expr": "world"}, inst=inst)["value"].split()[-1]
            check(w == f["world"], f"{who} is in the saved map", w[-40:])
            check(qstate(inst, f["quest"]) == "InProgress", f"{who} has the quest in progress", f["questName"])
            check(door_state(inst, f["door"]) == "Open", f"{who} has the door open", f["door"].split(".")[-1])
            p = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=inst)["value"]
            d = sum((p[k] - f["pos"][k]) ** 2 for k in "XYZ") ** 0.5
            check(d < 800, f"{who} stands where the host saved", f"{d:.0f} units")
        s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
        check(s0["puppet"] != "none" and s1["puppet"] != "none", "both see each other", f"{s0['puppet']} / {s1['puppet']}")
        stray = strays(f, s0["puppet"])
        check(not stray, "the save holds no leftover stand-in", "; ".join(stray)[:200])
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
