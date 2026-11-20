import sys, os, struct, capstone
def load(d):
    m = {}
    for f in os.listdir(d):
        if f.endswith('.bin'): m[int(f.split('_')[1][:8], 16)] = open(d + '/' + f, 'rb').read()
    return m
def rd(m, a, n):
    for b, x in m.items():
        if b <= a < b + len(x): return x[a-b:a-b+n]
def u32(m, a): return struct.unpack('<I', rd(m, a, 4))[0]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
def dis(m, a, n=48):
    for i in md.disasm(rd(m, a, n), a): print('  ', hex(i.address), i.mnemonic, i.op_str)
