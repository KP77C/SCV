// sc_ddraw.cpp — DirectDraw (v1 interfaces) for StarCraft: one 8-bit
// 640x480 display mode, surfaces in guest memory, a 256-entry palette.
//
// COM objects live in guest memory as { vtbl, id }; every vtable slot is a
// trap into a native method here. The primary surface's pixels and the
// current palette are handed to sc::present() on Unlock/Flip; on the Vita
// that becomes the sceGxm palette pass, on the host test it is a capture.
#include "sc_host.h"

#include "runtime/guest_scratch.h"

#include <cstring>
#include <map>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {

// Implemented in sc_present.cpp.
void present(Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h, const uint32_t* pal);

namespace {

constexpr uint32_t DD_OK = 0, DDERR_GENERIC = 0x80004005u, E_NOINTERFACE = 0x80004002u,
                   DDERR_INVALIDPARAMS = 0x80070057u, DDERR_NOTFOUND = 0x887600FFu;

struct Surface { uint32_t w = 640, h = 480, pitch = 640, pixels = 0, caps = 0, palette = 0, refs = 1, back = 0; bool primary = false; };
struct Palette { uint32_t entries[256] = {}; uint32_t refs = 1; };

std::map<uint32_t, Surface> g_surf;   // guest object VA -> surface
std::map<uint32_t, Palette> g_pal;    // guest object VA -> palette
uint32_t g_dd = 0, g_primary = 0;
uint32_t g_vt_dd = 0, g_vt_surf = 0, g_vt_pal = 0;
uint32_t g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 8;

struct Method { const char* name; uint32_t argc; std::function<uint32_t(Cpu&)> fn; };

// Builds a vtable of traps in guest memory.
uint32_t make_vtable(d2rt::Bridge& br, const char* iface, const std::vector<Method>& ms) {
    uint32_t vt = wx86_scratch_alloc(4 * (uint32_t)ms.size());
    for (size_t i = 0; i < ms.size(); ++i) {
        Shim s; s.argc = ms[i].argc; s.stdcall_cleanup = true;
        std::string key = std::string(iface) + "::" + ms[i].name;
        s.tag = "ddraw.com!" + key;
        auto fn = ms[i].fn;
        if (!fn) {
            std::string k = key;
            fn = [k](Cpu& c) { log("[not-bound] [cause: missing COM method] %s ra=0x%08x", k.c_str(), c.trap_retaddr()); return DDERR_GENERIC; };
        }
        if (std::getenv("SC_TRACE_ALL")) {
            std::string k = key;
            fn = [k, f = fn](Cpu& c) { uint32_t r = f(c); log("[not-bound] call %s -> 0x%x", k.c_str(), r); return r; };
        }
        s.fn = fn;
        br.register_shim("ddraw.com", key, s);
        g_cpu->write_u32(vt + 4 * (uint32_t)i, br.shim_trap("ddraw.com", key));
    }
    return vt;
}

uint32_t new_object(uint32_t vt) {
    uint32_t o = wx86_scratch_alloc(16);
    g_cpu->write_u32(o, vt);
    return o;
}

uint32_t new_surface(Cpu& c, uint32_t w, uint32_t h, uint32_t caps, bool primary) {
    uint32_t o = new_object(g_vt_surf);
    Surface s; s.w = w; s.h = h; s.pitch = (w + 3) & ~3u; s.caps = caps; s.primary = primary;
    s.pixels = wx86_scratch_alloc(s.pitch * h);
    if (!s.pixels) { log("[RAM-bound] [cause: scratch exhaustion] surface %ux%u", w, h); return 0; }
    std::vector<uint8_t> z(s.pitch * h, 0); c.write(s.pixels, z.data(), (uint32_t)z.size());
    g_surf[o] = s;
    return o;
}

void fill_desc(Cpu& c, uint32_t d, const Surface& s) {
    uint8_t b[108] = {};
    uint32_t v;
    v = 108; std::memcpy(b, &v, 4);
    v = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20;   // CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT|BACKBUFFERCOUNT
    std::memcpy(b + 4, &v, 4);
    std::memcpy(b + 8, &s.h, 4); std::memcpy(b + 12, &s.w, 4); std::memcpy(b + 16, &s.pitch, 4);
    v = s.back ? 1 : 0; std::memcpy(b + 20, &v, 4);
    std::memcpy(b + 36, &s.pixels, 4);
    v = 32; std::memcpy(b + 72, &v, 4);            // DDPIXELFORMAT.dwSize
    v = 0x40 | 0x20; std::memcpy(b + 76, &v, 4);   // DDPF_RGB | DDPF_PALETTEINDEXED8
    v = 8; std::memcpy(b + 84, &v, 4);             // dwRGBBitCount
    std::memcpy(b + 104, &s.caps, 4);
    c.write(d, b, sizeof b);
}

void do_present(Cpu& c, uint32_t surf) {
    auto it = g_surf.find(surf);
    if (it == g_surf.end()) return;
    const Surface& s = it->second;
    uint32_t palobj = s.palette;
    if (!palobj && g_primary) palobj = g_surf[g_primary].palette;
    static uint32_t gray[256];
    static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) gray[i] = i | i << 8 | i << 16; init = true; }
    const uint32_t* pal = gray;
    auto p = g_pal.find(palobj);
    if (p != g_pal.end()) pal = p->second.entries;
    present(c, s.pixels, s.pitch, s.w, s.h, pal);
}

// 8-bit rectangle copy between surfaces (no stretching, optional source key).
void blit(Cpu& c, Surface& dst, int dx, int dy, Surface& src, int sx, int sy, int w, int h, int key) {
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > (int)dst.w) w = dst.w - dx;
    if (dy + h > (int)dst.h) h = dst.h - dy;
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> row(w), drow(w);
    for (int y = 0; y < h; ++y) {
        c.read(src.pixels + (sy + y) * src.pitch + sx, row.data(), w);
        if (key >= 0) {
            c.read(dst.pixels + (dy + y) * dst.pitch + dx, drow.data(), w);
            for (int x = 0; x < w; ++x) if (row[x] != key) drow[x] = row[x];
            c.write(dst.pixels + (dy + y) * dst.pitch + dx, drow.data(), w);
        } else {
            c.write(dst.pixels + (dy + y) * dst.pitch + dx, row.data(), w);
        }
    }
}

void read_rect(Cpu& c, uint32_t p, int r[4], const Surface& s) {
    if (p) { for (int i = 0; i < 4; ++i) r[i] = (int)c.read_u32(p + 4 * i); }
    else { r[0] = 0; r[1] = 0; r[2] = s.w; r[3] = s.h; }
}

}  // namespace

void install_ddraw(d2rt::Bridge& br) {
    auto nop1 = [](Cpu&) { return 1u; };
    // ---- IDirectDrawPalette -------------------------------------------------------
    g_vt_pal = make_vtable(br, "IDirectDrawPalette", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, [](Cpu& c) { return ++g_pal[c.arg(0)].refs; } },
        { "Release", 1, [](Cpu& c) { auto& p = g_pal[c.arg(0)]; return p.refs ? --p.refs : 0u; } },
        { "GetCaps", 2, [](Cpu& c) { c.write_u32(c.arg(1), 0x4 | 0x8); return DD_OK; } },
        { "GetEntries", 5, [](Cpu& c) -> uint32_t {
            auto& p = g_pal[c.arg(0)]; uint32_t base = c.arg(2), n = c.arg(3);
            if (base + n > 256) return DDERR_INVALIDPARAMS;
            for (uint32_t i = 0; i < n; ++i) {   // PALETTEENTRY = r, g, b, flags
                uint32_t v = p.entries[base + i];
                uint8_t e[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), 0 };
                c.write(c.arg(4) + 4 * i, e, 4);
            }
            return DD_OK; } },
        { "Initialize", 4, [](Cpu&) { return DD_OK; } },
        { "SetEntries", 5, [](Cpu& c) -> uint32_t {
            auto& p = g_pal[c.arg(0)]; uint32_t base = c.arg(2), n = c.arg(3);
            if (base + n > 256) return DDERR_INVALIDPARAMS;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t e[4]; c.read(c.arg(4) + 4 * i, e, 4);
                p.entries[base + i] = e[0] | e[1] << 8 | e[2] << 16;
            }
            return DD_OK; } },
    });

    // ---- IDirectDrawSurface ---------------------------------------------------------
    g_vt_surf = make_vtable(br, "IDirectDrawSurface", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, [](Cpu& c) { return ++g_surf[c.arg(0)].refs; } },
        { "Release", 1, [](Cpu& c) { auto& s = g_surf[c.arg(0)]; return s.refs ? --s.refs : 0u; } },
        { "AddAttachedSurface", 2, nullptr },
        { "AddOverlayDirtyRect", 2, nullptr },
        { "Blt", 6, [](Cpu& c) -> uint32_t {
            auto& dst = g_surf[c.arg(0)];
            int dr[4]; read_rect(c, c.arg(1), dr, dst);
            uint32_t flags = c.arg(4);
            if (flags & 0x400) {   // DDBLT_COLORFILL: DDBLTFX.dwFillColor at +80
                uint8_t col = (uint8_t)c.read_u32(c.arg(5) + 80);
                int w = dr[2] - dr[0];
                std::vector<uint8_t> row(w > 0 ? w : 0, col);
                for (int y = dr[1]; y < dr[3] && w > 0; ++y) c.write(dst.pixels + y * dst.pitch + dr[0], row.data(), w);
                return DD_OK;
            }
            auto it = g_surf.find(c.arg(2));
            if (it == g_surf.end()) return DDERR_INVALIDPARAMS;
            int sr[4]; read_rect(c, c.arg(3), sr, it->second);
            if (sr[2] - sr[0] != dr[2] - dr[0] || sr[3] - sr[1] != dr[3] - dr[1])
                log("[not-bound] [cause: unsupported stretch blit] Blt %dx%d -> %dx%d (copied unscaled)",
                    sr[2] - sr[0], sr[3] - sr[1], dr[2] - dr[0], dr[3] - dr[1]);
            blit(c, dst, dr[0], dr[1], it->second, sr[0], sr[1], sr[2] - sr[0], sr[3] - sr[1], -1);
            if (dst.primary) do_present(c, c.arg(0));
            return DD_OK; } },
        { "BltBatch", 4, nullptr },
        { "BltFast", 6, [](Cpu& c) -> uint32_t {
            auto& dst = g_surf[c.arg(0)];
            auto it = g_surf.find(c.arg(3));
            if (it == g_surf.end()) return DDERR_INVALIDPARAMS;
            int sr[4]; read_rect(c, c.arg(4), sr, it->second);
            blit(c, dst, (int)c.arg(1), (int)c.arg(2), it->second, sr[0], sr[1], sr[2] - sr[0], sr[3] - sr[1], -1);
            if (dst.primary) do_present(c, c.arg(0));
            return DD_OK; } },
        { "DeleteAttachedSurface", 3, nullptr },
        { "EnumAttachedSurfaces", 3, nullptr },
        { "EnumOverlayZOrders", 4, nullptr },
        { "Flip", 3, [](Cpu& c) -> uint32_t {
            auto& s = g_surf[c.arg(0)];
            if (s.back) {   // swap pixel buffers between front and back
                auto& b = g_surf[s.back];
                std::swap(s.pixels, b.pixels);
            }
            do_present(c, c.arg(0));
            return DD_OK; } },
        { "GetAttachedSurface", 3, [](Cpu& c) -> uint32_t {
            auto& s = g_surf[c.arg(0)];
            if (!s.back) return DDERR_NOTFOUND;
            ++g_surf[s.back].refs; c.write_u32(c.arg(2), s.back); return DD_OK; } },
        { "GetBltStatus", 2, [](Cpu&) { return DD_OK; } },
        { "GetCaps", 2, [](Cpu& c) { c.write_u32(c.arg(1), g_surf[c.arg(0)].caps); return DD_OK; } },
        { "GetClipper", 2, nullptr },
        { "GetColorKey", 3, nullptr },
        { "GetDC", 2, [](Cpu& c) { c.write_u32(c.arg(1), 0x87000002u); return DD_OK; } },
        { "GetFlipStatus", 2, [](Cpu&) { return DD_OK; } },
        { "GetOverlayPosition", 3, nullptr },
        { "GetPalette", 2, [](Cpu& c) -> uint32_t {
            uint32_t p = g_surf[c.arg(0)].palette; if (!p) return 0x887600F1u;   // DDERR_NOPALETTEATTACHED
            ++g_pal[p].refs; c.write_u32(c.arg(1), p); return DD_OK; } },
        { "GetPixelFormat", 2, [](Cpu& c) {
            uint8_t b[32] = {}; uint32_t v = 32; std::memcpy(b, &v, 4); v = 0x60; std::memcpy(b + 4, &v, 4);
            v = 8; std::memcpy(b + 12, &v, 4); c.write(c.arg(1), b, 32); return DD_OK; } },
        { "GetSurfaceDesc", 2, [](Cpu& c) { fill_desc(c, c.arg(1), g_surf[c.arg(0)]); return DD_OK; } },
        { "Initialize", 3, [](Cpu&) { return DD_OK; } },
        { "IsLost", 1, [](Cpu&) { return DD_OK; } },
        { "Lock", 5, [](Cpu& c) -> uint32_t {
            auto& s = g_surf[c.arg(0)];
            fill_desc(c, c.arg(2), s);
            if (uint32_t r = c.arg(1)) {   // locked sub-rectangle: point at its first byte
                uint32_t x = c.read_u32(r), y = c.read_u32(r + 4);
                c.write_u32(c.arg(2) + 36, s.pixels + y * s.pitch + x);
            }
            return DD_OK; } },
        { "ReleaseDC", 2, [](Cpu&) { return DD_OK; } },
        { "Restore", 1, [](Cpu&) { return DD_OK; } },
        { "SetClipper", 2, [](Cpu&) { return DD_OK; } },
        { "SetColorKey", 3, [](Cpu&) { return DD_OK; } },
        { "SetOverlayPosition", 3, nullptr },
        { "SetPalette", 2, [](Cpu& c) {
            auto& s = g_surf[c.arg(0)]; s.palette = c.arg(1);
            if (s.palette) ++g_pal[s.palette].refs;
            return DD_OK; } },
        { "Unlock", 2, [](Cpu& c) {
            if (g_surf[c.arg(0)].primary) do_present(c, c.arg(0));
            return DD_OK; } },
        { "UpdateOverlay", 6, nullptr },
        { "UpdateOverlayDisplay", 2, nullptr },
        { "UpdateOverlayZOrder", 3, nullptr },
    });

    // ---- IDirectDraw -----------------------------------------------------------------
    g_vt_dd = make_vtable(br, "IDirectDraw", {
        { "QueryInterface", 3, [](Cpu& c) {
            log("[not-bound] [cause: unsupported interface] IDirectDraw::QueryInterface");
            if (c.arg(2)) c.write_u32(c.arg(2), 0);
            return E_NOINTERFACE; } },
        { "AddRef", 1, nop1 },
        { "Release", 1, [](Cpu&) { return 0u; } },
        { "Compact", 1, [](Cpu&) { return DD_OK; } },
        { "CreateClipper", 4, nullptr },
        { "CreatePalette", 5, [](Cpu& c) -> uint32_t {
            uint32_t o = new_object(g_vt_pal);
            Palette p;
            if (uint32_t e = c.arg(2))
                for (uint32_t i = 0; i < 256; ++i) { uint8_t v[4]; c.read(e + 4 * i, v, 4); p.entries[i] = v[0] | v[1] << 8 | v[2] << 16; }
            g_pal[o] = p;
            c.write_u32(c.arg(3), o);
            return DD_OK; } },
        { "CreateSurface", 4, [](Cpu& c) -> uint32_t {
            uint32_t d = c.arg(1);
            uint32_t flags = c.read_u32(d + 4), caps = c.read_u32(d + 104);
            bool primary = (caps & 0x200) != 0;   // DDSCAPS_PRIMARYSURFACE
            uint32_t w = primary ? g_mode_w : ((flags & 4) ? c.read_u32(d + 12) : g_mode_w);
            uint32_t h = primary ? g_mode_h : ((flags & 2) ? c.read_u32(d + 8) : g_mode_h);
            uint32_t o = new_surface(c, w, h, caps | 0x4000 /* DDSCAPS_VIDEOMEMORY */, primary);
            if (!o) return 0x80070008u;   // DDERR_OUTOFMEMORY
            if (primary) {
                g_primary = o;
                if ((flags & 0x20) && c.read_u32(d + 20)) {   // flipping chain with one back buffer
                    uint32_t b = new_surface(c, w, h, 0x4 /* DDSCAPS_BACKBUFFER */, false);
                    g_surf[o].back = b;
                }
            }
            log("[not-bound] CreateSurface %ux%u caps=0x%08x%s -> 0x%08x", w, h, caps, primary ? " (primary)" : "", o);
            c.write_u32(c.arg(2), o);
            return DD_OK; } },
        { "DuplicateSurface", 3, nullptr },
        { "EnumDisplayModes", 5, nullptr },
        { "EnumSurfaces", 5, nullptr },
        { "FlipToGDISurface", 1, [](Cpu&) { return DD_OK; } },
        { "GetCaps", 3, [](Cpu& c) {
            for (uint32_t p : { c.arg(1), c.arg(2) }) if (p) {
                uint32_t n = c.read_u32(p); std::vector<uint8_t> z(n ? n : 316, 0); c.write(p, z.data(), (uint32_t)z.size());
                c.write_u32(p, n ? n : 316);
                c.write_u32(p + 4, 0x40 | 0x400);   // DDCAPS_BLT | DDCAPS_PALETTE
            }
            return DD_OK; } },
        { "GetDisplayMode", 2, [](Cpu& c) {
            Surface s; s.w = g_mode_w; s.h = g_mode_h; s.pitch = g_mode_w; fill_desc(c, c.arg(1), s); return DD_OK; } },
        { "GetFourCCCodes", 3, [](Cpu& c) { if (c.arg(1)) c.write_u32(c.arg(1), 0); return DD_OK; } },
        { "GetGDISurface", 2, [](Cpu& c) { c.write_u32(c.arg(1), g_primary); return g_primary ? DD_OK : DDERR_NOTFOUND; } },
        { "GetMonitorFrequency", 2, [](Cpu& c) { c.write_u32(c.arg(1), 60); return DD_OK; } },
        { "GetScanLine", 2, [](Cpu& c) { c.write_u32(c.arg(1), 0); return DD_OK; } },
        { "GetVerticalBlankStatus", 2, [](Cpu& c) { c.write_u32(c.arg(1), 1); return DD_OK; } },
        { "Initialize", 2, [](Cpu&) { return DD_OK; } },
        { "RestoreDisplayMode", 1, [](Cpu&) { return DD_OK; } },
        { "SetCooperativeLevel", 3, [](Cpu&) { return DD_OK; } },
        { "SetDisplayMode", 4, [](Cpu& c) -> uint32_t {
            g_mode_w = c.arg(1); g_mode_h = c.arg(2); g_mode_bpp = c.arg(3);
            log("[not-bound] SetDisplayMode %ux%ux%u", g_mode_w, g_mode_h, g_mode_bpp);
            if (g_mode_bpp != 8) log("[not-bound] [cause: unsupported mode] only 8-bit is presented");
            return DD_OK; } },
        { "WaitForVerticalBlank", 3, [](Cpu&) { return DD_OK; } },
    });

    // ---- entry points ------------------------------------------------------------------
    Shim s; s.argc = 3; s.stdcall_cleanup = true; s.tag = "DDRAW.dll!DirectDrawCreate";
    s.fn = [](Cpu& c) -> uint32_t {
        if (!g_dd) g_dd = new_object(g_vt_dd);
        c.write_u32(c.arg(1), g_dd);
        log("[not-bound] DirectDrawCreate -> 0x%08x", g_dd);
        return DD_OK;
    };
    br.register_shim("DDRAW.dll", "DirectDrawCreate", s);
}

}  // namespace sc
