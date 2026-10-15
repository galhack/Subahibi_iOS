#!/usr/bin/env python3
"""Recover the opcode tables of BGI.exe.

Each family dispatcher (the function that reports "未定義の…命令 $90%02X")
reads the sub-opcode byte and loads `[eax*4 + TABLE]`; a NULL slot is an
undefined instruction.  Prints the defined opcodes of every family, and with
-v the handler address of each."""
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pe import Image, instr_containing  # noqa: E402

FAMILIES = {
    "main": "未定義の基本命令", "80": "$80%02X", "81": "$81%02X", "90": "$90%02X",
    "91": "$91%02X", "92": "$92%02X", "A0": "$A0%02X", "B0": "$B0%02X",
    "C0": "$C0%02X", "D0": "$D0%02X", "E0": "$E0%02X", "FF": "ユーザー定義命令 $FF",
}


def string_va(img, text):
    for va in img.find_bytes(text.encode("cp932")):
        s = va
        while img.bytes_at(s - 1, 1)[0] != 0:
            s -= 1
        return s
    return None


def table_of(img, ref_ins_va):
    """Search back from the message push for `[reg*4 + imm]`."""
    for back in range(0x10, 0x80):
        lines = img.dis(ref_ins_va - back, 40)
        for ln in lines:
            m = re.search(r"\[(?:e\w\w)\*4 \+ (0x[0-9a-f]+)\]", ln)
            if m and int(ln.split()[0], 16) < ref_ins_va:
                return int(m.group(1), 16)
    return None


def tables(img):
    out = {}
    for fam, text in FAMILIES.items():
        sva = string_va(img, text)
        for ref in img.code_refs(sva):
            ins = instr_containing(img, ref)
            t = table_of(img, ins.address)
            if t:
                out.setdefault(fam, t)
    return out


def main():
    img = Image(sys.argv[1])
    verbose = "-v" in sys.argv
    for fam, t in tables(img).items():
        entries = struct.unpack("<256I", img.bytes_at(t, 1024))
        defined = [i for i, e in enumerate(entries) if e]
        print(f"{fam} (table {t:08x}) {len(defined)}: " + " ".join(f"{i:02X}" for i in defined))
        if verbose:
            for i in defined:
                print(f"    {fam} {i:02X} -> {entries[i]:08x}")


if __name__ == "__main__":
    main()
