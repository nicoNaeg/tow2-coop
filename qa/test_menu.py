"""Host and join through the game's main menu (the copies started in menu mode, TOW2_COOP_MENU=1; direct IP keeps
copy 0 the host side and copy 1 the client side, as the menu then offers).

A. Both copies show the co-op choice in the main menu (the Moon Man's conversation); nobody plays together yet.
B. The host opens it (key 6) and picks "I'll host" (key 1): it hosts; the game did not take the key (no Continue: still
   at the main menu).
C. The host loads its game.
D. The client opens it with a click, goes back with a click on "Never mind", opens it again (key 6), picks "I'll join"
   (key 1): the list shows the host; a click on it: the client joins, loads the host's game, both see each other.
Usage: python -I test_menu.py <host pid> <client pid>"""
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

FAILS = 0
OPEN = ("Let's play together (co-op).", "Jouons ensemble (co-op).")


def check(ok, what, detail=""):
    global FAILS
    print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
    FAILS += not ok
    return ok


def safe(f, d=None):
    try:
        return f()
    except Exception as e:  # noqa: BLE001
        return d if d is not None else "err " + str(e)[:160]


def st(i):
    return call("coop.status", {}, inst=i)


def menu(i):
    return safe(lambda: st(i)["menu"], {"shown": False, "entries": [], "level": -1, "rects": []})


def world(i):  # the world's path (the reference also holds this process's address)
    return safe(lambda: call("eval", {"expr": "world"}, inst=i)["value"].split(" ", 2)[-1], "")


def wait_for(cond, secs, step=0.5):
    t0 = time.time()
    while time.time() - t0 < secs:
        v = safe(cond, None)
        if v and not (isinstance(v, str) and v.startswith("err")):
            return v
        time.sleep(step)
    return None


def laid_out(i, n=None):
    """The menu's choices once drawn (a rectangle is empty until the game lays the new choice out)."""
    def ok():
        m = menu(i)
        r = m.get("rects") or []
        return m if r and all(len(x) == 4 and x[2] - x[0] > 10 and x[3] - x[1] > 5 for x in r) and (n is None or m["level"] == n) else None
    wait_for(ok, 5, step=0.25)
    time.sleep(1.0)  # the new choices slide into place for a moment after a level change (probe 99)
    return wait_for(ok, 3, step=0.25) or menu(i)


def click(game, rect):
    """A posted click at the middle of a menu entry (viewport pixels; the mod scales the window's back to them)."""
    import ctypes
    u = ctypes.windll.user32
    h = game.hwnd()
    # This script is not DPI aware: Windows scales the coordinates it posts by the display scale (150 %: a click it posts
    # at 485, 507 reaches the game at 727, 760, outside its 960 x 540 viewport; probe 98). Post in its own units.
    class R(ctypes.Structure):
        _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long), ("r", ctypes.c_long), ("b", ctypes.c_long)]
    rc = R()
    u.GetClientRect(h, ctypes.byref(rc))  # in this script's units (640 x 360 for a 960 x 540 window at 150 %)
    scale = rc.r / game.viewport_w if rc.r > 0 and game.viewport_w > 0 else 1.0
    x, y = (rect[0] + rect[2]) / 2 * scale, (rect[1] + rect[3]) / 2 * scale
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)
    time.sleep(0.05)
    u.PostMessageW(h, 0x0201, 1, lp)
    time.sleep(0.06)
    u.PostMessageW(h, 0x0202, 0, lp)


def main(host_pid, client_pid):
    games = {0: Game(host_pid), 1: Game(client_pid)}
    for i, g in games.items():  # the game's viewport width (its UI pixels: where the menu's choices are measured)
        lib = call("ue.find", {"path": "/Script/UMG.Default__WidgetLayoutLibrary"}, inst=i)[0].split()[0]
        pc = call("eval", {"expr": "pc"}, inst=i)["value"].split()[0]
        g.viewport_w = safe(lambda: call("ue.call", {"ref": lib, "fn": "GetViewportSize", "args": {"WorldContextObject": pc}}, inst=i)["ReturnValue"]["X"], 0)
    for i in (0, 1):  # past the title screen ("press any key") to the Moon Man's choices
        shown = wait_for(lambda: menu(i)["shown"] or games[i].key("SPACE") or False, 150, step=3.0)
        m = menu(i)
        check(bool(shown) and m["entries"][:1] and m["entries"][0] in OPEN, f"copy {i}: the co-op choice is in the main menu", str(m["entries"]))
        check(st(i)["roleChosen"] is False, f"copy {i}: nobody plays together yet")
    # B: the host hosts
    games[0].key("6")
    lvl = wait_for(lambda: menu(0)["level"] == 1, 5)
    check(bool(lvl), "the host opened the co-op choices (key 6)", str(menu(0)["entries"]))
    games[0].key("1")
    hosting = wait_for(lambda: st(0)["roleChosen"] and st(0)["host"], 5)
    time.sleep(3)
    check(bool(hosting), "it picked 'I'll host' (key 1)", str(menu(0)["entries"]))
    check("MainMenu" in world(0), "the game did not take that key (still at the main menu, no Continue)", world(0)[-50:])
    # C: the host loads its game
    r = subprocess.run([sys.executable, "-I", "session.py", "--instances", "0"], cwd=str(Path(__file__).resolve().parent), capture_output=True, text=True)
    check(r.returncode == 0 and "MainMenu" not in world(0), "the host loaded its game", r.stdout.strip().splitlines()[-1:] if r.stdout else r.stderr[-200:])
    # D: the client joins, with clicks and keys
    m = laid_out(1, 0)
    if m["rects"] and m["rects"][0]:
        click(games[1], m["rects"][0])
    opened = wait_for(lambda: menu(1)["level"] == 1, 5)
    check(bool(opened), "the client opened the co-op choices with a click", f"last click {menu(1).get('lastClick')}, entry at {m['rects'][:1]}")
    m = laid_out(1, 1)
    if opened and m["rects"]:
        click(games[1], m["rects"][-1])  # "Never mind."
    back = wait_for(lambda: menu(1)["level"] == 0, 5)
    if not back:
        print(f"info: the 'Never mind' click: rects {m.get('rects')}, last click {menu(1).get('lastClick')}")
    check(bool(back) and menu(1)["entries"][:1] and menu(1)["entries"][0] in OPEN, "'Never mind' (a click) went back", str(menu(1)["entries"]))
    games[1].key("6")
    wait_for(lambda: menu(1)["level"] == 1, 5)
    games[1].key("1")
    listed = wait_for(lambda: menu(1)["level"] == 2 and len(menu(1)["entries"]) >= 2, 6)
    m = laid_out(1, 2)
    check(bool(listed), "'I'll join' (key 1) lists the host", str(m["entries"]))
    if listed and m["rects"]:
        click(games[1], m["rects"][0])
    chosen = wait_for(lambda: st(1)["roleChosen"] and not st(1)["host"], 6)
    check(bool(chosen), "a click on the host: the client joins it", str(menu(1)["entries"]))
    t0 = time.time()
    joined = wait_for(lambda: world(1) == world(0) and "MainMenu" not in world(1), 300, step=3.0)
    check(bool(joined), "the client loaded the host's game", f"after {time.time() - t0:.0f} s: {world(1)[-40:]}")
    seen = wait_for(lambda: st(0).get("puppet", "none") != "none" and st(1).get("puppet", "none") != "none", 60, step=2.0)
    check(bool(seen), "both see each other", f"{st(0).get('puppet')} / {st(1).get('puppet')}")
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]), int(sys.argv[2])))
