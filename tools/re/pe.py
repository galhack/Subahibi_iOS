#!/usr/bin/env python3
"""Small static-analysis helper for the original BGI.exe (x86 PE).

Used to compare OpenBGI's behaviour with the engine build of this game
(Ethornell 1.647).  Commands:

  pe.py EXE str TEXT            find a Shift-JIS/ASCII string, its VA and the
                                instructions that reference it
  pe.py EXE dis VA [N]          disassemble N instructions from VA
  pe.py EXE refs VA             instructions whose immediate/displacement is VA
  pe.py EXE dword VA [N]        read N dwords at VA
  pe.py EXE func VA             disassemble from the function start containing
                                VA (heuristic: previous int3/ret padding) to ret
"""
import struct
import sys

import capstone
import pefile


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False
        text = [s for s in self.pe.sections if s.Name.startswith(b".text")][0]
        self.text_start = self.base + text.VirtualAddress
        self.text_end = self.text_start + text.Misc_VirtualSize

    def bytes_at(self, va, n):
        off = va - self.base
        return self.mem[off:off + n]

    def find_bytes(self, needle):
        out, start = [], 0
        while True:
            i = self.mem.find(needle, start)
            if i < 0:
                return out
            out.append(self.base + i)
            start = i + 1

    def code_refs(self, va):
        """Raw scan of .text for the little-endian VA (push/mov/cmp operands)."""
        needle = struct.pack("<I", va)
        lo, hi = self.text_start - self.base, self.text_end - self.base
        out, start = [], lo
        while True:
            i = self.mem.find(needle, start, hi)
            if i < 0:
                return out
            out.append(self.base + i)
            start = i + 1

    def dis(self, va, count=40):
        code = self.bytes_at(va, count * 15)
        lines = []
        for ins in self.md.disasm(code, va):
            lines.append(f"{ins.address:08x}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
            if len(lines) >= count:
                break
        return lines

    def func_start(self, va):
        """Walk back to a likely function start: after 0xCC/0xC3 padding."""
        p = va
        while p > self.text_start:
            b = self.bytes_at(p - 1, 1)[0]
            if b == 0xCC:
                return p
            p -= 1
        return va

    def func(self, va, limit=2000):
        start = self.func_start(va)
        code = self.bytes_at(start, limit * 15)
        lines = []
        for ins in self.md.disasm(code, start):
            lines.append(f"{ins.address:08x}  {ins.mnemonic} {ins.op_str}")
            if ins.mnemonic == "int3" or len(lines) >= limit:
                break
        return lines


def instr_containing(img, ref_va):
    """Find the instruction that contains the byte at ref_va (scan back up to 10)."""
    for back in range(1, 11):
        start = ref_va - back
        for ins in img.md.disasm(img.bytes_at(start, 16), start):
            if ins.address <= ref_va < ins.address + ins.size:
                return ins
            break
    return None


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "str":
        text = sys.argv[3]
        for enc in ("cp932", "ascii"):
            try:
                needle = text.encode(enc)
            except UnicodeEncodeError:
                continue
            for va in img.find_bytes(needle):
                # walk back to the start of the string
                s = va
                while img.bytes_at(s - 1, 1)[0] != 0:
                    s -= 1
                end = img.mem.find(b"\0", s - img.base)
                full = img.mem[s - img.base:end].decode("cp932", "replace")
                print(f"string {s:08x}: {full!r}")
                for r in img.code_refs(s):
                    ins = instr_containing(img, r)
                    desc = f"{ins.address:08x} {ins.mnemonic} {ins.op_str}" if ins else f"{r:08x} ?"
                    print(f"  ref {desc}")
            break
    elif cmd == "dis":
        va = int(sys.argv[3], 16)
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        print("\n".join(img.dis(va, n)))
    elif cmd == "refs":
        va = int(sys.argv[3], 16)
        for r in img.code_refs(va):
            ins = instr_containing(img, r)
            print(f"{ins.address:08x} {ins.mnemonic} {ins.op_str}" if ins else f"{r:08x} ?")
    elif cmd == "dword":
        va = int(sys.argv[3], 16)
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 16
        for i in range(n):
            print(f"{va + 4 * i:08x}: {struct.unpack('<I', img.bytes_at(va + 4 * i, 4))[0]:08x}")
    elif cmd == "func":
        print("\n".join(img.func(int(sys.argv[3], 16))))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()


def callers(img, target):
    """Every E8 rel32 call in .text whose destination is target."""
    import struct as _s
    lo, hi = img.text_start - img.base, img.text_end - img.base
    out, i = [], lo
    while True:
        i = img.mem.find(b"\xe8", i, hi)
        if i < 0:
            return out
        rel = _s.unpack_from("<i", img.mem, i + 1)[0]
        if img.base + i + 5 + rel == target:
            out.append(img.base + i)
        i += 1
