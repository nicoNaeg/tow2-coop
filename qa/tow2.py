"""Call a test copy's core over the coop-anything bridge (pipe tow2-<i>).

    python -I tow2.py [-i N] METHOD [JSON | key=value ...]

A value is parsed as JSON when it can be (numbers, true, [..], {..}), else kept as a string.
Examples:  tow2.py core.status      tow2.py eval expr=pawn.K2_GetActorLocation()
           tow2.py ue.call ref=pc fn=IsLocalController"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "bridge"))
from bridge_client import Bridge  # noqa: E402


def parse(rest):
    if len(rest) == 1 and rest[0].lstrip().startswith("{"):
        return json.loads(rest[0])
    params = {}
    for kv in rest:
        k, _, v = kv.partition("=")
        try:
            params[k] = json.loads(v)
        except ValueError:
            params[k] = v
    return params


def call(method, params=None, inst=0, timeout_s=60.0):
    with Bridge.connect(f"pipe://tow2-{inst}", wait_s=5, timeout_s=timeout_s) as g:
        return g.call(method, params or {}, timeout_s=timeout_s)


def main(argv):
    inst, out = 0, None
    if argv[:1] == ["-i"]:
        inst, argv = int(argv[1]), argv[2:]
    if "--out" in argv:  # write the JSON result to a UTF-8 file instead of printing it
        k = argv.index("--out")
        out, argv = argv[k + 1], argv[:k] + argv[k + 2:]
    res = call(argv[0], parse(argv[1:]), inst)
    text = res if isinstance(res, str) else json.dumps(res, indent=1)
    if out:
        Path(out).write_text(text, encoding="utf-8")
    else:
        print(text)


if __name__ == "__main__":
    main(sys.argv[1:])
