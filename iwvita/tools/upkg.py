# Minimal UE2 package reader (names, imports, exports) for version-95 .d2u files.
import sys,struct
def ci(d,o):
    b=d[o];o+=1;neg=b&0x80;v=b&0x3f
    if b&0x40:
        s=6
        while True:
            b=d[o];o+=1;v|=(b&0x7f)<<s;s+=7
            if not b&0x80:break
    return (-v if neg else v),o
def read(path):
    d=open(path,'rb').read()
    tag,ver,lic,fl,nc,no,ec,eo,ic,io=struct.unpack_from('<IHHIIIIIII',d,0)
    names=[];o=no
    for i in range(nc):
        l,o=ci(d,o); s=d[o:o+l-1].decode('latin1'); o+=l; o+=4; names.append(s)
    imps=[];o=io
    for i in range(ic):
        cp,o=ci(d,o); cn,o=ci(d,o); pk,=struct.unpack_from('<i',d,o);o+=4; on,o=ci(d,o)
        imps.append((names[cp],names[cn],pk,names[on]))
    exps=[];o=eo
    for i in range(ec):
        cl,o=ci(d,o); su,o=ci(d,o); pk,=struct.unpack_from('<i',d,o);o+=4; on,o=ci(d,o); of,=struct.unpack_from('<I',d,o);o+=4
        sz,o=ci(d,o); so=0
        if sz>0: so,o=ci(d,o)
        exps.append((cl,su,pk,names[on],of,sz,so))
    return d,ver,lic,names,imps,exps
if __name__=='__main__':
    d,ver,lic,names,imps,exps=read(sys.argv[1])
    def cname(i):
        if i<0: return imps[-i-1][3]
        if i>0: return exps[i-1][3]
        return 'Class'
    from collections import Counter
    c=Counter(cname(e[0]) for e in exps)
    print(sys.argv[1].split('/')[-1],'ver',ver,lic,'names',len(names),'exports',len(exps),dict(c.most_common(6)))
