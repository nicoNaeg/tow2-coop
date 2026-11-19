"""Death and reload on a joined pair (copy 0 host, copy 1 client, both in gameplay near each other).
A. The host's game loads a save (what its death screen's Continue or the Load menu do): the same save goes to the client
   at once and both load it together; no second hand-over once the host's world is up; the client keeps its character.
B. The client dies: the host's current game is fetched and held; the client's own Continue (its game's LoadGame of a
   local save) is refused and the held game loads instead: the client is back, alive, next to the host.
C. The client dies, then is revived (as with a trauma kit): the fetched game is dropped, no reload.
D. The host quicksaves (F5) and quickloads (F9), real keys to its window: the client loads that quicksave with it.
Usage: python -I test_death.py [ABCD] [host pid]"""
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

HOST_SAVE = "B9F05C364F38D4A68B3AB39CCB0910F8"  # the host's test save (session.py)
LOCAL_SAVE = "Autosave00"                       # one of the client's own saves


def safe(f, d=None):
    try:
        return f()
    except Exception:  # noqa: BLE001
        return d


def wait(pred, timeout, step=0.25):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if safe(pred, False):
            return True
        time.sleep(step)
    return bool(safe(pred, False))


def st(i):
    return call("coop.status", {}, inst=i, timeout_s=10)


def world(i):
    return safe(lambda: call("eval", {"expr": "world"}, inst=i, timeout_s=4)["value"].split()[0], "busy")


def life(i):
    return safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "GetStateOfBeing"}, inst=i, timeout_s=4)["ReturnValue"], "?")


def bits(i):
    k = next(r.split()[0] for r in call("ue.find", {"name": "Bit_Cartridge_C", "limit": 20}, inst=i)
             if r.split()[1].endswith("Class") and r.split()[2].endswith(".Bit_Cartridge_C"))
    return call("ue.call", {"ref": "pawn.InventoryComponent", "fn": "GetItemCountOnActor", "args": {"Actor": "pawn", "ItemType": k}}, inst=i)["ReturnValue"]


def sgm(i):
    return call("ue.find", {"class": "SaveGameManager", "limit": 2}, inst=i)[0].split()[0]


def both_see():
    return st(0)["puppet"] != "none" and st(1)["puppet"] != "none"


def distance():
    hp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=0)["value"]
    cp = call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"]
    return ((hp["X"] - cp["X"]) ** 2 + (hp["Y"] - cp["Y"]) ** 2) ** 0.5


def kill(i):
    call("ue.call", {"ref": "pawn.HealthComponent", "fn": "SetGod", "args": {"bEnabled": False}}, inst=i)
    call("ue.call", {"ref": "pawn.HealthComponent", "fn": "Suicide", "args": {"bPreventRevival": False}}, inst=i)


def god_on():
    for i in (0, 1):
        safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "SetGod", "args": {"bEnabled": True}}, inst=i))


def main(which, host_pid=None):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    if "A" in which:
        w0, w1 = world(0), world(1)
        r0, b1 = st(0)["resyncs"], safe(lambda: bits(1))
        t0 = time.time()
        threading.Thread(target=lambda: safe(lambda: call("ue.call", {"ref": sgm(0), "fn": "LoadGame", "args": {"Filename": HOST_SAVE, "bPreValidateMetadata": True},
                                                          "timeoutMs": 240000}, inst=0, timeout_s=250)), daemon=True).start()
        seen = {}  # when each world changed: the core's own clock (a loading game answers the test late)
        while time.time() - t0 < 180 and len(seen) < 2:
            for i, w in ((0, w0), (1, w1)):
                now = world(i)
                if i not in seen and now not in (w, "busy"):
                    age = safe(lambda: st(i)["worldAge"], 0.0)
                    seen[i] = time.time() - age
            time.sleep(0.25)
        check(len(seen) == 2, "both games loaded a new world", f"host {seen.get(0, 0) - t0:.1f} s, client {seen.get(1, 0) - t0:.1f} s after the host's load started")
        lag = seen[1] - seen[0] if len(seen) == 2 else 999
        print(f"client in its new world {lag:.1f} s after the host (before: about 20 s)", flush=True)
        check(lag < 8, "the client loaded the same save at the same time", f"{lag:.1f} s")
        check(st(0).get("reloadsHanded", 0) >= 1, "the host sent the save its game loads", st(0).get("resyncReason"))
        wait(both_see, 120)
        time.sleep(8)
        r1 = st(0)["resyncs"]
        check(r1 == r0 + 1, "one hand-over only (the save being loaded, none after the host's world was up)", f"resyncs {r0} -> {r1}")
        check(both_see(), "both see each other after the reload", f"{st(0)['puppet']} / {st(1)['puppet']}")
        b = safe(lambda: bits(1))
        check(b == b1, "the client kept its own character", f"bits {b1} -> {b}")
    if "B" in which:
        god_on()
        w1, deaths = world(1), st(1).get("deaths", 0)
        kill(1)
        check(wait(lambda: st(1).get("died"), 15), "the client died", life(1))
        held = wait(lambda: call("coop.joinStatus", {}, inst=1)["state"].startswith("received: loads"), 60)
        check(held, "the host's current game came and waits for the client's Continue", call("coop.joinStatus", {}, inst=1)["state"])
        time.sleep(1)
        t0 = time.time()
        r = safe(lambda: call("ue.call", {"ref": sgm(1), "fn": "LoadGame", "args": {"Filename": LOCAL_SAVE, "bPreValidateMetadata": True}}, inst=1))
        check(isinstance(r, dict) and r.get("ReturnValue") == "LoadCanceledByUser", "the client's own Continue (a local save) is refused", str(r))
        back = wait(lambda: world(1) not in (w1, "busy"), 60)
        check(back, "the client loaded the host's game instead", f"{time.time() - t0:.1f} s after Continue")
        wait(both_see, 90)
        check(wait(lambda: life(1) == "Healthy", 30), "the client is alive again", life(1))
        check(both_see() and distance() < 1500, "the client is back next to the host", f"{safe(distance, -1):.0f} units")
        s = st(1)
        check(s.get("deaths", 0) == deaths + 1 and s.get("loadsRedirected", 0) >= 1 and not s.get("died"), "the mod saw the death and the redirected load",
              f"deaths {s.get('deaths')}, redirected {s.get('loadsRedirected')}")
        god_on()
    if "C" in which:
        god_on()
        w1 = world(1)
        kill(1)
        check(wait(lambda: st(1).get("died"), 15), "the client died", life(1))
        wait(lambda: call("coop.joinStatus", {}, inst=1)["state"].startswith("received"), 60)
        call("ue.call", {"ref": "pawn.HealthComponent", "fn": "Resurrect", "args": {"bDebug": False, "HealthPercentOverride": 1.0}}, inst=1)
        check(wait(lambda: not st(1).get("died"), 10), "the client was revived", life(1))
        time.sleep(15)
        check(world(1) == w1, "no reload after the revive (the fetched game was dropped)", call("coop.joinStatus", {}, inst=1)["state"])
        check(life(1) == "Healthy", "the client is alive", life(1))
        god_on()
    if "D" in which and host_pid:
        god_on()
        game = Game(int(host_pid))
        game.key("F5")
        time.sleep(5)  # the quicksave is written
        w0, w1, h0 = world(0), world(1), st(0).get("reloadsHanded", 0)
        t0 = time.time()
        game.key("F9")
        seen = {}
        while time.time() - t0 < 120 and len(seen) < 2:
            for i, w in ((0, w0), (1, w1)):
                now = world(i)
                if i not in seen and now not in (w, "busy"):
                    seen[i] = time.time() - safe(lambda: st(i)["worldAge"], 0.0)
            time.sleep(0.25)
        check(len(seen) == 2, "F9: both games loaded the host's quicksave", f"host {seen.get(0, 0) - t0:.1f} s, client {seen.get(1, 0) - t0:.1f} s after F9")
        lag = seen[1] - seen[0] if len(seen) == 2 else 999
        check(lag < 8 and st(0).get("reloadsHanded", 0) > h0, "the client loaded it at the same time as the host", f"{lag:.1f} s behind")
        check(wait(both_see, 90), "both see each other after the quickload")
        god_on()
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "ABC", sys.argv[2] if len(sys.argv) > 2 else None))
