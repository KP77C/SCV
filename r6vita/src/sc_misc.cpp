// sc_misc.cpp — the port's own shims beyond KERNEL32's file/module set:
// version info (A variants), CreateThread on the scheduler, an in-memory
// registry, and dialogs/message boxes that log instead of drawing.
#include "sc_host.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/win32_shims_kernel32.h"
#include "runtime/host_clock.h"

#include <cstring>
#include <cstdlib>
#include <map>
#include <set>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {
namespace {

void R(d2rt::Bridge& br, const char* dll, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    if (std::getenv("SC_TRACE_ALL")) {
        std::string t = name;
        fn = [t, f = std::move(fn)](Cpu& c) { uint32_t r = f(c); log("[not-bound] call %s -> 0x%x ra=0x%08x", t.c_str(), r, c.trap_retaddr()); return r; };
    }
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string(dll) + "!" + name;
    br.register_shim(dll, name, s);
}
std::string gstr(Cpu& c, uint32_t p) { return p ? wx86_gread_mb(c, p, -1) : std::string(); }

// ---- version resource straight from the PE file on disk ------------------
// Returns the raw RT_VERSION (16) resource of a PE file, or empty.
std::vector<uint8_t> pe_version_resource(const std::vector<uint8_t>& f) {
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= f.size() ? (uint32_t)(f[o] | f[o + 1] << 8) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= f.size() ? (u16(o) | u16(o + 2) << 16) : 0; };
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return {};
    size_t pe = u32(0x3c);
    uint32_t nsec = u16(pe + 6), optsz = u16(pe + 20);
    size_t sect = pe + 24 + optsz;
    uint32_t rs_rva = u32(pe + 24 + 96 + 2 * 8);
    if (!rs_rva) return {};
    auto rva2off = [&](uint32_t rva) -> size_t {
        for (uint32_t i = 0; i < nsec; ++i) {
            size_t s = sect + 40 * i;
            uint32_t va = u32(s + 12), vs = u32(s + 8), raw = u32(s + 20), rsz = u32(s + 16);
            if (rva >= va && rva < va + (vs > rsz ? vs : rsz)) return raw + (rva - va);
        }
        return 0;
    };
    size_t root = rva2off(rs_rva);
    if (!root) return {};
    // root -> type 16 -> first name -> first language -> data entry
    auto child = [&](size_t dir, int want_id) -> uint32_t {
        uint32_t n = u16(dir + 12) + u16(dir + 14);
        for (uint32_t i = 0; i < n; ++i) {
            size_t e = dir + 16 + 8 * i;
            if (want_id < 0 || (!(u32(e) & 0x80000000u) && u32(e) == (uint32_t)want_id)) return u32(e + 4);
        }
        return 0xFFFFFFFFu;
    };
    uint32_t o = child(root, 16);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return {};
    o = child(root + (o & 0x7FFFFFFFu), -1);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return {};
    o = child(root + (o & 0x7FFFFFFFu), -1);
    if (o == 0xFFFFFFFFu || (o & 0x80000000u)) return {};
    size_t de = root + o;
    size_t off = rva2off(u32(de)), sz = u32(de + 4);
    if (!off || off + sz > f.size()) return {};
    return std::vector<uint8_t>(f.begin() + off, f.begin() + off + sz);
}

// ---- registry: in memory, saved to <game>/w3vita_registry.txt -------------
// WarCraft III keeps its player profiles, campaign progress and options in
// the registry, so the values are written back to a text file whenever the
// game closes or flushes a key it changed, and read again at the next start.
std::map<uint32_t, std::string> g_keys;                       // HKEY -> full path (lower)
std::map<std::string, std::map<std::string, std::vector<uint8_t>>> g_vals;   // path -> name -> data
std::map<std::string, std::map<std::string, uint32_t>> g_types;
uint32_t g_next_key = 0x00A00000u;
std::map<uint32_t, uint32_t> g_timer_kill;   // timeSetEvent id -> guest kill flag

std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
bool g_reg_dirty = false;
std::string reg_file() { return g_root + "/w3vita_registry.txt"; }
// One value per line: key <TAB> name <TAB> type <TAB> hex bytes. Keys and
// names are stored lower-case (lookups are case-insensitive anyway).
void reg_save() {
    if (!g_reg_dirty) return;
    g_reg_dirty = false;
    std::string out;
    for (auto& k : g_vals) {
        if (k.second.empty()) { out += k.first + "\t\t0\t\n"; continue; }   // an empty key still exists
        for (auto& v : k.second) {
            char t[16]; std::snprintf(t, sizeof t, "%u", g_types[k.first][v.first]);
            out += k.first + "\t" + v.first + "\t" + t + "\t";
            static const char* hx = "0123456789abcdef";
            for (uint8_t b : v.second) { out += hx[b >> 4]; out += hx[b & 15]; }
            out += "\n";
        }
    }
    const std::string tmp = reg_file() + ".new";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        if (ok) { std::remove(reg_file().c_str()); std::rename(tmp.c_str(), reg_file().c_str()); }
    }
}
void reg_load() {
    FILE* f = std::fopen(reg_file().c_str(), "rb"); if (!f) return;
    std::string text; char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    size_t i = 0, count = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i); i = e + 1;
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1),
               c3 = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c3 == std::string::npos) continue;
        const std::string key = line.substr(0, a), name = line.substr(a + 1, b - a - 1);
        auto& vals = g_vals[key];
        if (name.empty()) continue;
        std::vector<uint8_t> d;
        for (size_t k = c3 + 1; k + 1 < line.size(); k += 2) d.push_back((uint8_t)std::strtoul(line.substr(k, 2).c_str(), nullptr, 16));
        vals[name] = d; g_types[key][name] = (uint32_t)std::strtoul(line.substr(b + 1, c3 - b - 1).c_str(), nullptr, 10);
        ++count;
    }
    log("[not-bound] registry: %u values from %s", (unsigned)count, reg_file().c_str());
}
std::string key_path(uint32_t parent, const std::string& sub) {
    std::string base;
    switch (parent) {
        case 0x80000000u: base = "hkcr"; break;
        case 0x80000001u: base = "hkcu"; break;
        case 0x80000002u: base = "hklm"; break;
        case 0x80000003u: base = "hku";  break;
        default: { auto it = g_keys.find(parent); base = it == g_keys.end() ? "?" : it->second; }
    }
    return sub.empty() ? base : base + "\\" + lower(sub);
}

// wsprintfA's formatting (integers, strings, chars; no floating point,
// which wsprintf never supported). args = guest address of the first vararg.
std::string wsprintf_fmt(Cpu& c, const std::string& f, uint32_t args) {
    std::string out;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') { out += f[i]; continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < f.size() && strchr("-+ #0", f[j])) spec += f[j++];
        while (j < f.size() && isdigit((unsigned char)f[j])) spec += f[j++];
        if (j < f.size() && f[j] == '.') { spec += f[j++]; while (j < f.size() && isdigit((unsigned char)f[j])) spec += f[j++]; }
        while (j < f.size() && (f[j] == 'l' || f[j] == 'h')) ++j;   // all 32-bit on Win32
        if (j >= f.size()) break;
        char t = f[j]; char buf[512];
        switch (t) {
            case 'd': case 'i': std::snprintf(buf, sizeof buf, (spec + "d").c_str(), (int32_t)c.read_u32(args)); args += 4; out += buf; break;
            case 'u': case 'x': case 'X': std::snprintf(buf, sizeof buf, (spec + t).c_str(), c.read_u32(args)); args += 4; out += buf; break;
            case 'c': out += (char)c.read_u32(args); args += 4; break;
            case 's': std::snprintf(buf, sizeof buf, (spec + "s").c_str(), gstr(c, c.read_u32(args)).c_str()); args += 4; out += buf; break;
            case '%': out += '%'; break;
            default: out += spec; out += t; break;
        }
        i = j;
    }
    return out;
}

}  // namespace

// Pre-set a registry value (REG_DWORD or REG_SZ) before the game starts.
void reg_seed_dword(const std::string& key, const std::string& name, uint32_t v) {
    std::string p = lower(key), n = lower(name);
    std::vector<uint8_t> d(4); std::memcpy(d.data(), &v, 4);
    g_vals[p][n] = d; g_types[p][n] = 4;
}
// A first-launch default: set only when the value is not there yet, so a
// change the player makes in the game's options (saved to the registry file)
// wins on the next launch.
void reg_default_dword(const std::string& key, const std::string& name, uint32_t v) {
    std::string p = lower(key), n = lower(name);
    auto k = g_vals.find(p);
    if (k != g_vals.end() && k->second.count(n)) return;
    reg_seed_dword(key, name, v);
}
bool reg_has(const std::string& key, const std::string& name) {
    auto k = g_vals.find(lower(key));
    return k != g_vals.end() && k->second.count(lower(name));
}
void reg_seed_sz(const std::string& key, const std::string& name, const std::string& v) {
    std::string p = lower(key), n = lower(name);
    std::vector<uint8_t> d(v.begin(), v.end()); d.push_back(0);
    g_vals[p][n] = d; g_types[p][n] = 1;
}

void install_misc(d2rt::Bridge& br) {
    reg_load();
    {   // cdecl, variadic: the caller cleans the stack.
        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "USER32.dll!wsprintfA";
        s.fn = [](Cpu& c) -> uint32_t {
            std::string r = wsprintf_fmt(c, gstr(c, c.arg(1)), c.reg(d2rt::R_ESP) + 12);
            if (r.size() > 1023) r.resize(1023);   // wsprintf's documented 1024-byte limit
            return put_str(c, c.arg(0), r, 1024);
        };
        br.register_shim("USER32.dll", "wsprintfA", s);
    }
    R(br, "KERNEL32.dll", "GetProfileIntA", 3, [](Cpu& c) { return c.arg(2); });   // no win.ini: defaults
    R(br, "WINMM.dll", "timeGetTime", 0, [](Cpu&) { return (uint32_t)wx86_now_ms(); });
    // ---- GDI pieces the Smacker player asks for -------------------------------
    R(br, "GDI32.dll", "GetSystemPaletteUse", 1, [](Cpu&) { return 1u; });   // SYSPAL_STATIC
    R(br, "GDI32.dll", "SetSystemPaletteUse", 2, [](Cpu&) { return 1u; });
    R(br, "GDI32.dll", "SetDIBColorTable", 4, [](Cpu& c) { return c.arg(2); });
    R(br, "GDI32.dll", "CreateDIBSection", 6, [](Cpu& c) -> uint32_t {
        uint32_t bmi = c.arg(1);
        int32_t w = (int32_t)c.read_u32(bmi + 4), h = (int32_t)c.read_u32(bmi + 8);
        uint16_t bpp = 0; c.read(bmi + 14, &bpp, 2);
        uint32_t stride = ((uint32_t)(w < 0 ? -w : w) * bpp / 8 + 3) & ~3u;
        uint32_t bytes = stride * (uint32_t)(h < 0 ? -h : h);
        uint32_t bits = wx86_scratch_alloc(bytes ? bytes : 4);
        if (!bits) { log("[RAM-bound] [cause: scratch exhaustion] CreateDIBSection %dx%dx%u", w, h, bpp); return 0; }
        if (c.arg(3)) c.write_u32(c.arg(3), bits);
        static uint32_t next = 0x88000000u;
        log("[not-bound] CreateDIBSection %dx%dx%u -> bits 0x%08x", w, h, bpp, bits);
        return next++;
    });
    R(br, "WINMM.dll", "timeBeginPeriod", 1, [](Cpu&) { return 0u; });
    R(br, "WINMM.dll", "timeEndPeriod", 1, [](Cpu&) { return 0u; });
    // timeSetEvent: a guest thread running a small x86 loop,
    //   loop: Sleep(delay); if (kill) exit; callback(id, 0, user, 0, 0);
    //         if (one-shot) exit; goto loop
    // The kill check sits between the sleep and the callback: after
    // timeKillEvent returns, Windows guarantees no further callback, and the
    // caller frees the callback's data right away (Smacker does).
    R(br, "WINMM.dll", "timeSetEvent", 5, [](Cpu& c) -> uint32_t {
        static uint32_t next_id = 1;
        uint32_t delay = c.arg(0), cb = c.arg(2), user = c.arg(3), flags = c.arg(4);
        if (flags & 0x10) { log("[not-bound] [cause: unsupported timer kind] timeSetEvent event-handle flag 0x%x", flags); return 0; }
        bool periodic = flags & 1;   // TIME_PERIODIC
        uint32_t id = next_id++;
        uint32_t kill = wx86_scratch_alloc(4); c.write_u32(kill, 0);
        uint32_t sleep_trap = g_br->shim_trap("KERNEL32.dll", "Sleep");
        std::vector<uint8_t> code;
        auto b = [&](std::initializer_list<int> v) { for (int x : v) code.push_back((uint8_t)x); };
        auto d = [&](uint32_t v) { for (int i = 0; i < 4; ++i) code.push_back((uint8_t)(v >> (8 * i))); };
        b({0x68}); d(delay ? delay : 1); b({0xB8}); d(sleep_trap); b({0xFF, 0xD0});      // Sleep(delay)
        b({0x83, 0x3D}); d(kill); b({0x00});                                             // cmp [kill], 0
        size_t jne_at = code.size(); b({0x75, 0x00});                                    // jne exit (patched)
        b({0x6A, 0x00, 0x6A, 0x00}); b({0x68}); d(user); b({0x6A, 0x00}); b({0x68}); d(id);
        b({0xB8}); d(cb); b({0xFF, 0xD0});                                               // cb(id,0,user,0,0)
        if (periodic) { int rel = -(int)(code.size() + 2); b({0xEB, rel & 0xFF}); }      // jmp loop
        code[jne_at + 1] = (uint8_t)(code.size() - (jne_at + 2));
        b({0xC2, 0x04, 0x00});                                                           // exit: ret 4
        uint32_t stub = wx86_scratch_alloc((uint32_t)code.size());
        c.write(stub, code.data(), (uint32_t)code.size());
        g_timer_kill[id] = kill;
        auto* sch = wx86_sched();
        if (!sch || !sch->create_thread(stub, 0, 0x10000, false)) return 0;
        log("[not-bound] timeSetEvent id=%u every %u ms -> callback 0x%08x%s", id, delay, cb, periodic ? "" : " (one-shot)");
        return id;
    });
    R(br, "WINMM.dll", "timeKillEvent", 1, [](Cpu& c) -> uint32_t {
        auto it = g_timer_kill.find(c.arg(0)); if (it == g_timer_kill.end()) return 97u;   // MMSYSERR_INVALPARAM
        c.write_u32(it->second, 1); return 0u;
    });
    // ---- VERSION.dll, ANSI variants (the engine ships the W ones) --------
    R(br, "VERSION.dll", "GetFileVersionInfoSizeA", 2, [](Cpu& c) {
        auto v = pe_version_resource(read_file(resolve_path(gstr(c, c.arg(0)))));
        if (c.arg(1)) c.write_u32(c.arg(1), 0);
        if (v.empty()) wx86_set_lasterr(c, 1813);   // ERROR_RESOURCE_TYPE_NOT_FOUND
        return (uint32_t)v.size();
    });
    R(br, "VERSION.dll", "GetFileVersionInfoA", 4, [](Cpu& c) -> uint32_t {
        auto v = pe_version_resource(read_file(resolve_path(gstr(c, c.arg(0)))));
        uint32_t buf = c.arg(3), len = c.arg(2);
        if (v.empty() || !buf) return 0;
        c.write(buf, v.data(), (uint32_t)std::min<size_t>(v.size(), len));
        return 1;
    });

    // ---- threads ---------------------------------------------------------
    R(br, "KERNEL32.dll", "CreateThread", 6, [](Cpu& c) -> uint32_t {
        uint32_t stack = c.arg(1), entry = c.arg(2), param = c.arg(3), flags = c.arg(4), tidp = c.arg(5);
        auto* s = wx86_sched();
        if (!s) return 0;
        d2rt::GuestThread* t = s->create_thread(entry, param, stack ? stack : 0x100000, (flags & 4) != 0);
        if (!t) { log("[RAM-bound] [cause: thread stack region full] CreateThread(entry=0x%08x) refused", entry); return 0; }
        auto* w = new WxThread(); w->gt = t;
        uint32_t h = wx86_handle_add(w);
        if (tidp) c.write_u32(tidp, wx86_win_tid(t->id));
        log("[not-bound] CreateThread entry=0x%08x param=0x%08x -> thread %u", entry, param, t->id);
        return h;
    });

    R(br, "KERNEL32.dll", "ExitThread", 1, [](Cpu& c) -> uint32_t {
        auto* s = wx86_sched();
        if (s) s->exit_current(c.arg(0));
        return 0;
    });

    // ---- registry --------------------------------------------------------
    auto open = [](Cpu& c, uint32_t parent, uint32_t sub, uint32_t out, bool create) -> uint32_t {
        std::string p = key_path(parent, gstr(c, sub));
        static const bool tr = std::getenv("SC_TRACE_REG") != nullptr;
        if (tr) log("[reg] %s %s%s", create ? "create" : "open", p.c_str(), (!create && !g_vals.count(p)) ? " (missing)" : "");
        if (!create && !g_vals.count(p)) return 2u;   // ERROR_FILE_NOT_FOUND
        if (!g_vals.count(p)) g_reg_dirty = true;
        g_vals[p];
        uint32_t h = g_next_key++; g_keys[h] = p;
        if (out) c.write_u32(out, h);
        return 0u;
    };
    R(br, "ADVAPI32.dll", "RegOpenKeyExA", 5, [open](Cpu& c) { return open(c, c.arg(0), c.arg(1), c.arg(4), false); });
    R(br, "ADVAPI32.dll", "RegCreateKeyExA", 9, [open](Cpu& c) {
        uint32_t r = open(c, c.arg(0), c.arg(1), c.arg(7), true);
        if (c.arg(8)) c.write_u32(c.arg(8), 1);   // REG_CREATED_NEW_KEY
        return r;
    });
    R(br, "ADVAPI32.dll", "RegCloseKey", 1, [](Cpu& c) { g_keys.erase(c.arg(0)); reg_save(); return 0u; });
    R(br, "ADVAPI32.dll", "RegFlushKey", 1, [](Cpu&) { reg_save(); return 0u; });
    R(br, "ADVAPI32.dll", "RegQueryValueExA", 6, [](Cpu& c) -> uint32_t {
        std::string p = key_path(c.arg(0), ""), n = lower(gstr(c, c.arg(1)));
        auto k = g_vals.find(p);
        static const bool tr = std::getenv("SC_TRACE_REG") != nullptr;
        if (tr) log("[reg] query %s\\%s%s", p.c_str(), n.c_str(), (k == g_vals.end() || !k->second.count(n)) ? " (missing)" : "");
        if (k == g_vals.end() || !k->second.count(n)) return 2u;
        auto& d = k->second[n];
        uint32_t typep = c.arg(3), data = c.arg(4), lenp = c.arg(5);
        if (typep) c.write_u32(typep, g_types[p][n]);
        uint32_t cap = lenp ? c.read_u32(lenp) : 0;
        if (lenp) c.write_u32(lenp, (uint32_t)d.size());
        if (data) { if (cap < d.size()) return 234u; c.write(data, d.data(), (uint32_t)d.size()); }   // ERROR_MORE_DATA
        return 0u;
    });
    R(br, "ADVAPI32.dll", "RegSetValueExA", 6, [](Cpu& c) -> uint32_t {
        std::string p = key_path(c.arg(0), ""), n = lower(gstr(c, c.arg(1)));
        std::vector<uint8_t> d(c.arg(5)); if (!d.empty()) c.read(c.arg(4), d.data(), (uint32_t)d.size());
        g_vals[p][n] = d; g_types[p][n] = c.arg(3); g_reg_dirty = true;
        static const bool tr = std::getenv("SC_TRACE_REG") != nullptr;
        if (tr) log("[reg] set %s\\%s (%u bytes, type %u)", p.c_str(), n.c_str(), (unsigned)d.size(), c.arg(3));
        return 0u;
    });
    R(br, "ADVAPI32.dll", "RegDeleteValueA", 2, [](Cpu& c) -> uint32_t {
        std::string p = key_path(c.arg(0), ""), n = lower(gstr(c, c.arg(1)));
        if (!g_vals[p].erase(n)) return 2u;
        g_reg_dirty = true; return 0u;
    });
    // Subkeys of a key, in name order (dwIndex counts from 0).
    R(br, "ADVAPI32.dll", "RegEnumKeyExA", 8, [](Cpu& c) -> uint32_t {
        const std::string p = key_path(c.arg(0), "") + "\\";
        std::set<std::string> subs;
        for (auto& k : g_vals)
            if (k.first.size() > p.size() && k.first.compare(0, p.size(), p) == 0)
                subs.insert(k.first.substr(p.size(), k.first.find('\\', p.size()) - p.size()));
        uint32_t idx = c.arg(1);
        if (idx >= subs.size()) return 259u;                                     // ERROR_NO_MORE_ITEMS
        auto it = subs.begin(); std::advance(it, idx);
        const uint32_t cap = c.arg(3) ? c.read_u32(c.arg(3)) : 0;
        if (cap <= it->size()) return 234u;                                      // ERROR_MORE_DATA
        c.write(c.arg(2), it->c_str(), (uint32_t)it->size() + 1);
        c.write_u32(c.arg(3), (uint32_t)it->size());
        return 0u;
    });
    R(br, "ADVAPI32.dll", "RegDeleteKeyA", 2, [](Cpu& c) -> uint32_t {
        const std::string p = key_path(c.arg(0), gstr(c, c.arg(1)));
        if (!g_vals.erase(p)) return 2u;
        g_types.erase(p); g_reg_dirty = true; reg_save(); return 0u;
    });
    R(br, "ADVAPI32.dll", "RegQueryInfoKeyA", 12, [](Cpu& c) {
        for (int i : { 4, 5, 6, 7, 8, 9 }) if (c.arg(i)) c.write_u32(c.arg(i), 0);
        return 0u;
    });
    R(br, "ADVAPI32.dll", "AllocateAndInitializeSid", 11, [](Cpu& c) {
        static uint32_t sid = wx86_scratch_alloc(16);
        if (c.arg(10)) c.write_u32(c.arg(10), sid);
        return 1u;
    });

    // ---- windows and dialogs: report, don't draw --------------------------
    R(br, "USER32.dll", "GetForegroundWindow", 0, [](Cpu&) { return 0u; });
    R(br, "USER32.dll", "MessageBoxA", 4, [](Cpu& c) {
        log("[not-bound] [cause: guest error dialog] MessageBoxA \"%s\": %s",
            gstr(c, c.arg(2)).c_str(), gstr(c, c.arg(1)).c_str());
        log("[not-bound]   stack:%s", guest_stack(c).c_str());
        return 1u;   // IDOK
    });
    R(br, "USER32.dll", "DialogBoxParamA", 5, [](Cpu& c) {
        uint32_t tmpl = c.arg(1);
        std::string id = tmpl < 0x10000 ? "#" + std::to_string(tmpl) : gstr(c, tmpl);
        log("[not-bound] [cause: guest dialog] DialogBoxParamA template=%s dlgproc=0x%08x param=0x%08x ra=0x%08x",
            id.c_str(), c.arg(3), c.arg(4), c.trap_retaddr());
        if (const char* d = std::getenv("SC_DUMP")) {   // guest heap + VA arena snapshot for offline checks
            FILE* f = std::fopen(d, "wb");
            std::vector<uint8_t> buf(0x100000);
            for (uint32_t base : { 0x01000000u, 0x08000000u }) {
                uint32_t size = base == 0x01000000u ? 0x04000000u : base == 0x08000000u ? 0x0C000000u : 0x00100000u;
                for (uint32_t o = 0; o < size; o += 0x100000) { c.read(base + o, buf.data(), 0x100000); std::fwrite(buf.data(), 1, 0x100000, f); }
            }
            buf.resize(0x69000); c.read(0x30000000u, buf.data(), 0x69000); std::fwrite(buf.data(), 1, 0x69000, f);   // storm.dll image
            std::fclose(f);
            log("[not-bound] dumped heap+VA+storm to %s", d);
        }
        return 2u;   // IDCANCEL
    });
}

}  // namespace sc
