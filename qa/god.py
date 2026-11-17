"""Test helper: the bot players stand still among hostile NPCs during the combat tests, and a dead or downed host cannot
save, so a later hand-over fails (runs 44-47). Turns god mode on for both players' current pawns (a reload resets it).
Usage: python -I god.py"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

fails = 0
for inst in (0, 1):
    try:
        call("ue.call", {"ref": "pawn.HealthComponent", "fn": "SetGod", "args": {"bEnabled": True}}, inst=inst)
        hp = call("ue.call", {"ref": "pawn.HealthComponent", "fn": "GetHealth"}, inst=inst)["ReturnValue"]
        print(f"PASS copy {inst}: god mode on, health {hp:.0f}")
    except Exception as e:
        print(f"FAIL copy {inst}: {e}")
        fails += 1
print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
sys.exit(fails)
