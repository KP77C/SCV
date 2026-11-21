// r6_shims.cpp — Rainbow Six: Black Ops extras: DirectInput (keyboard,
// mouse, no joystick), and the WINMM pieces the engine imports.
//
// DirectInput state comes from the same input messages the port posts to
// the game window (sc_user32.cpp post_input): a 256-byte DIK_ keyboard
// array and relative mouse motion.
#include "sc_host.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/win32_shims_window.h"
#include "runtime/win32_shims_gdi32.h"

#include <cstring>
#include <deque>
#include <map>
#include <functional>
#include <string>
#include <vector>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {

namespace {

constexpr uint32_t DI_OK = 0, E_FAIL = 0x80004005u, E_NOINTERFACE = 0x80004002u, DIERR_NOTFOUND = 0x80070002u;

struct Method { const char* name; uint32_t argc; std::function<uint32_t(Cpu&)> fn; };
uint32_t make_vtable(d2rt::Bridge& br, const char* iface, const std::vector<Method>& ms) {
    uint32_t vt = wx86_scratch_alloc(4 * (uint32_t)ms.size());
    for (size_t i = 0; i < ms.size(); ++i) {
        Shim s; s.argc = ms[i].argc; s.stdcall_cleanup = true;
        std::string key = std::string(iface) + "::" + ms[i].name;
        s.tag = "dinput.com!" + key;
        auto fn = ms[i].fn;
        if (!fn) { std::string k = key; fn = [k](Cpu& c) { log("[not-bound] [cause: missing COM method] %s ra=0x%08x", k.c_str(), c.trap_retaddr()); return E_FAIL; }; }
        if (std::getenv("R6_TRACE_DI")) { std::string k = key; fn = [k, f = fn](Cpu& c) { uint32_t r = f(c); log("[di] %s(%x,%x,%x) -> 0x%x", k.c_str(), c.arg(1), c.arg(2), c.arg(3), r); return r; }; }
        s.fn = fn;
        br.register_shim("dinput.com", key, s);
        g_cpu->write_u32(vt + 4 * (uint32_t)i, br.shim_trap("dinput.com", key));
    }
    return vt;
}
uint32_t new_object(uint32_t vt, uint32_t id) { uint32_t o = wx86_scratch_alloc(16); g_cpu->write_u32(o, vt); g_cpu->write_u32(o + 4, id); return o; }

// GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000}, GUID_SysMouse {6F1D2B60-...}
bool is_sys(Cpu& c, uint32_t g, uint32_t first) {
    if (!g) return false;
    static const uint8_t tail[12] = { 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    uint8_t b[16]; c.read(g, b, 16);
    uint32_t a; std::memcpy(&a, b, 4);
    return a == first && std::memcmp(b + 4, tail, 12) == 0;
}

uint8_t g_keys[256];
int32_t g_mdx = 0, g_mdy = 0, g_mdz = 0;
int g_lastx = -1, g_lasty = -1;   // where the game thinks its cursor is (its SetCursorPos calls), so moves become deltas from there
uint8_t g_mbtn[4];
struct Ev { uint32_t ofs, data, ts; };
std::deque<Ev> g_kev, g_mev;
uint32_t g_seq = 0;
uint32_t g_vt_di = 0, g_vt_dev = 0, g_di = 0, g_kbd = 0, g_mouse = 0;

// Windows virtual key -> DirectInput scan code (DIK_).
uint8_t vk_to_dik(uint32_t vk) {
    static uint8_t t[256];
    static bool init = false;
    if (!init) {
        init = true;
        const char* row1 = "1234567890"; for (int i = 0; i < 10; ++i) t[(uint8_t)row1[i]] = (uint8_t)(0x02 + i);
        const char* q = "QWERTYUIOP"; for (int i = 0; i < 10; ++i) t[(uint8_t)q[i]] = (uint8_t)(0x10 + i);
        const char* a = "ASDFGHJKL"; for (int i = 0; i < 9; ++i) t[(uint8_t)a[i]] = (uint8_t)(0x1E + i);
        const char* z = "ZXCVBNM"; for (int i = 0; i < 7; ++i) t[(uint8_t)z[i]] = (uint8_t)(0x2C + i);
        t[0x1B] = 0x01; t[0x08] = 0x0E; t[0x09] = 0x0F; t[0x0D] = 0x1C; t[0x11] = 0x1D; t[0x10] = 0x2A; t[0x20] = 0x39;
        t[0x12] = 0x38; t[0x14] = 0x3A;
        for (int i = 0; i < 10; ++i) t[0x70 + i] = (uint8_t)(0x3B + i);   // F1-F10
        t[0x7A] = 0x57; t[0x7B] = 0x58;
        t[0x26] = 0xC8; t[0x28] = 0xD0; t[0x25] = 0xCB; t[0x27] = 0xCD;  // arrows
        t[0x24] = 0xC7; t[0x23] = 0xCF; t[0x21] = 0xC9; t[0x22] = 0xD1; t[0x2D] = 0xD2; t[0x2E] = 0xD3;
        t[0xBA] = 0x27; t[0xBB] = 0x0D; t[0xBC] = 0x33; t[0xBD] = 0x0C; t[0xBE] = 0x34; t[0xBF] = 0x35; t[0xC0] = 0x29;
        t[0xDB] = 0x1A; t[0xDC] = 0x2B; t[0xDD] = 0x1B; t[0xDE] = 0x28;
        for (int i = 0; i < 10; ++i) { static const uint8_t np[10] = { 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49 }; t[0x60 + i] = np[i]; }
    }
    return t[vk & 255];
}

}  // namespace

// Called for every input message the port posts (sc_user32.cpp).
void dinput_feed(uint32_t msg, uint32_t wp, uint32_t lp) {
    const uint32_t ts = ++g_seq;
    auto key = [&](bool down) {
        uint8_t k = vk_to_dik(wp); if (!k) return;
        g_keys[k] = down ? 0x80 : 0; g_kev.push_back({ k, down ? 0x80u : 0u, ts });
        if (g_kev.size() > 256) g_kev.pop_front();
    };
    auto btn = [&](int b, bool down) { g_mbtn[b] = down ? 0x80 : 0; g_mev.push_back({ 12u + b, down ? 0x80u : 0u, ts }); if (g_mev.size() > 256) g_mev.pop_front(); };
    switch (msg) {
    case 0x100: case 0x104: key(true); break;
    case 0x101: case 0x105: key(false); break;
    case 0x201: btn(0, true); break; case 0x202: btn(0, false); break;
    case 0x204: btn(1, true); break; case 0x205: btn(1, false); break;
    case 0x207: btn(2, true); break; case 0x208: btn(2, false); break;
    case 0x200: {
        const int x = (int16_t)(lp & 0xFFFF), y = (int16_t)(lp >> 16);
        int& lastx = g_lastx; int& lasty = g_lasty;
        if (lastx >= 0) {
            const int dx = x - lastx, dy = y - lasty;
            g_mdx += dx; g_mdy += dy;
            if (dx) g_mev.push_back({ 0, (uint32_t)dx, ts });
            if (dy) g_mev.push_back({ 4, (uint32_t)dy, ts });
            while (g_mev.size() > 256) g_mev.pop_front();
        }
        lastx = x; lasty = y; break;
    }
    default: break;
    }
}
// Relative mouse motion without a cursor position (camera look).
void dinput_mouse_move(int dx, int dy) {
    const uint32_t ts = ++g_seq;
    g_mdx += dx; g_mdy += dy;
    if (dx) g_mev.push_back({ 0, (uint32_t)dx, ts });
    if (dy) g_mev.push_back({ 4, (uint32_t)dy, ts });
    while (g_mev.size() > 256) g_mev.pop_front();
}

namespace {
// ---- WINMM multimedia file I/O (the engine reads its .wav files with it) ----
struct Mmio { std::vector<uint8_t> data; uint32_t pos = 0; std::string name; };
std::map<uint32_t, Mmio*> g_mmio;
uint32_t g_next_mmio = 0x4D4D0001u;
void W(d2rt::Bridge& br, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    Shim s; s.argc = argc; s.stdcall_cleanup = true; s.tag = std::string("WINMM.dll!") + name; s.fn = std::move(fn);
    br.register_shim("WINMM.dll", name, s);
}
uint32_t rd32(const Mmio& m, uint32_t at) { if (at + 4 > m.data.size()) return 0; uint32_t v; std::memcpy(&v, &m.data[at], 4); return v; }
}  // namespace

void install_r6(d2rt::Bridge& br) {
    {   // The game keeps its own cursor, moved by DirectInput deltas, and syncs the system cursor to it.
        Shim s; s.argc = 2; s.stdcall_cleanup = true; s.tag = "USER32.dll!SetCursorPos";
        s.fn = [](Cpu& c) {
            uint32_t ax = c.arg(0), ay = c.arg(1);
            if (ax >= wx86_screen_w() || ay >= wx86_screen_h()) { ax = wx86_screen_w() / 2; ay = wx86_screen_h() / 2; }   // the first call passes an unset position
            wx86_set_cursor(ax, ay); uint32_t x, y; wx86_get_cursor(&x, &y); g_lastx = (int)x; g_lasty = (int)y;
            if (std::getenv("R6_TRACE_DI")) log("[di] SetCursorPos(%d,%d)", (int)c.arg(0), (int)c.arg(1)); return 1u; };
        br.register_shim("USER32.dll", "SetCursorPos", s);
    }
    // mmioOpenA(name, info, flags): read-only, the whole file in host memory.
    W(br, "mmioOpenA", 3, [](Cpu& c) -> uint32_t {
        std::string g = wx86_gread_mb(c, c.arg(0), -1);
        const uint32_t flags = c.arg(2);
        if (flags & 0x3) { log("[not-bound] [cause: unsupported] mmioOpenA %s for writing (flags 0x%x)", g.c_str(), flags); return 0; }
        Mmio* m = new Mmio(); m->name = g; m->data = read_file(resolve_path(g));
        if (m->data.empty()) { delete m; if (c.arg(1)) c.write_u32(c.arg(1) + 12, 257); return 0; }   // MMIOERR_CANNOTOPEN in wErrorRet
        const uint32_t h = g_next_mmio++; g_mmio[h] = m;
        return h;
    });
    W(br, "mmioClose", 2, [](Cpu& c) -> uint32_t {
        auto it = g_mmio.find(c.arg(0)); if (it == g_mmio.end()) return 0;
        delete it->second; g_mmio.erase(it); return 0;
    });
    W(br, "mmioRead", 3, [](Cpu& c) -> uint32_t {
        auto it = g_mmio.find(c.arg(0)); if (it == g_mmio.end()) return 0xFFFFFFFFu;
        Mmio& m = *it->second; uint32_t n = c.arg(2);
        if (m.pos >= m.data.size()) return 0;
        if (n > m.data.size() - m.pos) n = (uint32_t)(m.data.size() - m.pos);
        c.write(c.arg(1), &m.data[m.pos], n); m.pos += n; return n;
    });
    W(br, "mmioWrite", 3, [](Cpu&) -> uint32_t { return 0xFFFFFFFFu; });
    W(br, "mmioCreateChunk", 3, [](Cpu&) -> uint32_t { return 264; });   // MMIOERR_CANNOTWRITE
    // MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags.
    W(br, "mmioDescend", 4, [](Cpu& c) -> uint32_t {
        auto it = g_mmio.find(c.arg(0)); if (it == g_mmio.end()) return 257;
        Mmio& m = *it->second; const uint32_t ck = c.arg(1), par = c.arg(2), flags = c.arg(3);
        uint32_t end = (uint32_t)m.data.size();
        if (par) { const uint32_t pe = c.read_u32(par + 12) + c.read_u32(par + 4); if (pe < end) end = pe; }
        const uint32_t want_id = c.read_u32(ck), want_type = c.read_u32(ck + 8);
        const uint32_t RIFF = 0x46464952u, LIST = 0x5453494Cu;
        for (;;) {
            if (m.pos + 8 > end) return 265;                 // MMIOERR_CHUNKNOTFOUND
            const uint32_t id = rd32(m, m.pos), size = rd32(m, m.pos + 4);
            const uint32_t data = m.pos + 8;
            const bool form = id == RIFF || id == LIST;
            const uint32_t type = form ? rd32(m, data) : 0;
            bool match = true;
            if (flags & 0x10) match = id == want_id;
            else if (flags & 0x20) match = id == RIFF && type == want_type;
            else if (flags & 0x40) match = id == LIST && type == want_type;
            if (match) {
                c.write_u32(ck, id); c.write_u32(ck + 4, size); c.write_u32(ck + 8, type);
                c.write_u32(ck + 12, data); c.write_u32(ck + 16, 0);
                m.pos = form ? data + 4 : data;
                return 0;
            }
            m.pos = data + size + (size & 1);
        }
    });
    W(br, "mmioAscend", 3, [](Cpu& c) -> uint32_t {
        auto it = g_mmio.find(c.arg(0)); if (it == g_mmio.end()) return 257;
        Mmio& m = *it->second; const uint32_t ck = c.arg(1);
        const uint32_t size = c.read_u32(ck + 4);
        m.pos = c.read_u32(ck + 12) + size + (size & 1);
        return 0;
    });

    // ---- IDirectInputDevice2A -------------------------------------------------------------
    auto is_kbd = [](Cpu& c) { return c.read_u32(c.arg(0) + 4) == 1; };
    g_vt_dev = make_vtable(br, "IDirectInputDevice2A", {
        { "QueryInterface", 3, [](Cpu& c) { c.write_u32(c.arg(2), c.arg(0)); return DI_OK; } },
        { "AddRef", 1, [](Cpu&) { return 2u; } }, { "Release", 1, [](Cpu&) { return 1u; } },
        { "GetCapabilities", 2, [is_kbd](Cpu& c) {
            uint32_t p = c.arg(1), n = c.read_u32(p); if (n < 44 || n > 64) n = 44;
            std::vector<uint8_t> z(n - 4, 0); c.write(p + 4, z.data(), n - 4);
            c.write_u32(p + 4, 0x5);                          // ATTACHED | POLLEDDEVICE? (ATTACHED=1, EMULATED=4)
            c.write_u32(p + 8, is_kbd(c) ? 0x13 : 0x12);      // DIDEVTYPE_KEYBOARD / MOUSE
            c.write_u32(p + 12, is_kbd(c) ? 0 : 3);           // axes
            c.write_u32(p + 16, is_kbd(c) ? 128 : 3);         // buttons
            return DI_OK; } },
        { "EnumObjects", 4, [](Cpu&) { return DI_OK; } },
        { "GetProperty", 3, [](Cpu&) { return E_FAIL; } },
        { "SetProperty", 3, [](Cpu&) { return DI_OK; } },
        { "Acquire", 1, [](Cpu&) { return DI_OK; } },
        { "Unacquire", 1, [](Cpu&) { return DI_OK; } },
        { "GetDeviceState", 3, [is_kbd](Cpu& c) {
            const uint32_t n = c.arg(1), p = c.arg(2);
            if (is_kbd(c)) { c.write(p, g_keys, n < 256 ? n : 256); return DI_OK; }
            std::vector<uint8_t> z(n, 0); c.write(p, z.data(), n);   // DIMOUSESTATE: lX lY lZ buttons[4]
            c.write_u32(p, (uint32_t)g_mdx); c.write_u32(p + 4, (uint32_t)g_mdy); c.write_u32(p + 8, (uint32_t)g_mdz);
            c.write(p + 12, g_mbtn, n >= 16 ? 4 : 0);
            g_mdx = g_mdy = g_mdz = 0;
            return DI_OK; } },
        { "GetDeviceData", 5, [is_kbd](Cpu& c) {
            const uint32_t sz = c.arg(1), out = c.arg(2), pn = c.arg(3), flags = c.arg(4);
            std::deque<Ev>& q = is_kbd(c) ? g_kev : g_mev;
            uint32_t want = c.read_u32(pn), n = 0;
            while (n < want && !q.empty()) {
                const Ev e = q.front();
                if (out) { c.write_u32(out + n * sz, e.ofs); c.write_u32(out + n * sz + 4, e.data); c.write_u32(out + n * sz + 8, e.ts); c.write_u32(out + n * sz + 12, e.ts); }
                { static const bool tr = std::getenv("R6_TRACE_DI") != nullptr; if (tr) log("[di]   ev %s ofs=%u data=%d", is_kbd(c) ? "kbd" : "mouse", e.ofs, (int)e.data); }
                if (!(flags & 1)) q.pop_front();                 // DIGDD_PEEK
                ++n;
                if (flags & 1) break;
            }
            if (!is_kbd(c)) g_mdx = g_mdy = 0;
            c.write_u32(pn, n);
            return DI_OK; } },
        { "SetDataFormat", 2, [](Cpu&) { return DI_OK; } },
        { "SetEventNotification", 2, [](Cpu&) { return DI_OK; } },
        { "SetCooperativeLevel", 3, [](Cpu&) { return DI_OK; } },
        { "GetObjectInfo", 4, [](Cpu&) { return DIERR_NOTFOUND; } },
        { "GetDeviceInfo", 2, [](Cpu&) { return E_FAIL; } },
        { "RunControlPanel", 3, [](Cpu&) { return DI_OK; } },
        { "Initialize", 4, [](Cpu&) { return DI_OK; } },
        { "CreateEffect", 5, [](Cpu&) { return E_FAIL; } },
        { "EnumEffects", 4, [](Cpu&) { return DI_OK; } },
        { "GetEffectInfo", 3, [](Cpu&) { return E_FAIL; } },
        { "GetForceFeedbackState", 2, [](Cpu&) { return E_FAIL; } },
        { "SendForceFeedbackCommand", 2, [](Cpu&) { return E_FAIL; } },
        { "EnumCreatedEffectObjects", 4, [](Cpu&) { return DI_OK; } },
        { "Escape", 2, [](Cpu&) { return E_FAIL; } },
        { "Poll", 1, [](Cpu&) { return DI_OK; } },
        { "SendDeviceData", 5, [](Cpu&) { return E_FAIL; } },
    });
    g_kbd = new_object(g_vt_dev, 1); g_mouse = new_object(g_vt_dev, 2);

    // ---- IDirectInputA ---------------------------------------------------------------------
    g_vt_di = make_vtable(br, "IDirectInputA", {
        { "QueryInterface", 3, [](Cpu& c) { c.write_u32(c.arg(2), c.arg(0)); return DI_OK; } },
        { "AddRef", 1, [](Cpu&) { return 2u; } }, { "Release", 1, [](Cpu&) { return 1u; } },
        { "CreateDevice", 4, [](Cpu& c) -> uint32_t {
            if (is_sys(c, c.arg(1), 0x6F1D2B61)) { c.write_u32(c.arg(2), g_kbd); return DI_OK; }
            if (is_sys(c, c.arg(1), 0x6F1D2B60)) { c.write_u32(c.arg(2), g_mouse); return DI_OK; }
            log("[not-bound] [cause: no such device] DirectInput CreateDevice (joystick)");
            return DIERR_NOTFOUND; } },
        { "EnumDevices", 5, [](Cpu&) { return DI_OK; } },   // no joysticks
        { "GetDeviceStatus", 2, [](Cpu&) { return DI_OK; } },
        { "RunControlPanel", 3, [](Cpu&) { return DI_OK; } },
        { "Initialize", 3, [](Cpu&) { return DI_OK; } },
    });
    g_di = new_object(g_vt_di, 1);
    {
        Shim s; s.argc = 4; s.stdcall_cleanup = true; s.tag = "DINPUT.dll!DirectInputCreateA";
        s.fn = [](Cpu& c) -> uint32_t { c.write_u32(c.arg(2), g_di); log("[not-bound] DirectInputCreateA(version 0x%x)", c.arg(1)); return DI_OK; };
        br.register_shim("DINPUT.dll", "DirectInputCreateA", s);
    }
}

}  // namespace sc
