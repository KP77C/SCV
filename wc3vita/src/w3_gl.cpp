// w3_gl.cpp — OPENGL32.dll and the WGL/GDI pixel-format calls for
// WarCraft III's OpenGL renderer (started with -opengl).
//
// The game uses a small, fixed-function GL 1.3 subset: client vertex arrays
// drawn with glDrawElements, textures (BGRA), fixed-function lighting and
// fog, two texture units (GL_ARB_multitexture). Each call is forwarded to a
// real GL with the same entry points:
//   Vita: vitaGL (fixed-function emulation on sceGxm)
//   host: OSMesa (Mesa's off-screen software GL), frames saved by SC_SHOTS
// Guest pointers (arrays, texels, matrices) are read in place: the guest
// arena is one block, so a guest address maps to one host address.
//
// W3_RES=WxH sets the size the game renders at. Host default 800x600. On the
// Vita it must be one of the display's own modes, 960x544 (default), 720x408,
// 640x368 or 480x272: the display hardware scales a smaller framebuffer up to
// the screen for free, so a lower setting saves GPU fill and costs nothing.
#include "sc_host.h"
#include "sc_input.h"

#include "runtime/guest_scratch.h"
#include "runtime/win32_shims_gdi32.h"
#include "runtime/guest_str.h"
#include "runtime/host_clock.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef __vita__
#include <vitaGL.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>
#endif

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {

void present_rgba(d2rt::Cpu& c, const uint8_t* rgba, uint32_t w, uint32_t h);
void present(d2rt::Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h, const uint32_t* pal);

// WarCraft III always draws through OpenGL.
bool present_gl_mode() { return true; }

namespace {

#ifdef __vita__
uint32_t g_w = 960, g_h = 544;
#else
uint32_t g_w = 800, g_h = 600;
#endif
bool     g_current = false;
uint64_t g_calls = 0, g_draws = 0, g_tris = 0, g_swaps = 0;
uint64_t g_sub_kept = 0, g_sub_calls = 0, g_sub_skips = 0, g_sub_skip_bytes = 0;
GlFrameStats g_fs;   // this frame's real texture uploads (spike report)   // texture upload dedupe (below)
#ifndef __vita__
OSMesaContext g_ctx = nullptr;
std::vector<uint8_t> g_fb;
#endif

float    F(Cpu& c, int k) { uint32_t u = c.arg(k); float f; std::memcpy(&f, &u, 4); return f; }
double   D(Cpu& c, int k) { uint64_t u = (uint64_t)c.arg(k) | ((uint64_t)c.arg(k + 1) << 32); double d; std::memcpy(&d, &u, 8); return d; }
template <class T = void>
T*       P(Cpu& c, uint32_t va) { return va ? (T*)c.hostptr(va, 1) : nullptr; }

// W3_GLSTAT=1: per entry point, calls and REDUNDANT calls (arguments, and
// for pointer arguments the data, equal to that entry's previous call; for
// glEnable/glDisable and the client states, the state already set). Logged
// with the 5 s line. Measures what a filter in front of the GL could save.
struct GlStat { std::string name; uint32_t argc; uint64_t calls = 0, same = 0; uint32_t last[9] = {}; uint64_t lasth = 0; };
std::vector<GlStat*> g_stats;
const bool g_glstat = std::getenv("W3_GLSTAT") != nullptr;
std::map<uint32_t, bool> g_caps;          // glEnable/glDisable state (unit-tagged for GL_TEXTURE_2D)
uint32_t g_unit = 0, g_cunit = 0;          // active / client-active texture unit (GL_TEXTURE0 = 0)
uint64_t data_hash(Cpu& c, uint32_t va, uint32_t n) {
    const uint8_t* p = va ? (const uint8_t*)c.hostptr(va, n) : nullptr;
    uint64_t h = 1469598103934665603ull;
    if (p) for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
bool redundant(GlStat& st, Cpu& c) {
    const std::string& n = st.name;
    if (n == "glEnable" || n == "glDisable" || n == "glEnableClientState" || n == "glDisableClientState") {
        const bool on = n == "glEnable" || n == "glEnableClientState";
        const bool client = n.find("Client") != std::string::npos;
        uint32_t key = c.arg(0);
        if (key == GL_TEXTURE_2D) key |= g_unit << 24;
        if (client && key == GL_TEXTURE_COORD_ARRAY) key |= g_cunit << 24;
        if (client) key |= 0x80000000u;
        auto it = g_caps.find(key);
        const bool same = it != g_caps.end() && it->second == on;
        g_caps[key] = on;
        return same;
    }
    if (n == "glActiveTextureARB") g_unit = c.arg(0) - GL_TEXTURE0;
    if (n == "glClientActiveTextureARB") g_cunit = c.arg(0) - GL_TEXTURE0;
    uint64_t h = 0;
    if (n == "glLightfv" || n == "glMaterialfv") h = data_hash(c, c.arg(2), 16);
    else if (n == "glLightModelfv" || n == "glFogfv") h = data_hash(c, c.arg(1), 16);
    else if (n == "glLoadMatrixf") h = data_hash(c, c.arg(0), 64);
    bool same = h == st.lasth;
    const uint32_t k = st.argc < 9 ? st.argc : 9;
    for (uint32_t i = 0; i < k; ++i) { same = same && st.last[i] == c.arg(i); st.last[i] = c.arg(i); }
    st.lasth = h;
    return same && st.calls > 1;
}
void glstat_line() {
    if (!g_glstat) return;
    std::vector<GlStat*> v = g_stats;
    std::sort(v.begin(), v.end(), [](GlStat* a, GlStat* b) { return a->calls > b->calls; });
    std::string line; char b[96];
    for (size_t i = 0; i < v.size() && i < 24 && v[i]->calls; ++i) {
        std::snprintf(b, sizeof b, " %s=%llu(%llu%% same)", v[i]->name.c_str() + 2, (unsigned long long)v[i]->calls,
                      (unsigned long long)(v[i]->same * 100 / v[i]->calls));
        line += b;
    }
    log("[not-bound] gl calls (5s):%s", line.c_str());
    for (GlStat* s : g_stats) { s->calls = 0; s->same = 0; }
}

// Texture units are set lazily (see the filters below): the game's
// glActiveTextureARB / glClientActiveTextureARB only write a guest record,
// and the unit the GL really has is brought in line before any GL call.
uint32_t g_rec_act = 0, g_rec_cact = 0;       // the records (guest VAs); 0 = filters off
uint32_t g_gl_act = GL_TEXTURE0, g_gl_cact = GL_TEXTURE0;   // what the GL has now
inline void sync_units(Cpu& c) {
    if (!g_rec_act || !g_current) return;
    const uint32_t a = c.read_u32(g_rec_act), ca = c.read_u32(g_rec_cact);
    if (a != g_gl_act && a != 0xFFFFFFFFu) { glActiveTexture(a); g_gl_act = a; }
    if (ca != g_gl_cact && ca != 0xFFFFFFFFu) { glClientActiveTexture(ca); g_gl_cact = ca; }
}

void G(d2rt::Bridge& br, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    if (g_glstat) {
        GlStat* st = new GlStat; st->name = name; st->argc = argc; g_stats.push_back(st);
        fn = [st, f = std::move(fn)](Cpu& c) { ++st->calls; if (redundant(*st, c)) ++st->same; return f(c); };
    }
    if (std::getenv("W3_TRACE_GL")) {
        std::string t = name;
        fn = [t, f = std::move(fn)](Cpu& c) { uint32_t r = f(c); log("[not-bound] gl %s(%x,%x,%x,%x) -> 0x%x", t.c_str(), c.arg(0), c.arg(1), c.arg(2), c.arg(3), r); return r; };
    }
    fn = [f = std::move(fn)](Cpu& c) { ++g_calls; sync_units(c); return f(c); };
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string("OPENGL32.dll!") + name;
    br.register_shim("OPENGL32.dll", name, s);
}
void GDI(d2rt::Bridge& br, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string("GDI32.dll!") + name;
    br.register_shim("GDI32.dll", name, s);
}

// The render size from W3_RES; on the Vita only the display's own modes.
void read_render_size() {
    if (const char* r = std::getenv("W3_RES")) { unsigned w, h; if (std::sscanf(r, "%ux%u", &w, &h) == 2) { g_w = w; g_h = h; } }
#ifdef __vita__
    static const uint32_t modes[][2] = { { 960, 544 }, { 720, 408 }, { 640, 368 }, { 480, 272 } };
    bool ok = false;
    for (auto& m : modes) ok |= (g_w == m[0] && g_h == m[1]);
    if (!ok) { log("[not-bound] W3_RES=%ux%u is not a Vita display mode; using 960x544", g_w, g_h); g_w = 960; g_h = 544; }
#endif
}

bool make_current() {
    if (g_current) return true;
#ifdef __vita__
    // vitaGL draws straight into display buffers of the render size; the
    // display scales them to the screen. RAM: the guest arena, our heap and
    // the JIT pool are already allocated; leave 24 MiB for the JIT pool's
    // second segment and system allocations, give vitaGL the rest and VRAM.
    // vglInitExtended's return value is not a failure flag: GL_TRUE only
    // says the size was above the display's maximum and was lowered (the
    // Deus Ex port's build 3 quit at boot by treating GL_FALSE as failure).
    if (vglInitExtended(0x400000, (int)g_w, (int)g_h, 24 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE))
        log("[not-bound] vitaGL lowered the render size %ux%u to the display maximum", g_w, g_h);
    const int interval = fps_lock_interval();   // steady 15 fps by default (K); SC_FPS=20/30/60
    eglSwapInterval(nullptr, interval);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); vglSwapBuffers(GL_FALSE);
    log("[not-bound] GL context: vitaGL %ux%u, vsync every %d vblank(s); free RAM %u KiB, VRAM %u KiB", g_w, g_h, interval,
        (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10));
#else
    g_ctx = OSMesaCreateContextExt(OSMESA_RGBA, 24, 8, 0, nullptr);
    if (!g_ctx) { log("[not-bound] [cause: OSMesa] context creation failed"); return false; }
    g_fb.assign((size_t)g_w * g_h * 4, 0);
    if (!OSMesaMakeCurrent(g_ctx, g_fb.data(), GL_UNSIGNED_BYTE, (GLsizei)g_w, (GLsizei)g_h)) {
        log("[not-bound] [cause: OSMesa] make current failed"); return false;
    }
    log("[not-bound] GL context %ux%u: %s / %s", g_w, g_h, (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
#endif
    g_current = true;
    return true;
}

// Command-card overlay while D-pad up is held: every card button tinted, the
// picked one bright with a frame. Drawn over the game's finished frame. The
// game keeps track of the GL state it has set, so everything changed here is
// put back exactly as it was.
void draw_card_overlay() {
    int sel = 0;
    if (!input_grid_active(&sel)) return;
    GLint vp[4], mode = GL_MODELVIEW, active = GL_TEXTURE0, src = GL_ONE, dst = GL_ZERO;
    glGetIntegerv(GL_VIEWPORT, vp); glGetIntegerv(GL_MATRIX_MODE, &mode); glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_BLEND_SRC, &src); glGetIntegerv(GL_BLEND_DST, &dst);
    GLboolean dmask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    GLfloat col[4] = { 1, 1, 1, 1 }; glGetFloatv(GL_CURRENT_COLOR, col);
    static const GLenum caps[] = { GL_DEPTH_TEST, GL_ALPHA_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_FOG, GL_LIGHTING,
                                   GL_POLYGON_OFFSET_FILL, GL_STENCIL_TEST, GL_BLEND };
    constexpr int kCaps = sizeof caps / sizeof caps[0];
    GLboolean on[kCaps], tex[2];
    for (int i = 0; i < kCaps; ++i) { on[i] = glIsEnabled(caps[i]); if (on[i]) glDisable(caps[i]); }
    for (int u = 0; u < 2; ++u) { glActiveTexture(GL_TEXTURE0 + u); tex[u] = glIsEnabled(GL_TEXTURE_2D); if (tex[u]) glDisable(GL_TEXTURE_2D); }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE);
    glViewport(0, 0, (GLsizei)g_w, (GLsizei)g_h);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, g_w, g_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    auto quad = [](float x, float y, float w, float h) {
        glVertex2f(x, y); glVertex2f(x + w, y); glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    };
    glBegin(GL_QUADS);
    for (int i = 0, n = input_card_cells(); i < n; ++i) {
        int x, y, w, h; input_card_rect(i, &x, &y, &w, &h);
        if (i == sel) {
            glColor4f(1.0f, 0.9f, 0.25f, 0.35f); quad(x, y, w, h);
            const float t = g_h >= 500 ? 3 : 2;   // frame thickness
            glColor4f(1.0f, 0.85f, 0.1f, 0.95f);
            quad(x - t, y - t, w + 2 * t, t); quad(x - t, y + h, w + 2 * t, t);
            quad(x - t, y, t, h); quad(x + w, y, t, h);
        } else {
            glColor4f(1.0f, 1.0f, 1.0f, 0.12f); quad(x, y, w, h);
        }
    }
    glEnd();
    glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(mode);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glColor4f(col[0], col[1], col[2], col[3]);
    glDepthMask(dmask); glBlendFunc(src, dst);
    for (int i = 0; i < kCaps; ++i) { if (on[i]) glEnable(caps[i]); else glDisable(caps[i]); }
    for (int u = 0; u < 2; ++u) if (tex[u]) { glActiveTexture(GL_TEXTURE0 + u); glEnable(GL_TEXTURE_2D); }
    glActiveTexture(active);
}

void swap(Cpu& c) {
    draw_card_overlay();
    ++g_swaps;
    static uint64_t frames = 0, t0 = 0, last_calls = 0, last_draws = 0, last_tris = 0;
    ++frames;
    uint64_t now = wx86_now_us(); if (!t0) t0 = now;
    if (now - t0 >= 5000000) {
        static uint64_t last_mp3 = 0; uint64_t mp3_opens = 0, mp3_frames = 0; mp3_stats(&mp3_opens, &mp3_frames);
        log("[not-bound] gl(5s): %llu frames, %llu calls, %llu draws, %llu triangles; sub-images %llu, %llu skipped as repeats (%llu KiB), %llu KiB kept; mp3 %llu frames (%llu streams opened)",
            (unsigned long long)frames, (unsigned long long)(g_calls - last_calls),
            (unsigned long long)(g_draws - last_draws), (unsigned long long)(g_tris - last_tris),
            (unsigned long long)g_sub_calls, (unsigned long long)g_sub_skips,
            (unsigned long long)(g_sub_skip_bytes >> 10), (unsigned long long)(g_sub_kept >> 10),
            (unsigned long long)(mp3_frames - last_mp3), (unsigned long long)mp3_opens);
        last_mp3 = mp3_frames;
        g_sub_calls = g_sub_skips = g_sub_skip_bytes = 0;
        frames = 0; t0 = now; last_calls = g_calls; last_draws = g_draws; last_tris = g_tris;
        glstat_line();
    }
#ifdef __vita__
    vglSwapBuffers(present_ime_open() ? GL_TRUE : GL_FALSE);   // GL_TRUE also draws the keyboard dialog
    present(c, 0, 0, 0, 0, nullptr);                          // controller, sound pump, frame stats
#else
    glFinish();
    present_rgba(c, g_fb.data(), g_w, g_h);
#endif
}

uint32_t guest_string(Cpu& c, const std::string& s) {
    static std::map<std::string, uint32_t>* cache = new std::map<std::string, uint32_t>();
    auto it = cache->find(s); if (it != cache->end()) return it->second;
    uint32_t p = wx86_scratch_put_cstr(c, s.c_str());
    (*cache)[s] = p; return p;
}

}  // namespace

void gl_render_size(uint32_t* w, uint32_t* h) {
    static bool read = false;
    if (!read) { read = true; read_render_size(); }
    *w = g_w; *h = g_h;
}

// ---- redundant-call filters, as guest x86 ------------------------------------
// Every GL call the game makes leaves the translated code for a shim: a
// dispatcher exit, the bridge, and the way back in, which costs far more
// than most of these calls do in vitaGL. WarCraft III sets much of its state
// again each time it draws (W3_GLSTAT on the host, Booty Bay: most client
// array switches, glEnable/glDisable and glMatrixMode calls set what is
// already set). A short x86 stub in front of such an entry point compares
// the call with what was last set and returns at once when it changes
// nothing; the translator compiles it inline with the caller. Any other call
// updates the stub's record and jumps on to the shim.
//
// The record mirrors the GL exactly because nothing else changes that state:
// the game has no glPushAttrib/glPopAttrib, and the port's own command-card
// overlay puts back everything it changes. W3_GL_FILTER=0 turns this off.
namespace {
struct X86 {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void mov_eax_arg(int k) { u8(0x8B); u8(0x44); u8(0x24); u8((uint8_t)(4 + 4 * k)); }    // mov eax,[esp+4+4k]
    void cmp_eax_mem(uint32_t a) { u8(0x3B); u8(0x05); u32(a); }                           // cmp eax,[a]
    void cmp_eax_mem_ecx(uint32_t a) { u8(0x3B); u8(0x81); u32(a); }                       // cmp eax,[ecx+a]
    void mov_mem_eax(uint32_t a) { u8(0xA3); u32(a); }                                     // mov [a],eax
    void mov_mem_ecx_eax(uint32_t a) { u8(0x89); u8(0x81); u32(a); }                       // mov [ecx+a],eax
    size_t jne32() { u8(0x0F); u8(0x85); u32(0); return b.size(); }                       // patched by bind()
    void bind(size_t at) { const uint32_t rel = (uint32_t)(b.size() - at); std::memcpy(&b[at - 4], &rel, 4); }
    void ret(uint32_t argc) { if (argc) { u8(0xC2); u8((uint8_t)(4 * argc)); u8(0); } else u8(0xC3); }
    void jmp(uint32_t from_va, uint32_t to) { u8(0xE9); u32(to - (from_va + (uint32_t)b.size() + 4)); }
    // ecx = (unit & 1) * 16, the unit from an active-texture record
    void ecx_unit16(uint32_t rec) { u8(0x8B); u8(0x0D); u32(rec); u8(0x83); u8(0xE1); u8(0x01); u8(0xC1); u8(0xE1); u8(0x04); }
};
std::map<std::string, uint32_t> g_filters;   // entry point -> stub VA
std::map<std::string, uint32_t> g_recs;      // entry point -> its record (same-argument stubs)
uint32_t g_caps_tab = 0, g_client_tab = 0;    // glEnable/glDisable and client-state tables

uint32_t put_stub(Cpu& c, X86& x) {
    const uint32_t va = wx86_scratch_alloc((uint32_t)x.b.size() + 16);
    if (va) c.write(va, x.b.data(), (uint32_t)x.b.size());
    return va;
}
uint32_t record(Cpu& c, uint32_t words) {
    const uint32_t va = wx86_scratch_alloc(words * 4);
    if (va) for (uint32_t i = 0; i < words; ++i) c.write_u32(va + 4 * i, 0xFFFFFFFFu);   // matches no real call
    return va;
}
// Same arguments as the previous call (per texture unit when `unit_rec`).
uint32_t same_args_stub(Cpu& c, const char* name, uint32_t argc, uint32_t unit_rec = 0, uint32_t* rec_out = nullptr) {
    const uint32_t trap = g_br->shim_trap("OPENGL32.dll", name);
    const uint32_t rec = record(c, unit_rec ? 8 : 4);
    if (!trap || !rec) return 0;
    if (rec_out) *rec_out = rec;
    g_recs[name] = rec;
    // Two passes: the jmp's displacement depends on the stub's own address.
    uint32_t va = 0;
    for (int pass = 0; pass < 2; ++pass) {
        X86 x; std::vector<size_t> miss;
        if (unit_rec) x.ecx_unit16(unit_rec);
        for (uint32_t k = 0; k < argc; ++k) {
            x.mov_eax_arg((int)k);
            if (unit_rec) x.cmp_eax_mem_ecx(rec + 4 * k); else x.cmp_eax_mem(rec + 4 * k);
            miss.push_back(x.jne32());
        }
        x.ret(argc);
        for (size_t m : miss) x.bind(m);
        for (uint32_t k = 0; k < argc; ++k) {
            x.mov_eax_arg((int)k);
            if (unit_rec) x.mov_mem_ecx_eax(rec + 4 * k); else x.mov_mem_eax(rec + 4 * k);
        }
        x.jmp(va, trap);
        if (pass == 0) { va = wx86_scratch_alloc((uint32_t)x.b.size() + 16); if (!va) return 0; }
        else c.write(va, x.b.data(), (uint32_t)x.b.size());
    }
    return va;
}
// glEnable/glDisable (and the client-state pair): one table of 64 (key, on)
// entries shared by the pair; the key is the capability, with the texture
// unit for the per-unit ones. A collision only costs a call.
uint32_t cap_stub(Cpu& c, const char* name, uint32_t on, uint32_t table, uint32_t per_unit_cap, uint32_t unit_rec) {
    const uint32_t trap = g_br->shim_trap("OPENGL32.dll", name);
    if (!trap || !table) return 0;
    uint32_t va = 0;
    for (int pass = 0; pass < 2; ++pass) {
        X86 x;
        x.mov_eax_arg(0);                                                     // eax = cap
        x.u8(0x3D); x.u32(per_unit_cap);                                      // cmp eax, per_unit_cap
        x.u8(0x75); x.u8(0x0C);                                               // jne +12
        x.u8(0x8B); x.u8(0x0D); x.u32(unit_rec);                              // mov ecx,[unit_rec]   (6)
        x.u8(0xC1); x.u8(0xE1); x.u8(0x10);                                   // shl ecx,16           (3)
        x.u8(0x0B); x.u8(0xC1);                                               // or eax,ecx           (2)
        x.u8(0x90);                                                           // nop                  (1)
        x.u8(0x69); x.u8(0xC8); x.u32(0x9E3779B1u);                           // imul ecx,eax,golden
        x.u8(0xC1); x.u8(0xE9); x.u8(26);                                     // shr ecx,26 -> 0..63
        x.u8(0x3B); x.u8(0x04); x.u8(0xCD); x.u32(table);                     // cmp eax,[table+ecx*8]
        const size_t m1 = x.jne32();
        x.u8(0x83); x.u8(0x3C); x.u8(0xCD); x.u32(table + 4); x.u8((uint8_t)on);  // cmp dword [table+4+ecx*8], on
        const size_t m2 = x.jne32();
        x.ret(1);
        x.bind(m1); x.bind(m2);
        x.u8(0x89); x.u8(0x04); x.u8(0xCD); x.u32(table);                     // mov [table+ecx*8],eax
        x.u8(0xC7); x.u8(0x04); x.u8(0xCD); x.u32(table + 4); x.u32(on);      // mov dword [table+4+ecx*8], on
        x.jmp(va, trap);
        if (pass == 0) { va = wx86_scratch_alloc((uint32_t)x.b.size() + 16); if (!va) return 0; }
        else c.write(va, x.b.data(), (uint32_t)x.b.size());
    }
    return va;
}
uint32_t cap_table(Cpu& c) {
    const uint32_t t = wx86_scratch_alloc(64 * 8);
    if (t) for (uint32_t i = 0; i < 64 * 2; ++i) c.write_u32(t + 4 * i, 0);   // key 0 is no capability
    return t;
}
}  // namespace

void gl_install_filters(Cpu& c) {
    static const bool off = [] { const char* e = std::getenv("W3_GL_FILTER"); return e && e[0] == '0'; }();
    if (off || !g_br || !g_filters.empty()) return;
    // Texture-unit records first: the per-unit stubs read them.
    // The game switches units around nearly every texture call (in game ~600
    // switches of each kind a frame, most of them undone before any call that
    // depends on the unit): the stubs only record the unit, never trap, and
    // sync_units() applies it when a GL call is actually made.
    for (auto [n, out] : { std::pair<const char*, uint32_t*>{ "glActiveTextureARB", &g_rec_act },
                           std::pair<const char*, uint32_t*>{ "glClientActiveTextureARB", &g_rec_cact } }) {
        const uint32_t rec = record(c, 4);
        if (!rec) return;
        X86 x; x.mov_eax_arg(0); x.mov_mem_eax(rec); x.ret(1);
        g_filters[n] = put_stub(c, x);
        if (!g_filters[n]) return;
        g_recs[n] = rec;
        *out = rec;
    }
    {   GLint a = GL_TEXTURE0, ca = GL_TEXTURE0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &a); glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &ca);
        g_gl_act = (uint32_t)a; g_gl_cact = (uint32_t)ca; }
    for (auto& [n, argc] : std::vector<std::pair<const char*, uint32_t>>{
             { "glMatrixMode", 1 }, { "glBlendFunc", 2 }, { "glAlphaFunc", 2 }, { "glDepthFunc", 1 }, { "glDepthMask", 1 },
             { "glVertexPointer", 4 }, { "glNormalPointer", 3 }, { "glColorPointer", 4 }, { "glScissor", 4 } })
        g_filters[n] = same_args_stub(c, n, argc);
    // (Not glBindTexture: deleting a bound texture unbinds it behind the record.)
    if (g_rec_act && g_rec_cact) {
        g_filters["glTexEnvi"] = same_args_stub(c, "glTexEnvi", 3, g_rec_act);
        g_filters["glTexCoordPointer"] = same_args_stub(c, "glTexCoordPointer", 4, g_rec_cact);
        const uint32_t caps = cap_table(c), client = cap_table(c);
        g_caps_tab = caps; g_client_tab = client;
        g_filters["glEnable"] = cap_stub(c, "glEnable", 1, caps, GL_TEXTURE_2D, g_rec_act);
        g_filters["glDisable"] = cap_stub(c, "glDisable", 0, caps, GL_TEXTURE_2D, g_rec_act);
        g_filters["glEnableClientState"] = cap_stub(c, "glEnableClientState", 1, client, GL_TEXTURE_COORD_ARRAY, g_rec_cact);
        g_filters["glDisableClientState"] = cap_stub(c, "glDisableClientState", 0, client, GL_TEXTURE_COORD_ARRAY, g_rec_cact);
    }
    {   // Pure no-ops in this port: no trap at all.
        X86 l; l.ret(2); g_filters["glLockArraysEXT"] = put_stub(c, l);
        X86 u; u.ret(0); g_filters["glUnlockArraysEXT"] = put_stub(c, u);
    }
    int slots = 0, stubs = 0;
    for (auto& [n, va] : g_filters) {
        if (!va) continue;
        ++stubs;
        slots += g_br->repatch_import("OPENGL32.dll", n, va);
    }
    log("[not-bound] GL filters: %d stubs, %d import slots (W3_GL_FILTER=0 turns them off)", stubs, slots);
}

// W3_GL_FILTER_CHECK=1 (host test): at every draw, compare what the stubs'
// records claim with the GL's real state; a mismatch means a filter would
// have dropped a call that mattered.
void gl_check_filters(Cpu& c) {
#ifdef __vita__
    (void)c;   // host test only (vitaGL has no glGet for some of this state)
#else
    static const bool on = std::getenv("W3_GL_FILTER_CHECK") != nullptr;
    if (!on || g_filters.empty()) return;
    static uint64_t checks = 0, bad = 0;
    auto rec = [&](const char* n, int k) -> uint32_t { auto it = g_recs.find(n); return it == g_recs.end() ? 0xFFFFFFFFu : c.read_u32(it->second + 4 * k); };
    auto geti = [](GLenum e) { GLint v = 0; glGetIntegerv(e, &v); return (uint32_t)v; };
    auto expect = [&](const char* what, uint32_t claimed, uint32_t real) {
        if (claimed == 0xFFFFFFFFu || claimed == real) return;
        if (bad++ < 30) log("[not-bound] GL filter MISMATCH %s: record 0x%x, GL 0x%x (draw check %llu)", what, claimed, real, (unsigned long long)checks);
    };
    ++checks;
    const uint32_t act = geti(GL_ACTIVE_TEXTURE), cact = geti(GL_CLIENT_ACTIVE_TEXTURE);
    expect("active texture", rec("glActiveTextureARB", 0), act);
    expect("client active texture", rec("glClientActiveTextureARB", 0), cact);
    expect("matrix mode", rec("glMatrixMode", 0), geti(GL_MATRIX_MODE));
    expect("blend src", rec("glBlendFunc", 0), geti(GL_BLEND_SRC));
    expect("blend dst", rec("glBlendFunc", 1), geti(GL_BLEND_DST));
    expect("depth func", rec("glDepthFunc", 0), geti(GL_DEPTH_FUNC));
    { GLboolean m = 0; glGetBooleanv(GL_DEPTH_WRITEMASK, &m); const uint32_t r = rec("glDepthMask", 0);
      expect("depth mask", r == 0xFFFFFFFFu ? r : (uint32_t)(r != 0), (uint32_t)(m != 0)); }
    expect("alpha func", rec("glAlphaFunc", 0), geti(GL_ALPHA_TEST_FUNC));
    { GLvoid* p = nullptr; glGetPointerv(GL_VERTEX_ARRAY_POINTER, &p); const uint32_t r = rec("glVertexPointer", 3);
      expect("vertex pointer", r == 0xFFFFFFFFu ? r : (uint32_t)(uintptr_t)P(c, r), (uint32_t)(uintptr_t)p);
      expect("vertex stride", rec("glVertexPointer", 2), geti(GL_VERTEX_ARRAY_STRIDE)); }
    auto tab = [&](uint32_t t, bool client) {
        for (uint32_t i = 0; t && i < 64; ++i) {
            const uint32_t key = c.read_u32(t + 8 * i), st = c.read_u32(t + 8 * i + 4);
            if (!key) continue;
            const uint32_t cap = key & 0xFFFF, unit = key >> 16;
            if (unit) { if (client) glClientActiveTexture(unit); else glActiveTexture(unit); }
            const uint32_t real = glIsEnabled(cap) ? 1u : 0u;
            if (unit) { if (client) glClientActiveTexture(cact); else glActiveTexture(act); }
            char w[48]; std::snprintf(w, sizeof w, "%s 0x%x unit 0x%x", client ? "client state" : "cap", cap, unit);
            expect(w, st, real);
        }
    };
    tab(g_caps_tab, false); tab(g_client_tab, true);
    if (checks % 20000 == 0) log("[not-bound] GL filter check: %llu draws checked, %llu mismatches", (unsigned long long)checks, (unsigned long long)bad);
#endif
}

namespace {
// ---- texture uploads: skip a glTexSubImage2D that sends what the texture
// already holds. The game sends ~30 sub-images a frame and most repeat the
// previous one (build 6 Vita log: up to 730 ms of 10 s in vitaGL's
// glTexSubImage2D). Kept: a copy of the last upload per (texture, level,
// rectangle), compared byte for byte; rectangles up to 256 KiB, 4 MiB in all.
// W3_TEX_DEDUP=0 turns it off.
constexpr uint32_t kSubMax = 256u * 1024u, kSubTotal = 4u * 1024u * 1024u;
struct Unpack { int32_t row = 0, align = 4, skip_px = 0, skip_rows = 0; } g_unpack;
uint32_t g_tex_bound[2] = { 0, 0 };   // texture bound to GL_TEXTURE_2D on units 0 and 1
struct SubCopy { uint32_t level, x, y, w, h, fmt, type; std::vector<uint8_t> bytes; };
std::map<uint32_t, std::vector<SubCopy>> g_sub_copies;   // by texture name
const bool g_dedup_on = [] { const char* e = std::getenv("W3_TEX_DEDUP"); return !(e && *e == '0'); }();
// Level-0 size of each texture (from glTexImage2D), for W3_TSI_LOG=1: logs the
// first 400 sub-image uploads that are not repeats, with the texture's size.
struct TexSize { uint32_t w = 0, h = 0, levels = 0; };
std::map<uint32_t, TexSize> g_tex_size;
const bool g_tsi_log = [] { const char* e = std::getenv("W3_TSI_LOG"); return e && *e == '1'; }();

uint32_t texel_bytes(uint32_t fmt, uint32_t type) {
    uint32_t comps = 0;
    switch (fmt) {
    case 0x1908: case 0x80E1: comps = 4; break;              // GL_RGBA, GL_BGRA
    case 0x1907: case 0x80E0: comps = 3; break;              // GL_RGB, GL_BGR
    case 0x190A: comps = 2; break;                           // GL_LUMINANCE_ALPHA
    case 0x1906: case 0x1909: case 0x1903: comps = 1; break; // GL_ALPHA, GL_LUMINANCE, GL_RED
    default: return 0;
    }
    switch (type) {
    case 0x1401: return comps;                               // GL_UNSIGNED_BYTE
    case 0x8033: case 0x8363: case 0x8034: return 2;         // 4_4_4_4, 5_6_5, 5_5_5_1
    case 0x8035: case 0x8367: return 4;                      // 8_8_8_8, 8_8_8_8_REV
    default: return 0;
    }
}
void sub_forget(uint32_t tex) {
    auto it = g_sub_copies.find(tex); if (it == g_sub_copies.end()) return;
    for (const SubCopy& s : it->second) g_sub_kept -= s.bytes.size();
    g_sub_copies.erase(it);
}
// True when this upload repeats the texels the bound texture got last time
// for the same rectangle (then the call is skipped); otherwise remembers them.
bool sub_repeats(Cpu& c, uint32_t target, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t fmt, uint32_t type, uint32_t va) {
    ++g_sub_calls;
    if (!g_dedup_on || target != GL_TEXTURE_2D || !va || !w || !h) return false;
    const uint32_t bpp = texel_bytes(fmt, type); if (!bpp) return false;
    const uint32_t row = w * bpp, n = row * h;
    if (n > kSubMax) return false;
    const uint32_t a = g_unpack.align > 0 ? (uint32_t)g_unpack.align : 1u;
    const uint32_t stride = ((g_unpack.row > 0 ? (uint32_t)g_unpack.row : w) * bpp + a - 1) / a * a;
    const uint32_t start = (uint32_t)g_unpack.skip_rows * stride + (uint32_t)g_unpack.skip_px * bpp;
    const uint8_t* src = (const uint8_t*)c.hostptr(va + start, stride * (h - 1) + row);
    if (!src) return false;
    const uint32_t tex = g_tex_bound[(g_gl_act - GL_TEXTURE0) & 1];
    std::vector<SubCopy>& v = g_sub_copies[tex];
    SubCopy* rec = nullptr;
    for (size_t i = 0; i < v.size();) {
        SubCopy& s = v[i];
        if (s.level == level && s.x == x && s.y == y && s.w == w && s.h == h && s.fmt == fmt && s.type == type) { rec = &s; ++i; continue; }
        // Another rectangle of the same level that overlaps this one is now stale.
        if (s.level == level && x < s.x + s.w && s.x < x + w && y < s.y + s.h && s.y < y + h) {
            g_sub_kept -= s.bytes.size(); v.erase(v.begin() + (long)i); continue;
        }
        ++i;
    }
    if (rec) {
        bool same = true;
        for (uint32_t r = 0; r < h && same; ++r) same = std::memcmp(rec->bytes.data() + r * row, src + r * stride, row) == 0;
        if (same) { ++g_sub_skips; g_sub_skip_bytes += n; return true; }
    } else {
        if (g_sub_kept + n > kSubTotal) return false;
        v.push_back(SubCopy{ level, x, y, w, h, fmt, type, std::vector<uint8_t>(n) });
        rec = &v.back(); g_sub_kept += n;
    }
    for (uint32_t r = 0; r < h; ++r) std::memcpy(rec->bytes.data() + r * row, src + r * stride, row);
    return false;
}
}  // namespace

void gl_frame_stats(GlFrameStats* out) {
    *out = g_fs; out->draws = (uint32_t)g_draws;   // draws: running total (the caller takes the difference)
    g_fs = GlFrameStats();
}

void install_gl(d2rt::Bridge& br) {
    { uint32_t w, h; gl_render_size(&w, &h); }   // W3_RES -> g_w, g_h
    // The game's window is the whole screen: GetSystemMetrics, the default
    // window size and the cursor bounds all report the render size.
    wx86_set_screen_size(g_w, g_h);
    set_default_window_size((int)g_w, (int)g_h);
    input_set_screen((int)g_w, (int)g_h);
    // ---- pixel formats (GDI32) ----------------------------------------------
    // One format: 32-bit RGBA, 24-bit depth, 8-bit stencil, double buffered.
    auto fill_pfd = [](Cpu& c, uint32_t p) {
        uint8_t z[40] = {}; c.write(p, z, sizeof z);
        uint16_t sz = 40, ver = 1; c.write(p, &sz, 2); c.write(p + 2, &ver, 2);
        c.write_u32(p + 4, 0x4 | 0x20 | 0x1);          // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
        uint8_t b[] = { 0 /*RGBA*/, 32, 8, 16, 8, 8, 8, 0, 8, 24 };
        c.write(p + 8, b, 1); c.write(p + 9, b + 1, 1);
        uint8_t rb = 8, rs = 16, gb = 8, gs = 8, bb = 8, bs = 0, ab = 8, as = 24;
        c.write(p + 10, &rb, 1); c.write(p + 11, &rs, 1); c.write(p + 12, &gb, 1); c.write(p + 13, &gs, 1);
        c.write(p + 14, &bb, 1); c.write(p + 15, &bs, 1); c.write(p + 16, &ab, 1); c.write(p + 17, &as, 1);
        uint8_t depth = 24, stencil = 8; c.write(p + 23, &depth, 1); c.write(p + 24, &stencil, 1);
    };
    GDI(br, "ChoosePixelFormat", 2, [](Cpu&) { return 1u; });
    GDI(br, "SetPixelFormat", 3, [](Cpu&) { return 1u; });
    GDI(br, "GetPixelFormat", 1, [](Cpu&) { return 1u; });
    GDI(br, "DescribePixelFormat", 4, [fill_pfd](Cpu& c) {
        if (c.arg(3) && c.arg(2) >= 40) fill_pfd(c, c.arg(3));
        return 1u;                                      // number of formats
    });
    GDI(br, "SwapBuffers", 1, [](Cpu& c) { swap(c); return 1u; });

    // ---- WGL -------------------------------------------------------------------
    G(br, "wglCreateContext", 1, [](Cpu&) { return 0x00E10001u; });
    G(br, "wglDeleteContext", 1, [](Cpu&) { return 1u; });
    G(br, "wglMakeCurrent", 2, [](Cpu& c) { return c.arg(1) ? (make_current() ? 1u : 0u) : 1u; });
    G(br, "wglSwapLayerBuffers", 2, [](Cpu& c) { swap(c); return 1u; });
    G(br, "wglGetProcAddress", 1, [](Cpu& c) -> uint32_t {
        std::string n = wx86_gread_mb(c, c.arg(0), -1);
        auto f = g_filters.find(n);
        uint32_t t = f != g_filters.end() && f->second ? f->second : g_br->shim_trap("OPENGL32.dll", n);
        log("[not-bound] wglGetProcAddress(%s) -> 0x%08x", n.c_str(), t);
        return t;
    });

    // ---- state -----------------------------------------------------------------
    G(br, "glGetString", 1, [](Cpu& c) -> uint32_t {
        switch (c.arg(0)) {
        case GL_VENDOR:     return guest_string(c, "WarCraft III Vita");
        case GL_RENDERER:   return guest_string(c, "Vita GPU (SGX543MP4+)");
        case GL_VERSION:    return guest_string(c, "1.3");
        case GL_EXTENSIONS: return guest_string(c, "GL_ARB_multitexture GL_EXT_bgra GL_EXT_texture_edge_clamp GL_EXT_compiled_vertex_array");
        }
        return 0u;
    });
    G(br, "glGetIntegerv", 2, [](Cpu& c) {
        GLint v[16] = {}; glGetIntegerv(c.arg(0), v);
        if (c.arg(0) == GL_MAX_TEXTURE_UNITS) v[0] = 2;
        int n = (c.arg(0) == GL_VIEWPORT || c.arg(0) == GL_SCISSOR_BOX) ? 4 : 1;
        c.write(c.arg(1), v, 4 * n); return 0u;
    });
    G(br, "glEnable", 1, [](Cpu& c) { glEnable(c.arg(0)); return 0u; });
    G(br, "glDisable", 1, [](Cpu& c) { glDisable(c.arg(0)); return 0u; });
    G(br, "glEnableClientState", 1, [](Cpu& c) { glEnableClientState(c.arg(0)); return 0u; });
    G(br, "glDisableClientState", 1, [](Cpu& c) { glDisableClientState(c.arg(0)); return 0u; });
    G(br, "glAlphaFunc", 2, [](Cpu& c) { glAlphaFunc(c.arg(0), F(c, 1)); return 0u; });
    G(br, "glBlendFunc", 2, [](Cpu& c) { glBlendFunc(c.arg(0), c.arg(1)); return 0u; });
    G(br, "glDepthFunc", 1, [](Cpu& c) { glDepthFunc(c.arg(0)); return 0u; });
    G(br, "glDepthMask", 1, [](Cpu& c) { glDepthMask((GLboolean)c.arg(0)); return 0u; });
    G(br, "glDepthRange", 4, [](Cpu& c) { glDepthRange(D(c, 0), D(c, 2)); return 0u; });
    G(br, "glPolygonOffset", 2, [](Cpu& c) { glPolygonOffset(F(c, 0), F(c, 1)); return 0u; });
    G(br, "glClear", 1, [](Cpu& c) { glClear(c.arg(0)); return 0u; });
    G(br, "glClearColor", 4, [](Cpu& c) { glClearColor(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); return 0u; });
    G(br, "glViewport", 4, [](Cpu& c) { glViewport((GLint)c.arg(0), (GLint)c.arg(1), (GLsizei)c.arg(2), (GLsizei)c.arg(3)); return 0u; });
    G(br, "glScissor", 4, [](Cpu& c) { glScissor((GLint)c.arg(0), (GLint)c.arg(1), (GLsizei)c.arg(2), (GLsizei)c.arg(3)); return 0u; });
    G(br, "glDrawBuffer", 1, [](Cpu&) { return 0u; });   // always the back buffer
    G(br, "glReadBuffer", 1, [](Cpu&) { return 0u; });
    G(br, "glFinish", 0, [](Cpu&) { return 0u; });       // the swap waits instead
    G(br, "glPixelStorei", 2, [](Cpu& c) {
        const int32_t v = (int32_t)c.arg(1);
        switch (c.arg(0)) {   // the unpack layout, for the sub-image compare
        case 0x0CF2: g_unpack.row = v; break; case 0x0CF3: g_unpack.skip_rows = v; break;
        case 0x0CF4: g_unpack.skip_px = v; break; case 0x0CF5: g_unpack.align = v; break;
        }
        glPixelStorei(c.arg(0), (GLint)c.arg(1)); return 0u; });
    G(br, "glReadPixels", 7, [](Cpu& c) {
        glReadPixels((GLint)c.arg(0), (GLint)c.arg(1), (GLsizei)c.arg(2), (GLsizei)c.arg(3), c.arg(4), c.arg(5), P(c, c.arg(6)));
        return 0u;
    });

    // ---- matrices, lighting, fog -------------------------------------------------
    G(br, "glMatrixMode", 1, [](Cpu& c) { glMatrixMode(c.arg(0)); return 0u; });
    G(br, "glLoadMatrixf", 1, [](Cpu& c) { glLoadMatrixf(P<const GLfloat>(c, c.arg(0))); return 0u; });
    G(br, "glLightf", 3, [](Cpu& c) { const GLfloat v[4] = { F(c, 2), 0, 0, 0 }; glLightfv(c.arg(0), c.arg(1), v); return 0u; });
    G(br, "glLightfv", 3, [](Cpu& c) { glLightfv(c.arg(0), c.arg(1), P<const GLfloat>(c, c.arg(2))); return 0u; });
    G(br, "glLightModelfv", 2, [](Cpu& c) { glLightModelfv(c.arg(0), P<const GLfloat>(c, c.arg(1))); return 0u; });
    G(br, "glMaterialfv", 3, [](Cpu& c) { glMaterialfv(c.arg(0), c.arg(1), P<const GLfloat>(c, c.arg(2))); return 0u; });
    G(br, "glColorMaterial", 2, [](Cpu& c) { glColorMaterial(c.arg(0), c.arg(1)); return 0u; });
    G(br, "glNormal3fv", 1, [](Cpu& c) { glNormal3fv(P<const GLfloat>(c, c.arg(0))); return 0u; });
    G(br, "glFogf", 2, [](Cpu& c) { glFogf(c.arg(0), F(c, 1)); return 0u; });
    G(br, "glFogi", 2, [](Cpu& c) { glFogi(c.arg(0), (GLint)c.arg(1)); return 0u; });
    G(br, "glFogfv", 2, [](Cpu& c) { glFogfv(c.arg(0), P<const GLfloat>(c, c.arg(1))); return 0u; });

    // ---- textures ------------------------------------------------------------------
    G(br, "glGenTextures", 2, [](Cpu& c) { glGenTextures((GLsizei)c.arg(0), P<GLuint>(c, c.arg(1))); return 0u; });
    G(br, "glDeleteTextures", 2, [](Cpu& c) {
        const GLuint* names = P<const GLuint>(c, c.arg(1));
        for (GLsizei i = 0; names && i < (GLsizei)c.arg(0); ++i) {
            sub_forget(names[i]);
            for (uint32_t& b : g_tex_bound) if (b == names[i]) b = 0;
        }
        glDeleteTextures((GLsizei)c.arg(0), names); return 0u; });
    G(br, "glBindTexture", 2, [](Cpu& c) {
        if (c.arg(0) == GL_TEXTURE_2D) g_tex_bound[(g_gl_act - GL_TEXTURE0) & 1] = c.arg(1);
        glBindTexture(c.arg(0), c.arg(1)); return 0u; });
    G(br, "glTexParameteri", 3, [](Cpu& c) { glTexParameteri(c.arg(0), c.arg(1), (GLint)c.arg(2)); return 0u; });
    G(br, "glTexEnvi", 3, [](Cpu& c) { glTexEnvi(c.arg(0), c.arg(1), (GLint)c.arg(2)); return 0u; });
#ifdef __vita__
    // No texture-coordinate generation in vitaGL: sphere-mapped reflections
    // on a few models are drawn without their shine.
    G(br, "glTexGeni", 3, [](Cpu& c) {
        static bool once = false;
        if (!once) { once = true; log("[not-bound] [cause: GL entry point not on this platform] glTexGeni(0x%x, 0x%x, 0x%x)", c.arg(0), c.arg(1), c.arg(2)); }
        return 0u; });
#else
    G(br, "glTexGeni", 3, [](Cpu& c) { glTexGeni(c.arg(0), c.arg(1), (GLint)c.arg(2)); return 0u; });
#endif
    G(br, "glTexImage2D", 9, [](Cpu& c) {
        if (c.arg(0) == GL_TEXTURE_2D) sub_forget(g_tex_bound[(g_gl_act - GL_TEXTURE0) & 1]);
        if (g_tsi_log && c.arg(0) == GL_TEXTURE_2D) {
            TexSize& t = g_tex_size[g_tex_bound[(g_gl_act - GL_TEXTURE0) & 1]];
            if (c.arg(1) == 0) { t.w = c.arg(3); t.h = c.arg(4); t.levels = 1; } else if (c.arg(1) + 1 > t.levels) t.levels = c.arg(1) + 1;
        }
        const uint64_t t0 = wx86_now_us();
        glTexImage2D(c.arg(0), (GLint)c.arg(1), (GLint)c.arg(2), (GLsizei)c.arg(3), (GLsizei)c.arg(4), (GLint)c.arg(5),
                     c.arg(6), c.arg(7), P(c, c.arg(8)));
        ++g_fs.img_n; g_fs.img_us += (uint32_t)(wx86_now_us() - t0);
        g_fs.img_bytes += (c.arg(3) * c.arg(4) * (texel_bytes(c.arg(6), c.arg(7)) ? texel_bytes(c.arg(6), c.arg(7)) : 4));
        return 0u;
    });
    G(br, "glTexSubImage2D", 9, [](Cpu& c) {
        if (sub_repeats(c, c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5), c.arg(6), c.arg(7), c.arg(8))) return 0u;
        if (g_tsi_log) {
            static int n = 0;
            const uint32_t tex = g_tex_bound[(g_gl_act - GL_TEXTURE0) & 1];
            const TexSize& t = g_tex_size[tex];
            if (n++ < 400) log("[not-bound] tsi: frame %llu tex %u (%ux%u, %u levels) level %u rect %u,%u %ux%u fmt 0x%x type 0x%x",
                               (unsigned long long)g_swaps, tex, t.w, t.h, t.levels, c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5), c.arg(6), c.arg(7));
        }
        const uint64_t t0 = wx86_now_us();
        glTexSubImage2D(c.arg(0), (GLint)c.arg(1), (GLint)c.arg(2), (GLint)c.arg(3), (GLsizei)c.arg(4), (GLsizei)c.arg(5),
                        c.arg(6), c.arg(7), P(c, c.arg(8)));
        ++g_fs.sub_n; g_fs.sub_us += (uint32_t)(wx86_now_us() - t0);
        g_fs.sub_bytes += (c.arg(4) * c.arg(5) * (texel_bytes(c.arg(6), c.arg(7)) ? texel_bytes(c.arg(6), c.arg(7)) : 4));
        return 0u;
    });

    // ---- vertex arrays ---------------------------------------------------------------
    G(br, "glVertexPointer", 4, [](Cpu& c) { glVertexPointer((GLint)c.arg(0), c.arg(1), (GLsizei)c.arg(2), P(c, c.arg(3))); return 0u; });
    G(br, "glNormalPointer", 3, [](Cpu& c) { glNormalPointer(c.arg(0), (GLsizei)c.arg(1), P(c, c.arg(2))); return 0u; });
    G(br, "glColorPointer", 4, [](Cpu& c) { glColorPointer((GLint)c.arg(0), c.arg(1), (GLsizei)c.arg(2), P(c, c.arg(3))); return 0u; });
    G(br, "glTexCoordPointer", 4, [](Cpu& c) { glTexCoordPointer((GLint)c.arg(0), c.arg(1), (GLsizei)c.arg(2), P(c, c.arg(3))); return 0u; });
    G(br, "glDrawElements", 4, [](Cpu& c) {
        ++g_draws; if (c.arg(0) == GL_TRIANGLES) g_tris += c.arg(1) / 3;
        gl_check_filters(c);
        glDrawElements(c.arg(0), (GLsizei)c.arg(1), c.arg(2), P(c, c.arg(3)));
        return 0u;
    });
    G(br, "glLockArraysEXT", 2, [](Cpu&) { return 0u; });
    G(br, "glUnlockArraysEXT", 0, [](Cpu&) { return 0u; });

    // ---- GL_ARB_multitexture (through wglGetProcAddress) -------------------------------
    G(br, "glActiveTextureARB", 1, [](Cpu& c) { glActiveTexture(c.arg(0)); g_gl_act = c.arg(0); return 0u; });   // filters off
    G(br, "glClientActiveTextureARB", 1, [](Cpu& c) { glClientActiveTexture(c.arg(0)); return 0u; });
    G(br, "glMultiTexCoord2fARB", 3, [](Cpu& c) { glMultiTexCoord2f(c.arg(0), F(c, 1), F(c, 2)); return 0u; });
    G(br, "glMultiTexCoord2fvARB", 2, [](Cpu& c) { const GLfloat* v = P<const GLfloat>(c, c.arg(1)); glMultiTexCoord2f(c.arg(0), v[0], v[1]); return 0u; });
}

}  // namespace sc
