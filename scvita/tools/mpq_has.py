# mpq_has.py — report whether an MPQ's hash table lists a file (and its locale/block).
import sys, struct
ct = [0]*0x500
seed = 0x00100001
for i in range(0x100):
    j = i
    for _ in range(5):
        seed = (seed*125+3) % 0x2AAAAB; t1 = (seed & 0xFFFF) << 16
        seed = (seed*125+3) % 0x2AAAAB; t2 = seed & 0xFFFF
        ct[j] = t1 | t2; j += 0x100
def h(s, t):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().encode():
        s1 = (ct[(t<<8)+ch] ^ (s1+s2)) & 0xFFFFFFFF
        s2 = (ch + s1 + s2 + (s2<<5) + 3) & 0xFFFFFFFF
    return s1
def decrypt(data, key):
    out = []; s2 = 0xEEEEEEEE
    for i in range(0, len(data), 4):
        s2 = (s2 + ct[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        v = struct.unpack_from('<I', data, i)[0] ^ ((key + s2) & 0xFFFFFFFF)
        key = (((~key << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        s2 = (v + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
        out.append(v)
    return out
f = open(sys.argv[1], 'rb'); b = f.read(1 << 20)
off = b.find(b'MPQ\x1a')
f.seek(off); hdr = f.read(32)
_, hsz, asz, ver, sec, htpos, btpos, hcnt, bcnt = struct.unpack('<4sIIHHIIII', hdr)
f.seek(off + htpos); ht = decrypt(f.read(hcnt*16), h('(hash table)', 3))
print(f"{sys.argv[1]}: header@{off} hash entries={hcnt} blocks={bcnt}")
for name in sys.argv[2:]:
    a, b2, i0 = h(name, 1), h(name, 2), h(name, 0) % hcnt
    i = i0; found = []
    while True:
        e = ht[i*4:i*4+4]
        if e[3] == 0xFFFFFFFF: break
        if e[0] == a and e[1] == b2: found.append((hex(e[2] & 0xFFFF), e[3]))
        i = (i + 1) % hcnt
        if i == i0: break
    print(f"  {name}: {'found ' + str(found) if found else 'NOT in hash table'}")
