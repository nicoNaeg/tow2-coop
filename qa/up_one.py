"""Start one test copy alone (no pair), e.g. to let the graphics driver fill its shader cache for that copy.
Holds both game slots so nothing else starts a copy meanwhile; `gamepool down` stops it and frees them.

    python -I up_one.py <config> <instance>"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "gamepool"))
from gplib import launcher  # noqa: E402
from gplib.config import Config  # noqa: E402

cfg, i = Config.load(sys.argv[1]), int(sys.argv[2])
cfg.require_dirs()
if launcher.foreign_pids(cfg):
    sys.exit("the game is already running outside the test copies (maybe the owner's own game); nothing started")
print("slots", launcher.slot_acquire(launcher.DEFAULT_OWNER, slots=2))
sys.exit(launcher.start_instances(cfg, [i], launcher.DEFAULT_OWNER, launcher.load_adapter(cfg), 10, 900))
