"""Run Lua inside a running test copy through the AgentBridge UE4SS mod.

    python -I bridge.py <instance dir> "print(1+1)"
    python -I bridge.py <instance dir> -f script.lua [--timeout 60]

Prints the reply (id, ok, printed lines, return value or error). Exit code 0 when ok, 1 on a Lua error,
2 on timeout (the game is loading, hung, or the bridge is not installed)."""
import argparse, os, sys, time
from pathlib import Path


def call(instance_dir, code, timeout=30.0):
    d = Path(instance_dir) / "sandbox" / "bridge"
    d.mkdir(parents=True, exist_ok=True)
    cid = str(int(time.time() * 1000) % 1_000_000_000)
    (d / "out.txt").unlink(missing_ok=True)
    tmp = d / "cmd.lua.tmp"
    tmp.write_text(f"-- id: {cid}\n{code}\n", encoding="utf-8")
    os.replace(tmp, d / "cmd.lua")
    end = time.time() + timeout
    while time.time() < end:
        try:
            text = (d / "out.txt").read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            text = ""
        if text.startswith(f"id: {cid}\n"):
            return text
        time.sleep(0.1)
    (d / "cmd.lua").unlink(missing_ok=True)  # a command nobody picked up must not run later by surprise
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("instance_dir")
    ap.add_argument("code", nargs="?")
    ap.add_argument("-f", "--file")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("-D", "--define", action="append", default=[], metavar="NAME=VALUE",
                    help="set a Lua global string before the code runs (no shell quoting needed)")
    a = ap.parse_args(argv)
    code = Path(a.file).read_text(encoding="utf-8") if a.file else a.code
    if not code:
        ap.error("give Lua code or -f FILE")
    for d in a.define:
        name, _, value = d.partition("=")
        if not name.isidentifier():
            ap.error(f"bad --define name {name!r}")
        code = f"{name} = {value!r}\n" + code  # Python repr of a str is a valid Lua string literal for plain text
    reply = call(a.instance_dir, code, a.timeout)
    if reply is None:
        print(f"TIMEOUT after {a.timeout:.0f} s (no reply from the bridge)")
        return 2
    sys.stdout.write(reply)
    return 0 if "\nok: true\n" in reply else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
