import sys,os,struct
iso,out=sys.argv[1],sys.argv[2]
f=open(iso,'rb')
def rd(off,n): f.seek(off); return f.read(n)
base=0
for b in (0,0x18300000//0x800*0x800, 0xfd90000):
    if rd(b+0x10000,20)==b'MICROSOFT*XBOX*MEDIA': base=b;break
h=rd(base+0x10000,0x20); rs,rz=struct.unpack('<II',h[20:28])
def walk(sec,size,path):
    data=rd(base+sec*2048,size)
    seen=set()
    def node(o):
        if o in seen or o*4>=len(data): return
        seen.add(o)
        e=data[o*4:o*4+14]
        l,r,s,z,a,n=struct.unpack('<HHIIBB',e)
        if l==0xffff: return
        name=data[o*4+14:o*4+14+n].decode('latin1')
        p=os.path.join(path,name)
        if a&0x10:
            os.makedirs(p,exist_ok=True)
            if z: walk(s,z,p)
        else:
            with open(p,'wb') as g:
                left=z;pos=base+s*2048
                while left>0:
                    c=rd(pos,min(left,1<<24));g.write(c);pos+=len(c);left-=len(c)
        if l: node(l)
        if r: node(r)
    node(0)
os.makedirs(out,exist_ok=True); walk(rs,rz,out)
