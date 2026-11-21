// dx_gl.cpp — OPENGL32.dll for Deus Ex's OpenGlDrv.dll.
//
// OpenGlDrv loads OpenGL32.dll at run time and fetches every entry point with
// GetProcAddress (extensions with wglGetProcAddress). Each one is a shim that
// calls the real GL of the platform:
//   * qemu host: Mesa's OSMesa, rendering into a buffer inside the guest
//     arena, so the usual host screenshot path (present) can save frames;
//   * Vita: vitaGL (sceGxm underneath), drawing straight to the screen.
// The per-function shims are generated (tools/gen/gen_gl.py -> dx_gl_gen.inc);
// contexts, pixel formats, strings and SwapBuffers are written here.
//
// DX_GLSTATS=1 logs the most-called GL functions every 300 frames.
#include "sc_host.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/host_clock.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
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

void present(d2rt::Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h, const uint32_t* pal);

namespace {

constexpr uint32_t kW = 640, kH = 480;
d2rt::Bridge* g_glbr = nullptr;
std::map<std::string, uint64_t>* g_counts = nullptr;
bool g_stats = false;

uint64_t* gl_counter(const char* name) {
    if (!g_counts) g_counts = new std::map<std::string, uint64_t>();
    return &(*g_counts)[name];
}
#define GLCOUNT(n) do { if (g_stats) { static uint64_t* _c = gl_counter(n); ++*_c; } } while (0)

void gl_missing(const char* name) {
    static std::map<std::string, int> seen;
    if (seen[name]++ == 0) log("[not-bound] [cause: GL entry point not on this platform] %s", name);
}

inline float GF(const uint32_t* A, int i) { float f; std::memcpy(&f, &A[i], 4); return f; }
inline double GD(const uint32_t* A, int i) { uint64_t v = A[i] | ((uint64_t)A[i + 1] << 32); double d; std::memcpy(&d, &v, 8); return d; }
inline void* GP(Cpu& c, uint32_t va) { return va ? c.hostptr(va, 1) : nullptr; }

uint32_t g_bn = 0;      // vertices waiting in the draw batch (see below)
void batch_flush();

// Register one OPENGL32 entry point. The shim reads its arguments straight
// from the guest stack (one host pointer for all of them). Any GL call can
// change state the batched draws depend on, so it first sends the batch;
// only the client-array calls the batcher handles itself skip that (GB).
void reg(const char* name, uint32_t words, uint32_t (*fn)(Cpu&, const uint32_t*), bool flush) {
    Shim s; s.argc = words; s.stdcall_cleanup = true;
    s.tag = std::string("OPENGL32.dll!") + name;
    s.fn = [fn, words, flush](Cpu& c) -> uint32_t {
        static const uint32_t zero[16] = {};
        if (flush && g_bn) batch_flush();
        const uint32_t* A = words ? (const uint32_t*)c.hostptr(c.reg(d2rt::R_ESP) + 4, 4 * words) : zero;
        return fn(c, A ? A : zero);
    };
    g_glbr->register_shim("OPENGL32.dll", name, s);
}
void G(const char* name, uint32_t words, uint32_t (*fn)(Cpu&, const uint32_t*)) { reg(name, words, fn, true); }
void GB(const char* name, uint32_t words, uint32_t (*fn)(Cpu&, const uint32_t*)) { reg(name, words, fn, false); }

// ---- draw batching ----------------------------------------------------------
// OpenGlDrv draws every polygon with its own glDrawArrays (about 800 per frame
// on Liberty Island, plus a glEnable/DisableClientState pair each). Each GPU
// draw has a fixed cost in vitaGL, so consecutive draws with nothing changed
// in between are merged: their vertices are copied into one buffer as plain
// triangles and sent as a single glDrawArrays when anything else happens.
// The guest's client-array state is kept here; the real GL only ever sees
// the batch buffer (or, for draws the batcher can't take, the guest arrays).
struct Arr { bool on = false; int size = 4; uint32_t type = GL_FLOAT; int stride = 0; uint32_t va = 0; };
struct GuestArrays { Arr v, c, n, sc, fog, t[2]; int unit = 0; } g_ga;
struct BV { float x, y, z, u0, v0, u1, v1; uint8_t r, g, b, a; };
constexpr uint32_t kBatchMax = 32768;
BV* g_batch = nullptr;
int g_bkey = -1;        // layout of the batch: bit0 colour, bit1 tex0, bit2 tex1
int g_real_key = -2;    // layout the real GL arrays are set up for (-1 = guest arrays)
Cpu* g_bcpu = nullptr;
uint64_t g_st_draws = 0, g_st_batches = 0, g_st_direct = 0;
#ifndef GL_SECONDARY_COLOR_ARRAY
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#endif
#ifndef GL_FOG_COORD_ARRAY
#define GL_FOG_COORD_ARRAY 0x8457
#endif

void real_client(GLenum cap, bool on) { if (on) glEnableClientState(cap); else glDisableClientState(cap); }

void batch_flush() {
    if (!g_bn) return;
    if (g_real_key != g_bkey) {
        real_client(GL_VERTEX_ARRAY, true);
        real_client(GL_COLOR_ARRAY, g_bkey & 1);
        real_client(GL_NORMAL_ARRAY, false);
        for (int u = 0; u < 2; ++u) {
            glClientActiveTexture(GL_TEXTURE0 + u);
            real_client(GL_TEXTURE_COORD_ARRAY, g_bkey & (2 << u));
        }
        glClientActiveTexture(GL_TEXTURE0);
        g_real_key = g_bkey;
    }
    glVertexPointer(3, GL_FLOAT, sizeof(BV), &g_batch[0].x);
    if (g_bkey & 1) glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(BV), &g_batch[0].r);
    if (g_bkey & 2) { glClientActiveTexture(GL_TEXTURE0); glTexCoordPointer(2, GL_FLOAT, sizeof(BV), &g_batch[0].u0); }
    if (g_bkey & 4) { glClientActiveTexture(GL_TEXTURE1); glTexCoordPointer(2, GL_FLOAT, sizeof(BV), &g_batch[0].u1); glClientActiveTexture(GL_TEXTURE0); }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_bn);
    g_bn = 0; ++g_st_batches;
}

// Draw with the guest's own arrays (formats the batcher doesn't take).
void apply_guest_arrays(Cpu& c) {
    auto P = [&](const Arr& a) -> const void* { return a.va ? c.hostptr(a.va, 1) : nullptr; };
    real_client(GL_VERTEX_ARRAY, g_ga.v.on); if (g_ga.v.on) glVertexPointer(g_ga.v.size, g_ga.v.type, g_ga.v.stride, P(g_ga.v));
    real_client(GL_COLOR_ARRAY, g_ga.c.on); if (g_ga.c.on) glColorPointer(g_ga.c.size, g_ga.c.type, g_ga.c.stride, P(g_ga.c));
    real_client(GL_NORMAL_ARRAY, g_ga.n.on); if (g_ga.n.on) glNormalPointer(g_ga.n.type, g_ga.n.stride, P(g_ga.n));
    for (int u = 0; u < 2; ++u) {
        glClientActiveTexture(GL_TEXTURE0 + u);
        real_client(GL_TEXTURE_COORD_ARRAY, g_ga.t[u].on);
        if (g_ga.t[u].on) glTexCoordPointer(g_ga.t[u].size, g_ga.t[u].type, g_ga.t[u].stride, P(g_ga.t[u]));
    }
    glClientActiveTexture(GL_TEXTURE0);
    g_real_key = -1;
}

inline int tsize(uint32_t type) { return type == GL_UNSIGNED_BYTE || type == GL_BYTE ? 1 : type == GL_SHORT || type == GL_UNSIGNED_SHORT ? 2 : type == 0x140A /* GL_DOUBLE */ ? 8 : 4; }
inline const uint8_t* arr_base(Cpu& c, const Arr& a) { return a.va ? (const uint8_t*)c.hostptr(a.va, 1) : nullptr; }
inline int arr_stride(const Arr& a) { return a.stride ? a.stride : a.size * tsize(a.type); }

// Can the batcher take a draw with the current guest arrays?
bool batchable(uint32_t mode) {
    if (mode != GL_TRIANGLES && mode != GL_TRIANGLE_FAN && mode != GL_TRIANGLE_STRIP && mode != GL_QUADS && mode != GL_POLYGON) return false;
    if (!g_ga.v.on || g_ga.v.type != GL_FLOAT || g_ga.v.size < 2 || g_ga.v.size > 3) return false;
    if (g_ga.n.on || g_ga.sc.on || g_ga.fog.on) return false;
    if (g_ga.c.on && !((g_ga.c.type == GL_UNSIGNED_BYTE && g_ga.c.size == 4) || (g_ga.c.type == GL_FLOAT && g_ga.c.size >= 3))) return false;
    for (int u = 0; u < 2; ++u) if (g_ga.t[u].on && (g_ga.t[u].type != GL_FLOAT || g_ga.t[u].size != 2)) return false;
    return true;
}

void put_vertex(BV& o, int i, const uint8_t* vb, int vs, const uint8_t* cb, int cs, const uint8_t* t0, int s0, const uint8_t* t1, int s1) {
    const float* p = (const float*)(vb + i * vs);
    o.x = p[0]; o.y = p[1]; o.z = g_ga.v.size > 2 ? p[2] : 0.0f;
    if (t0) { const float* t = (const float*)(t0 + i * s0); o.u0 = t[0]; o.v0 = t[1]; }
    if (t1) { const float* t = (const float*)(t1 + i * s1); o.u1 = t[0]; o.v1 = t[1]; }
    if (cb) {
        if (g_ga.c.type == GL_UNSIGNED_BYTE) std::memcpy(&o.r, cb + i * cs, 4);
        else {
            const float* f = (const float*)(cb + i * cs);
            auto q = [](float v) -> uint8_t { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
            o.r = q(f[0]); o.g = q(f[1]); o.b = q(f[2]); o.a = g_ga.c.size > 3 ? q(f[3]) : 255;
        }
    }
}

uint32_t draw_arrays(Cpu& c, const uint32_t* A) {
    GLCOUNT("glDrawArrays");
    const uint32_t mode = A[0]; const int first = (int32_t)A[1], count = (int32_t)A[2];
    ++g_st_draws;
    if (count <= 0) return 0;
    const int ntri = mode == GL_TRIANGLES ? count / 3 : mode == GL_QUADS ? (count / 4) * 2 : count - 2;
    if (!batchable(mode) || ntri <= 0 || (uint32_t)ntri * 3 > kBatchMax) {
        batch_flush(); apply_guest_arrays(c); ++g_st_direct;
        glDrawArrays(mode, first, count);
        return 0;
    }
    const int key = (g_ga.c.on ? 1 : 0) | (g_ga.t[0].on ? 2 : 0) | (g_ga.t[1].on ? 4 : 0);
    if (g_bn && (key != g_bkey || g_bn + ntri * 3 > kBatchMax)) batch_flush();
    if (!g_batch) g_batch = (BV*)std::calloc(kBatchMax, sizeof(BV));
    g_bkey = key;
    const uint8_t* vb = arr_base(c, g_ga.v); const int vs = arr_stride(g_ga.v);
    const uint8_t* cb = g_ga.c.on ? arr_base(c, g_ga.c) : nullptr; const int cs = arr_stride(g_ga.c);
    const uint8_t* t0 = g_ga.t[0].on ? arr_base(c, g_ga.t[0]) : nullptr; const int s0 = arr_stride(g_ga.t[0]);
    const uint8_t* t1 = g_ga.t[1].on ? arr_base(c, g_ga.t[1]) : nullptr; const int s1 = arr_stride(g_ga.t[1]);
    if (!vb) return 0;
    BV* o = g_batch + g_bn;
    auto V = [&](int i) { put_vertex(*o++, first + i, vb, vs, cb, cs, t0, s0, t1, s1); };
    if (mode == GL_TRIANGLES) for (int i = 0; i < ntri * 3; ++i) V(i);
    else if (mode == GL_QUADS) for (int q = 0; q < count / 4; ++q) { V(4*q); V(4*q+1); V(4*q+2); V(4*q); V(4*q+2); V(4*q+3); }
    else if (mode == GL_TRIANGLE_STRIP) for (int i = 0; i < ntri; ++i) { if (i & 1) { V(i+1); V(i); V(i+2); } else { V(i); V(i+1); V(i+2); } }
    else for (int i = 1; i <= ntri; ++i) { V(0); V(i); V(i+1); }   // fan / polygon
    g_bn += ntri * 3;
    return 0;
}

Arr* client_arr(uint32_t cap) {
    switch (cap) {
    case GL_VERTEX_ARRAY: return &g_ga.v;
    case GL_COLOR_ARRAY: return &g_ga.c;
    case GL_NORMAL_ARRAY: return &g_ga.n;
    case GL_TEXTURE_COORD_ARRAY: return &g_ga.t[g_ga.unit];
    case GL_SECONDARY_COLOR_ARRAY: return &g_ga.sc;
    case GL_FOG_COORD_ARRAY: return &g_ga.fog;
    default: return nullptr;
    }
}

void install_batcher() {
    GB("glEnableClientState", 1, [](Cpu&, const uint32_t* A) -> uint32_t { GLCOUNT("glEnableClientState"); if (Arr* a = client_arr(A[0])) a->on = true; return 0; });
    GB("glDisableClientState", 1, [](Cpu&, const uint32_t* A) -> uint32_t { GLCOUNT("glDisableClientState"); if (Arr* a = client_arr(A[0])) a->on = false; return 0; });
    GB("glClientActiveTextureARB", 1, [](Cpu&, const uint32_t* A) -> uint32_t { GLCOUNT("glClientActiveTextureARB"); g_ga.unit = (A[0] - GL_TEXTURE0) & 1; return 0; });
    GB("glVertexPointer", 4, [](Cpu&, const uint32_t* A) -> uint32_t { g_ga.v.size = A[0]; g_ga.v.type = A[1]; g_ga.v.stride = A[2]; g_ga.v.va = A[3]; return 0; });
    GB("glColorPointer", 4, [](Cpu&, const uint32_t* A) -> uint32_t { g_ga.c.size = A[0]; g_ga.c.type = A[1]; g_ga.c.stride = A[2]; g_ga.c.va = A[3]; return 0; });
    GB("glTexCoordPointer", 4, [](Cpu&, const uint32_t* A) -> uint32_t { Arr& t = g_ga.t[g_ga.unit]; t.size = A[0]; t.type = A[1]; t.stride = A[2]; t.va = A[3]; return 0; });
    GB("glNormalPointer", 3, [](Cpu&, const uint32_t* A) -> uint32_t { g_ga.n.size = 3; g_ga.n.type = A[0]; g_ga.n.stride = A[1]; g_ga.n.va = A[2]; return 0; });
    GB("glSecondaryColorPointerEXT", 4, [](Cpu&, const uint32_t* A) -> uint32_t { g_ga.sc.size = A[0]; g_ga.sc.type = A[1]; g_ga.sc.stride = A[2]; g_ga.sc.va = A[3]; return 0; });
    GB("glFogCoordPointerEXT", 3, [](Cpu&, const uint32_t* A) -> uint32_t { g_ga.fog.size = 1; g_ga.fog.type = A[0]; g_ga.fog.stride = A[1]; g_ga.fog.va = A[2]; return 0; });
    GB("glLockArraysEXT", 2, [](Cpu&, const uint32_t*) -> uint32_t { return 0; });
    GB("glUnlockArraysEXT", 0, [](Cpu&, const uint32_t*) -> uint32_t { return 0; });
    GB("glDrawArrays", 3, draw_arrays);
    // Indexed and single-element draws use the guest's arrays directly.
    G("glArrayElement", 1, [](Cpu& c, const uint32_t* A) -> uint32_t {
        GLCOUNT("glArrayElement"); apply_guest_arrays(c);
#ifdef __vita__
        (void)A; gl_missing("glArrayElement");
#else
        glArrayElement((GLint)A[0]);
#endif
        return 0; });
    G("glInterleavedArrays", 3, [](Cpu&, const uint32_t*) -> uint32_t { gl_missing("glInterleavedArrays (batcher)"); return 0; });
    G("glDrawElements", 4, [](Cpu& c, const uint32_t* A) -> uint32_t {
        GLCOUNT("glDrawElements"); apply_guest_arrays(c); ++g_st_direct;
        glDrawElements(A[0], (GLsizei)A[1], A[2], GP(c, A[3])); return 0; });
}

void R(d2rt::Bridge& br, const char* dll, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string(dll) + "!" + name;
    br.register_shim(dll, name, s);
}

// ---- context ---------------------------------------------------------------
bool g_ctx_made = false;
uint32_t g_fb_va = 0;                    // host: the colour buffer (guest memory)
uint64_t g_swaps = 0, g_swap_us = 0, g_frame_us = 0, g_last_swap_end = 0;
#ifndef __vita__
OSMesaContext g_mesa = nullptr;
#endif

bool make_context(Cpu& c) {
    if (g_ctx_made) return true;
#ifdef __vita__
    (void)c;
    // vitaGL on the 960x544 screen; the game's 640x480 picture is drawn at
    // 725x544 in the middle (see the glViewport/glScissor shims), so the GPU
    // rasterises at the final size and nothing is scaled afterwards.
    // RAM: everything already belongs to the guest arena and our heap; leave
    // 24 MiB for the JIT pool to grow into, give vitaGL the rest and all VRAM.
    if (!vglInitExtended(0x400000, 960, 544, 24 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE)) {
        log("[not-bound] [cause: vglInitExtended failed]"); return false;
    }
    vglWaitVblankStart(GL_TRUE);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); vglSwapBuffers(GL_FALSE);
    glClear(GL_COLOR_BUFFER_BIT);
    g_ctx_made = true;
    log("[not-bound] GL context: vitaGL, 960x544, game picture 725x544");
#else
    g_fb_va = wx86_scratch_alloc(kW * kH * 4);
    if (!g_fb_va) { log("[not-bound] [cause: scratch exhaustion] GL colour buffer"); return false; }
    g_mesa = OSMesaCreateContextExt(OSMESA_BGRA, 24, 8, 0, nullptr);
    if (!g_mesa || !OSMesaMakeCurrent(g_mesa, c.hostptr(g_fb_va, kW * kH * 4), GL_UNSIGNED_BYTE, kW, kH)) {
        log("[not-bound] [cause: OSMesa context]"); return false;
    }
    OSMesaPixelStore(OSMESA_Y_UP, 0);    // top row first, like a top-down DIB
    g_ctx_made = true;
    log("[not-bound] GL context: %s / %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
#endif
    return true;
}

void stats_line() {
    if (!g_stats || !g_counts) return;
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& kv : *g_counts) if (kv.second) v.push_back({ kv.second, kv.first });
    std::sort(v.rbegin(), v.rend());
    std::string s; uint64_t total = 0;
    for (auto& p : v) total += p.first;
    for (size_t i = 0; i < v.size() && i < 14; ++i) s += " " + v[i].second + "=" + std::to_string(v[i].first / 300);
    log("[not-bound] GL calls per frame (300-frame avg): total %llu,%s", (unsigned long long)(total / 300), s.c_str());
    if (g_swap_us || g_frame_us)
        log("[not-bound] GL frame: %.2f ms game+GL calls, %.2f ms in vglSwapBuffers (GPU wait + vblank)",
            g_frame_us / 300.0 / 1000.0, g_swap_us / 300.0 / 1000.0);
    g_swap_us = g_frame_us = 0;
    if (g_st_draws)
        log("[not-bound] GL batching: %llu game draws -> %llu GPU draws batched + %llu direct (per frame)",
            (unsigned long long)(g_st_draws / 300), (unsigned long long)(g_st_batches / 300), (unsigned long long)(g_st_direct / 300));
    g_st_draws = g_st_batches = g_st_direct = 0;
    for (auto& kv : *g_counts) kv.second = 0;
}

uint32_t swap(Cpu& c) {
    batch_flush();
    ++g_swaps;
#ifdef __vita__
    const uint64_t t0 = wx86_now_us();
    vglSwapBuffers(GL_FALSE);
    const uint64_t t1 = wx86_now_us();
    g_swap_us += t1 - t0; if (g_last_swap_end) g_frame_us += t0 - g_last_swap_end;
    g_last_swap_end = t1;
    present(c, 0, 0, 0, 0, nullptr);     // controller, sound pump, frame stats
#else
    glFinish();
    present(c, g_fb_va, kW * 4, kW, kH, nullptr);
#endif
    if (g_swaps % 300 == 0) stats_line();
    return 1;
}

// Extension list as an early-2000s driver would show it: short (the renderer
// logs it through a fixed-size buffer) and only what both platforms do.
const char* kExtensions = "GL_ARB_multitexture GL_EXT_bgra GL_EXT_compiled_vertex_array GL_EXT_texture_env_add ";

uint32_t gl_string(Cpu& c, uint32_t name) {
    static std::map<uint32_t, uint32_t> cache;
    auto it = cache.find(name); if (it != cache.end()) return it->second;
    const char* s = nullptr;
    switch (name) {
    case GL_VENDOR:     s = "Deus Ex Vita"; break;
    case GL_RENDERER:   s = "vitaGL"; break;
    case GL_VERSION:    s = "1.2.0"; break;
    case GL_EXTENSIONS: s = kExtensions; break;
    default: return 0;
    }
    uint32_t va = wx86_scratch_put_cstr(c, s);
    cache[name] = va;
    return va;
}

}  // namespace

void install_gl(d2rt::Bridge& br) {
    g_glbr = &br;
    // GL call counts: on by default on the Vita (one line per 300 frames).
    const char* gs = std::getenv("DX_GLSTATS");
#ifdef __vita__
    g_stats = !gs || std::atoi(gs) != 0;
#else
    g_stats = gs && std::atoi(gs) != 0;
#endif
#include "dx_gl_gen.inc"
    if (!(std::getenv("DX_GLBATCH") && std::atoi(std::getenv("DX_GLBATCH")) == 0)) install_batcher();
#ifdef __vita__
    // 640x480 game coordinates -> the 725x544 area in the middle of the screen.
    G("glViewport", 4, [](Cpu&, const uint32_t* A) -> uint32_t {
        GLCOUNT("glViewport");
        const float sx = 725.0f / 640.0f, sy = 544.0f / 480.0f;
        glViewport((GLint)(117.5f + (int32_t)A[0] * sx), (GLint)((int32_t)A[1] * sy), (GLsizei)(A[2] * sx + 0.5f), (GLsizei)(A[3] * sy + 0.5f));
        return 0;
    });
    G("glScissor", 4, [](Cpu&, const uint32_t* A) -> uint32_t {
        const float sx = 725.0f / 640.0f, sy = 544.0f / 480.0f;
        glScissor((GLint)(117.5f + (int32_t)A[0] * sx), (GLint)((int32_t)A[1] * sy), (GLsizei)(A[2] * sx + 0.5f), (GLsizei)(A[3] * sy + 0.5f));
        return 0;
    });
#endif
    G("glGetPointerv", 2, [](Cpu&, const uint32_t*) -> uint32_t { gl_missing("glGetPointerv"); return 0; });
    G("glGetString", 1, [](Cpu& c, const uint32_t* A) -> uint32_t { return gl_string(c, A[0]); });

    // WGL. One context; "sharing" lists is a no-op since there is only one.
    G("wglCreateContext", 1, [](Cpu& c, const uint32_t*) -> uint32_t { return make_context(c) ? 0x0C0DE001u : 0u; });
    G("wglCreateLayerContext", 2, [](Cpu& c, const uint32_t*) -> uint32_t { return make_context(c) ? 0x0C0DE001u : 0u; });
    G("wglDeleteContext", 1, [](Cpu&, const uint32_t*) -> uint32_t { return 1; });
    G("wglMakeCurrent", 2, [](Cpu&, const uint32_t*) -> uint32_t { return 1; });
    G("wglShareLists", 2, [](Cpu&, const uint32_t*) -> uint32_t { return 1; });
    G("wglCopyContext", 3, [](Cpu&, const uint32_t*) -> uint32_t { return 1; });
    G("wglGetCurrentContext", 0, [](Cpu&, const uint32_t*) -> uint32_t { return g_ctx_made ? 0x0C0DE001u : 0u; });
    G("wglGetCurrentDC", 0, [](Cpu&, const uint32_t*) -> uint32_t { return 0x00D20001u; });
    G("wglGetProcAddress", 1, [](Cpu& c, const uint32_t* A) -> uint32_t {
        std::string n = A[0] ? wx86_gread_mb(c, A[0], -1) : std::string();
        uint32_t va = g_glbr->shim_trap("opengl32.dll", n);
        log("[not-bound] wglGetProcAddress(%s) -> 0x%08x", n.c_str(), va);
        return va;
    });

    // Pixel formats: one, 32-bit colour, 24-bit depth, 8-bit stencil, double buffered.
    auto describe = [](Cpu& c) -> uint32_t {
        uint32_t pfd = c.arg(3), n = c.arg(2);
        if (pfd && n >= 40) {
            uint8_t b[40] = {};
            b[0] = 40; b[2] = 1;                         // nSize, nVersion
            const uint32_t flags = 0x4 | 0x20 | 0x1;     // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
            std::memcpy(b + 4, &flags, 4);
            b[8] = 0;                                    // PFD_TYPE_RGBA
            b[9] = 32;                                   // cColorBits
            b[10] = 8; b[11] = 16; b[12] = 8; b[13] = 8; b[14] = 8; b[15] = 0;   // R/G/B bits+shift
            b[16] = 8; b[17] = 24;                       // alpha
            b[23] = 24; b[24] = 8;                       // cDepthBits, cStencilBits
            c.write(pfd, b, 40);
        }
        return 1;                                        // number of formats
    };
    for (const char* dll : { "OPENGL32.dll", "GDI32.dll" }) {
        R(br, dll, "ChoosePixelFormat", 2, [](Cpu&) -> uint32_t { return 1; });
        R(br, dll, "SetPixelFormat", 3, [](Cpu&) -> uint32_t { return 1; });
        R(br, dll, "GetPixelFormat", 1, [](Cpu&) -> uint32_t { return 1; });
        R(br, dll, "DescribePixelFormat", 4, describe);
        R(br, dll, "SwapBuffers", 1, swap);
        std::string w = std::string("wgl") + "SwapBuffers";
        R(br, dll, w.c_str(), 1, swap);
    }
    R(br, "GDI32.dll", "GetDeviceGammaRamp", 2, [](Cpu& c) -> uint32_t {
        if (uint32_t p = c.arg(1)) for (uint32_t ch = 0; ch < 3; ++ch) for (uint32_t i = 0; i < 256; ++i) {
            uint16_t v = (uint16_t)(i * 257); c.write(p + (ch * 256 + i) * 2, &v, 2);
        }
        return 1;
    });
    R(br, "GDI32.dll", "SetDeviceGammaRamp", 2, [](Cpu&) -> uint32_t { return 1; });
    // One display mode (the game's 640x480), so mode lists end.
    // DEVMODEA: dmBitsPerPel at 0x68; DEVMODEW has 64 more bytes of names before it.
    auto enum_modes = [](Cpu& c, uint32_t off) -> uint32_t {
        uint32_t i = c.arg(1), dm = c.arg(2);
        if (i != 0 && i != 0xFFFFFFFFu && i != 0xFFFFFFFEu) return 0;
        if (dm) { c.write_u32(dm + off, 32); c.write_u32(dm + off + 4, kW); c.write_u32(dm + off + 8, kH); c.write_u32(dm + off + 16, 60); }
        return 1;
    };
    R(br, "USER32.dll", "EnumDisplaySettingsA", 3, [enum_modes](Cpu& c) { return enum_modes(c, 0x68); });
    R(br, "USER32.dll", "EnumDisplaySettingsW", 3, [enum_modes](Cpu& c) { return enum_modes(c, 0xA8); });
    R(br, "USER32.dll", "ChangeDisplaySettingsA", 2, [](Cpu&) -> uint32_t { return 0; });
    R(br, "USER32.dll", "ChangeDisplaySettingsW", 2, [](Cpu&) -> uint32_t { return 0; });
}

}  // namespace sc
