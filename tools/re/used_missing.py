#!/usr/bin/env python3
"""Which opcodes without a handler do the game's programs use?

usage: used_missing.py GAME_DIR [EXTRA_BP...]
Disassembles every ._bp of system.arc and sysprg.arc (and the extra loose
programs, e.g. those of a translation patch) with bpasm, maps the mnemonics
back to (family, opcode) through src/asm/opnames.c and intersects them with
the engine's --list-missing output for the 1.647 profile."""
import collections
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(ROOT, "engine", "openbgi")
BPASM = os.path.join(ENG, "bin", "bpasm")
SIM = os.path.join(ENG, "bin", "openbgi-wasmsim")
FAMS = {"sys7f": 0x7F, "sys": 0x80, "sys81": 0x81, "gfx0": 0x90, "gfx1": 0x91, "gfx2": 0x92,
        "snd": 0xA0, "ext0": 0xB0, "ext1": 0xC0, "eval": 0xD0, "ext2": 0xE0}
FAMNAME = {0x80: "80", 0x81: "81", 0x90: "90", 0x91: "91", 0x92: "92", 0xA0: "A0", 0xB0: "B0",
           0xC0: "C0", 0xD0: "D0", 0xE0: "E0", 0: "main"}

names = {}
for fam, op, lo, hi, name in re.findall(r'\{0x([0-9a-f]+), 0x([0-9a-f]+), (\w+), (\w+), "([^"]+)"\}',
                                        open(os.path.join(ENG, "src/asm/opnames.c")).read()):
    if lo in ("GEN_1_653", "GEN_1_654", "GEN_1_659", "GEN_1_662", "GEN_1_667", "GEN_1_669"):
        continue
    fam = int(fam, 16)
    prefix = {v: k for k, v in FAMS.items()}.get(fam)
    names[(prefix + "." if prefix else "") + name] = (fam, int(op, 16))

game = sys.argv[1]
out = subprocess.run([SIM, "--engine=1.647", "--list-missing"], cwd=game, capture_output=True, text=True).stdout
missing = set()
for line in out.splitlines():
    m = re.match(r"(\w+): ((?:[0-9A-F]{2} ?)+)$", line)
    if m:
        fam = 0 if m.group(1) == "main" else int(m.group(1), 16)
        missing |= {(fam, int(x, 16)) for x in m.group(2).split()}

progs = []
for arc in ("system.arc", "sysprg.arc"):
    listing = subprocess.run([BPASM, "-l", os.path.join(game, arc)], capture_output=True, text=True).stdout
    progs += [(arc, n) for n in re.findall(r"^\s*(\S+\._bp)", listing, re.M)]
progs += [(None, p) for p in sys.argv[2:]]

used = collections.defaultdict(set)
with tempfile.TemporaryDirectory() as tmp:
    for arc, name in progs:
        s = os.path.join(tmp, os.path.basename(name) + ".s")
        cmd = [BPASM, "-d", "-e", "1.647", "-o", s]
        cmd += ["--arc", os.path.join(game, arc), name] if arc else [name]
        subprocess.run(cmd, capture_output=True)
        if not os.path.exists(s):
            print("cannot disassemble", name)
            continue
        for line in open(s, encoding="utf-8", errors="replace"):
            tok = line.split(";")[0].split()
            if not tok:
                continue
            m = tok[0]
            key = names.get(m)
            if key is None:
                hm = re.match(r"(\w+)\.(?:0x)?([0-9a-f]{2})$", m)
                if hm and hm.group(1) in FAMS:
                    key = (FAMS[hm.group(1)], int(hm.group(2), 16))
                elif re.match(r"op_([0-9a-f]{2})$", m):
                    key = (0, int(m[3:], 16))
            if key and key in missing:
                used[key].add(os.path.basename(name))
for (fam, op), files in sorted(used.items()):
    print(f"{FAMNAME.get(fam, hex(fam))} {op:02X}: {', '.join(sorted(files))}")
print(len(used), "missing opcodes are used")
