"""Find UObject::ProcessEvent's vtable slot in a running test copy (read-only).
ProcessEvent tests UFunction::FunctionFlags for FUNC_Native: `test dword ptr [reg+0xB0], 0x400`
(F7 modrm B0 00 00 00 00 04 00 00), the fingerprint Dumper-7 uses. Prints every slot whose function
carries it within its first 0x800 bytes, plus how often each function repeats in the table.
Usage: python -I find_processevent.py <pid> <UObject address hex> [slots] [flags offset hex]"""
import re, sys
from collections import Counter
from vtdump import k32, main_module, read, PROCESS_VM_READ, PROCESS_QUERY_INFORMATION


def main(argv):
    pid, obj = int(argv[1]), int(argv[2], 16)
    slots = int(argv[3]) if len(argv) > 3 else 160
    flags_off = int(argv[4], 16) if len(argv) > 4 else 0xB0
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    base, size = main_module(h)
    vt = int.from_bytes(read(h, obj, 8), "little")
    raw = read(h, vt, 8 * slots)
    funcs = [int.from_bytes(raw[8 * i:8 * i + 8], "little") for i in range(slots)]
    repeats = Counter(funcs)
    pat = re.compile(rb"\xF7[\x80-\xBF]" + re.escape(flags_off.to_bytes(4, "little")) + rb"\x00\x04\x00\x00", re.S)
    for i, f in enumerate(funcs):
        if not base <= f < base + size:
            continue
        try:
            code = read(h, f, 0x800)
        except OSError:
            continue
        m = pat.search(code)
        if m:
            print(f"slot {i} (+{8 * i:#x}) rva {f - base:#x}: FUNC_Native test at +{m.start():#x}, used by {repeats[f]} slot(s)")
    stub, n = repeats.most_common(1)[0]
    print(f"most repeated function: rva {stub - base:#x} in {n} slots (an empty stub)")


if __name__ == "__main__":
    main(sys.argv)
