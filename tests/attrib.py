#!/usr/bin/env python3
"""Dynamic attribution reporting for benchmark runs.

Modes:
  routines BASE_ASM BASE_PROFILE CAND_ASM CAND_PROFILE [TOP]
      Join glulxe VM_PROFILING output (per-function self_ops keyed by
      address) against an inform6 "$!asm" trace (routine names keyed by
      code offset) for two builds, and print the routines whose dynamic
      self-op counts differ the most.

  opcodes BASE_COUNT_LOG CAND_COUNT_LOG [TOP]
      Diff two glulxe-counted --opcode-histogram stderr logs and print
      the opcodes whose dispatch counts differ the most.

Both reports print candidate-minus-base deltas, largest magnitude
first. Lines are indented to sit under a benchmark's "ok" header.
"""
import re
import sys
import xml.etree.ElementTree as ET

OPCODE_NAMES = {
    0x00: "nop", 0x10: "add", 0x11: "sub", 0x12: "mul", 0x13: "div",
    0x14: "mod", 0x15: "neg", 0x18: "bitand", 0x19: "bitor",
    0x1A: "bitxor", 0x1B: "bitnot", 0x1C: "shiftl", 0x1D: "sshiftr",
    0x1E: "ushiftr", 0x20: "jump", 0x22: "jz", 0x23: "jnz", 0x24: "jeq",
    0x25: "jne", 0x26: "jlt", 0x27: "jge", 0x28: "jgt", 0x29: "jle",
    0x2A: "jltu", 0x2B: "jgeu", 0x2C: "jgtu", 0x2D: "jleu", 0x30: "call",
    0x31: "return", 0x32: "catch", 0x33: "throw", 0x34: "tailcall",
    0x40: "copy", 0x41: "copys", 0x42: "copyb", 0x44: "sexs",
    0x45: "sexb", 0x48: "aload", 0x49: "aloads", 0x4A: "aloadb",
    0x4B: "aloadbit", 0x4C: "astore", 0x4D: "astores", 0x4E: "astoreb",
    0x4F: "astorebit", 0x50: "stkcount", 0x51: "stkpeek",
    0x52: "stkswap", 0x53: "stkroll", 0x54: "stkcopy",
    0x70: "streamchar", 0x71: "streamnum", 0x72: "streamstr",
    0x73: "streamunichar", 0x100: "gestalt", 0x102: "getmemsize",
    0x104: "jumpabs", 0x110: "random", 0x111: "setrandom",
    0x120: "quit", 0x122: "restart", 0x123: "save", 0x124: "restore",
    0x125: "saveundo", 0x126: "restoreundo", 0x127: "protect",
    0x130: "glk", 0x140: "getstringtbl", 0x141: "setstringtbl",
    0x148: "getiosys", 0x149: "setiosys", 0x150: "linearsearch",
    0x151: "binarysearch", 0x152: "linkedsearch", 0x160: "callf",
    0x161: "callfi", 0x162: "callfii", 0x163: "callfiii",
    0x170: "mzero", 0x171: "mcopy", 0x178: "malloc", 0x179: "mfree",
    0x180: "accelfunc", 0x181: "accelparam",
}


def routine_offsets(asm_path):
    """Code offsets of routine headers: lines like ' 123  +0edf1  [ Name'."""
    rx = re.compile(r"\+([0-9a-f]+)\s+\[\s+(\S+)")
    out = {}
    for line in open(asm_path, errors="replace"):
        m = rx.search(line)
        if m:
            out[int(m.group(1), 16)] = m.group(2)
    return out


def profile_functions(profile_path):
    root = ET.parse(profile_path).getroot()
    return {
        int(f.get("addr"), 16): (int(f.get("self_ops")),
                                 int(f.get("call_count")))
        for f in root.iter("function")
    }


def named_self_ops(asm_path, profile_path):
    """Map routine name -> (self_ops, calls), fitting the code-area base
    (offset-to-address delta) that matches the most profile entries.
    Unmatched addresses (glk/accel dispatch stubs) carry no self ops."""
    offsets = routine_offsets(asm_path)
    prof = profile_functions(profile_path)
    offset_set = set(offsets)
    best_base, best_hits = 0, -1
    for addr in prof:
        for off in offsets:
            base = addr - off
            if base < 0:
                continue
            hits = sum(1 for a in prof if a - base in offset_set)
            if hits > best_hits:
                best_base, best_hits = base, hits
    named = {}
    dropped = 0
    for addr, (self_ops, calls) in prof.items():
        name = offsets.get(addr - best_base)
        if name is None:
            dropped += self_ops
        else:
            named[name] = (self_ops, calls)
    return named, dropped


def report_routines(argv):
    base_asm, base_prof, cand_asm, cand_prof = argv[:4]
    top = int(argv[4]) if len(argv) > 4 else 10
    base, base_dropped = named_self_ops(base_asm, base_prof)
    cand, cand_dropped = named_self_ops(cand_asm, cand_prof)
    if base_dropped or cand_dropped:
        print("      routine attribution incomplete: dropped "
              f"{base_dropped}/{cand_dropped} base/candidate self ops")
    rows = []
    for name in set(base) | set(cand):
        bs, bc = base.get(name, (0, 0))
        cs, cc = cand.get(name, (0, 0))
        if cs != bs:
            rows.append((cs - bs, name, bs, cs, bc, cc))
    rows.sort(key=lambda r: -abs(r[0]))
    for delta, name, bs, cs, bc, cc in rows[:top]:
        calls = str(bc) if bc == cc else f"{bc}->{cc}"
        print(f"      {delta:+8d}  {name} ({bs} -> {cs} self ops, "
              f"{calls} calls)")
    if not rows:
        print("      routine self-op counts are identical")


def histogram(count_log):
    out = {}
    rx = re.compile(r"GLULXE_OPCODE_COUNT_0x([0-9A-Fa-f]+)=(\d+)")
    for line in open(count_log, errors="replace"):
        m = rx.match(line)
        if m:
            out[int(m.group(1), 16)] = int(m.group(2))
    return out


def report_opcodes(argv):
    base_log, cand_log = argv[:2]
    top = int(argv[2]) if len(argv) > 2 else 10
    base, cand = histogram(base_log), histogram(cand_log)
    rows = []
    for op in set(base) | set(cand):
        delta = cand.get(op, 0) - base.get(op, 0)
        if delta:
            rows.append((delta, op))
    rows.sort(key=lambda r: -abs(r[0]))
    for delta, op in rows[:top]:
        name = OPCODE_NAMES.get(op, f"0x{op:03X}")
        print(f"      {delta:+8d}  {name} "
              f"({base.get(op, 0)} -> {cand.get(op, 0)})")
    if not rows:
        print("      opcode histograms are identical")


def main():
    if len(sys.argv) >= 6 and sys.argv[1] == "routines":
        report_routines(sys.argv[2:])
    elif len(sys.argv) >= 4 and sys.argv[1] == "opcodes":
        report_opcodes(sys.argv[2:])
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
