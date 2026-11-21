# w3replay.py — reference replay of a w3vita snapshot (SC_TRAP_SNAP) under unicorn.
# Runs guest x86 from the snapshot (regs.txt + mem_*.bin) until it reaches
# the native-thunk window (a shim call), faults, or hits the step limit, and
# prints the last basic blocks executed. Used to tell a dynarec bug (unicorn
# goes on cleanly where box86 faulted) from a guest-state problem.
import sys, os, collections
from unicorn import *
from unicorn.x86_const import *
snap = sys.argv[1]; limit = int(sys.argv[2]) if len(sys.argv) > 2 else 50_000_000
TRAP_LO, TRAP_HI = 0x0E900000, 0x0EA00000   # boot.cpp kTrapBase .. kArenaEnd
STOP = int(os.environ["STOP"], 16) if "STOP" in os.environ else None   # stop when EIP reaches this (e.g. the return address)
r = [int(l, 16) for l in open(snap + '/regs.txt')]
mu = Uc(UC_ARCH_X86, UC_MODE_32)
mu.mem_map(0, 0x0F000000)
for f in sorted(os.listdir(snap)):
    if f.startswith('mem_'):
        mu.mem_write(int(f[4:12], 16), open(snap + '/' + f, 'rb').read())
for reg, v in zip([UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
                   UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI], r[:8]):
    mu.reg_write(reg, v)
last = collections.deque(maxlen=int(os.environ.get('LAST', '40')))
n = [0]
def blk(uc, addr, size, ud):
    last.append((addr, uc.reg_read(UC_X86_REG_ESP)))
    n[0] += 1
    if STOP is not None and addr == STOP:
        print(f"reached STOP 0x{addr:08x} after {n[0]} blocks"); uc.emu_stop(); return
    if TRAP_LO <= addr < TRAP_HI:
        esp = uc.reg_read(UC_X86_REG_ESP)
        ra = int.from_bytes(uc.mem_read(esp, 4), 'little')
        print(f"reached trap 0x{addr:08x} (shim call) ra=0x{ra:08x} after {n[0]} blocks"); uc.emu_stop()
mu.hook_add(UC_HOOK_BLOCK, blk)
try:
    mu.emu_start(r[8], 0xFFFFFFFF, count=limit)
except UcError as e:
    print("unicorn error:", e, "eip=0x%08x" % mu.reg_read(UC_X86_REG_EIP))
for a, sp in last: print(f"  block 0x{a:08x} esp=0x{sp:08x}")
print("EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x" % tuple(mu.reg_read(x) for x in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI)))
