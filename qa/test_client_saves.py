"""The joined player's own saves are safe: the client's autosave and quicksave slots ("Autosave02", ...) are shared with
its owner's own campaign, so a co-op autosave there would overwrite them. On the joined client this triggers every way
the game saves (autosave default and travel, quicksave, a manual save) and checks that no save file outside the
received CoopHost_* folders was written or changed. Run on a pair where the client (copy 1) has joined the host.
Mode "session" (end of a suite): the client's saves outside CoopHost_* still equal the saves it started with.
Usage: python -I test_client_saves.py <client sandbox dir> [session <folder the saves were copied from>]"""
import hashlib
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402


def snapshot(saves):
    return {str(f.relative_to(saves)): hashlib.sha256(f.read_bytes()).hexdigest()
            for f in saves.rglob("*") if f.is_file() and not f.relative_to(saves).parts[0].startswith("CoopHost")}


def main(sandbox, source=None):
    fails = 0

    def check(ok, what, detail=""):
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
        fails += not ok

    saves = Path(sandbox) / "SavedGames" / "TheOuterWorlds2"
    if source:  # a queued autosave may land later: compare the whole session against the original copies
        orig, now = snapshot(Path(source)), snapshot(saves)
        changed = sorted(k for k in orig if now.get(k) != orig[k])
        new = sorted(k for k in now if k not in orig)
        check(not changed, "after the whole session the client's own saves are unchanged", ", ".join(changed)[:200])
        check(not new, "after the whole session the client wrote no save outside CoopHost_*", ", ".join(new)[:200])
        print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
        return fails
    before = snapshot(saves)
    sgm = call("ue.find", {"class": "SaveGameManager"}, inst=1)[0].split()[0]
    tries = {
        "autosave": call("ue.call", {"ref": sgm, "fn": "Autosave", "args": {"AutoSaveType": "Default", "bIgnoreSuperNova": True}}, inst=1),
        "travel autosave": call("ue.call", {"ref": sgm, "fn": "Autosave", "args": {"AutoSaveType": "Travel", "bIgnoreSuperNova": True}}, inst=1),
        "quicksave": call("ue.call", {"ref": sgm, "fn": "Quicksave", "args": {}}, inst=1),
        "manual save": call("ue.call", {"ref": sgm, "fn": "SaveGame", "args": {"Filename": "ClientSaveTest", "bIgnoreSuperNova": True, "SaveGameType": "Standard"}}, inst=1),
    }
    call("ue.call", {"ref": sgm, "fn": "RequestAutosave", "args": {"bAllowOnSuperNova": True}}, inst=1)
    print("save calls on the client:", {k: v.get("ReturnValue") for k, v in tries.items()})
    time.sleep(12)  # an async save would be on disk by now
    after = snapshot(saves)
    changed = sorted(k for k in before if after.get(k) != before[k])
    new = sorted(k for k in after if k not in before)
    check(not changed, "the client's own save files are unchanged", ", ".join(changed)[:200])
    check(not new, "the client wrote no save outside CoopHost_*", ", ".join(new)[:200])
    print(f"RESULT: {'PASS' if not fails else 'FAIL'} ({fails} failure(s))")
    return fails


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "session" else None))
