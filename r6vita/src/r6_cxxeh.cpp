// r6_cxxeh.cpp — MSVC C++ exceptions (RaiseException 0xE06D7363) for the
// guest, done on the host side.
//
// R6BOLegacy.exe (MSVC 6) throws through _CxxThrowException -> RaiseException
// and expects the SEH chain at fs:[0] to find its catch. Running the game's own
// __CxxFrameHandler from a shim would never return (it jumps straight to the
// catch continuation), so this file does what that handler does, with the
// game's own tables:
//   1. walk the registration records from fs:[0];
//   2. a record whose handler is the compiler thunk "mov eax, FuncInfo;
//      jmp __CxxFrameHandler" is a C++ frame: find a try block that covers
//      its current state and a catch whose type the thrown object matches;
//   3. unwind the frames above it (their unwind-map destructors, or the SEH
//      handler called with EXCEPTION_UNWINDING), then the target frame down to
//      the try block's low state;
//   4. copy the object into the catch variable, run the catch funclet with
//      the frame's EBP (it returns the continuation address), destroy the
//      thrown object;
//   5. resume at the continuation with EBP = frame, ESP = the frame's saved ESP
//      (the shim's return is redirected there).
// Frame layout (MSVC x86): [rec-4] saved ESP, rec = { next, handler, state },
// frame EBP = rec + 12.
#include "sc_host.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/guest_thread.h"

#include <cstring>
#include <string>
#include <vector>

namespace sc {
using d2rt::Cpu;

namespace {
constexpr uint32_t kEhMagic = 0x19930520u;   // VC6 FuncInfo / throw magic (also 0x19930521/22)

bool magic_ok(uint32_t m) { return m >= 0x19930520u && m <= 0x19930522u; }

// Call guest `fn` with EBP (and ECX) set, as _CallSettingFrame does; returns EAX.
uint32_t call_frame(Cpu& c, uint32_t fn, uint32_t ebp, uint32_t ecx, std::initializer_list<uint32_t> args) {
    d2rt::X86Context saved; c.save_context(saved);
    uint32_t esp = (c.reg(d2rt::R_ESP) - 0x100u) & ~15u;
    std::vector<uint32_t> a(args);
    for (size_t i = a.size(); i-- > 0;) { esp -= 4; c.write_u32(esp, a[i]); }
    esp -= 4; c.write_u32(esp, g_br->sentinel());
    c.set_reg(d2rt::R_ESP, esp); c.set_reg(d2rt::R_EBP, ebp); c.set_reg(d2rt::R_ECX, ecx);
    const char* fault = nullptr;
    const bool ok = run_nested(c, fn, &fault);
    const uint32_t r = c.reg(d2rt::R_EAX);
    c.load_context(saved);
    if (!ok) log("[not-bound] [cause: guest fault] C++ EH funclet 0x%08x: %s", fn, fault ? fault : "?");
    return r;
}

// FuncInfo of a C++ frame, or 0: handler thunk = B8 imm32 (mov eax) E9 rel32 (jmp).
uint32_t func_info_of(Cpu& c, uint32_t handler) {
    uint8_t b[10];
    if (!handler || !c.read(handler, b, sizeof b)) return 0;
    if (b[0] != 0xB8 || b[5] != 0xE9) return 0;
    uint32_t fi; std::memcpy(&fi, b + 1, 4);
    return magic_ok(c.read_u32(fi)) ? fi : 0;
}

std::string type_name(Cpu& c, uint32_t td) {   // TypeDescriptor { vftable, spare, name[] }
    if (!td) return "...";
    std::string s; for (uint32_t i = 0; i < 128; ++i) { uint8_t ch = 0; c.read(td + 8 + i, &ch, 1); if (!ch) break; s += (char)ch; }
    return s;
}

// Run frame `rec`'s unwind map from its current state down to `to`.
void unwind_frame(Cpu& c, uint32_t rec, uint32_t fi, int to) {
    const uint32_t map = c.read_u32(fi + 8);
    const int maxs = (int)c.read_u32(fi + 4);
    int cur = (int)c.read_u32(rec + 8);
    int guard = 0;
    while (cur != to && cur >= 0 && cur < maxs && guard++ < 4096) {
        const int next = (int)c.read_u32(map + 8 * cur);
        const uint32_t action = c.read_u32(map + 8 * cur + 4);
        c.write_u32(rec + 8, (uint32_t)next);
        if (action) call_frame(c, action, rec + 12, 0, {});
        cur = next;
    }
    c.write_u32(rec + 8, (uint32_t)to);
}

struct Match { uint32_t rec = 0, fi = 0, tb = 0, handler = 0, ct = 0; };
}  // namespace

// Returns true when the exception was caught: the guest then resumes at the
// catch continuation (the RaiseException shim's return is redirected).
bool cxx_dispatch(Cpu& c, uint32_t code, uint32_t nparams, uint32_t params, uint32_t shim_esp_add) {
    if (code != 0xE06D7363u || nparams < 3 || !params) return false;
    const uint32_t magic = c.read_u32(params), obj = c.read_u32(params + 4), ti = c.read_u32(params + 8);
    if (!magic_ok(magic) || !ti) { log("[not-bound] [cause: guest exception] C++ rethrow (no object): not handled"); return false; }
    const uint32_t cta = c.read_u32(ti + 12);
    const uint32_t ncts = c.read_u32(cta);
    const uint32_t tib = wx86_cur_tib();

    // 2. find the catch.
    Match m;
    std::vector<uint32_t> above;
    for (uint32_t rec = c.read_u32(tib), n = 0; rec && rec != 0xFFFFFFFFu && n < 256; rec = c.read_u32(rec), ++n) {
        const uint32_t fi = func_info_of(c, c.read_u32(rec + 4));
        if (fi) {
            const int state = (int)c.read_u32(rec + 8);
            const uint32_t ntb = c.read_u32(fi + 12), tbm = c.read_u32(fi + 16);
            for (uint32_t i = 0; i < ntb && !m.rec; ++i) {
                const uint32_t tb = tbm + 20 * i;   // tryLow, tryHigh, catchHigh, nCatches, pHandlerArray
                const int lo = (int)c.read_u32(tb), hi = (int)c.read_u32(tb + 4);
                if (state < lo || state > hi) continue;
                const uint32_t nc = c.read_u32(tb + 12), ha = c.read_u32(tb + 16);
                for (uint32_t h = 0; h < nc && !m.rec; ++h) {
                    const uint32_t ht = ha + 16 * h;          // adjectives, pType, dispCatchObj, addressOfHandler
                    const uint32_t ctype = c.read_u32(ht + 4);
                    const std::string cname = type_name(c, ctype);
                    for (uint32_t k = 0; k < ncts; ++k) {
                        const uint32_t ct = c.read_u32(cta + 4 + 4 * k);   // properties, pType, PMD(3), size, copy
                        const uint32_t ttype = c.read_u32(ct + 4);
                        if (!ctype || cname.empty() || cname == "..." || ctype == ttype || cname == type_name(c, ttype)) {
                            m.rec = rec; m.fi = fi; m.tb = tb; m.handler = ht; m.ct = ct; break;
                        }
                    }
                }
            }
        }
        if (m.rec) break;
        above.push_back(rec);
    }
    static int s_logged = 0;
    if (!m.rec) {
        log("[not-bound] [cause: guest exception] C++ throw of %s: no catch on the frame chain",
            ncts ? type_name(c, c.read_u32(c.read_u32(cta + 4) + 4)).c_str() : "?");
        return false;
    }
    if (s_logged++ < 16)
        log("[not-bound] C++ throw of %s caught by frame 0x%08x (catch at 0x%08x)",
            type_name(c, c.read_u32(m.ct + 4)).c_str(), m.rec, c.read_u32(m.handler + 12));

    // 3. unwind the frames above the target, innermost first.
    for (uint32_t rec : above) {
        const uint32_t h = c.read_u32(rec + 4);
        if (uint32_t fi = func_info_of(c, h)) unwind_frame(c, rec, fi, -1);
        else if (h) {   // plain SEH frame: let its handler run its __finally blocks
            const uint32_t er = (c.reg(d2rt::R_ESP) - 0x400u) & ~15u;   // EXCEPTION_RECORD (0x50)
            uint8_t z[0x50] = {}; c.write(er, z, sizeof z);
            c.write_u32(er, code); c.write_u32(er + 4, 2u /*EXCEPTION_UNWINDING*/);
            call_frame(c, h, c.reg(d2rt::R_EBP), 0, { er, rec, 0, 0 });
        }
        c.write_u32(tib, c.read_u32(rec));   // pop it
    }
    c.write_u32(tib, m.rec);
    const int lo = (int)c.read_u32(m.tb), hi = (int)c.read_u32(m.tb + 4);
    unwind_frame(c, m.rec, m.fi, lo);

    // 4. catch object, catch funclet, thrown object's destructor.
    const uint32_t ebp = m.rec + 12;
    const uint32_t adj = c.read_u32(m.handler), disp = c.read_u32(m.handler + 8), fn = c.read_u32(m.handler + 12);
    const uint32_t props = c.read_u32(m.ct), mdisp = c.read_u32(m.ct + 8), size = c.read_u32(m.ct + 20), copy = c.read_u32(m.ct + 24);
    if (disp && c.read_u32(m.handler + 4)) {
        const uint32_t dst = ebp + disp;
        if (adj & 8) c.write_u32(dst, obj + mdisp);                    // catch by reference
        else if ((props & 1) || !copy) {                                 // simple type / POD
            std::vector<uint8_t> tmp(size ? size : 4);
            c.read(obj, tmp.data(), (uint32_t)tmp.size()); c.write(dst, tmp.data(), (uint32_t)tmp.size());
            if ((props & 1) && size == 4 && mdisp) { uint32_t p = c.read_u32(dst); if (p) c.write_u32(dst, p + mdisp); }
        } else call_frame(c, copy, c.reg(d2rt::R_EBP), dst, { obj + mdisp });   // copy constructor (thiscall)
    }
    c.write_u32(m.rec + 8, (uint32_t)(hi + 1));
    const uint32_t cont = call_frame(c, fn, ebp, 0, {});
    if (uint32_t dtor = c.read_u32(ti + 4)) call_frame(c, dtor, c.reg(d2rt::R_EBP), obj, {});

    // 5. resume at the continuation. The bridge then adds the shim's own
    //    stack cleanup to ESP, so take it off first.
    c.set_reg(d2rt::R_EBP, ebp);
    c.set_reg(d2rt::R_ESP, c.read_u32(m.rec - 4) - shim_esp_add);
    g_br->redirect_next(cont);
    (void)kEhMagic;
    return true;
}

}  // namespace sc
