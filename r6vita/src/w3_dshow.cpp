// w3_dshow.cpp — a stand-in DirectShow filter graph for WarCraft III's
// movie player ("Blizzard Player", Game.dll).
//
// The game plays its logo and cinematics through DirectShow (CLSID_FilterGraph
// plus Blizzard's own blizzard.ax filter). There is no DirectShow here yet, so
// this graph accepts every file and reports each movie as finished at once:
// IMediaEvent hands out EC_COMPLETE and IMediaPosition says the movie is
// 0 s long and already at its end. The game then carries on to the menu, as
// if the player had skipped the movie.
//
// Each interface is a guest COM object (vtable of shim traps) in scratch;
// every method pops its exact stdcall argument count.
#include "sc_host.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_sync.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {
namespace {

constexpr uint32_t S_OK_ = 0, E_NOINTERFACE_ = 0x80004002u, E_NOTIMPL_ = 0x80004001u, E_ABORT_ = 0x80004004u;

// What a method does with its first argument after `this`.
enum Out : uint8_t { NONE, Z4, Z8 };
struct M { uint8_t argc; Out out; };   // argc includes `this`

// IDispatch's four methods (all interfaces below except IGraphBuilder derive from it).
#define DISPATCH {2, Z4}, {4, NONE}, {6, NONE}, {9, NONE}

struct Iface {
    const char* name;
    uint8_t     iid_tail;          // byte 0 of 56a868xx-0ad4-11ce-b03a-0020af0ba770
    std::vector<M> methods;        // after IUnknown's three
};

const Iface kIfaces[] = {
    { "IGraphBuilder", 0xA9, {
        {3, NONE}, {2, NONE}, {2, Z4}, {3, Z4}, {4, NONE}, {2, NONE}, {2, NONE}, {1, NONE},   // IFilterGraph
        {3, NONE}, {2, NONE}, {3, NONE}, {4, Z4}, {2, NONE}, {1, NONE}, {1, NONE} } },        // Connect .. ShouldOperationContinue
    { "IMediaControl", 0xB1, { DISPATCH,
        {1, NONE}, {1, NONE}, {1, NONE}, {3, NONE}, {2, NONE}, {3, NONE}, {2, Z4}, {2, Z4}, {1, NONE} } },
    { "IMediaPosition", 0xB2, { DISPATCH,
        {2, Z8}, {3, NONE}, {2, Z8}, {2, Z8}, {3, NONE}, {2, Z8}, {3, NONE}, {3, NONE}, {2, Z8}, {2, Z4}, {2, Z4} } },
    { "IBasicAudio", 0xB3, { DISPATCH, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4} } },
    { "IVideoWindow", 0xB4, { DISPATCH,
        {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4},
        {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4},
        {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE}, {2, Z4}, {2, Z4}, {2, NONE}, {2, Z4}, {2, NONE},
        {2, NONE}, {5, NONE}, {5, NONE}, {5, NONE}, {3, NONE}, {3, NONE}, {5, NONE}, {2, NONE}, {2, Z4} } },
    // No IBasicVideo: the player then sizes its picture from its own 800x368 default.
    { "IMediaEvent", 0xB6, { DISPATCH,
        {2, Z4}, {5, NONE}, {3, NONE}, {2, NONE}, {2, NONE}, {4, NONE} } },
    { "IMediaEventEx", 0xC0, { DISPATCH,
        {2, Z4}, {5, NONE}, {3, NONE}, {2, NONE}, {2, NONE}, {4, NONE}, {4, NONE}, {2, NONE}, {2, Z4} } },
};

struct Graph {
    std::map<uint8_t, uint32_t> objs;   // iid tail -> guest object
    bool complete_sent = false;
    uint32_t event = 0;
    uint32_t notify_hwnd = 0, notify_msg = 0;
};
Graph g_graph;
std::map<std::string, uint32_t> g_vtables;    // interface name -> guest vtable

uint32_t vtable_for(d2rt::Bridge& br, Cpu& c, const Iface& f);

uint32_t object_for(Cpu& c, uint8_t tail) {
    auto it = g_graph.objs.find(tail);
    if (it != g_graph.objs.end()) return it->second;
    for (const Iface& f : kIfaces) if (f.iid_tail == tail) {
        uint32_t o = wx86_scratch_alloc(16);
        c.write_u32(o, vtable_for(*g_br, c, f));
        g_graph.objs[tail] = o;
        return o;
    }
    return 0;
}

bool is_dshow_iid(const uint8_t* g) {
    static const uint8_t tail[13] = { 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11, 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B };
    return !std::memcmp(g + 1, tail, 13) && g[14] == 0xA7 && g[15] == 0x70;
}

uint32_t method(Cpu& c, const Iface& f, uint32_t idx) {
    const std::string n = f.name;
    if (idx == 0) {                                                // QueryInterface
        uint8_t g[16] = {}; c.read(c.arg(1), g, 16);
        uint32_t o = is_dshow_iid(g) ? object_for(c, g[0]) : 0;
        if (c.arg(2)) c.write_u32(c.arg(2), o);
        if (!o) log("[not-bound] dshow %s::QueryInterface(%02x...) -> E_NOINTERFACE", f.name, g[0]);
        return o ? S_OK_ : E_NOINTERFACE_;
    }
    if (idx == 1 || idx == 2) return 1u;                           // AddRef / Release (objects live forever)
    const M& m = f.methods[idx - 3];
    if (m.out == Z4 && c.arg(1)) c.write_u32(c.arg(1), 0);
    if (m.out == Z8 && c.arg(1)) { c.write_u32(c.arg(1), 0); c.write_u32(c.arg(1) + 4, 0); }
    const bool event_if = n == "IMediaEvent" || n == "IMediaEventEx";
    if (n == "IGraphBuilder" && idx == 3 + 10) {                   // RenderFile(file, playlist)
        std::string file; for (uint32_t p = c.arg(1), i = 0; p && i < 260; ++i) {
            uint16_t ch = 0; c.read(p + 2 * i, &ch, 2); if (!ch) break; file += (char)(ch < 0x80 ? ch : '?'); }
        log("[not-bound] dshow RenderFile(%s): movie shown as already finished (no video playback yet)", file.c_str());
        g_graph.complete_sent = false;
        return S_OK_;
    }
    if (event_if) {
        if (idx == 7) {                                            // GetEventHandle(OAEVENT*)
            if (!g_graph.event) {                                  // manual reset, signalled: "an event is waiting"
                WxEvent* e = new WxEvent(); e->manual = true; e->signaled = true;
                g_graph.event = wx86_handle_add(e);
            }
            if (c.arg(1)) c.write_u32(c.arg(1), g_graph.event);
            return S_OK_;
        }
        if (idx == 8) {                                            // GetEvent(code*, p1*, p2*, timeout)
            if (g_graph.complete_sent) return E_ABORT_;
            g_graph.complete_sent = true;
            if (c.arg(1)) c.write_u32(c.arg(1), 1);                // EC_COMPLETE
            if (c.arg(2)) c.write_u32(c.arg(2), 0);
            if (c.arg(3)) c.write_u32(c.arg(3), 0);
            return S_OK_;
        }
        if (idx == 9) { if (c.arg(2)) c.write_u32(c.arg(2), 1); return S_OK_; }   // WaitForCompletion
        if (n == "IMediaEventEx" && idx == 13) {                   // SetNotifyWindow(hwnd, msg, inst)
            g_graph.notify_hwnd = c.arg(1); g_graph.notify_msg = c.arg(2);
            return S_OK_;
        }
    }
    if (n == "IMediaControl" && idx == 7) {                        // Run: tell the owner window an event is waiting
        if (g_graph.notify_hwnd && g_graph.notify_msg) post_message(g_graph.notify_hwnd, g_graph.notify_msg, 0, 0);
        return S_OK_;
    }
    if (n == "IMediaControl" && idx == 10) { if (c.arg(2)) c.write_u32(c.arg(2), 0); return S_OK_; }   // GetState: stopped
    if (idx < 7 && n != "IGraphBuilder") return E_NOTIMPL_;        // IDispatch
    return S_OK_;
}

uint32_t vtable_for(d2rt::Bridge& br, Cpu& c, const Iface& f) {
    auto it = g_vtables.find(f.name); if (it != g_vtables.end()) return it->second;
    const uint32_t n = 3 + (uint32_t)f.methods.size();
    uint32_t vt = wx86_scratch_alloc(4 * n);
    for (uint32_t i = 0; i < n; ++i) {
        Shim s; s.argc = i == 0 ? 3 : i < 3 ? 1 : f.methods[i - 3].argc; s.stdcall_cleanup = true;
        s.tag = std::string("dshow!") + f.name + "." + std::to_string(i);
        const Iface* fp = &f;
        s.fn = [fp, i](Cpu& cc) {
            uint32_t r = method(cc, *fp, i);
            static const bool trace = std::getenv("W3_TRACE_DSHOW") != nullptr;
            if (trace) log("[not-bound] dshow %s.%u(%x,%x,%x) -> 0x%x ra=0x%08x", fp->name, i, cc.arg(1), cc.arg(2), cc.arg(3), r, cc.read_u32(cc.reg(d2rt::R_ESP)));
            return r;
        };
        std::string key = std::string(f.name) + "." + std::to_string(i);
        br.register_shim("native.dshow", key, s);
        c.write_u32(vt + 4 * i, br.shim_trap("native.dshow", key));
    }
    g_vtables[f.name] = vt;
    return vt;
}

}  // namespace

void install_dshow(d2rt::Bridge& br) {
    Shim s; s.argc = 5; s.stdcall_cleanup = true; s.tag = "ole32.dll!CoCreateInstance";
    s.fn = [](Cpu& c) -> uint32_t {
        uint8_t g[16] = {}; c.read(c.arg(0), g, 16);
        uint8_t iid[16] = {}; if (c.arg(3)) c.read(c.arg(3), iid, 16);
        if (c.arg(4)) c.write_u32(c.arg(4), 0);
        static const uint8_t fg[16] = { 0xB3, 0xEB, 0x36, 0xE4, 0x4F, 0x52, 0xCE, 0x11, 0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 };
        if (!std::memcmp(g, fg, 16)) {                             // CLSID_FilterGraph
            uint32_t o = is_dshow_iid(iid) ? object_for(c, iid[0]) : 0;
            log("[not-bound] CoCreateInstance(FilterGraph, %02x) -> stand-in graph 0x%08x", iid[0], o);
            if (c.arg(4)) c.write_u32(c.arg(4), o);
            return o ? S_OK_ : E_NOINTERFACE_;
        }
        static const uint8_t ds[8] = { 0x46, 0xD9, 0xD4, 0x47, 0xE8, 0x62, 0xCF, 0x11 };   // CLSID_DirectSound
        if (!std::memcmp(g, ds, 8) && c.arg(4)) {
            return call_shim(c, "DSOUND.dll", "DirectSoundCreate", { 0, c.arg(4), 0 });
        }
        log("[not-bound] CoCreateInstance clsid=%08x-%04x-%04x: class not registered",
            g[0] | g[1] << 8 | g[2] << 16 | (uint32_t)g[3] << 24, g[4] | g[5] << 8, g[6] | g[7] << 8);
        return 0x80040154u;                                        // REGDB_E_CLASSNOTREG
    };
    br.register_shim("ole32.dll", "CoCreateInstance", s);
}

}  // namespace sc
