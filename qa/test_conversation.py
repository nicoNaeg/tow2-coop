"""Shared conversations (owner, 2026-11-20), on a running pair in gameplay: the player who starts a conversation leads
it; the other player's game plays the same conversation (same node, its camera and voices) and can't choose there;
the leader's pick is made on the other side too; when the leader's conversation ends, the other's ends.
Both directions: the host leads, then the client.
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


def conv(inst):  # the player's conversation instance, or None
    r = uc(inst, lib(inst), "GetActivePlayerConversation").get("ReturnValue")
    return None if r in (None, "None") else r.split()[0]


def active(inst):
    return conv(inst) is not None


def node(inst):
    c = conv(inst)
    return uc(inst, c, "BP_GetCurrentNodeID")["ReturnValue"] if c else None


def screen(inst):
    for row in call("ue.find", {"class": "/Script/Arkansas.ConversationWidget", "limit": 10}, inst=inst):
        ref = row.split()[0]
        if "Default__" not in row and safe(lambda: uc(inst, ref, "IsVisible")["ReturnValue"], False):
            return ref
    return None


def responses(inst):  # the screen's selectable response texts
    w = screen(inst)
    out = []
    for x in (call("eval", {"expr": w + ".SelectableResponseWidgets"}, inst=inst)["value"] or []) if w else []:
        t = call("eval", {"expr": x.split()[0] + ".MessageTextBlock"}, inst=inst)["value"].split()[0]
        out.append(str(uc(inst, t, "GetText").get("ReturnValue")))
    return out


def status(inst):
    return call("coop.status", {}, inst=inst)


def scripts(inst):
    return call("ue.find", {"path": "/Script/Arkansas.Default__Scripts"}, inst=inst)[0].split()[0]


def held(inst, cls_name):  # how many of that item the player holds
    return sum((x.get("Count") or 0) for x in call("eval", {"expr": "pawn.InventoryComponent.Items"}, inst=inst)["value"] or []
               if isinstance(x, dict) and x["Item"].split()[1] == cls_name)


def wait(pred, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(0.25)
    return safe(pred, False)


def start_with_companion(inst):
    for row in call("ue.find", {"class": "Character", "limit": 900}, inst=inst):
        if row.split()[1] not in COMPANIONS:
            continue
        r = uc(inst, lib(inst), "StartConversationWithActor", {"Speaker": row.split()[0], "StartNode": -1, "Stage": None, "bAllowBarkConversationDuringCombat": False})
        if r.get("Result") == "OnTrue":
            return row.split()[1]
    return None


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok
        return ok

    for talker, other, who in ((0, 1, "host"), (1, 0, "client")):
        if active(other):  # a conversation left from before
            uc(other, lib(other), "EndConversationInstance", {"Instance": conv(other)})
            wait(lambda: not active(other), 5)
        Game(pid(other)).key("W", hold_s=0.05)  # the other player is awake (the idle camera hides the screen)
        st0 = status(other)
        name = start_with_companion(talker)
        if not check(bool(name) and wait(lambda: active(talker), 5), f"the {who} talks to a companion", str(name)):
            continue
        ok = wait(lambda: status(other).get("convFollow") and active(other), 10)
        check(ok, "the other player's game plays the same conversation", f"follows {status(other).get('convFollows')}")
        same = wait(lambda: node(other) == node(talker) and node(talker) is not None, 6)
        check(same, "at the same node", f"leader {safe(lambda: node(talker))} / other {safe(lambda: node(other))}")
        time.sleep(1.5)  # the lines play; the responses come
        mine, theirs = responses(talker), responses(other)
        print(f"info: the leader's choices {mine[:4]} / the other's {theirs[:4]}", flush=True)
        # the other player can't choose: its own response input is dropped
        before = safe(lambda: node(other))
        w = screen(other)
        if w:
            uc(other, w, "OnResponse1InputAction")
        time.sleep(1.0)
        st = status(other)
        check(st.get("convBlocked", 0) > st0.get("convBlocked", 0) and safe(lambda: node(other)) == before,
              "the other player can't choose (their response input is dropped)", f"blocked {st.get('convBlocked')}, node {before} -> {safe(lambda: node(other))}")
        # the leader picks: a response that goes on if there is one
        pick = next((i for i, t in enumerate(mine) if "Leave" not in t and "Quitter" not in t), 0) + 1
        lead_before = safe(lambda: node(talker))
        moved = False
        for _ in range(8):  # as a player would: pressed again while the line still plays (the game ignores it then)
            tw = screen(talker)
            if tw:
                uc(talker, tw, f"OnResponse{pick}InputAction")
            moved = wait(lambda: node(talker) != lead_before, 1.0)
            if moved:
                break
        check(moved, f"the leader picks response {pick}", f"{(mine[pick - 1] if len(mine) >= pick else '?')[:60]!r}: node {lead_before} -> {safe(lambda: node(talker))}")
        took = lambda: node(other) == node(talker) and node(talker) not in (None, lead_before)  # noqa: E731
        followed = ok and wait(lambda: took() or (not active(talker) and not active(other) and status(other).get("convPicksApplied", 0) + status(other).get("convJumps", 0) > st0.get("convPicksApplied", 0) + st0.get("convJumps", 0)), 8)
        st = status(other)
        check(followed, "the other player's conversation took the leader's pick",
              f"leader {safe(lambda: node(talker))} / other {safe(lambda: node(other))}; picks applied {st.get('convPicksApplied')}, restarts {st.get('convJumps')}")
        if len(mine) >= pick and mine[pick - 1] in theirs:  # the same response is there to choose: chosen, not started again
            check(st.get("convPicksApplied", 0) > st0.get("convPicksApplied", 0), "the same response was chosen on the other side",
                  f"picks applied {st0.get('convPicksApplied')} -> {st.get('convPicksApplied')}")
        if active(talker):
            uc(talker, lib(talker), "EndConversationInstance", {"Instance": conv(talker)})
        check(wait(lambda: not active(other) and not status(other).get("convFollow"), 6), "the leader's conversation ended: the other's ended too")
        check(wait(lambda: not paused(other) and not paused(talker), 5), "and neither game is paused",
              f"paused {safe(lambda: paused(talker))} / {safe(lambda: paused(other))}")
        time.sleep(2.0)
    # A response the other player's character can't take (a skill check, a flaw): the test makes the client's game treat
    # the leader's pick as locked; it is picked past the lock (put first in the selectable list for the call), no restart.
    talker, other = 0, 1
    gear = ("Armor_", "Headgear_", "Weapon_")
    host_items = call("eval", {"expr": "pawn.InventoryComponent.Items"}, inst=talker)["value"] or []
    pay_name = next((x["Item"].split()[1] for x in host_items if isinstance(x, dict) and not x["Item"].split()[1].startswith(gear)), None)
    pay_ref = next((r.split()[0] for r in call("ue.find", {"name": pay_name, "limit": 20}, inst=other) if r.split()[1] == "BlueprintGeneratedClass"), None) if pay_name else None
    if pay_ref:
        safe(lambda: uc(other, scripts(other), "GiveItemToPlayer", {"Item": pay_ref, "Count": 2}))
    st0 = status(other)
    if check(bool(start_with_companion(talker)) and wait(lambda: status(other).get("convFollow") and active(other), 10), "locked: the host talks, the client follows"):
        wait(lambda: len([e for e in status(talker).get("convShown", []) if e[1]]) >= 2 and node(other) == node(talker), 8)
        time.sleep(1.5)
        sel = [e[0] for e in status(talker).get("convShown", []) if e[1]]
        pick = len(sel)  # the last one ("talk later": leaves, changes nothing), never first on the client's list
        # a payment the conversation takes (an item, through the game's script) is the leader's, not the follower's
        if check(bool(pay_ref), "the client was given an item to pay with", str(pay_name)):
            c0 = held(other, pay_name)
            safe(lambda: uc(other, scripts(other), "RemoveItemFromPlayer", {"Item": pay_ref, "Count": 1}))
            time.sleep(0.5)
            check(held(other, pay_name) == c0, "while it follows, a payment script takes nothing from the client", f"{c0} -> {held(other, pay_name)} {pay_name}")
        call("coop.set", {"convLockTest": int(sel[-1])}, inst=other)
        before = safe(lambda: node(talker))
        for _ in range(8):
            uc(talker, screen(talker), f"OnResponse{pick}InputAction")
            if wait(lambda: node(talker) != before, 1.0):
                break
        took = wait(lambda: status(other).get("convBypassed", 0) > st0.get("convBypassed", 0), 6)
        check(bool(took), "the client took the leader's response past its lock", f"bypassed {st0.get('convBypassed')} -> {status(other).get('convBypassed')}")
        check(wait(lambda: (node(other) == node(talker) and node(talker) not in (None, before)) or (not active(talker) and not active(other)), 8),
              "at the leader's next line", f"leader {safe(lambda: node(talker))} / other {safe(lambda: node(other))}")
        wait(lambda: not active(talker) and not active(other), 10)  # the reply plays out: no restart meanwhile either
        st = status(other)
        check(st.get("convJumps", 0) == st0.get("convJumps", 0), "the same response, so no restart (no camera cut)",
              f"restarts {st0.get('convJumps')} -> {st.get('convJumps')}")
        call("coop.set", {"convLockTest": -1}, inst=other)
        if active(talker):
            uc(talker, lib(talker), "EndConversationInstance", {"Instance": conv(talker)})
        wait(lambda: not active(other), 6)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
