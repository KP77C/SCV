// r6_jobs.cpp — see r6_jobs.h. Two helpers: core 2 (otherwise only the
// logger and the sampler) and core 1 (the sound mixer, a few % busy).
// R6_JOBS=0 runs everything inline on the caller.
#include "r6_jobs.h"
#include "sc_host.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <condition_variable>
#include <thread>
#endif

namespace sc {
namespace {
constexpr int kHelpers = 2;

struct Part { uint32_t b = 0, e = 0; };
const std::function<void(uint32_t, uint32_t)>* g_fn = nullptr;
Part g_part[kHelpers];
int g_running = 0;            // helpers started
std::mutex g_call_mx;         // one par_rows at a time (others run inline)

#ifdef __vita__
SceUID g_go[kHelpers], g_done;
int helper_main(SceSize, void* argp) {
    const int i = *(const int*)argp;
    for (;;) {
        if (sceKernelWaitSema(g_go[i], 1, nullptr) < 0) return 0;
        const Part p = g_part[i];
        if (p.e > p.b) (*g_fn)(p.b, p.e);
        sceKernelSignalSema(g_done, 1);
    }
}
void start() {
    static const int ids[kHelpers] = { 0, 1 };
    static const int masks[kHelpers] = { SCE_KERNEL_CPU_MASK_USER_2, SCE_KERNEL_CPU_MASK_USER_1 };
    g_done = sceKernelCreateSema("r6_jobs_done", 0, 0, kHelpers, nullptr);
    for (int i = 0; i < kHelpers; ++i) {
        g_go[i] = sceKernelCreateSema("r6_jobs_go", 0, 0, 1, nullptr);
        SceUID t = sceKernelCreateThread("r6_jobs", helper_main, 0x10000100, 0x4000, 0, masks[i], nullptr);
        if (t < 0 || g_go[i] < 0 || g_done < 0) { log("[not-bound] jobs: helper %d not started (0x%08x)", i, (unsigned)t); break; }
        sceKernelStartThread(t, sizeof(int), (void*)&ids[i]);
        ++g_running;
    }
}
void kick(int i) { sceKernelSignalSema(g_go[i], 1); }
void wait_all(int n) { for (int i = 0; i < n; ++i) sceKernelWaitSema(g_done, 1, nullptr); }
#else
std::mutex g_mx; std::condition_variable g_cv, g_cv_done;
int g_gen[kHelpers] = {}, g_ndone = 0;
void start() {
    for (int i = 0; i < kHelpers; ++i) {
        std::thread([i] {
            int seen = 0;
            for (;;) {
                { std::unique_lock<std::mutex> lk(g_mx); g_cv.wait(lk, [&] { return g_gen[i] != seen; }); seen = g_gen[i]; }
                const Part p = g_part[i];
                if (p.e > p.b) (*g_fn)(p.b, p.e);
                { std::lock_guard<std::mutex> lk(g_mx); ++g_ndone; } g_cv_done.notify_all();
            }
        }).detach();
        ++g_running;
    }
}
void kick(int i) { { std::lock_guard<std::mutex> lk(g_mx); ++g_gen[i]; } g_cv.notify_all(); }
void wait_all(int n) { std::unique_lock<std::mutex> lk(g_mx); g_cv_done.wait(lk, [&] { return g_ndone >= n; }); g_ndone -= n; }
#endif

bool ready() {
    static int state = 0;   // 0 unknown, 1 on, -1 off
    if (!state) {
        const char* e = std::getenv("R6_JOBS");
        if (e && e[0] == '0') state = -1;
        else { start(); state = g_running ? 1 : -1; log("[not-bound] jobs: %d helper threads (cores 2 and 1) for pixel work", g_running); }
    }
    return state > 0;
}
}  // namespace

int jobs_helpers() { return g_running; }

void par_rows(uint32_t n, const std::function<void(uint32_t, uint32_t)>& fn, uint32_t min_split) {
    if (n < min_split || !ready()) { fn(0, n); return; }
    std::unique_lock<std::mutex> lk(g_call_mx, std::try_to_lock);
    if (!lk.owns_lock()) { fn(0, n); return; }
    // Caller takes a slightly bigger first share (it starts at once).
    const int parts = g_running + 1;
    const uint32_t step = (n + parts - 1) / parts;
    g_fn = &fn;
    int used = 0;
    for (int i = 0; i < g_running; ++i) {
        const uint32_t b = std::min(n, step * (uint32_t)(i + 1)), e = std::min(n, b + step);
        g_part[i] = { b, e };
        if (e > b) { kick(i); ++used; }
    }
    fn(0, std::min(n, step));
    wait_all(used);
    g_fn = nullptr;
}

}  // namespace sc
