// w3_trace.cpp — what WarCraft III loads and allocates, for the Vita logs.
//
// Hooks three Storm.dll functions at their entry (no game code changed):
//   SFileOpenFileEx (#268)  every file opened inside the .mpq archives,
//   SFileLoadFile   (#279)  every file the game loads whole,
//   SMemAlloc       (#401)  every Storm heap allocation, by source file+line.
// Each hook logs what it sees, then replays the entry's `push ebp; mov
// ebp,esp` and continues into Storm, so behaviour is unchanged.
//
// Build 2 on the Vita ran out of guest memory 26 s into startup (155 MiB in
// use, where the host's main menu needs 55), with no clue to what was
// loaded. These lines say which files, how often, and who allocated what.
// W3_TRACE=0 turns them off; W3_TRACE_ALL=1 logs every call.
#include "sc_host.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/host_clock.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/pe_image.h"

using d2rt::Cpu;

namespace sc {
namespace {

struct Site { uint32_t file = 0, line = 0; uint64_t n = 0, bytes = 0; };
std::unordered_map<uint64_t, Site> g_sites;
std::unordered_map<std::string, uint32_t> g_names;   // file name -> times loaded/opened
uint64_t g_alloc_n = 0, g_alloc_bytes = 0, g_open_n = 0, g_load_n = 0;
uint64_t g_last_hb_ms = 0, g_next_step = 16ull << 20;
bool     g_all = false;

std::string gs(Cpu& c, uint32_t va, size_t max = 200) {
    std::string s;
    if (!va || va < 0x10000) return s;
    for (size_t i = 0; i < max; ++i) {
        const char* p = (const char*)c.hostptr(va + (uint32_t)i, 1);
        if (!p || !*p) break;
        s += (*p >= 0x20 && *p < 0x7f) ? *p : '?';
    }
    return s;
}

uint32_t cur_tid() {
    auto* s = wx86_sched(); auto* t = s ? s->current() : nullptr;
    return t ? t->id : 0;
}

uint32_t va_used() { uint32_t hc, hp, vc, vp; guest_mem(&hc, &hp, &vc, &vp); return vc; }

std::string top_sites(Cpu& c, size_t n) {
    std::vector<const Site*> v;
    for (auto& kv : g_sites) v.push_back(&kv.second);
    std::partial_sort(v.begin(), v.begin() + std::min(n, v.size()), v.end(),
                      [](const Site* a, const Site* b) { return a->bytes > b->bytes; });
    std::string out;
    for (size_t i = 0; i < std::min(n, v.size()); ++i) {
        std::string f = gs(c, v[i]->file, 48);
        size_t k = f.find_last_of("\\/"); if (k != std::string::npos) f = f.substr(k + 1);
        char b[128];
        std::snprintf(b, sizeof b, " %s:%u=%lluK/%llu", f.empty() ? "?" : f.c_str(), v[i]->line,
                      (unsigned long long)(v[i]->bytes >> 10), (unsigned long long)v[i]->n);
        out += b;
    }
    return out;
}

// One status line every 2 s, and at each 16 MiB of guest memory: which
// guest thread is running, memory in use, and the biggest allocators so far.
void heartbeat(Cpu& c, bool force = false) {
    static uint64_t last_n = 0; static uint32_t last_used = 0;
    const uint64_t now = wx86_now_ms();
    const uint32_t used = va_used();
    const bool step = used >= g_next_step;
    const bool busy = g_alloc_n - last_n >= 200 || used > last_used + (512u << 10) || used + (512u << 10) < last_used;
    if (!force && !step && !(now - g_last_hb_ms >= 2000 && busy) && now - g_last_hb_ms < 20000) return;
    g_last_hb_ms = now; last_n = g_alloc_n; last_used = used;
    while (used >= g_next_step) g_next_step += 16ull << 20;
    log("[not-bound] trace: thread %u, VA used %u KiB, Storm allocs %llu (%llu KiB total), mpq opens %llu, whole-file loads %llu; top:%s",
        cur_tid(), used >> 10, (unsigned long long)g_alloc_n, (unsigned long long)(g_alloc_bytes >> 10),
        (unsigned long long)g_open_n, (unsigned long long)g_load_n, top_sites(c, 8).c_str());
}

// Log the first 300 names, then names seen again (2nd, 10th, 100th time)
// and every 100th call.
void note_name(Cpu& c, const char* what, const std::string& name, uint64_t n, uint32_t extra) {
    uint32_t& k = g_names[name];
    ++k;
    if (g_all || n <= 300 || k == 2 || k == 10 || k == 100 || k == 1000 || n % 100 == 0)
        log("[not-bound] %s #%llu thread %u: %s%s (x%u) VA %u KiB ra=0x%08x", what, (unsigned long long)n, cur_tid(),
            name.c_str(), extra ? " [async]" : "", k, va_used() >> 10, c.read_u32(c.reg(d2rt::R_ESP)));
}

// Replays `push ebp; mov ebp,esp` (the first 3 bytes of all three entries)
// and continues at entry+3.
uint32_t resume(Cpu& c, uint32_t entry) {
    const uint32_t E = c.reg(d2rt::R_ESP);
    c.write_u32(E - 4, c.reg(d2rt::R_EBP)); c.set_reg(d2rt::R_EBP, E - 4);
    c.set_reg(d2rt::R_ESP, E - 8);           // the trap's return pops 4: ESP ends at E - 4
    sc::g_br->redirect_next(entry + 3);
    return c.reg(d2rt::R_EAX);
}

bool hook(Cpu& cpu, d2rt::Bridge& br, d2rt::PeImage* storm, uint32_t ord, const char* name,
          std::function<void(Cpu&)> body) {
    const uint32_t at = storm->export_va_ordinal(ord);
    uint8_t pro[3] = {};
    if (!at) { log("[not-bound] trace: Storm #%u not found", ord); return false; }
    cpu.read(at, pro, 3);
    if (pro[0] != 0x55 || pro[1] != 0x8b || pro[2] != 0xec) {
        log("[not-bound] trace: Storm #%u at 0x%08x does not start with push ebp; mov ebp,esp — not hooked", ord, at);
        return false;
    }
    d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = std::string("trace!") + name;
    s.fn = [at, body](Cpu& c) -> uint32_t { body(c); return resume(c, at); };
    br.register_shim("w3trace", name, s);
    cpu.set_alternate(at, br.shim_trap("w3trace", name));
    return true;
}

}  // namespace

void trace_report(Cpu& c) { heartbeat(c, true); }

void install_w3_trace(Cpu& cpu, d2rt::Bridge& br) {
    if (const char* e = std::getenv("W3_TRACE")) if (std::atoi(e) == 0) return;
    g_all = std::getenv("W3_TRACE_ALL") != nullptr;
    d2rt::PeImage* storm = br.module("Storm.dll");
    if (!storm) { log("[not-bound] trace: Storm.dll not loaded"); return; }
    int ok = 0;
    ok += hook(cpu, br, storm, 268, "SFileOpenFileEx", [](Cpu& c) {
        note_name(c, "mpq open", gs(c, c.arg(1)), ++g_open_n, 0);
        heartbeat(c);
    });
    ok += hook(cpu, br, storm, 279, "SFileLoadFile", [](Cpu& c) {
        note_name(c, "load", gs(c, c.arg(0)), ++g_load_n, c.arg(4));
        heartbeat(c);
    });
    ok += hook(cpu, br, storm, 401, "SMemAlloc", [](Cpu& c) {
        const uint32_t size = c.arg(0), file = c.arg(1), line = c.arg(2);
        Site& s = g_sites[(uint64_t)file << 32 | line];
        s.file = file; s.line = line; ++s.n; s.bytes += size;
        ++g_alloc_n; g_alloc_bytes += size;
        if (size >= (1u << 20) || g_all)
            log("[not-bound] Storm alloc %u KiB for %s:%u thread %u (VA %u KiB)%s", size >> 10,
                gs(c, file, 64).c_str(), line, cur_tid(), va_used() >> 10, guest_stack(c).c_str());
        heartbeat(c);
    });
    log("[not-bound] trace: %d of 3 Storm hooks on (file opens, whole-file loads, allocations)", ok);
}

}  // namespace sc
