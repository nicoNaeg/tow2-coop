"""Real input combat check on a running pair: the host player walks up to an NPC and attacks it with posted mouse
clicks (the equipped weapon); the NPC must lose health on the host and the client's copy must follow.
Usage: python -I test_melee.py <attacker pid> [attacker inst: 0 host (default) or 1 client] [picture folder]"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402


def loc(inst, ref):
    return call("eval", {"expr": ref + ".K2_GetActorLocation()"}, inst=inst)["value"]


def hp(inst, ref):
    return call("ue.call", {"ref": ref + ".HealthComponent", "fn": "GetHealth"}, inst=inst)["ReturnValue"]


def main(host_pid, att=0, pics=Path(r"C:\coop-anything\local\the-outer-worlds-2\evidence\run13")):
    who = "host" if att == 0 else "client"
    me = loc(att, "pawn")
    best = None
    lib = call("ue.find", {"path": "/Script/Arkansas.Default__CharacterBlueprintFunctionLibrary"}, inst=0)[0].split()[0]
    pawn0 = call("eval", {"expr": "pawn"}, inst=0)["value"].split()[0]
    for c in call("ue.find", {"class": "Character", "limit": 400}, inst=0):
        a, cls, path = c.split(" ", 2)
        if "_UAID_" not in path or not cls.startswith(("OA_", "Bear_")):
            continue
        # only hostile NPCs: swings at a friendly one (it offers "Talk") do no damage (run 33)
        att_to = call("ue.call", {"ref": lib, "fn": "GetTeamAttitudeTowards", "args": {"Character": a, "Other": pawn0}}, inst=0)["ReturnValue"]
        if str(att_to).split(":")[-1] not in ("Hostile", "2"):
            continue
        p = loc(0, a)
        d = math.dist((p["X"], p["Y"], p["Z"]), (me["X"], me["Y"], me["Z"]))
        b = call("ue.find", {"path": path}, inst=1)
        if b and hp(0, a) > 0 and (best is None or d < best[0]):
            best = (d, a, b[0].split()[0], path)
    if not best:
        print("FAIL no live hostile NPC found\nRESULT: FAIL (1 failure(s))")
        return 1
    _, h_npc, c_npc, path = best
    mine = h_npc if att == 0 else c_npc  # the target in the attacker's own world

    def approach():
        """Stand 1.5 m from the target, facing it, as a player chasing it would. Its position comes from the attacker's
        world: the client's copy of an NPC can be up to 3 m from the host's and keeps moving (run 52: the client swung
        at the host's position, the bear had walked off and another one took the hits)."""
        p, me = loc(att, mine), loc(att, "pawn")
        if math.dist((p["X"], p["Y"]), (me["X"], me["Y"])) > 200:
            ang = math.atan2(me["Y"] - p["Y"], me["X"] - p["X"])
            me = {"X": p["X"] + 150 * math.cos(ang), "Y": p["Y"] + 150 * math.sin(ang), "Z": p["Z"] + 30}
            call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": me, "bSweep": False, "bTeleport": True}}, inst=att)
        yaw = math.degrees(math.atan2(p["Y"] - me["Y"], p["X"] - me["X"]))
        call("ue.call", {"ref": "pc", "fn": "SetControlRotation", "args": {"NewRotation": {"Pitch": -5, "Yaw": yaw, "Roll": 0}}}, inst=att)

    approach()
    time.sleep(0.8)
    before_h, before_c = hp(0, h_npc), hp(1, c_npc)
    g = Game(host_pid)
    g.shot(str(pics / f"melee-{who}-before.png"))  # a menu or popup in front would eat the clicks
    for _ in range(6):
        approach()
        g.click("left", hold_s=0.1)
        time.sleep(0.7)
    time.sleep(1.0)
    for _ in range(16):  # the last hit may still be on its way (client report -> host -> health stream back)
        after_h, after_c = hp(0, h_npc), hp(1, c_npc)
        if abs(after_c - after_h) < 1.0:
            break
        time.sleep(0.25)
    print(f"target {path.split('.')[-1]}: host {before_h:.0f} -> {after_h:.0f}, client {before_c:.0f} -> {after_c:.0f}")
    ok_hit = after_h < before_h
    ok_sync = abs(after_c - after_h) < 1.0
    print(f"{'PASS' if ok_hit else 'FAIL'} {who} player's attack damaged the NPC on the host (real input)")
    print(f"{'PASS' if ok_sync else 'FAIL'} client's copy has the host's health")
    g.shot(str(pics / f"melee-{who}-after.png"))
    try:  # no fight left for the next steps
        call("ue.call", {"ref": h_npc + ".HealthComponent", "fn": "Suicide", "args": {"bPreventRevival": True}}, inst=0)
    except Exception:  # noqa: BLE001
        pass
    fails = (not ok_hit) + (not ok_sync)
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")  # run_suite.ps1 reads this line
    return fails


if __name__ == "__main__":
    pics = Path(sys.argv[3]) if len(sys.argv) > 3 else Path(r"C:\coop-anything\local\the-outer-worlds-2\evidence\run13")
    pics.mkdir(parents=True, exist_ok=True)
    sys.exit(main(int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 0, pics))
