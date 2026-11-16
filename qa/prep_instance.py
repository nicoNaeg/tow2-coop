"""Prepare a test copy's private sandbox: game settings that skip the first-run screens and run cheap (two copies
share one GPU), and fresh copies of the test saves. Every write goes through gamepool's guard.

    python -I prep_instance.py --config tow2.gamepool.json --instance 1 [--saves <folder with TheOuterWorlds2>]"""
import argparse
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HUB = HERE.parents[2]
sys.path.insert(0, str(HUB / "tools" / "gamepool"))
from gplib.config import Config  # noqa: E402
from gplib.instances import guard_write  # noqa: E402

TEMPLATE = HERE / "GameUserSettings.template.ini"
LOW = {  # [ScalabilityGroups]: 0 = low; resolution scale in percent
    "sg.ResolutionQuality": "50", "sg.ViewDistanceQuality": "0", "sg.AntiAliasingQuality": "0", "sg.ShadowQuality": "0",
    "sg.GlobalIlluminationQuality": "0", "sg.ReflectionQuality": "0", "sg.PostProcessQuality": "0", "sg.TextureQuality": "0",
    "sg.EffectsQuality": "0", "sg.FoliageQuality": "0", "sg.ShadingQuality": "0", "sg.LandscapeQuality": "0",
}


VRAM_BUDGET = {  # per copy, [SystemSettings] of the copy's own Engine.ini
    "r.Streaming.PoolSize": "600", "r.Streaming.UseFixedPoolSize": "1", "r.Streaming.LimitPoolSizeToVRAM": "1",
    "r.Nanite.Streaming.StreamingPoolSize": "256", "r.SkinCache.SceneMemoryLimitInMB": "128",
}
# Tried against the start-up RAM bursts of run 25-29 (13-19 GB 20 s after start), none prevented them, so not kept:
# r.ShaderPipelineCache.Enabled=0, r.pso.PrecompileThreadPoolSizeMax=2, deleting Saved/*.upipelinecache, -corelimit=8.


def set_keys(text, section, kv):
    lines = text.splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.strip() == f"[{section}]")
    except StopIteration:
        lines += ["", f"[{section}]"]
        start = len(lines) - 1
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
    for k, v in kv.items():
        hit = next((i for i in range(start + 1, end) if lines[i].split("=", 1)[0] == k), None)
        if hit is None:
            lines.insert(end, f"{k}={v}")
            end += 1
        else:
            lines[hit] = f"{k}={v}"
    return "\n".join(lines) + "\n"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--instance", type=int, required=True)
    ap.add_argument("--saves", default=str(HUB / "local" / "the-outer-worlds-2" / "test-saves" / "TheOuterWorlds2"))
    ap.add_argument("--keep-saves", action="store_true", help="leave the sandbox saves as they are")
    a = ap.parse_args(argv)
    cfg = Config.load(a.config)
    sb = cfg.instance_dir(a.instance) / "sandbox"
    ini = sb / "LocalAppData" / "Arkansas" / "Saved" / "Config" / "Windows" / "GameUserSettings.ini"
    text = TEMPLATE.read_text(encoding="utf-8-sig")
    # first-run screens off; muted, so copies running behind the owner's windows make no sound
    text = set_keys(text, "/Script/Arkansas.IndianaGameUserSettings", {"bIsFirstTimeFrontEndFlow": "False", "MasterSoundVolume": "0.000000"})
    text = set_keys(text, "ScalabilityGroups", LOW)
    text = set_keys(text, "/Script/Engine.GameUserSettings", {"FrameRateLimit": "60.000000", "bUseVSync": "False"})
    assert re.search(r"^bIsFirstTimeFrontEndFlow=False$", text, re.M)
    guard_write(cfg, ini)
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text(text, encoding="utf-8")
    # Two copies share one 8 GB laptop GPU: without a budget the host stopped with "Out of video memory" (run 17).
    engine = ini.parent / "Engine.ini"
    guard_write(cfg, engine)
    engine.write_text("[SystemSettings]\n" + "".join(f"{k}={v}\n" for k, v in VRAM_BUDGET.items()), encoding="utf-8")
    done = [f"settings -> {ini}", "VRAM budget -> Engine.ini"]
    if not a.keep_saves:
        dst = sb / "SavedGames" / "TheOuterWorlds2"
        guard_write(cfg, dst)
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(a.saves, dst)
        for f in dst.rglob("*"):
            if f.is_file():
                f.chmod(0o666)  # the backup is read-only; the game rotates autosaves in place
        done.append(f"{sum(1 for f in dst.rglob('*') if f.is_file())} save files -> {dst}")
    print(f"instance {a.instance}: " + "; ".join(done))


if __name__ == "__main__":
    main(sys.argv[1:])
