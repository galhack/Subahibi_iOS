#!/usr/bin/env python3
"""Insert a generation bit into OpenBGI's src/vm/opset.c.

usage: insert_gen.py opset.c DUMP BIT [BASE_BIT]
Bits >= BIT move up by one; the new bit BIT is set from DUMP (the output of
optables.py) for the families the dump has, and copied from BASE_BIT for the
others (the user-defined FF family and the 7F family, which the dump of a
pre-1.667 build cannot show)."""
import re
import sys

path, dump, bit = sys.argv[1], sys.argv[2], int(sys.argv[3])
base = int(sys.argv[4]) if len(sys.argv) > 4 else bit - 1

sets = {}
for line in open(dump):
    m = re.match(r"(\w+) \(table \w+\) \d+: ?(.*)", line)
    if m and m.group(1) != "FF":
        sets[m.group(1)] = {int(x, 16) for x in m.group(2).split()}

src = open(path).read()
out, fam, idx = [], None, 0
for line in src.splitlines(keepends=True):
    m = re.match(r"\s*//\s*(main table|family (\w+))\s*$", line)
    if m:
        fam = "main" if m.group(1) == "main table" else m.group(2)
        idx = 0
        out.append(line)
        continue
    vals = re.findall(r"0x([0-9a-f]{6})", line)
    if fam and len(vals) == 8 and line.strip().startswith("0x"):
        new = []
        for v in vals:
            v = int(v, 16)
            low, high = v & ((1 << bit) - 1), v >> bit
            if fam in sets:
                b = 1 if idx in sets[fam] else 0
            else:
                b = v >> base & 1
            new.append(low | b << bit | high << (bit + 1))
            idx += 1
        comment = line[line.index("//"):] if "//" in line else "\n"
        out.append("\t\t" + ", ".join(f"0x{v:06x}" for v in new) + ", " + comment)
        continue
    out.append(line)
open(path, "w").write("".join(out))
