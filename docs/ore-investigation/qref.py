import struct,sys,re
EXE=r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IB=0x140000000
def sections(data):
    pe=struct.unpack_from("<I",data,0x3C)[0]
    n=struct.unpack_from("<H",data,pe+6)[0]; o=struct.unpack_from("<H",data,pe+20)[0]
    t=pe+24+o; out=[]
    for i in range(n):
        e=t+i*40
        nm=data[e:e+8].rstrip(b"\0").decode("ascii","replace")
        vs,va,rs,ra=struct.unpack_from("<IIII",data,e+8); out.append((nm,va,vs,ra,rs))
    return out
def o2r(secs,off):
    for nm,va,vs,ra,rs in secs:
        if ra<=off<ra+rs: return va+(off-ra),nm
    return None,None
data=open(EXE,'rb').read(); secs=sections(data)
rva=int(sys.argv[1],16)
pat=struct.pack('<Q',IB+rva)
hits=[]
s=0
while True:
    i=data.find(pat,s)
    if i<0: break
    s=i+1
    r,nm=o2r(secs,i)
    if r is not None: hits.append((r,nm))
print("%d qword pointer(s) to RVA 0x%X:"%(len(hits),rva))
for r,nm in hits[:60]: print("  %-10s at RVA 0x%08X"%(nm,r))
