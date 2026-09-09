"""Scan CrimsonDesert.exe for instructions that store a constant into [reg+0x350].

A vein's gimmick component goes from +0x350 = 3 to 5 when it breaks. If the game
writes that as an immediate, the encoding is fixed enough to grep for:

    C6 /r disp32 imm8    mov byte  ptr [reg+0x350], imm8
    C7 /r disp32 imm32   mov dword ptr [reg+0x350], imm32

with disp32 = 50 03 00 00. ModRM with mod=10 (disp32) is 0x80..0xBF.
"""
import os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "codex", "python-deps"))
import capstone  # noqa: E402

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IMAGE_BASE = 0x140000000
DISP = bytes([0x50, 0x03, 0x00, 0x00])   # 0x350 little-endian


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


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "5"
    want = None if arg == "any" else int(arg)
    data = open(EXE, "rb").read()
    secs = sections(data)
    exec_secs = [s for s in secs if s[0] in (".text", ".text0", ".text1") or s[4] > 0x1000000]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    print("scanning for stores into [reg+0x350], value filter: %s" % (want if want is not None else "any"))
    print("sections: " + ", ".join("%s(%.1f MB)" % (s[0], s[4] / 1048576.0) for s in secs if s[4]))
    print()

    hits = []
    for name, vaddr, vsize, raddr, rsize in secs:
        if not rsize:
            continue
        blob = data[raddr:raddr + rsize]
        start = 0
        while True:
            i = blob.find(DISP, start)
            if i < 0:
                break
            start = i + 1
            # Walk back a few bytes and try to decode an instruction that lands here.
            for back in range(2, 12):
                off = i - back
                if off < 0:
                    continue
                try:
                    ins = next(md.disasm(blob[off:off + 20], IMAGE_BASE + vaddr + off, 1))
                except StopIteration:
                    continue
                if ins.size <= back:
                    continue
                op = ins.op_str
                if "0x350" not in op:
                    continue
                if not ins.mnemonic.startswith("mov"):
                    continue
                # Stores only: the destination must be the memory operand.
                if not op.startswith("byte ptr [") and not op.startswith("dword ptr [") \
                   and not op.startswith("qword ptr [") and not op.startswith("word ptr ["):
                    continue
                if want is not None:
                    m = re.search(r",\s*(0x[0-9a-f]+|\d+)$", op)
                    if not m:
                        continue
                    try:
                        val = int(m.group(1), 0)
                    except ValueError:
                        continue
                    if val != want:
                        continue
                hits.append((name, ins.address - IMAGE_BASE, ins.mnemonic, op))
                break

    print("%d store instruction(s) touching [reg+0x350]:" % len(hits))
    for name, rva, mn, op in hits:
        print("  %-8s RVA 0x%07X   %s %s" % (name, rva, mn, op))
    if not hits:
        print("  none. The 5 is probably written from a register, or the field is not a")
        print("  byte/dword immediate store, or +0x350 is not the component's own offset.")


if __name__ == "__main__":
    main()
