import sys, struct, zlib, bz2
import os; _D = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, _D)
from explode import explode
src = open(_D + "/mpq_has.py").read().split("f = open(sys.argv[1]")[0]
exec(src)
def open_mpq(path, off):
    d = open(path, 'rb').read()
    _, hsz, asz, ver, sec, htpos, btpos, hcnt, bcnt = struct.unpack_from('<4sIIHHIIII', d, off)
    ht = decrypt(d[off+htpos:off+htpos+hcnt*16], h('(hash table)', 3))
    bt = decrypt(d[off+btpos:off+btpos+bcnt*16], h('(block table)', 3))
    return d, off, 512 << sec, hcnt, ht, bt
def dec_bytes(b, key):
    n = len(b) // 4 * 4
    w = decrypt(b[:n], key)
    return struct.pack('<%dI' % len(w), *w) + b[n:]
def extract(m, name):
    d, off, ss, hcnt, ht, bt = m
    a, b2, i = h(name, 1), h(name, 2), h(name, 0) % hcnt
    while True:
        e = ht[i*4:i*4+4]
        if e[3] == 0xFFFFFFFF: raise KeyError(name)
        if e[0] == a and e[1] == b2: break
        i = (i + 1) % hcnt
    pos, csz, fsz, fl = bt[e[3]*4:e[3]*4+4]
    base = name.replace('/', '\\').split('\\')[-1]
    key = h(base, 3)
    if fl & 0x20000: key = ((key + pos) ^ fsz) & 0xFFFFFFFF
    p = off + pos
    if fl & 0x1000000:  # single unit
        raise NotImplementedError
    ns = (fsz + ss - 1) // ss
    tb = d[p:p + (ns + 1) * 4]
    if fl & 0x10000: tb = dec_bytes(tb, (key - 1) & 0xFFFFFFFF)
    offs = struct.unpack('<%dI' % (ns + 1), tb)
    out = bytearray()
    for k in range(ns):
        blk = d[p + offs[k]:p + offs[k+1]]
        if fl & 0x10000: blk = dec_bytes(blk, (key + k) & 0xFFFFFFFF)
        want = min(ss, fsz - k * ss)
        if len(blk) < want:
            if fl & 0x200:
                t = blk[0]; blk = blk[1:]
                if t & 0x10: blk = bz2.decompress(blk)
                elif t & 0x02: blk = zlib.decompress(blk)
                elif t & 0x08: blk = explode(blk)
                else: raise ValueError('compression %x' % t)
            elif fl & 0x100: blk = explode(blk)
        out += blk
    return bytes(out)
if __name__ == '__main__':
    m = open_mpq(sys.argv[1], int(sys.argv[2], 16))
    data = extract(m, sys.argv[3]); open(sys.argv[4], 'wb').write(data); print(len(data))
