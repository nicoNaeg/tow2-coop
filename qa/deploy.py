"""Install the test-copy mods into a gamepool instance: the sandbox proxy (dsound.dll), and optionally UE4SS
with the AgentBridge mod. Every write goes through gamepool's guard (never through a hard link into the
install, never into protected folders).

    python -I deploy.py --config tow2.gamepool.json --instance 0 [--ue4ss <pristine UE4SS folder>] [--no-ue4ss]
"""
import argparse, shutil, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
HUB = REPO.parent.parent  # local/tow2-coop -> local -> hub root
sys.path.insert(0, str(HUB / "tools" / "gamepool"))
from gplib.config import Config  # noqa: E402
from gplib.instances import guard_write  # noqa: E402

WIN64 = Path("Arkansas/Binaries/Win64")
MODS_TXT = "AgentBridge : 1\n\n; Built-in keybinds, do not move up!\nKeybinds : 1\n"


def put(cfg, src: Path, dst: Path):
    guard_write(cfg, dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()  # a fresh file, never an edit in place
    shutil.copy2(src, dst)


def put_text(cfg, dst: Path, text: str):
    guard_write(cfg, dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.chmod(0o666)  # may be read-only (steam_appid.txt)
        dst.unlink()
    dst.write_text(text, encoding="utf-8")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--instance", type=int, required=True)
    ap.add_argument("--ue4ss", help="pristine UE4SS release folder (holds dwmapi.dll and ue4ss/)")
    ap.add_argument("--no-ue4ss", action="store_true", help="remove UE4SS from the instance")
    ap.add_argument("--no-core", action="store_true", help="remove tow2coop.dll from the instance")
    ap.add_argument("--keep-ini", action="store_true", help="keep a tow2coop.ini placed in the instance (settings-file test)")
    ap.add_argument("--key", help="write tow2coop.ini with only this shared key (key test)")
    a = ap.parse_args(argv)
    cfg = Config.load(a.config)
    inst = cfg.instance_dir(a.instance)
    if not (inst / "instance.json").exists():
        sys.exit(f"instance {a.instance} is not provisioned: {inst}")
    win64 = inst / WIN64
    put(cfg, REPO / "bin" / "dsound.dll", win64 / "dsound.dll")
    (inst / "sandbox" / "bridge").mkdir(parents=True, exist_ok=True)
    # The game deletes steam_appid.txt when it closes normally; without it the next start asks Steam to launch the
    # owner's real install (Steam's launch-option dialog) and quits with code 53. Rewrite it and make it read-only.
    for name, text in cfg["write_files"].items():
        put_text(cfg, inst / name, cfg.fmt(text, a.instance))
        (inst / name).chmod(0o444)
    done = ["dsound.dll", "steam_appid.txt"]
    if a.key:
        put_text(cfg, win64 / "tow2coop.ini", f"[coop]\nkey={a.key}\n")
        done.append(f"tow2coop.ini key={a.key}")
    elif (win64 / "tow2coop.ini").exists() and not a.keep_ini:  # a player's settings file; test copies use the environment
        guard_write(cfg, win64 / "tow2coop.ini")
        (win64 / "tow2coop.ini").unlink()
        done.append("tow2coop.ini removed")
    core = REPO / "bin" / "core" / "tow2coop.dll"
    if a.no_core:
        if (win64 / "tow2coop.dll").exists():
            guard_write(cfg, win64 / "tow2coop.dll")
            (win64 / "tow2coop.dll").unlink()
        done.append("core removed")
    elif core.exists():
        put(cfg, core, win64 / "tow2coop.dll")
        done.append("tow2coop.dll")
    if a.no_ue4ss:
        for p in (win64 / "dwmapi.dll",):
            if p.exists():
                guard_write(cfg, p)
                p.unlink()
        ue = win64 / "ue4ss"
        if ue.exists():
            for p in sorted(ue.rglob("*"), reverse=True):
                guard_write(cfg, p)
                p.unlink() if p.is_file() else p.rmdir()
        done.append("UE4SS removed")
    elif a.ue4ss:
        src = Path(a.ue4ss)
        put(cfg, src / "dwmapi.dll", win64 / "dwmapi.dll")
        for f in (src / "ue4ss").rglob("*"):
            if f.is_file():
                put(cfg, f, win64 / "ue4ss" / f.relative_to(src / "ue4ss"))
        for f in (REPO / "ue4ss").rglob("*"):
            if f.is_file():
                put(cfg, f, win64 / "ue4ss" / "Mods" / f.relative_to(REPO / "ue4ss"))
        for f in (REPO / "ue4ss-config").glob("*.ini"):  # game-specific layouts (VTableLayout.ini) next to UE4SS.dll
            put(cfg, f, win64 / "ue4ss" / f.name)
        put_text(cfg, win64 / "ue4ss" / "Mods" / "mods.txt", MODS_TXT)
        done.append("UE4SS + AgentBridge")
    print(f"instance {a.instance}: deployed {', '.join(done)} into {win64}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
