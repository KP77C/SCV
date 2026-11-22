// r6_d3d.cpp — DirectDraw (IDirectDraw, 2, 4) and Direct3D 6 (IDirect3D3,
// IDirect3DDevice3, IDirect3DViewport3, IDirect3DTexture2, materials, vertex
// buffers) for the Rogue Spear: Black Thorn engine, drawn with OpenGL:
//   Vita: vitaGL (fixed-function emulation on sceGxm)
//   host: OSMesa, frames saved by SC_SHOTS
//
// COM objects live in guest memory as { vtbl, id }; every vtable slot is a
// trap into a native method here, `this` is argument 0.
//
// Surfaces keep their pixels in guest memory (Lock hands the game a pointer).
// The render target (the flipping chain's back buffer) is ALSO the GL frame:
//   gl_dirty   the GL frame has drawing that guest memory does not have yet
//   mem_dirty  guest memory was written (Lock/Blt) after the last sync
// Lock of the render target reads the GL frame back first; the next draw or
// Flip uploads guest memory as a full-screen quad first. Blts from a surface
// onto the render target are drawn as textured quads, so menus never read
// the frame back.
//
// The game renders at its own size (640x480 by default); that picture is
// scaled to the GL frame (Vita 960x544: 725x544, pillarboxed).
#include "sc_host.h"
#include "r6_jobs.h"

#include "runtime/guest_scratch.h"
#include "runtime/host_clock.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __vita__
#include <vitaGL.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {

namespace {

constexpr uint32_t DD_OK = 0, E_FAIL = 0x80004005u, E_NOINTERFACE = 0x80004002u,
                   DDERR_INVALIDPARAMS = 0x80070057u, DDERR_NOTFOUND = 0x887600FFu,
                   DDERR_OUTOFMEMORY = 0x8007000Eu, DDERR_NOPALETTEATTACHED = 0x887600F1u,
                   DDERR_NOCOLORKEY = 0x887600D7u, DDERR_NOCLIPLIST = 0x887600CDu,
                   DDERR_UNSUPPORTED = 0x80004001u;

const bool g_trace = std::getenv("R6_TRACE_D3D") != nullptr;

// ---- guest GUIDs ----------------------------------------------------------------
struct Guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
bool guid_is(Cpu& c, uint32_t p, const Guid& g) {
    if (!p) return false;
    uint8_t b[16]; c.read(p, b, 16);
    return std::memcmp(b, &g, 16) == 0;
}
const Guid IID_IDirectDraw        = { 0x6C14DB80, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
const Guid IID_IDirectDraw2       = { 0xB3A6F3E0, 0x2B43, 0x11CF, { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
const Guid IID_IDirectDraw4       = { 0x9c59509a, 0x39bd, 0x11d1, { 0x8c, 0x4a, 0x00, 0xc0, 0x4f, 0xd9, 0x30, 0xc5 } };
const Guid IID_IDirect3D3         = { 0xbb223240, 0xe72b, 0x11d0, { 0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e } };
const Guid IID_IDirectDrawSurface = { 0x6C14DB81, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
const Guid IID_IDirectDrawSurface2= { 0x57805885, 0x6eec, 0x11cf, { 0x94, 0x41, 0xa8, 0x23, 0x03, 0xc1, 0x0e, 0x27 } };
const Guid IID_IDirectDrawSurface3= { 0xDA044E00, 0x69B2, 0x11D0, { 0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB } };
const Guid IID_IDirectDrawSurface4= { 0x0B2B8630, 0xAD35, 0x11D0, { 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } };
const Guid IID_IDirect3DTexture2  = { 0x93281502, 0x8cf8, 0x11d0, { 0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29 } };
const Guid IID_IDirect3DHALDevice = { 0x84E63dE0, 0x46AA, 0x11CF, { 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E } };

std::string guid_str(Cpu& c, uint32_t p) {
    if (!p) return "null";
    Guid g; c.read(p, &g, 16);
    char b[64]; std::snprintf(b, sizeof b, "%08x-%04x-%04x-%02x%02x", g.a, g.b, g.c, g.d[0], g.d[1]);
    return b;
}

// ---- vtables -----------------------------------------------------------------------
struct Method { const char* name; uint32_t argc; std::function<uint32_t(Cpu&)> fn; };

uint32_t make_vtable(d2rt::Bridge& br, const char* iface, const std::vector<Method>& ms) {
    uint32_t vt = wx86_scratch_alloc(4 * (uint32_t)ms.size());
    for (size_t i = 0; i < ms.size(); ++i) {
        Shim s; s.argc = ms[i].argc; s.stdcall_cleanup = true;
        std::string key = std::string(iface) + "::" + ms[i].name;
        s.tag = "d3d.com!" + key;
        auto fn = ms[i].fn;
        if (!fn) {
            std::string k = key;
            fn = [k](Cpu& c) { log("[not-bound] [cause: missing COM method] %s ra=0x%08x", k.c_str(), c.trap_retaddr()); return E_FAIL; };
        }
        if (g_trace) {
            std::string k = key;
            fn = [k, f = fn](Cpu& c) { const uint32_t ra = c.trap_retaddr(); uint32_t r = f(c); log("[d3d] %s(%x,%x,%x,%x) -> 0x%x ra=%08x", k.c_str(), c.arg(1), c.arg(2), c.arg(3), c.arg(4), r, ra); return r; };
        }
        s.fn = fn;
        br.register_shim("d3d.com", key, s);
        g_cpu->write_u32(vt + 4 * (uint32_t)i, br.shim_trap("d3d.com", key));
    }
    return vt;
}

uint32_t new_object(uint32_t vt, uint32_t id) {
    uint32_t o = wx86_scratch_alloc(16);
    g_cpu->write_u32(o, vt); g_cpu->write_u32(o + 4, id);
    return o;
}
uint32_t obj_id(Cpu& c, uint32_t o) { return o ? c.read_u32(o + 4) : 0; }

// ---- state ------------------------------------------------------------------------
struct Fmt {
    uint32_t bpp = 16, flags = 0x40, r = 0xF800, g = 0x07E0, b = 0x001F, a = 0;
    bool pal8() const { return (flags & 0x20) != 0; }
    bool z() const { return (flags & 0x400) != 0; }
};
const Fmt kFmt565;

struct Pal { uint32_t e[256] = {}; uint32_t refs = 1; uint32_t obj = 0; };

struct Surf {
    uint32_t id = 0;
    uint32_t w = 0, h = 0, pitch = 0, caps = 0, caps2 = 0, pixels = 0, refs = 1;
    Fmt fmt;
    Pal* pal = nullptr;
    bool ck = false; uint32_t ck_lo = 0, ck_hi = 0;
    Surf* back = nullptr; Surf* zbuf = nullptr; Surf* mip = nullptr; Surf* top = nullptr;
    int level = 0, mips = 1;
    bool primary = false, is_back = false;
    uint32_t obj1 = 0, obj4 = 0, objt2 = 0;   // guest interface objects (v1, v4, texture2)
    GLuint tex = 0; bool tex_dirty = true; uint32_t uniq = 1;
    uint32_t tex_params = 0xFFFFFFFFu;          // last filter/address key set on the GL texture
    bool dead = false;
};

std::vector<Surf*> g_surfs(1, nullptr);   // id -> surface (0 unused)
std::vector<Pal*>  g_pals(1, nullptr);
Surf* S(Cpu& c, uint32_t obj) { uint32_t id = obj_id(c, obj); return id < g_surfs.size() ? g_surfs[id] : nullptr; }
Pal*  P(Cpu& c, uint32_t obj) { uint32_t id = obj_id(c, obj); return id < g_pals.size() ? g_pals[id] : nullptr; }

uint32_t g_vt_dd1 = 0, g_vt_dd2 = 0, g_vt_dd4 = 0, g_vt_s1 = 0, g_vt_s4 = 0, g_vt_pal = 0, g_vt_clip = 0,
         g_vt_d3d3 = 0, g_vt_dev3 = 0, g_vt_vp3 = 0, g_vt_tex2 = 0, g_vt_mat3 = 0, g_vt_light = 0, g_vt_vb = 0;
uint32_t g_dd1 = 0, g_dd2 = 0, g_dd4 = 0, g_d3d3 = 0, g_dev3 = 0;
uint32_t g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
Surf* g_primary = nullptr;
Surf* g_rt = nullptr;            // render target (back buffer)
bool g_gl_dirty = false, g_mem_dirty = false;
uint64_t g_part_rb = 0;
uint64_t g_flips = 0, g_draw_calls = 0, g_tris = 0, g_readbacks = 0, g_uploads = 0, g_blt_gl = 0, g_blt_mem = 0, g_tex_uploads = 0;

uint32_t surf_obj(Cpu& c, Surf* s, bool v4) {
    uint32_t& o = v4 ? s->obj4 : s->obj1;
    if (!o) o = new_object(v4 ? g_vt_s4 : g_vt_s1, s->id);
    (void)c; return o;
}
uint32_t tex2_obj(Surf* s) {
    if (!s->objt2) s->objt2 = new_object(g_vt_tex2, s->id);
    return s->objt2;
}

// Mask -> shift and width, for format conversion.
struct Ch { int shift = 0, bits = 0; };
Ch ch_of(uint32_t m) { Ch r; if (!m) return r; while (!(m & 1)) { m >>= 1; ++r.shift; } while (m & 1) { m >>= 1; ++r.bits; } return r; }
inline uint32_t expand(uint32_t v, Ch ch) {
    if (!ch.bits) return 255;
    v = (v >> ch.shift) & ((1u << ch.bits) - 1);
    if (ch.bits >= 8) return v >> (ch.bits - 8);
    return (v << (8 - ch.bits)) | (v >> (2 * ch.bits - 8 > 0 ? 2 * ch.bits - 8 : 0)) ;
}
inline uint32_t pack(uint32_t r8, uint32_t g8, uint32_t b8, uint32_t a8, const Fmt& f) {
    auto put = [](uint32_t v8, uint32_t m) -> uint32_t { Ch ch = ch_of(m); if (!ch.bits) return 0; return ((v8 >> (8 - std::min(ch.bits, 8))) << ch.shift) & m; };
    return put(r8, f.r) | put(g8, f.g) | put(b8, f.b) | put(a8, f.a);
}
inline uint32_t read_px(const uint8_t* p, uint32_t bpp) {
    switch (bpp) { case 8: return *p; case 16: return p[0] | p[1] << 8; case 24: return p[0] | p[1] << 8 | p[2] << 16;
                   default: return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
}
inline void write_px(uint8_t* p, uint32_t bpp, uint32_t v) {
    switch (bpp) { case 8: *p = (uint8_t)v; break; case 16: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
                   case 24: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
                   default: std::memcpy(p, &v, 4); }
}

// Rows needed before a loop is split across cores: about 24k pixels.
inline uint32_t big_rows(uint32_t w) { return w ? std::max<uint32_t>(8, 24576u / w) : 0xFFFFFFFFu; }

// Whole surface -> RGBA8 (colour-keyed texels get alpha 0).
void to_rgba(Cpu& c, const Surf& s, std::vector<uint8_t>& out) {
    out.resize((size_t)s.w * s.h * 4);
    const uint8_t* src = (const uint8_t*)c.hostptr(s.pixels, s.pitch * s.h);
    if (!src) return;
    const uint32_t bpp = s.fmt.bpp, bytes = bpp / 8;
    if (s.fmt.pal8()) {
        const uint32_t* pe = s.pal ? s.pal->e : nullptr;
        par_rows(s.h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const uint8_t* row = src + y * s.pitch; uint8_t* o = &out[(size_t)y * s.w * 4];
            for (uint32_t x = 0; x < s.w; ++x) {
                uint32_t i = row[x], v = pe ? pe[i] : (i | i << 8 | i << 16);
                o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16);
                o[3] = (s.ck && i >= s.ck_lo && i <= s.ck_hi) ? 0 : 255; o += 4;
            }
        }
        }, big_rows(s.w));
        return;
    }
    const Ch r = ch_of(s.fmt.r), g = ch_of(s.fmt.g), b = ch_of(s.fmt.b), a = ch_of(s.fmt.a);
    const bool fast565 = bpp == 16 && s.fmt.r == 0xF800 && s.fmt.g == 0x07E0 && s.fmt.b == 0x1F;
    par_rows(s.h, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t* row = src + y * s.pitch; uint8_t* o = &out[(size_t)y * s.w * 4];
        for (uint32_t x = 0; x < s.w; ++x, row += bytes, o += 4) {
            const uint32_t v = read_px(row, bpp);
            if (fast565) {
                const uint32_t r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
                o[0] = (uint8_t)(r5 << 3 | r5 >> 2); o[1] = (uint8_t)(g6 << 2 | g6 >> 4); o[2] = (uint8_t)(b5 << 3 | b5 >> 2); o[3] = 255;
            } else {
                o[0] = (uint8_t)expand(v, r); o[1] = (uint8_t)expand(v, g); o[2] = (uint8_t)expand(v, b);
                o[3] = a.bits ? (uint8_t)expand(v, a) : 255;
            }
            if (s.ck) { const uint32_t k = v & (s.fmt.r | s.fmt.g | s.fmt.b); if (k >= s.ck_lo && k <= s.ck_hi) o[3] = 0; }
        }
    }
    }, big_rows(s.w));
}

Surf* new_surf(Cpu& c, uint32_t w, uint32_t h, const Fmt& f, uint32_t caps) {
    Surf* s = new Surf();
    s->id = (uint32_t)g_surfs.size(); g_surfs.push_back(s);
    s->w = w ? w : 1; s->h = h ? h : 1; s->fmt = f; s->caps = caps;
    const uint32_t bytes = f.z() ? f.bpp / 8 : (f.bpp + 7) / 8;
    s->pitch = (s->w * bytes + 3) & ~3u;
    s->pixels = surf_alloc(s->pitch * s->h);
    if (!s->pixels) { log("[RAM-bound] [cause: VA arena refused] surface %ux%u", w, h); return s; }
    if (uint8_t* p = (uint8_t*)c.hostptr(s->pixels, s->pitch * s->h)) std::memset(p, 0, s->pitch * s->h);
    return s;
}

void release_surf(Surf* s) {
    if (!s || s->dead) return;
    s->dead = true;
    if (s->tex) { glDeleteTextures(1, &s->tex); s->tex = 0; }
    surf_free(s->pixels); s->pixels = 0;
    if (s == g_rt) g_rt = nullptr;
    if (s == g_primary) g_primary = nullptr;
    for (Surf* m = s->mip; m; m = m->mip) { m->dead = true; surf_free(m->pixels); m->pixels = 0; }
    if (s->back && s->back != s) release_surf(s->back);
}

void mark_dirty(Surf* s) { Surf* t = s->top ? s->top : s; t->tex_dirty = true; ++t->uniq; }

// ---- GL frame geometry ---------------------------------------------------------
// The game draws into an offscreen framebuffer (texture) of the render
// target's own size, which keeps its picture across Flip as DirectDraw
// does; Flip scales it into the window (Vita 960x544: 725x544 pillarboxed).
uint32_t g_fbw = 640, g_fbh = 480;                    // window framebuffer
int g_sx = 0, g_sy = 0, g_sw = 640, g_sh = 480;       // game picture in the window
int g_ox = 0, g_oy = 0, g_ow = 640, g_oh = 480;       // drawing area in the offscreen frame (= render target)
bool g_gl_ok = false;
GLuint g_fbo = 0, g_fbo_tex = 0, g_fbo_depth = 0;
uint32_t g_fbo_w = 0, g_fbo_h = 0;

void layout() {
    gl_render_size(&g_fbw, &g_fbh);
    const uint32_t rw = g_rt ? g_rt->w : g_mode_w, rh = g_rt ? g_rt->h : g_mode_h;
    double sc = std::min((double)g_fbw / rw, (double)g_fbh / rh);
    g_sw = (int)std::lround(rw * sc); g_sh = (int)std::lround(rh * sc);
    if (g_sw > (int)g_fbw) g_sw = g_fbw; if (g_sh > (int)g_fbh) g_sh = g_fbh;
    g_sx = ((int)g_fbw - g_sw) / 2; g_sy = ((int)g_fbh - g_sh) / 2;
    g_ox = 0; g_oy = 0; g_ow = (int)rw; g_oh = (int)rh;
}
void bind_fbo() {
    const uint32_t rw = g_rt ? g_rt->w : g_mode_w, rh = g_rt ? g_rt->h : g_mode_h;
    if (!g_fbo || g_fbo_w != rw || g_fbo_h != rh) {
        if (g_fbo) { glDeleteFramebuffers(1, &g_fbo); glDeleteTextures(1, &g_fbo_tex); glDeleteRenderbuffers(1, &g_fbo_depth); }
        glGenTextures(1, &g_fbo_tex); glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenRenderbuffers(1, &g_fbo_depth); glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rw, rh);
        glGenFramebuffers(1, &g_fbo); glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_fbo_depth);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_fbo_depth);
        g_fbo_w = rw; g_fbo_h = rh;
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1); glClearDepth(1.0); glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        log("[not-bound] d3d: offscreen frame %ux%u (fbo %u)", rw, rh, g_fbo);
        layout();
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}
bool ensure_gl() {
    if (!g_gl_ok) {
        if (!gl_ensure_context()) return false;
        g_gl_ok = true; layout();
        log("[not-bound] d3d: GL window %ux%u, game picture %dx%d at %d,%d", g_fbw, g_fbh, g_sw, g_sh, g_sx, g_sy);
    }
    bind_fbo();
    return true;
}
// A rectangle in render-target pixels (y down) -> offscreen-frame rectangle (y up).
void rt_rect_to_gl(int x, int y, int w, int h, int* gx, int* gy, int* gw, int* gh) {
    const int rh = g_rt ? (int)g_rt->h : (int)g_mode_h;
    *gx = x; *gy = rh - (y + h); *gw = w; *gh = h;
}

// ---- Direct3D state --------------------------------------------------------------
uint32_t g_rs[256];
uint32_t g_tss[8][32];
uint32_t g_stage_tex[8] = {};       // surface id per stage
float g_world[16], g_view[16], g_proj[16];
struct Viewport { uint32_t x = 0, y = 0, w = 640, h = 480; float minz = 0, maxz = 1; uint32_t bgmat = 0; bool set = false; };
std::vector<Viewport> g_vps(1);
uint32_t g_cur_vp = 0;
struct Material { float diffuse[4] = { 0, 0, 0, 1 }; };
std::vector<Material> g_mats(1);
bool g_legacy_blend = false;        // stage 0 set from TEXTUREMAPBLEND
uint32_t g_tmb = 2;                 // D3DTBLEND_MODULATE
struct VB { uint32_t mem = 0, fvf = 0, n = 0, stride = 0; };
std::vector<VB> g_vbs(1);

void ident(float* m) { std::memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

void reset_state() {
    std::memset(g_rs, 0, sizeof g_rs); std::memset(g_tss, 0, sizeof g_tss);
    g_rs[3] = 1; g_rs[4] = 1; g_rs[7] = 1; g_rs[8] = 3; g_rs[9] = 2; g_rs[14] = 1; g_rs[16] = 1;
    g_rs[17] = 1; g_rs[18] = 1; g_rs[19] = 2; g_rs[20] = 1; g_rs[21] = 2; g_rs[22] = 3; g_rs[23] = 4;
    g_rs[25] = 8; g_rs[44] = 1; g_rs[45] = 1; g_rs[60] = 0xFFFFFFFFu;
    for (int s = 0; s < 8; ++s) {
        g_tss[s][1] = s ? 1 : 4; g_tss[s][2] = 2; g_tss[s][3] = 1;     // COLOROP (stage 0 MODULATE), ARG1 TEXTURE, ARG2 CURRENT
        g_tss[s][4] = s ? 1 : 2; g_tss[s][5] = 2; g_tss[s][6] = 1;     // ALPHAOP (stage 0 SELECTARG1)
        g_tss[s][11] = s; g_tss[s][12] = 1; g_tss[s][13] = 1; g_tss[s][14] = 1;
        g_tss[s][16] = 1; g_tss[s][17] = 1; g_tss[s][18] = 1;
    }
    ident(g_world); ident(g_view); ident(g_proj);
}

Surf* stage_surf(int st) { uint32_t id = g_stage_tex[st]; return id && id < g_surfs.size() && !g_surfs[id]->dead ? g_surfs[id] : nullptr; }

// Upload a texture (its whole mip chain) if guest memory changed.
void upload_tex(Cpu& c, Surf* t) {
    if (!t->tex) { glGenTextures(1, &t->tex); t->tex_dirty = true; t->tex_params = 0xFFFFFFFFu; }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    if (!t->tex_dirty) return;
    static std::vector<uint8_t> buf;
    int lv = 0;
    for (Surf* m = t; m && lv < 12; m = m->mip, ++lv) {
        if (!m->pixels) break;
        to_rgba(c, *m, buf);
        glTexImage2D(GL_TEXTURE_2D, lv, GL_RGBA, m->w, m->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    }
    t->mips = lv;
#ifndef __vita__
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, lv - 1);
#endif
    t->tex_dirty = false; t->tex_params = 0xFFFFFFFFu;
    ++g_tex_uploads;
}

GLenum gl_wrap(uint32_t a) { return a == 3 ? GL_CLAMP_TO_EDGE : a == 2 ? GL_MIRRORED_REPEAT : a == 4 ? GL_CLAMP_TO_EDGE : GL_REPEAT; }

void set_tex_params(Surf* t, int st) {
    const uint32_t mag = g_tss[st][16], min = g_tss[st][17], mip = t->mips > 1 ? g_tss[st][18] : 1;
    const uint32_t au = g_tss[st][13], av = g_tss[st][14];
    const uint32_t key = mag | min << 4 | mip << 8 | au << 12 | av << 16;
    if (key == t->tex_params) return;
    t->tex_params = key;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
    GLenum mf;
    const bool lin = min != 1;
    if (mip == 1 || mip == 0) mf = lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 2) mf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else mf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(au));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(av));
}

// D3D texture argument -> GL combiner source and operand.
void arg_src(uint32_t a, int st, bool alpha, GLint* src, GLint* op) {
    switch (a & 0xF) {
    case 0: *src = GL_PRIMARY_COLOR; break;
    case 1: *src = st ? GL_PREVIOUS : GL_PRIMARY_COLOR; break;
    case 2: *src = GL_TEXTURE; break;
    case 3: *src = GL_CONSTANT; break;
    default: *src = GL_PRIMARY_COLOR; break;
    }
    const bool comp = (a & 0x10) != 0, rep = (a & 0x20) != 0;
    if (alpha || rep) *op = comp ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    else *op = comp ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
}

// One stage's colour or alpha operation as a GL_COMBINE setup.
void combine(int st, bool alpha, uint32_t op, uint32_t a1, uint32_t a2) {
    const GLenum MODE = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    const GLenum SRC[3] = { (GLenum)(alpha ? GL_SRC0_ALPHA : GL_SRC0_RGB), (GLenum)(alpha ? GL_SRC1_ALPHA : GL_SRC1_RGB), (GLenum)(alpha ? GL_SRC2_ALPHA : GL_SRC2_RGB) };
    const GLenum OPR[3] = { (GLenum)(alpha ? GL_OPERAND0_ALPHA : GL_OPERAND0_RGB), (GLenum)(alpha ? GL_OPERAND1_ALPHA : GL_OPERAND1_RGB), (GLenum)(alpha ? GL_OPERAND2_ALPHA : GL_OPERAND2_RGB) };
    const GLenum SCALE = alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE;
    GLint s1, o1, s2, o2; arg_src(a1, st, alpha, &s1, &o1); arg_src(a2, st, alpha, &s2, &o2);
    GLint mode = GL_MODULATE, scale = 1, s3 = GL_PRIMARY_COLOR, o3 = GL_SRC_ALPHA;
    bool three = false, one = false; GLint only_s = s1, only_o = o1;
    switch (op) {
    case 2: mode = GL_REPLACE; one = true; break;
    case 3: mode = GL_REPLACE; one = true; only_s = s2; only_o = o2; break;
    case 4: break;
    case 5: scale = 2; break;
    case 6: scale = 4; break;
    case 7: case 11: mode = GL_ADD; break;
    case 8: mode = GL_ADD_SIGNED; break;
    case 9: mode = GL_ADD_SIGNED; scale = 2; break;
    case 10: mode = GL_SUBTRACT; break;
    case 12: mode = GL_INTERPOLATE; three = true; s3 = GL_PRIMARY_COLOR; break;
    case 13: case 15: mode = GL_INTERPOLATE; three = true; s3 = GL_TEXTURE; break;
    case 14: mode = GL_INTERPOLATE; three = true; s3 = GL_CONSTANT; break;
    case 16: mode = GL_INTERPOLATE; three = true; s3 = st ? GL_PREVIOUS : GL_PRIMARY_COLOR; break;
    default: break;
    }
    glTexEnvi(GL_TEXTURE_ENV, MODE, mode);
    if (one) { glTexEnvi(GL_TEXTURE_ENV, SRC[0], only_s); glTexEnvi(GL_TEXTURE_ENV, OPR[0], only_o); }
    else {
        glTexEnvi(GL_TEXTURE_ENV, SRC[0], s1); glTexEnvi(GL_TEXTURE_ENV, OPR[0], o1);
        glTexEnvi(GL_TEXTURE_ENV, SRC[1], s2); glTexEnvi(GL_TEXTURE_ENV, OPR[1], o2);
        if (three) { glTexEnvi(GL_TEXTURE_ENV, SRC[2], s3); glTexEnvi(GL_TEXTURE_ENV, OPR[2], o3); }
    }
    glTexEnvi(GL_TEXTURE_ENV, SCALE, scale);
}

GLenum gl_blend(uint32_t b, bool src) {
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA; case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR; case 11: return GL_SRC_ALPHA_SATURATE;
    case 12: return src ? GL_SRC_ALPHA : GL_ONE_MINUS_SRC_ALPHA; case 13: return src ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    default: return src ? GL_ONE : GL_ZERO;
    }
}
GLenum gl_cmp(uint32_t f) { return f >= 1 && f <= 8 ? (GLenum)(GL_NEVER + (f - 1)) : GL_ALWAYS; }

// Fixed-function state for the next draw, from the render and stage states.
void apply_state(Cpu& c) {
    // Depth.
    if (g_rs[7]) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(gl_cmp(g_rs[23]));
    glDepthMask(g_rs[14] ? GL_TRUE : GL_FALSE);
    if (g_rs[47]) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -(float)g_rs[47]); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    // Blending.
    if (g_rs[27]) { glEnable(GL_BLEND); glBlendFunc(gl_blend(g_rs[19], true), gl_blend(g_rs[20], false)); }
    else glDisable(GL_BLEND);
    // Culling. D3DCULL_CCW (3) keeps the faces that are clockwise on screen; checked
    // against the game (the tutorial range draws whole with CULL_CCW -> GL_FRONT here).
    if (g_rs[22] == 1) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(g_rs[22] == 3 ? GL_FRONT : GL_BACK); }
    glShadeModel(g_rs[9] == 1 ? GL_FLAT : GL_SMOOTH);
    // Textures, stages 0 and 1.
    Surf* t0 = stage_surf(0);
    bool ck_test = false;
    int active = 0;
    for (int st = 0; st < 2; ++st) {
        glActiveTexture(GL_TEXTURE0 + st);
        Surf* t = stage_surf(st);
        uint32_t cop = g_tss[st][1], aop = g_tss[st][4];
        if (st == 0 && g_legacy_blend) {
            // TEXTUREMAPBLEND: D3D's own translation of the DX5 blend modes.
            const bool ta = t && (t->fmt.a || t->ck || t->fmt.pal8());
            switch (g_tmb) {
            case 1: case 7: cop = 2; aop = 2; break;                                  // DECAL, COPY
            case 3: cop = 13; aop = 3; break;                                          // DECALALPHA
            case 4: cop = 4; aop = 4; break;                                           // MODULATEALPHA
            case 8: cop = 7; aop = 3; break;                                           // ADD
            default: cop = 4; aop = ta ? 2 : 3; break;                                 // MODULATE
            }
            g_tss[0][2] = 2; g_tss[0][3] = 0; g_tss[0][5] = 2; g_tss[0][6] = 0;
        }
        const bool on = active == st && cop != 1 && t;
        if (!on) { glDisable(GL_TEXTURE_2D); continue; }
        upload_tex(c, t);
        set_tex_params(t, st);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        combine(st, false, cop, g_tss[st][2], g_tss[st][3]);
        combine(st, true, aop == 1 ? 3 : aop, g_tss[st][5], g_tss[st][6]);
        if (g_rs[60] != 0xFFFFFFFFu || st == 0) {
            const uint32_t f = g_rs[60];
            GLfloat col[4] = { ((f >> 16) & 255) / 255.f, ((f >> 8) & 255) / 255.f, (f & 255) / 255.f, (f >> 24) / 255.f };
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, col);
        }
        if (st == 0 && t->ck && g_rs[41]) ck_test = true;
        ++active;
    }
    (void)t0;
    glActiveTexture(GL_TEXTURE0);
    // Alpha test (and the colour key: keyed texels have alpha 0).
    if (g_rs[15]) { glEnable(GL_ALPHA_TEST); glAlphaFunc(gl_cmp(g_rs[25]), (g_rs[24] & 255) / 255.f); }
    else if (ck_test) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
    else glDisable(GL_ALPHA_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    // R6_DBG bits, to find what hides geometry: 1 no depth test, 2 no culling, 4 no texture.
    static const unsigned dbg = std::getenv("R6_DBG") ? (unsigned)std::strtoul(std::getenv("R6_DBG"), nullptr, 0) : 0;
    if (dbg & 1) glDisable(GL_DEPTH_TEST);
    if (dbg & 2) glDisable(GL_CULL_FACE);
    if (dbg & 4) { glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D); glActiveTexture(GL_TEXTURE0); glDisable(GL_TEXTURE_2D); }
}

// ---- render-target sync ------------------------------------------------------------
void rt_dirty_all();
// Full-frame read-backs by cause, for the 5 s stats line.
std::map<std::pair<const char*, uint32_t>, uint32_t> g_rb_why;
void rb_note(const char* why, uint32_t ra) { if (g_rb_why.size() < 64 || g_rb_why.count({ why, ra })) ++g_rb_why[{ why, ra }]; }
std::string rb_report() {
    std::string r; char b[96];
    for (auto& e : g_rb_why) { std::snprintf(b, sizeof b, " %s@0x%08x x%u", e.first.first, e.first.second, e.second); r += b; }
    g_rb_why.clear(); return r;
}
void rt_to_mem(Cpu& c, const char* why = "?") {
    if (!g_rt || !g_gl_dirty || !g_gl_ok) return;
    g_gl_dirty = false;
    ++g_readbacks;
    static const bool trace_rb = std::getenv("R6_TRACE_RB") != nullptr;
    if (trace_rb) log("[rb] read-back for %s, ra=0x%08x frame %llu", why, c.read_u32(c.reg(d2rt::R_ESP)), (unsigned long long)g_flips);
    rb_note(why, c.read_u32(c.reg(d2rt::R_ESP)));
    rt_dirty_all();
    static std::vector<uint8_t> px;
    px.resize((size_t)g_ow * g_oh * 4);
    glReadPixels(g_ox, g_oy, g_ow, g_oh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Surf& s = *g_rt;
    uint8_t* dst = (uint8_t*)c.hostptr(s.pixels, s.pitch * s.h);
    if (!dst) return;
    const uint32_t bytes = s.fmt.bpp / 8;
    par_rows(s.h, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
        const uint32_t sy = (uint32_t)((uint64_t)(s.h - 1 - y) * g_oh / s.h);
        const uint8_t* srow = &px[(size_t)sy * g_ow * 4];
        uint8_t* drow = dst + y * s.pitch;
        for (uint32_t x = 0; x < s.w; ++x) {
            const uint8_t* p = srow + (size_t)((uint64_t)x * g_ow / s.w) * 4;
            write_px(drow + x * bytes, s.fmt.bpp, pack(p[0], p[1], p[2], 255, s.fmt));
        }
    }
    }, big_rows(s.w));
}

// Read rectangle sr of the GL frame straight into dst (rect dr), unscaled
// in RT coordinates; nearest sampling from the offscreen frame.
void rt_rect_to(Cpu& c, Surf& dst, const int dr[4], const int sr[4], bool key) {
    Surf& s = *g_rt;
    int x0 = std::max(0, sr[0]), y0 = std::max(0, sr[1]), x1 = std::min((int)s.w, sr[2]), y1 = std::min((int)s.h, sr[3]);
    if (x1 <= x0 || y1 <= y0) return;
    ++g_part_rb;
    // RT rows -> offscreen frame pixels (GL is bottom-up).
    const int fx0 = (int)((int64_t)x0 * g_ow / s.w), fx1 = std::max(fx0 + 1, (int)(((int64_t)x1 * g_ow + s.w - 1) / s.w));
    const int fy0 = (int)((int64_t)(s.h - y1) * g_oh / s.h), fy1 = std::max(fy0 + 1, (int)(((int64_t)(s.h - y0) * g_oh + s.h - 1) / s.h));
    const int fw = fx1 - fx0, fh = fy1 - fy0;
    static std::vector<uint8_t> px; px.resize((size_t)fw * fh * 4);
    glReadPixels(g_ox + fx0, g_oy + fy0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    uint8_t* dp = (uint8_t*)c.hostptr(dst.pixels, dst.pitch * dst.h);
    if (!dp) return;
    const uint32_t db = (dst.fmt.bpp + 7) / 8;
    const int dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        const int dy = dr[1] + y; if (dy < 0 || dy >= (int)dst.h) continue;
        const int sy = sr[1] + (int)((int64_t)y * sh / dh); if (sy < y0 || sy >= y1) continue;
        int fy = (int)((int64_t)(s.h - 1 - sy) * g_oh / s.h) - fy0; fy = std::min(std::max(fy, 0), fh - 1);
        for (int x = 0; x < dw; ++x) {
            const int dx = dr[0] + x; if (dx < 0 || dx >= (int)dst.w) continue;
            const int sx = sr[0] + (int)((int64_t)x * sw / dw); if (sx < x0 || sx >= x1) continue;
            int fx = (int)((int64_t)sx * g_ow / s.w) - fx0; fx = std::min(std::max(fx, 0), fw - 1);
            const uint8_t* p = &px[((size_t)fy * fw + fx) * 4];
            const uint32_t v = pack(p[0], p[1], p[2], 255, s.fmt);
            if (key && v >= g_rt->ck_lo && v <= g_rt->ck_hi) continue;
            uint32_t o = v;
            if (dst.fmt.bpp != s.fmt.bpp || dst.fmt.r != s.fmt.r || dst.fmt.g != s.fmt.g) o = pack(p[0], p[1], p[2], 255, dst.fmt);
            write_px(dp + dy * dst.pitch + dx * db, dst.fmt.bpp, o);
        }
    }
}

// Draw a surface (or a part of it) as a quad over the render target.
void draw_surface_quad(Cpu& c, Surf* src, const int sr[4], const int dr[4], bool keyed) {
    ensure_gl();
    upload_tex(c, src);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    src->tex_params = 0xFFFFFFFFu;
    glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_POLYGON_OFFSET_FILL);
    if (keyed) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); } else glDisable(GL_ALPHA_TEST);
    glViewport(g_ox, g_oy, g_ow, g_oh);
    const float rw = (float)(g_rt ? g_rt->w : g_mode_w), rh = (float)(g_rt ? g_rt->h : g_mode_h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    const float x0 = 2.f * dr[0] / rw - 1, x1 = 2.f * dr[2] / rw - 1, y0 = 1 - 2.f * dr[1] / rh, y1 = 1 - 2.f * dr[3] / rh;
    const float u0 = (float)sr[0] / src->w, u1 = (float)sr[2] / src->w, v0 = (float)sr[1] / src->h, v1 = (float)sr[3] / src->h;
    const GLfloat pos[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    const GLfloat uv[] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4f(1, 1, 1, 1);
    glVertexPointer(2, GL_FLOAT, 0, pos); glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

// 2D mode: when a frame drew no 3D, the next frame's blits to the render target go to
// guest memory, which is uploaded once at present (only the changed rectangle). The menu
// cursor saves the area under it from the back buffer every frame; with the GL frame as
// the master copy that was a full glReadPixels per frame (5 fps on the Vita).
bool g_2d = false;
// In a mission: the last frames drew 3D (the input layer switches to FPS controls).
bool fps_active_impl() {
    static const bool off = std::getenv("R6_NOFPS") && std::getenv("R6_NOFPS")[0] == '1';
    return !off && !g_2d && g_draw_calls > 0;
}
int g_rt_dirty[4] = { 0, 0, 0, 0 };     // changed rectangle of the RT's memory since its last upload
Surf* g_rt_tex_for = nullptr;           // the RT whose GL texture matches its memory outside g_rt_dirty
void rt_dirty_add(const int r[4]) {
    if (!g_rt) return;
    int x0 = std::max(0, r[0]), y0 = std::max(0, r[1]), x1 = std::min((int)g_rt->w, r[2]), y1 = std::min((int)g_rt->h, r[3]);
    if (x1 <= x0 || y1 <= y0) return;
    if (g_rt_dirty[2] <= g_rt_dirty[0]) { g_rt_dirty[0] = x0; g_rt_dirty[1] = y0; g_rt_dirty[2] = x1; g_rt_dirty[3] = y1; return; }
    g_rt_dirty[0] = std::min(g_rt_dirty[0], x0); g_rt_dirty[1] = std::min(g_rt_dirty[1], y0);
    g_rt_dirty[2] = std::max(g_rt_dirty[2], x1); g_rt_dirty[3] = std::max(g_rt_dirty[3], y1);
}
void rt_dirty_all() { g_rt_tex_for = nullptr; }

// The flip chain in 2D mode. The menu cursor keeps one save-under per buffer and expects
// Flip to hand back the buffer shown two flips ago (real page flipping); with one buffer
// it left cursor ghosts. Each entry: guest pixels, GL texture, and that texture's state.
struct ChainBuf { uint32_t px = 0; GLuint tex = 0; bool tex_ok = false; int dirty[4] = { 0, 0, 0, 0 }; };
std::vector<ChainBuf> g_chain;          // [0] is the back buffer's current memory
Surf* g_chain_rt = nullptr;
void chain_rotate() {
    if (!g_rt || g_rt != g_chain_rt || g_chain.size() < 2) return;
    ChainBuf& cur = g_chain[0];
    cur.px = g_rt->pixels; cur.tex = g_rt->tex;
    cur.tex_ok = g_rt_tex_for == g_rt && !g_rt->tex_dirty;
    std::memcpy(cur.dirty, g_rt_dirty, sizeof cur.dirty);
    std::rotate(g_chain.begin(), g_chain.begin() + 1, g_chain.end());
    const ChainBuf& nx = g_chain[0];
    g_rt->pixels = nx.px; g_rt->tex = nx.tex;
    g_rt->tex_dirty = !nx.tex_ok; g_rt->tex_params = 0xFFFFFFFFu; ++g_rt->uniq;
    g_rt_tex_for = nx.tex_ok ? g_rt : nullptr;
    std::memcpy(g_rt_dirty, nx.dirty, sizeof g_rt_dirty);
    g_mem_dirty = true;   // the next present shows this buffer's memory
}

void mem_to_gl(Cpu& c) {
    if (!g_rt || !g_mem_dirty) return;
    g_mem_dirty = false;
    ++g_uploads;
    const int* d = g_rt_dirty;
    if (g_rt_tex_for == g_rt && g_rt->tex && !g_rt->tex_dirty && d[2] > d[0] && d[3] > d[1]) {
        // Only the changed rectangle.
        Surf v = *g_rt;
        const uint32_t bytes = (g_rt->fmt.bpp + 7) / 8;
        v.pixels += (uint32_t)d[1] * g_rt->pitch + (uint32_t)d[0] * bytes;
        v.w = (uint32_t)(d[2] - d[0]); v.h = (uint32_t)(d[3] - d[1]); v.mip = nullptr;
        static std::vector<uint8_t> buf;
        to_rgba(c, v, buf);
        glBindTexture(GL_TEXTURE_2D, g_rt->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, d[0], d[1], (GLsizei)v.w, (GLsizei)v.h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    } else mark_dirty(g_rt);
    const int r[4] = { 0, 0, (int)g_rt->w, (int)g_rt->h };
    draw_surface_quad(c, g_rt, r, r, false);
    g_rt_tex_for = g_rt; g_rt_dirty[0] = g_rt_dirty[1] = g_rt_dirty[2] = g_rt_dirty[3] = 0;
    g_gl_dirty = false;
}

void present_frame(Cpu& c) {
    ensure_gl();
    mem_to_gl(c);
    ++g_flips;
    {   // a frame with no 3D draws puts the next one in 2D mode (R6_NO2D=1 turns it off)
        static const bool off = std::getenv("R6_NO2D") && std::getenv("R6_NO2D")[0] == '1';
        static uint64_t draws_then = 0;
        g_2d = !off && g_draw_calls == draws_then;
        draws_then = g_draw_calls;
    }
    static uint64_t t0 = 0, last_flips = 0, last_draws = 0, last_tris = 0, last_rb = 0, last_up = 0, last_tex = 0;
    const uint64_t now = wx86_now_us(); if (!t0) t0 = now;
    if (now - t0 >= 5000000) {
        log("[not-bound] d3d(5s): %llu flips, %llu draws, %llu triangles, %llu read-backs, %llu frame uploads, %llu texture uploads, blts gl %llu mem %llu, rect read-backs %llu;%s",
            (unsigned long long)(g_flips - last_flips), (unsigned long long)(g_draw_calls - last_draws), (unsigned long long)(g_tris - last_tris),
            (unsigned long long)(g_readbacks - last_rb), (unsigned long long)(g_uploads - last_up), (unsigned long long)(g_tex_uploads - last_tex),
            (unsigned long long)g_blt_gl, (unsigned long long)g_blt_mem, (unsigned long long)g_part_rb, rb_report().c_str());
        t0 = now; last_flips = g_flips; last_draws = g_draw_calls; last_tris = g_tris; last_rb = g_readbacks; last_up = g_uploads; last_tex = g_tex_uploads;
    }
    {
        static const long at = std::getenv("R6_DUMP_AT") ? std::atol(std::getenv("R6_DUMP_AT")) : -1;
        if (at > 0 && g_flips >= 1000 && (long)((g_flips - 1) % at) == 0) {
            uint8_t px[4] = {}; glReadPixels(320, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            log("[dump] present: px(320,300 gl) %02x%02x%02x", px[0], px[1], px[2]);
        }
    }
    // The offscreen frame, scaled into the window.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_fbw, g_fbh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(g_sx, g_sy, g_sw, g_sh);
    glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST);
    glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_POLYGON_OFFSET_FILL);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    {
        const GLfloat pos[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        const GLfloat uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
        glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1, 1, 1, 1);
        glVertexPointer(2, GL_FLOAT, 0, pos); glTexCoordPointer(2, GL_FLOAT, 0, uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    gl_swap(c);
    bind_fbo();   // the game keeps drawing over its last picture, as after a DirectDraw flip
}

// ---- drawing -------------------------------------------------------------------------
struct VSrc {   // one vertex component: base pointer + stride (host)
    const uint8_t* p = nullptr; uint32_t stride = 0;
};
struct VDecl { uint32_t fvf = 0; VSrc pos, diff, spec, tc[2]; bool rhw = false; int ntex = 0; };

uint32_t fvf_fix(uint32_t fvf) {
    if (fvf == 1) return 0x112; if (fvf == 2) return 0x1E2; if (fvf == 3) return 0x1C4;
    return fvf;
}
uint32_t fvf_stride(uint32_t fvf) {
    uint32_t n = 0;
    switch (fvf & 0xE) { case 2: n = 12; break; case 4: n = 16; break; case 6: n = 16; break; case 8: n = 20; break;
                         case 0xA: n = 24; break; case 0xC: n = 28; break; case 0xE: n = 32; break; }
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    n += 8 * ((fvf >> 8) & 0xF);
    return n;
}
VDecl decl_interleaved(const uint8_t* base, uint32_t fvf) {
    VDecl d; d.fvf = fvf; const uint32_t st = fvf_stride(fvf);
    uint32_t o = 0;
    d.pos = { base, st }; d.rhw = (fvf & 0xE) == 4;
    switch (fvf & 0xE) { case 2: o = 12; break; case 4: o = 16; break; case 6: o = 16; break; case 8: o = 20; break;
                         case 0xA: o = 24; break; case 0xC: o = 28; break; case 0xE: o = 32; break; }
    if (fvf & 0x10) o += 12;
    if (fvf & 0x20) o += 4;
    if (fvf & 0x40) { d.diff = { base + o, st }; o += 4; }
    if (fvf & 0x80) { d.spec = { base + o, st }; o += 4; }
    d.ntex = (int)((fvf >> 8) & 0xF);
    for (int i = 0; i < d.ntex; ++i) { if (i < 2) d.tc[i] = { base + o, st }; o += 8; }
    return d;
}

GLenum gl_prim(uint32_t p, uint32_t n, uint32_t* tris) {
    switch (p) {
    case 1: *tris = 0; return GL_POINTS;
    case 2: *tris = 0; return GL_LINES;
    case 3: *tris = 0; return GL_LINE_STRIP;
    case 4: *tris = n / 3; return GL_TRIANGLES;
    case 5: *tris = n > 2 ? n - 2 : 0; return GL_TRIANGLE_STRIP;
    case 6: *tris = n > 2 ? n - 2 : 0; return GL_TRIANGLE_FAN;
    default: *tris = 0; return GL_TRIANGLES;
    }
}

std::map<uint32_t, uint64_t> g_fvf_seen;

void draw(Cpu& c, uint32_t prim, const VDecl& d, uint32_t nverts, const uint16_t* idx, uint32_t nidx) {
    if (!nverts || !g_rt) return;
    if (!ensure_gl()) return;
    if (g_fvf_seen[d.fvf]++ == 0) log("[not-bound] d3d: first draw with vertex format 0x%x (prim %u, %u verts)", d.fvf, prim, nverts);
    mem_to_gl(c);
    apply_state(c);
    static std::vector<float> pos, tc0, tc1; static std::vector<uint8_t> col;
    pos.resize(nverts * 4); col.resize(nverts * 4); tc0.resize(nverts * 2); tc1.resize(nverts * 2);
    for (uint32_t i = 0; i < nverts; ++i) {
        const float* p = (const float*)(d.pos.p + i * d.pos.stride);
        if (d.rhw) {
            float rhw = p[3]; float w = (rhw > 0 && std::isfinite(rhw)) ? 1.f / rhw : 1.f;
            pos[4 * i] = p[0] * w; pos[4 * i + 1] = p[1] * w; pos[4 * i + 2] = p[2] * w; pos[4 * i + 3] = w;
        } else {
            pos[4 * i] = p[0]; pos[4 * i + 1] = p[1]; pos[4 * i + 2] = p[2]; pos[4 * i + 3] = 1;
        }
        uint32_t cd = 0xFFFFFFFFu;
        if (d.diff.p) std::memcpy(&cd, d.diff.p + i * d.diff.stride, 4);
        col[4 * i] = (uint8_t)(cd >> 16); col[4 * i + 1] = (uint8_t)(cd >> 8); col[4 * i + 2] = (uint8_t)cd; col[4 * i + 3] = (uint8_t)(cd >> 24);
        for (int t = 0; t < 2; ++t) {
            std::vector<float>& tc = t ? tc1 : tc0;
            const int src = (int)(g_tss[t][11] & 7);
            const VSrc& s = src < 2 ? d.tc[src] : d.tc[0];
            if (s.p) { const float* q = (const float*)(s.p + i * s.stride); tc[2 * i] = q[0]; tc[2 * i + 1] = q[1]; }
            else { tc[2 * i] = 0; tc[2 * i + 1] = 0; }
        }
    }
    {   // R6_DUMP_AT=<n>: log the draws of every n-th frame (after 1000) (vertex data and the states that hide geometry)
        static const long at = std::getenv("R6_DUMP_AT") ? std::atol(std::getenv("R6_DUMP_AT")) : -1;
        if (at > 0 && g_flips >= 1000 && (long)(g_flips % at) == 0) {   // every <at>-th flip
            log("[dump] prim %u fvf %x verts %u idx %u | zen %u zfn %u zw %u cull %u ablend %u src %u dst %u atest %u aref %x afn %u fog %u tex %d",
                prim, d.fvf, nverts, idx ? nidx : 0, g_rs[7], g_rs[23], g_rs[14], g_rs[22], g_rs[27], g_rs[19], g_rs[20], g_rs[15], g_rs[24], g_rs[25], g_rs[28], (int)g_stage_tex[0]);
            for (uint32_t i = 0; i < nverts && i < 3; ++i) {
                const float* p = (const float*)(d.pos.p + i * d.pos.stride);
                uint32_t cd = 0; if (d.diff.p) std::memcpy(&cd, d.diff.p + i * d.diff.stride, 4);
                log("[dump]   v%u %.2f %.2f %.4f %.4f col %08x uv %.3f %.3f", i, p[0], p[1], p[2], d.rhw ? p[3] : 1.f, cd, tc0[2 * i], tc0[2 * i + 1]);
            }
        }
    }
    const float rw = (float)g_rt->w, rh = (float)g_rt->h;
    if (d.rhw) {
        // Screen pixels (D3D pixel centres on integers) -> clip space; the
        // vertex carries w = 1/rhw so textures are perspective-correct.
        glViewport(g_ox, g_oy, g_ow, g_oh);
        const float m[16] = { 2.f / rw, 0, 0, 0,   0, -2.f / rh, 0, 0,   0, 0, 2, 0,
                              -1.f + 1.f / rw, 1.f - 1.f / rh, -1, 1 };
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glMatrixMode(GL_MODELVIEW); glLoadMatrixf(m);
    } else {
        const Viewport& vp = g_vps[g_cur_vp < g_vps.size() ? g_cur_vp : 0];
        int gx, gy, gw, gh; rt_rect_to_gl((int)vp.x, (int)vp.y, (int)vp.w, (int)vp.h, &gx, &gy, &gw, &gh);
        glViewport(gx, gy, gw, gh);
        // D3D clip z is 0..w, GL's -w..w.
        const float zfix[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 2, 0,  0, 0, -1, 1 };
        glMatrixMode(GL_PROJECTION); glLoadMatrixf(zfix); glMultMatrixf(g_proj);
        glMatrixMode(GL_MODELVIEW); glLoadMatrixf(g_view); glMultMatrixf(g_world);
    }
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(4, GL_FLOAT, 0, pos.data());
    glEnableClientState(GL_COLOR_ARRAY); glColorPointer(4, GL_UNSIGNED_BYTE, 0, col.data());
    glClientActiveTexture(GL_TEXTURE1); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 0, tc1.data());
    glClientActiveTexture(GL_TEXTURE0); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 0, tc0.data());
    uint32_t tris = 0;
    const GLenum mode = gl_prim(prim, idx ? nidx : nverts, &tris);
    if (idx) glDrawElements(mode, (GLsizei)nidx, GL_UNSIGNED_SHORT, idx);
    else glDrawArrays(mode, 0, (GLsizei)nverts);
    glDisableClientState(GL_COLOR_ARRAY);
    ++g_draw_calls; g_tris += tris;
    g_gl_dirty = true;
    {
        static const long at = std::getenv("R6_DUMP_AT") ? std::atol(std::getenv("R6_DUMP_AT")) : -1;
        if (at > 0 && g_flips >= 1000 && (long)(g_flips % at) == 0) {
            uint8_t px[4] = {}; GLint fb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
            glReadPixels(320, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            log("[dump]   after: px(320,300 gl) %02x%02x%02x fb %d err %x", px[0], px[1], px[2], fb, glGetError());
            GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); GLboolean cm[4]; glGetBooleanv(GL_COLOR_WRITEMASK, cm);
            GLfloat mv[16], pj[16], dr[2]; glGetFloatv(GL_MODELVIEW_MATRIX, mv); glGetFloatv(GL_PROJECTION_MATRIX, pj); glGetFloatv(GL_DEPTH_RANGE, dr);
            const float* v = pos.data(); float cl[4];
            for (int r = 0; r < 4; ++r) cl[r] = mv[r] * v[0] + mv[4 + r] * v[1] + mv[8 + r] * v[2] + mv[12 + r] * v[3];
            log("[dump]   vp %d %d %d %d mask %d%d%d%d scis %d sten %d alpha %d blend %d tex0 %d dr %.2f-%.2f clip(v0) %.2f %.2f %.2f %.2f pj00 %.2f",
                vp[0], vp[1], vp[2], vp[3], cm[0], cm[1], cm[2], cm[3], glIsEnabled(GL_SCISSOR_TEST), glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_ALPHA_TEST),
                glIsEnabled(GL_BLEND), glIsEnabled(GL_TEXTURE_2D), dr[0], dr[1], cl[0], cl[1], cl[2], cl[3], pj[0]);
        }
    }
}

// Clear the render target's GL frame (rects in render-target pixels).
void clear_rt(Cpu& c, uint32_t n, uint32_t rects, uint32_t flags, uint32_t color, float z) {
    if (!ensure_gl() || !g_rt) return;
    mem_to_gl(c);
    GLbitfield bits = 0;
    if (flags & 1) { bits |= GL_COLOR_BUFFER_BIT; glClearColor(((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, 1); }
    if (flags & 2) { bits |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); glClearDepth(z); }
    if (!bits) return;
    glEnable(GL_SCISSOR_TEST);
    if (!n || !rects) { glScissor(g_ox, g_oy, g_ow, g_oh); glClear(bits); }
    for (uint32_t i = 0; i < n && rects; ++i) {
        int x1 = (int)c.read_u32(rects + 16 * i), y1 = (int)c.read_u32(rects + 16 * i + 4);
        int x2 = (int)c.read_u32(rects + 16 * i + 8), y2 = (int)c.read_u32(rects + 16 * i + 12);
        int gx, gy, gw, gh; rt_rect_to_gl(x1, y1, x2 - x1, y2 - y1, &gx, &gy, &gw, &gh);
        glScissor(gx, gy, gw, gh); glClear(bits);
    }
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(g_rs[14] ? GL_TRUE : GL_FALSE);
    g_gl_dirty = true;
}

// ---- 2D blits ------------------------------------------------------------------------
void read_rect(Cpu& c, uint32_t p, int r[4], const Surf& s) {
    if (p) { for (int i = 0; i < 4; ++i) r[i] = (int)c.read_u32(p + 4 * i); }
    else { r[0] = 0; r[1] = 0; r[2] = (int)s.w; r[3] = (int)s.h; }
}

void blit_mem(Cpu& c, Surf& dst, const int dr[4], Surf& src, const int sr[4], bool key) {
    const int dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    uint8_t* dp = (uint8_t*)c.hostptr(dst.pixels, dst.pitch * dst.h);
    const uint8_t* sp = (const uint8_t*)c.hostptr(src.pixels, src.pitch * src.h);
    if (!dp || !sp) return;
    const uint32_t db = (dst.fmt.bpp + 7) / 8, sb = (src.fmt.bpp + 7) / 8;
    const bool same = dst.fmt.bpp == src.fmt.bpp && dst.fmt.r == src.fmt.r && dst.fmt.g == src.fmt.g;
    const Ch r = ch_of(src.fmt.r), g = ch_of(src.fmt.g), b = ch_of(src.fmt.b);
    if (same && dw == sw && dh == sh && !src.fmt.pal8()) {   // unscaled, same format: row copies
        int x0 = 0, x1 = dw, y0 = 0, y1 = dh;
        x0 = std::max({ x0, -dr[0], -sr[0] }); x1 = std::min({ x1, (int)dst.w - dr[0], (int)src.w - sr[0] });
        y0 = std::max({ y0, -dr[1], -sr[1] }); y1 = std::min({ y1, (int)dst.h - dr[1], (int)src.h - sr[1] });
        if (x1 <= x0 || y1 <= y0) return;
        par_rows((uint32_t)(y1 - y0), [&](uint32_t r0, uint32_t r1) {
        for (int y = y0 + (int)r0; y < y0 + (int)r1; ++y) {
            uint8_t* d = dp + (dr[1] + y) * dst.pitch + (dr[0] + x0) * db;
            const uint8_t* s = sp + (sr[1] + y) * src.pitch + (sr[0] + x0) * sb;
            if (!key) { std::memmove(d, s, (size_t)(x1 - x0) * db); continue; }
            if (db == 2) {
                const uint16_t* s16 = (const uint16_t*)s; uint16_t* d16 = (uint16_t*)d;
                for (int x = 0; x < x1 - x0; ++x) { const uint32_t v = s16[x]; if (v < src.ck_lo || v > src.ck_hi) d16[x] = (uint16_t)v; }
            } else {
                for (int x = 0; x < x1 - x0; ++x) {
                    const uint32_t v = read_px(s + x * sb, src.fmt.bpp);
                    if (v < src.ck_lo || v > src.ck_hi) write_px(d + x * db, dst.fmt.bpp, v);
                }
            }
        }
        }, dst.pixels == src.pixels ? 0xFFFFFFFFu : big_rows((uint32_t)(x1 - x0)));
        return;
    }
    par_rows((uint32_t)dh, [&](uint32_t r0, uint32_t r1) {
    for (int y = (int)r0; y < (int)r1; ++y) {
        const int dy = dr[1] + y; if (dy < 0 || dy >= (int)dst.h) continue;
        const int sy = sr[1] + (int)((int64_t)y * sh / dh); if (sy < 0 || sy >= (int)src.h) continue;
        for (int x = 0; x < dw; ++x) {
            const int dx = dr[0] + x; if (dx < 0 || dx >= (int)dst.w) continue;
            const int sx = sr[0] + (int)((int64_t)x * sw / dw); if (sx < 0 || sx >= (int)src.w) continue;
            uint32_t v = read_px(sp + sy * src.pitch + sx * sb, src.fmt.bpp);
            if (key && v >= src.ck_lo && v <= src.ck_hi) continue;
            if (!same) {
                uint32_t r8, g8, b8;
                if (src.fmt.pal8() && src.pal) { uint32_t e = src.pal->e[v & 255]; r8 = e & 255; g8 = (e >> 8) & 255; b8 = (e >> 16) & 255; }
                else { r8 = expand(v, r); g8 = expand(v, g); b8 = expand(v, b); }
                v = pack(r8, g8, b8, 255, dst.fmt);
            }
            write_px(dp + dy * dst.pitch + dx * db, dst.fmt.bpp, v);
        }
    }
    }, dst.pixels == src.pixels ? 0xFFFFFFFFu : big_rows((uint32_t)dw));
}

void fill_mem(Cpu& c, Surf& dst, const int dr[4], uint32_t col) {
    uint8_t* dp = (uint8_t*)c.hostptr(dst.pixels, dst.pitch * dst.h);
    if (!dp) return;
    const uint32_t db = (dst.fmt.bpp + 7) / 8;
    const int fy0 = std::max(0, dr[1]), fy1 = std::min((int)dst.h, dr[3]);
    const int fx0 = std::max(0, dr[0]), fx1 = std::min((int)dst.w, dr[2]);
    if (fy1 <= fy0 || fx1 <= fx0) return;
    par_rows((uint32_t)(fy1 - fy0), [&](uint32_t r0, uint32_t r1) {
        for (int y = fy0 + (int)r0; y < fy0 + (int)r1; ++y) {
            uint8_t* row = dp + y * dst.pitch;
            if (db == 2) { uint16_t* p16 = (uint16_t*)row; for (int x = fx0; x < fx1; ++x) p16[x] = (uint16_t)col; }
            else for (int x = fx0; x < fx1; ++x) write_px(row + x * db, dst.fmt.bpp, col);
        }
    }, big_rows((uint32_t)(fx1 - fx0)));
}

// Blt and BltFast: dst/src rects, colour fill, source colour key.
uint32_t do_blt(Cpu& c, Surf* dst, const int dr_in[4], Surf* src, const int sr_in[4], bool key, bool fill, uint32_t col) {
    if (!dst) return DDERR_INVALIDPARAMS;
    int dr[4] = { dr_in[0], dr_in[1], dr_in[2], dr_in[3] };
    if (dst->primary) {   // windowed-style present: the source is the frame
        if (src && src == g_rt) { present_frame(c); return DD_OK; }
        if (src) { g_rt = src; present_frame(c); return DD_OK; }
        return DD_OK;
    }
    const bool to_rt = dst == g_rt;
    if (fill) {
        if (dst->fmt.z()) {   // depth fill
            if (dst == (g_rt ? g_rt->zbuf : nullptr) && ensure_gl()) clear_rt(c, 0, 0, 2, 0, 1.0f);
            return DD_OK;
        }
        if (to_rt && !g_mem_dirty && !g_2d) {
            const uint32_t r8 = expand(col, ch_of(dst->fmt.r)), g8 = expand(col, ch_of(dst->fmt.g)), b8 = expand(col, ch_of(dst->fmt.b));
            uint32_t rect = wx86_scratch_alloc(16);
            c.write_u32(rect, dr[0]); c.write_u32(rect + 4, dr[1]); c.write_u32(rect + 8, dr[2]); c.write_u32(rect + 12, dr[3]);
            clear_rt(c, 1, rect, 1, r8 << 16 | g8 << 8 | b8, 1.0f);
            ++g_blt_gl;
            return DD_OK;
        }
        if (to_rt) rt_to_mem(c, "fill");
        fill_mem(c, *dst, dr, col); if (!to_rt) mark_dirty(dst);
        if (to_rt) { g_mem_dirty = true; rt_dirty_add(dr); }
        ++g_blt_mem;
        return DD_OK;
    }
    if (!src) return DDERR_INVALIDPARAMS;
    int sr[4] = { sr_in[0], sr_in[1], sr_in[2], sr_in[3] };
    if (src == g_rt && dst != g_rt && g_gl_dirty && !g_mem_dirty && !fill && g_gl_ok && !std::getenv("R6_NO_PARTRB")) {
        // The game copies a piece of the frame out (a save-under for a HUD
        // element): read only that rectangle back, the frame stays in GL.
        rt_rect_to(c, *dst, dr, sr, key && src->ck);
        mark_dirty(dst); ++g_blt_mem;
        return DD_OK;
    }
    if (src == g_rt) rt_to_mem(c, "blt from rt");
    if (to_rt && !g_mem_dirty && src != g_rt && !g_2d) {
        draw_surface_quad(c, src, sr, dr, key && src->ck);
        g_gl_dirty = true; ++g_blt_gl;
        return DD_OK;
    }
    if (to_rt) rt_to_mem(c, g_mem_dirty ? "blt, mem dirty" : "blt 2d");
    const bool k = key && src->ck;
    blit_mem(c, *dst, dr, *src, sr, k);
    if (!to_rt) mark_dirty(dst);
    if (to_rt) { g_mem_dirty = true; rt_dirty_add(dr); }
    ++g_blt_mem;
    return DD_OK;
}

// ---- descriptions --------------------------------------------------------------------
void write_pf(Cpu& c, uint32_t p, const Fmt& f) {
    uint8_t b[32] = {}; uint32_t v = 32; std::memcpy(b, &v, 4);
    std::memcpy(b + 4, &f.flags, 4); std::memcpy(b + 12, &f.bpp, 4);
    if (f.z()) { v = f.bpp == 16 ? 0xFFFFu : f.bpp == 24 ? 0xFFFFFFu : 0xFFFFFF00u; std::memcpy(b + 20, &v, 4); }
    else { std::memcpy(b + 16, &f.r, 4); std::memcpy(b + 20, &f.g, 4); std::memcpy(b + 24, &f.b, 4); std::memcpy(b + 28, &f.a, 4); }
    c.write(p, b, 32);
}
Fmt read_pf(Cpu& c, uint32_t p) {
    Fmt f; f.flags = c.read_u32(p + 4); f.bpp = c.read_u32(p + 12);
    if (f.flags & 0x400) { f.r = f.g = f.b = f.a = 0; return f; }
    if (f.flags & 0x20) { f.bpp = 8; f.r = f.g = f.b = f.a = 0; return f; }
    f.r = c.read_u32(p + 16); f.g = c.read_u32(p + 20); f.b = c.read_u32(p + 24);
    f.a = (f.flags & 1) ? c.read_u32(p + 28) : 0;
    return f;
}
void fill_desc(Cpu& c, uint32_t d, const Surf& s, bool locked) {
    const uint32_t sz = c.read_u32(d) == 124 ? 124 : 108;
    std::vector<uint8_t> z(sz, 0); c.write(d, z.data(), sz);
    c.write_u32(d, sz);
    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;   // CAPS HEIGHT WIDTH PITCH PIXELFORMAT
    if (locked) flags |= 0x800;
    if (s.primary) flags |= 0x20;
    if (s.mips > 1 || s.mip) flags |= 0x20000;
    c.write_u32(d + 4, flags);
    c.write_u32(d + 8, s.h); c.write_u32(d + 12, s.w); c.write_u32(d + 16, s.pitch);
    c.write_u32(d + 20, s.primary && s.back ? 1 : 0);
    int n = 0; for (const Surf* m = &s; m; m = m->mip) ++n;
    c.write_u32(d + 24, n);
    c.write_u32(d + 36, locked ? s.pixels : 0);
    if (s.ck) { c.write_u32(d + 64, s.ck_lo); c.write_u32(d + 68, s.ck_hi); }
    write_pf(c, d + 72, s.fmt);
    c.write_u32(d + 104, s.caps);
    if (sz == 124) c.write_u32(d + 108, s.caps2);
}

Fmt display_fmt() {
    Fmt f;
    if (g_mode_bpp == 32) { f.bpp = 32; f.r = 0xFF0000; f.g = 0xFF00; f.b = 0xFF; }
    else if (g_mode_bpp == 8) { f.bpp = 8; f.flags = 0x60; f.r = f.g = f.b = 0; }
    return f;
}

uint32_t create_surface(Cpu& c, uint32_t d, uint32_t out, bool v4) {
    const uint32_t flags = c.read_u32(d + 4), caps = c.read_u32(d + 104);
    const uint32_t caps2 = c.read_u32(d) == 124 ? c.read_u32(d + 108) : 0;
    const bool primary = (caps & 0x200) != 0, zb = (caps & 0x20000) != 0, tex = (caps & 0x1000) != 0;
    uint32_t w = (flags & 4) ? c.read_u32(d + 12) : g_mode_w, h = (flags & 2) ? c.read_u32(d + 8) : g_mode_h;
    if (primary) { w = g_mode_w; h = g_mode_h; }
    Fmt f = display_fmt();
    if (flags & 0x1000) f = read_pf(c, d + 72);
    else if (zb) { f.flags = 0x400; f.bpp = (flags & 0x40) ? c.read_u32(d + 24) : 16; if (!f.bpp) f.bpp = 16; f.r = f.g = f.b = f.a = 0; }
    if (zb && !(f.flags & 0x400)) f.flags = 0x400;
    uint32_t outcaps = caps | (caps & 0x800 ? 0 : 0x4000);   // VIDEOMEMORY unless SYSTEMMEMORY
    if (tex || primary || zb) outcaps |= 0x8000;               // LOCALVIDMEM
    Surf* s = new_surf(c, w, h, f, outcaps);
    if (!s->pixels) return DDERR_OUTOFMEMORY;
    s->caps2 = caps2;
    if (flags & 0x10000) {   // DDSD_CKSRCBLT
        s->ck = true; s->ck_lo = c.read_u32(d + 64); s->ck_hi = c.read_u32(d + 68);
    }
    if (primary) {
        g_primary = s; s->primary = true;
        const uint32_t nb = (flags & 0x20) ? c.read_u32(d + 20) : 0;
        if (nb) {
            Surf* b = new_surf(c, w, h, f, (caps & ~0x200u & ~0x10u) | 0x4 | 0x4000 | 0x2000 /* BACKBUFFER VIDEOMEMORY 3DDEVICE */);
            b->is_back = true; s->back = b;
            // 2D-mode flip chain: the back buffer's memory, the front's, then any extra backs.
            g_chain.assign(1, ChainBuf{}); g_chain[0].px = b->pixels;
            ChainBuf f0; f0.px = s->pixels; g_chain.push_back(f0);
            for (uint32_t i = 1; i < nb && i < 3; ++i) {
                Surf* x = new_surf(c, w, h, f, 0x4 | 0x4000);
                ChainBuf e; e.px = x->pixels; g_chain.push_back(e);
            }
            g_chain_rt = b;
            log("[not-bound] d3d: flip chain of %u buffers", (unsigned)g_chain.size());
        }
        g_rt = s->back ? s->back : s;
        g_gl_dirty = true; g_mem_dirty = false;
        if (g_gl_ok) layout();
    }
    if (tex && (caps & 0x400000) && (flags & 0x20000)) {   // MIPMAP + DDSD_MIPMAPCOUNT
        uint32_t n = c.read_u32(d + 24);
        Surf* prev = s; uint32_t mw = w, mh = h;
        for (uint32_t i = 1; i < n && i < 12; ++i) {
            mw = std::max(1u, mw / 2); mh = std::max(1u, mh / 2);
            Surf* m = new_surf(c, mw, mh, f, outcaps & ~0x200u);
            m->top = s; m->level = (int)i; m->ck = s->ck; m->ck_lo = s->ck_lo; m->ck_hi = s->ck_hi;
            prev->mip = m; prev = m;
        }
        s->mips = (int)n;
    }
    static int logged = 0;
    if (logged < 40 || primary) {
        ++logged;
        log("[not-bound] d3d: CreateSurface %ux%u %ubpp caps=0x%08x%s%s%s -> #%u", w, h, f.bpp, caps,
            primary ? " primary" : "", zb ? " zbuffer" : "", tex ? " texture" : "", s->id);
    }
    c.write_u32(out, surf_obj(c, s, v4));
    return DD_OK;
}

// Callback buffers in guest memory.
uint32_t g_cb_desc = 0, g_cb_pf = 0, g_cb_guid = 0, g_cb_hw = 0, g_cb_hel = 0, g_cb_name = 0, g_cb_descr = 0;

void fill_d3d_desc(Cpu& c, uint32_t p, bool hw) {
    uint32_t sz = c.read_u32(p); if (sz < 252 || sz > 512) sz = 252;
    std::vector<uint8_t> z(sz, 0); c.write(p, z.data(), sz);
    c.write_u32(p, sz);
    if (!hw) return;
    c.write_u32(p + 4, 0x7FF);
    c.write_u32(p + 8, 2);                      // D3DCOLOR_RGB
    c.write_u32(p + 12, 0x1 | 0x10 | 0x40 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x80000);
    c.write_u32(p + 16, 8); c.write_u32(p + 20, 1);   // transform caps: CLIP
    c.write_u32(p + 24, 1);                     // bClipping
    c.write_u32(p + 28, 16); c.write_u32(p + 32, 7); c.write_u32(p + 36, 1); c.write_u32(p + 40, 8);
    for (uint32_t pc : { p + 44u, p + 100u }) {
        c.write_u32(pc, 56);
        c.write_u32(pc + 4, 0x2 | 0x10 | 0x20 | 0x40);                // MASKZ CULLNONE CULLCW CULLCCW
        c.write_u32(pc + 8, 0x1 | 0x10 | 0x20 | 0x80 | 0x100 | 0x4000); // DITHER ZTEST SUBPIXEL FOGVERTEX FOGTABLE ZBIAS
        c.write_u32(pc + 12, 0xFF); c.write_u32(pc + 16, 0x1FFF); c.write_u32(pc + 20, 0x1FFF); c.write_u32(pc + 24, 0xFF);
        c.write_u32(pc + 28, 0x8 | 0x200 | 0x4000 | 0x80000);
        c.write_u32(pc + 32, 0x1 | 0x2 | 0x4 | 0x8);                  // PERSPECTIVE POW2 ALPHA TRANSPARENCY
        c.write_u32(pc + 36, 0x0303033F);
        c.write_u32(pc + 40, 0xFF);
        c.write_u32(pc + 44, 0x7);
    }
    c.write_u32(p + 156, 0x400 | 0x100);        // render: 16, 32 bit
    c.write_u32(p + 160, 0x400 | 0x200 | 0x100);
    c.write_u32(p + 164, 0); c.write_u32(p + 168, 0xFFFF);
    c.write_u32(p + 172, 1); c.write_u32(p + 176, 1); c.write_u32(p + 180, 2048); c.write_u32(p + 184, 2048);
    c.write_u32(p + 204, 2048); c.write_u32(p + 208, 2048); c.write_u32(p + 212, 1);
    c.write_u32(p + 240, 2);                    // FVF caps: 2 texture coordinate sets
    c.write_u32(p + 244, 0x3FFF);
    uint16_t two = 2; c.write(p + 248, &two, 2); c.write(p + 250, &two, 2);
}

// Texture formats offered to the game (EnumTextureFormats).
std::vector<Fmt> tex_formats() {
    std::vector<Fmt> v;
    Fmt f565; v.push_back(f565);
    Fmt f1555; f1555.flags = 0x41; f1555.r = 0x7C00; f1555.g = 0x3E0; f1555.b = 0x1F; f1555.a = 0x8000; v.push_back(f1555);
    Fmt f4444; f4444.flags = 0x41; f4444.r = 0xF00; f4444.g = 0xF0; f4444.b = 0xF; f4444.a = 0xF000; v.push_back(f4444);
    Fmt f8888; f8888.flags = 0x41; f8888.bpp = 32; f8888.r = 0xFF0000; f8888.g = 0xFF00; f8888.b = 0xFF; f8888.a = 0xFF000000u; v.push_back(f8888);
    return v;
}

uint32_t g_clip_hwnd = 0;

}  // namespace

void install_d3d(d2rt::Bridge& br) {
    reset_state();
    g_cb_desc = wx86_scratch_alloc(128); g_cb_pf = wx86_scratch_alloc(32); g_cb_guid = wx86_scratch_alloc(16);
    g_cb_hw = wx86_scratch_alloc(256); g_cb_hel = wx86_scratch_alloc(256);
    g_cb_name = wx86_scratch_put_cstr(*g_cpu, "Direct3D HAL");
    g_cb_descr = wx86_scratch_put_cstr(*g_cpu, "PS Vita GPU (vitaGL)");
    g_cpu->write(g_cb_guid, &IID_IDirect3DHALDevice, 16);

    auto refs_up = [](Cpu&) { return 2u; };
    auto refs_dn = [](Cpu&) { return 1u; };

    // ---- IDirectDrawPalette --------------------------------------------------------
    g_vt_pal = make_vtable(br, "IDirectDrawPalette", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, [](Cpu& c) { Pal* p = P(c, c.arg(0)); return p ? ++p->refs : 0u; } },
        { "Release", 1, [](Cpu& c) { Pal* p = P(c, c.arg(0)); return p && p->refs ? --p->refs : 0u; } },
        { "GetCaps", 2, [](Cpu& c) { c.write_u32(c.arg(1), 0x4 | 0x8); return DD_OK; } },
        { "GetEntries", 5, [](Cpu& c) -> uint32_t {
            Pal* p = P(c, c.arg(0)); uint32_t base = c.arg(2), n = c.arg(3);
            if (!p || base + n > 256) return DDERR_INVALIDPARAMS;
            for (uint32_t i = 0; i < n; ++i) c.write_u32(c.arg(4) + 4 * i, p->e[base + i] & 0xFFFFFF);
            return DD_OK; } },
        { "Initialize", 4, [](Cpu&) { return DD_OK; } },
        { "SetEntries", 5, [](Cpu& c) -> uint32_t {
            Pal* p = P(c, c.arg(0)); uint32_t base = c.arg(2), n = c.arg(3);
            if (!p || base + n > 256) return DDERR_INVALIDPARAMS;
            for (uint32_t i = 0; i < n; ++i) p->e[base + i] = c.read_u32(c.arg(4) + 4 * i) & 0xFFFFFF;
            for (Surf* s : g_surfs) if (s && s->pal == p) mark_dirty(s);
            return DD_OK; } },
    });

    // ---- IDirectDrawClipper --------------------------------------------------------
    g_vt_clip = make_vtable(br, "IDirectDrawClipper", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
        { "GetClipList", 4, [](Cpu&) { return DDERR_NOCLIPLIST; } },
        { "GetHWnd", 2, [](Cpu& c) { c.write_u32(c.arg(1), g_clip_hwnd); return DD_OK; } },
        { "Initialize", 3, [](Cpu&) { return DD_OK; } },
        { "IsClipListChanged", 2, [](Cpu& c) { c.write_u32(c.arg(1), 0); return DD_OK; } },
        { "SetClipList", 3, [](Cpu&) { return DD_OK; } },
        { "SetHWnd", 3, [](Cpu& c) { g_clip_hwnd = c.arg(2); return DD_OK; } },
    });

    // ---- IDirectDrawSurface (v1..v3) and IDirectDrawSurface4 ---------------------
    for (int ver : { 1, 4 }) {
        const bool v4 = ver == 4;
        std::vector<Method> m = {
            { "QueryInterface", 3, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); uint32_t iid = c.arg(1), out = c.arg(2);
                if (!s || !out) return E_NOINTERFACE;
                if (guid_is(c, iid, IID_IDirectDrawSurface4)) { c.write_u32(out, surf_obj(c, s, true)); ++s->refs; return DD_OK; }
                if (guid_is(c, iid, IID_IDirectDrawSurface) || guid_is(c, iid, IID_IDirectDrawSurface2) || guid_is(c, iid, IID_IDirectDrawSurface3)) {
                    c.write_u32(out, surf_obj(c, s, false)); ++s->refs; return DD_OK; }
                if (guid_is(c, iid, IID_IDirect3DTexture2)) { c.write_u32(out, tex2_obj(s)); ++s->refs; return DD_OK; }
                log("[not-bound] [cause: unsupported interface] Surface::QueryInterface %s", guid_str(c, iid).c_str());
                c.write_u32(out, 0); return E_NOINTERFACE; } },
            { "AddRef", 1, [](Cpu& c) { Surf* s = S(c, c.arg(0)); return s ? ++s->refs : 0u; } },
            { "Release", 1, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s || !s->refs) return 0u;
                if (--s->refs == 0 && !s->top && !s->is_back) release_surf(s);
                return s->refs; } },
            { "AddAttachedSurface", 2, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); Surf* a = S(c, c.arg(1));
                if (!s || !a) return DDERR_INVALIDPARAMS;
                if (a->fmt.z()) { s->zbuf = a; ++a->refs; return DD_OK; }
                log("[not-bound] [cause: unsupported attach] AddAttachedSurface #%u to #%u", a->id, s->id);
                return DD_OK; } },
            { "AddOverlayDirtyRect", 2, nullptr },
            { "Blt", 6, [](Cpu& c) -> uint32_t {
                Surf* dst = S(c, c.arg(0)); if (!dst) return DDERR_INVALIDPARAMS;
                Surf* src = c.arg(2) ? S(c, c.arg(2)) : nullptr;
                const uint32_t flags = c.arg(4), fx = c.arg(5);
                int dr[4]; read_rect(c, c.arg(1), dr, *dst);
                int sr[4] = { 0, 0, 0, 0 }; if (src) read_rect(c, c.arg(3), sr, *src);
                if (g_trace) log("[d3d]   blt #%u%s%s [%d,%d,%d,%d] <- #%u%s%s [%d,%d,%d,%d] fl=%x ck=%d", dst->id, dst->primary ? "P" : "", dst == g_rt ? "R" : "",
                                 dr[0], dr[1], dr[2], dr[3], src ? src->id : 0, src && src->primary ? "P" : "", src == g_rt ? "R" : "", sr[0], sr[1], sr[2], sr[3], flags, src ? (int)src->ck : 0);
                if (flags & 0x400) return do_blt(c, dst, dr, nullptr, sr, false, true, fx ? c.read_u32(fx + 80) : 0);
                if (flags & 0x20000) return do_blt(c, dst, dr, nullptr, sr, false, true, 0);   // DEPTHFILL
                return do_blt(c, dst, dr, src, sr, (flags & 0x8000) != 0, false, 0); } },
            { "BltBatch", 4, nullptr },
            { "BltFast", 6, [](Cpu& c) -> uint32_t {
                Surf* dst = S(c, c.arg(0)); Surf* src = S(c, c.arg(3));
                if (!dst || !src) return DDERR_INVALIDPARAMS;
                int sr[4]; read_rect(c, c.arg(4), sr, *src);
                const int x = (int)c.arg(1), y = (int)c.arg(2);
                const int dr[4] = { x, y, x + sr[2] - sr[0], y + sr[3] - sr[1] };
                return do_blt(c, dst, dr, src, sr, (c.arg(5) & 1) != 0, false, 0); } },
            { "DeleteAttachedSurface", 3, [](Cpu& c) { Surf* s = S(c, c.arg(0)); if (s) s->zbuf = nullptr; return DD_OK; } },
            { "EnumAttachedSurfaces", 3, [v4](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                std::vector<Surf*> att; if (s->back) att.push_back(s->back); if (s->zbuf) att.push_back(s->zbuf); if (s->mip) att.push_back(s->mip);
                for (Surf* a : att) {
                    c.write_u32(g_cb_desc, v4 ? 124 : 108); fill_desc(c, g_cb_desc, *a, false);
                    ++a->refs;
                    if (!guest_call(c, c.arg(2), { surf_obj(c, a, v4), g_cb_desc, c.arg(1) })) break;
                }
                return DD_OK; } },
            { "EnumOverlayZOrders", 4, nullptr },
            { "Flip", 3, [](Cpu& c) -> uint32_t {
                static const long bt = std::getenv("R6_FLIP_BT") ? std::atol(std::getenv("R6_FLIP_BT")) : 0;
                if (bt && (long)g_flips == bt) log("[not-bound] flip %ld stack:%s", bt, guest_stack(c).c_str());
                const bool was_2d = g_2d;
                present_frame(c);
                if (was_2d && g_2d) chain_rotate();
                return DD_OK; } },
            { "GetAttachedSurface", 3, [v4](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                const uint32_t caps = c.read_u32(c.arg(1));
                Surf* r = nullptr;
                if (caps & 0x4) r = s->back;                 // BACKBUFFER
                else if (caps & 0x20000) r = s->zbuf;        // ZBUFFER
                else if (caps & 0x400000 || caps & 0x1000) r = s->mip;   // MIPMAP / TEXTURE
                else if (caps & 0x2000) r = s->back ? s->back : nullptr; // 3DDEVICE
                if (!r) return DDERR_NOTFOUND;
                ++r->refs; c.write_u32(c.arg(2), surf_obj(c, r, v4)); return DD_OK; } },
            { "GetBltStatus", 2, [](Cpu&) { return DD_OK; } },
            { "GetCaps", 2, [v4](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                c.write_u32(c.arg(1), s->caps);
                if (v4) { c.write_u32(c.arg(1) + 4, s->caps2); c.write_u32(c.arg(1) + 8, 0); c.write_u32(c.arg(1) + 12, 0); }
                return DD_OK; } },
            { "GetClipper", 2, nullptr },
            { "GetColorKey", 3, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s || !s->ck) return DDERR_NOCOLORKEY;
                c.write_u32(c.arg(2), s->ck_lo); c.write_u32(c.arg(2) + 4, s->ck_hi); return DD_OK; } },
            { "GetDC", 2, [](Cpu& c) -> uint32_t {
                static int n = 0; if (n++ < 5) log("[not-bound] [cause: GDI on a surface] Surface::GetDC ra=0x%08x", c.trap_retaddr());
                return E_FAIL; } },
            { "GetFlipStatus", 2, [](Cpu&) { return DD_OK; } },
            { "GetOverlayPosition", 3, nullptr },
            { "GetPalette", 2, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s || !s->pal) return DDERR_NOPALETTEATTACHED;
                ++s->pal->refs; c.write_u32(c.arg(1), s->pal->obj); return DD_OK; } },
            { "GetPixelFormat", 2, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                write_pf(c, c.arg(1), s->fmt); return DD_OK; } },
            { "GetSurfaceDesc", 2, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                fill_desc(c, c.arg(1), *s, false); return DD_OK; } },
            { "Initialize", 3, [](Cpu&) { return DD_OK; } },
            { "IsLost", 1, [](Cpu&) { return DD_OK; } },
            { "Lock", 5, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s || !s->pixels) return DDERR_INVALIDPARAMS;
                if (s == g_rt) rt_to_mem(c, "lock");
                fill_desc(c, c.arg(2), *s, true);
                if (uint32_t r = c.arg(1)) {
                    uint32_t x = c.read_u32(r), y = c.read_u32(r + 4);
                    c.write_u32(c.arg(2) + 36, s->pixels + y * s->pitch + x * ((s->fmt.bpp + 7) / 8));
                }
                return DD_OK; } },
            { "ReleaseDC", 2, [](Cpu&) { return DD_OK; } },
            { "Restore", 1, [](Cpu&) { return DD_OK; } },
            { "SetClipper", 2, [](Cpu&) { return DD_OK; } },
            { "SetColorKey", 3, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                const uint32_t flags = c.arg(1), k = c.arg(2);
                if (!(flags & 0x8)) return DD_OK;   // only DDCKEY_SRCBLT matters here
                if (!k) s->ck = false; else { s->ck = true; s->ck_lo = c.read_u32(k); s->ck_hi = c.read_u32(k + 4); }
                for (Surf* m = s->mip; m; m = m->mip) { m->ck = s->ck; m->ck_lo = s->ck_lo; m->ck_hi = s->ck_hi; }
                mark_dirty(s);
                return DD_OK; } },
            { "SetOverlayPosition", 3, nullptr },
            { "SetPalette", 2, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                s->pal = c.arg(1) ? P(c, c.arg(1)) : nullptr; if (s->pal) ++s->pal->refs;
                mark_dirty(s); return DD_OK; } },
            { "Unlock", 2, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); if (!s) return DDERR_INVALIDPARAMS;
                mark_dirty(s);
                if (s == g_rt) { g_mem_dirty = true; rt_dirty_all(); }
                if (s->primary && !s->back) present_frame(c);
                return DD_OK; } },
            { "UpdateOverlay", 6, nullptr },
            { "UpdateOverlayDisplay", 2, nullptr },
            { "UpdateOverlayZOrder", 3, nullptr },
            // IDirectDrawSurface2
            { "GetDDInterface", 2, [v4](Cpu& c) { c.write_u32(c.arg(1), v4 ? g_dd4 : g_dd1); return DD_OK; } },
            { "PageLock", 2, [](Cpu&) { return DD_OK; } },
            { "PageUnlock", 2, [](Cpu&) { return DD_OK; } },
            // IDirectDrawSurface3
            { "SetSurfaceDesc", 3, [](Cpu& c) -> uint32_t {
                Surf* s = S(c, c.arg(0)); uint32_t d = c.arg(1);
                if (!s || !d) return DDERR_INVALIDPARAMS;
                if (c.read_u32(d + 4) & 0x800) s->pixels = c.read_u32(d + 36);
                mark_dirty(s); return DD_OK; } },
        };
        if (v4) {
            m.push_back({ "SetPrivateData", 5, [](Cpu&) { return DD_OK; } });
            m.push_back({ "GetPrivateData", 4, [](Cpu&) { return DDERR_NOTFOUND; } });
            m.push_back({ "FreePrivateData", 2, [](Cpu&) { return DD_OK; } });
            m.push_back({ "GetUniquenessValue", 2, [](Cpu& c) { Surf* s = S(c, c.arg(0)); c.write_u32(c.arg(1), s ? s->uniq : 0); return DD_OK; } });
            m.push_back({ "ChangeUniquenessValue", 1, [](Cpu& c) { Surf* s = S(c, c.arg(0)); if (s) ++s->uniq; return DD_OK; } });
        }
        (v4 ? g_vt_s4 : g_vt_s1) = make_vtable(br, v4 ? "IDirectDrawSurface4" : "IDirectDrawSurface", m);
    }

    // ---- IDirect3DTexture2 --------------------------------------------------------------
    g_vt_tex2 = make_vtable(br, "IDirect3DTexture2", {
        { "QueryInterface", 3, [](Cpu& c) -> uint32_t {
            Surf* s = S(c, c.arg(0)); if (!s) return E_NOINTERFACE;
            if (guid_is(c, c.arg(1), IID_IDirectDrawSurface4)) { c.write_u32(c.arg(2), surf_obj(c, s, true)); ++s->refs; return DD_OK; }
            if (guid_is(c, c.arg(1), IID_IDirectDrawSurface)) { c.write_u32(c.arg(2), surf_obj(c, s, false)); ++s->refs; return DD_OK; }
            return E_NOINTERFACE; } },
        { "AddRef", 1, [](Cpu& c) { Surf* s = S(c, c.arg(0)); return s ? ++s->refs : 0u; } },
        { "Release", 1, [](Cpu& c) -> uint32_t {
            Surf* s = S(c, c.arg(0)); if (!s || !s->refs) return 0u;
            if (--s->refs == 0 && !s->top) release_surf(s);
            return s->refs; } },
        { "GetHandle", 3, [](Cpu& c) { Surf* s = S(c, c.arg(0)); c.write_u32(c.arg(2), s ? s->id : 0); return DD_OK; } },
        { "PaletteChanged", 3, [](Cpu& c) { Surf* s = S(c, c.arg(0)); if (s) mark_dirty(s); return DD_OK; } },
        { "Load", 2, [](Cpu& c) -> uint32_t {
            Surf* d = S(c, c.arg(0)); Surf* s = S(c, c.arg(1));
            if (!d || !s) return DDERR_INVALIDPARAMS;
            for (Surf *dm = d, *sm = s; dm && sm; dm = dm->mip, sm = sm->mip) {
                if (dm->w == sm->w && dm->h == sm->h && dm->pitch == sm->pitch && dm->pixels && sm->pixels) {
                    const uint8_t* sp = (const uint8_t*)c.hostptr(sm->pixels, sm->pitch * sm->h);
                    uint8_t* dp = (uint8_t*)c.hostptr(dm->pixels, dm->pitch * dm->h);
                    if (sp && dp) std::memcpy(dp, sp, sm->pitch * sm->h);
                } else {
                    const int r[4] = { 0, 0, (int)sm->w, (int)sm->h }, q[4] = { 0, 0, (int)dm->w, (int)dm->h };
                    blit_mem(c, *dm, q, *sm, r, false);
                }
                if (sm->pal) dm->pal = sm->pal;
            }
            if (s->ck && !d->ck) { d->ck = true; d->ck_lo = s->ck_lo; d->ck_hi = s->ck_hi; }
            mark_dirty(d);
            return DD_OK; } },
    });

    // ---- IDirectDraw, 2, 4 -------------------------------------------------------------
    for (int ver : { 1, 2, 4 }) {
        const bool v4 = ver == 4;
        std::vector<Method> m = {
            { "QueryInterface", 3, [](Cpu& c) -> uint32_t {
                const uint32_t iid = c.arg(1), out = c.arg(2);
                if (!out) return DDERR_INVALIDPARAMS;
                if (guid_is(c, iid, IID_IDirectDraw4)) { c.write_u32(out, g_dd4); return DD_OK; }
                if (guid_is(c, iid, IID_IDirectDraw2)) { c.write_u32(out, g_dd2); return DD_OK; }
                if (guid_is(c, iid, IID_IDirectDraw)) { c.write_u32(out, g_dd1); return DD_OK; }
                if (guid_is(c, iid, IID_IDirect3D3)) { c.write_u32(out, g_d3d3); return DD_OK; }
                log("[not-bound] [cause: unsupported interface] DirectDraw::QueryInterface %s", guid_str(c, iid).c_str());
                c.write_u32(out, 0); return E_NOINTERFACE; } },
            { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
            { "Compact", 1, [](Cpu&) { return DD_OK; } },
            { "CreateClipper", 4, [](Cpu& c) { c.write_u32(c.arg(2), new_object(g_vt_clip, 1)); return DD_OK; } },
            { "CreatePalette", 5, [](Cpu& c) -> uint32_t {
                Pal* p = new Pal(); uint32_t id = (uint32_t)g_pals.size(); g_pals.push_back(p);
                if (uint32_t e = c.arg(2)) for (uint32_t i = 0; i < 256; ++i) p->e[i] = c.read_u32(e + 4 * i) & 0xFFFFFF;
                p->obj = new_object(g_vt_pal, id);
                c.write_u32(c.arg(3), p->obj); return DD_OK; } },
            { "CreateSurface", 4, [v4](Cpu& c) { return create_surface(c, c.arg(1), c.arg(2), v4); } },
            { "DuplicateSurface", 3, nullptr },
            { "EnumDisplayModes", 5, [v4](Cpu& c) -> uint32_t {
                static const uint32_t modes[][3] = { { 640, 480, 16 }, { 800, 600, 16 }, { 1024, 768, 16 }, { 640, 480, 32 }, { 800, 600, 32 }, { 1024, 768, 32 } };
                for (auto& md : modes) {
                    Surf s; s.w = md[0]; s.h = md[1]; s.fmt = Fmt();
                    if (md[2] == 32) { s.fmt.bpp = 32; s.fmt.r = 0xFF0000; s.fmt.g = 0xFF00; s.fmt.b = 0xFF; }
                    s.pitch = s.w * md[2] / 8;
                    c.write_u32(g_cb_desc, v4 ? 124 : 108); fill_desc(c, g_cb_desc, s, false);
                    c.write_u32(g_cb_desc + 4, c.read_u32(g_cb_desc + 4) | 0x40000);   // REFRESHRATE
                    c.write_u32(g_cb_desc + 24, 60);
                    if (!guest_call(c, c.arg(4), { g_cb_desc, c.arg(3) })) break;
                }
                return DD_OK; } },
            { "EnumSurfaces", 5, nullptr },
            { "FlipToGDISurface", 1, [](Cpu&) { return DD_OK; } },
            { "GetCaps", 3, [](Cpu& c) {
                for (uint32_t p : { c.arg(1), c.arg(2) }) if (p) {
                    uint32_t n = c.read_u32(p); if (!n || n > 512) n = 380;
                    std::vector<uint8_t> z(n, 0); c.write(p, z.data(), n); c.write_u32(p, n);
                    c.write_u32(p + 4, 0x1 | 0x40 | 0x200 | 0x400 | 0x40000 | 0x400000 | 0x4000000);   // 3D BLT BLTSTRETCH PALETTE ZBLTS COLORKEY BLTCOLORFILL
                    c.write_u32(p + 8, 0x80000 | 0x200000);   // DDCAPS2_WIDESURFACES | FLIPNOVSYNC
                    c.write_u32(p + 12, 0x200);               // DDCKEYCAPS_SRCBLT
                    c.write_u32(p + 56, 0x400 | 0x200 | 0x100);
                    c.write_u32(p + 60, 64u << 20); c.write_u32(p + 64, 48u << 20);
                    const uint32_t sc = 0x2000 | 0x1000 | 0x20000 | 0x4000 | 0x10 | 0x200 | 0x4 | 0x8 | 0x400000 | 0x40;
                    c.write_u32(p + 132, sc);
                    if (n >= 380) c.write_u32(p + 364, sc);
                }
                return DD_OK; } },
            { "GetDisplayMode", 2, [](Cpu& c) {
                Surf s; s.w = g_mode_w; s.h = g_mode_h; s.fmt = display_fmt(); s.pitch = s.w * s.fmt.bpp / 8;
                fill_desc(c, c.arg(1), s, false); c.write_u32(c.arg(1) + 24, 60); return DD_OK; } },
            { "GetFourCCCodes", 3, [](Cpu& c) { if (c.arg(1)) c.write_u32(c.arg(1), 0); return DD_OK; } },
            { "GetGDISurface", 2, [v4](Cpu& c) -> uint32_t {
                if (!g_primary) return DDERR_NOTFOUND;
                c.write_u32(c.arg(1), surf_obj(c, g_primary, v4)); return DD_OK; } },
            { "GetMonitorFrequency", 2, [](Cpu& c) { c.write_u32(c.arg(1), 60); return DD_OK; } },
            { "GetScanLine", 2, [](Cpu& c) { c.write_u32(c.arg(1), 0); return DD_OK; } },
            { "GetVerticalBlankStatus", 2, [](Cpu& c) { c.write_u32(c.arg(1), 1); return DD_OK; } },
            { "Initialize", 2, [](Cpu&) { return DD_OK; } },
            { "RestoreDisplayMode", 1, [](Cpu&) { return DD_OK; } },
            { "SetCooperativeLevel", 3, [](Cpu&) { return DD_OK; } },
            { "SetDisplayMode", ver == 1 ? 4u : 6u, [](Cpu& c) -> uint32_t {
                g_mode_w = c.arg(1); g_mode_h = c.arg(2); g_mode_bpp = c.arg(3);
                log("[not-bound] d3d: SetDisplayMode %ux%ux%u", g_mode_w, g_mode_h, g_mode_bpp);
                return DD_OK; } },
            { "WaitForVerticalBlank", 3, [](Cpu&) { return DD_OK; } },
        };
        if (ver >= 2) m.push_back({ "GetAvailableVidMem", 4, [](Cpu& c) {
            if (c.arg(2)) c.write_u32(c.arg(2), 64u << 20); if (c.arg(3)) c.write_u32(c.arg(3), 48u << 20); return DD_OK; } });
        if (v4) {
            m.push_back({ "GetSurfaceFromDC", 3, nullptr });
            m.push_back({ "RestoreAllSurfaces", 1, [](Cpu&) { return DD_OK; } });
            m.push_back({ "TestCooperativeLevel", 1, [](Cpu&) { return DD_OK; } });
            m.push_back({ "GetDeviceIdentifier", 3, [](Cpu& c) {
                std::vector<uint8_t> z(1024 + 40, 0); c.write(c.arg(1), z.data(), (uint32_t)z.size());
                const char* drv = "vitagl"; const char* ds = "PS Vita GPU";
                c.write(c.arg(1), drv, (uint32_t)std::strlen(drv)); c.write(c.arg(1) + 512, ds, (uint32_t)std::strlen(ds));
                return DD_OK; } });
        }
        (ver == 1 ? g_vt_dd1 : ver == 2 ? g_vt_dd2 : g_vt_dd4) = make_vtable(br, ver == 1 ? "IDirectDraw" : ver == 2 ? "IDirectDraw2" : "IDirectDraw4", m);
    }

    // ---- IDirect3DMaterial3 --------------------------------------------------------------
    g_vt_mat3 = make_vtable(br, "IDirect3DMaterial3", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
        { "SetMaterial", 2, [](Cpu& c) {
            uint32_t id = obj_id(c, c.arg(0)); if (id < g_mats.size()) for (int i = 0; i < 4; ++i) { uint32_t u = c.read_u32(c.arg(1) + 4 + 4 * i); std::memcpy(&g_mats[id].diffuse[i], &u, 4); }
            return DD_OK; } },
        { "GetMaterial", 2, [](Cpu& c) {
            uint32_t id = obj_id(c, c.arg(0)); if (id < g_mats.size()) for (int i = 0; i < 4; ++i) { uint32_t u; std::memcpy(&u, &g_mats[id].diffuse[i], 4); c.write_u32(c.arg(1) + 4 + 4 * i, u); }
            return DD_OK; } },
        { "GetHandle", 3, [](Cpu& c) { c.write_u32(c.arg(2), obj_id(c, c.arg(0))); return DD_OK; } },
    });
    g_vt_light = make_vtable(br, "IDirect3DLight", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
        { "Initialize", 2, [](Cpu&) { return DD_OK; } },
        { "SetLight", 2, [](Cpu&) { return DD_OK; } },
        { "GetLight", 2, [](Cpu&) { return DD_OK; } },
    });

    // ---- IDirect3DViewport3 ----------------------------------------------------------------
    auto vp_of = [](Cpu& c) -> Viewport& { uint32_t id = obj_id(c, c.arg(0)); return g_vps[id < g_vps.size() ? id : 0]; };
    g_vt_vp3 = make_vtable(br, "IDirect3DViewport3", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
        { "Initialize", 2, [](Cpu&) { return DD_OK; } },
        { "GetViewport", 2, [vp_of](Cpu& c) {
            Viewport& v = vp_of(c); uint32_t p = c.arg(1);
            c.write_u32(p + 4, v.x); c.write_u32(p + 8, v.y); c.write_u32(p + 12, v.w); c.write_u32(p + 16, v.h); return DD_OK; } },
        { "SetViewport", 2, [vp_of](Cpu& c) {
            Viewport& v = vp_of(c); uint32_t p = c.arg(1);
            v.x = c.read_u32(p + 4); v.y = c.read_u32(p + 8); v.w = c.read_u32(p + 12); v.h = c.read_u32(p + 16); v.set = true; return DD_OK; } },
        { "TransformVertices", 5, nullptr },
        { "LightElements", 3, nullptr },
        { "SetBackground", 2, [vp_of](Cpu& c) { vp_of(c).bgmat = c.arg(1); return DD_OK; } },
        { "GetBackground", 3, [vp_of](Cpu& c) { c.write_u32(c.arg(1), vp_of(c).bgmat); if (c.arg(2)) c.write_u32(c.arg(2), 1); return DD_OK; } },
        { "SetBackgroundDepth", 2, [](Cpu&) { return DD_OK; } },
        { "GetBackgroundDepth", 3, [](Cpu&) { return DDERR_NOTFOUND; } },
        { "Clear", 4, [vp_of](Cpu& c) {
            Viewport& v = vp_of(c);
            uint32_t col = 0;
            if (v.bgmat && v.bgmat < g_mats.size()) {
                const float* d = g_mats[v.bgmat].diffuse;
                auto u8 = [](float f) { return (uint32_t)std::lround(std::min(1.f, std::max(0.f, f)) * 255); };
                col = u8(d[0]) << 16 | u8(d[1]) << 8 | u8(d[2]);
            }
            clear_rt(c, c.arg(1), c.arg(2), c.arg(3), col, 1.0f); return DD_OK; } },
        { "AddLight", 2, [](Cpu&) { return DD_OK; } },
        { "DeleteLight", 2, [](Cpu&) { return DD_OK; } },
        { "NextLight", 4, [](Cpu&) { return DDERR_NOTFOUND; } },
        { "GetViewport2", 2, [vp_of](Cpu& c) {
            Viewport& v = vp_of(c); uint32_t p = c.arg(1);
            c.write_u32(p + 4, v.x); c.write_u32(p + 8, v.y); c.write_u32(p + 12, v.w); c.write_u32(p + 16, v.h);
            uint32_t u; std::memcpy(&u, &v.minz, 4); c.write_u32(p + 36, u); std::memcpy(&u, &v.maxz, 4); c.write_u32(p + 40, u); return DD_OK; } },
        { "SetViewport2", 2, [vp_of](Cpu& c) {
            Viewport& v = vp_of(c); uint32_t p = c.arg(1);
            v.x = c.read_u32(p + 4); v.y = c.read_u32(p + 8); v.w = c.read_u32(p + 12); v.h = c.read_u32(p + 16);
            uint32_t u = c.read_u32(p + 36); std::memcpy(&v.minz, &u, 4); u = c.read_u32(p + 40); std::memcpy(&v.maxz, &u, 4);
            v.set = true; return DD_OK; } },
        { "SetBackgroundDepth2", 2, [](Cpu&) { return DD_OK; } },
        { "GetBackgroundDepth2", 3, [](Cpu&) { return DDERR_NOTFOUND; } },
        { "Clear2", 7, [](Cpu& c) {
            uint32_t zu = c.arg(5); float z; std::memcpy(&z, &zu, 4);
            clear_rt(c, c.arg(1), c.arg(2), c.arg(3), c.arg(4), z); return DD_OK; } },
    });

    // ---- IDirect3DVertexBuffer -------------------------------------------------------------
    g_vt_vb = make_vtable(br, "IDirect3DVertexBuffer", {
        { "QueryInterface", 3, [](Cpu&) { return E_NOINTERFACE; } },
        { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
        { "Lock", 4, [](Cpu& c) -> uint32_t {
            uint32_t id = obj_id(c, c.arg(0)); if (id >= g_vbs.size()) return DDERR_INVALIDPARAMS;
            c.write_u32(c.arg(2), g_vbs[id].mem); if (c.arg(3)) c.write_u32(c.arg(3), g_vbs[id].n * g_vbs[id].stride); return DD_OK; } },
        { "Unlock", 1, [](Cpu&) { return DD_OK; } },
        { "ProcessVertices", 8, nullptr },
        { "GetVertexBufferDesc", 2, [](Cpu& c) {
            uint32_t id = obj_id(c, c.arg(0)); if (id >= g_vbs.size()) return DDERR_INVALIDPARAMS;
            c.write_u32(c.arg(1) + 4, 0); c.write_u32(c.arg(1) + 8, g_vbs[id].fvf); c.write_u32(c.arg(1) + 12, g_vbs[id].n); return DD_OK; } },
        { "Optimize", 3, [](Cpu&) { return DD_OK; } },
    });

    // ---- IDirect3D3 --------------------------------------------------------------------------
    g_vt_d3d3 = make_vtable(br, "IDirect3D3", {
        { "QueryInterface", 3, [](Cpu& c) -> uint32_t {
            if (guid_is(c, c.arg(1), IID_IDirectDraw4)) { c.write_u32(c.arg(2), g_dd4); return DD_OK; }
            if (guid_is(c, c.arg(1), IID_IDirect3D3)) { c.write_u32(c.arg(2), g_d3d3); return DD_OK; }
            log("[not-bound] [cause: unsupported interface] IDirect3D3::QueryInterface %s", guid_str(c, c.arg(1)).c_str());
            return E_NOINTERFACE; } },
        { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
        { "EnumDevices", 3, [](Cpu& c) {
            c.write_u32(g_cb_hw, 252); fill_d3d_desc(c, g_cb_hw, true);
            c.write_u32(g_cb_hel, 252); fill_d3d_desc(c, g_cb_hel, false);
            guest_call(c, c.arg(1), { g_cb_guid, g_cb_descr, g_cb_name, g_cb_hw, g_cb_hel, c.arg(2) });
            return DD_OK; } },
        { "CreateLight", 3, [](Cpu& c) { c.write_u32(c.arg(1), new_object(g_vt_light, 1)); return DD_OK; } },
        { "CreateMaterial", 3, [](Cpu& c) {
            uint32_t id = (uint32_t)g_mats.size(); g_mats.emplace_back();
            c.write_u32(c.arg(1), new_object(g_vt_mat3, id)); return DD_OK; } },
        { "CreateViewport", 3, [](Cpu& c) {
            uint32_t id = (uint32_t)g_vps.size(); g_vps.emplace_back();
            c.write_u32(c.arg(1), new_object(g_vt_vp3, id)); return DD_OK; } },
        { "FindDevice", 3, [](Cpu& c) {
            uint32_t r = c.arg(2);
            c.write(r + 4, &IID_IDirect3DHALDevice, 16);
            c.write_u32(r + 20, 252); fill_d3d_desc(c, r + 20, true);
            c.write_u32(r + 20 + 252, 252); fill_d3d_desc(c, r + 20 + 252, false);
            return DD_OK; } },
        { "CreateDevice", 5, [](Cpu& c) -> uint32_t {
            Surf* rt = S(c, c.arg(2));
            if (rt) g_rt = rt;
            log("[not-bound] d3d: CreateDevice %s on surface #%u (%ux%u)", guid_str(c, c.arg(1)).c_str(), rt ? rt->id : 0, rt ? rt->w : 0, rt ? rt->h : 0);
            if (!g_dev3) g_dev3 = new_object(g_vt_dev3, 1);
            reset_state();
            ensure_gl(); layout();
            c.write_u32(c.arg(3), g_dev3); return DD_OK; } },
        { "CreateVertexBuffer", 5, [](Cpu& c) -> uint32_t {
            uint32_t d = c.arg(1); VB vb; vb.fvf = fvf_fix(c.read_u32(d + 8)); vb.n = c.read_u32(d + 12); vb.stride = fvf_stride(vb.fvf);
            vb.mem = surf_alloc(vb.n * vb.stride + 16); if (!vb.mem) return DDERR_OUTOFMEMORY;
            uint32_t id = (uint32_t)g_vbs.size(); g_vbs.push_back(vb);
            c.write_u32(c.arg(2), new_object(g_vt_vb, id)); return DD_OK; } },
        { "EnumZBufferFormats", 4, [](Cpu& c) {
            for (uint32_t bits : { 16u, 24u, 32u }) {
                Fmt f; f.flags = 0x400; f.bpp = bits; write_pf(c, g_cb_pf, f);
                if (!guest_call(c, c.arg(2), { g_cb_pf, c.arg(3) })) break;
            }
            return DD_OK; } },
        { "EvictManagedTextures", 1, [](Cpu&) { return DD_OK; } },
    });

    // ---- IDirect3DDevice3 ------------------------------------------------------------------
    auto dp = [](Cpu& c, uint32_t prim, uint32_t fvf, uint32_t verts, uint32_t n, uint32_t idx, uint32_t nidx) {
        fvf = fvf_fix(fvf);
        const uint32_t st = fvf_stride(fvf);
        const uint8_t* base = (const uint8_t*)c.hostptr(verts, st * n);
        if (!base) return DDERR_INVALIDPARAMS;
        VDecl d = decl_interleaved(base, fvf);
        const uint16_t* ip = idx ? (const uint16_t*)c.hostptr(idx, nidx * 2) : nullptr;
        draw(c, prim, d, n, ip, nidx);
        return DD_OK;
    };
    static std::vector<uint8_t> g_imm;     // Begin/Vertex/End
    static uint32_t g_imm_prim = 0, g_imm_fvf = 0;
    g_vt_dev3 = make_vtable(br, "IDirect3DDevice3", {
        { "QueryInterface", 3, [](Cpu& c) {
            log("[not-bound] [cause: unsupported interface] IDirect3DDevice3::QueryInterface %s", guid_str(c, c.arg(1)).c_str());
            return E_NOINTERFACE; } },
        { "AddRef", 1, refs_up }, { "Release", 1, refs_dn },
        { "GetCaps", 3, [](Cpu& c) { if (c.arg(1)) fill_d3d_desc(c, c.arg(1), true); if (c.arg(2)) fill_d3d_desc(c, c.arg(2), false); return DD_OK; } },
        { "GetStats", 2, [](Cpu& c) { uint32_t n = c.read_u32(c.arg(1)); std::vector<uint8_t> z(n > 4 && n < 64 ? n - 4 : 16, 0); c.write(c.arg(1) + 4, z.data(), (uint32_t)z.size()); return DD_OK; } },
        { "AddViewport", 2, [](Cpu& c) { if (!g_cur_vp) g_cur_vp = obj_id(c, c.arg(1)); return DD_OK; } },
        { "DeleteViewport", 2, [](Cpu&) { return DD_OK; } },
        { "NextViewport", 4, [](Cpu&) { return DDERR_NOTFOUND; } },
        { "EnumTextureFormats", 3, [](Cpu& c) {
            for (const Fmt& f : tex_formats()) { write_pf(c, g_cb_pf, f); if (!guest_call(c, c.arg(1), { g_cb_pf, c.arg(2) })) break; }
            return DD_OK; } },
        { "BeginScene", 1, [](Cpu&) { return DD_OK; } },
        { "EndScene", 1, [](Cpu&) { return DD_OK; } },
        { "GetDirect3D", 2, [](Cpu& c) { c.write_u32(c.arg(1), g_d3d3); return DD_OK; } },
        { "SetCurrentViewport", 2, [](Cpu& c) { g_cur_vp = obj_id(c, c.arg(1)); return DD_OK; } },
        { "GetCurrentViewport", 2, [](Cpu& c) { c.write_u32(c.arg(1), 0); return DD_OK; } },
        { "SetRenderTarget", 3, [](Cpu& c) {
            Surf* s = S(c, c.arg(1));
            if (s && s != g_rt) { log("[not-bound] d3d: SetRenderTarget #%u (%ux%u)", s->id, s->w, s->h); g_rt = s; }
            return DD_OK; } },
        { "GetRenderTarget", 2, [](Cpu& c) { if (g_rt) { ++g_rt->refs; c.write_u32(c.arg(1), surf_obj(c, g_rt, true)); } return DD_OK; } },
        { "Begin", 4, [](Cpu& c) { g_imm.clear(); g_imm_prim = c.arg(1); g_imm_fvf = fvf_fix(c.arg(2)); return DD_OK; } },
        { "BeginIndexed", 6, [](Cpu&) { log("[not-bound] [cause: unsupported] BeginIndexed"); return E_FAIL; } },
        { "Vertex", 2, [](Cpu& c) {
            const uint32_t st = fvf_stride(g_imm_fvf); size_t o = g_imm.size(); g_imm.resize(o + st);
            c.read(c.arg(1), g_imm.data() + o, st); return DD_OK; } },
        { "Index", 2, [](Cpu&) { return E_FAIL; } },
        { "End", 2, [](Cpu& c) {
            const uint32_t st = fvf_stride(g_imm_fvf); if (!st || g_imm.empty()) return DD_OK;
            VDecl d = decl_interleaved(g_imm.data(), g_imm_fvf);
            draw(c, g_imm_prim, d, (uint32_t)(g_imm.size() / st), nullptr, 0); return DD_OK; } },
        { "GetRenderState", 3, [](Cpu& c) { uint32_t s = c.arg(1); c.write_u32(c.arg(2), s < 256 ? g_rs[s] : 0); return DD_OK; } },
        { "SetRenderState", 3, [](Cpu& c) {
            uint32_t s = c.arg(1), v = c.arg(2);
            if (s >= 256) return DD_OK;
            g_rs[s] = v;
            switch (s) {
            case 1: g_stage_tex[0] = v; break;                                       // TEXTUREHANDLE
            case 21: g_tmb = v; g_legacy_blend = true; break;                        // TEXTUREMAPBLEND
            case 17: g_tss[0][16] = v == 1 ? 1 : 2; break;                           // TEXTUREMAG
            case 18: g_tss[0][17] = (v == 1 || v == 3 || v == 4) ? 1 : 2;            // TEXTUREMIN
                     g_tss[0][18] = v <= 2 ? 1 : (v == 3 || v == 5) ? 2 : 3; break;
            case 3: g_tss[0][13] = g_tss[0][14] = v; break;                          // TEXTUREADDRESS
            case 44: g_tss[0][13] = v; break; case 45: g_tss[0][14] = v; break;
            default: break;
            }
            return DD_OK; } },
        { "GetLightState", 3, [](Cpu& c) { c.write_u32(c.arg(2), 0); return DD_OK; } },
        { "SetLightState", 3, [](Cpu&) { return DD_OK; } },
        { "SetTransform", 3, [](Cpu& c) {
            float* m = c.arg(1) == 1 ? g_world : c.arg(1) == 2 ? g_view : c.arg(1) == 3 ? g_proj : nullptr;
            if (m) c.read(c.arg(2), m, 64); return DD_OK; } },
        { "GetTransform", 3, [](Cpu& c) {
            float* m = c.arg(1) == 1 ? g_world : c.arg(1) == 2 ? g_view : c.arg(1) == 3 ? g_proj : nullptr;
            if (m) c.write(c.arg(2), m, 64); return DD_OK; } },
        { "MultiplyTransform", 3, [](Cpu& c) {
            float* m = c.arg(1) == 1 ? g_world : c.arg(1) == 2 ? g_view : c.arg(1) == 3 ? g_proj : nullptr;
            if (!m) return DD_OK;
            float a[16], r[16]; c.read(c.arg(2), a, 64);
            for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float s = 0; for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * m[k * 4 + j]; r[i * 4 + j] = s; }
            std::memcpy(m, r, 64); return DD_OK; } },
        { "DrawPrimitive", 6, [dp](Cpu& c) { return dp(c, c.arg(1), c.arg(2), c.arg(3), c.arg(4), 0, 0); } },
        { "DrawIndexedPrimitive", 8, [dp](Cpu& c) { return dp(c, c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5), c.arg(6)); } },
        { "SetClipStatus", 2, [](Cpu&) { return DD_OK; } },
        { "GetClipStatus", 2, [](Cpu& c) { std::vector<uint8_t> z(24, 0); c.write(c.arg(1), z.data(), 24); return DD_OK; } },
        { "DrawPrimitiveStrided", 6, [](Cpu& c) -> uint32_t {
            const uint32_t fvf = c.arg(2), sd = c.arg(3), n = c.arg(4);
            VDecl d; d.fvf = fvf; d.rhw = (fvf & 0xE) == 4; d.ntex = (int)((fvf >> 8) & 0xF);
            auto comp = [&](uint32_t off) -> VSrc { uint32_t p = c.read_u32(sd + off), s = c.read_u32(sd + off + 4); VSrc v; v.p = p ? (const uint8_t*)c.hostptr(p, s * n + 32) : nullptr; v.stride = s; return v; };
            d.pos = comp(0); if (fvf & 0x40) d.diff = comp(16); if (fvf & 0x80) d.spec = comp(24);
            for (int i = 0; i < d.ntex && i < 2; ++i) d.tc[i] = comp(32 + 8 * i);
            draw(c, c.arg(1), d, n, nullptr, 0); return DD_OK; } },
        { "DrawIndexedPrimitiveStrided", 8, [](Cpu& c) -> uint32_t {
            const uint32_t fvf = c.arg(2), sd = c.arg(3), n = c.arg(4);
            VDecl d; d.fvf = fvf; d.rhw = (fvf & 0xE) == 4; d.ntex = (int)((fvf >> 8) & 0xF);
            auto comp = [&](uint32_t off) -> VSrc { uint32_t p = c.read_u32(sd + off), s = c.read_u32(sd + off + 4); VSrc v; v.p = p ? (const uint8_t*)c.hostptr(p, s * n + 32) : nullptr; v.stride = s; return v; };
            d.pos = comp(0); if (fvf & 0x40) d.diff = comp(16); if (fvf & 0x80) d.spec = comp(24);
            for (int i = 0; i < d.ntex && i < 2; ++i) d.tc[i] = comp(32 + 8 * i);
            const uint16_t* ip = (const uint16_t*)c.hostptr(c.arg(5), c.arg(6) * 2);
            draw(c, c.arg(1), d, n, ip, c.arg(6)); return DD_OK; } },
        { "DrawPrimitiveVB", 6, [dp](Cpu& c) -> uint32_t {
            uint32_t id = obj_id(c, c.arg(2)); if (id >= g_vbs.size()) return DDERR_INVALIDPARAMS;
            const VB& vb = g_vbs[id];
            return dp(c, c.arg(1), vb.fvf, vb.mem + c.arg(3) * vb.stride, c.arg(4), 0, 0); } },
        { "DrawIndexedPrimitiveVB", 6, [dp](Cpu& c) -> uint32_t {
            uint32_t id = obj_id(c, c.arg(2)); if (id >= g_vbs.size()) return DDERR_INVALIDPARAMS;
            const VB& vb = g_vbs[id];
            return dp(c, c.arg(1), vb.fvf, vb.mem, vb.n, c.arg(3), c.arg(4)); } },
        { "ComputeSphereVisibility", 7, [](Cpu& c) { for (uint32_t i = 0; i < c.arg(3); ++i) c.write_u32(c.arg(5) + 4 * i, 0); return DD_OK; } },
        { "GetTexture", 3, [](Cpu& c) {
            Surf* s = c.arg(1) < 8 ? stage_surf((int)c.arg(1)) : nullptr;
            c.write_u32(c.arg(2), s ? tex2_obj(s) : 0); if (s) ++s->refs; return DD_OK; } },
        { "SetTexture", 3, [](Cpu& c) {
            if (c.arg(1) < 8) g_stage_tex[c.arg(1)] = c.arg(2) ? obj_id(c, c.arg(2)) : 0;
            return DD_OK; } },
        { "GetTextureStageState", 4, [](Cpu& c) { uint32_t s = c.arg(1), t = c.arg(2); c.write_u32(c.arg(3), s < 8 && t < 32 ? g_tss[s][t] : 0); return DD_OK; } },
        { "SetTextureStageState", 4, [](Cpu& c) {
            uint32_t s = c.arg(1), t = c.arg(2);
            if (s < 8 && t < 32) {
                g_tss[s][t] = c.arg(3);
                if (s == 0 && t >= 1 && t <= 6) g_legacy_blend = false;
                if (t == 12) { g_tss[s][13] = g_tss[s][14] = c.arg(3); }
            }
            return DD_OK; } },
        { "ValidateDevice", 2, [](Cpu& c) { if (c.arg(1)) c.write_u32(c.arg(1), 1); return DD_OK; } },
    });

    g_dd1 = new_object(g_vt_dd1, 1); g_dd2 = new_object(g_vt_dd2, 1); g_dd4 = new_object(g_vt_dd4, 1);
    g_d3d3 = new_object(g_vt_d3d3, 1);

    // ---- entry points ------------------------------------------------------------------
    {
        Shim s; s.argc = 3; s.stdcall_cleanup = true; s.tag = "DDRAW.dll!DirectDrawCreate";
        s.fn = [](Cpu& c) -> uint32_t {
            c.write_u32(c.arg(1), g_dd1);
            log("[not-bound] d3d: DirectDrawCreate(%s) -> 0x%08x", guid_str(c, c.arg(0)).c_str(), g_dd1);
            return DD_OK;
        };
        br.register_shim("DDRAW.dll", "DirectDrawCreate", s);
    }
    {   // DirectDrawEnumerateA(cb, ctx): one driver, the primary display (GUID NULL).
        Shim s; s.argc = 2; s.stdcall_cleanup = true; s.tag = "DDRAW.dll!DirectDrawEnumerateA";
        s.fn = [](Cpu& c) -> uint32_t {
            static uint32_t d = 0, n = 0;
            if (!d) { d = wx86_scratch_put_cstr(c, "Primary Display Driver"); n = wx86_scratch_put_cstr(c, "display"); }
            guest_call(c, c.arg(0), { 0, d, n, c.arg(1) });
            return DD_OK;
        };
        br.register_shim("DDRAW.dll", "DirectDrawEnumerateA", s);
    }
}

bool r6_fps_active() { return fps_active_impl(); }

}  // namespace sc
