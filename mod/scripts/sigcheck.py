r"""Check every kSig_ pattern in src/loot/signatures.h against the installed exe.

    py -3 sigcheck.py [path\to\CrimsonDesert.exe]

Reads the PE from disk, scans all sections, and prints hit counts with RVAs so
a game patch can be diagnosed without launching the game. Also confirms the RTTI
and table-name strings exist and resolves the two globals behind the
desc_mask+queue site.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HDR = os.path.join(HERE, "..", "src", "loot", "signatures.h")


def default_exe():
    """The installed CrimsonDesert.exe: every Steam library the registry knows, then the usual folders."""
    libs = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            steam = winreg.QueryValueEx(k, "SteamPath")[0].replace("/", "\\")
        libs.append(steam)
        vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
        if os.path.exists(vdf):
            for m in re.finditer(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="replace").read()):
                libs.append(m.group(1).replace("\\\\", "\\"))
    except OSError:
        pass
    libs += [r"C:\Program Files (x86)\Steam", r"D:\SteamLibrary", r"E:\SteamLibrary"]
    for lib in libs:
        exe = os.path.join(lib, "steamapps", "common", "Crimson Desert", "bin64", "CrimsonDesert.exe")
        if os.path.exists(exe):
            return exe
    return None


EXE = sys.argv[1] if len(sys.argv) > 1 else default_exe()
if not EXE or not os.path.exists(EXE):
    sys.exit("CrimsonDesert.exe not found; pass its path: py -3 sigcheck.py <path\\to\\CrimsonDesert.exe>")


def pe_sections(buf):
    e_lfanew = struct.unpack_from("<I", buf, 0x3C)[0]
    assert buf[e_lfanew:e_lfanew + 4] == b"PE\0\0"
    fh = e_lfanew + 4
    nsec = struct.unpack_from("<H", buf, fh + 2)[0]
    optsz = struct.unpack_from("<H", buf, fh + 16)[0]
    opt = fh + 20
    imagebase = struct.unpack_from("<Q", buf, opt + 24)[0]
    tstamp = struct.unpack_from("<I", buf, fh + 4)[0]
    sect = opt + optsz
    secs = []
    for i in range(nsec):
        o = sect + i * 40
        name = buf[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", buf, o + 8)
        chars = struct.unpack_from("<I", buf, o + 36)[0]
        secs.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize, raw=rawptr, chars=chars))
    return imagebase, tstamp, secs


def off2rva(secs, off):
    for s in secs:
        if s["raw"] <= off < s["raw"] + s["rawsize"]:
            return s["rva"] + (off - s["raw"])
    return None


def rva2off(secs, rva):
    for s in secs:
        if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rawsize"]):
            return s["raw"] + (rva - s["rva"])
    return None


def pat2re(pat):
    out = b""
    for tok in pat.split():
        out += b"." if tok in ("??", "?") else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)


def scan(buf, secs, pat, limit=16):
    rx = pat2re(pat)
    hits = []
    for m in rx.finditer(buf):
        rva = off2rva(secs, m.start())
        if rva is not None:
            hits.append(rva)
            if len(hits) >= limit:
                break
    return hits


def patterns_from_header(path):
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"//[^\n]*", "", text)
    out = []
    for m in re.finditer(r"(kSig_\w+)\s*=\s*((?:\s*\"[^\"]*\")+)\s*;", text):
        lit = "".join(re.findall(r"\"([^\"]*)\"", m.group(2)))
        out.append((m.group(1), lit.strip()))
    strs = []
    for m in re.finditer(r"(k(?:Str|Rtti|Cls|Desc)_\w+)\s*=\s*\"([^\"]*)\"", text):
        strs.append((m.group(1), m.group(2)))
    return out, strs


def rip(buf, secs, rva, insn_len):
    off = rva2off(secs, rva)
    disp = struct.unpack_from("<i", buf, off + insn_len - 4)[0]
    return rva + insn_len + disp


def main():
    buf = open(EXE, "rb").read()
    base, tstamp, secs = pe_sections(buf)
    print("exe %s  size %d  timestamp 0x%08X  sections %d" % (EXE, len(buf), tstamp, len(secs)))
    pats, strs = patterns_from_header(HDR)
    bad = 0
    found = {}
    for name, pat in pats:
        hits = scan(buf, secs, pat)
        tag = "ok " if len(hits) == 1 else ("MULTI" if hits else "MISS")
        if len(hits) != 1 and name not in ("kSig_LeaR8Rip", "kSig_TableResolver16", "kSig_TableIndex", "kSig_OwnCallSite"):
            bad += 1
        print("%-5s %-24s hits=%-3d %s" % (tag, name, len(hits), " ".join("+0x%X" % h for h in hits[:6])))
        if len(hits) == 1:
            found[name] = hits[0]
    for name, s in strs:
        n = buf.count(s.encode() + (bytes([0]) if name.startswith("kStr_") else b""))
        print("%-5s %-24s %d occurrence(s) of %r" % ("ok " if n else "MISS", name, n, s))
        if not n:
            bad += 1

    if "kSig_TlsDesc" in found:
        d = found["kSig_TlsDesc"] + 0x60
        off = rva2off(secs, d)
        print("desc_lookup at +0x%X bytes %s" % (d, buf[off:off + 12].hex(" ")))
    if "kSig_AreaSweep" in found:
        st = found["kSig_AreaSweep"] - 0x0F
        off = rva2off(secs, st)
        print("area_sweep fn start +0x%X bytes %s" % (st, buf[off:off + 16].hex(" ")))
    if "kSig_DescMaskQueue" in found:
        h = found["kSig_DescMaskQueue"]
        mask = rip(buf, secs, h + 5, 7)
        queue = rip(buf, secs, h + 22, 7)
        img = max(s["rva"] + s["vsize"] for s in secs)
        ok = 0 < mask < img and 0 < queue < img
        print("%s DESC_MASK +0x%X  queue +0x%X  (image 0x%X)" % ("ok " if ok else "BAD", mask, queue, img))
        if not ok:
            bad += 1

    # Table resolvers: every `lea r8, [rip+str]` whose string is one of the two table
    # names, with the 16-bit resolver prologue at most 0x180 bytes above.
    pro = pat2re(dict(pats)["kSig_TableResolver16"])
    for want in ("iteminfo", "gimmickinfo"):
        soff = buf.find(want.encode() + b"\0")
        srva = off2rva(secs, soff) if soff >= 0 else None
        hits = []
        if srva is not None:
            for m in re.finditer(rb"\x4c\x8d\x05", buf):
                o = m.start()
                r = off2rva(secs, o)
                if r is None:
                    continue
                disp = struct.unpack_from("<i", buf, o + 3)[0]
                if r + 7 + disp != srva:
                    continue
                # prologue above?
                lo = max(0, o - 0x180)
                fn = None
                for pm in pro.finditer(buf, lo, o):
                    fn = pm.start()
                if fn is not None:
                    g = rip(buf, secs, off2rva(secs, fn) + 0x15, 7)
                    hits.append((r, off2rva(secs, fn), g))
        tag = "ok " if len(hits) == 1 else ("MULTI" if hits else "MISS")
        if len(hits) != 1:
            bad += 1
        print("%-5s table %-12s %s" % (tag, want, "  ".join("lea +0x%X fn +0x%X global +0x%X" % h for h in hits)))

    print("RESULT:", "all good" if not bad else "%d problem(s)" % bad)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
