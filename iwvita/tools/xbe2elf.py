# Convert a retail XBE into an ELF32 i386 image (one PT_LOAD per section) for Ghidra.
import sys,struct
d=open(sys.argv[1],'rb').read()
base,hdrsz=struct.unpack_from('<II',d,0x104)
nsec,secp=struct.unpack_from('<II',d,0x11c)
entry,=struct.unpack_from('<I',d,0x128); entry^=0xA8FC57AB
thunk,=struct.unpack_from('<I',d,0x158); thunk^=0x5B6D40B6
segs=[(base,0,hdrsz,hdrsz,4,'HEADERS')]
for i in range(nsec):
    fl,v,vs,raw,rs,np=struct.unpack_from('<IIIIII',d,secp-base+i*56)
    name=d[np-base:np-base+16].split(b'\0')[0].decode()
    perm=4|(2 if fl&1 else 0)|(1 if fl&4 else 0)
    segs.append((v,raw,rs,vs,perm,name))
print('entry',hex(entry),'thunk',hex(thunk))
n=len(segs); phoff=52; dataoff=phoff+32*n
out=bytearray(dataoff); ph=b''; body=bytearray()
for v,raw,rs,vs,perm,name in segs:
    off=dataoff+len(body); body+=d[raw:raw+rs]
    while len(body)%16: body+=b'\0'
    ph+=struct.pack('<IIIIIIII',1,off,v,v,rs,max(vs,rs),perm,0x10)
eh=b'\x7fELF\x01\x01\x01'+b'\0'*9+struct.pack('<HHIIIIIHHHHHH',2,3,1,entry,phoff,0,0,52,32,n,0,0,0)
open(sys.argv[2],'wb').write(eh+ph+body)
open(sys.argv[2]+'.info','w').write(f'entry {entry:#x}\nthunk {thunk:#x}\n'+''.join(f'{s[5]} {s[0]:#x} {s[3]:#x}\n' for s in segs))
