#!/usr/bin/env python3
"""Print or dump one entry of a BURIKO ARC20 archive (raw, as stored).
usage: arcentry.py ARCHIVE NAME [OUT]"""
import struct
import sys


def find(path, name):
    with open(path, "rb") as f:
        hdr = f.read(16)
        assert hdr[:12] == b"BURIKO ARC20", "not an ARC20 archive"
        count = struct.unpack_from("<I", hdr, 12)[0]
        table = f.read(count * 0x80)
        for i in range(count):
            e = table[i * 0x80:(i + 1) * 0x80]
            ename = e[:0x60].split(b"\0")[0].decode("cp932")
            if ename.lower() == name.lower():
                off, size = struct.unpack_from("<II", e, 0x60)
                f.seek(16 + count * 0x80 + off)
                return f.read(size)
    raise SystemExit(f"{name} not in {path}")


if __name__ == "__main__":
    data = find(sys.argv[1], sys.argv[2])
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(data)
    print(len(data), "bytes")
    for off in range(0, 0x40, 16):
        print(f"{off:04x}: {data[off:off + 16].hex(' ')}  {data[off:off + 16]!r}")
