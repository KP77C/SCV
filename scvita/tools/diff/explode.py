# PKWARE DCL "implode" decompressor (port of zlib contrib/blast/blast.c, Mark Adler).
class _S:
    def __init__(s, d): s.d=d; s.p=0; s.bitbuf=0; s.bitcnt=0; s.out=bytearray()
def _bits(s, need):
    val=s.bitbuf
    while s.bitcnt<need:
        if s.p>=len(s.d): raise EOFError
        val|=s.d[s.p]<<s.bitcnt; s.p+=1; s.bitcnt+=8
    s.bitbuf=val>>need; s.bitcnt-=need
    return val&((1<<need)-1)
def _construct(rep):
    length=[]
    for r in rep:
        n=(r>>4)+1; l=r&15; length+= [l]*n
    n=len(length); count=[0]*14
    for l in length: count[l]+=1
    offs=[0]*14
    for i in range(1,13): offs[i+1]=offs[i]+count[i]
    symbol=[0]*n
    for sym in range(n):
        if length[sym]: symbol[offs[length[sym]]]=sym; offs[length[sym]]+=1
    return (count,symbol)
def _decode(s,h):
    count,symbol=h; code=first=index=0; bitbuf=s.bitbuf; left=s.bitcnt; ln=1; nxt=1
    while True:
        while left:
            code|=(bitbuf&1)^1; bitbuf>>=1; cnt=count[ln]
            if code<first+cnt:
                s.bitbuf=bitbuf; s.bitcnt=(s.bitcnt-ln)&7
                return symbol[index+(code-first)]
            index+=cnt; first+=cnt; first<<=1; code<<=1; ln+=1; left-=1
        left=16-ln  # MAXBITS+1-len
        if left==0: raise ValueError('ran out of codes')
        if s.p>=len(s.d): raise EOFError
        bitbuf=s.d[s.p]; s.p+=1
        if left>8: left=8
_litlen=[11,124,8,7,28,7,188,13,76,4,10,8,12,10,12,10,8,23,8,9,7,6,7,8,7,6,55,8,23,24,12,11,7,9,11,12,6,7,22,5,7,24,6,11,9,6,7,22,7,11,38,7,9,8,25,11,8,11,9,12,8,12,5,38,5,38,5,11,7,5,6,21,6,10,53,8,7,24,10,27,44,253,253,253,252,252,252,13,12,45,12,45,12,61,12,45,44,173]
_lenlen=[2,35,36,53,38,23]
_distlen=[2,20,53,230,247,151,248]
_base=[3,2,4,5,6,7,8,9,10,12,16,24,40,72,136,264]
_extra=[0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8]
_H=None
def explode(data):
    global _H
    if _H is None: _H=(_construct(_litlen),_construct(_lenlen),_construct(_distlen))
    litcode,lencode,distcode=_H
    s=_S(data); lit=_bits(s,8); dictb=_bits(s,8)
    if lit>1 or dictb<4 or dictb>6: raise ValueError('bad header')
    out=s.out
    while True:
        if _bits(s,1):
            sym=_decode(s,lencode); ln=_base[sym]+_bits(s,_extra[sym])
            if ln==519: break
            sym=2 if ln==2 else dictb
            dist=(_decode(s,distcode)<<sym)+_bits(s,sym)+1
            for _ in range(ln): out.append(out[-dist])
        else:
            out.append(_decode(s,litcode) if lit else _bits(s,8))
    return bytes(out)
