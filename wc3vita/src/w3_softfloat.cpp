// w3_softfloat.cpp — Game.dll's software float routines served natively.
//
// The translator calls a helper here directly when a block starts at one of
// these entry points (dyn86_intrin.h: no trap, no dispatcher), instead of
// running the 200-300 ARM instructions the x86 body translates to. The math
// is in w3_softfloat.h, transcribed instruction by instruction and checked
// against Game.dll's own bytes, run natively in a 32-bit x86 test: 56 million
// inputs (random bits, every exponent pair within 30, cancelling sums, zero
// and all-ones fields), 0 differences in the result, EAX, ECX, EDX or the
// argument-slot store. Any pointer outside the guest arena falls back to the
// guest's own code, which then faults exactly as it would have.
//
// W3_SOFTFLOAT=0 leaves the game's code alone; =2 calls the helpers but
// always falls back (measures the plumbing on its own).
#include "sc_host.h"
#include "w3_softfloat.h"
#include "dynarec86/dyn86_intrin.h"

#include <cstdlib>
#include <cstring>

extern "C" {
extern uintptr_t dyn86_membase;
extern uint32_t dyn86_mi_span;
}

namespace sc {
namespace {

inline bool in_arena(uint32_t va) { return dyn86_mi_span == 0 || (uint64_t)va + 4u <= dyn86_mi_span; }
inline uint32_t rd(uint32_t va) { uint32_t v; std::memcpy(&v, (const void*)(dyn86_membase + va), 4); return v; }
inline void wr(uint32_t va, uint32_t v) { std::memcpy((void*)(dyn86_membase + va), &v, 4); }

enum { F_MUL, F_ADD, F_SUB, F_FTOI, F_FLOOR, F_N };
const char* const kName[F_N] = { "mul", "add", "sub", "ftoi", "floor" };
uint32_t g_served[F_N], g_fallback[F_N];
uint32_t g_k_neg_va = 0, g_k_pos_va = 0;   // floor's two .data constants (runtime VAs)

inline void note(int f) {
    if (++g_served[f] == 2000000u)
        log("[not-bound] softfloat: %s served natively 2000000 times (%u fallbacks)", kName[f], g_fallback[f]);
}
inline int refuse(int f) { ++g_fallback[f]; return 0; }

// __fastcall binary ops: ecx = out, edx = &a, [esp+4] = &b, ret 4.
template <int F>
int bin_op(void* emu, uint32_t esp) {
    if (dyn86_intrin_on != 1) return 0;
    const uint32_t out = dyn86_intrin_getreg(emu, 1), pa = dyn86_intrin_getreg(emu, 2);
    if (!in_arena(esp + 4) || !in_arena(pa)) return refuse(F);
    const uint32_t pb = rd(esp + 4);
    if (!in_arena(pb) || !in_arena(out)) return refuse(F);
    const uint32_t A = rd(pa), B = rd(pb);
    w3sf_out o;
    if (F == F_MUL) w3sf_mul(A, B, &o);
    else if (F == F_ADD) w3sf_add(A, B, out, pa, &o);
    else w3sf_sub(A, B, &o);
    if (o.slot_written) wr(esp + 4, o.slot);   // the guest reuses its argument slot
    wr(out, o.res);
    dyn86_intrin_setreg(emu, 0, out);
    dyn86_intrin_setreg(emu, 1, o.ecx);
    dyn86_intrin_setreg(emu, 2, o.edx);
    note(F);
    return 1;
}

// eax = (int)*ecx
int h_ftoi(void* emu, uint32_t) {
    if (dyn86_intrin_on != 1) return 0;
    const uint32_t pa = dyn86_intrin_getreg(emu, 1), edx0 = dyn86_intrin_getreg(emu, 2);
    if (!in_arena(pa)) return refuse(F_FTOI);
    w3sf_out o;
    w3sf_ftoi(rd(pa), pa, edx0, &o);
    dyn86_intrin_setreg(emu, 0, o.res);
    dyn86_intrin_setreg(emu, 1, o.ecx);
    dyn86_intrin_setreg(emu, 2, o.edx);
    note(F_FTOI);
    return 1;
}

// *ecx = floor(*edx), eax = ecx
int h_floor(void* emu, uint32_t) {
    if (dyn86_intrin_on != 1) return 0;
    const uint32_t out = dyn86_intrin_getreg(emu, 1), pa = dyn86_intrin_getreg(emu, 2);
    if (!in_arena(pa) || !in_arena(out)) return refuse(F_FLOOR);
    w3sf_out o;
    w3sf_floor(rd(pa), out, rd(g_k_neg_va), rd(g_k_pos_va), g_k_pos_va, &o);
    wr(out, o.res);
    dyn86_intrin_setreg(emu, 0, out);
    dyn86_intrin_setreg(emu, 1, o.ecx);
    dyn86_intrin_setreg(emu, 2, o.edx);
    note(F_FLOOR);
    return 1;
}

uint32_t fnv(d2rt::Cpu& c, uint32_t va, uint32_t n, uint32_t skip1 = 0, uint32_t skip2 = 0) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t a = va + i;
        uint8_t b = 0;
        const bool masked = (skip1 && a >= skip1 && a < skip1 + 4) || (skip2 && a >= skip2 && a < skip2 + 4);
        if (!masked) c.read(a, &b, 1);
        h = (h ^ b) * 0x01000193u;
    }
    return h;
}

}  // namespace

// Called when Game.dll has been mapped and relocated, before any of its code runs.
void softfloat_install(d2rt::Cpu& c, uint32_t base) {
    const char* e = std::getenv("W3_SOFTFLOAT");
    const int mode = e && *e ? std::atoi(e) : 1;
    if (mode == 0) { log("[not-bound] softfloat: off (W3_SOFTFLOAT=0), the game's own code runs"); return; }
    if (!dyn86_membase) { log("[not-bound] softfloat: no guest arena, not installed"); return; }
    const uint32_t R = base - 0x6f000000u;   // runtime = preferred + R
    struct T { uint32_t pref, len, hash; dyn86_intrin_fn fn; uint16_t retn; int f; };
    const T t[] = {
        { 0x6f0527e0u, 201, 0x2ae217bfu, &bin_op<F_MUL>, 4, F_MUL },
        { 0x6f0529d0u, 274, 0xcb55c767u, &bin_op<F_ADD>, 4, F_ADD },
        { 0x6f0528b0u, 260, 0xf9680a80u, &bin_op<F_SUB>, 4, F_SUB },
        { 0x6f052f30u,  73, 0x83106e3eu, &h_ftoi,        0, F_FTOI },
        { 0x6f053a90u, 149, 0x17b647eau, &h_floor,       0, F_FLOOR },
    };
    // floor's constants: the relocated immediates of its two `mov reg, imm32`.
    g_k_neg_va = c.read_u32(0x6f053ab1u + R);
    g_k_pos_va = c.read_u32(0x6f053ab6u + R);
    const uint16_t regs = DYN86_IR_AX | DYN86_IR_CX | DYN86_IR_DX;
    int n = 0;
    for (const T& x : t) {
        const uint32_t va = x.pref + R;
        const uint32_t h = x.f == F_FLOOR ? fnv(c, va, x.len, 0x6f053ab1u + R, 0x6f053ab6u + R) : fnv(c, va, x.len);
        if (h != x.hash) { log("[not-bound] [cause: unknown Game.dll] softfloat: %s at 0x%08x does not match 1.27a, left alone", kName[x.f], va); continue; }
        if (x.f == F_FLOOR && (!in_arena(g_k_neg_va) || !in_arena(g_k_pos_va))) continue;
        if (dyn86_intrin_add(va, x.fn, x.retn, DYN86_IR_CX | DYN86_IR_DX, regs, kName[x.f])) ++n;
    }
    if (n) dyn86_intrin_on = mode == 2 ? 2 : 1;
    log("[not-bound] softfloat: %d of 5 Game.dll float routines served natively%s (W3_SOFTFLOAT=0 turns this off)",
        n, mode == 2 ? " [plumbing only: always fall back]" : "");
}

// Miles (mss32.dll 1.27a, pref 0x21100000): the MMX check at 2113cfea runs on
// EVERY Miles API call (the API lock asks it whether to use the MMX mixer):
// pushfd/popfd to test the ID bit, then cpuid. Through the translator that is
// a flags round trip and a cpuid helper per call; K's build 10 battle log
// has the block right after it at 21-27% of the guest core. The answer never
// changes: it is the emulated CPU's own (shim_impl.c my_cpuid, leaf 1),
// so it is served here with the registers the guest code leaves: EAX = 1
// (MMX present), ECX/EDX = cpuid leaf 1, EBX/ESI/EDI/flags as on entry.
// W3_MILES_MMX=0 leaves Miles' code alone.
extern "C" void wx86_cpuid_features(uint32_t* edx, uint32_t* ecx);
namespace {
uint32_t g_mmx_edx = 0, g_mmx_ecx = 0; unsigned long long g_mmx_n = 0;
int h_miles_mmx(void* emu, uint32_t) {
    if (dyn86_intrin_on != 1) return 0;
    dyn86_intrin_setreg(emu, 0, (g_mmx_edx & 0x800000u) ? 1u : 0u);
    dyn86_intrin_setreg(emu, 1, g_mmx_ecx);
    dyn86_intrin_setreg(emu, 2, g_mmx_edx);
    if (++g_mmx_n == 1000000ull) log("[not-bound] miles: MMX check served natively 1000000 times");
    return 1;
}
}  // namespace
void miles_install(d2rt::Cpu& c, uint32_t base) {
    const char* e = std::getenv("W3_MILES_MMX");
    if (e && *e == '0') { log("[not-bound] miles: MMX check left to Miles (W3_MILES_MMX=0)"); return; }
    if (!dyn86_membase) return;
    const uint32_t va = 0x2113cfeau - 0x21100000u + base;
    if (fnv(c, va, 61) != 0x9af4ef3bu) { log("[not-bound] [cause: unknown mss32.dll] miles: MMX check at 0x%08x not recognised, left alone", va); return; }
    wx86_cpuid_features(&g_mmx_edx, &g_mmx_ecx);
    if (dyn86_intrin_add(va, &h_miles_mmx, 0, 0, DYN86_IR_AX | DYN86_IR_CX | DYN86_IR_DX, "miles_mmx")) {
        if (!dyn86_intrin_on) dyn86_intrin_on = 1;
        log("[not-bound] miles: MMX check at 0x%08x served natively (W3_MILES_MMX=0 turns this off)", va);
    }
}

}  // namespace sc
