"""A new game together from the very beginning (owner, 2026-11-20), on copies started in menu mode (TOW2_COOP_MENU=1,
direct IP: copy 0 the host side, copy 1 the client side) with no character of the client's own.

A. The host opens the co-op choice (key 6) and picks "I'll host a new game" (key 2): the game's New Game starts
   (the character creator); the test makes a character (keys and clicks, as a player would) and the game starts.
B. The client opens it (key 6), "I'll join" (key 1), the host (key 1): having no character, its own New Game starts;
   the test makes a different character.
C. In the intro, the client's new character is kept (a private save, once the intro allows one), then the host's game
   comes: the client plays in the host's world with its own new character; both see each other.
Usage: python -I test_newgame.py <host pid> <client pid>"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from test_menu import click, laid_out  # noqa: E402
from tow2 import call  # noqa: E402

FAILS = 0


def check(ok, what, detail=""):
    global FAILS
    print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
    FAILS += not ok
    return ok


def safe(f, d=None):
    try:
        return f()
    except Exception:  # noqa: BLE001
        return d


def st(i):
    return call("coop.status", {}, inst=i)


def world(i):
    return safe(lambda: call("eval", {"expr": "world"}, inst=i)["value"].split(" ", 2)[-1], "")


def wait_for(cond, secs, step=0.5):
    t0 = time.time()
    while time.time() - t0 < secs:
        v = safe(cond, None)
        if v:
            return v
        time.sleep(step)
    return None


def visible(i, cls):
    for row in safe(lambda: call("ue.find", {"class": cls, "limit": 8}, inst=i), []) or []:
        if "Default__" not in row and safe(lambda: call("ue.call", {"ref": row.split()[0], "fn": "IsVisible", "args": {}}, inst=i)["ReturnValue"], False):
            return row.split()[0]
    return None


def lib(i):
    return call("ue.find", {"path": "/Script/Arkansas.Default__ConversationBlueprintLibrary"}, inst=i)[0].split()[0]


def conv(i):
    r = safe(lambda: call("ue.call", {"ref": lib(i), "fn": "GetActivePlayerConversation", "args": {}}, inst=i)["ReturnValue"], "None")
    return None if r in (None, "None") else r.split()[0]


def make_character(i, game, trait, skills):
    """The character creator, as a player: skip the intro movie, Next (E) through its pages with a background, a trait
    and two skills clicked (the 960 x 540 viewport's places, probe 109), the name kept, Normal accepted."""
    if not wait_for(lambda: visible(i, "/Script/Arkansas.MoviePlayerWidget") or visible(i, "/Script/Arkansas.ARCC_MasterWidget"), 90, 1.0):
        return False
    m = visible(i, "/Script/Arkansas.MoviePlayerWidget")
    if m:
        safe(lambda: call("ue.call", {"ref": m, "fn": "OnMovieSkipped", "args": {}}, inst=i))
    if not wait_for(lambda: visible(i, "/Script/Arkansas.ARCC_MasterWidget"), 60, 1.0):
        return False
    time.sleep(3.0)
    steps = [("key", "E"), ("click", (170, 153)), ("key", "E"), ("click", trait), ("key", "E"), ("click", skills[0]), ("click", skills[1]),
             ("key", "E"), ("key", "E"), ("key", "E"), ("key", "E")]
    for kind, v in steps:
        if kind == "key":
            game.key(v)
        else:
            click(game, (v[0] - 1, v[1] - 1, v[0] + 1, v[1] + 1))
        time.sleep(1.8)
    return wait_for(lambda: "PS_Intro" in world(i), 120, 2.0)


def through_intro(i, secs=150):
    """The intro's opening conversation, skipped line by line (a response: the first)."""
    t0 = time.time()
    while time.time() - t0 < secs:
        c = conv(i)
        if not c:
            if time.time() - t0 > 20:
                return True
            time.sleep(1.0)
            continue
        w = visible(i, "/Script/Arkansas.ConversationWidget")
        if w:
            rs = safe(lambda: call("eval", {"expr": w + ".SelectableResponseWidgets"}, inst=i)["value"], []) or []
            safe(lambda: call("ue.call", {"ref": w, "fn": "OnResponse1InputAction" if rs else "OnSkipInputAction", "args": {}}, inst=i))
        time.sleep(1.0)
    return not conv(i)


def main(host_pid, client_pid):
    games = {0: Game(host_pid), 1: Game(client_pid)}
    for g in games.values():
        g.viewport_w = 960
    for i in (0, 1):  # past the title screen to the Moon Man's choices
        shown = wait_for(lambda: st(i)["menu"]["shown"] or games[i].key("SPACE") or False, 360, step=3.0)  # the game's shader screen first after a driver cache reset
        check(bool(shown), f"copy {i}: the co-op choice is in the main menu", str(safe(lambda: st(i)["menu"]["entries"])))
    check(not safe(lambda: st(1)["character"]["source"] == "kept", False), "the client has no kept character")
    # A: the host hosts a new game
    games[0].key("6")
    wait_for(lambda: st(0)["menu"]["level"] == 1, 5)
    print(f"info: the host's choices {safe(lambda: st(0)['menu']['entries'])}", flush=True)
    games[0].key("2")
    check(bool(wait_for(lambda: st(0)["hostNew"] and st(0)["roleChosen"], 8)), "the host picked 'I'll host a new game' (key 2)")
    check(bool(wait_for(lambda: "CharacterCreation" in world(0), 60, 1.0)), "the host's game starts a New Game (the character creator)", world(0)[-50:])
    check(bool(make_character(0, games[0], (240, 170), ((175, 165), (175, 300)))), "the host made a character; its game starts", world(0)[-50:])
    check(through_intro(0), "the host is past the intro's opening conversation")
    # B: the client joins: its own new game first
    m = laid_out(1, 0)  # its number follows the game's own choices (4 without a save: 5 then), so a click on it
    if m.get("rects"):
        click(games[1], m["rects"][0])
    laid_out(1, 1)
    games[1].key("1")
    m = laid_out(1, 2)  # the list laid out first: a digit with no choice of ours goes to the game (its own New Game)
    print(f"info: the client's choices {m.get('entries')}", flush=True)
    games[1].key("1")
    check(bool(wait_for(lambda: st(1)["roleChosen"] and not st(1)["host"] and st(1)["newChar"], 30)), "the client joins; having no character, it makes one first")
    check(bool(wait_for(lambda: "CharacterCreation" in world(1), 60, 1.0)), "the client's game starts its own New Game", world(1)[-50:])
    check(bool(make_character(1, games[1], (240, 325), ((175, 230), (175, 365)))), "the client made a different character; its game starts", world(1)[-50:])
    through_intro(1, 90)
    # C: kept, then the host's game
    kept = wait_for(lambda: st(1)["newCharStage"] >= 2 or st(1)["joined"], 120, 2.0)
    check(bool(kept), "the client's new character is kept (a private save once the intro allows it)", str(safe(lambda: st(1)["character"])))
    t0 = time.time()
    joined = wait_for(lambda: st(1)["joined"] and not st(1)["newChar"] and world(1) == world(0) and "PS_Intro" in world(1), 300, 3.0)
    check(bool(joined), "the client plays in the host's game", f"after {time.time() - t0:.0f} s: {world(1)[-40:]} / {world(0)[-40:]}")
    check(safe(lambda: st(1)["character"]["source"] == "kept", False), "with its own new character", str(safe(lambda: st(1)["character"]["source"])))
    seen = wait_for(lambda: st(0).get("puppet", "none") != "none" and st(1).get("puppet", "none") != "none", 60, 2.0)
    check(bool(seen), "both see each other", f"{safe(lambda: st(0).get('puppet'))} / {safe(lambda: st(1).get('puppet'))}")
    # Out of the intro together: the player ship, then the first planet's landing, by the game's travel (the intro's own
    # scripted end is not played by the bots: 40 DebugAdvanceQuest steps of 0101_PQ_Intro left it in the intro, probe 126)
    import subprocess
    r = subprocess.run([sys.executable, "-I", "test_ship.py", "SHIP_0102_Landing_Pad"], cwd=str(Path(__file__).resolve().parent), capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if line.startswith(("PASS", "FAIL", "info")):
            print("  " + line, flush=True)
    check(r.returncode == 0 and "0101_JP" in world(0) and world(1) == world(0), "both left the intro together for the first planet", world(1)[-30:])
    check(safe(lambda: st(1)["character"]["source"] == "kept", False), "the client still plays its own new character there", str(safe(lambda: st(1)["character"]["source"])))
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]), int(sys.argv[2])))
