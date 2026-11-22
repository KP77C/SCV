// r6_jobs.h — helper threads for big host-side loops (pixel conversion,
// blits), so they don't all run on the game's core. The game's own x86 code
// never runs here: only plain host loops over memory that the caller owns
// for the duration of the call.
#pragma once
#include <cstdint>
#include <functional>

namespace sc {
// Run fn(begin, end) over [0, n) split across the helpers and the caller;
// returns when every part is done. Small n (< min_split) runs inline.
void par_rows(uint32_t n, const std::function<void(uint32_t, uint32_t)>& fn, uint32_t min_split = 64);
// Number of helper threads running (0 = all inline).
int jobs_helpers();
}
