// r6_glthread.h — one thread owns the GL context (vitaGL / OSMesa are
// single-threaded) and runs the GL work the Direct3D shims queue up, so draw
// submission and the frame swap (with its vsync wait) leave the game's core.
// Jobs run in the order they were queued. Anything a job reads must be
// captured by value: the caller does not wait for it.
// R6_GLTHREAD=0 runs every job inline on the caller (the old behaviour).
#pragma once
#include <cstdint>
#include <functional>

namespace sc {
void gl_async(std::function<void()> fn);   // queue
void gl_sync(std::function<void()> fn);    // queue and wait for it (and everything before it)
void gl_finish();                          // wait until the queue is empty
// Frame pacing: call before queueing frame N's swap; waits until no more
// than `ahead` earlier swaps are still pending. The swap job calls gl_frame_done().
void gl_frame_begin(int ahead = 1);
void gl_frame_done();
bool gl_threaded();
// GL texture names live on the GL thread: the shims hand out their own ids.
uint32_t gl_tex_new();                     // caller side
uint32_t gl_tex_real(uint32_t id);         // GL thread: real name (created on first use)
void gl_tex_delete(uint32_t id);           // GL thread
uint32_t& gl_tex_params(uint32_t id);      // GL thread: cached sampler key per texture
}
