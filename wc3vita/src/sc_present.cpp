#include <thread>
#include <chrono>
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
// sc_present.cpp — frame presentation, host test build.
//
// Receives the game's 8-bit frame and palette. On the host it measures the
// frame interval and saves captures as 8-bit BMP (lossless, palette kept;
// tools/bmp2png.py turns them into PNG). The Vita build replaces this file
// with the sceGxm palette pass (640x480 -> 725x544 pillarboxed).
//
// SC_SHOTS=<dir>   save frames there
// SC_SHOT_EVERY=N  save every Nth presented frame (default 30)
// SC_PAD_SCRIPT=<file>  drive the controller from a script ("<frame> goto x y" warps the cursor): lines of
//                  "<frame> <buttons-hex> <lx> <ly> <rx> <ry>", each held until
//                  the next line; frames are counted at a fixed 1/60 s so a
//                  script means the same thing on any host speed.
#ifndef __vita__
#include "sc_host.h"
#include "sc_input.h"

#include "runtime/host_clock.h"
#include "runtime/ds_emul.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace sc {

namespace {
struct PadStep { uint64_t frame; Pad pad; std::string text; int wx = -1, wy = -1; };
std::vector<PadStep> load_script(const char* path) {
    std::vector<PadStep> v; FILE* f = std::fopen(path, "r"); if (!f) return v;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long fr; unsigned b, lx, ly, rx, ry;
        unsigned long long tf; char txt[200];
        int gx, gy;
        if (std::sscanf(line, "%llu goto %d %d", &tf, &gx, &gy) == 3) {   // "<frame> goto x y": test-only cursor warp
            PadStep s; s.frame = tf; s.wx = gx; s.wy = gy; s.pad = v.empty() ? Pad() : v.back().pad; v.push_back(s); continue;
        }
        if (std::sscanf(line, "%llu touch %d %d", &tf, &gx, &gy) == 3) {  // "<frame> touch x y": finger on the screen (640x480)
            PadStep s; s.frame = tf; s.pad = v.empty() ? Pad() : v.back().pad; s.pad.touch = true; s.pad.tx = gx; s.pad.ty = gy; v.push_back(s); continue;
        }
        if (std::sscanf(line, "%llu untouc%1[h]", &tf, txt) == 2) {          // "<frame> untouch": finger lifted
            PadStep s; s.frame = tf; s.pad = v.empty() ? Pad() : v.back().pad; s.pad.touch = false; v.push_back(s); continue;
        }
        if (std::sscanf(line, "%llu text %199[^\n]", &tf, txt) == 2) {   // "<frame> text <chars>"
            PadStep s; s.frame = tf; s.text = txt; s.pad = v.empty() ? Pad() : v.back().pad; v.push_back(s); continue;
        }
        if (line[0] == '#' || std::sscanf(line, "%llu %x %u %u %u %u", &fr, &b, &lx, &ly, &rx, &ry) != 6) continue;
        PadStep s; s.frame = fr; if (!v.empty()) { s.pad.touch = v.back().pad.touch; s.pad.tx = v.back().pad.tx; s.pad.ty = v.back().pad.ty; }
        s.pad.buttons = b; s.pad.lx = lx; s.pad.ly = ly; s.pad.rx = rx; s.pad.ry = ry;
        v.push_back(s);
    }
    std::fclose(f); return v;
}
uint64_t g_frames = 0, g_last_us = 0, g_win_start = 0, g_win_frames = 0;
bool g_shot_now = false;   // SC_PAD_LIVE "shot": save the next frame

void write_bmp(const char* path, d2rt::Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h, const uint32_t* pal) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t row = (w + 3) & ~3u, data = row * h, off = 14 + 40 + 1024;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v = off + data; std::memcpy(hdr + 2, &v, 4); std::memcpy(hdr + 10, &off, 4);
    v = 40; std::memcpy(hdr + 14, &v, 4);
    std::memcpy(hdr + 18, &w, 4);
    int32_t ih = (int32_t)h; std::memcpy(hdr + 22, &ih, 4);   // bottom-up rows
    uint16_t planes = 1, bpp = 8; std::memcpy(hdr + 26, &planes, 2); std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(hdr + 34, &data, 4);
    v = 256; std::memcpy(hdr + 46, &v, 4);
    std::fwrite(hdr, 1, 54, f);
    for (int i = 0; i < 256; ++i) {   // RGBQUAD = b, g, r, 0
        uint8_t q[4] = { (uint8_t)(pal[i] >> 16), (uint8_t)(pal[i] >> 8), (uint8_t)pal[i], 0 };
        std::fwrite(q, 1, 4, f);
    }
    std::vector<uint8_t> line(row, 0);
    for (int y = (int)h - 1; y >= 0; --y) { c.read(pixels + y * pitch, line.data(), w); std::fwrite(line.data(), 1, row, f); }
    std::fclose(f);
}
// 32-bit BGRX frame (Deus Ex's software renderer) as a 24-bit BMP.
void write_bmp32(const char* path, d2rt::Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t row = (w * 3 + 3) & ~3u, data = row * h, off = 54;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v = off + data; std::memcpy(hdr + 2, &v, 4); std::memcpy(hdr + 10, &off, 4);
    v = 40; std::memcpy(hdr + 14, &v, 4);
    std::memcpy(hdr + 18, &w, 4);
    int32_t ih = (int32_t)h; std::memcpy(hdr + 22, &ih, 4);
    uint16_t planes = 1, bpp = 24; std::memcpy(hdr + 26, &planes, 2); std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(hdr + 34, &data, 4);
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> src(w * 4), line(row, 0);
    for (int y = (int)h - 1; y >= 0; --y) {
        c.read(pixels + y * pitch, src.data(), w * 4);
        for (uint32_t x = 0; x < w; ++x) { line[3 * x] = src[4 * x]; line[3 * x + 1] = src[4 * x + 1]; line[3 * x + 2] = src[4 * x + 2]; }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}
}  // namespace

extern "C" int d2rt_natprof_lines(char* out, unsigned n, int topk);
// Host stall watchdog: if no frame arrives for 10 s, dump the guest threads
// (same report as the Vita build) so a deadlock can be traced on the host.
static void start_stall_watch() {
    static bool started = false; if (started) return; started = true;
    std::thread([] {
        uint64_t last = 0; int dumps = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const uint64_t f = g_frames;
            if (f == last && f > 0 && dumps < 3) {
                ++dumps;
                log("[not-bound] [cause: guest stall] no frame for 10 s; guest threads:");
                if (d2rt::ThreadScheduler* sc = wx86_sched()) sc->dump_threads_to([](const char* m) { log("[not-bound]   %s", m); });
                if (std::getenv("WX86_NATPROF")) {   // which shims the stalled guest is calling
                    static char buf[32768];
                    const char* k = std::getenv("W3_NATPROF_TOP");
                    if (d2rt_natprof_lines(buf, sizeof buf, k ? std::atoi(k) : 25) > 0) log("[not-bound] shims in the last 10 s:\n%s", buf);
                }
            }
            last = f;
        }
    }).detach();
}

extern "C" { extern uint64_t dyn86_jp_created, dyn86_jp_invalid; }
// Host profiling (engine built with -DD2_EMITPROF): W3_EPCSV=<file> writes
// every translated block's execution count every 300 frames.
extern "C" void d2ep_csv(const char* path) __attribute__((weak));
extern "C" void d2ep_dump(const char* path, int topn) __attribute__((weak));
extern "C" void d2ep_report(void (*out)(const char*)) __attribute__((weak));
extern "C" void dyn86_jit_clock_tick(int chunks);
extern "C" uint32_t dyn86_clock_probes, dyn86_clock_fallbacks;
static void present_pump(d2rt::Cpu& c) {
    dyn86_jit_clock_tick(1);   // JIT clock: unlink one arena chunk per frame (custommem.c)
    static const char* epcsv = std::getenv("W3_EPCSV");
    if (epcsv && d2ep_csv && g_frames && g_frames % 300 == 0) d2ep_csv(epcsv);
    // W3_EPDUMP=<file>: the emitted ARM of the 200 costliest blocks, at frame 1200.
    static const char* epdump = std::getenv("W3_EPDUMP");
    if (epdump && d2ep_dump && g_frames == 1200) {
        d2ep_dump(epdump, 200);
        if (d2ep_report) d2ep_report([](const char* l) { log("[not-bound] ep: %s", l); });
    }
    // Host: which shims the frames call, and how often (PROF_COUNTERS engines).
    if (g_br && g_frames && g_frames % 300 == 0) {
        char line[2048];
        if (g_br->prof_window(line, sizeof line, 16) > 0) log("[not-bound] shim time (300 frames):%s", line);
    }
    uint64_t now = wx86_now_us();
    // W3_SPIKE=1 (host, with WX86_NATPROF=1): a frame that takes over twice the
    // median of the last 31 is logged with the shims it spent time in and the
    // blocks it translated, to find one-off stalls (a unit appearing, say).
    static const bool spike = std::getenv("W3_SPIKE") && std::getenv("WX86_NATPROF");
    if (spike && g_last_us) {
        static uint64_t hist[31]; static unsigned nh = 0; static uint64_t pj = 0;
        static char buf[8192];
        const uint64_t dt = now - g_last_us, nj = dyn86_jp_created - pj; pj = dyn86_jp_created;
        d2rt_natprof_lines(buf, sizeof buf, 10);   // also resets the per-frame window
        GlFrameStats fs; gl_frame_stats(&fs);
        uint64_t s[31]; unsigned k = nh < 31 ? nh : 31; std::memcpy(s, hist, k * sizeof s[0]);
        std::sort(s, s + k);
        if (k >= 15 && dt > 2 * s[k / 2])
            log("[not-bound] spike: frame %llu took %llu ms (median %llu), %llu blocks translated, texture uploads %u (%u KiB), new images %u (%u KiB); shims:\n%s",
                (unsigned long long)g_frames, (unsigned long long)(dt / 1000), (unsigned long long)(s[k / 2] / 1000),
                (unsigned long long)nj, fs.sub_n, fs.sub_bytes >> 10, fs.img_n, fs.img_bytes >> 10, buf);
        hist[nh++ % 31] = dt;
    }
    start_stall_watch();
    ++g_frames; ++g_win_frames;
    if (!g_win_start) g_win_start = now;
    if (now - g_win_start >= 5000000) {
        double fps = g_win_frames * 1e6 / double(now - g_win_start);
        // Host numbers say nothing about the Vita; logged to watch the flow only.
        uint32_t hc, hp, vc, vp; guest_mem(&hc, &hp, &vc, &vp);
        log("[not-bound] [host qemu, not representative] presented %.1f frames/s (%llu total) heap %u/%u KiB va %u/%u KiB (cur/peak)",
            fps, (unsigned long long)g_frames, hc >> 10, hp >> 10, vc >> 10, vp >> 10);
        char line[400]; io_window_line(line, sizeof line, (now - g_win_start) / 1e6); log("[not-bound] %s", line);
        if (d2rt::dsound::stat_line(line, sizeof line) > 0) log("[not-bound] %s", line);
        {   // JIT churn per window (WX86_JITPROFILE=1 arms the counters)
            static uint64_t p_new = 0, p_inv = 0;
            log("[not-bound] jit(5s): new=%llu invalidated=%llu clock probes=%u fallbacks=%u", (unsigned long long)(dyn86_jp_created - p_new),
                (unsigned long long)(dyn86_jp_invalid - p_inv), dyn86_clock_probes, dyn86_clock_fallbacks);
            p_new = dyn86_jp_created; p_inv = dyn86_jp_invalid;
        }
        g_win_start = now; g_win_frames = 0;
    }
    g_last_us = now;
    d2rt::dsound::frame_pump(c);
    static std::vector<PadStep> script = std::getenv("SC_PAD_SCRIPT") ? load_script(std::getenv("SC_PAD_SCRIPT")) : std::vector<PadStep>();
    // SC_PAD_LIVE=<file>: drive a running host session. Every 10 frames the
    // steps in that file are added to the script and the file is removed;
    // "+N" in place of a frame number means N frames from now, and a line
    // "shot" saves the next frame to SC_SHOTS.
    static const char* live = std::getenv("SC_PAD_LIVE");
    if (live && g_frames % 10 == 0) {
        if (FILE* f = std::fopen(live, "r")) {
            const std::string tmp = std::string(live) + ".abs";
            if (FILE* o = std::fopen(tmp.c_str(), "w")) {
                char line[256];
                while (std::fgets(line, sizeof line, f)) {
                    if (!std::strncmp(line, "shot", 4)) { g_shot_now = true; continue; }
                    if (line[0] == '+') { char* end = nullptr; unsigned long long d = std::strtoull(line + 1, &end, 10);
                                          std::fprintf(o, "%llu%s", (unsigned long long)g_frames + d, end); }
                    else std::fputs(line, o);
                }
                std::fclose(o);
                std::vector<PadStep> add = load_script(tmp.c_str());
                script.insert(script.end(), add.begin(), add.end());
                log("[not-bound] pad live: %u steps added at frame %llu", (unsigned)add.size(), (unsigned long long)g_frames);
            }
            std::fclose(f); std::remove(live);
        }
    }
    if (!script.empty()) {
        Pad cur; for (auto& st : script) {
            if (st.frame <= g_frames) cur = st.pad;
            if (st.frame == g_frames && !st.text.empty()) input_text(st.text);
            if (st.frame == g_frames && st.wx >= 0) input_warp(st.wx, st.wy);
        }
        input_tick(cur, 16667);
    }
}

void present(d2rt::Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h, const uint32_t* pal) {
    present_pump(c);
    static const char* dir = std::getenv("SC_SHOTS");
    static uint64_t every = std::getenv("SC_SHOT_EVERY") ? std::strtoull(std::getenv("SC_SHOT_EVERY"), nullptr, 10) : 30;
    if (dir && (g_frames == 1 || g_frames % every == 0 || g_shot_now)) {
        g_shot_now = false;
        mkdir(dir, 0777);
        char p[512]; std::snprintf(p, sizeof p, "%s/frame_%06llu.bmp", dir, (unsigned long long)g_frames);
        if (pal) write_bmp(p, c, pixels, pitch, w, h, pal);
        else write_bmp32(p, c, pixels, pitch, w, h);
    }
}


// A finished OpenGL frame (WarCraft III): host RGBA rows, bottom row first,
// as glReadPixels returns them (which is also BMP's row order).
void present_rgba(d2rt::Cpu& c, const uint8_t* rgba, uint32_t w, uint32_t h) {
    present_pump(c);
    static const char* dir = std::getenv("SC_SHOTS");
    static uint64_t every = std::getenv("SC_SHOT_EVERY") ? std::strtoull(std::getenv("SC_SHOT_EVERY"), nullptr, 10) : 30;
    if (!dir || !(g_frames == 1 || g_frames % every == 0 || g_shot_now)) return;
    g_shot_now = false;
    mkdir(dir, 0777);
    char p[512]; std::snprintf(p, sizeof p, "%s/frame_%06llu.bmp", dir, (unsigned long long)g_frames);
    FILE* f = std::fopen(p, "wb"); if (!f) return;
    uint32_t row = (w * 3 + 3) & ~3u, data = row * h, off = 54;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v = off + data; std::memcpy(hdr + 2, &v, 4); std::memcpy(hdr + 10, &off, 4);
    v = 40; std::memcpy(hdr + 14, &v, 4); std::memcpy(hdr + 18, &w, 4);
    int32_t ih = (int32_t)h; std::memcpy(hdr + 22, &ih, 4);
    uint16_t planes = 1, bpp = 24; std::memcpy(hdr + 26, &planes, 2); std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(hdr + 34, &data, 4);
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) { line[3 * x] = s[4 * x + 2]; line[3 * x + 1] = s[4 * x + 1]; line[3 * x + 2] = s[4 * x]; }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

}  // namespace sc
#endif  // !__vita__
