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
// W3_RES=WxH sets the size the game renders at (default 800x600 on the host).
#include "sc_host.h"
#include "sc_input.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/host_clock.h"

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

namespace {

uint32_t g_w = 800, g_h = 600;
bool     g_current = false;
uint64_t g_calls = 0, g_draws = 0, g_tris = 0;
#ifndef __vita__
OSMesaContext g_ctx = nullptr;
std::vector<uint8_t> g_fb;
#endif

float    F(Cpu& c, int k) { uint32_t u = c.arg(k); float f; std::memcpy(&f, &u, 4); return f; }
double   D(Cpu& c, int k) { uint64_t u = (uint64_t)c.arg(k) | ((uint64_t)c.arg(k + 1) << 32); double d; std::memcpy(&d, &u, 8); return d; }
template <class T = void>
T*       P(Cpu& c, uint32_t va) { return va ? (T*)c.hostptr(va, 1) : nullptr; }

void G(d2rt::Bridge& br, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    if (std::getenv("W3_TRACE_GL")) {
        std::string t = name;
        fn = [t, f = std::move(fn)](Cpu& c) { uint32_t r = f(c); log("[not-bound] gl %s(%x,%x,%x,%x) -> 0x%x", t.c_str(), c.arg(0), c.arg(1), c.arg(2), c.arg(3), r); return r; };
    }
    fn = [f = std::move(fn)](Cpu& c) { ++g_calls; return f(c); };
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string("OPENGL32.dll!") + name;
    br.register_shim("OPENGL32.dll", name, s);
}
void GDI(d2rt::Bridge& br, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string("GDI32.dll!") + name;
    br.register_shim("GDI32.dll", name, s);
}

bool make_current() {
    if (g_current) return true;
    if (const char* r = std::getenv("W3_RES")) { unsigned w, h; if (std::sscanf(r, "%ux%u", &w, &h) == 2) { g_w = w; g_h = h; } }
#ifdef __vita__
    // vitaGL is initialised by the presenter (present_init); nothing to bind.
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

void swap(Cpu& c) {
    static uint64_t frames = 0, t0 = 0, last_calls = 0, last_draws = 0, last_tris = 0;
    ++frames;
    uint64_t now = wx86_now_us(); if (!t0) t0 = now;
    if (now - t0 >= 5000000) {
        log("[not-bound] gl(5s): %llu frames, %llu calls, %llu draws, %llu triangles",
            (unsigned long long)frames, (unsigned long long)(g_calls - last_calls),
            (unsigned long long)(g_draws - last_draws), (unsigned long long)(g_tris - last_tris));
        frames = 0; t0 = now; last_calls = g_calls; last_draws = g_draws; last_tris = g_tris;
    }
#ifdef __vita__
    vglSwapBuffers(GL_FALSE);
    present_vita_frame(c);
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

void install_gl(d2rt::Bridge& br) {
    if (const char* r = std::getenv("W3_RES")) { unsigned w, h; if (std::sscanf(r, "%ux%u", &w, &h) == 2) { g_w = w; g_h = h; } }
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
        uint32_t t = g_br->shim_trap("OPENGL32.dll", n);
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
    G(br, "glPixelStorei", 2, [](Cpu& c) { glPixelStorei(c.arg(0), (GLint)c.arg(1)); return 0u; });
    G(br, "glReadPixels", 7, [](Cpu& c) {
        glReadPixels((GLint)c.arg(0), (GLint)c.arg(1), (GLsizei)c.arg(2), (GLsizei)c.arg(3), c.arg(4), c.arg(5), P(c, c.arg(6)));
        return 0u;
    });

    // ---- matrices, lighting, fog -------------------------------------------------
    G(br, "glMatrixMode", 1, [](Cpu& c) { glMatrixMode(c.arg(0)); return 0u; });
    G(br, "glLoadMatrixf", 1, [](Cpu& c) { glLoadMatrixf(P<const GLfloat>(c, c.arg(0))); return 0u; });
    G(br, "glLightf", 3, [](Cpu& c) { glLightf(c.arg(0), c.arg(1), F(c, 2)); return 0u; });
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
    G(br, "glDeleteTextures", 2, [](Cpu& c) { glDeleteTextures((GLsizei)c.arg(0), P<const GLuint>(c, c.arg(1))); return 0u; });
    G(br, "glBindTexture", 2, [](Cpu& c) { glBindTexture(c.arg(0), c.arg(1)); return 0u; });
    G(br, "glTexParameteri", 3, [](Cpu& c) { glTexParameteri(c.arg(0), c.arg(1), (GLint)c.arg(2)); return 0u; });
    G(br, "glTexEnvi", 3, [](Cpu& c) { glTexEnvi(c.arg(0), c.arg(1), (GLint)c.arg(2)); return 0u; });
    G(br, "glTexGeni", 3, [](Cpu& c) { glTexGeni(c.arg(0), c.arg(1), (GLint)c.arg(2)); return 0u; });
    G(br, "glTexImage2D", 9, [](Cpu& c) {
        glTexImage2D(c.arg(0), (GLint)c.arg(1), (GLint)c.arg(2), (GLsizei)c.arg(3), (GLsizei)c.arg(4), (GLint)c.arg(5),
                     c.arg(6), c.arg(7), P(c, c.arg(8)));
        return 0u;
    });
    G(br, "glTexSubImage2D", 9, [](Cpu& c) {
        glTexSubImage2D(c.arg(0), (GLint)c.arg(1), (GLint)c.arg(2), (GLint)c.arg(3), (GLsizei)c.arg(4), (GLsizei)c.arg(5),
                        c.arg(6), c.arg(7), P(c, c.arg(8)));
        return 0u;
    });

    // ---- vertex arrays ---------------------------------------------------------------
    G(br, "glVertexPointer", 4, [](Cpu& c) { glVertexPointer((GLint)c.arg(0), c.arg(1), (GLsizei)c.arg(2), P(c, c.arg(3))); return 0u; });
    G(br, "glNormalPointer", 3, [](Cpu& c) { glNormalPointer(c.arg(0), (GLsizei)c.arg(1), P(c, c.arg(2))); return 0u; });
    G(br, "glColorPointer", 4, [](Cpu& c) { glColorPointer((GLint)c.arg(0), c.arg(1), (GLsizei)c.arg(2), P(c, c.arg(3))); return 0u; });
    G(br, "glTexCoordPointer", 4, [](Cpu& c) { glTexCoordPointer((GLint)c.arg(0), c.arg(1), (GLsizei)c.arg(2), P(c, c.arg(3))); return 0u; });
    G(br, "glDrawElements", 4, [](Cpu& c) {
        ++g_draws; if (c.arg(0) == GL_TRIANGLES) g_tris += c.arg(1) / 3;
        glDrawElements(c.arg(0), (GLsizei)c.arg(1), c.arg(2), P(c, c.arg(3)));
        return 0u;
    });
    G(br, "glLockArraysEXT", 2, [](Cpu&) { return 0u; });
    G(br, "glUnlockArraysEXT", 0, [](Cpu&) { return 0u; });

    // ---- GL_ARB_multitexture (through wglGetProcAddress) -------------------------------
    G(br, "glActiveTextureARB", 1, [](Cpu& c) { glActiveTexture(c.arg(0)); return 0u; });
    G(br, "glClientActiveTextureARB", 1, [](Cpu& c) { glClientActiveTexture(c.arg(0)); return 0u; });
    G(br, "glMultiTexCoord2fARB", 3, [](Cpu& c) { glMultiTexCoord2f(c.arg(0), F(c, 1), F(c, 2)); return 0u; });
    G(br, "glMultiTexCoord2fvARB", 2, [](Cpu& c) { const GLfloat* v = P<const GLfloat>(c, c.arg(1)); glMultiTexCoord2f(c.arg(0), v[0], v[1]); return 0u; });
}

}  // namespace sc
