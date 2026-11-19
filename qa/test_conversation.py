"""Shared conversations (owner, 2026-11-19), on a running pair in gameplay: a conversation pauses the game and concerns
both players. The host starts one with a companion: its game pauses; the client's game shows the same screen (the
speaker, the line, the responses) and pauses too; the client presses 1 and the host's conversation takes that response;
the host ends it: both games run again. Then the other way: the client talks, the host sees it.
Usage: python -I test_conversation.py"""
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

COMPANIONS = ("Marisol_C", "Azahelia_C", "Niles_C", "Inez_C", "Tristan_C", "Valerie_C", "Aza_C")


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def uc(inst, ref, fn, args=None):
    return call("ue.call", {"ref": ref, "fn": fn, "args": args or {}}, inst=inst)


def pid(i):
    cmd = r"(Get-Process -Name TheOuterWorlds2-Win64-Shipping | Where-Object { $_.Path -like 'C:\coop-anything\local\gamepool\instances\tow2\%d\*' }).Id" % i
    return int(subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.split()[0])


def lib(inst):
    return call("ue.find", {"path": "/Script/Arkansas.Default__ConversationBlueprintLibrary"}, inst=inst)[0].split()[0]


def paused(inst):
    gs = call("ue.find", {"path": "/Script/Engine.Default__GameplayStatics"}, inst=inst)[0].split()[0]
    pc = call("eval", {"expr": "pc"}, inst=inst)["value"].split()[0]
    return uc(inst, gs, "IsGamePaused", {"WorldContextObject": pc})["ReturnValue"]


def active(inst):
    return uc(inst, lib(inst), "GetActivePlayerConversation").get("ReturnValue") not in (None, "None")


def responses(inst):  # the starter's screen: its response texts
    for row in call("ue.find", {"class": "/Script/Arkansas.ConversationWidget", "limit": 10}, inst=inst):
        ref = row.split()[0]
        if "Default__" in row or not safe(lambda: uc(inst, ref, "IsVisible")["ReturnValue"], False):
            continue
        out = []
        for w in call("eval", {"expr": ref + ".SelectableResponseWidgets"}, inst=inst)["value"] or []:
            t = call("eval", {"expr": w.split()[0] + ".MessageTextBlock"}, inst=inst)["value"].split()[0]
            out.append(str(uc(inst, t, "GetText").get("ReturnValue")))
        return out
    return []


def wait(pred, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(0.25)
    return safe(pred, False)


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok
        return ok

    for talker, other, who in ((0, 1, "host"), (1, 0, "client")):
        npc = next((r.split()[0] for r in call("ue.find", {"class": "Character", "limit": 900}, inst=talker) if r.split()[1] in COMPANIONS), None)
        if not check(bool(npc), f"a companion to talk to in the {who}'s world"):
            continue
        Game(pid(other)).key("W", hold_s=0.05)  # the other player is awake (the idle camera hides the screen)
        r = uc(talker, lib(talker), "StartConversationWithActor", {"Speaker": npc, "StartNode": -1, "Stage": None, "bAllowBarkConversationDuringCombat": False})
        check(wait(lambda: active(talker), 5), f"the {who} talks to a companion", str(r.get("Result")))
        time.sleep(1.5)
        mine = responses(talker)
        tw = call("ue.find", {"path": "/Script/Engine.Default__GameplayStatics"}, inst=talker)[0].split()[0]
        wt = lambda: uc(talker, tw, "GetTimeSeconds", {"WorldContextObject": call("eval", {"expr": "pc"}, inst=talker)["value"].split()[0]})["ReturnValue"]  # noqa: E731
        a = wt()
        time.sleep(1.0)
        print(f"info: the {who}'s world time during its conversation: +{wt() - a:.2f} s in 1 s (the base game's own behaviour)", flush=True)
        st = {}
        ok = wait(lambda: st.update(call("coop.status", {}, inst=other)) or st.get("mirrorShown"), 5)
        rows = st.get("mirrorRows") or []
        check(bool(ok), "the other player's game shows that conversation", " | ".join(rows)[:300])
        same = bool(mine) and all(any(m[:40] in row for row in rows) for m in mine)
        check(same, "with the same responses", f"starter {mine[:4]}")
        ow = call("ue.find", {"path": "/Script/Engine.Default__GameplayStatics"}, inst=other)[0].split()[0]
        owt = lambda: uc(other, ow, "GetTimeSeconds", {"WorldContextObject": call("eval", {"expr": "pc"}, inst=other)["value"].split()[0]})["ReturnValue"]  # noqa: E731
        b0 = owt()
        time.sleep(1.5)
        # (the world time, not IsGamePaused: the game lifts the pause each frame and the mod asks it again before the world runs)
        check(owt() - b0 < 0.2, "and is paused too", f"its world time +{owt() - b0:.2f} s in 1.5 s")
        before = mine
        Game(pid(other)).key("1", hold_s=0.05)  # the other player picks the first response
        changed = wait(lambda: responses(talker) != before, 6)
        check(changed and call("coop.status", {}, inst=talker).get("convPicksDone", 0) > 0,
              "the other player pressed 1: the conversation took that response", f"now {responses(talker)[:3]}")
        inst = uc(talker, lib(talker), "GetActivePlayerConversation").get("ReturnValue")
        if inst and inst != "None":
            uc(talker, lib(talker), "EndConversationInstance", {"Instance": inst.split()[0]})
        check(wait(lambda: not call("coop.status", {}, inst=other).get("mirrorShown"), 5), "the conversation ended: the other's screen closed")
        check(wait(lambda: not paused(other) and not paused(talker), 5), "and both games run again",
              f"paused {safe(lambda: paused(talker))} / {safe(lambda: paused(other))}")
        time.sleep(1.0)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
