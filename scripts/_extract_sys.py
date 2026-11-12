import struct, hashlib

ISO = 'out/novus-live.iso'

def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]

LBA = 55104
with open(ISO,'rb') as f:
    f.seek(LBA*512); esp=f.read(63488*512)
bps=u16(esp,11); spc=esp[13]; rs=u16(esp,14); nf=esp[16]
ts=u16(esp,19) or u32(esp,32); re=u16(esp,17)
fz=u16(esp,22) or u32(esp,36)
rds=(re*32+bps-1)//bps
fdrs=(rs+nf*fz)
root=fdrs*bps
rdir=esp[root:root+re*32]
tgt=b'SYSTEM  IMG'
sys_off=None
for i in range(0,re*32,32):
    e=rdir[i:i+32]
    if e[0]==0: break
    if e[11]&8: continue
    if e[:11]==tgt: sys_off=i; break
assert sys_off is not None, "SYSTEM.IMG not found"
clus=u16(esp, root+sys_off+26) or (u32(esp, root+sys_off+20)<<16|u16(esp, root+sys_off+26))
size=u32(esp, root+sys_off+28)
fat1=rs*bps
def nx(c): return u16(esp, fat1+c*2)
out=bytearray(); c=clus; seen=set()
while c<0xFFF8 and c>1 and c not in seen:
    seen.add(c); off=(fdrs+(c-2)*spc)*bps; out+=esp[off:off+spc*bps]; c=nx(c)
sysimg=bytes(out[:size])
open('out/sys_extracted.img','wb').write(sysimg)
print('extracted SYSTEM.IMG size=%d md5=%s'%(len(sysimg), hashlib.md5(sysimg).hexdigest()))
ITBL=130; INO=128; BS=4096
blk=sysimg[ITBL*BS:]
off=(2-1)*INO
mode,pad,ino,sz,nl,bl=struct.unpack_from('<HHIIII',blk,off)
print('font.bin(inode2): size=%d blocks=%d'%(sz,bl))
