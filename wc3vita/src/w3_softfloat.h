// w3_softfloat.h — WarCraft III 1.27a's deterministic software float, in C.
//
// Game.dll does its game-state math with its own integer-only float routines
// (so every player's machine computes the same bits, which lockstep
// multiplayer and replays need). They are small, called tens of thousands of
// times a frame, and through the x86 translator each call costs 200-300 ARM
// instructions. These are instruction-by-instruction transcriptions of five
// of them, written as pure functions so the very same code is compiled into
// the port and into a 32-bit x86 test that runs the game's original bytes
// side by side on millions of inputs (scratchpad oracle, see w3_softfloat.cpp).
//
// Each one returns what the guest code leaves behind: the stored result and
// the final ECX and EDX (scratch registers under __fastcall, kept exact
// anyway), plus the store the routine makes to its own argument slot.
// Shifts follow x86: the count is taken modulo 32.
#pragma once
#include <stdint.h>

typedef struct { uint32_t res, ecx, edx, slot; int slot_written; } w3sf_out;

static inline uint32_t w3sf_sar(uint32_t v, uint32_t n) { return (uint32_t)((int32_t)v >> (n & 31u)); }
static inline uint32_t w3sf_shr(uint32_t v, uint32_t n) { return v >> (n & 31u); }
static inline uint32_t w3sf_shl(uint32_t v, uint32_t n) { return v << (n & 31u); }
// 6f052b50 on a nonzero value: eax = 31 - bsr(x), edx = -bsr(x).
static inline uint32_t w3sf_bsr(uint32_t x) { return 31u - (uint32_t)__builtin_clz(x); }

// 6f0527e0  out = a * b   (ecx = out, edx = &a, [esp+4] = &b, ret 4)
static inline void w3sf_mul(uint32_t A, uint32_t B, w3sf_out* o) {
    const uint32_t sign = (B ^ A) & 0x80000000u;
    const uint32_t eA = A & 0x7f800000u, eB = B & 0x7f800000u;
    const uint32_t mB = B & 0x7fffffu, mA = A & 0x7fffffu;
    o->slot = sign; o->slot_written = 1;
    if (mA != 0 && mB != 0) {
        const uint32_t b8 = (mB | 0xff800000u) << 8, a8 = (mA | 0xff800000u) << 8;
        uint32_t hi = (uint32_t)(((uint64_t)b8 * a8) >> 32);
        const uint32_t esi = eA - 0x3f800000u + eB;
        uint32_t ecx = hi >> 31;
        hi = w3sf_shr(hi, ecx);
        hi >>= 7;
        uint32_t eax = esi - 0x800000u;
        ecx <<= 23;
        hi &= 0x7fffffu;
        ecx += esi;
        eax = ~w3sf_sar(eax, 31);
        hi = (hi | ecx | sign) & eax;
        o->res = hi; o->ecx = ecx; o->edx = hi;
        return;
    }
    if (eA == 0 || eB == 0) { o->res = 0; o->ecx = eB; o->edx = mB; return; }
    uint32_t ecx = eA + (eB + 0xc0800000u);
    const uint32_t eax = ~w3sf_sar(ecx + 0xff800000u, 31);
    ecx = (ecx | mB | mA | sign) & eax;
    o->res = ecx; o->ecx = ecx; o->edx = mB;
}

// The shared tail of add and sub: normalise a nonzero signed sum.
// ebx = the larger exponent field. Returns the result; sets final ecx/edx.
static inline uint32_t w3sf_norm(uint32_t sum, uint32_t ebx, w3sf_out* o) {
    const uint32_t sign = sum & 0x80000000u;
    const uint32_t s = w3sf_sar(sum, 31);
    uint32_t mag = (sum ^ s) - s;
    o->slot = sign; o->slot_written = 1;
    const uint32_t clz = 31u - w3sf_bsr(mag);
    const uint32_t edx = 8u - clz;
    uint32_t ecx = edx;
    if ((int32_t)edx < 0) { ecx = 0u - ecx; mag = w3sf_shl(mag, ecx); }
    else mag = w3sf_sar(mag, ecx);
    o->ecx = ecx; o->edx = edx;
    return (((edx - 1u) << 23) + ebx) | (mag & 0x7fffffu) | sign;
}

// 6f0529d0  out = a + b   (ecx = out, edx = &a, [esp+4] = &b, ret 4)
// ecx0/edx0: the registers at entry (the early exit keeps them).
static inline void w3sf_add(uint32_t A, uint32_t B, uint32_t ecx0, uint32_t edx0, w3sf_out* o) {
    o->slot_written = 0;
    const uint32_t eA = A & 0x7f800000u;
    if (eA == 0) { o->res = B; o->ecx = ecx0; o->edx = edx0; return; }
    const uint32_t eB = B & 0x7f800000u;
    if (eB == 0) { o->res = A; o->ecx = 0; o->edx = B; return; }
    const uint32_t diff = eB - eA;
    uint32_t s = w3sf_sar(A, 31);
    uint32_t ma = ((((A & 0x7fffffu) | 0x800000u) * 2u) ^ s) - s;
    s = w3sf_sar(B, 31);
    uint32_t mb = ((((B & 0x7fffffu) | 0x800000u) * 2u) ^ s) - s;
    uint32_t ebx = eA, cnt;
    if ((int32_t)diff > 0) {
        if ((int32_t)diff >= 0xb800000) { o->res = B; o->ecx = diff; o->edx = mb; return; }
        ebx = eB; cnt = diff >> 23; ma = w3sf_sar(ma, cnt);
    } else {
        if ((int32_t)diff <= (int32_t)0xf4800000u) { o->res = A; o->ecx = diff; o->edx = mb; return; }
        cnt = (eA - eB) >> 23; mb = w3sf_sar(mb, cnt);
    }
    const uint32_t sum = mb + ma;
    if (sum == 0) { o->res = 0; o->ecx = cnt; o->edx = mb; return; }
    o->res = w3sf_norm(sum, ebx, o);
}

// 6f0528b0  out = a - b   (ecx = out, edx = &a, [esp+4] = &b, ret 4)
static inline void w3sf_sub(uint32_t A, uint32_t B, w3sf_out* o) {
    const uint32_t NB = B ^ 0x80000000u;
    const uint32_t eA = A & 0x7f800000u;
    o->slot = eA; o->slot_written = 1;
    if (eA == 0) { o->res = NB; o->ecx = eA; o->edx = A; return; }
    const uint32_t eN = NB & 0x7f800000u;
    if (eN == 0) { o->res = A; o->ecx = eA; o->edx = A; return; }
    uint32_t s = w3sf_sar(A, 31);
    uint32_t ma = ((((A & 0x7fffffu) | 0x800000u) * 2u) ^ s) - s;
    s = w3sf_sar(NB, 31);
    uint32_t mn = ((((NB & 0x7fffffu) | 0x800000u) * 2u) ^ s) - s;
    const uint32_t diff = eN - eA;
    uint32_t ebx = eN, cnt;
    if ((int32_t)diff > 0) {
        if ((int32_t)diff >= 0xb800000) { o->res = NB; o->ecx = diff; o->edx = ma; return; }
        cnt = diff >> 23; ma = w3sf_sar(ma, cnt);
    } else {
        if ((int32_t)diff <= (int32_t)0xf4800000u) { o->res = A; o->ecx = diff; o->edx = A; return; }
        ebx = eA; cnt = (eA - eN) >> 23; mn = w3sf_sar(mn, cnt);
    }
    const uint32_t sum = mn + ma;
    if (sum == 0) { o->res = 0; o->ecx = cnt; o->edx = ma; return; }
    o->res = w3sf_norm(sum, ebx, o);
}

// 6f052f30  eax = (int)a, truncated   (ecx = &a, bare ret). res = eax.
static inline void w3sf_ftoi(uint32_t A, uint32_t ecx0, uint32_t edx0, w3sf_out* o) {
    o->slot_written = 0;
    const uint32_t e = (A >> 23) & 0xffu;
    if (e < 0x7fu) { o->res = 0; o->ecx = ecx0; o->edx = edx0; return; }
    uint32_t m = (A & 0x7fffffu) | 0x800000u;
    uint32_t ecx = 0x96u - e;
    const uint32_t s = w3sf_sar(A, 31);
    if ((int32_t)ecx < 0) { ecx = 0u - ecx; m = w3sf_shl(m, ecx); }
    else m = w3sf_shr(m, ecx);
    o->res = (s ^ m) - s; o->ecx = ecx; o->edx = m;
}

// 6f053a90  out = floor(a)   (ecx = out, edx = &a, bare ret)
// k_neg/k_pos: the values of the two .data constants (6fbb81d0 / 6fbb81d4)
// the guest reads for |a| < 1; kaddr_pos: the runtime address of the second
// one, which the guest leaves in ECX on that path.
static inline void w3sf_floor(uint32_t A, uint32_t ecx0, uint32_t k_neg, uint32_t k_pos, uint32_t kaddr_pos, w3sf_out* o) {
    o->slot_written = 0;
    const uint32_t e = ((A >> 23) & 0xffu) - 0x7fu;
    if ((int32_t)e < 0) {
        const uint32_t s = w3sf_sar(A, 31);
        o->res = ((A * 2u) & s) == 0 ? k_pos : k_neg;
        o->ecx = kaddr_pos; o->edx = A;
        return;
    }
    if ((int32_t)e >= 0x17) { o->res = A; o->ecx = ecx0; o->edx = A; return; }
    const uint32_t ebx = w3sf_sar(0x80000000u, e + 8u);
    const uint32_t ecx = ~w3sf_sar(A, 31);
    if (((A * 2u) & ecx) != 0) { const uint32_t r = A & ebx; o->res = r; o->ecx = ecx; o->edx = r; return; }
    uint32_t edx = ((A & 0x7fffffu) + ~ebx) & ebx;
    const uint32_t eax = edx;
    edx &= 0x7fffffu;
    o->res = ((((eax >> 23) + 0x7fu + e) << 23) | edx) | 0x80000000u;
    o->ecx = ecx; o->edx = edx;
}
