"""Start one test copy by hand the way gamepool does: detached, no inherited handles, same environment.
(relaunch.ps1 started the game with handles of a short-lived caller; the copy crashed in LoadMap after that caller
exited.) Usage: python -I relaunch.py <instance> <host|client|none> [port]"""
import os
import subprocess
import sys
from pathlib import Path

DETACHED_PROCESS, CREATE_NEW_PROCESS_GROUP, CREATE_BREAKAWAY_FROM_JOB = 0x8, 0x200, 0x01000000


def main(inst, role, port="27960"):
    d = Path(rf"C:\coop-anything\local\gamepool\instances\tow2\{inst}")
    exe = d / "Arkansas" / "Binaries" / "Win64" / "TheOuterWorlds2-Win64-Shipping.exe"
    if not (d / "instance.json").exists():
        sys.exit(f"instance {inst} is not provisioned")
    env = dict(os.environ, SteamAppId="1449110", SteamGameId="1449110", TOW2_SANDBOX_DIR=str(d / "sandbox"),
               TOW2_BRIDGE_PIPE=f"tow2-{inst}", GAMEPOOL_ROLE=role, GAMEPOOL_PORT=str(port), GAMEPOOL_PAIR="0")
    kw = dict(cwd=str(exe.parent), env=env, close_fds=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p = subprocess.Popen([str(exe), "-windowed", "-ResX=1280", "-ResY=720", "-nomovie"],
                             creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, **kw)
    except OSError:  # breakaway refused when our own job forbids it
        p = subprocess.Popen([str(exe), "-windowed", "-ResX=1280", "-ResY=720", "-nomovie"],
                             creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, **kw)
    print(f"started instance {inst} as {role}, pid {p.pid}")


if __name__ == "__main__":
    main(*sys.argv[1:])
