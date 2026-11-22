// r6_glthread.cpp — see r6_glthread.h. The GL thread runs on core 2 at the
// game thread's priority.
#include "r6_glthread.h"
#include "sc_host.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#ifdef __vita__
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <thread>
#endif

namespace sc {
namespace {
std::mutex g_mx;
std::condition_variable g_cv_work, g_cv_idle, g_cv_frame;
std::deque<std::function<void()>> g_q;
bool g_busy = false;
int g_state = 0;   // 0 unknown, 1 threaded, -1 inline
uint64_t g_frames_queued = 0, g_frames_done = 0;

void worker() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv_work.wait(lk, [] { return !g_q.empty(); });
            fn = std::move(g_q.front()); g_q.pop_front(); g_busy = true;
        }
        fn();
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_busy = false;
            if (g_q.empty()) g_cv_idle.notify_all();
        }
    }
}
#ifdef __vita__
int worker_main(SceSize, void*) { worker(); return 0; }
#endif

bool threaded() {
    if (!g_state) {
        const char* e = std::getenv("R6_GLTHREAD");
        if (e && e[0] == '0') { g_state = -1; log("[not-bound] GL thread: off (R6_GLTHREAD=0), GL runs on the game thread"); return false; }
#ifdef __vita__
        SceUID t = sceKernelCreateThread("r6_gl", worker_main, 0x10000100, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (t < 0 || sceKernelStartThread(t, 0, nullptr) < 0) { g_state = -1; log("[not-bound] GL thread: not started (0x%08x)", (unsigned)t); return false; }
#else
        std::thread(worker).detach();
#endif
        g_state = 1;
        log("[not-bound] GL thread: started (core 2): draw submission and the frame swap leave the game thread");
    }
    return g_state > 0;
}

// Textures: id -> real GL name (0 = not created yet), sampler key.
struct T { GLuint name = 0; uint32_t params = 0xFFFFFFFFu; };
std::vector<T> g_tex(1);
std::mutex g_tex_mx;   // id allocation only (caller side)
}  // namespace

bool gl_threaded() { return threaded(); }

void gl_async(std::function<void()> fn) {
    if (!threaded()) { fn(); return; }
    { std::lock_guard<std::mutex> lk(g_mx); g_q.push_back(std::move(fn)); }
    g_cv_work.notify_one();
}
void gl_finish() {
    if (!threaded()) return;
    std::unique_lock<std::mutex> lk(g_mx);
    g_cv_idle.wait(lk, [] { return g_q.empty() && !g_busy; });
}
void gl_sync(std::function<void()> fn) {
    if (!threaded()) { fn(); return; }
    gl_async(std::move(fn));
    gl_finish();
}
void gl_frame_begin(int ahead) {
    if (!threaded()) return;
    std::unique_lock<std::mutex> lk(g_mx);
    g_cv_frame.wait(lk, [ahead] { return g_frames_queued - g_frames_done < (uint64_t)ahead; });
    ++g_frames_queued;
}
void gl_frame_done() {
    if (g_state <= 0) return;
    { std::lock_guard<std::mutex> lk(g_mx); ++g_frames_done; }
    g_cv_frame.notify_all();
}

uint32_t gl_tex_new() {
    std::lock_guard<std::mutex> lk(g_tex_mx);
    static uint32_t next = 1;
    return next++;
}
uint32_t gl_tex_real(uint32_t id) {
    if (id >= g_tex.size()) g_tex.resize(id + 256);
    T& t = g_tex[id];
    if (!t.name) { glGenTextures(1, &t.name); t.params = 0xFFFFFFFFu; }
    return t.name;
}
void gl_tex_delete(uint32_t id) {
    if (id >= g_tex.size() || !g_tex[id].name) return;
    glDeleteTextures(1, &g_tex[id].name); g_tex[id] = T{};
}
uint32_t& gl_tex_params(uint32_t id) {
    if (id >= g_tex.size()) g_tex.resize(id + 256);
    return g_tex[id].params;
}

}  // namespace sc
