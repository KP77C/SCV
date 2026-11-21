#include <string>
#include <atomic>
#include <mutex>
// boot.cpp — Rainbow Six: Black Ops 2.0 Vita port boot harness.
//
// Loads the user's own R6BOLegacy.exe (Black Thorn v2.61 engine) and the DLLs it links
// against from its install folder into one guest address
// space, installs the winx86 engine shims then the port's own, runs the DLLs'
// DllMains, and starts the exe's entry point on the cooperative scheduler.
// Every import that reaches the catch-all shim is logged once, in call order:
// that list is the work queue for the next shims.
//
// Usage (host, qemu-arm):  r6vita_boot <game-dir> [max-unshimmed]
// <game-dir> is the install folder (R6BOLegacy.exe, FFC11.dll, data/, music/ ...).
// On the Vita it is ux0:data/DeusEx/, with optional KEY=VALUE lines in
// ux0:data/DeusEx/r6vita_env.txt.
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
#include <algorithm>
#include <vector>
#include <functional>
#include <cctype>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __vita__
#include "platform/vita_host.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#define SC_VITA_DIR "ux0:data/R6BlackOps"
extern "C" {
// Host newlib heap (box86 metadata has its own pool; guest memory is the arena).
int _newlib_heap_size_user = 48 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
const char* const wx86_vita_progress_path = SC_VITA_DIR "/r6vita_progress.txt";
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
    if (FILE* f = std::fopen(SC_VITA_DIR "/r6vita_log.txt", "a")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
}
static int logger_main(SceSize, void*) {
    for (;;) { sceKernelDelayThread(200000); log_flush(); }
    return 0;
}
static void start_logger() {
    SceUID t = sceKernelCreateThread("w3_logger", logger_main, 0x10000100 + 20, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
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
    } else if (FILE* f = std::fopen(SC_VITA_DIR "/r6vita_log.txt", "a")) {   // early boot: direct
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
    // Guest paths: absolute ("C:\DeusEx\Maps\x.dx"), or relative to the
    // game's working directory, System ("..\Maps\x.dx", "DeusEx.ini").
    std::string p = guest;
    for (auto& ch : p) if (ch == '/') ch = '\\';
    std::string full;
    if (p.size() > 1 && p[1] == ':') full = p.substr(2);
    else if (!p.empty() && p[0] == '\\') full = p;
    else full = std::string(kGuestCwd).substr(2) + "\\" + p;
    // Normalize into components, resolving "." and "..".
    std::vector<std::string> comps;
    size_t i = 0;
    while (i <= full.size()) {
        size_t j = full.find('\\', i); if (j == std::string::npos) j = full.size();
        std::string c = full.substr(i, j - i);
        if (c == "..") { if (!comps.empty()) comps.pop_back(); }
        else if (!c.empty() && c != ".") comps.push_back(c);
        i = j + 1;
    }
    // Strip the game directory prefix ("\DeusEx").
    const std::string gdir = std::string(kGuestDir).substr(3);
    size_t k = 0;
    if (!comps.empty() && !strcasecmp(comps[0].c_str(), gdir.c_str())) k = 1;
    std::string host = g_root;
    for (; k < comps.size(); ++k) host += "/" + match_ci(host, comps[k]);
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

// Guest memory plan: ONE packed arena (winx86 single-block arena, H(va) =
// va + membase), the same layout on the qemu host and the Vita.
// R6BOLegacy.exe has no relocations and must sit at its preferred base
// 0x00400000 (image 0x4B0000); the DLLs it loads from the game folder
// (FFC11.dll ...) carry relocations and go in the module window.
//   0x00000000  page 0 (main TIB)
//   0x00400000  R6BOLegacy.exe       fixed, 0x4B0000
//   0x00900000  HeapAlloc            32 MiB (the MSVC CRT heap)
//   0x02900000  module window        4 MiB
//   0x02D00000  scratch              16 MiB (DirectDraw surfaces, COM objects)
//   0x03D00000  VirtualAlloc         128 MiB
//   0x0BD00000  worker stacks        16 x 1 MiB
//   0x0CD00000  worker TIBs          1 MiB
//   0x0CE00000  main stack           4 MiB
//   0x0D200000  native-thunk window  1 MiB
//   0x0D300000  end of arena (211 MiB)
static constexpr uint32_t kExeBase     = 0x00400000, kExeSize     = 0x004B0000;
static constexpr uint32_t kHeapBase    = 0x00900000, kHeapSize    = 0x02000000;
static constexpr uint32_t kModBase     = 0x02900000, kModEnd      = 0x02D00000;
static constexpr uint32_t kScratchBase = 0x02D00000, kScratchSize = 0x01000000;
static constexpr uint32_t kVaBase      = 0x03D00000, kVaSize      = 0x08000000;
static constexpr uint32_t kStacks      = 0x0BD00000;
static constexpr uint32_t kTibs        = 0x0CD00000;
static constexpr uint32_t kMainStack   = 0x0CE00000, kMainStackSize = 0x00400000;
static constexpr uint32_t kTrapBase    = 0x0D200000;
static constexpr uint32_t kArenaEnd    = 0x0D300000;
static_assert(kExeBase + kExeSize <= kHeapBase && kHeapBase + kHeapSize <= kModBase && kModEnd <= kScratchBase && kScratchBase + kScratchSize <= kVaBase &&
              kVaBase + kVaSize <= kStacks && kStacks + 0x01000000u <= kTibs && kTibs + 0x00100000u <= kMainStack &&
              kMainStack + kMainStackSize <= kTrapBase && kTrapBase + 0x00100000u <= kArenaEnd, "layout overlap");

static wx86::GuestRegion g_heap, g_va;
void sc::guest_mem(uint32_t* hc, uint32_t* hp, uint32_t* vc, uint32_t* vp) {
    *hc = g_heap.used_bytes(); *hp = g_heap.peak(); *vc = g_va.used_bytes(); *vp = g_va.peak();
}

uint32_t sc::surf_alloc(uint32_t n) { return g_va.alloc(n); }
void sc::surf_free(uint32_t va) { if (va) g_va.free(va); }

void sc::dump_arena(d2rt::Cpu& c, const std::string& dir) {
    struct Rg { uint32_t a, n; } rg[] = {
        { 0, 0x1000 }, { kExeBase, kExeSize }, { kHeapBase, (g_heap.peak() + 0xFFFFu) & ~0xFFFFu }, { kModBase, kModEnd - kModBase },
        { kScratchBase, kScratchSize }, { kVaBase, (g_va.peak() + 0xFFFFu) & ~0xFFFFu },
        { kStacks, kArenaEnd - kStacks } };
    for (auto& g : rg) {
        if (!g.n) continue;
        std::vector<uint8_t> b(g.n); c.read(g.a, b.data(), g.n);
        char fn[64]; std::snprintf(fn, sizeof fn, "/mem_%08x.bin", g.a);
        if (FILE* o = std::fopen((dir + fn).c_str(), "wb")) { std::fwrite(b.data(), 1, g.n, o); std::fclose(o); }
    }
}

#if 0  // Deus Ex only
// The port's own settings file, System/DXVita.ini, made once from the game's
// DeusEx.ini with the port's overrides; the game is started with
// INI=DXVita.ini, so the PC settings in DeusEx.ini are never touched.
static void make_port_ini() {
    const std::string out = resolve_path("DXVita.ini");
    if (FILE* f = std::fopen(out.c_str(), "rb")) { std::fclose(f); return; }
    std::vector<uint8_t> src = read_file(resolve_path("DeusEx.ini"));
    if (src.empty()) { log("[not-bound] [cause: file not found] DeusEx.ini"); return; }
    struct Ov { const char* section; const char* key; const char* value; };
    static const Ov ov[] = {
        { "Engine.Engine", "GameRenderDevice", "SoftDrv.SoftwareRenderDevice" },
        { "Engine.Engine", "WindowedRenderDevice", "SoftDrv.SoftwareRenderDevice" },
        { "Engine.Engine", "RenderDevice", "SoftDrv.SoftwareRenderDevice" },
        { "WinDrv.WindowsClient", "StartupFullscreen", "False" },
        { "WinDrv.WindowsClient", "UseDirectDraw", "False" },
        { "WinDrv.WindowsClient", "WindowedColorBits", "16" },
    };
    std::string text(src.begin(), src.end()), res, section;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i); if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        std::string bare = line; if (!bare.empty() && bare.back() == '\r') bare.pop_back();
        if (!bare.empty() && bare[0] == '[') section = bare.substr(1, bare.find(']') - 1);
        size_t eq = bare.find('=');
        if (eq != std::string::npos)
            for (const Ov& o : ov)
                if (section == o.section && bare.substr(0, eq) == o.key) {
                    line = std::string(o.key) + "=" + o.value + (line.back() == '\r' ? "\r" : "");
                    break;
                }
        res += line; if (j < text.size()) res += "\n";
        i = j + 1;
    }
    if (FILE* f = std::fopen(out.c_str(), "wb")) { std::fwrite(res.data(), 1, res.size(), f); std::fclose(f); }
    log("[not-bound] wrote %s (software renderer, windowed)", out.c_str());
}
#endif

static bool vquery(uint32_t a, uint32_t& base, uint32_t& size, uint32_t& state,
                   uint32_t& type, uint32_t& prot, uint32_t& allocbase) {
    auto in = [&](uint32_t b, uint32_t s) { return a >= b && a < b + s; };
    if (in(kExeBase, kExeSize)) { base = kExeBase; size = kExeSize; type = 0x1000000; }   // MEM_IMAGE
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
    std::remove(SC_VITA_DIR "/r6vita_log_prev.txt");
    std::rename(SC_VITA_DIR "/r6vita_log.txt", SC_VITA_DIR "/r6vita_log_prev.txt");
    if (FILE* f = std::fopen(SC_VITA_DIR "/r6vita_log.txt", "w")) std::fclose(f);
    // The engine reports guest faults (thread, EIP, stack scan) on stdout, which
    // the Vita throws away: the mid-game freezes were the main game thread
    // dying silently while the music thread kept going. Keep that output.
    std::remove(SC_VITA_DIR "/r6vita_stdout_prev.txt");
    std::rename(SC_VITA_DIR "/r6vita_stdout.txt", SC_VITA_DIR "/r6vita_stdout_prev.txt");
    if (std::freopen(SC_VITA_DIR "/r6vita_stdout.txt", "w", stdout)) std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    setenv("THREADLOG", "1", 0);   // also report guest threads that end normally
    if (FILE* f = std::fopen(SC_VITA_DIR "/r6vita_env.txt", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            char* e = std::strchr(line, '='); if (!e || line[0] == '#') continue;
            *e = 0; char* v = e + 1; v[std::strcspn(v, "\r\n")] = 0;
            setenv(line, v, 1);
        }
        std::fclose(f);
    }
    log("[not-bound] r6vita start: game dir %s", SC_VITA_DIR);
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
    // SC_FPSCR=hex: run with the Vita's floating-point mode (flush-to-zero,
    // default NaN) to compare against the console.
    if (const char* fp = std::getenv("SC_FPSCR")) { uint32_t v = (uint32_t)std::strtoul(fp, nullptr, 16); asm volatile("vmsr fpscr, %0" :: "r"(v)); }
#endif
    // Never mute a looping sound ring for going unrefilled (a Deus Ex rule):
    // Miles refills WarCraft III's mixer ring from a timer thread that goes
    // longer than the rule's 300 ms at console frame rates.
    setenv("WX86_SON_STALE_MS", "0", 0);
    // The engine requires DirectSound 3D (listener + 3D buffers): mixed in software.
    setenv("WX86_SON_3D", "1", 0);
    // The ARM floating-point mode the guest's x87/SSE code runs under: the
    // Vita's default may differ from Linux's (flush-to-zero, default NaN).
    { uint32_t fpscr; asm volatile("vmrs %0, fpscr" : "=r"(fpscr)); log("[not-bound] fpscr 0x%08x", fpscr); }
    {   // What one clock read costs: every timer shim and the shim profiler pay it.
        volatile uint64_t sink = 0;
        const uint64_t a = wx86_now_us();
        for (int i = 0; i < 4096; ++i) sink = sink + wx86_now_us();
        const uint64_t b = wx86_now_us();
        log("[not-bound] clock read: %.3f us each (4096 in %llu us)", (double)(b - a) / 4096.0, (unsigned long long)(b - a));
    }

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
    // DeusEx.exe and, depth first, every import DLL that exists in the game's
    // System folder (Core, Engine, Window, the game's own MSVCRT.dll ...).
    // Anything else (KERNEL32, USER32 ...) is a shim. DllMains run later in
    // dependency order (deepest first), as the Windows loader does.
    std::vector<d2rt::PeImage*> init_order;
    std::vector<std::pair<uint32_t, std::pair<std::string, std::vector<uint8_t>>>> static_files;
    {
        std::set<std::string> seen;
        std::function<bool(const std::string&)> load = [&](const std::string& f) -> bool {
            std::string lb = f; for (auto& ch : lb) ch = (char)std::tolower((unsigned char)ch);
            if (!seen.insert(lb).second) return true;
            // Folder DLLs the port replaces: the PC graphics wrappers
            // (DDrawCompat as ddraw.dll/ddcpt.dll), ogg-winmm's winmm.dll
            // (CD music; the port plays music/trackNN.ogg itself) and
            // Creative's eax.dll (EAX.DLL #5 forwards to DirectSoundCreate).
            for (const char* skip : { "ddraw.dll", "ddcpt.dll", "winmm.dll", "eax.dll", "dsound.dll", "dinput.dll" })
                if (lb == skip) return true;
            std::vector<uint8_t> bytes = read_file(resolve_path(f));
            if (bytes.empty()) return true;                          // a system DLL: shims
            d2rt::PeImage* pi = br.add_module(f, bytes, err);
            if (!pi) { log("[not-bound] [cause: load error] %s: %s", f.c_str(), err.c_str()); return false; }
            register_module(f, pi->load_base());
            static_files.emplace_back(pi->load_base(), std::make_pair(f, bytes));
            log("[not-bound] loaded %-14s base=0x%08x size=0x%07x entry=0x%08x imports=%zu",
                f.c_str(), pi->load_base(), pi->image_size(), pi->entry_va(), pi->imports().size());
            std::set<std::string> deps;
            for (auto& imp : pi->imports()) deps.insert(imp.dll);
            for (auto& d : deps) if (!load(d)) return false;
            if (strcasecmp(lb.c_str(), kExeName)) init_order.push_back(pi);
            return true;
        };
        if (!load(kExeName)) return 1;
        if (!br.module(kExeName)) { log("[not-bound] [cause: file not found] %s", kExeName); return 1; }
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
    // Memory diagnostics, always on: refused or suspicious VirtualAlloc
    // requests (with who asked), big ones, and each 16 MiB step of arena use.
    // Vita build 1 quit with Storm "error 8" (out of memory) during startup.
    wx86_mem_set_observer([](const WxMemEvent& e) {
        static uint32_t big = 0, step = 0;
        const uint32_t used = g_va.cur();
        switch (e.kind) {
        case WX86_MEM_VA_FAIL: case WX86_MEM_VA_GARBAGE:
            log("[RAM-bound] [cause: %s] VirtualAlloc(0x%08x, %u KiB, type 0x%x) ra=0x%08x; VA used %u/%u KiB (peak %u)",
                e.kind == WX86_MEM_VA_FAIL ? "VA arena refused" : "garbage size", e.addr, e.size >> 10, e.type, e.ra,
                used >> 10, kVaSize >> 10, g_va.peak() >> 10);
            if (e.cpu) log("[RAM-bound]   stack:%s", sc::guest_stack(*e.cpu).c_str());
            if (e.kind == WX86_MEM_VA_FAIL) {   // full, or fragmented? and who holds it
                static int dumps = 0;
                if (e.cpu && dumps < 2) sc::trace_report(*e.cpu);   // the biggest Storm allocators so far
                if (dumps++ < 2) {
                    std::vector<std::pair<uint32_t, uint32_t>> v;
                    for (auto& b : g_va.used_map()) v.push_back({ b.second, b.first });
                    std::sort(v.rbegin(), v.rend());
                    std::string top;
                    for (size_t i = 0; i < v.size() && i < 12; ++i) {
                        char t[48]; std::snprintf(t, sizeof t, " %uK@%08x", v[i].first >> 10, v[i].second); top += t;
                    }
                    log("[RAM-bound]   largest free hole %u KiB; %u blocks; biggest:%s", g_va.largest_free() >> 10, (unsigned)v.size(), top.c_str());
                }
            }
            break;
        case WX86_MEM_VA_BIG:
            if (e.size >= 0x00800000u || big < 24) {
                ++big;
                log("[not-bound] VirtualAlloc big: %u KiB type 0x%x ra=0x%08x (VA used %u KiB)", e.size >> 10, e.type, e.ra, used >> 10);
            }
            break;
        case WX86_MEM_VA_RELEASE_MISS:
            log("[RAM-bound] [cause: VirtualFree missed] 0x%08x ra=0x%08x", e.addr, e.ra);
            break;
        default: break;
        }
        if (used >> 24 > step) { step = used >> 24; log("[not-bound] VA arena use passed %u MiB (of %u)", step * 16, kVaSize >> 20); }
    });
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
    sc::install_d3d(br);
    sc::install_r6(br);
    sc::install_dx(br);
    sc::install_w3(br);
    sc::install_gl(br);
    sc::install_dshow(br);
    sc::install_net(br);
    {   // DirectSound: the engine's COM emulation, mixed at 22.05 kHz for now.
        static d2rt::dsound::HostOps ops;
        ops.va_alloc = [](uint32_t n) { return g_va.alloc(n); };
        ops.tick_ms  = [](d2rt::Cpu&) { return (uint32_t)wx86_now_ms(); };
        ops.write_root = ".";
        ops.mix_rate = 44100;   // Galaxy mixes at 44.1 kHz (OutputRate)
        d2rt::dsound::set_logger([](const char* l) { log("[not-bound] dsound: %s", l); });
        d2rt::dsound::install(br, *cpu, ops);
    }
    // W3_MEMLOG=1: log every VirtualAlloc/VirtualFree (address, size, type, caller).
    if (std::getenv("W3_MEMLOG")) for (const char* n : { "VirtualAlloc", "VirtualFree", "VirtualQuery" }) {
        const d2rt::Shim* s0 = br.find_shim("KERNEL32.dll", n); if (!s0) continue;
        d2rt::Shim s = *s0; std::string tag = n; auto inner = s.fn;
        s.fn = [tag, inner](d2rt::Cpu& c) {
            uint32_t a0 = c.arg(0), a1 = c.arg(1), a2 = c.arg(2), a3 = c.arg(3), ra = c.read_u32(c.reg(d2rt::R_ESP));
            uint32_t r = inner(c);
            log("[mem] %s(0x%08x, 0x%x, 0x%x, 0x%x) -> 0x%08x ra=0x%08x", tag.c_str(), a0, a1, a2, a3, r, ra);
            return r;
        };
        br.register_shim("KERNEL32.dll", n, s);
    }
    br.set_version_resource_source([](const std::string& g) { return read_file(resolve_path(g)); });

    br.set_default_shim([](d2rt::Cpu& c, const std::string& tag) -> uint32_t {
        note_unshimmed(c, tag);
        return 0;
    });

    if (!br.commit(err)) { log("[not-bound] [cause: map error] commit: %s", err.c_str()); return 1; }
    for (auto& sf : static_files) mark_writable_sections(*cpu, sf.first, sf.second.second, sf.second.first);
    static_files.clear();
    if (!br.link(err))   { log("[not-bound] [cause: link error] link: %s", err.c_str()); return 1; }
    log("[not-bound] linked: %u imports routed to the catch-all shim", br.unresolved_count());
    sc::install_w3_trace(*cpu, br);
    if (std::getenv("W3_LIST_UNBOUND")) {     // name every import with no shim and no loaded module
        for (const char* m : { kExeName, "FFC11.dll" })
            if (d2rt::PeImage* pi = br.module(m))
                for (auto& imp : pi->imports())
                    if (!br.module(imp.dll) && !br.find_shim(imp.dll, imp.name.empty() ? "#" + std::to_string(imp.ordinal) : imp.name))
                        log("[not-bound] unbound import %s: %s!%s#%u", m, imp.dll.c_str(), imp.name.c_str(), imp.ordinal);
    }

    // 0x14610: ecx=archive, eax=name, [esp+4]=locale, [esp+8]=platform),
    // redo the probe natively for comparison, then replay `push ebp; mov ebp,esp`.
    if (const char* ta = std::getenv("SC_TRAP_AT")) {
        uint32_t at = (uint32_t)std::strtoul(ta, nullptr, 16);
        d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native.hook!trap_at";
        s.fn = [at](d2rt::Cpu& c) -> uint32_t {
            uint32_t E = c.reg(d2rt::R_ESP);
            // SC_TRAP_NTH=n: stop at the n-th hit (0 = log every hit's count and
            // continue); the trapped instruction must be `push ebp; mov ebp,esp`.
            static int hits = 0; ++hits;
            const char* nth = std::getenv("SC_TRAP_NTH");
            if (nth && hits != std::atoi(nth)) {
                if (std::atoi(nth) == 0) log("[not-bound] trap hit %d esp=0x%08x ra=0x%08x", hits, E, c.read_u32(E));
                uint32_t ebp = c.reg(d2rt::R_EBP);
                c.write_u32(E - 4, ebp); c.set_reg(d2rt::R_EBP, E - 4);
                c.set_reg(d2rt::R_ESP, E - 8);
                g_br->redirect_next(at + 3);
                return c.reg(d2rt::R_EAX);
            }
            log("[not-bound] reached 0x%08x: EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x", at,
                c.reg(d2rt::R_EAX), c.reg(d2rt::R_EBX), c.reg(d2rt::R_ECX), c.reg(d2rt::R_EDX),
                c.reg(d2rt::R_ESI), c.reg(d2rt::R_EDI), c.reg(d2rt::R_EBP), E);
            for (int k = -4; k < 16; ++k) log("[not-bound]   [esp%+d] = %08x", 4 * k, c.read_u32(E + 4 * k));
            if (const char* sd = std::getenv("SC_TRAP_SNAP")) {   // dir: full snapshot for tools/diff/dxreplay.py
                std::string dir = sd; mkdir(dir.c_str(), 0777);
                FILE* r = std::fopen((dir + "/regs.txt").c_str(), "w");
                const int ids[] = { d2rt::R_EAX, d2rt::R_ECX, d2rt::R_EDX, d2rt::R_EBX, d2rt::R_ESP, d2rt::R_EBP, d2rt::R_ESI, d2rt::R_EDI };
                for (int id : ids) std::fprintf(r, "%08x\n", c.reg(id));
                std::fprintf(r, "%08x\n", at); std::fclose(r);
                sc::dump_arena(c, dir);
                log("[not-bound] snapshot at 0x%08x written to %s", at, dir.c_str());
            }
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

    // W3_LOG_CALLS=<hexaddr>:<name>:<nargs>[,...]: log the arguments of guest
    // functions that start with `push ebp; mov ebp,esp` and carry on.
    // W3_LOG_RANGE=<lo>-<hi> (hex): only calls whose first argument is in it.
    if (const char* lc = std::getenv("W3_LOG_CALLS")) {
        static uint32_t lo = 0, hi = 0xFFFFFFFFu;
        if (const char* lr = std::getenv("W3_LOG_RANGE")) std::sscanf(lr, "%x-%x", &lo, &hi);
        std::string spec = lc; size_t i = 0;
        while (i < spec.size()) {
            size_t j = spec.find(',', i); if (j == std::string::npos) j = spec.size();
            std::string one = spec.substr(i, j - i); i = j + 1;
            unsigned at = 0, na = 0; char nm[64] = {0};
            if (std::sscanf(one.c_str(), "%x:%63[^:]:%u", &at, nm, &na) != 3) continue;
            std::string name = nm;
            d2rt::Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native.hook!log_" + name;
            s.fn = [at, name, na](d2rt::Cpu& c) -> uint32_t {
                uint32_t E = c.reg(d2rt::R_ESP);
                uint32_t a0 = c.read_u32(E + 4);
                if (a0 >= lo && a0 <= hi) {
                    char line[400]; int n = std::snprintf(line, sizeof line, "[call] %s(", name.c_str());
                    for (unsigned k = 0; k < na && n < 350; ++k) n += std::snprintf(line + n, sizeof line - n, "%s%x", k ? "," : "", c.read_u32(E + 4 + 4 * k));
                    log("%s) ecx=0x%08x ra=0x%08x thr=%u", line, c.reg(d2rt::R_ECX), c.read_u32(E), wx86_sched() && wx86_sched()->current() ? wx86_sched()->current()->id : 0u);
                    if (std::getenv("W3_LOG_BT")) {   // caller frames by the EBP chain
                        char bt[300]; int m = 0;
                        for (uint32_t bp = c.reg(d2rt::R_EBP), k = 0; bp && k < 10 && m < 280; ++k, bp = c.read_u32(bp))
                            m += std::snprintf(bt + m, sizeof bt - m, " %08x", c.read_u32(bp + 4));
                        log("[call]   bt:%s", bt);
                    }
                }
                // W3_LOG_RET=1: also log the return value (the return address
                // is swapped for a trap that logs EAX and goes back).
                static const bool ret_log = std::getenv("W3_LOG_RET") != nullptr;
                if (ret_log) {
                    static std::vector<std::pair<uint32_t, std::string>> pending;   // (caller ra, name)
                    static uint32_t ret_trap = 0;
                    if (!ret_trap) {
                        d2rt::Shim rs; rs.argc = 0; rs.stdcall_cleanup = false; rs.tag = "native.hook!log_ret";
                        rs.fn = [](d2rt::Cpu& cc) -> uint32_t {
                            if (pending.empty()) { log("[call] return with no pending call"); return cc.reg(d2rt::R_EAX); }
                            auto pr = pending.back(); pending.pop_back();
                            log("[call]   %s -> 0x%08x", pr.second.c_str(), cc.reg(d2rt::R_EAX));
                            // Reached by `ret`, not `call`: the bridge pops 4 on the way
                            // out, so step back 4 first, and send it to the real caller.
                            cc.set_reg(d2rt::R_ESP, cc.reg(d2rt::R_ESP) - 4);
                            g_br->redirect_next(pr.first);
                            return cc.reg(d2rt::R_EAX);
                        };
                        g_br->register_shim("native.hook", "log_ret", rs);
                        ret_trap = g_br->shim_trap("native.hook", "log_ret");
                    }
                    pending.emplace_back(c.read_u32(E), name);
                    c.write_u32(E, ret_trap);
                }
                uint32_t ebp = c.reg(d2rt::R_EBP);
                c.write_u32(E - 4, ebp); c.set_reg(d2rt::R_EBP, E - 4);
                c.set_reg(d2rt::R_ESP, E - 8);
                g_br->redirect_next(at + 3);
                return c.reg(d2rt::R_EAX);
            };
            br.register_shim("native.hook", "log_" + name, s);
            cpu->set_alternate(at, br.shim_trap("native.hook", "log_" + name));
        }
    }

    // DllMain(DLL_PROCESS_ATTACH) of every statically loaded DLL, deepest
    // dependency first, before the exe runs, as Windows does. Windows passes
    // a NON-NULL lpReserved for static loads (NULL only for LoadLibrary).
    {
        uint32_t reserved = wx86_scratch_alloc(0x400);
        for (d2rt::PeImage* pi : init_order) {
            if (!pi->entry_rva()) continue;
            uint32_t ret = 0; const char* fault = nullptr;
            bool ok = br.call_va(pi->entry_va(), { pi->load_base(), 1u, reserved }, ret, &fault);
            log("[not-bound] %s DllMain -> ok=%d ret=%u%s%s", pi->module_name().c_str(), ok, ret,
                fault ? " fault=" : "", fault ? fault : "");
            if (!ok || !ret) return 1;
        }
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
    uint32_t entry = br.module(kExeName)->entry_va();
    sched.set_main(entry, kMainStack + kMainStackSize - 0x1000u, 0);
    sc::inline_hot_imports();   // fs: now points at each guest thread's own block
    log("[not-bound] entering %s at 0x%08x", kExeName, entry);
    sched.run();
    log("[not-bound] scheduler stopped: %s", sched.stop_reason());
    return 0;
}
