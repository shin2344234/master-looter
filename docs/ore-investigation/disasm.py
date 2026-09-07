"""Disassemble a function out of CrimsonDesert.exe by RVA.

Usage: python disasm.py <rva hex, e.g. 29E45D0> [max instructions]
The exe has relocations stripped and no ASLR, ImageBase 0x140000000, so an RVA
here is a fixed virtual address at runtime too.
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "codex", "python-deps"))
import capstone  # noqa: E402

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IMAGE_BASE = 0x140000000


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    tbl = pe + 24 + opt
    out = []
    for i in range(nsec):
        e = tbl + i * 40
        name = data[e:e + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, e + 8)
        out.append((name, vaddr, vsize, raddr, rsize))
    return out


def rva_to_off(secs, rva):
    for name, vaddr, vsize, raddr, rsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return raddr + (rva - vaddr), name
    return None, None


def main():
    rva = int(sys.argv[1], 16)
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    data = open(EXE, "rb").read()
    secs = sections(data)
    off, sec = rva_to_off(secs, rva)
    if off is None:
        print("RVA 0x%X is not in any section" % rva)
        return
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    print("RVA 0x%X in %s (file offset 0x%X), VA 0x%X\n" % (rva, sec, off, IMAGE_BASE + rva))
    n = 0
    for ins in md.disasm(data[off:off + limit * 16], IMAGE_BASE + rva):
        print("  +0x%07X: %-9s %s" % (ins.address - IMAGE_BASE, ins.mnemonic, ins.op_str))
        n += 1
        if ins.mnemonic == "ret" or n >= limit:
            break


if __name__ == "__main__":
    main()
