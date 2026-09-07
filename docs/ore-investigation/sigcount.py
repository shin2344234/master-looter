import struct,sys,re
EXE=r'D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe'
d=open(EXE,'rb').read()
pe=struct.unpack_from('<I',d,0x3C)[0]
n=struct.unpack_from('<H',d,pe+6)[0]; o=struct.unpack_from('<H',d,pe+20)[0]
t=pe+24+o; secs=[]
for i in range(n):
    e=t+i*40
    nm=d[e:e+8].rstrip(b'\0').decode()
    vs,va,rs,ra=struct.unpack_from('<IIII',d,e+8); secs.append((nm,va,vs,ra,rs))
def o2r(off):
    for nm,va,vs,ra,rs in secs:
        if ra<=off<ra+rs: return va+(off-ra),nm
    return None,None
pat=sys.argv[1]
rx=b''
for tok in pat.split():
    rx += b'.' if tok=='??' else re.escape(bytes([int(tok,16)]))
hits=[o2r(m.start()) for m in re.finditer(rx,d,re.S)]
print('%d hit(s)'%len(hits))
for r,s in hits[:20]: print('  %-10s RVA 0x%08X'%(s,r))
