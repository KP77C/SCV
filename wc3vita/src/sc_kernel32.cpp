// sc_kernel32.cpp — the StarCraft port's own KERNEL32 shims: files, module
// and resource lookup, process info, TLS, critical sections, time.
//
// Engine shims (winx86 win32_shims_*) are installed first; everything here
// is registered after them, so these win on any shared key.
#include "sc_host.h"
#include <new>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif

#include "runtime/guest_files.h"
#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/host_clock.h"
#include "runtime/win32_shims_kernel32.h"
#include "runtime/win32_shims_wait.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <list>
#include <ctime>
#include <dirent.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {

namespace {

enum : uint32_t {
    ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
    ERROR_INVALID_HANDLE = 6, ERROR_NO_MORE_FILES = 18, ERROR_ALREADY_EXISTS = 183,
    ERROR_FILE_EXISTS = 80, ERROR_RESOURCE_TYPE_NOT_FOUND = 1813,
    INVALID_HANDLE = 0xFFFFFFFFu, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80,
};

void K(d2rt::Bridge& br, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn,
       bool stdcall = true) {
    if (std::getenv("SC_TRACE_ALL")) {
        std::string t = name;
        fn = [t, f = std::move(fn)](Cpu& c) { uint32_t r = f(c); log("[not-bound] call %s -> 0x%x ra=0x%08x", t.c_str(), r, c.trap_retaddr()); return r; };
    }
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = stdcall;
    s.tag = std::string("KERNEL32.dll!") + name;
    br.register_shim("KERNEL32.dll", name, s);
}

std::string gstr(Cpu& c, uint32_t p) { return p ? wx86_gread_mb(c, p, -1) : std::string(); }

// ---------------------------------------------------------------- modules
// Pseudo handles for system DLLs (they have no image in guest memory).
std::map<uint32_t, std::string> g_sysmods;   // handle -> lower-case dll name
uint32_t g_next_sysmod = 0x7E000000u;

// ---- read cache + I/O accounting ---------------------------------------------
// The MPQs are read in small pieces with a seek before almost every read. On
// the Vita each fseek/fread pair is a syscall to the memory card, so reads of
// read-only files go through a 64 KiB block cache (LRU, SC_FILECACHE_MB,
// default 32; 0 disables). Writable opens of a path drop its blocks.
struct IoStats { uint64_t calls = 0, bytes = 0, host_us = 0, misses = 0, hits = 0, opens = 0, open_us = 0; };
IoStats g_io;
struct RoFile { uint32_t id; long size; };
std::map<uint32_t, RoFile> g_ro;                 // handle -> read-only file
std::map<std::string, uint32_t> g_path_id;
constexpr uint32_t kBlk = 64 * 1024;
struct CBlock { uint64_t key; uint32_t len; std::vector<uint8_t> d; };
std::list<CBlock> g_lru;
std::unordered_map<uint64_t, std::list<CBlock>::iterator> g_blocks;
#ifdef __vita__
constexpr int kCacheDefMB = 8;    // the host heap is shared with everything else on the Vita
#else
constexpr int kCacheDefMB = 32;
#endif
size_t cache_cap() {
    static size_t cap = [] { const char* e = std::getenv("SC_FILECACHE_MB"); 
                                    return (size_t)(e ? std::atoi(e) : kCacheDefMB) * 1024 * 1024 / kBlk; }();
    return cap;
}
uint32_t path_id(const std::string& p) {
    auto it = g_path_id.find(p); if (it != g_path_id.end()) return it->second;
    uint32_t id = (uint32_t)g_path_id.size() + 1; g_path_id[p] = id; return id;
}
void cache_drop(uint32_t id) {
    for (auto it = g_lru.begin(); it != g_lru.end();)
        if ((uint32_t)(it->key >> 32) == id) { g_blocks.erase(it->key); it = g_lru.erase(it); } else ++it;
}
const CBlock* cache_get(FILE* f, uint32_t id, uint32_t idx) {
    const uint64_t key = (uint64_t)id << 32 | idx;
    auto it = g_blocks.find(key);
    if (it != g_blocks.end()) { g_lru.splice(g_lru.begin(), g_lru, it->second); ++g_io.hits; return &*it->second; }
    ++g_io.misses;
    CBlock b; b.key = key;
    bool reuse = g_lru.size() >= cache_cap() && !g_lru.empty();
    if (!reuse) {
        // Out of host heap (bad_alloc killed the game on the Vita when the
        // cache could grow as large as the whole heap): recycle instead.
        try { b.d.resize(kBlk); } catch (const std::bad_alloc&) {
            static int said = 0; if (said++ < 4) log("[RAM-bound] [cause: host heap full] file cache stops growing at %u blocks", (unsigned)g_lru.size());
            if (g_lru.empty()) return nullptr;
            reuse = true;
        }
    }
    if (reuse) {   // take the oldest block's storage
        b.d.swap(g_lru.back().d); g_blocks.erase(g_lru.back().key); g_lru.pop_back();
    }
    std::fseek(f, (long)idx * kBlk, SEEK_SET);
    b.len = (uint32_t)std::fread(b.d.data(), 1, kBlk, f);
    g_lru.push_front(std::move(b)); g_blocks[key] = g_lru.begin();
    return &g_lru.front();
}
// Reads [pos, pos+n) of a read-only file into dst through the cache.
uint32_t cached_read(FILE* f, const RoFile& r, long pos, uint8_t* dst, uint32_t n) {
    if (pos >= r.size) return 0;
    if ((long)n > r.size - pos) n = (uint32_t)(r.size - pos);
    uint32_t done = 0;
    if (n >= 4 * kBlk || !cache_cap()) {                    // big reads: straight through
        std::fseek(f, pos, SEEK_SET); ++g_io.misses;
        return (uint32_t)std::fread(dst, 1, n, f);
    }
    while (done < n) {
        const long at = pos + done; const uint32_t idx = (uint32_t)(at / kBlk), off = (uint32_t)(at % kBlk);
        const CBlock* b = cache_get(f, r.id, idx);
        if (!b) { std::fseek(f, at, SEEK_SET); done += (uint32_t)std::fread(dst + done, 1, n - done, f); break; }
        if (off >= b->len) break;
        const uint32_t k = std::min(n - done, b->len - off);
        std::memcpy(dst + done, b->d.data() + off, k); done += k;
    }
    return done;
}
}  // namespace
// One line per 10 s window for the presenter's log.
void io_window_line(char* out, unsigned n, double span_s) {
    static uint64_t bf = 0, bs = 0;
    std::snprintf(out, n, "io: %.0f reads/s, %.0f KiB/s, %.1f%% of wall time in file calls, cache %llu hits / %llu misses, %llu opens (%.1f ms); "
                  "SBltROP3 %.0f/s native, %.0f/s storm code",
                  g_io.calls / span_s, g_io.bytes / 1024.0 / span_s, g_io.host_us / 1e4 / span_s,
                  (unsigned long long)g_io.hits, (unsigned long long)g_io.misses, (unsigned long long)g_io.opens, g_io.open_us / 1000.0,
                  (g_blt_fast - bf) / span_s, (g_blt_slow - bs) / span_s);
    bf = g_blt_fast; bs = g_blt_slow;
    g_io = IoStats();
}
namespace {
// timegm() without relying on it (newlib on the Vita has none): civil date -> days.
int64_t utc_seconds(const struct tm& t) {
    int y = t.tm_year + 1900, m = t.tm_mon + 1; y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400; const int yoe = (int)(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}
std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
std::string basename_of(const std::string& p) {
    size_t k = p.find_last_of("\\/"); return k == std::string::npos ? p : p.substr(k + 1);
}
std::string with_dll_ext(std::string n) {
    if (n.find('.') == std::string::npos) n += ".dll";
    return n;
}

// Module key in the bridge for a guest name, if loaded ("" otherwise).
std::map<std::string, std::string> g_modkey;   // lower basename -> bridge key
std::map<uint32_t, std::string>    g_modbase;  // base -> bridge key
uint32_t g_exe_base = 0;

bool is_system_dll(const std::string& lb) {
    static const char* sys[] = { "kernel32.dll","user32.dll","gdi32.dll","advapi32.dll","shell32.dll",
        "version.dll","imm32.dll","comdlg32.dll","ddraw.dll","dsound.dll","winmm.dll","wsock32.dll",
        "ws2_32.dll","ntdll.dll","ole32.dll","tapi32.dll","dbghelp.dll","mscoree.dll","wininet.dll",
        "msvcrt.dll","oleaut32.dll","comctl32.dll","rpcrt4.dll","psapi.dll","riched20.dll","opengl32.dll" };
    for (auto* s : sys) if (lb == s) return true;
    return false;
}

uint32_t sysmod_handle(const std::string& lb) {
    for (auto& kv : g_sysmods) if (kv.second == lb) return kv.first;
    uint32_t h = g_next_sysmod; g_next_sysmod += 0x10000;
    g_sysmods[h] = lb; return h;
}

// Runs a freshly loaded DLL's DllMain(DLL_PROCESS_ATTACH).
bool run_dllmain(d2rt::PeImage* pi) {
    if (!pi->entry_rva()) return true;
    // Bridge::call_va builds its frame at the top of the MAIN stack, which
    // the calling guest thread is still using here (we are inside its
    // LoadLibraryA). Run the nested call on a private loader stack instead,
    // and restore the caller's registers afterwards.
    static uint32_t s_stack = wx86_scratch_alloc(0x40000);
    d2rt::X86Context saved; g_cpu->save_context(saved);
    uint32_t esp = s_stack + 0x40000 - 0x100;
    for (uint32_t a : { 0u, 1u, pi->load_base() }) { esp -= 4; g_cpu->write_u32(esp, a); }   // lpReserved, reason, hinst
    esp -= 4; g_cpu->write_u32(esp, g_br->sentinel());
    g_cpu->set_reg(d2rt::R_ESP, esp); g_cpu->set_reg(d2rt::R_EBP, 0);
    const char* fault = nullptr;
    bool ok = run_nested(*g_cpu, pi->entry_va(), &fault);
    uint32_t ret = g_cpu->reg(d2rt::R_EAX);
    g_cpu->load_context(saved);
    log("[not-bound] DllMain %s -> %u%s%s", pi->module_name().c_str(), ret,
        fault ? " fault=" : "", fault ? fault : "");
    return ok && ret;
}

// Names of the DLLs a PE file imports from (raw file bytes).
std::vector<std::string> pe_import_dlls(const std::vector<uint8_t>& f) {
    std::vector<std::string> out;
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= f.size() ? (uint32_t)(f[o] | f[o + 1] << 8) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= f.size() ? (u16(o) | u16(o + 2) << 16) : 0; };
    if (f.size() < 0x40) return out;
    const uint32_t pe = u32(0x3c), ns = u16(pe + 6), so = u16(pe + 20);
    auto r2f = [&](uint32_t rva) -> size_t {
        for (uint32_t i = 0; i < ns; ++i) {
            size_t o = pe + 24 + so + i * 40;
            uint32_t vs = u32(o + 8), va = u32(o + 12), rs = u32(o + 16), ra = u32(o + 20);
            if (rva >= va && rva < va + std::max(vs, rs)) return rva - va + ra;
        }
        return 0;
    };
    size_t d = r2f(u32(pe + 24 + 104));
    for (; d && d + 20 <= f.size() && u32(d + 12); d += 20) {
        size_t n = r2f(u32(d + 12)); std::string nm;
        while (n && n < f.size() && f[n]) nm += (char)f[n++];
        out.push_back(nm);
    }
    return out;
}

// Deus Ex's Fire.dll keeps its water/fire renderers in .data and rewrites
// their displacements as it runs (self-modifying code). The dynarec has no
// write-fault SMC detection, so code in writable sections must be marked
// volatile (re-validated on every entry). Data that never runs costs nothing.
void mark_writable_sections_impl(Cpu& c, uint32_t base, const std::vector<uint8_t>& f, const std::string& name) {
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= f.size() ? (uint32_t)(f[o] | f[o + 1] << 8) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= f.size() ? (u16(o) | u16(o + 2) << 16) : 0; };
    if (f.size() < 0x40) return;
    const uint32_t pe = u32(0x3c), ns = u16(pe + 6), so = u16(pe + 20);
    for (uint32_t i = 0; i < ns; ++i) {
        size_t o = pe + 24 + so + i * 40;
        uint32_t vs = u32(o + 8), va = u32(o + 12), ch = u32(o + 36);
        if ((ch & 0x80000000u) && vs) {       // IMAGE_SCN_MEM_WRITE
            c.mark_volatile_code(base + va, vs);
            if (std::getenv("DX_TRACE_SMC")) log("[not-bound] %s: writable section 0x%08x+0x%x marked volatile", name.c_str(), base + va, vs);
        }
    }
}

uint32_t load_library(Cpu& c, const std::string& name) {
    std::string lb = lower(with_dll_ext(basename_of(name)));
    auto it = g_modkey.find(lb);
    if (it != g_modkey.end()) return g_br->module_base(it->second);
    std::string host = resolve_path(name);
    std::vector<uint8_t> bytes = read_file(host);
    if (bytes.empty()) {
        if (is_system_dll(lb)) return sysmod_handle(lb);
        log("[not-bound] [cause: file not found] LoadLibraryA(%s)", name.c_str());
        wx86_set_lasterr(c, 126);   // ERROR_MOD_NOT_FOUND
        return 0;
    }
    // The Windows loader first loads (and initializes) the DLLs this one
    // imports from: Deus Ex's DeusEx.dll links against Extension.dll and
    // ConSys.dll, which nothing has loaded yet. Without this their imports,
    // data ones included (PrivateStaticClass), would bind to stub traps.
    for (const std::string& dep : pe_import_dlls(bytes)) {
        std::string dl = lower(dep);
        if (g_modkey.count(dl) || is_system_dll(dl)) continue;
        std::string dir = name.substr(0, name.find_last_of("\\/") + 1);
        if (!read_file(resolve_path(dir + dep)).empty()) load_library(c, dir + dep);
    }
    std::string err;
    d2rt::PeImage* pi = g_br->load_library_runtime(lb, bytes, err);
    if (!pi) { log("[not-bound] [cause: load error] LoadLibraryA(%s): %s", name.c_str(), err.c_str()); return 0; }
    g_modkey[lb] = lb; g_modbase[pi->load_base()] = lb;
    mark_writable_sections_impl(c, pi->load_base(), bytes, lb);
    log("[not-bound] LoadLibraryA(%s) -> 0x%08x", name.c_str(), pi->load_base());
    if (lb == "smackw32.dll" && std::getenv("SC_TRACE_SMK")) {
        // Debug: observe Smacker's sound-service routine (entry = base+0x8e20,
        // first instruction `call base+0x8e30`), logging the head of its
        // movie list and that movie's sound-buffer field, then replay the call.
        uint32_t base = pi->load_base(), entry = base + 0x8e20;
        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native.hook!smk_service";
        s.fn = [base](Cpu& c) -> uint32_t {
            const uint32_t E = c.reg(d2rt::R_ESP);
            uint32_t head = c.read_u32(base + 0x148dc);
            static uint32_t last_head = 1, last_buf = 1;
            uint32_t buf = head ? c.read_u32(head + 0x60) : 0;
            if (head != last_head || buf != last_buf) {
                auto* s = wx86_sched(); auto* t = s ? s->current() : nullptr;
                log("[not-bound] smk service: thread %u ra=0x%08x head=0x%08x head->buf=0x%08x next=0x%08x",
                    t ? t->id : 0, c.read_u32(E), head, buf, head ? c.read_u32(head + 0x5c) : 0);
                last_head = head; last_buf = buf;
            }
            c.write_u32(E - 4, base + 0x8e25);   // replay `call base+0x8e30`
            c.set_reg(d2rt::R_ESP, E - 8);
            g_br->redirect_next(base + 0x8e30);
            return c.reg(d2rt::R_EAX);
        };
        g_br->register_shim("native.hook", "smk_service", s);
        g_cpu->set_alternate(entry, g_br->shim_trap("native.hook", "smk_service"));
    }
    if (!run_dllmain(pi)) return 0;
    // DX_SNAP_LOADLIB=<dll>,<dir>: after that DLL is loaded, write the guest
    // state as it will be when LoadLibraryA returns (EIP = return address,
    // stdcall arg popped, EAX = module base) for a reference replay
    // (tools/diff/dxreplay.py), then stop.
    if (const char* sn = std::getenv("DX_SNAP_LOADLIB")) {
        std::string spec = sn; size_t k = spec.find(',');
        if (k != std::string::npos && lower(spec.substr(0, k)) == lb) {
            std::string dir = spec.substr(k + 1); mkdir(dir.c_str(), 0777);
            const uint32_t E = c.reg(d2rt::R_ESP);
            FILE* r = std::fopen((dir + "/regs.txt").c_str(), "w");
            uint32_t v[9] = { pi->load_base(), c.reg(d2rt::R_ECX), c.reg(d2rt::R_EDX), c.reg(d2rt::R_EBX), E + 8,
                              c.reg(d2rt::R_EBP), c.reg(d2rt::R_ESI), c.reg(d2rt::R_EDI), c.read_u32(E) };
            for (uint32_t x : v) std::fprintf(r, "%08x\n", x);
            std::fclose(r);
            dump_arena(c, dir);
            log("[not-bound] snapshot after LoadLibraryA(%s) written to %s", lb.c_str(), dir.c_str());
            std::fflush(nullptr); _exit(4);
        }
    }
    return pi->load_base();
}

// ---------------------------------------------------------------- resources
// Win32 resource lookup over a guest image (FindResourceA/LoadResource/...).
struct RsrcDir { uint16_t named, ids; };

uint32_t rsrc_root(Cpu& c, uint32_t base) {
    uint32_t pe = c.read_u32(base + 0x3c);
    uint32_t rva = c.read_u32(base + pe + 24 + 96 + 2 * 8);   // data directory #2
    return rva ? base + rva : 0;
}

// Finds the child of directory `dir` matching an id (<0x10000) or a name;
// returns the entry's OffsetToData word, or 0xFFFFFFFF.
uint32_t rsrc_find(Cpu& c, uint32_t root, uint32_t dir, uint32_t key) {
    uint16_t named = 0, ids = 0;
    c.read(dir + 12, &named, 2); c.read(dir + 14, &ids, 2);
    std::string want;
    bool byname = key >= 0x10000;
    if (byname) {
        want = gstr(c, key);
        if (!want.empty() && want[0] == '#') { key = (uint32_t)strtoul(want.c_str() + 1, nullptr, 10); byname = false; }
        else for (auto& ch : want) ch = (char)toupper((unsigned char)ch);
    }
    for (uint32_t i = 0; i < (uint32_t)named + ids; ++i) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t nm = c.read_u32(e), off = c.read_u32(e + 4);
        if (byname && (nm & 0x80000000u)) {
            uint32_t s = root + (nm & 0x7FFFFFFFu);
            uint16_t len = 0; c.read(s, &len, 2);
            std::string n;
            for (uint16_t k = 0; k < len; ++k) { uint16_t w = 0; c.read(s + 2 + 2 * k, &w, 2); n += (char)toupper(w & 0xFF); }
            if (n == want) return off;
        } else if (!byname && !(nm & 0x80000000u) && nm == key) {
            return off;
        }
    }
    return 0xFFFFFFFFu;
}

uint32_t module_base_of(uint32_t h) { return h ? h : g_exe_base; }

// ---------------------------------------------------------------- files
struct FindState { std::string dir; std::string pat; DIR* d = nullptr; };
std::map<uint32_t, FindState> g_finds;

bool wild_match(const char* p, const char* s) {   // case-insensitive * and ?
    if (!*p) return !*s;
    if (*p == '*') return wild_match(p + 1, s) || (*s && wild_match(p, s + 1));
    if (*s && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s))) return wild_match(p + 1, s + 1);
    return false;
}

void unix_to_filetime(time_t t, uint32_t& lo, uint32_t& hi) {
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    lo = (uint32_t)ft; hi = (uint32_t)(ft >> 32);
}

// WIN32_FIND_DATAA, 320 bytes.
void fill_find_data(Cpu& c, uint32_t out, const std::string& host, const std::string& name) {
    struct stat st{}; stat(host.c_str(), &st);
    uint8_t buf[320] = {};
    uint32_t attr = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    std::memcpy(buf, &attr, 4);
    uint32_t lo, hi; unix_to_filetime(st.st_mtime, lo, hi);
    for (int k = 0; k < 3; ++k) { std::memcpy(buf + 4 + 8 * k, &lo, 4); std::memcpy(buf + 8 + 8 * k, &hi, 4); }
    uint32_t sz = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
    std::memcpy(buf + 32, &sz, 4);
    std::strncpy((char*)buf + 44, name.c_str(), 259);
    c.write(out, buf, sizeof buf);
}

bool find_next(Cpu& c, FindState& fs, uint32_t out) {
    while (dirent* e = readdir(fs.d)) {
        if (wild_match(fs.pat.c_str(), e->d_name)) {
            fill_find_data(c, out, fs.dir + "/" + e->d_name, e->d_name);
            return true;
        }
    }
    return false;
}

void sys_time(bool local, uint8_t out[16]) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm{}; time_t t = ts.tv_sec;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    uint16_t v[8] = { (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday,
                      (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min,
                      (uint16_t)tm.tm_sec, (uint16_t)(ts.tv_nsec / 1000000) };
    std::memcpy(out, v, 16);
}

}  // namespace

// Win32 FindResourceA over a guest image: returns the guest address of the
// IMAGE_RESOURCE_DATA_ENTRY, or 0. Shared with LoadAcceleratorsA (sc_user32).
uint32_t find_resource(Cpu& c, uint32_t hmod, uint32_t name, uint32_t type) {
    uint32_t base = module_base_of(hmod);
    uint32_t root = rsrc_root(c, base);
    if (!root) { wx86_set_lasterr(c, ERROR_RESOURCE_TYPE_NOT_FOUND); return 0; }
    uint32_t o = rsrc_find(c, root, root, type);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    o = rsrc_find(c, root, root + (o & 0x7FFFFFFFu), name);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    uint32_t lang = root + (o & 0x7FFFFFFFu);
    uint16_t n1 = 0, n2 = 0; c.read(lang + 12, &n1, 2); c.read(lang + 14, &n2, 2);
    if (!(n1 + n2)) return 0;
    uint32_t leaf = c.read_u32(lang + 16 + 4);
    return (leaf & 0x80000000u) ? 0 : root + leaf;
}


// ReadFile into a buffer the host can't map in one piece (it runs past the
// end of the guest arena, or n is far larger than the file). Windows only
// writes what it reads, so read in 64 KiB pieces and stop at end of file
// instead of allocating n bytes up front: a huge n threw bad_alloc and
// killed the game on the Vita (K, build 6, first command-card click).
template <class F>
static uint32_t read_chunked(Cpu& c, uint32_t buf, uint32_t n, F&& rd) {
    static uint8_t tmp[65536];
    uint32_t done = 0;
    static int said = 0;
    while (done < n) {
        const uint32_t k = n - done < sizeof tmp ? n - done : (uint32_t)sizeof tmp;
        const uint32_t g = rd(tmp, k, done);
        if (g && !c.write(buf + done, tmp, g)) break;
        done += g;
        if (g < k) break;
    }
    if (said < 8) { ++said; log("[not-bound] ReadFile into unmappable buffer 0x%08x (n=%u): read %u in pieces", buf, n, done); }
    return done;
}

// Return addresses into loaded modules found on the guest stack, as
// " module+0xoffset ..." (for error logs: who called).
std::string guest_stack(Cpu& c) {
    const uint32_t E = c.reg(d2rt::R_ESP);
    std::string chain;
    for (uint32_t k = 0; k < 96 && chain.size() < 400; ++k) {
        uint32_t v = c.read_u32(E + 4 * k);
        auto it = g_modbase.upper_bound(v);
        if (it == g_modbase.begin()) continue;
        --it;
        d2rt::PeImage* m = g_br->module(it->second);
        if (m && v < it->first + m->image_size()) {
            char b[64]; std::snprintf(b, sizeof b, " %s+0x%x", it->second.c_str(), v - it->first); chain += b;
        }
    }
    return chain;
}

void install_kernel32(d2rt::Bridge& br) {
    // ---- process / module -------------------------------------------------
    // WarCraft III 1.27a (MSVCR120) needs Windows XP or later: report
    // XP SP3, 5.1.2600, VER_PLATFORM_WIN32_NT.
    K(br, "GetVersion", 0, [](Cpu&) { return 0x0A280105u; });
    auto verex = [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        c.write_u32(p + 4, 5); c.write_u32(p + 8, 1); c.write_u32(p + 12, 2600); c.write_u32(p + 16, 2);
        return 1u;
    };
    K(br, "GetVersionExA", 1, verex);
    K(br, "GetVersionExW", 1, verex);
    K(br, "GetCommandLineW", 0, [](Cpu& c) {
        static uint32_t p = 0;
        if (!p) {
            static const char s[] = "\"C:\\Warcraft III\\War3.exe\" -opengl -window";
            p = wx86_scratch_alloc(2 * sizeof s);
            for (uint32_t i = 0; i < sizeof s; ++i) wx86_gwrite_wc(c, p + 2 * i, (uint8_t)s[i]);
        }
        return p;
    });
    K(br, "GetCommandLineA", 0, [](Cpu& c) {
        static uint32_t p = wx86_scratch_put_cstr(c, "\"C:\\Warcraft III\\War3.exe\" -opengl -window");
        return p;
    });
    K(br, "GetModuleFileNameA", 3, [](Cpu& c) {
        uint32_t h = c.arg(0), out = c.arg(1), cap = c.arg(2);
        std::string name = kExeName;
        if (h && h != g_exe_base) {
            auto it = g_modbase.find(h);
            if (it != g_modbase.end()) name = it->second;
            else if (g_sysmods.count(h)) return put_str(c, out, "C:\\WINDOWS\\system32\\" + g_sysmods[h], cap);
        }
        return put_str(c, out, std::string(kGuestCwd) + "\\" + name, cap);
    });
    static auto module_handle = [](Cpu& c, const std::string& name) -> uint32_t {
        std::string lb = lower(with_dll_ext(basename_of(name)));
        auto it = g_modkey.find(lb);
        if (it != g_modkey.end()) return g_br->module_base(it->second);
        if (is_system_dll(lb)) return sysmod_handle(lb);
        wx86_set_lasterr(c, 126);
        return 0;
    };
    K(br, "GetModuleHandleA", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        if (!p) return g_exe_base;
        return module_handle(c, gstr(c, p));
    });
    K(br, "GetModuleHandleW", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        if (!p) return g_exe_base;
        std::vector<uint16_t> w = wx86_gread_wc(c, p, -1);
        std::string a; for (uint16_t ch : w) a += (char)ch;
        return module_handle(c, a);
    });
    // SC_MOVIES: unset or anything but "0" plays the cinematics.
    static auto movies_off = []() { const char* m = std::getenv("SC_MOVIES"); return m && m[0] == '0'; };
    K(br, "LoadLibraryA", 1, [](Cpu& c) { return load_library(c, gstr(c, c.arg(0))); });
    K(br, "GetProcAddress", 2, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0), p = c.arg(1);
        std::string sym = p < 0x10000 ? "#" + std::to_string(p) : gstr(c, p);
        auto sm = g_sysmods.find(h);
        if (sm != g_sysmods.end()) {
            uint32_t va = g_br->shim_trap(sm->second, sym);
            if (!va) log("[not-bound] [cause: missing shim] GetProcAddress(%s, %s)", sm->second.c_str(), sym.c_str());
            return va;
        }
        auto mk = g_modbase.find(h);
        std::string key = mk != g_modbase.end() ? mk->second : "";
        d2rt::PeImage* pi = key.empty() ? nullptr : g_br->module(key);
        if (!pi) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
        // Movies play by default since build 16 (K asked for them, and the
        // game's own Smacker player runs once audio has its own clock).
        // SC_MOVIES=0 brings back the old auto-skip: one Escape press as
        // each movie starts, the way a player skips it.
        if (key == "smackw32.dll" && sym == "_SmackOpen@12" && movies_off()) {
            uint32_t real = pi->export_va(sym);
            Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "SMACKW32.dll!_SmackOpen@12 (auto-skip)";
            s.fn = [real](Cpu& c) -> uint32_t {
                // Only full-screen cinematics, played by the exe's movie routine
                // (its SVidPlayBegin call returns to 0x004d44ca). The main menu's
                // animated buttons are Smacker files too and must keep playing.
                bool cinematic = false;
                for (uint32_t bp = c.reg(d2rt::R_EBP), k = 0; bp && k < 8; ++k, bp = c.read_u32(bp))
                    if (c.read_u32(bp + 4) == 0x004d44cau) { cinematic = true; break; }
                if (cinematic) {
                    log("[not-bound] cinematic opened, skipping it with Escape (cinematics deferred)");
                    post_key(0x1B);
                }
                c.set_reg(d2rt::R_ESP, c.reg(d2rt::R_ESP) - 4);   // epilogue pops 4: frame intact
                g_br->redirect_next(real);
                return c.reg(d2rt::R_EAX);
            };
            g_br->register_shim("native.hook", "SmackOpen_skip", s);
            return g_br->shim_trap("native.hook", "SmackOpen_skip");
        }
        uint32_t va = p < 0x10000 ? pi->export_va_ordinal(p) : pi->export_va(sym);
        if (!va) wx86_set_lasterr(c, 127);   // ERROR_PROC_NOT_FOUND
        return va;
    });
    K(br, "GetCurrentProcess", 0, [](Cpu&) { return 0xFFFFFFFFu; });
    K(br, "GetCurrentThread", 0, [](Cpu&) { return 0xFFFFFFFEu; });
    K(br, "GetCurrentThreadId", 0, [](Cpu&) {
        auto* s = wx86_sched(); auto* t = s ? s->current() : nullptr;
        return wx86_win_tid(t ? t->id : 1u);
    });
    K(br, "ExitProcess", 1, [](Cpu& c) -> uint32_t {
        log("[not-bound] ExitProcess(%u)", c.arg(0));
        std::fflush(nullptr);
#ifdef __vita__
        present_shutdown(); sceKernelExitProcess((int)c.arg(0));
#endif
        _exit((int)c.arg(0));
    });
    K(br, "TerminateProcess", 2, [](Cpu& c) -> uint32_t {
        log("[not-bound] TerminateProcess(%u)", c.arg(1));
        std::fflush(nullptr);
#ifdef __vita__
        present_shutdown(); sceKernelExitProcess((int)c.arg(1));
#endif
        _exit((int)c.arg(1));
    });
    K(br, "GetComputerNameA", 2, [](Cpu& c) {
        uint32_t out = c.arg(0), szp = c.arg(1);
        uint32_t n = put_str(c, out, "VITA", c.read_u32(szp));
        c.write_u32(szp, n); return 1u;
    });
    K(br, "SetUnhandledExceptionFilter", 1, [](Cpu&) { return 0u; });
    K(br, "OutputDebugStringA", 1, [](Cpu& c) { log("[not-bound] guest debug: %s", gstr(c, c.arg(0)).c_str()); return 0u; });
    K(br, "RaiseException", 4, [](Cpu& c) -> uint32_t {
        // Floating-point status exceptions (0xC000008D..0xC0000093) raised by
        // msvcrt's math routines (pow in Render.dll's tables): treated as
        // masked, which is what the game's FPU setup asks for on Windows.
        // msvcrt then keeps its default result. Counted, not logged: the
        // software renderer raises them thousands of times per frame.
        if (c.arg(0) >= 0xC000008Du && c.arg(0) <= 0xC0000093u) {
            static uint64_t n = 0;
            if ((n++ & (n - 1)) == 0) log("[not-bound] FP exception 0x%08x treated as masked (%llu so far)", c.arg(0), (unsigned long long)n);
            return 0;
        }
        log("[not-bound] [cause: guest exception] RaiseException(0x%08x) at ra=0x%08x — not dispatched yet",
            c.arg(0), c.trap_retaddr());
        log("[not-bound]   stack:%s", guest_stack(c).c_str());   // where from
        // Unreal's appErrorf keeps the message (and the guard call history)
        // in Core's GErrorHist (UTF-16) before it throws: log it.
        if (d2rt::PeImage* core = g_br->module("Core.dll"))
            if (uint32_t h = core->export_va("?GErrorHist@@3PAGA")) {
                std::vector<uint16_t> w = wx86_gread_wc(c, h, 2048);
                std::string a; for (uint16_t ch : w) { if (!ch) break; a += ch < 0x80 ? (char)ch : '?'; }
                if (!a.empty()) log("[not-bound] [cause: game error] %s", a.c_str());
            }
        return 0;
    });

    // ---- TLS (TEB.TlsSlots at +0xE10, 64 slots, as on Windows) ------------
    K(br, "TlsAlloc", 0, [](Cpu&) {
        static uint64_t used = 0;
        for (uint32_t i = 0; i < 64; ++i) if (!(used & (1ull << i))) { used |= 1ull << i; return i; }
        return 0xFFFFFFFFu;
    });
    K(br, "TlsGetValue", 1, [](Cpu& c) {
        uint32_t i = c.arg(0); if (i >= 64) return 0u;
        wx86_set_lasterr(c, 0);
        return c.read_u32(wx86_cur_tib() + 0xE10 + 4 * i);
    });
    K(br, "TlsSetValue", 2, [](Cpu& c) {
        uint32_t i = c.arg(0); if (i >= 64) return 0u;
        c.write_u32(wx86_cur_tib() + 0xE10 + 4 * i, c.arg(1)); return 1u;
    });

    // ---- critical sections -------------------------------------------------
    K(br, "InitializeCriticalSection", 1, [](Cpu& c) {
        uint8_t z[24] = {}; c.write(c.arg(0), z, sizeof z);
        wx86_crit_for(c.arg(0)); return 0u;
    });
    // MSVCR120 and Storm use the spin-count and Ex forms (spin count ignored).
    K(br, "InitializeCriticalSectionAndSpinCount", 2, [](Cpu& c) {
        uint8_t z[24] = {}; c.write(c.arg(0), z, sizeof z);
        wx86_crit_for(c.arg(0)); return 1u;
    });
    K(br, "InitializeCriticalSectionEx", 3, [](Cpu& c) {
        uint8_t z[24] = {}; c.write(c.arg(0), z, sizeof z);
        wx86_crit_for(c.arg(0)); return 1u;
    });
    K(br, "DeleteCriticalSection", 1, [](Cpu& c) { wx86_crit_forget(c.arg(0)); return 0u; });
    K(br, "EnterCriticalSection", 1, [](Cpu& c) {
        auto* cs = wx86_crit_for(c.arg(0));
        auto* s = wx86_sched(); auto* t = s ? s->current() : nullptr;
        if (!cs->try_acquire(t) && s) s->wait_noresult(cs, 0xFFFFFFFFu);
        return 0u;
    });
    K(br, "LeaveCriticalSection", 1, [](Cpu& c) { wx86_crit_leave(c.arg(0), wx86_sched()); return 0u; });

    // ---- time ----------------------------------------------------------------
    K(br, "GetTickCount", 0, [](Cpu&) { return (uint32_t)wx86_now_ms(); });
    K(br, "QueryPerformanceCounter", 1, [](Cpu& c) {
        uint64_t f = wx86_perf_frequency();
        uint64_t us = wx86_now_us();
        uint64_t v = f ? (us / 1000000ull) * f + (us % 1000000ull) * f / 1000000ull : us;
        c.write(c.arg(0), &v, 8); return 1u;
    });
    K(br, "GetSystemTimeAsFileTime", 1, [](Cpu& c) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ft = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + ts.tv_nsec / 100;
        c.write(c.arg(0), &ft, 8); return 0u;
    });
    K(br, "GetLocalTime", 1, [](Cpu& c) { uint8_t b[16]; sys_time(true, b); c.write(c.arg(0), b, 16); return 0u; });
    K(br, "GetSystemTime", 1, [](Cpu& c) { uint8_t b[16]; sys_time(false, b); c.write(c.arg(0), b, 16); return 0u; });
    K(br, "GetTimeZoneInformation", 1, [](Cpu& c) {
        uint8_t z[172] = {}; c.write(c.arg(0), z, sizeof z); return 0u;   // TIME_ZONE_ID_UNKNOWN, UTC
    });
    K(br, "FileTimeToLocalFileTime", 2, [](Cpu& c) {
        uint64_t v = 0; c.read(c.arg(0), &v, 8); c.write(c.arg(1), &v, 8); return 1u;   // guest runs in UTC
    });
    K(br, "FileTimeToSystemTime", 2, [](Cpu& c) {
        uint64_t ft = 0; c.read(c.arg(0), &ft, 8);
        time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
        struct tm tm{}; gmtime_r(&t, &tm);
        uint16_t v[8] = { (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday,
                          (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min,
                          (uint16_t)tm.tm_sec, (uint16_t)((ft / 10000) % 1000) };
        c.write(c.arg(1), v, 16); return 1u;
    });
    K(br, "SystemTimeToFileTime", 2, [](Cpu& c) {
        uint16_t v[8] = {}; c.read(c.arg(0), v, 16);
        struct tm tm{}; tm.tm_year = v[0] - 1900; tm.tm_mon = v[1] - 1; tm.tm_mday = v[3];
        tm.tm_hour = v[4]; tm.tm_min = v[5]; tm.tm_sec = v[6];
        uint64_t ft = ((uint64_t)utc_seconds(tm) + 11644473600ull) * 10000000ull + v[7] * 10000ull;
        c.write(c.arg(1), &ft, 8); return 1u;
    });
    K(br, "CompareFileTime", 2, [](Cpu& c) {
        uint64_t a = 0, b = 0; c.read(c.arg(0), &a, 8); c.read(c.arg(1), &b, 8);
        return a < b ? 0xFFFFFFFFu : (a > b ? 1u : 0u);
    });
    K(br, "Sleep", 1, [](Cpu& c) { wx86_wait_sleep(c.arg(0)); return 0u; });

    // ---- small helpers ----------------------------------------------------
    K(br, "MulDiv", 3, [](Cpu& c) {
        int32_t a = (int32_t)c.arg(0), b = (int32_t)c.arg(1), d = (int32_t)c.arg(2);
        if (!d) return 0xFFFFFFFFu;
        int64_t r = (int64_t)a * b; r = r >= 0 ? (r + (d > 0 ? d : -d) / 2) / d : (r - (d > 0 ? d : -d) / 2) / d;
        return (uint32_t)(int32_t)r;
    });
    K(br, "lstrcpynA", 3, [](Cpu& c) {
        uint32_t dst = c.arg(0), n = c.arg(2);
        if (n) put_str(c, dst, gstr(c, c.arg(1)), n);
        return dst;
    });
    K(br, "IsDBCSLeadByte", 1, [](Cpu&) { return 0u; });   // code page 1252: no lead bytes
    K(br, "FormatMessageA", 7, [](Cpu& c) {
        uint32_t flags = c.arg(0), id = c.arg(2), buf = c.arg(4), cap = c.arg(5);
        std::string m = "Error " + std::to_string(id) + "\r\n";
        log("[not-bound] [cause: guest error report] FormatMessageA(flags=0x%x, id=0x%08x) ra=0x%08x", flags, id, c.trap_retaddr());
        if (id != 2 && id != 3) log("[not-bound]   stack:%s", guest_stack(c).c_str());   // not "file/path not found": who reported it
        if (flags & 0x100) {   // FORMAT_MESSAGE_ALLOCATE_BUFFER
            uint32_t p = wx86_scratch_put_cstr(c, m.c_str());
            c.write_u32(buf, p); return (uint32_t)m.size();
        }
        return put_str(c, buf, m, cap);
    });
    K(br, "DisableThreadLibraryCalls", 1, [](Cpu&) { return 1u; });
    K(br, "VirtualLock", 2, [](Cpu&) { return 1u; });
    K(br, "VirtualUnlock", 2, [](Cpu&) { return 1u; });

    // ---- resources -------------------------------------------------------------
    K(br, "FindResourceA", 3, [](Cpu& c) -> uint32_t { return find_resource(c, c.arg(0), c.arg(1), c.arg(2)); });
    K(br, "LoadResource", 2, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(1); return h ? module_base_of(c.arg(0)) + c.read_u32(h) : 0;
    });
    K(br, "LockResource", 1, [](Cpu& c) { return c.arg(0); });
    K(br, "FreeResource", 1, [](Cpu&) { return 0u; });
    K(br, "SizeofResource", 2, [](Cpu& c) -> uint32_t { uint32_t h = c.arg(1); return h ? c.read_u32(h + 4) : 0; });

    // ---- files -------------------------------------------------------------
    K(br, "CreateFileA", 7, [](Cpu& c) -> uint32_t {
        std::string guest = gstr(c, c.arg(0));
        uint32_t access = c.arg(1), disp = c.arg(4);
        std::string host = resolve_path(guest);
        if (std::getenv("SC_TRACE_FILES")) log("[not-bound] CreateFileA(%s) host=%s disp=%u", guest.c_str(), host.c_str(), disp);
        struct stat st{}; bool exists = stat(host.c_str(), &st) == 0;
        bool wr = access & 0x40000000u;   // GENERIC_WRITE
        const char* mode = nullptr;
        switch (disp) {
            case 1: if (exists) { wx86_set_lasterr(c, ERROR_FILE_EXISTS); return INVALID_HANDLE; } mode = "w+b"; break; // CREATE_NEW
            case 2: mode = "w+b"; break;                                                          // CREATE_ALWAYS
            case 3: if (!exists) { wx86_set_lasterr(c, ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; }  // OPEN_EXISTING
                    mode = wr ? "r+b" : "rb"; break;
            case 4: mode = exists ? (wr ? "r+b" : "rb") : "w+b"; break;                           // OPEN_ALWAYS
            case 5: if (!exists) { wx86_set_lasterr(c, ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; } mode = "w+b"; break; // TRUNCATE_EXISTING
            default: wx86_set_lasterr(c, 87); return INVALID_HANDLE;
        }
        if (exists && S_ISDIR(st.st_mode)) { wx86_set_lasterr(c, ERROR_ACCESS_DENIED); return INVALID_HANDLE; }
        const uint64_t t0 = wx86_now_us();
        FILE* f = std::fopen(host.c_str(), mode);
        g_io.opens++; g_io.open_us += wx86_now_us() - t0;
        if (!f) { wx86_set_lasterr(c, errno == ENOENT ? ERROR_PATH_NOT_FOUND : ERROR_ACCESS_DENIED); return INVALID_HANDLE; }
        uint32_t h = wx86_handle_next_id();
        const uint32_t pid = path_id(host);
        if (!std::strcmp(mode, "rb")) {
            std::setvbuf(f, nullptr, _IONBF, 0);            // the block cache does the buffering
            g_ro[h] = RoFile{ pid, exists ? (long)st.st_size : 0 };
        } else cache_drop(pid);
        wx86_files()[h] = f; wx86_file_paths()[h] = host; wx86_file_pos()[h] = WxFPos{};
        wx86_set_lasterr(c, exists && (disp == 2 || disp == 4) ? ERROR_ALREADY_EXISTS : 0);
        if (std::getenv("SC_TRACE_FILES")) log("[not-bound] CreateFileA(%s) -> 0x%x", guest.c_str(), h);
        else {   // the first opens, always: which loose files the game reads, and their sizes
            static int opens = 0;
            if (opens++ < 600) log("[not-bound] open %s (%ld bytes, %s)", guest.c_str(), exists ? (long)st.st_size : 0L, mode);
        }
        return h;
    });
    K(br, "ReadFile", 5, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0), buf = c.arg(1), n = c.arg(2), outp = c.arg(3);
        auto it = wx86_files().find(h);
        if (it == wx86_files().end()) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
        WxFPos& p = wx86_file_pos()[h];
        const uint64_t t0 = wx86_now_us();
        uint32_t got = 0;
        auto ro = g_ro.find(h);
        if (ro != g_ro.end()) {
            if (void* hp = c.hostptr(buf, n)) got = cached_read(it->second, ro->second, p.logical, (uint8_t*)hp, n);
            else got = read_chunked(c, buf, n, [&](uint8_t* d, uint32_t k, uint32_t done) {
                return cached_read(it->second, ro->second, p.logical + done, d, k); });
            p.dir = 0; p.real = -1;                          // the stream position is the cache's business
        } else {
            if (p.real != p.logical || p.dir == 2) { std::fseek(it->second, p.logical, SEEK_SET); p.real = p.logical; }
            p.dir = 1;
            if (void* hp = c.hostptr(buf, n)) got = (uint32_t)std::fread(hp, 1, n, it->second);
            else got = read_chunked(c, buf, n, [&](uint8_t* d, uint32_t k, uint32_t) {
                return (uint32_t)std::fread(d, 1, k, it->second); });
        }
        g_io.calls++; g_io.bytes += got; g_io.host_us += wx86_now_us() - t0;
        if (std::getenv("SC_TRACE_FILES")) log("[not-bound] ReadFile(0x%x @%ld, %u -> buf 0x%08x) -> %u", h, p.logical, n, buf, got);
        p.logical += got; if (ro == g_ro.end()) p.real = p.logical;
        if (outp) c.write_u32(outp, got);
        return 1;
    });
    K(br, "WriteFile", 5, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0), buf = c.arg(1), n = c.arg(2), outp = c.arg(3);
        auto it = wx86_files().find(h);
        if (it == wx86_files().end()) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
        WxFPos& p = wx86_file_pos()[h];
        if (p.real != p.logical || p.dir == 1) { std::fseek(it->second, p.logical, SEEK_SET); p.real = p.logical; }
        p.dir = 2;
        std::vector<uint8_t> tmp(n); c.read(buf, tmp.data(), n);
        uint32_t put = (uint32_t)std::fwrite(tmp.data(), 1, n, it->second);
        p.logical += put; p.real = p.logical;
        if (outp) c.write_u32(outp, put);
        return put == n;
    });
    K(br, "SetFilePointer", 4, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0), hip = c.arg(2), how = c.arg(3);
        int32_t lo = (int32_t)c.arg(1);
        auto it = wx86_files().find(h);
        if (it == wx86_files().end()) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return INVALID_HANDLE; }
        if (hip) c.write_u32(hip, 0);   // files here are all < 2 GiB
        WxFPos& p = wx86_file_pos()[h];
        long base = 0;
        if (how == 1) base = p.logical;
        else if (how == 2) {
            auto ro = g_ro.find(h);
            if (ro != g_ro.end()) base = ro->second.size;
            else { std::fseek(it->second, 0, SEEK_END); base = std::ftell(it->second); p.real = base; }
        }
        long np = base + lo;
        if (np < 0) { wx86_set_lasterr(c, 131); return INVALID_HANDLE; }   // ERROR_NEGATIVE_SEEK
        p.logical = np;
        if (std::getenv("SC_TRACE_FILES")) log("[not-bound] SetFilePointer(0x%x, %d, how=%u, hi=%s) -> %ld", h, lo, how, hip ? "yes" : "no", np);
        wx86_set_lasterr(c, 0);
        return (uint32_t)np;
    });
    K(br, "GetFileSize", 2, [](Cpu& c) -> uint32_t {
        auto it = wx86_file_paths().find(c.arg(0));
        if (it == wx86_file_paths().end()) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return INVALID_HANDLE; }
        auto ro = g_ro.find(c.arg(0));
        if (ro != g_ro.end()) { if (c.arg(1)) c.write_u32(c.arg(1), 0); return (uint32_t)ro->second.size; }
        std::fflush(wx86_files()[c.arg(0)]);
        struct stat st{}; stat(it->second.c_str(), &st);
        if (c.arg(1)) c.write_u32(c.arg(1), 0);
        return (uint32_t)st.st_size;
    });
    K(br, "FlushFileBuffers", 1, [](Cpu& c) -> uint32_t {
        auto it = wx86_files().find(c.arg(0)); if (it == wx86_files().end()) return 0;
        std::fflush(it->second); return 1;
    });
    K(br, "CloseHandle", 1, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0);
        auto it = wx86_files().find(h);
        if (it != wx86_files().end()) {
            std::fclose(it->second); wx86_files().erase(it); g_ro.erase(h);
            wx86_file_paths().erase(h); wx86_file_pos().erase(h); return 1;
        }
        if (wx86_file_maps().erase(h)) return 1;
        if (wx86_handle_find(h)) { wx86_handle_erase(h); return 1; }
        if (h == 0xFFFFFFFFu || h == 0xFFFFFFFEu) return 1;   // pseudo handles
        wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0;
    });
    K(br, "DeleteFileA", 1, [](Cpu& c) -> uint32_t {
        if (::unlink(resolve_path(gstr(c, c.arg(0))).c_str()) == 0) return 1;
        wx86_set_lasterr(c, ERROR_FILE_NOT_FOUND); return 0;
    });
    K(br, "CreateDirectoryA", 2, [](Cpu& c) -> uint32_t {
        if (::mkdir(resolve_path(gstr(c, c.arg(0))).c_str(), 0777) == 0) return 1;
        wx86_set_lasterr(c, errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND); return 0;
    });
    K(br, "GetFileAttributesA", 1, [](Cpu& c) -> uint32_t {
        struct stat st{};
        if (stat(resolve_path(gstr(c, c.arg(0))).c_str(), &st)) { wx86_set_lasterr(c, ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; }
        return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    });
    K(br, "SetFileAttributesA", 2, [](Cpu& c) -> uint32_t {
        struct stat st{}; return stat(resolve_path(gstr(c, c.arg(0))).c_str(), &st) == 0;
    });
    K(br, "FindFirstFileA", 2, [](Cpu& c) -> uint32_t {
        std::string g = gstr(c, c.arg(0));
        size_t k = g.find_last_of("\\/");
        std::string gdir = k == std::string::npos ? "" : g.substr(0, k), pat = k == std::string::npos ? g : g.substr(k + 1);
        FindState fs; fs.dir = resolve_path(gdir.empty() ? "." : gdir); fs.pat = pat;
        fs.d = opendir(fs.dir.c_str());
        if (!fs.d || !find_next(c, fs, c.arg(1))) {
            if (fs.d) closedir(fs.d);
            wx86_set_lasterr(c, ERROR_FILE_NOT_FOUND); return INVALID_HANDLE;
        }
        uint32_t h = wx86_handle_next_id(); g_finds[h] = fs; return h;
    });
    K(br, "FindNextFileA", 2, [](Cpu& c) -> uint32_t {
        auto it = g_finds.find(c.arg(0));
        if (it == g_finds.end()) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
        if (find_next(c, it->second, c.arg(1))) return 1;
        wx86_set_lasterr(c, ERROR_NO_MORE_FILES); return 0;
    });
    K(br, "FindClose", 1, [](Cpu& c) -> uint32_t {
        auto it = g_finds.find(c.arg(0)); if (it == g_finds.end()) return 0;
        closedir(it->second.d); g_finds.erase(it); return 1;
    });
    K(br, "GetDiskFreeSpaceA", 5, [](Cpu& c) {
        // 512-byte sectors, 8 per cluster, 1 GiB free of 4 GiB.
        if (c.arg(1)) c.write_u32(c.arg(1), 8);
        if (c.arg(2)) c.write_u32(c.arg(2), 512);
        if (c.arg(3)) c.write_u32(c.arg(3), 262144);
        if (c.arg(4)) c.write_u32(c.arg(4), 1048576);
        return 1u;
    });
    K(br, "GetCurrentDirectoryA", 2, [](Cpu& c) { return put_str(c, c.arg(1), kGuestCwd, c.arg(0)); });
    K(br, "GetFullPathNameA", 4, [](Cpu& c) {
        std::string g = gstr(c, c.arg(0));
        std::string full = (g.size() > 1 && g[1] == ':') ? g : std::string(kGuestCwd) + "\\" + g;
        for (auto& ch : full) if (ch == '/') ch = '\\';
        uint32_t n = put_str(c, c.arg(2), full, c.arg(1));
        if (c.arg(3)) { size_t k = full.find_last_of('\\'); c.write_u32(c.arg(3), c.arg(2) + (uint32_t)(k + 1)); }
        return n;
    });
    K(br, "GetTempFileNameA", 4, [](Cpu& c) {
        static uint32_t seq = 1;
        char nm[32]; std::snprintf(nm, sizeof nm, "%s%04X.tmp", gstr(c, c.arg(1)).substr(0, 3).c_str(), seq++ & 0xFFFF);
        put_str(c, c.arg(3), gstr(c, c.arg(0)) + "\\" + nm, 260);
        return seq;
    });
    K(br, "GetLogicalDriveStringsA", 2, [](Cpu& c) {
        static const char drives[] = "C:\\\0";   // the game's own drive only
        if (c.arg(0) >= sizeof drives) c.write(c.arg(1), drives, sizeof drives);
        return (uint32_t)(sizeof drives - 1);
    });
    K(br, "GetVolumeInformationA", 8, [](Cpu& c) {
        if (c.arg(1)) put_str(c, c.arg(1), "VITA", c.arg(2));
        if (c.arg(3)) c.write_u32(c.arg(3), 0x56495441u);
        if (c.arg(4)) c.write_u32(c.arg(4), 255);
        if (c.arg(5)) c.write_u32(c.arg(5), 0x3);
        if (c.arg(6)) put_str(c, c.arg(6), "NTFS", c.arg(7));
        return 1u;
    });

    // ---- file mapping (storm reads MPQ headers this way) ----------------------
    K(br, "CreateFileMappingA", 6, [](Cpu& c) -> uint32_t {
        auto it = wx86_file_paths().find(c.arg(0));
        if (it == wx86_file_paths().end()) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
        std::fflush(wx86_files()[c.arg(0)]);
        struct stat st{}; stat(it->second.c_str(), &st);
        uint32_t h = wx86_fmap_next_id();
        wx86_file_maps()[h] = WxFMap{ it->second, c.arg(5) ? c.arg(5) : (uint32_t)st.st_size, 0 };
        return h;
    });
    K(br, "MapViewOfFile", 5, [](Cpu& c) -> uint32_t {
        auto it = wx86_file_maps().find(c.arg(0));
        if (it == wx86_file_maps().end()) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
        uint32_t off = c.arg(3), n = c.arg(4) ? c.arg(4) : it->second.size - off;
        uint32_t va = wx86_scratch_alloc((n + 0xFFF) & ~0xFFFu);
        if (!va) { log("[RAM-bound] [cause: scratch exhaustion] MapViewOfFile %u bytes", n); return 0; }
        std::vector<uint8_t> data = read_file(it->second.path);
        if (off < data.size()) c.write(va, data.data() + off, std::min<uint32_t>(n, (uint32_t)data.size() - off));
        it->second.view = va;
        return va;
    });
    K(br, "UnmapViewOfFile", 1, [](Cpu&) { return 1u; });
}

void register_module(const std::string& key, uint32_t base) {
    std::string lb = lower(key);
    g_modkey[lb] = key; g_modbase[base] = key;
    if (lb == "deusex.exe") g_exe_base = base;
}
uint32_t exe_base() { return g_exe_base; }
void mark_writable_sections(d2rt::Cpu& c, uint32_t base, const std::vector<uint8_t>& f, const std::string& name) {
    mark_writable_sections_impl(c, base, f, name);
}

}  // namespace sc
