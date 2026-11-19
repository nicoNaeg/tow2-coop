"""Public games on one Steam account (the join between two accounts needs two): the host copy makes an invisible Steam
lobby (not shown to friends, no rich presence set), describes it, searches (a search leaves out our own), then leaves
it. Usage: python -I test_lobby.py"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def main():
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok
        return ok

    r = call("coop.lobbyProbe", {}, inst=0, timeout_s=60)
    if check(r.get("ok") is True, "the Steam link and its matchmaking functions are there", str(r.get("why", ""))):
        check(r.get("lobby", "0") != "0", "a public game's lobby was made", f"{r.get('lobby')} in {r.get('madeIn', 0):.1f} s")
        check(r.get("searched") is True, "a search for public games of a version completed", f"{r.get('found')} found (our own is left out)")
        check(r.get("left") is True, "and the lobby was left")
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main())
