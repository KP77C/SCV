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

// Register one OPENGL32 entry point. The shim reads its arguments straight
// from the guest stack (one host pointer for all of them).
void G(const char* name, uint32_t words, uint32_t (*fn)(Cpu&, const uint32_t*)) {
    Shim s; s.argc = words; s.stdcall_cleanup = true;
    s.tag = std::string("OPENGL32.dll!") + name;
    s.fn = [fn, words](Cpu& c) -> uint32_t {
        static const uint32_t zero[16] = {};
        const uint32_t* A = words ? (const uint32_t*)c.hostptr(c.reg(d2rt::R_ESP) + 4, 4 * words) : zero;
        return fn(c, A ? A : zero);
    };
    g_glbr->register_shim("OPENGL32.dll", name, s);
}

void R(d2rt::Bridge& br, const char* dll, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string(dll) + "!" + name;
    br.register_shim(dll, name, s);
}

// ---- context ---------------------------------------------------------------
bool g_ctx_made = false;
uint32_t g_fb_va = 0;                    // host: the colour buffer (guest memory)
uint64_t g_swaps = 0;
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
    for (auto& kv : *g_counts) kv.second = 0;
}

uint32_t swap(Cpu& c) {
    ++g_swaps;
#ifdef __vita__
    vglSwapBuffers(GL_FALSE);
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
    g_stats = std::getenv("DX_GLSTATS") && std::atoi(std::getenv("DX_GLSTATS"));
#include "dx_gl_gen.inc"
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
