"""Story sync check on a running pair in gameplay (copy 0 host, copy 1 client, same campaign state):
the host starts an unstarted quest, triggers one of its addendums (objective update), sets one of the global
variables a quest objective tests; each change must reach the client (end states: test_resync.py).
Usage: python -I test_story.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

Q = "/Game/Exported/BaseGame/Design/Quests/quests.quests#QuestBundles"


def state(inst, gid):
    qm = call("ue.find", {"class": "OwQuestManager"}, inst=inst)[0].split()[0]
    return call("ue.call", {"ref": qm, "fn": "GetQuestState_BP", "args": {"QuestID": gid}}, inst=inst)["ReturnValue"]


def wait(pred, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return True
        time.sleep(0.25)
    return pred()


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    s0, s1 = call("coop.status", {}, inst=0)["story"], call("coop.status", {}, inst=1)["story"]
    check(s0["loaded"] and s0["quests"] > 100, "host loaded the quest data", f"{s0['quests']} quests, {s0['globals']} globals, {s0['probes']} probes")
    n = len(call("eval", {"expr": Q}, inst=0)["value"])
    # an unstarted quest (on both) that has addendums and objective globals
    pick = None
    names = [call("eval", {"expr": f"{Q}[{i}].Name"}, inst=0)["value"] for i in range(n)]
    order = sorted(range(n), key=lambda i: not names[i].startswith("06"))  # quests of the current region (0601) first
    for i in order:
        gid = call("eval", {"expr": f"{Q}[{i}].ID"}, inst=0)["value"]
        if state(0, gid) != "Invalid" or state(1, gid) != "Invalid":
            continue
        adds = []
        for arr in ("GenericObjectiveNodes", "GlobalVariableObjectiveNodes", "TalkObjectiveNodes"):
            for node in call("eval", {"expr": f"{Q}[{i}].{arr}"}, inst=0)["value"] or []:
                pass
            k = len(call("eval", {"expr": f"{Q}[{i}].{arr}"}, inst=0)["value"] or [])
            for j in range(k):
                adds += call("eval", {"expr": f"{Q}[{i}].{arr}[{j}].AddendumIDs"}, inst=0)["value"] or []
        gvs = call("eval", {"expr": f"{Q}[{i}].GlobalVariableObjectiveNodes"}, inst=0)["value"] or []
        if adds and gvs:
            gv = call("eval", {"expr": f"{Q}[{i}].GlobalVariableObjectiveNodes[0].Conditional.ID.ID"}, inst=0)["value"]
            pick = (i, call("eval", {"expr": f"{Q}[{i}].Name"}, inst=0)["value"], gid, adds[0], gv)
            break
    check(pick is not None, "found an unstarted quest with addendums and objective globals", str(pick and pick[1]))
    if not pick:
        return fails
    i, name, gid, add, gv = pick
    call("ue.call", {"ref": "QuestScripts", "fn": "StartQuest", "args": {"QuestID": gid}}, inst=0)
    time.sleep(0.5)
    check(state(0, gid) == "InProgress", "host started it", name)
    check(wait(lambda: state(1, gid) == "InProgress"), "client's copy started too", state(1, gid))
    call("ue.call", {"ref": "QuestScripts", "fn": "TriggerQuestAddendum", "args": {"QuestID": gid, "AddendumID": add, "DisplayNotification": True}}, inst=0)
    trig = lambda inst: call("ue.call", {"ref": "QuestConditionals", "fn": "IsQuestAddendumTriggered", "args": {"QuestID": gid, "AddendumID": add}}, inst=inst)["ReturnValue"]
    check(trig(0), f"host triggered addendum {add}")
    check(wait(lambda: trig(1)), "client's copy got the addendum")
    is_val = lambda inst, v: call("ue.call", {"ref": "GlobalVariableConditionals", "fn": "IsGlobalValue", "args": {"Variable": gv, "Operator": "Equals", "Value": v}}, inst=inst)["ReturnValue"]
    target = 7 if not is_val(0, 7) else 8
    call("ue.call", {"ref": "GlobalVariableScripts", "fn": "SetGlobalValue", "args": {"Variable": gv, "Value": target}}, inst=0)
    check(is_val(0, target), f"host set an objective global to {target}")
    check(wait(lambda: is_val(1, target)), "client's global has the same value")
    # Quest end states (complete/fail) cannot be applied by script in this build; they reach the client through a
    # save resync, tested by test_resync.py A.
    s0, s1 = call("coop.status", {}, inst=0)["story"], call("coop.status", {}, inst=1)["story"]
    print(f"host events {s0['events']} acked {s0['peerAcked']}; client applied {s1['applied']} ({s1['appliedCount']} changes, {s1['skipped']} already true), last: {s1['lastApplied']}")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
