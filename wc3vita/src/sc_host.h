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
#include <initializer_list>

namespace sc {

// Host directory holding the user's own install (StarCraft.exe, storm.dll,
// StarDat.mpq, BrooDat.mpq, StarCraft.mpq, BroodWar.mpq, patch_rt.mpq ...).
extern std::string g_root;
// storm SBltROP3 calls done natively / left to storm's own generated code.
extern uint64_t g_blt_fast, g_blt_slow;
// The guest sees the game installed at this Windows path.
constexpr const char* kGuestDir = "C:\\Warcraft III";
// The game runs from its install folder.
constexpr const char* kGuestCwd = "C:\\Warcraft III";
// The exe, as named in System/ (a bridge module key).
constexpr const char* kExeName = "War3.exe";
// Record a module loaded by the boot harness (GetModuleHandle / GetProcAddress).
void register_module(const std::string& key, uint32_t base);
// Load base of the exe (GetModuleHandle(NULL)).
uint32_t exe_base();
// Mark a loaded image's writable sections as volatile code (sc_kernel32.cpp).
void mark_writable_sections(d2rt::Cpu& c, uint32_t base, const std::vector<uint8_t>& file, const std::string& name);
// Run guest stdcall `fn` to completion from inside a shim; returns its EAX (sc_user32.cpp).
uint32_t guest_call(d2rt::Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args);
bool run_nested(d2rt::Cpu& c, uint32_t eip, const char** fault);
// Write every mapped, in-use guest region to <dir>/mem_<addr>.bin (boot.cpp).
void dump_arena(d2rt::Cpu& c, const std::string& dir);

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
void install_gl(d2rt::Bridge& br);
// True when the game draws through OpenGL (DX_RENDER unset or "gl").
bool present_gl_mode();
bool present_ime_open();   // Vita: the on-screen keyboard is up (GL mode draws it in vglSwapBuffers)
void install_dx(d2rt::Bridge& br);       // Deus Ex extras (dx_shims.cpp)
void install_w3(d2rt::Bridge& br);       // WarCraft III extras (w3_shims.cpp)
void inline_hot_imports();               // TLS / last-error imports as guest x86 (w3_shims.cpp); after each module load
void install_dshow(d2rt::Bridge& br);    // stand-in DirectShow graph (w3_dshow.cpp)
void set_default_window_size(int w, int h);   // CW_USEDEFAULT window size (sc_user32.cpp)
std::string guest_stack(d2rt::Cpu& c);       // " module+0xoff ..." return addresses on the guest stack
void gl_render_size(uint32_t* w, uint32_t* h); // the size the game renders at (w3_gl.cpp, W3_RES)
void install_w3_trace(d2rt::Cpu& cpu, d2rt::Bridge& br);   // Storm file/alloc trace (w3_trace.cpp)
void trace_report(d2rt::Cpu& c);                       // one trace status line now
void trace_mem_line();                                 // memory line (guest VA, host heap, jump tables)
void install_net(d2rt::Bridge& br);      // offline Winsock (w3_net.cpp)
void install_gl(d2rt::Bridge& br);       // OPENGL32 + pixel formats (w3_gl.cpp)

// Pre-set registry values (sc_misc.cpp); key like "hkcu\\software\\...".
void reg_seed_dword(const std::string& key, const std::string& name, uint32_t v);
void reg_seed_sz(const std::string& key, const std::string& name, const std::string& v);
void reg_default_dword(const std::string& key, const std::string& name, uint32_t v);   // only if missing
// Call another registered shim natively, args on the guest stack (w3_shims.cpp).
uint32_t call_shim(d2rt::Cpu& c, const char* dll, const char* name, std::initializer_list<uint32_t> args);
// Queue a message for a window, as PostMessageA does (sc_user32.cpp).
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
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
