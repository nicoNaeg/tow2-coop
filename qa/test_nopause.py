"""Co-op never pauses (owner, 2026-11-19: "c'est multi"), on a running pair in gameplay: the pause menu or the inventory
one player opens leaves their world running (the game's own pause is lifted while the other player is in the game)
and the menu stays open; with the option off (coop.set noPause false) the same menu pauses the game, as before.
Usage: python -I test_nopause.py"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def uc(inst, ref, fn, args=None):
    return call("ue.call", {"ref": ref, "fn": fn, "args": args or {}}, inst=inst)


def gs(inst):
    return call("ue.find", {"path": "/Script/Engine.Default__GameplayStatics"}, inst=inst)[0].split()[0]


def pc(inst):
    return call("eval", {"expr": "pc"}, inst=inst)["value"].split()[0]


def world_time(inst):
    return uc(inst, gs(inst), "GetTimeSeconds", {"WorldContextObject": pc(inst)})["ReturnValue"]


def paused(inst):
    return uc(inst, gs(inst), "IsGamePaused", {"WorldContextObject": pc(inst)})["ReturnValue"]


def shown(inst, cls):  # a widget of that class on screen
    for row in call("ue.find", {"class": cls, "limit": 40}, inst=inst):
        ref = row.split()[0]
        if "Default__" in row:
            continue
        if safe(lambda: uc(inst, ref, "IsVisible")["ReturnValue"], False):  # (a page sits in its screen, not in the viewport)
            return True
    return False


MENUS = (("the pause menu", "TogglePauseMenu", "TogglePauseMenu", "/Script/Arkansas.PauseMenuWidget"),
         ("the inventory", "OpenInventory", "ToggleInventory", "/Script/Arkansas.InventoryPageWidget"))


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok
        return ok

    for inst, who in ((0, "host"), (1, "client")):
        for name, open_fn, close_fn, cls in MENUS:
            lifted = call("coop.status", {}, inst=inst).get("pausesLifted", 0)
            uc(inst, pc(inst), open_fn)
            time.sleep(1.0)
            t0 = world_time(inst)
            time.sleep(2.0)
            t1, p, up = world_time(inst), paused(inst), shown(inst, cls)
            # (IsGamePaused may still say yes: the game asks for its pause again each frame, the mod lifts it before the world runs)
            check(t1 - t0 > 1.5, f"the {who} opened {name}: its world kept running", f"world time +{t1 - t0:.2f} s in 2 s")
            check(up, f"and {name} is on screen", cls.rsplit(".", 1)[-1])
            check(call("coop.status", {}, inst=inst).get("pausesLifted", 0) > lifted, "the mod lifted the game's pause",
                  f"{call('coop.status', {}, inst=inst).get('pausesLifted')} in all")
            uc(inst, pc(inst), close_fn)
            time.sleep(1.0)
            t2 = world_time(inst)
            time.sleep(1.0)
            check(world_time(inst) - t2 > 0.7, f"{name} closed, the game runs", f"paused {paused(inst)}")
    # The option off: the game pauses for its menu as before (the control).
    call("coop.set", {"noPause": False}, inst=0)
    try:
        uc(0, pc(0), "TogglePauseMenu")
        time.sleep(1.0)
        t0 = world_time(0)
        time.sleep(1.5)
        t1, p = world_time(0), paused(0)
        check(p and t1 - t0 < 0.2, "with the option off, the host's pause menu pauses its game (as before)", f"paused {p}, world time +{t1 - t0:.2f} s")
        uc(0, pc(0), "TogglePauseMenu")
        time.sleep(1.0)
    finally:
        call("coop.set", {"noPause": True}, inst=0)
    check(not paused(0), "and closed, it runs again", f"paused {paused(0)}")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
