# Differential replay: run a guest function from a winx86 snapshot under unicorn
# (reference x86 semantics) and report what it wrote, to compare with box86.
import sys, struct, os
from unicorn import *
from unicorn.x86_const import *
snap = sys.argv[1]; storm = sys.argv[2]; stop_ret = None
regs = [int(l, 16) for l in open(snap + '/regs.txt')]
eax, ecx, edx, ebx, esp, ebp, esi, edi, eip = regs
mu = Uc(UC_ARCH_X86, UC_MODE_32)
regions = {}
for f in sorted(os.listdir(snap)):
    if f.startswith('mem_'):
        a = int(f[4:12], 16); d = open(snap + '/' + f, 'rb').read()
        n = (len(d) + 0xfff) & ~0xfff
        mu.mem_map(a, n); mu.mem_write(a, d); regions[a] = d
# extra free arena for VirtualAlloc at 0x0a000000
ARENA = 0x0a000000; mu.mem_map(ARENA, 0x400000); arena_next = [ARENA]
# imports of storm: rva of IAT slots -> name
d = open(storm, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe+6)[0]; so = struct.unpack_from('<H', d, pe+20)[0]
secs = []
for i in range(ns):
    o = pe+24+so+i*40; vs, va, rs, ra = struct.unpack_from('<IIII', d, o+8); secs.append((va, max(vs, rs), ra))
def r2f(r):
    for va, sz, ra in secs:
        if va <= r < va+sz: return r-va+ra
imp = struct.unpack_from('<I', d, pe+24+104)[0]; o = r2f(imp); traps = {}
sm = regions[0x30000000]
while True:
    oft, ts, fc, nm, ft = struct.unpack_from('<IIIII', d, o)
    if not nm: break
    dll = d[r2f(nm):d.find(b'\0', r2f(nm))].decode(); t = r2f(oft or ft); k = 0
    while True:
        v = struct.unpack_from('<I', d, t+4*k)[0]
        if not v: break
        name = ('#%d' % (v & 0xffff)) if v & 0x80000000 else d[r2f(v)+2:d.find(b'\0', r2f(v)+2)].decode()
        slot = ft + 4*k; tv = struct.unpack_from('<I', sm, slot)[0]
        traps[tv] = (dll, name); k += 1
    o += 20
for tv in traps:
    try: mu.mem_map(tv & ~0xfff, 0x1000)
    except UcError: pass
for r, v in ((UC_X86_REG_EAX, eax), (UC_X86_REG_ECX, ecx), (UC_X86_REG_EDX, edx), (UC_X86_REG_EBX, ebx),
             (UC_X86_REG_ESP, esp), (UC_X86_REG_EBP, ebp), (UC_X86_REG_ESI, esi), (UC_X86_REG_EDI, edi)):
    mu.reg_write(r, v)
# FS: TIB at 0 (flat segment base 0)
retaddr = struct.unpack('<I', mu.mem_read(esp, 4))[0]
lasterr = [0]
def arg(i): return struct.unpack('<I', mu.mem_read(mu.reg_read(UC_X86_REG_ESP) + 4 + 4*i, 4))[0]
def ret(v, n):
    sp = mu.reg_read(UC_X86_REG_ESP); ra = struct.unpack('<I', mu.mem_read(sp, 4))[0]
    mu.reg_write(UC_X86_REG_EAX, v & 0xffffffff); mu.reg_write(UC_X86_REG_ESP, sp + 4 + 4*n); mu.reg_write(UC_X86_REG_EIP, ra)
calls = []
def hook(uc, addr, size, ud):
    if addr == retaddr:
        uc.emu_stop(); return
    if addr in traps:
        dll, name = traps[addr]; calls.append(name)
        if name in ('EnterCriticalSection', 'LeaveCriticalSection', 'SetLastError'):
            if name == 'SetLastError': lasterr[0] = arg(0)
            ret(0, 1)
        elif name == 'GetLastError': ret(lasterr[0], 0)
        elif name == 'VirtualAlloc':
            sz = (arg(1) + 0xfff) & ~0xfff; a = arg(0)
            if not a: a = arena_next[0]; arena_next[0] += (sz + 0xffff) & ~0xffff
            ret(a, 4)
        elif name == 'VirtualFree': ret(1, 3)
        elif name == 'VirtualProtect': ret(1, 4)
        elif name == 'FlushInstructionCache': ret(1, 3)
        elif name == 'GetCurrentProcess': ret(0xffffffff, 0)
        elif name == 'GetCurrentThreadId': ret(1, 0)
        elif name == 'TlsGetValue': ret(struct.unpack('<I', mu.mem_read(0xe10 + 4*arg(0), 4))[0], 1)
        elif name == 'HeapAlloc':
            sz = (arg(2) + 15) & ~15; a = arena_next[0]; arena_next[0] += sz
            if arg(1) & 8: mu.mem_write(a, b'\0' * sz)
            ret(a, 3)
        else:
            print('UNHANDLED import', dll, name, hex(addr)); uc.emu_stop()
mu.hook_add(UC_HOOK_CODE, hook)
greads = {}
def mr(uc, access, addr, size, value, ud):
    if 0x30059000 <= addr < 0x30061000 and addr not in greads:
        greads[addr] = (size, bytes(uc.mem_read(addr, size)).hex(), hex(uc.reg_read(UC_X86_REG_EIP)))
oreads = {}
def mr2(uc, access, addr, size, value, ud):
    if not (0x30000000 <= addr < 0x30069000 or 0x0a000000 <= addr < 0x0a400000 or 0x601f0000 <= addr < 0x60200000):
        k = addr & ~3
        if k not in oreads: oreads[k] = (bytes(uc.mem_read(k, 4)).hex(), hex(uc.reg_read(UC_X86_REG_EIP)))
if os.environ.get('OTHERREAD'):
    mu.hook_add(UC_HOOK_MEM_READ, mr2)
if os.environ.get('GLOBREAD'):
    mu.hook_add(UC_HOOK_MEM_READ, mr)
import collections
ring = collections.deque(maxlen=int(os.environ.get('RING','400')))
WATCH = int(os.environ.get('WATCH','0'),16)
def tr(uc, addr, size, ud):
    ring.append((addr, uc.reg_read(UC_X86_REG_EAX), uc.reg_read(UC_X86_REG_ECX), uc.reg_read(UC_X86_REG_EDX), uc.reg_read(UC_X86_REG_EBX), uc.reg_read(UC_X86_REG_ESI), uc.reg_read(UC_X86_REG_EDI), uc.reg_read(UC_X86_REG_EBP)))
def mw(uc, access, addr, size, value, ud):
    if WATCH and addr <= WATCH < addr + size and (os.environ.get('WATCHVAL') is None or ((value >> (8*(WATCH-addr))) & 0xff) == int(os.environ['WATCHVAL'],16)):
        print('WRITE to %x size %d value %x' % (addr, size, value))
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for (a, *r) in ring:
            ins = next(md.disasm(bytes(uc.mem_read(a, 16)), a), None)
            print('%08x %-40s eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x' % (a, (ins.mnemonic + ' ' + ins.op_str) if ins else '?', *r))
        uc.emu_stop()
if os.environ.get('TRACEALL'):
    import capstone as _c
    _md=_c.Cs(_c.CS_ARCH_X86,_c.CS_MODE_32); _n=[0]
    def ta(uc, addr, size, ud):
        _n[0]+=1
        if _n[0] < int(os.environ['TRACEALL']) or (addr & 0xff000000) == 0x08000000 and _n[0] > 200:
            if (addr & 0xff000000) == 0x08000000: return
        ins = next(_md.disasm(bytes(uc.mem_read(addr, 16)), addr), None)
        print('T %08x %-36s ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x' % (addr, (ins.mnemonic+' '+ins.op_str) if ins else '?', uc.reg_read(UC_X86_REG_EBX), uc.reg_read(UC_X86_REG_ECX), uc.reg_read(UC_X86_REG_EDX), uc.reg_read(UC_X86_REG_ESI), uc.reg_read(UC_X86_REG_EDI)))
    mu.hook_add(UC_HOOK_CODE, ta)
if WATCH:
    mu.hook_add(UC_HOOK_CODE, tr)
    mu.hook_add(UC_HOOK_MEM_WRITE, mw)
try:
    mu.emu_start(eip, 0xffffffff, count=50_000_000)
except UcError as e:
    print('emu error', e, hex(mu.reg_read(UC_X86_REG_EIP)))
if os.environ.get('GLOBREAD'):
    for a in sorted(greads): print('GREAD %08x %s' % (a, greads[a]))
if os.environ.get('OTHERREAD'):
    for a in sorted(oreads): print('OREAD %08x %s' % (a, oreads[a]))
print('stopped at', hex(mu.reg_read(UC_X86_REG_EIP)), 'eax', hex(mu.reg_read(UC_X86_REG_EAX)))
from collections import Counter
print('imports:', Counter(calls))
out = sys.argv[3]
os.makedirs(out, exist_ok=True)
for a, dd in regions.items():
    nd = bytes(mu.mem_read(a, len(dd)))
    open(out + '/mem_%08x.bin' % a, 'wb').write(nd)
open(out + '/arena_0a000000.bin', 'wb').write(bytes(mu.mem_read(ARENA, 0x400000)))
