#!/usr/bin/env python3
"""Compare the opcode set dumped by optables.py with OpenBGI's kOpSet for a
generation index (bit number in src/vm/opset.c)."""
import re
import sys

opset_c, dump, gen = sys.argv[1], sys.argv[2], int(sys.argv[3])
src = open(opset_c).read()
blocks = re.split(r"//\s*(main table|family \w+)\n", src)[1:]
openbgi = {}
for name, body in zip(blocks[0::2], blocks[1::2]):
    vals = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{6})", body)[:256]]
    fam = "main" if name == "main table" else name.split()[1]
    openbgi[fam] = {i for i, v in enumerate(vals) if v >> gen & 1}
for line in open(dump):
    m = re.match(r"(\w+) \(table \w+\) \d+: ?(.*)", line)
    if not m:
        continue
    fam = m.group(1)
    mine = {int(x, 16) for x in m.group(2).split()}
    theirs = openbgi.get(fam, set())
    extra, missing = sorted(mine - theirs), sorted(theirs - mine)
    print(f"{fam}: +{' '.join(f'{x:02X}' for x in extra)}  -{' '.join(f'{x:02X}' for x in missing)}")
