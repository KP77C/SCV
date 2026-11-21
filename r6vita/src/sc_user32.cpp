// sc_user32.cpp — the port's own window manager: window classes, windows,
// the message queue, timers, and guest window-procedure callbacks.
//
// There is one real "screen" on the Vita, so windows are bookkeeping only:
// what matters is that the game's window procedure receives the messages
// it expects (activation, focus, timers, input) in Windows order.
//
// Guest callbacks (DispatchMessageA, SendMessageA, CallWindowProcA) are
// tail calls: the shim rewrites the guest stack so the window procedure
// returns straight to the original call site, then redirects EIP to it.
// No nested dynarec run, so no shared-stack hazard.
#include "sc_host.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/guest_thread.h"
#include "sc_input.h"
#include "runtime/win32_shims_window.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/host_clock.h"
#include "runtime/win32_shims_wait.h"

#include <cstring>
#include <set>
#include <deque>
#include <map>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {
namespace {

enum : uint32_t {
    WM_NULL = 0x0000, WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006,
    WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008, WM_PAINT = 0x000F, WM_QUIT = 0x0012,
    WM_SHOWWINDOW = 0x0018, WM_ACTIVATEAPP = 0x001C, WM_TIMER = 0x0113,
};

struct WndClass { std::string name; uint32_t wndproc = 0, style = 0, extra = 0, hinst = 0, cursor = 0; };
struct Wnd {
    std::string cls, text;
    uint32_t wndproc = 0, style = 0, exstyle = 0, parent = 0, id = 0, userdata = 0, hinst = 0;
    bool visible = false;
    int32_t x = 0, y = 0, w = 640, h = 480;   // client area = window area (no frame)
    std::vector<uint32_t> extra;
    std::map<std::string, uint32_t> props;
};
struct Msg { uint32_t hwnd, msg, wp, lp; };
struct Timer { uint32_t hwnd, id, period_ms, proc; uint64_t due_ms; };

std::map<std::string, WndClass> g_classes;    // lower-case name -> class
std::map<uint32_t, Wnd>         g_wnds;       // hwnd -> window
std::deque<Msg>                 g_queue;
int32_t g_def_w = 640, g_def_h = 480;         // CW_USEDEFAULT size (set_default_window_size)
std::map<uint64_t, Timer>       g_timers;     // (hwnd << 32 | id) -> timer
struct Accel { uint8_t fvirt; uint16_t key, cmd; };
std::map<uint32_t, std::vector<Accel>> g_accels;   // accelerator handle -> entries
uint32_t g_next_hwnd = 0x00010010u, g_next_atom = 0xC000u, g_next_accel = 0x00F00000u;
uint32_t g_focus = 0, g_active = 0, g_capture = 0;
bool     g_quit = false; uint32_t g_quit_code = 0;

void U(d2rt::Bridge& br, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn, bool stdcall = true) {
    if (std::getenv("SC_TRACE_ALL")) {
        std::string t = name;
        fn = [t, f = std::move(fn)](Cpu& c) { uint32_t r = f(c); log("[not-bound] call %s -> 0x%x ra=0x%08x", t.c_str(), r, c.trap_retaddr()); return r; };
    }
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = stdcall;
    s.tag = std::string("USER32.dll!") + name;
    br.register_shim("USER32.dll", name, s);
}
std::string gstr(Cpu& c, uint32_t p) { return p ? wx86_gread_mb(c, p, -1) : std::string(); }
std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
// Class names may be atoms (MAKEINTATOM) rather than strings.
std::string class_key(Cpu& c, uint32_t p) {
    if (p < 0x10000) { for (auto& kv : g_classes) if (kv.second.extra == p) return kv.first; return "#" + std::to_string(p); }
    return lower(gstr(c, p));
}

// Tail-call a stdcall guest function from a shim registered with argc=0 and
// no cleanup. shim_argc = the dwords the ORIGINAL import pops (its stdcall
// argc): after `fn` returns, the caller must see exactly what it would
// after that import returned.
uint32_t tail_call(Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t shim_argc) {
    const uint32_t E = c.reg(d2rt::R_ESP);
    const uint32_t ret = c.read_u32(E);
    const uint32_t F = E + 4 + 4 * shim_argc;                  // caller's ESP after the import
    const uint32_t S = F - 4 - 4 * (uint32_t)args.size();      // fn's frame: [ret][args...]
    std::vector<uint32_t> a(args);                             // copy BEFORE overwriting the frame
    c.write_u32(S, ret);
    for (size_t i = 0; i < a.size(); ++i) c.write_u32(S + 4 + 4 * (uint32_t)i, a[i]);
    c.set_reg(d2rt::R_ESP, S - 4);                             // epilogue pops 4
    g_br->redirect_next(fn);
    return c.reg(d2rt::R_EAX);
}

void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { g_queue.push_back({ hwnd, msg, wp, lp }); }

void write_msg(Cpu& c, uint32_t p, const Msg& m) {
    uint32_t cx = 320, cy = 240; wx86_get_cursor(&cx, &cy);
    uint32_t v[7] = { m.hwnd, m.msg, m.wp, m.lp, (uint32_t)wx86_now_ms(), cx, cy };
    c.write(p, v, sizeof v);
}

// Moves due timers into the queue (WM_TIMER is synthesized, as on Windows).
void pump_timers() {
    uint64_t now = wx86_now_ms();
    for (auto& kv : g_timers) {
        Timer& t = kv.second;
        if (now >= t.due_ms) {
            bool queued = false;
            for (auto& m : g_queue) if (m.msg == WM_TIMER && m.hwnd == t.hwnd && m.wp == t.id) queued = true;
            if (!queued) post(t.hwnd, WM_TIMER, t.id, t.proc);
            t.due_ms = now + (t.period_ms ? t.period_ms : 1);
        }
    }
}

bool msg_matches(const Msg& m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd && hwnd != 0xFFFFFFFFu && m.hwnd != hwnd) return false;
    if (lo || hi) return m.msg >= lo && m.msg <= hi;
    return true;
}

bool take_msg(uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove, Msg& out) {
    pump_timers();
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
        if (msg_matches(*it, hwnd, lo, hi)) { out = *it; if (remove) g_queue.erase(it); return true; }
    if (g_quit) { out = { 0, WM_QUIT, g_quit_code, 0 }; if (remove) g_quit = false; return true; }
    return false;
}

Wnd* wnd(uint32_t h) { auto it = g_wnds.find(h); return it == g_wnds.end() ? nullptr : &it->second; }

}  // namespace

// Runs guest `fn` (stdcall) to completion from inside a shim and returns its
// EAX: the synchronous messages Windows sends from CreateWindowEx
// (WM_NCCREATE, WM_CREATE) before it returns. The nested frame sits below the
// calling thread's own ESP, so nested calls nest naturally; the caller's
// registers are restored afterwards.
// Runs guest code from `eip` until it returns to the bridge sentinel. Inside a
// scheduler slice the CPU carries a time budget, so one run() can stop early
// on a preemption or a yield; treating that as "returned" cut Game.dll's
// DllMain off halfway through its ~2,000 static constructors. A nested call
// cannot hand the CPU to another guest thread, so it just keeps going. A wait
// that would block can never be satisfied here (no other guest thread runs
// until this returns), so it ends as a timeout instead.
bool run_nested(d2rt::Cpu& c, uint32_t eip, const char** fault) {
    static int s_warned = 0;
    for (;;) {
        if (!c.run(eip, fault)) return false;
        const bool pre = c.take_limit_hit();
        const bool y = g_br->take_yield();
        if (!pre && !y) return true;               // reached the sentinel
        if (y) {
            d2rt::ThreadScheduler* s = wx86_sched();
            d2rt::GuestThread* t = s ? s->current() : nullptr;
            using S = d2rt::GuestThread::State;
            if (t && t->state == S::Blocked) {
                if (t->wake_writes_eax) c.set_reg(d2rt::R_EAX, t->wake_timeout_eax);
                t->wait_obj = nullptr; t->wake_deadline = 0;
                t->wake_writes_eax = true; t->wake_timeout_eax = 0x102;
                t->state = S::Running;
                if (s_warned++ < 8)
                    log("[not-bound] nested call: a blocking wait at 0x%08x ended as a timeout", c.reg(d2rt::R_EIP));
            } else if (t && t->state == S::Finished) {
                log("[not-bound] nested call: thread exited inside it");
                return true;
            }
        }
        eip = c.reg(d2rt::R_EIP);
    }
}

uint32_t guest_call(d2rt::Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    d2rt::X86Context saved; c.save_context(saved);
    uint32_t esp = (c.reg(d2rt::R_ESP) - 0x200u) & ~15u;
    std::vector<uint32_t> a(args);
    for (size_t i = a.size(); i-- > 0;) { esp -= 4; c.write_u32(esp, a[i]); }
    esp -= 4; c.write_u32(esp, g_br->sentinel());
    c.set_reg(d2rt::R_ESP, esp);
    const char* fault = nullptr;
    bool ok = run_nested(c, fn, &fault);
    uint32_t r = c.reg(d2rt::R_EAX);
    c.load_context(saved);
    if (!ok) log("[not-bound] [cause: guest fault] nested call 0x%08x: %s", fn, fault ? fault : "?");
    return r;
}

void install_user32(d2rt::Bridge& br) {
    // ---- classes -------------------------------------------------------------
    U(br, "RegisterClassExA", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        WndClass k; k.style = c.read_u32(p + 4); k.wndproc = c.read_u32(p + 8);
        k.hinst = c.read_u32(p + 20); k.cursor = c.read_u32(p + 28);
        k.name = class_key(c, c.read_u32(p + 40));
        k.extra = g_next_atom++;
        g_classes[k.name] = k;
        log("[not-bound] RegisterClassExA \"%s\" wndproc=0x%08x", k.name.c_str(), k.wndproc);
        return k.extra;
    });
    U(br, "RegisterClassA", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        WndClass k; k.style = c.read_u32(p); k.wndproc = c.read_u32(p + 4); k.hinst = c.read_u32(p + 16);
        k.name = class_key(c, c.read_u32(p + 36));
        k.extra = g_next_atom++;
        g_classes[k.name] = k;
        log("[not-bound] RegisterClassA \"%s\" wndproc=0x%08x", k.name.c_str(), k.wndproc);
        return k.extra;
    });
    U(br, "GetClassInfoA", 3, [](Cpu& c) -> uint32_t {
        auto it = g_classes.find(class_key(c, c.arg(1)));
        if (it == g_classes.end()) return 0;
        uint8_t z[40] = {}; c.write(c.arg(2), z, sizeof z);
        c.write_u32(c.arg(2), it->second.style); c.write_u32(c.arg(2) + 4, it->second.wndproc);
        return 1;
    });
    // System classes (BUTTON, EDIT, STATIC ...) asked about before use: they
    // exist on Windows with a built-in procedure; here DefWindowProcA.
    static auto sys_class = [](Cpu& c, const std::string& key) -> WndClass* {
        auto it = g_classes.find(key);
        if (it != g_classes.end()) return &it->second;
        // Any other name is taken as a system control class (common
        // controls included): nothing is ever drawn through them here.
        WndClass k; k.name = key; k.wndproc = g_br->shim_trap("USER32.dll", "DefWindowProcA");
        k.extra = g_next_atom++; g_classes[key] = k;
        (void)c; return &g_classes[key];
    };
    U(br, "GetClassInfoExA", 3, [](Cpu& c) -> uint32_t {
        std::string key = class_key(c, c.arg(1));
        WndClass* k = sys_class(c, key);
        if (!k) { log("[not-bound] GetClassInfoExA(\"%s\"): no such class", key.c_str()); return 0; }
        uint32_t p = c.arg(2);
        uint8_t z[44] = {}; c.write(p + 4, z + 4, 40);          // keep cbSize
        c.write_u32(p + 4, k->style); c.write_u32(p + 8, k->wndproc);
        c.write_u32(p + 20, k->hinst); c.write_u32(p + 28, k->cursor);
        c.write_u32(p + 40, c.arg(1));
        return k->extra;
    });
    U(br, "GetClassNameA", 3, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        return put_str(c, c.arg(1), w->cls, c.arg(2));
    });
    U(br, "GetClassLongA", 2, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        auto& k = g_classes[w->cls];
        switch ((int32_t)c.arg(1)) { case -24: return k.wndproc; case -26: return k.style; case -12: return k.cursor; default: return 0; }
    });
    U(br, "SetClassLongA", 3, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        auto& k = g_classes[w->cls]; uint32_t old = 0;
        switch ((int32_t)c.arg(1)) { case -24: old = k.wndproc; k.wndproc = c.arg(2); break;
                                     case -26: old = k.style; k.style = c.arg(2); break;
                                     case -12: old = k.cursor; k.cursor = c.arg(2); break; }
        return old;
    });

    // ---- windows -------------------------------------------------------------
    U(br, "CreateWindowExA", 12, [](Cpu& c) -> uint32_t {
        std::string ck = class_key(c, c.arg(1));
        sys_class(c, ck);
        auto it = g_classes.find(ck);
        if (it == g_classes.end()) {
            log("[not-bound] [cause: unknown window class] CreateWindowExA(\"%s\")", ck.c_str());
            return 0;
        }
        uint32_t h = g_next_hwnd; g_next_hwnd += 0x10;
        Wnd w; w.cls = ck; w.text = gstr(c, c.arg(2)); w.wndproc = it->second.wndproc;
        w.exstyle = c.arg(0); w.style = c.arg(3); w.parent = c.arg(8); w.id = c.arg(9); w.hinst = c.arg(10);
        w.visible = (w.style & 0x10000000u) != 0;   // WS_VISIBLE
        w.w = g_def_w; w.h = g_def_h;
        {   // Geometry. CW_USEDEFAULT (0x80000000) in x or width picks the default.
            const uint32_t x = c.arg(4), y = c.arg(5), cw = c.arg(6), ch = c.arg(7);
            if (x != 0x80000000u) { w.x = (int32_t)x; w.y = (int32_t)y; }
            if (cw != 0x80000000u && (int32_t)cw > 0 && (int32_t)ch > 0) { w.w = (int32_t)cw; w.h = (int32_t)ch; }
        }
        g_wnds[h] = w;
        log("[not-bound] CreateWindowExA class=\"%s\" title=\"%s\" style=0x%08x %dx%d -> hwnd 0x%x",
            ck.c_str(), w.text.c_str(), w.style, w.w, w.h, h);
        // Windows sends WM_NCCREATE then WM_CREATE synchronously, with a
        // CREATESTRUCTA (lpCreateParams first) that Unreal's WWindow uses
        // to bind the HWND to its C++ object.
        {
            uint32_t cs = (c.reg(d2rt::R_ESP) - 0x100u) & ~15u;   // below the caller's frame, above the nested one
            uint32_t v[12] = { c.arg(11), c.arg(10), c.arg(9), c.arg(8), c.arg(7), c.arg(6), c.arg(5), c.arg(4),
                               c.arg(3), c.arg(2), c.arg(1), c.arg(0) };
            c.write(cs, v, sizeof v);
            guest_call(c, w.wndproc, { h, 0x0081u /*WM_NCCREATE*/, 0, cs });
            Wnd* nw = wnd(h);
            uint32_t r = nw ? guest_call(c, nw->wndproc, { h, 0x0001u /*WM_CREATE*/, 0, cs }) : 0xFFFFFFFFu;
            if (r == 0xFFFFFFFFu) { g_wnds.erase(h); log("[not-bound] CreateWindowExA: WM_CREATE refused"); return 0; }
            // Then WM_SIZE and WM_MOVE, as Windows does before CreateWindowEx
            // returns. WarCraft III sizes its OpenGL viewport from WM_SIZE
            // alone; without it the game draws into a 0x0 viewport forever.
            if ((nw = wnd(h)) && nw->wndproc) {
                const uint32_t sz = (uint32_t)(nw->h & 0xFFFF) << 16 | (uint32_t)(nw->w & 0xFFFF);
                const uint32_t pos = (uint32_t)(nw->y & 0xFFFF) << 16 | (uint32_t)(nw->x & 0xFFFF);
                guest_call(c, nw->wndproc, { h, 0x0005u /*WM_SIZE*/, 0 /*SIZE_RESTORED*/, sz });
                if ((nw = wnd(h)) && nw->wndproc) guest_call(c, nw->wndproc, { h, 0x0003u /*WM_MOVE*/, 0, pos });
            }
        }
        if (!w.parent) {
            g_active = g_focus = h;
            post(h, WM_ACTIVATEAPP, 1, 0);
            post(h, WM_ACTIVATE, 1, 0);
            post(h, WM_SETFOCUS, 0, 0);
            if (w.visible) post(h, WM_SHOWWINDOW, 1, 0);
        }
        return h;
    });
    U(br, "DestroyWindow", 1, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0);
        Wnd* w = wnd(h); if (!w) return 0;
        // Windows sends WM_DESTROY then WM_NCDESTROY before DestroyWindow
        // returns (WarCraft III's movie player ends its loop in WM_DESTROY).
        if (uint32_t proc = w->wndproc) { guest_call(c, proc, { h, WM_DESTROY, 0, 0 }); guest_call(c, proc, { h, 0x0082u, 0, 0 }); }
        if (!g_wnds.erase(h)) return 1;
        for (auto it = g_timers.begin(); it != g_timers.end();) it = (it->second.hwnd == h) ? g_timers.erase(it) : std::next(it);
        if (g_focus == h) g_focus = 0;
        if (g_active == h) g_active = 0;
        return 1;
    });
    // ---- geometry (Deus Ex sizes its viewport window from these) ----------
    static auto resize = [](Cpu& c, uint32_t hw, int32_t x, int32_t y, int32_t w, int32_t h, bool move, bool size) -> uint32_t {
        Wnd* p = wnd(hw); if (!p) return 0;
        bool changed = size && (p->w != w || p->h != h);
        if (move) { p->x = x; p->y = y; }
        if (size) { p->w = w; p->h = h; }
        if (changed && p->wndproc)
            guest_call(c, p->wndproc, { hw, 0x0005u /*WM_SIZE*/, 0, (uint32_t)(p->h & 0xFFFF) << 16 | (uint32_t)(p->w & 0xFFFF) });
        return 1;
    };
    U(br, "MoveWindow", 6, [](Cpu& c) {
        return resize(c, c.arg(0), (int32_t)c.arg(1), (int32_t)c.arg(2), (int32_t)c.arg(3), (int32_t)c.arg(4), true, true);
    });
    U(br, "SetWindowPos", 7, [](Cpu& c) {
        const uint32_t fl = c.arg(6);   // SWP_NOSIZE 1, SWP_NOMOVE 2
        return resize(c, c.arg(0), (int32_t)c.arg(2), (int32_t)c.arg(3), (int32_t)c.arg(4), (int32_t)c.arg(5), !(fl & 2), !(fl & 1));
    });
    U(br, "GetClientRect", 2, [](Cpu& c) -> uint32_t {
        Wnd* p = wnd(c.arg(0)); int32_t w = p ? p->w : 640, h = p ? p->h : 480;
        uint32_t r[4] = { 0, 0, (uint32_t)w, (uint32_t)h }; c.write(c.arg(1), r, sizeof r); return 1;
    });
    U(br, "GetWindowRect", 2, [](Cpu& c) -> uint32_t {
        Wnd* p = wnd(c.arg(0)); int32_t x = p ? p->x : 0, y = p ? p->y : 0, w = p ? p->w : 640, h = p ? p->h : 480;
        uint32_t r[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)(x + w), (uint32_t)(y + h) }; c.write(c.arg(1), r, sizeof r); return 1;
    });
    U(br, "MapWindowPoints", 4, [](Cpu&) { return 0u; });      // every window sits at the screen origin
    U(br, "AdjustWindowRect", 3, [](Cpu&) { return 1u; });     // no frame: window rect == client rect
    U(br, "AdjustWindowRectEx", 4, [](Cpu&) { return 1u; });
    // ---- menus (the PC window's menu bar; never shown) ----------------------
    U(br, "GetMenu", 1, [](Cpu&) { return 0u; });
    U(br, "SetMenu", 2, [](Cpu&) { return 1u; });
    U(br, "CheckMenuItem", 3, [](Cpu&) { return 0u; });
    U(br, "EnableMenuItem", 3, [](Cpu&) { return 0u; });
    U(br, "DrawMenuBar", 1, [](Cpu&) { return 1u; });
    U(br, "IsWindow", 1, [](Cpu& c) { return wnd(c.arg(0)) ? 1u : 0u; });
    U(br, "ShowWindow", 2, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        bool was = w->visible; w->visible = c.arg(1) != 0;
        if (was != w->visible) post(c.arg(0), WM_SHOWWINDOW, w->visible, 0);
        return was;
    });
    U(br, "IsWindowVisible", 1, [](Cpu& c) { Wnd* w = wnd(c.arg(0)); return w && w->visible ? 1u : 0u; });
    U(br, "IsWindowEnabled", 1, [](Cpu& c) { return wnd(c.arg(0)) ? 1u : 0u; });
    U(br, "IsIconic", 1, [](Cpu&) { return 0u; });
    U(br, "GetParent", 1, [](Cpu& c) { Wnd* w = wnd(c.arg(0)); return w ? w->parent : 0u; });
    U(br, "GetWindow", 2, [](Cpu& c) -> uint32_t {
        if (c.arg(1) == 4) { Wnd* w = wnd(c.arg(0)); return w ? w->parent : 0u; }   // GW_OWNER
        return 0;
    });
    U(br, "GetTopWindow", 1, [](Cpu&) { return 0u; });
    U(br, "GetLastActivePopup", 1, [](Cpu& c) { return c.arg(0); });
    U(br, "EnumWindows", 2, [](Cpu&) { return 1u; });   // no other top-level windows exist
    U(br, "FindWindowExA", 4, [](Cpu&) { return 0u; });
    U(br, "GetForegroundWindow", 0, [](Cpu&) { return g_active; });
    U(br, "GetActiveWindow", 0, [](Cpu&) { return g_active; });
    U(br, "SetActiveWindow", 1, [](Cpu& c) { uint32_t o = g_active; g_active = c.arg(0); return o; });
    U(br, "GetFocus", 0, [](Cpu&) { return g_focus; });
    U(br, "SetFocus", 1, [](Cpu& c) -> uint32_t {
        uint32_t o = g_focus, n = c.arg(0);
        if (n != o) { if (o) post(o, WM_KILLFOCUS, n, 0); g_focus = n; if (n) post(n, WM_SETFOCUS, o, 0); }
        return o;
    });
    U(br, "SetCapture", 1, [](Cpu& c) { uint32_t o = g_capture; g_capture = c.arg(0); return o; });
    U(br, "ReleaseCapture", 0, [](Cpu&) { g_capture = 0; return 1u; });
    U(br, "GetDlgCtrlID", 1, [](Cpu& c) { Wnd* w = wnd(c.arg(0)); return w ? w->id : 0u; });
    U(br, "GetWindowTextA", 3, [](Cpu& c) -> uint32_t { Wnd* w = wnd(c.arg(0)); return w ? put_str(c, c.arg(1), w->text, c.arg(2)) : 0u; });
    U(br, "GetWindowTextLengthA", 1, [](Cpu& c) { Wnd* w = wnd(c.arg(0)); return w ? (uint32_t)w->text.size() : 0u; });
    U(br, "SetWindowTextA", 2, [](Cpu& c) { if (Wnd* w = wnd(c.arg(0))) w->text = gstr(c, c.arg(1)); return 1u; });
    U(br, "GetWindowLongA", 2, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        int32_t i = (int32_t)c.arg(1);
        switch (i) {
            case -4: return w->wndproc; case -16: return w->style; case -20: return w->exstyle;
            case -21: return w->userdata; case -8: return w->parent; case -12: return w->id; case -6: return w->hinst;
        }
        if (i >= 0 && (size_t)(i / 4) < w->extra.size()) return w->extra[i / 4];
        return 0;
    });
    U(br, "SetWindowLongA", 3, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        int32_t i = (int32_t)c.arg(1); uint32_t v = c.arg(2), old = 0;
        switch (i) {
            case -4: old = w->wndproc; w->wndproc = v; return old;
            case -16: old = w->style; w->style = v; return old;
            case -20: old = w->exstyle; w->exstyle = v; return old;
            case -21: old = w->userdata; w->userdata = v; return old;
            case -12: old = w->id; w->id = v; return old;
        }
        if (i >= 0) { size_t k = i / 4; if (w->extra.size() <= k) w->extra.resize(k + 1); old = w->extra[k]; w->extra[k] = v; }
        return old;
    });
    U(br, "SetPropA", 3, [](Cpu& c) { if (Wnd* w = wnd(c.arg(0))) { w->props[lower(gstr(c, c.arg(1)))] = c.arg(2); return 1u; } return 0u; });
    U(br, "GetPropA", 2, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        auto it = w->props.find(lower(gstr(c, c.arg(1)))); return it == w->props.end() ? 0 : it->second;
    });
    U(br, "RemovePropA", 2, [](Cpu& c) -> uint32_t {
        Wnd* w = wnd(c.arg(0)); if (!w) return 0;
        std::string k = lower(gstr(c, c.arg(1))); auto it = w->props.find(k);
        if (it == w->props.end()) return 0; uint32_t v = it->second; w->props.erase(it); return v;
    });
    U(br, "InvalidateRect", 3, [](Cpu& c) {
        bool q = false; for (auto& m : g_queue) if (m.msg == WM_PAINT && m.hwnd == c.arg(0)) q = true;
        if (!q && wnd(c.arg(0))) post(c.arg(0), WM_PAINT, 0, 0);
        return 1u;
    });
    U(br, "GetUpdateRgn", 3, [](Cpu&) { return 1u; });   // NULLREGION
    U(br, "GetSysColor", 1, [](Cpu&) { return 0x00C0C0C0u; });
    static const bool keylog = std::getenv("SC_KEYLOG") != nullptr;
    U(br, "GetKeyState", 1, [](Cpu& c) -> uint32_t {
        uint32_t v = key_state(c.arg(0) & 0xFF);
        if (keylog && v) log("[keylog] GetKeyState(0x%x)=0x%x ra=0x%08x", c.arg(0), v, c.read_u32(c.reg(d2rt::R_ESP)));
        return v; });
    U(br, "GetAsyncKeyState", 1, [](Cpu& c) -> uint32_t {
        uint32_t v = key_state(c.arg(0) & 0xFF) & 0x8000u;
        if (keylog && v) log("[keylog] GetAsyncKeyState(0x%x)=0x%x", c.arg(0), v);
        return v; });
    U(br, "WaitForInputIdle", 2, [](Cpu&) { return 0u; });

    // ---- accelerators (StarCraft builds its own hotkey table) ----------------
    // StarCraft's in-game hotkeys (control groups among them) come through
    // its accelerator table: TranslateAcceleratorA turns a matching key into
    // WM_COMMAND. Until build 16 the table was thrown away and Translate
    // always said "no match", so Ctrl+1..0 and 1..0 did nothing (K).
    U(br, "CreateAcceleratorTableA", 2, [](Cpu& c) -> uint32_t {
        uint32_t h = g_next_accel; g_next_accel += 4;
        std::vector<Accel>& t = g_accels[h];
        const uint32_t p = c.arg(0), n = c.arg(1);
        for (uint32_t i = 0; i < n && i < 4096; ++i) {
            uint8_t f = 0; uint16_t k = 0, cmd = 0;
            c.read(p + 6 * i, &f, 1); c.read(p + 6 * i + 2, &k, 2); c.read(p + 6 * i + 4, &cmd, 2);
            t.push_back(Accel{ f, k, cmd });
        }
        static int said = 0;
        if (said++ < 4) log("[not-bound] CreateAcceleratorTableA %u entries -> 0x%08x", n, h);
        return h;
    });
    // LoadAcceleratorsA: StarCraft loads tables 0x65 and 0x71 (from local.dll
    // and StarCraft.exe), copies both out and builds one table from them. The
    // engine stub returned an empty handle, so the merged table had 0 entries.
    U(br, "LoadAcceleratorsA", 2, [](Cpu& c) -> uint32_t {
        const uint32_t de = find_resource(c, c.arg(0), c.arg(1), 9 /*RT_ACCELERATOR*/);
        if (!de) return 0;
        const uint32_t base = c.arg(0) ? c.arg(0) : exe_base();
        const uint32_t p = base + c.read_u32(de), size = c.read_u32(de + 4);
        uint32_t h = g_next_accel; g_next_accel += 4;
        std::vector<Accel>& t = g_accels[h];
        for (uint32_t off = 0; off + 8 <= size && t.size() < 4096; off += 8) {
            uint16_t f = 0, k = 0, id = 0;
            c.read(p + off, &f, 2); c.read(p + off + 2, &k, 2); c.read(p + off + 4, &id, 2);
            t.push_back(Accel{ (uint8_t)(f & 0x7F), k, id });
            if (f & 0x80) break;                                   // last entry
        }
        static int said = 0;
        if (said++ < 4) log("[not-bound] LoadAcceleratorsA(0x%08x, %u) -> %zu entries", c.arg(0), c.arg(1), t.size());
        return h;
    });
    U(br, "DestroyAcceleratorTable", 1, [](Cpu& c) { g_accels.erase(c.arg(0)); return 1u; });
    U(br, "CopyAcceleratorTableA", 3, [](Cpu& c) -> uint32_t {
        auto it = g_accels.find(c.arg(0)); if (it == g_accels.end()) return 0;
        const uint32_t p = c.arg(1), n = c.arg(2);
        if (!p) return (uint32_t)it->second.size();
        uint32_t k = 0;
        for (; k < n && k < it->second.size(); ++k) {
            const Accel& a = it->second[k]; uint8_t z = 0;
            c.write(p + 6 * k, &a.fvirt, 1); c.write(p + 6 * k + 1, &z, 1);
            c.write(p + 6 * k + 2, &a.key, 2); c.write(p + 6 * k + 4, &a.cmd, 2);
        }
        return k;
    });
    U(br, "TranslateAcceleratorA", 3, [](Cpu& c) -> uint32_t {
        auto it = g_accels.find(c.arg(1)); const uint32_t p = c.arg(2);
        if (it == g_accels.end() || !p) return 0;
        const uint32_t hwnd = c.read_u32(p), msg = c.read_u32(p + 4), wp = c.read_u32(p + 8);
        const bool keydown = msg == 0x0100 || msg == 0x0104, chr = msg == 0x0102 || msg == 0x0106;
        if (!keydown && !chr) return 0;
        const bool sh = key_state(0x10) & 0x8000u, ct = key_state(0x11) & 0x8000u, al = key_state(0x12) & 0x8000u;
        for (const Accel& a : it->second) {
            if (a.fvirt & 1) {                                    // FVIRTKEY: virtual key + modifiers
                if (!keydown || a.key != (wp & 0xFFFF)) continue;
                if (!!(a.fvirt & 4) != sh || !!(a.fvirt & 8) != ct || !!(a.fvirt & 0x10) != al) continue;
            } else {                                              // character code, case-sensitive
                if (!chr || a.key != (wp & 0xFFFF)) continue;
                if (!!(a.fvirt & 0x10) != al) continue;
            }
            g_queue.push_front({ c.arg(0) ? c.arg(0) : hwnd, 0x0111u /*WM_COMMAND*/, 0x10000u | a.cmd, 0 });
            static int said = 0;
            if (said++ < 8) log("[not-bound] accelerator key 0x%x flags 0x%x -> WM_COMMAND %u", a.key, a.fvirt, a.cmd);
            return 1;
        }
        return 0;
    });

    // ---- timers --------------------------------------------------------------
    U(br, "SetTimer", 4, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0), id = c.arg(1);
        if (!h) { static uint32_t next = 0x100; id = next++; }
        g_timers[((uint64_t)h << 32) | id] = Timer{ h, id, c.arg(2), c.arg(3), wx86_now_ms() + c.arg(2) };
        return id ? id : 1;
    });
    U(br, "KillTimer", 2, [](Cpu& c) { return g_timers.erase(((uint64_t)c.arg(0) << 32) | c.arg(1)) ? 1u : 0u; });

    // ---- messages ------------------------------------------------------------
    // WM_KEYDOWN -> WM_CHAR for keys that type a character, queued next as
    // Windows does. Shift state is not tracked yet (letters come out lower case).
    U(br, "TranslateMessage", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0); if (!p) return 0;
        uint32_t hwnd = c.read_u32(p), msg = c.read_u32(p + 4), vk = c.read_u32(p + 8), lp = c.read_u32(p + 12);
        if (msg != 0x0100 && msg != 0x0104) return 0;           // WM_KEYDOWN / WM_SYSKEYDOWN
        uint32_t ch = 0;
        if (vk == 0x08 || vk == 0x09 || vk == 0x0D || vk == 0x1B || vk == 0x20 || (vk >= '0' && vk <= '9')) ch = vk;
        else if (vk >= 'A' && vk <= 'Z') ch = vk + 0x20;
        if (!ch) return 0;
        if ((key_state(0x11) & 0x8000u) && ch > 0x20) return 0;   // Ctrl+key: no character
        g_queue.push_front({ hwnd, msg == 0x0100 ? 0x0102u : 0x0106u, ch, lp });   // WM_CHAR / WM_SYSCHAR
        return 1;
    });
    U(br, "PostMessageA", 4, [](Cpu& c) { post(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); return 1u; });
    U(br, "PostQuitMessage", 1, [](Cpu& c) { g_quit = true; g_quit_code = c.arg(0); return 0u; });
    U(br, "PeekMessageA", 5, [](Cpu& c) -> uint32_t {
        if (std::getenv("W3_TRACE_MSG")) { static std::set<uint32_t> seen; auto* s = wx86_sched(); uint32_t id = s && s->current() ? s->current()->id : 1;
            if (seen.insert(id).second) log("[not-bound] PeekMessageA from thread %u ra=0x%08x hwnd=0x%x", id, c.trap_retaddr(), c.arg(1)); }
        Msg m;
        if (!take_msg(c.arg(1), c.arg(2), c.arg(3), (c.arg(4) & 1) != 0, m)) return 0;
        write_msg(c, c.arg(0), m);
        return 1;
    });
    U(br, "GetMessageA", 4, [](Cpu& c) -> uint32_t {
        if (std::getenv("W3_TRACE_MSG")) { static std::set<uint32_t> seen; auto* s = wx86_sched(); uint32_t id = s && s->current() ? s->current()->id : 1;
            if (seen.insert(id).second) log("[not-bound] GetMessageA from thread %u ra=0x%08x hwnd=0x%x", id, c.trap_retaddr(), c.arg(1)); }
        Msg m;
        if (!take_msg(c.arg(1), c.arg(2), c.arg(3), true, m)) {
            // Windows would block. Sleep a tick so other guest threads run,
            // then hand back WM_NULL; the window procedure ignores it.
            wx86_wait_sleep(1);
            m = { c.arg(1), WM_NULL, 0, 0 };
        }
        write_msg(c, c.arg(0), m);
        return m.msg == WM_QUIT ? 0u : 1u;
    });
    // DispatchMessageA(msg): tail-call the window procedure (or the timer
    // callback for a WM_TIMER that carries one).
    {
        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "USER32.dll!DispatchMessageA";
        s.fn = [](Cpu& c) -> uint32_t {
            uint32_t p = c.arg(0);
            uint32_t hwnd = c.read_u32(p), msg = c.read_u32(p + 4), wp = c.read_u32(p + 8), lp = c.read_u32(p + 12);
            if (msg == WM_TIMER && lp) return tail_call(c, lp, { hwnd, msg, wp, (uint32_t)wx86_now_ms() }, 1);
            static const bool keylog = std::getenv("SC_KEYLOG") != nullptr;
            if (keylog && (msg == 0x0100 || msg == 0x0101 || msg == 0x0102)) {
                uint8_t kt[4] = {0}; c.read(0x596a28, kt, 3); uint16_t f = 0; c.read(0x6cddc0, &f, 2);
                log("[keylog] dispatch msg=0x%x wp=0x%x lp=0x%08x hwnd=0x%x shift=%u ctrl=%u alt=%u state=0x%x",
                    msg, wp, lp, hwnd, kt[0], kt[1], kt[2], f);
                if (wp == 0x31) {
                    uint32_t pl = c.read_u32(0x512688), g[12];
                    c.read(0x57fe60 + (pl * 18 + 1) * 48, g, sizeof g);
                    log("[keylog]   player %u group1: %08x %08x %08x %08x", pl, g[0], g[1], g[2], g[3]);
                }
            }
            Wnd* w = wnd(hwnd);
            // No window (a WM_NULL from an idle GetMessageA, or a destroyed
            // window): just return. The bridge pops the return address and
            // jumps back itself, so only the one argument is popped here (the
            // old trap_epilogue(0, 8) popped it twice: every such dispatch
            // slid the caller's stack 4 bytes, until WarCraft III's movie
            // loop returned into garbage).
            if (!w || !w->wndproc) { c.set_reg(d2rt::R_ESP, c.reg(d2rt::R_ESP) + 4); return 0; }
            return tail_call(c, w->wndproc, { hwnd, msg, wp, lp }, 1);
        };
        br.register_shim("USER32.dll", "DispatchMessageA", s);
    }
    {
        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "USER32.dll!SendMessageA";
        s.fn = [](Cpu& c) -> uint32_t {
            uint32_t hwnd = c.arg(0);
            // Unreal's log window appends each log line with EM_REPLACESEL:
            // copy it into our own log (the game's DeusEx.log is buffered).
            if (c.arg(1) == 0x00C2u && c.arg(3)) {
                std::string t = gstr(c, c.arg(3));
                while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
                if (!t.empty()) log("[game] %s", t.c_str());
            }
            Wnd* w = wnd(hwnd);
            if (!w || !w->wndproc) {   // no procedure: behave like a 4-arg stdcall returning 0
                const uint32_t E = c.reg(d2rt::R_ESP), ret = c.read_u32(E);
                c.write_u32(E + 16, ret); c.set_reg(d2rt::R_ESP, E + 12); c.set_reg(d2rt::R_EAX, 0);
                g_br->redirect_next(ret);
                return 0;
            }
            return tail_call(c, w->wndproc, { hwnd, c.arg(1), c.arg(2), c.arg(3) }, 4);
        };
        br.register_shim("USER32.dll", "SendMessageA", s);
    }
    {
        Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "USER32.dll!CallWindowProcA";
        s.fn = [](Cpu& c) -> uint32_t {
            return tail_call(c, c.arg(0), { c.arg(1), c.arg(2), c.arg(3), c.arg(4) }, 5);
        };
        br.register_shim("USER32.dll", "CallWindowProcA", s);
    }
    U(br, "DefDlgProcA", 4, [](Cpu&) { return 0u; });
    U(br, "IsDialogMessageA", 2, [](Cpu&) { return 0u; });
    U(br, "DrawTextA", 5, [](Cpu&) { return 16u; });   // height of one line; nothing is drawn through GDI
}

// Input-layer message to the focused (else active) window.
void dinput_feed(uint32_t msg, uint32_t wp, uint32_t lp);   // r6_shims.cpp
void post_input(uint32_t msg, uint32_t wp, uint32_t lp) {
    dinput_feed(msg, wp, lp);
    uint32_t h = g_capture ? g_capture : g_focus ? g_focus : g_active;
    static const bool tr = std::getenv("SC_TRACE_INPUT") != nullptr;
    if (tr && msg != 0x0200) log("[not-bound] input msg 0x%04x wp=0x%x lp=0x%08x -> hwnd 0x%x (focus 0x%x capture 0x%x)", msg, wp, lp, h, g_focus, g_capture);
    // Windows sends WM_LBUTTONDBLCLK only to a window whose class has
    // CS_DBLCLKS; any other window gets a second WM_LBUTTONDOWN and times the
    // double click itself (WarCraft III does).
    if (msg == 0x0203 && h) {
        auto w = g_wnds.find(h);
        auto k = w == g_wnds.end() ? g_classes.end() : g_classes.find(w->second.cls);
        if (k == g_classes.end() || !(k->second.style & 0x0008u)) msg = 0x0201;
    }
    if (h) post(h, msg, wp, lp);
}

// Synthetic key press to the focused (else active) window: WM_KEYDOWN then
// WM_KEYUP, as a keyboard would deliver it. Used by the input layer.
// The size a CW_USEDEFAULT window gets. WarCraft III opens its window that
// way in windowed mode and renders at whatever client size WM_SIZE reports,
// so this is the game's render resolution.
void set_default_window_size(int w, int h) { if (w > 0 && h > 0) { g_def_w = w; g_def_h = h; } }

void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { post(hwnd, msg, wp, lp); }

void post_key(uint32_t vk) {
    uint32_t h = g_focus ? g_focus : g_active;
    if (!h) return;
    post(h, 0x0100, vk, 0x00000001u);               // WM_KEYDOWN, repeat 1
    post(h, 0x0101, vk, 0xC0000001u);               // WM_KEYUP
    dinput_feed(0x0100, vk, 1); dinput_feed(0x0101, vk, 0xC0000001u);
}

}  // namespace sc
