#!/usr/bin/env python3
"""Count CompressedBG versions in every archive of a game directory.
usage: cbg_survey.py GAME_DIR"""
import collections
import glob
import os
import struct
import sys

counts = collections.Counter()
for path in sorted(glob.glob(os.path.join(sys.argv[1], "*.arc"))):
    with open(path, "rb") as f:
        hdr = f.read(16)
        if hdr[:12] != b"BURIKO ARC20":
            continue
        n = struct.unpack_from("<I", hdr, 12)[0]
        table = f.read(n * 0x80)
        base = 16 + n * 0x80
        for i in range(n):
            off = struct.unpack_from("<I", table, i * 0x80 + 0x60)[0]
            f.seek(base + off)
            h = f.read(0x30)
            if h[:15] == b"CompressedBG___":
                ver = struct.unpack_from("<H", h, 0x2E)[0]
                bpp = struct.unpack_from("<I", h, 0x14)[0]
                counts[(os.path.basename(path), ver, bpp)] += 1
for k, v in sorted(counts.items()):
    print(*k, v)
