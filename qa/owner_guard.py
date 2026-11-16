"""Prove a test run left the owner's real game state alone.

    python -I owner_guard.py snapshot <file.json>     record hashes before a run
    python -I owner_guard.py compare <file.json>      exit 1 and list every difference after a run

Covers the real save folder, the game's real config folder, the game's Steam userdata folder, and the
top-level folder names of LocalAppData and Documents (a new folder there means the game wrote outside the
sandbox). Paths come from the environment, never from the game copy."""
import hashlib, json, os, sys
from pathlib import Path

HOME = Path(os.environ["USERPROFILE"])
LOCAL = Path(os.environ["LOCALAPPDATA"])
STEAM_USERDATA = Path(r"C:\Program Files (x86)\Steam\userdata")
APP_ID = "1449110"


def tree(root: Path) -> dict:
    """{relative path: [size, sha256]} for every file under root; {} when root is missing."""
    out = {}
    if root.is_dir():
        for p in sorted(root.rglob("*")):
            if p.is_file():
                out[str(p.relative_to(root))] = [p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest()]
    return out


def names(root: Path) -> list:
    return sorted(p.name for p in root.iterdir()) if root.is_dir() else []


def snapshot() -> dict:
    snap = {
        "saves": tree(HOME / "Saved Games" / "TheOuterWorlds2"),
        "saved_games_dir_names": names(HOME / "Saved Games"),
        "config_exists": (LOCAL / "Arkansas").exists(),
        "config": tree(LOCAL / "Arkansas"),
        "localappdata_names": names(LOCAL),
        "documents_names": names(HOME / "Documents"),
        "my_games_names": names(HOME / "Documents" / "My Games"),
        "steam_userdata": {},
    }
    if STEAM_USERDATA.is_dir():
        for user in STEAM_USERDATA.iterdir():
            snap["steam_userdata"][user.name] = tree(user / APP_ID)
    return snap


def compare(before: dict, after: dict) -> list:
    diffs = []
    for key in before:
        a, b = before[key], after.get(key)
        if a == b:
            continue
        if isinstance(a, dict) and isinstance(b, dict):
            for k in sorted(set(a) | set(b)):
                if a.get(k) != b.get(k):
                    diffs.append(f"{key}: {k}: {a.get(k)} -> {b.get(k)}")
        elif isinstance(a, list) and isinstance(b, list):
            diffs.append(f"{key}: added {sorted(set(b) - set(a))} removed {sorted(set(a) - set(b))}")
        else:
            diffs.append(f"{key}: {a} -> {b}")
    return diffs


def main(argv):
    if len(argv) != 3 or argv[1] not in ("snapshot", "compare"):
        print(__doc__)
        return 2
    path = Path(argv[2])
    if argv[1] == "snapshot":
        snap = snapshot()
        path.write_text(json.dumps(snap, indent=1))
        print(f"snapshot: {len(snap['saves'])} save files, config folder exists: {snap['config_exists']}, "
              f"steam userdata files: {sum(len(v) for v in snap['steam_userdata'].values())} -> {path}")
        return 0
    diffs = compare(json.loads(path.read_text()), snapshot())
    for d in diffs:
        print("CHANGED", d)
    print("OWNER STATE:", "UNCHANGED" if not diffs else f"CHANGED ({len(diffs)})")
    return 1 if diffs else 0


if __name__ == "__main__":
    # self-check of compare(): identical snapshots give no diff, a changed hash and a new folder name are both named
    s = {"saves": {"a": [1, "x"]}, "localappdata_names": ["A"], "config_exists": False}
    assert compare(s, json.loads(json.dumps(s))) == []
    t = {"saves": {"a": [1, "y"]}, "localappdata_names": ["A", "Arkansas"], "config_exists": True}
    assert len(compare(s, t)) == 3, compare(s, t)
    sys.exit(main(sys.argv))
