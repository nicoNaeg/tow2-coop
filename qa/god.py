"""Test helper: the bot players stand still among hostile NPCs during the combat tests, and a dead or downed host cannot
save, so a later hand-over fails (runs 44-47). Turns god mode on for both players' current pawns (a reload resets it), no fall damage, a downed player revived, a client
lost under the ground put back next to the host, the game's flaw offers off (one pending blocks the player's conversation
responses: run 193, Foot-in-Mouth Syndrome after the tests skipped many lines).
Usage: python -I god.py"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

fails = 0
for inst in (0, 1):
    try:
        life = str(call("ue.call", {"ref": "pawn.HealthComponent", "fn": "GetStateOfBeing"}, inst=inst)["ReturnValue"]).split(":")[-1]
        if life != "Healthy":  # left down by the step before (runs 168-169: the client died after the combat test)
            call("ue.call", {"ref": "pawn.HealthComponent", "fn": "Resurrect", "args": {"bDebug": False, "HealthPercentOverride": 1.0}}, inst=inst)
            print(f"info: copy {inst}'s player was {life}: revived")
        call("ue.call", {"ref": "pawn.HealthComponent", "fn": "SetGod", "args": {"bEnabled": True}}, inst=inst)
        call("ue.set", {"path": "pawn.HealthComponent.bImmuneToFallDamage", "value": True}, inst=inst)
        info = [x for x in call("ue.find", {"class": "/Script/Arkansas.PlayerInfoComponent", "limit": 5}, inst=inst) if "Default__" not in x]
        if info:
            call("ue.call", {"ref": info[0].split()[0], "fn": "DisableFlawOffers"}, inst=inst)
        hp = call("ue.call", {"ref": "pawn.HealthComponent", "fn": "GetHealth"}, inst=inst)["ReturnValue"]
        print(f"PASS copy {inst}: god mode on, health {hp:.0f}")
    except Exception as e:
        print(f"FAIL copy {inst}: {e}")
        fails += 1
# A test's teleport can leave the client under the ground (17:47, after the combat test: 75 m below the host, the steps
# after it swung at nothing): a client far from the host, or well below it, is put back next to the host.
try:
    h = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
    c = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"]
    far = ((h["X"] - c["X"]) ** 2 + (h["Y"] - c["Y"]) ** 2) ** 0.5
    if far > 3000 or h["Z"] - c["Z"] > 800:
        call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": {"X": h["X"] + 150, "Y": h["Y"], "Z": h["Z"] + 100},
                                                                              "bSweep": False, "bTeleport": True}}, inst=1)
        print(f"info: the client was {far / 100:.0f} m from the host, {(h['Z'] - c['Z']) / 100:.0f} m below: put back next to it")
except Exception as e:  # noqa: BLE001
    print(f"info: the players' places not read: {e}")
print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
sys.exit(fails)
