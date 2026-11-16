"""Does touching the controller-less puppet crash the game? On a running pair in gameplay: the host stands 3 m in front of
the client; the client (whose world shows the host's puppet there) walks into it N times, then attacks it N times.
After each try the client's crash folder is checked. Usage: python -I probe_puppet_contact.py <host pid> <client pid> walk|hit [n]"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamewin import Game  # noqa: E402
from tow2 import call  # noqa: E402

CRASHES = Path(r"C:\coop-anything\local\gamepool\instances\tow2\1\sandbox\LocalAppData\Arkansas\Saved\Crashes")


def crashed():
    return CRASHES.exists() and any(CRASHES.iterdir())


def main(host_pid, client_pid, mode, n=5):
    host, client = Game(host_pid), Game(client_pid)
    # flat yard below the start ledge (0601): client here facing +Y, host 3 m ahead
    yaw = 90.0
    c = {"X": -126500.0, "Y": -42600.0, "Z": 5570.0}
    spot = {"X": c["X"], "Y": c["Y"] + 300, "Z": c["Z"]}
    call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": c, "bSweep": False, "bTeleport": True}}, inst=1)
    call("ue.call", {"ref": "pawn", "fn": "K2_SetActorLocation", "args": {"NewLocation": spot, "bSweep": False, "bTeleport": True}}, inst=0)
    time.sleep(2.0)
    print("client at", call("eval", {"expr": "pawn.K2_GetActorLocation()"}, inst=1)["value"], flush=True)
    pup = call("coop.status", {}, inst=1)["puppet"]
    print("puppet", pup, "at", call("eval", {"expr": pup + ".K2_GetActorLocation()"}, inst=1)["value"], flush=True)
    for i in range(n):
        call("ue.call", {"ref": "pc", "fn": "SetControlRotation", "args": {"NewRotation": {"Pitch": -5, "Yaw": yaw, "Roll": 0}}}, inst=1)
        if mode == "walk":
            client.key("W", hold_s=1.6)   # 6.4 m: through the puppet 3 m ahead
            time.sleep(0.5)
            client.key("S", hold_s=1.6)
        else:
            client.key("W", hold_s=0.5)   # close in to ~1 m, then swing
            for _ in range(3):
                client.click("left")
                time.sleep(0.7)
            client.key("S", hold_s=0.5)
        time.sleep(1.5)
        print(f"try {i + 1}: client crash report: {crashed()}", flush=True)
        if crashed():
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 5))
