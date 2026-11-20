#include <string>
#include <atomic>
#include <mutex>
// boot.cpp — StarCraft Vita port boot harness.
//
// Loads the user's own StarCraft.exe (1.16.1, preferred base 0x00400000, no
// relocations) and storm.dll into one guest address space, installs the
// winx86 engine shims then the port's own, runs storm's DllMain, and starts
// the game's entry point on the cooperative scheduler. Every import that
// reaches the catch-all shim is logged once, in call order: that list is the
// work queue for the next shims.
//
// Usage (host, qemu-arm):  scvita_boot <game-dir> [max-unshimmed]
// On the Vita the game directory is ux0:data/StarCraft/ (K's own install),
// with optional KEY=VALUE lines in ux0:data/StarCraft/scvita_env.txt.
#include "sc_host.h"

#include "runtime/guest_region.h"
#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/sched_cooperative.h"
#include "runtime/win32_shims_advapi32.h"
#include "runtime/win32_shims_gdi32.h"
#include "runtime/win32_shims_kernel32.h"
#include "runtime/win32_shims_locale.h"
#include "runtime/win32_shims_memory.h"
#include "runtime/win32_shims_misc.h"
#include "runtime/win32_shims_psapi.h"
#include "runtime/win32_shims_shell32.h"
#include "runtime/win32_shims_sync.h"
#include "runtime/win32_shims_user32.h"
#include "runtime/win32_shims_version.h"
#include "runtime/win32_shims_wait.h"
#include "runtime/win32_shims_window.h"
#include "runtime/ds_emul.h"
#include "runtime/host_clock.h"

#include <cstdarg>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __vita__
#include "platform/vita_host.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#define SC_VITA_DIR "ux0:data/StarCraft"
extern "C" {
// Host newlib heap (box86 metadata has its own pool; guest memory is the arena).
int _newlib_heap_size_user = 64 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
const char* const wx86_vita_progress_path = SC_VITA_DIR "/scvita_progress.txt";
}
#endif

namespace sc {

std::string   g_root;
uint64_t      g_blt_fast = 0, g_blt_slow = 0;
d2rt::Bridge* g_br  = nullptr;
d2rt::Cpu*    g_cpu = nullptr;

static uint64_t g_t0_us = 0;
static int      g_max_unshimmed = 40;

#ifdef __vita__
static std::mutex g_log_mx;
static std::string g_log_buf;
static std::atomic<bool> g_log_async{false};
void log_flush() {
    std::string out;
    { std::lock_guard<std::mutex> lk(g_log_mx); out.swap(g_log_buf); }
    if (out.empty()) return;
    if (FILE* f = std::fopen(SC_VITA_DIR "/scvita_log.txt", "a")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
}
static int logger_main(SceSize, void*) {
    for (;;) { sceKernelDelayThread(200000); log_flush(); }
    return 0;
}
static void start_logger() {
    SceUID t = sceKernelCreateThread("sc_logger", logger_main, 0x10000100 + 20, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (t >= 0 && sceKernelStartThread(t, 0, nullptr) >= 0) g_log_async.store(true);
}
#else
void log_flush() {}
#endif
void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
#ifdef __vita__
    char line[1100];
    const int n = std::snprintf(line, sizeof line, "%8.3f %s\n", (wx86_now_us() - g_t0_us) / 1e6, buf);
    if (g_log_async.load()) {
        // A memory-card write costs up to a few hundred ms; done on the display
        // thread it showed up as one missed frame every 10 s window (the worst
        // 130-350 ms hitches). The logger thread writes every 200 ms instead.
        std::lock_guard<std::mutex> lk(g_log_mx); g_log_buf.append(line, n > 0 ? (size_t)n : 0);
    } else if (FILE* f = std::fopen(SC_VITA_DIR "/scvita_log.txt", "a")) {   // early boot: direct
        std::fwrite(line, 1, n > 0 ? (size_t)n : 0, f); std::fclose(f);
    }
#else
    std::fprintf(stdout, "%s\n", buf);
    std::fflush(stdout);
#endif
}

std::vector<uint8_t> read_file(const std::string& host) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(host.c_str(), "rb");
    if (!f) return v;
    struct stat st{};
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) {
        v.resize((size_t)st.st_size);
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

uint32_t put_str(d2rt::Cpu& c, uint32_t dst, const std::string& s, uint32_t cap) {
    if (!dst || !cap) return (uint32_t)s.size();
    uint32_t n = (uint32_t)std::min<size_t>(s.size(), cap - 1);
    c.write(dst, s.data(), n);
    uint8_t z = 0; c.write(dst + n, &z, 1);
    return n;
}

// Case-insensitive match of one component inside a host directory.
static std::string match_ci(const std::string& dir, const std::string& comp) {
    struct stat st{};
    if (stat((dir + "/" + comp).c_str(), &st) == 0) return comp;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (!strcasecmp(e->d_name, comp.c_str())) { std::string n = e->d_name; closedir(d); return n; }
        closedir(d);
    }
    return comp;
}

std::string resolve_path(const std::string& guest) {
    std::string p = guest;
    for (auto& ch : p) if (ch == '/') ch = '\\';
    // Absolute paths: strip the drive, then the game directory prefix.
    if (p.size() > 1 && p[1] == ':') p = p.substr(2);
    std::string gdir = std::string(kGuestDir).substr(2) + "\\";      // "\StarCraft\"
    if (!strncasecmp(p.c_str(), gdir.c_str(), gdir.size())) p = p.substr(gdir.size());
    else if (!strcasecmp(p.c_str(), gdir.substr(0, gdir.size() - 1).c_str())) p.clear();
    while (!p.empty() && p[0] == '\\') p = p.substr(1);
    if (!strncasecmp(p.c_str(), ".\\", 2)) p = p.substr(2);
    std::string host = g_root;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('\\', i);
        std::string comp = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!comp.empty() && comp != ".") host += "/" + match_ci(host, comp);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return host;
}

void note_unshimmed(d2rt::Cpu& c, const std::string& tag) {
    static std::set<std::string> seen;
    static int n = 0;
    if (!seen.insert(tag).second) return;
    ++n;
    log("[not-bound] [cause: missing shim] #%d %s ra=0x%08x", n, tag.c_str(), c.trap_retaddr());
    if (n >= g_max_unshimmed) {
        log("[not-bound] stopping after %d distinct unshimmed imports", n);
        std::fflush(nullptr);
        _exit(3);
    }
}

}  // namespace sc

using namespace sc;

// Guest memory plan: ONE packed arena, the Vita model (winx86 single-block
// arena, H(va) = va + membase), used on the qemu host too so host runs test
// the exact layout the console gets. Sized from measurement: a skirmish start
// peaks at ~2 MiB HeapAlloc and ~35 MiB VirtualAlloc (storm's SMem heap).
//   0x00000000  page 0 (main TIB)
//   0x00400000  StarCraft.exe (fixed base, no relocations), 0x2EC000
//   0x00800000  HeapAlloc            16 MiB
//   0x01800000  DLL window           4 MiB (storm, local, smackw32, *.snp ~1.2 MiB)
//   0x01C00000  scratch              8 MiB
//   0x02400000  VirtualAlloc         96 MiB
//   0x08400000  worker stacks        16 x 1 MiB
//   0x09400000  worker TIBs          1 MiB
//   0x09500000  main stack           2 MiB
//   0x09800000  native-thunk window  1 MiB
//   0x0A000000  end of arena (160 MiB)
static constexpr uint32_t kHeapBase    = 0x00800000, kHeapSize    = 0x01000000;
static constexpr uint32_t kModBase     = 0x01800000, kModEnd      = 0x01C00000;
static constexpr uint32_t kScratchBase = 0x01C00000, kScratchSize = 0x00800000;
static constexpr uint32_t kVaBase      = 0x02400000, kVaSize      = 0x06000000;
static constexpr uint32_t kStacks      = 0x08400000;
static constexpr uint32_t kTibs        = 0x09400000;
static constexpr uint32_t kMainStack   = 0x09500000, kMainStackSize = 0x00200000;
static constexpr uint32_t kTrapBase    = 0x09800000;
static constexpr uint32_t kArenaEnd    = 0x0A000000;

static wx86::GuestRegion g_heap, g_va;
void sc::guest_mem(uint32_t* hc, uint32_t* hp, uint32_t* vc, uint32_t* vp) {
    *hc = g_heap.used_bytes(); *hp = g_heap.peak(); *vc = g_va.used_bytes(); *vp = g_va.peak();
}

static bool vquery(uint32_t a, uint32_t& base, uint32_t& size, uint32_t& state,
                   uint32_t& type, uint32_t& prot, uint32_t& allocbase) {
    auto in = [&](uint32_t b, uint32_t s) { return a >= b && a < b + s; };
    if (in(0x00400000, 0x002EC000)) { base = 0x00400000; size = 0x002EC000; type = 0x1000000; }
    else if (in(kHeapBase, kHeapSize)) { base = kHeapBase; size = kHeapSize; type = 0x20000; }
    else if (in(kVaBase, kVaSize)) { base = kVaBase; size = kVaSize; type = 0x20000; }
    else return false;
    state = 0x1000; prot = 0x04; allocbase = base;   // MEM_COMMIT, PAGE_READWRITE
    return true;
}

int main(int argc, char** argv) {
#ifdef __vita__
    (void)argc; (void)argv;
    g_root = SC_VITA_DIR;
    g_t0_us = wx86_now_us();
    mkdir(SC_VITA_DIR, 0777);          // so the log can be written even before the game files are there
    // One log per run; the previous run's log is kept so a crash log survives a relaunch.
    std::remove(SC_VITA_DIR "/scvita_log_prev.txt");
    std::rename(SC_VITA_DIR "/scvita_log.txt", SC_VITA_DIR "/scvita_log_prev.txt");
    if (FILE* f = std::fopen(SC_VITA_DIR "/scvita_log.txt", "w")) std::fclose(f);
    // The engine reports guest faults (thread, EIP, stack scan) on stdout, which
    // the Vita throws away: the mid-game freezes were the main game thread
    // dying silently while the music thread kept going. Keep that output.
    std::remove(SC_VITA_DIR "/scvita_stdout_prev.txt");
    std::rename(SC_VITA_DIR "/scvita_stdout.txt", SC_VITA_DIR "/scvita_stdout_prev.txt");
    if (std::freopen(SC_VITA_DIR "/scvita_stdout.txt", "w", stdout)) std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    setenv("THREADLOG", "1", 0);   // also report guest threads that end normally
    if (FILE* f = std::fopen(SC_VITA_DIR "/scvita_env.txt", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            char* e = std::strchr(line, '='); if (!e || line[0] == '#') continue;
            *e = 0; char* v = e + 1; v[std::strcspn(v, "\r\n")] = 0;
            setenv(line, v, 1);
        }
        std::fclose(f);
    }
    log("[not-bound] scvita start: game dir %s", SC_VITA_DIR);
    setenv("WX86_JITPROFILE", "1", 0);   // diagnostic builds: JIT cost counters in the 10 s log line
    g_max_unshimmed = 1000;            // never stop the game for an unshimmed import on console
    // Guest x86 runs on the main thread: core 0. Clocks: request the system
    // maximum; K's overclock plugin raises the CPU to 500 MHz on top.
    wx86_vita_pin_self(SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    wx86_vita_core_register("guest", sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0, 0);
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    log("[not-bound] clocks: cpu %d MHz, bus %d, gpu %d, xbar %d (500/222 needs K's overclock plugin)",
        scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
        scePowerGetGpuXbarClockFrequency());
    if (!present_init()) { log("[not-bound] [cause: presenter init failed] stopping"); return 1; }
    start_logger();
#else
    if (argc < 2) { std::fprintf(stderr, "usage: %s <game-dir> [max-unshimmed]\n", argv[0]); return 2; }
    g_root = argv[1];
    if (argc > 2) g_max_unshimmed = std::atoi(argv[2]);
#endif

    // The engine reads its layout from the environment (lazily, before the
    // CPU and bridge exist). Set it here so no launcher has to.
    {
        char v[16];
        // +16 MiB: slack for the 16 MiB membase alignment the engine needs.
        std::snprintf(v, sizeof v, "%x", kArenaEnd + 0x01000000u); setenv("WX86_ARENA", v, 0);
        std::snprintf(v, sizeof v, "%x", kModBase);   setenv("WX86_MODBASE", v, 1);
        std::snprintf(v, sizeof v, "%x", kMainStack); setenv("WX86_STACKBASE", v, 1);
        std::snprintf(v, sizeof v, "%x", kTrapBase);  setenv("WX86_TRAPBASE", v, 1);
    }
    d2rt::Cpu* cpu = d2rt::make_cpu_box86();
    if (!cpu) { log("[not-bound] [cause: dynarec init] make_cpu_box86 failed"); return 1; }
    g_cpu = cpu;
    static d2rt::Bridge br(cpu);
    g_br = &br;
    br.set_module_limit(kModEnd);

    std::string err;
    for (const char* f : { "StarCraft.exe", "storm.dll" }) {
        std::vector<uint8_t> bytes = read_file(resolve_path(f));
        if (bytes.empty()) { log("[not-bound] [cause: file not found] %s", f); return 1; }
        d2rt::PeImage* pi = br.add_module(f, bytes, err);
        if (!pi) { log("[not-bound] [cause: load error] %s: %s", f, err.c_str()); return 1; }
        log("[not-bound] loaded %-13s base=0x%08x size=0x%07x entry=0x%08x imports=%zu",
            f, pi->load_base(), pi->image_size(), pi->entry_va(), pi->imports().size());
    }

    // Main thread TIB lives at linear 0 (winx86 fs-direct convention).
    cpu->map(0, 0x1000, nullptr, d2rt::P_RW);
    cpu->write_u32(0x18, 0);              // TEB.Self
    cpu->write_u32(0x00, 0xFFFFFFFFu);    // empty SEH chain
    wx86_set_main_tib(0);

    cpu->map(kHeapBase, kHeapSize, nullptr, d2rt::P_RW);
    cpu->map(kVaBase, kVaSize, nullptr, d2rt::P_RWX);
    cpu->map(kScratchBase, kScratchSize, nullptr, d2rt::P_RW);
    g_heap.init(kHeapBase, kHeapSize, 16, "heap");
    g_va.init(kVaBase, kVaSize, 0x10000, "va");
    wx86_scratch_init(kScratchBase, kScratchSize);
    wx86_set_perf_frequency(1000000);
    wx86_mem_set_total_phys_mb(304);

    // Engine shims first, then the port's own (last registration wins).
    win32_shims_kernel32_install(br);
    win32_shims_locale_install(br);
    win32_shims_sync_install(br);
    win32_shims_wait_install(br);
    win32_shims_psapi_install(br);
    Wx86MemoryPlan plan; plan.heap = &g_heap; plan.va = &g_va; plan.vquery = &vquery;
    win32_shims_memory_install(br, plan);
    win32_shims_advapi32_install(br);
    win32_shims_gdi32_install(br);
    win32_shims_user32_install(br);
    win32_shims_window_install(br);
    win32_shims_shell32_install(br);
    win32_shims_version_install(br);
    win32_shims_misc_install(br);
    sc::install_kernel32(br);
    sc::install_misc(br);
    sc::install_user32(br);
    sc::install_ddraw(br);
    {   // DirectSound: the engine's COM emulation, mixing at StarCraft's 22.05 kHz.
        static d2rt::dsound::HostOps ops;
        ops.va_alloc = [](uint32_t n) { return g_va.alloc(n); };
        ops.tick_ms  = [](d2rt::Cpu&) { return (uint32_t)wx86_now_ms(); };
        ops.write_root = ".";
        ops.mix_rate = 22050;
        d2rt::dsound::set_logger([](const char* l) { log("[not-bound] dsound: %s", l); });
        d2rt::dsound::install(br, *cpu, ops);
    }
    br.set_version_resource_source([](const std::string& g) { return read_file(resolve_path(g)); });

    br.set_default_shim([](d2rt::Cpu& c, const std::string& tag) -> uint32_t {
        note_unshimmed(c, tag);
        return 0;
    });

    if (!br.commit(err)) { log("[not-bound] [cause: map error] commit: %s", err.c_str()); return 1; }
    if (!br.link(err))   { log("[not-bound] [cause: link error] link: %s", err.c_str()); return 1; }
    log("[not-bound] linked: %u imports routed to the catch-all shim", br.unresolved_count());

    // SC_TRACE_LOOKUP=1: observe storm's internal hash-table probe (rva
    // 0x14610: ecx=archive, eax=name, [esp+4]=locale, [esp+8]=platform),
    // redo the probe natively for comparison, then replay `push ebp; mov ebp,esp`.
    if (std::getenv("SC_TRACE_LOOKUP")) {
        uint32_t entry = br.module("storm.dll")->load_base() + 0x14610;
        d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native.hook!storm_lookup";
        s.fn = [entry](d2rt::Cpu& c) -> uint32_t {
            static uint32_t ct[0x500]; static bool init = false;
            if (!init) { uint32_t seed = 0x100001;
                for (int i = 0; i < 0x100; ++i) for (int j = 0, k = i; j < 5; ++j, k += 0x100) {
                    seed = (seed * 125 + 3) % 0x2AAAAB; uint32_t a = (seed & 0xFFFF) << 16;
                    seed = (seed * 125 + 3) % 0x2AAAAB; ct[k] = a | (seed & 0xFFFF); }
                init = true; }
            const uint32_t E = c.reg(d2rt::R_ESP);
            uint32_t ar = c.reg(d2rt::R_ECX), namep = c.reg(d2rt::R_EAX);
            std::string nm = wx86_gread_mb(c, namep, -1);
            auto hs = [&](int t) { uint32_t s1 = 0x7FED7FED, s2 = 0xEEEEEEEE;
                for (unsigned char ch : nm) { ch = (unsigned char)toupper(ch);
                    s1 = ct[t * 256 + ch] ^ (s1 + s2); s2 = ch + s1 + s2 + (s2 << 5) + 3; } return s1; };
            uint32_t h0 = hs(0), h1 = hs(1), h2 = hs(2);
            uint32_t hdr = c.read_u32(ar + 0x130), tab = c.read_u32(ar + 0x138), n = c.read_u32(hdr + 0x18);
            log("[not-bound] lookup \"%s\" ar=0x%08x loc=0x%x plat=0x%x hdr=0x%08x tab=0x%08x n=%u h=%08x/%08x/%08x",
                nm.c_str(), ar, c.read_u32(E + 4) & 0xffff, c.read_u32(E + 8) & 0xff, hdr, tab, n, h0, h1, h2);
            if (n && !(n & (n - 1)) && nm.find("gluexp") != std::string::npos)
                for (uint32_t i = h0 & (n - 1), k = 0; k < 16; ++k, i = (i + 1) & (n - 1)) {
                    uint32_t e = tab + i * 16;
                    log("[not-bound]   slot %u: %08x %08x loc=%04x plat=%02x blk=%08x", i, c.read_u32(e), c.read_u32(e + 4),
                        c.read_u32(e + 8) & 0xffff, (c.read_u32(e + 8) >> 16) & 0xff, c.read_u32(e + 12));
                    if (c.read_u32(e + 12) == 0xffffffff) break;
                }
            uint32_t ebp = c.reg(d2rt::R_EBP);
            c.write_u32(E - 4, ebp); c.set_reg(d2rt::R_EBP, E - 4);
            c.set_reg(d2rt::R_ESP, E - 8);            // epilogue adds 4 -> ESP = E-4
            g_br->redirect_next(entry + 3);
            return c.reg(d2rt::R_EAX);
        };
        br.register_shim("native.hook", "storm_lookup", s);
        cpu->set_alternate(entry, br.shim_trap("native.hook", "storm_lookup"));
    }

    // storm SBltROP3 (#313), SRCCOPY/PATCOPY on 4-byte-aligned rows of at
    // most 640 pixels, done natively. This is storm's own "fast path": it runs
    // a runtime-generated row copier and loops with `jmp ebx`, but the copier
    // storm generates for SRCCOPY uses ebx as its scratch register, so the
    // second row jumps to pixel data (reproduced identically under a reference
    // x86 emulator, so it is not a translation bug). Doing these two ROPs
    // natively also skips the dynarec for the hottest blit. Every other case
    // (other ROPs, unaligned, wider) runs storm's own code unchanged.
    if (!std::getenv("SC_STORM_BLT")) {
        uint32_t entry = br.module("storm.dll")->export_va_ordinal(313);
        d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native.hle!SBltROP3";
        s.fn = [entry](d2rt::Cpu& c) -> uint32_t {
            const uint32_t E = c.reg(d2rt::R_ESP);
            uint32_t a[8]; for (int i = 0; i < 8; ++i) a[i] = c.read_u32(E + 4 + 4 * i);
            const uint32_t dst = a[0], src = a[1], w = a[2], h = a[3], dp = a[4], sp = a[5], pat = a[6], rop = a[7];
            const bool fast = (int32_t)w > 0 && (int32_t)h > 0 && ((dst | w) & 3) == 0 && w <= 0x280 &&
                              (rop == 0x00CC0020u || rop == 0x00F00021u);
            if (fast) ++g_blt_fast; else ++g_blt_slow;
            if (!fast) {   // replay storm's `push ebp; mov ebp,esp` and run the original
                uint32_t ebp = c.reg(d2rt::R_EBP);
                c.write_u32(E - 4, ebp); c.set_reg(d2rt::R_EBP, E - 4);
                c.set_reg(d2rt::R_ESP, E - 8);
                g_br->redirect_next(entry + 3);
                return c.reg(d2rt::R_EAX);
            }
            for (uint32_t y = 0; y < h; ++y) {
                uint32_t d = dst + y * dp;
                if (rop == 0x00CC0020u) {
                    uint32_t s2 = src + y * sp;
                    uint8_t* hd = (uint8_t*)c.hostptr(d, w); const uint8_t* hs = (const uint8_t*)c.hostptr(s2, w);
                    if (hd && hs) std::memmove(hd, hs, w);
                    else { std::vector<uint8_t> t(w); c.read(s2, t.data(), w); c.write(d, t.data(), w); }
                } else {
                    uint32_t* hd = (uint32_t*)c.hostptr(d, w);
                    if (hd) for (uint32_t i = 0; i < w / 4; ++i) hd[i] = pat;
                    else for (uint32_t i = 0; i < w / 4; ++i) c.write_u32(d + 4 * i, pat);
                }
            }
            const uint32_t ret = c.read_u32(E);
            c.set_reg(d2rt::R_ESP, E + 4 * 8);   // stdcall pops 8 args; the epilogue adds the final 4
            g_br->redirect_next(ret);
            return 1u;
        };
        br.register_shim("native.hle", "SBltROP3", s);
        cpu->set_alternate(entry, br.shim_trap("native.hle", "SBltROP3"));
    }

    // SC_SNAP_AT=<hex>,<dir>[,<string>]: at that address (optionally only when
    // the 2nd stack arg points at <string>), write the guest registers and
    // memory regions to <dir> for offline differential replay, then exit.
    if (const char* sa = std::getenv("SC_SNAP_AT")) {
        std::string spec = sa; uint32_t at = (uint32_t)std::strtoul(spec.c_str(), nullptr, 16);
        size_t c1 = spec.find(','), c2 = spec.find(',', c1 + 1);
        std::string dir = spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        std::string want = c2 == std::string::npos ? "" : spec.substr(c2 + 1);
        d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native.hook!snap_at";
        s.fn = [at, dir, want](d2rt::Cpu& c) -> uint32_t {
            const uint32_t E = c.reg(d2rt::R_ESP);
            static int hits = 0; ++hits;
            log("[not-bound] snap-point hit %d: ra=0x%08x arg2=\"%s\"", hits, c.read_u32(E), wx86_gread_mb(c, c.read_u32(E + 8), 40).c_str());
            const char* nth = std::getenv("SC_SNAP_NTH");
            if (nth ? hits == std::atoi(nth) : (want.empty() || wx86_gread_mb(c, c.read_u32(E + 8), -1) == want)) {
                mkdir(dir.c_str(), 0777);
                FILE* r = std::fopen((dir + "/regs.txt").c_str(), "w");
                const int ids[] = { d2rt::R_EAX, d2rt::R_ECX, d2rt::R_EDX, d2rt::R_EBX, d2rt::R_ESP, d2rt::R_EBP, d2rt::R_ESI, d2rt::R_EDI };
                for (int id : ids) std::fprintf(r, "%08x\n", c.reg(id));
                std::fprintf(r, "%08x\n", at); std::fclose(r);
                struct Rg { uint32_t a, n; } rg[] = { { 0, 0x1000 }, { 0x00400000, 0x002EC000 }, { kHeapBase, 0x00800000 },
                    { kModBase, kModEnd - kModBase }, { kScratchBase, kScratchSize }, { kVaBase, 0x00C00000 }, { kMainStack, kMainStackSize } };
                for (auto& g : rg) {
                    std::vector<uint8_t> b(g.n); c.read(g.a, b.data(), g.n);
                    char fn[64]; std::snprintf(fn, sizeof fn, "/mem_%08x.bin", g.a);
                    if (FILE* o = std::fopen((dir + fn).c_str(), "wb")) { std::fwrite(b.data(), 1, g.n, o); std::fclose(o); }
                }
                log("[not-bound] snapshot at 0x%08x written to %s", at, dir.c_str());
                std::fflush(stdout); _exit(4);
            }
            uint32_t ebp = c.reg(d2rt::R_EBP);   // replay `push ebp; mov ebp,esp`
            c.write_u32(E - 4, ebp); c.set_reg(d2rt::R_EBP, E - 4);
            c.set_reg(d2rt::R_ESP, E - 8);
            g_br->redirect_next(at + 3);
            return c.reg(d2rt::R_EAX);
        };
        br.register_shim("native.hook", "snap_at", s);
        cpu->set_alternate(at, br.shim_trap("native.hook", "snap_at"));
    }

    // SC_TRAP_AT=<hex>: report the guest state when execution reaches that
    // address (e.g. a garbage jump target), then stop.
    if (const char* ta = std::getenv("SC_TRAP_AT")) {
        uint32_t at = (uint32_t)std::strtoul(ta, nullptr, 16);
        d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native.hook!trap_at";
        s.fn = [at](d2rt::Cpu& c) -> uint32_t {
            uint32_t E = c.reg(d2rt::R_ESP);
            log("[not-bound] reached 0x%08x: EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x", at,
                c.reg(d2rt::R_EAX), c.reg(d2rt::R_EBX), c.reg(d2rt::R_ECX), c.reg(d2rt::R_EDX),
                c.reg(d2rt::R_ESI), c.reg(d2rt::R_EDI), c.reg(d2rt::R_EBP), E);
            for (int k = -4; k < 16; ++k) log("[not-bound]   [esp%+d] = %08x", 4 * k, c.read_u32(E + 4 * k));
            if (const char* pk = std::getenv("SC_TRAP_PEEK"))   // comma-separated hex addresses
                for (const char* q = pk; *q; ) { char* e2; uint32_t a = (uint32_t)std::strtoul(q, &e2, 16);
                    log("[not-bound]   peek %08x: %08x %08x %08x %08x", a, c.read_u32(a), c.read_u32(a + 4), c.read_u32(a + 8), c.read_u32(a + 12));
                    q = *e2 ? e2 + 1 : e2; }
            if (const char* dd = std::getenv("SC_TRAP_DUMP")) {   // addr,len,hostfile
                uint32_t a = 0, n = 0; char f[256] = {0};
                if (std::sscanf(dd, "%x,%x,%255s", &a, &n, f) == 3) {
                    std::vector<uint8_t> b(n); c.read(a, b.data(), n);
                    if (FILE* o = std::fopen(f, "wb")) { std::fwrite(b.data(), 1, n, o); std::fclose(o); }
                }
            }
            std::fflush(stdout); _exit(3);
        };
        br.register_shim("native.hook", "trap_at", s);
        cpu->set_alternate(at, br.shim_trap("native.hook", "trap_at"));
    }

    // SC_TRACE_MPQ=1: log every file the exe asks storm for (SFileOpenFile
    // #267, SFileOpenFileEx #268), then fall through to the real storm code.
    if (std::getenv("SC_TRACE_MPQ")) {
        for (uint32_t ord : { 267u, 268u }) {
            uint32_t real = br.module("storm.dll")->export_va_ordinal(ord);
            d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false;
            s.tag = "native.hook!storm#" + std::to_string(ord);
            s.fn = [real, ord](d2rt::Cpu& c) -> uint32_t {
                const uint32_t E = c.reg(d2rt::R_ESP);
                uint32_t namep = c.read_u32(E + 4 + (ord == 268 ? 4 : 0));
                log("[not-bound] storm#%u open \"%s\" ra=0x%08x mpq=0x%08x scope=0x%x", ord,
                    wx86_gread_mb(c, namep, -1).c_str(), c.read_u32(E), c.read_u32(E + 4),
                    ord == 268 ? c.read_u32(E + 12) : 0);
                if (uint32_t h = c.read_u32(E + 4)) {   // which archive: storm keeps its path in the handle
                    char buf[0x200]; c.read(h, buf, sizeof buf);
                    for (int k = 0; k + 4 < (int)sizeof buf; ++k)
                        if (!strncasecmp(buf + k, "C:\\", 3)) { log("[not-bound]   archive handle 0x%08x = %.*s", h, 120, buf + k); break; }
                }
                c.set_reg(d2rt::R_ESP, E - 4);   // epilogue pops 4: frame stays intact
                g_br->redirect_next(real);
                return c.reg(d2rt::R_EAX);
            };
            std::string nm = "storm" + std::to_string(ord);
            br.register_shim("native.hook", nm, s);
            // Bridge::repatch_import matches by name only; these are ordinal imports.
            uint32_t trap = br.shim_trap("native.hook", nm);
            int n = 0;
            for (const char* key : { "StarCraft.exe" }) {
                d2rt::PeImage* pi = br.module(key);
                for (const auto& imp : pi->imports())
                    if (imp.name.empty() && imp.ordinal == ord && !strcasecmp(imp.dll.c_str(), "storm.dll")) {
                        pi->patch_iat(imp.iat_va, trap); cpu->write_u32(imp.iat_va, trap); ++n;
                    }
            }
            log("[not-bound] traced storm#%u at %d import slots", ord, n);
        }
    }

    // storm.dll DllMain(DLL_PROCESS_ATTACH) before the exe runs, as Windows does.
    {
        d2rt::PeImage* st = br.module("storm.dll");
        uint32_t ret = 0; const char* fault = nullptr;
        // Windows passes a NON-NULL lpReserved to DllMain for DLLs loaded
        // statically with the process (NULL only for LoadLibrary). storm.dll
        // branches on it: with NULL it builds its MPQ crypt table from the
        // wrong seed (0x10000100 instead of 0x100001) and no archive lookup
        // ever matches. Pass a real non-NULL pointer, as the OS does.
        uint32_t reserved = wx86_scratch_alloc(0x400);
        bool ok = br.call_va(st->entry_va(), { st->load_base(), 1u, reserved }, ret, &fault);
        log("[not-bound] storm.dll DllMain -> ok=%d ret=%u%s%s", ok, ret, fault ? " fault=" : "", fault ? fault : "");
        if (!ok) return 1;
    }

    static d2rt::CooperativeScheduler sched(cpu, &br, kStacks, 0x00100000, kTibs);
    wx86_set_scheduler(&sched);
    // Real-time clock: timed waits (Sleep, WaitForSingleObject timeouts) last
    // real milliseconds and expire even while another guest thread is
    // runnable. Required on the Vita: in virtual mode a busy menu loop never
    // lets virtual time move, so the music streaming thread's Sleep never
    // ends and the sound buffer loops a tiny section (K, build 6). The host
    // keeps the deterministic virtual clock unless SC_REALCLOCK=1.
    {
#ifdef __vita__
        const bool real = true;
#else
        const char* rc = std::getenv("SC_REALCLOCK"); const bool real = rc && std::atoi(rc) != 0;
#endif
        if (real) {
            sched.set_real_clock([] { return (uint64_t)wx86_now_ms(); },
                                 [](uint32_t ms) { if (!ms) return;
#ifdef __vita__
                                     sceKernelDelayThread(ms * 1000u);
#else
                                     usleep(ms * 1000u);
#endif
                                 });
            sched.set_real_expire(true);
            log("[not-bound] scheduler clock: real time");
        }
    }
    uint32_t entry = br.module("StarCraft.exe")->entry_va();
    sched.set_main(entry, kMainStack + kMainStackSize - 0x1000u, 0);
    log("[not-bound] entering StarCraft.exe at 0x%08x", entry);
    sched.run();
    log("[not-bound] scheduler stopped: %s", sched.stop_reason());
    return 0;
}
