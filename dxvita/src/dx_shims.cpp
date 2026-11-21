// dx_shims.cpp — Win32 shims Deus Ex needs beyond the StarCraft port's set.
// Installed last (last registration wins).
#include "sc_host.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>

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

uint32_t g_next_gdi = 0x00D10000;   // fake GDI handles for objects nothing draws

// DIB sections: the software renderer draws into one and BitBlts it to the
// window each frame. That BitBlt is the frame boundary.
struct Dib { uint32_t bits, w, h, bpp, stride; bool topdown; };
std::map<uint32_t, Dib> g_dibs;      // HBITMAP -> DIB
std::map<uint32_t, uint32_t> g_dc_bmp;   // memory DC -> selected HBITMAP
uint32_t g_last_dib = 0;

}  // namespace

void present(d2rt::Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h, const uint32_t* pal);

void install_dx(d2rt::Bridge& br) {
    R(br, "GDI32.dll", "CreateDIBSection", 6, [](Cpu& c) -> uint32_t {
        uint32_t bmi = c.arg(1);
        int32_t w = (int32_t)c.read_u32(bmi + 4), h = (int32_t)c.read_u32(bmi + 8);
        uint16_t bpp = 0; c.read(bmi + 14, &bpp, 2);
        Dib d; d.w = (uint32_t)(w < 0 ? -w : w); d.h = (uint32_t)(h < 0 ? -h : h); d.bpp = bpp; d.topdown = h < 0;
        d.stride = (d.w * bpp / 8 + 3) & ~3u;
        uint32_t bytes = d.stride * d.h;
        d.bits = wx86_scratch_alloc(bytes ? bytes : 4);
        if (!d.bits) { log("[RAM-bound] [cause: scratch exhaustion] CreateDIBSection %dx%dx%u", w, h, bpp); return 0; }
        if (c.arg(3)) c.write_u32(c.arg(3), d.bits);
        uint32_t hb = g_next_gdi += 4;
        g_dibs[hb] = d; g_last_dib = hb;
        log("[not-bound] CreateDIBSection %dx%dx%u -> bitmap 0x%x bits 0x%08x", w, h, bpp, hb, d.bits);
        return hb;
    });
    R(br, "GDI32.dll", "SelectObject", 2, [](Cpu& c) -> uint32_t {
        uint32_t dc = c.arg(0), obj = c.arg(1);
        if (!g_dibs.count(obj)) return 1u;
        uint32_t old = g_dc_bmp.count(dc) ? g_dc_bmp[dc] : 1u;
        g_dc_bmp[dc] = obj;
        return old;
    });
    R(br, "GDI32.dll", "CreateCompatibleDC", 1, [](Cpu&) { return g_next_gdi += 4; });
    auto blit = [](Cpu& c, uint32_t src_dc) -> uint32_t {
        uint32_t hb = g_dc_bmp.count(src_dc) ? g_dc_bmp[src_dc] : g_last_dib;
        auto it = g_dibs.find(hb);
        if (it == g_dibs.end()) return 1u;
        const Dib& d = it->second;
        if (d.bpp != 32) {
            static int said = 0;
            if (said++ < 3) log("[not-bound] [cause: unsupported] BitBlt from a %u-bit DIB", d.bpp);
            return 1u;
        }
        present(c, d.bits, d.stride, d.w, d.h, nullptr);
        return 1u;
    };
    R(br, "GDI32.dll", "BitBlt", 9, [blit](Cpu& c) { return blit(c, c.arg(5)); });
    R(br, "GDI32.dll", "StretchBlt", 11, [blit](Cpu& c) { return blit(c, c.arg(5)); });

    // Working set: 64 MiB min / 256 MiB max (Unreal only logs it).
    R(br, "KERNEL32.dll", "GetProcessWorkingSetSize", 3, [](Cpu& c) {
        if (c.arg(1)) c.write_u32(c.arg(1), 64u << 20);
        if (c.arg(2)) c.write_u32(c.arg(2), 256u << 20);
        return 1u;
    });
    R(br, "KERNEL32.dll", "SetProcessWorkingSetSize", 3, [](Cpu&) { return 1u; });
    R(br, "COMCTL32.dll", "#17", 0, [](Cpu&) { return 0u; });   // InitCommonControls
    R(br, "COMCTL32.dll", "InitCommonControls", 0, [](Cpu&) { return 0u; });
    R(br, "KERNEL32.dll", "GetModuleHandleW", 1, [](Cpu& c) -> uint32_t {
        if (!c.arg(0)) return exe_base();
        std::vector<uint16_t> w = wx86_gread_wc(c, c.arg(0), -1);
        std::string a; for (uint16_t ch : w) a += (char)ch;
        log("[not-bound] GetModuleHandleW(%s): ANSI path expected on Windows 98", a.c_str());
        return 0u;
    });
    // Atoms and registered messages: unique numbers in the 0xC000 range.
    static auto next_atom = [] { static uint32_t a = 0xC800; return a++; };
    R(br, "KERNEL32.dll", "GlobalAddAtomA", 1, [](Cpu&) { return next_atom(); });
    R(br, "KERNEL32.dll", "GlobalAddAtomW", 1, [](Cpu&) { return next_atom(); });
    R(br, "KERNEL32.dll", "GlobalDeleteAtom", 1, [](Cpu&) { return 0u; });
    R(br, "USER32.dll", "RegisterWindowMessageA", 1, [](Cpu&) { return next_atom(); });
    // GDI objects the editor-style UI makes and nothing draws with.
    R(br, "GDI32.dll", "CreatePatternBrush", 1, [](Cpu&) { return g_next_gdi += 4; });
    R(br, "GDI32.dll", "CreateFontW", 14, [](Cpu&) { return g_next_gdi += 4; });
    R(br, "GDI32.dll", "CreateFontIndirectA", 1, [](Cpu&) { return g_next_gdi += 4; });
    // COM: only DirectSound is ever created, and Galaxy asks for it by
    // CLSID. Anything else is "class not registered".
    R(br, "ole32.dll", "CoInitialize", 1, [](Cpu&) { return 0u; });
    R(br, "ole32.dll", "CoUninitialize", 0, [](Cpu&) { return 0u; });
    R(br, "ole32.dll", "CoCreateInstance", 5, [](Cpu& c) -> uint32_t {
        uint8_t g[16] = {}; c.read(c.arg(0), g, 16);
        log("[not-bound] CoCreateInstance clsid=%08x-%04x-%04x ppv=0x%08x", g[0] | g[1] << 8 | g[2] << 16 | (uint32_t)g[3] << 24,
            g[4] | g[5] << 8, g[6] | g[7] << 8, c.arg(4));
        if (c.arg(4)) c.write_u32(c.arg(4), 0);
        // CLSID_DirectSound {47D4D946-62E8-11CF-...}: the engine's DirectSound
        // (the same object DirectSoundCreate returns; Galaxy then calls
        // IDirectSound::Initialize on it).
        static const uint8_t ds[8] = { 0x46, 0xD9, 0xD4, 0x47, 0xE8, 0x62, 0xCF, 0x11 };
        if (!std::memcmp(g, ds, 8) && c.arg(4)) {
            uint32_t t = g_br->shim_trap("DSOUND.dll", "DirectSoundCreate");
            uint32_t hr = t ? guest_call(c, t, { 0, c.arg(4), 0 }) : 0x80004005u;
            log("[not-bound] CoCreateInstance(DirectSound) -> 0x%08x obj=0x%08x", hr, c.read_u32(c.arg(4)));
            return hr;
        }
        return 0x80040154u;   // REGDB_E_CLASSNOTREG
    });
    // Wide registry calls (Galaxy's settings): no registry here.
    R(br, "ADVAPI32.dll", "RegCreateKeyExW", 9, [](Cpu&) { return 2u; });
    R(br, "ADVAPI32.dll", "RegOpenKeyExW", 5, [](Cpu&) { return 2u; });
    R(br, "ADVAPI32.dll", "RegSetValueExW", 6, [](Cpu&) { return 2u; });
    R(br, "ADVAPI32.dll", "RegQueryValueExW", 6, [](Cpu&) { return 2u; });
    // The startup splash runs on its own thread with a modeless dialog: no
    // dialog here, the thread just waits for the quit message.
    R(br, "USER32.dll", "CreateDialogParamA", 5, [](Cpu&) { return g_next_gdi += 4; });
    R(br, "USER32.dll", "PostThreadMessageA", 4, [](Cpu&) { return 1u; });
    // COM: only DirectSound is ever created, and Galaxy asks for it by
    // CLSID. Anything else is "class not registered".
    R(br, "ole32.dll", "CoInitialize", 1, [](Cpu&) { return 0u; });
    R(br, "ole32.dll", "CoUninitialize", 0, [](Cpu&) { return 0u; });
    R(br, "ole32.dll", "CoCreateInstance", 5, [](Cpu& c) -> uint32_t {
        uint8_t g[16] = {}; c.read(c.arg(0), g, 16);
        log("[not-bound] CoCreateInstance clsid=%08x-%04x-%04x ppv=0x%08x", g[0] | g[1] << 8 | g[2] << 16 | (uint32_t)g[3] << 24,
            g[4] | g[5] << 8, g[6] | g[7] << 8, c.arg(4));
        if (c.arg(4)) c.write_u32(c.arg(4), 0);
        // CLSID_DirectSound {47D4D946-62E8-11CF-...}: the engine's DirectSound
        // (the same object DirectSoundCreate returns; Galaxy then calls
        // IDirectSound::Initialize on it).
        static const uint8_t ds[8] = { 0x46, 0xD9, 0xD4, 0x47, 0xE8, 0x62, 0xCF, 0x11 };
        if (!std::memcmp(g, ds, 8) && c.arg(4)) {
            uint32_t t = g_br->shim_trap("DSOUND.dll", "DirectSoundCreate");
            uint32_t hr = t ? guest_call(c, t, { 0, c.arg(4), 0 }) : 0x80004005u;
            log("[not-bound] CoCreateInstance(DirectSound) -> 0x%08x obj=0x%08x", hr, c.read_u32(c.arg(4)));
            return hr;
        }
        return 0x80040154u;   // REGDB_E_CLASSNOTREG
    });
    // Wide registry calls (Galaxy's settings): no registry here.
    R(br, "ADVAPI32.dll", "RegCreateKeyExW", 9, [](Cpu&) { return 2u; });
    R(br, "ADVAPI32.dll", "RegOpenKeyExW", 5, [](Cpu&) { return 2u; });
    R(br, "ADVAPI32.dll", "RegSetValueExW", 6, [](Cpu&) { return 2u; });
    R(br, "ADVAPI32.dll", "RegQueryValueExW", 6, [](Cpu&) { return 2u; });
    // The startup splash (Help\Logo.bmp): a bitmap handle nothing ever draws.
    R(br, "GDI32.dll", "CreateDIBitmap", 6, [](Cpu&) { return g_next_gdi += 4; });
}

}  // namespace sc
