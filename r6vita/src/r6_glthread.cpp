// r6_glthread.cpp — see r6_glthread.h. The GL thread runs on core 2 at the
// game thread's priority.
#include "r6_glthread.h"
#include "sc_host.h"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <vector>
#ifndef __vita__
#include <condition_variable>
#include <mutex>
#endif

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
// Vita: kernel mutex + semaphores only. pthread condition variables crash
// when waited on from a thread made with sceKernelCreateThread (build 4h:
// fault in pthread_mutex_unlock inside pthread_cond_wait on r6_gl).
std::deque<std::function<void()>> g_q;
int g_state = 0;   // 0 unknown, 1 threaded, -1 inline
#ifdef __vita__
SceUID g_qmx = -1;     // protects g_q
SceUID g_work = -1;    // one count per queued job
SceUID g_frames = -1;  // free frame slots (gl_frame_begin takes, gl_frame_done gives)
struct Lock { Lock() { sceKernelLockMutex(g_qmx, 1, nullptr); } ~Lock() { sceKernelUnlockMutex(g_qmx, 1); } };
void worker() {
    for (;;) {
        if (sceKernelWaitSema(g_work, 1, nullptr) < 0) return;
        std::function<void()> fn;
        { Lock lk; fn = std::move(g_q.front()); g_q.pop_front(); }
        fn();
    }
}
int worker_main(SceSize, void*) { worker(); return 0; }
void push(std::function<void()> fn) {
    { Lock lk; g_q.push_back(std::move(fn)); }
    sceKernelSignalSema(g_work, 1);
}
#else
std::mutex g_mx;
std::condition_variable g_cv_work, g_cv_frame;
uint64_t g_frames_queued = 0, g_frames_done = 0;
void worker() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv_work.wait(lk, [] { return !g_q.empty(); });
            fn = std::move(g_q.front()); g_q.pop_front();
        }
        fn();
    }
}
void push(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lk(g_mx); g_q.push_back(std::move(fn)); }
    g_cv_work.notify_one();
}
#endif

bool threaded() {
    if (!g_state) {
        const char* e = std::getenv("R6_GLTHREAD");
        if (e && e[0] == '0') { g_state = -1; log("[not-bound] GL thread: off (R6_GLTHREAD=0), GL runs on the game thread"); return false; }
#ifdef __vita__
        g_qmx = sceKernelCreateMutex("r6_gl_q", 0, 0, nullptr);
        g_work = sceKernelCreateSema("r6_gl_work", 0, 0, 0x7FFFFFFF, nullptr);
        g_frames = sceKernelCreateSema("r6_gl_frames", 0, 1, 1, nullptr);
        SceUID t = -1;
        if (g_qmx >= 0 && g_work >= 0 && g_frames >= 0)
            t = sceKernelCreateThread("r6_gl", worker_main, 0x10000100, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (t < 0 || sceKernelStartThread(t, 0, nullptr) < 0) {
            g_state = -1;
            log("[not-bound] GL thread: not started (thread 0x%08x, mutex 0x%08x, semas 0x%08x 0x%08x), GL runs on the game thread",
                (unsigned)t, (unsigned)g_qmx, (unsigned)g_work, (unsigned)g_frames);
            return false;
        }
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
std::atomic<uint32_t> g_tex_next{1};   // id allocation (caller side)
}  // namespace

bool gl_threaded() { return threaded(); }

void gl_async(std::function<void()> fn) {
    if (!threaded()) { fn(); return; }
    push(std::move(fn));
}
void gl_finish() {
    if (!threaded()) return;
#ifdef __vita__
    SceUID done = sceKernelCreateSema("r6_gl_fence", 0, 0, 1, nullptr);
    if (done < 0) { log("[not-bound] GL thread: fence sema failed (0x%08x)", (unsigned)done); return; }
    push([done] { sceKernelSignalSema(done, 1); });
    sceKernelWaitSema(done, 1, nullptr);
    sceKernelDeleteSema(done);
#else
    std::mutex m; std::condition_variable cv; bool ok = false;
    push([&] { { std::lock_guard<std::mutex> lk(m); ok = true; } cv.notify_all(); });
    std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return ok; });
#endif
}
void gl_sync(std::function<void()> fn) {
    if (!threaded()) { fn(); return; }
    push(std::move(fn));
    gl_finish();
}
void gl_frame_begin(int ahead) {
    if (!threaded()) return;
#ifdef __vita__
    (void)ahead;   // the semaphore holds one slot
    sceKernelWaitSema(g_frames, 1, nullptr);
#else
    std::unique_lock<std::mutex> lk(g_mx);
    g_cv_frame.wait(lk, [ahead] { return g_frames_queued - g_frames_done < (uint64_t)ahead; });
    ++g_frames_queued;
#endif
}
void gl_frame_done() {
    if (g_state <= 0) return;
#ifdef __vita__
    sceKernelSignalSema(g_frames, 1);
#else
    { std::lock_guard<std::mutex> lk(g_mx); ++g_frames_done; }
    g_cv_frame.notify_all();
#endif
}

uint32_t gl_tex_new() { return g_tex_next.fetch_add(1); }
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
