import sys,struct,datetime
d=open(sys.argv[1],'rb').read()
base,=struct.unpack_from('<I',d,0x104)
def va(a): return a-base
hdrsz,imgsz,_,ts,cert,nsec,secp=struct.unpack_from('<IIIIIII',d,0x108)
print('base',hex(base),'imgsize',hex(imgsz),'time',datetime.datetime.fromtimestamp(ts,datetime.UTC))
c=va(cert); tid,=struct.unpack_from('<I',d,c+8); name=d[c+12:c+12+80].decode('utf-16le').strip('\0')
print('title',hex(tid),name)
pdb,=struct.unpack_from('<I',d,0x15c); 
for off in (0x14c,0x150):
  p,=struct.unpack_from('<I',d,off)
  if base<p<base+hdrsz: print(hex(off), d[va(p):va(p)+120].split(b'\0')[0])
nlib,libp=struct.unpack_from('<II',d,0x160)
for i in range(nlib):
  o=va(libp)+i*16; n=d[o:o+8].rstrip(b'\0').decode(); M,m,b=struct.unpack_from('<HHH',d,o+8); print('lib',n,f'{M}.{m}.{b}')
s=va(secp); tot=0
for i in range(nsec):
  fl,v,vs,raw,rs,np=struct.unpack_from('<IIIIII',d,s+i*56)
  print('sec',d[va(np):va(np)+12].split(b'\0')[0].decode(),hex(v),hex(vs))
