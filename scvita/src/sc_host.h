// sc_host.h — StarCraft Vita port: host-side state shared by the boot
// harness and the port's own Win32 shims.
//
// Logging rule for this port: every performance or error line carries what
// is bound and what is not, in brackets, e.g.
//   [not-bound] [cause: missing shim] ...
//   [CPU-bound] [cause: main-thread stall] ...
#pragma once
#include "runtime/bridge.h"
#include "runtime/cpu.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sc {

// Host directory holding the user's own install (StarCraft.exe, storm.dll,
// StarDat.mpq, BrooDat.mpq, StarCraft.mpq, BroodWar.mpq, patch_rt.mpq ...).
extern std::string g_root;
// storm SBltROP3 calls done natively / left to storm's own generated code.
extern uint64_t g_blt_fast, g_blt_slow;
// The guest sees the game installed at this Windows path.
constexpr const char* kGuestDir = "C:\\StarCraft";

extern d2rt::Bridge* g_br;
extern d2rt::Cpu*    g_cpu;

// Guest path (any case, '\' or '/', absolute or relative to the game dir)
// -> host path. Existing components are matched case-insensitively; missing
// trailing components keep the guest's spelling (for creation).
std::string resolve_path(const std::string& guest);

// Read a whole host file (empty vector on failure).
std::vector<uint8_t> read_file(const std::string& host);

// Write a NUL-terminated string into guest memory (truncated to cap, cap>0).
// Returns the length written, excluding the NUL.
uint32_t put_str(d2rt::Cpu& c, uint32_t dst, const std::string& s, uint32_t cap);

// One trace line per first use of an unshimmed import, in call order.
void note_unshimmed(d2rt::Cpu& c, const std::string& tag);

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Port-owned shims (last registration wins, so install after the engine's).
void install_kernel32(d2rt::Bridge& br);
void install_misc(d2rt::Bridge& br);
void install_user32(d2rt::Bridge& br);
void install_ddraw(d2rt::Bridge& br);

// Queue a key press (down+up) for the focused window (sc_user32.cpp).
void post_key(uint32_t vk);
// Queue an input message (WM_KEY*, WM_*BUTTON*, WM_MOUSEMOVE) for the focused window.
void post_input(uint32_t msg, uint32_t wp, uint32_t lp);

#ifdef __vita__
// GPU presenter, controller and on-screen keyboard (sc_present_vita.cpp).
bool present_init();
void present_shutdown();
void log_flush();          // write queued log lines now (Vita logger thread)
#endif

// File I/O over the last window, then reset (sc_kernel32.cpp).
void io_window_line(char* out, unsigned n, double span_s);

// Guest heap / VirtualAlloc use: current and peak bytes (boot.cpp).
void guest_mem(uint32_t* heap_cur, uint32_t* heap_peak, uint32_t* va_cur, uint32_t* va_peak);

uint32_t find_resource(d2rt::Cpu& c, uint32_t hmod, uint32_t name, uint32_t type);
} // namespace sc
