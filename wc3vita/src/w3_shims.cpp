// w3_shims.cpp — Win32 shims WarCraft III 1.27a needs beyond the StarCraft
// and Deus Ex sets (MSVCR120 runtime, Storm 1.27, Game.dll). Installed last
// (last registration wins).
#include "sc_host.h"

#include "runtime/guest_scratch.h"
#include "runtime/guest_str.h"
#include "runtime/host_clock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include <string>

#include "runtime/guest_files.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/win32_shims_kernel32.h"

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {
namespace {

void R(d2rt::Bridge& br, const char* dll, const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
    if (std::getenv("SC_TRACE_ALL")) {
        std::string t = name;
        fn = [t, f = std::move(fn)](Cpu& c) { uint32_t r = f(c); log("[not-bound] call %s -> 0x%x ra=0x%08x", t.c_str(), r, c.trap_retaddr()); return r; };
    }
    Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true;
    s.tag = std::string(dll) + "!" + name;
    br.register_shim(dll, name, s);
}


// Stubs: imports the game links against but never needs on the Vita (the
// World Editor's UI, MIDI/CD audio, consoles, pipes ...). Each pops its
// stdcall arguments and returns a fixed value; the first call is logged.
struct Stub { const char* dll; const char* name; uint8_t argc; uint32_t ret; };
const Stub kStubs[] = {
    {"ADVAPI32.dll","RegDeleteKeyA",2,2},
    {"COMCTL32.dll","ImageList_Add",3,0xFFFFFFFFu},{"COMCTL32.dll","ImageList_BeginDrag",4,0},
    {"COMCTL32.dll","ImageList_Create",5,0},{"COMCTL32.dll","ImageList_Destroy",1,1},
    {"COMCTL32.dll","ImageList_DragEnter",3,0},{"COMCTL32.dll","ImageList_DragLeave",1,0},
    {"COMCTL32.dll","ImageList_DragMove",2,0},{"COMCTL32.dll","ImageList_DragShowNolock",1,0},
    {"COMCTL32.dll","ImageList_EndDrag",0,0},{"COMCTL32.dll","ImageList_GetImageCount",1,0},
    {"COMCTL32.dll","ImageList_Replace",4,0},{"COMCTL32.dll","InitCommonControlsEx",1,1},
    {"COMCTL32.dll","_TrackMouseEvent",1,1},
    {"COMDLG32.dll","GetOpenFileNameA",1,0},{"COMDLG32.dll","GetSaveFileNameA",1,0},
    {"GDI32.dll","CombineRgn",4,1},{"GDI32.dll","CreateRectRgn",4,0x00D20004u},
    {"GDI32.dll","CreateRectRgnIndirect",1,0x00D20008u},{"GDI32.dll","ExtTextOutA",8,1},
    {"GDI32.dll","FillRgn",3,1},{"GDI32.dll","GetBkColor",1,0xFFFFFF},{"GDI32.dll","GetCharABCWidthsA",4,0},
    {"GDI32.dll","GetCurrentObject",2,0},{"GDI32.dll","GetDIBits",7,0},{"GDI32.dll","GetRegionData",3,0},
    {"GDI32.dll","GetTextMetricsA",2,0},{"GDI32.dll","RectInRegion",2,0},{"GDI32.dll","Rectangle",5,1},
    {"GDI32.dll","SetTextAlign",2,0},{"GDI32.dll","TextOutW",5,1},
    {"IMM32.dll","ImmAssociateContext",2,0},{"IMM32.dll","ImmAssociateContextEx",3,1},{"IMM32.dll","ImmNotifyIME",4,1},
    {"KERNEL32.dll","AreFileApisANSI",0,1},{"KERNEL32.dll","Beep",2,1},{"KERNEL32.dll","ChangeTimerQueueTimer",4,0},
    {"KERNEL32.dll","CreateHardLinkW",3,0},{"KERNEL32.dll","CreatePipe",4,0},{"KERNEL32.dll","CreateProcessA",10,0},
    {"KERNEL32.dll","CreateTimerQueue",0,0},{"KERNEL32.dll","CreateTimerQueueTimer",7,0},
    {"KERNEL32.dll","DeleteTimerQueueTimer",3,1},{"KERNEL32.dll","EnumSystemLocalesW",2,1},
    {"KERNEL32.dll","FreeLibraryAndExitThread",2,0},{"KERNEL32.dll","GetExitCodeProcess",2,0},
    {"KERNEL32.dll","GetLogicalProcessorInformation",2,0},{"KERNEL32.dll","GetNumaHighestNodeNumber",1,0},
    {"KERNEL32.dll","GetNumberOfConsoleInputEvents",2,0},{"KERNEL32.dll","GetProcessHeaps",2,0},
    {"KERNEL32.dll","GetProcessId",1,1},{"KERNEL32.dll","GetThreadTimes",5,0},
    {"KERNEL32.dll","GlobalHandle",1,0},{"KERNEL32.dll","HeapCompact",2,0x100000},
    {"KERNEL32.dll","HeapLock",1,1},{"KERNEL32.dll","HeapUnlock",1,1},{"KERNEL32.dll","HeapQueryInformation",5,0},
    {"KERNEL32.dll","HeapValidate",3,1},{"KERNEL32.dll","HeapWalk",2,0},{"KERNEL32.dll","LockFileEx",6,1},
    {"KERNEL32.dll","UnlockFileEx",5,1},{"KERNEL32.dll","OpenFile",3,0xFFFFFFFFu},
    {"KERNEL32.dll","OutputDebugStringW",1,0},{"KERNEL32.dll","PeekConsoleInputA",4,0},
    {"KERNEL32.dll","PeekNamedPipe",6,0},{"KERNEL32.dll","ReadConsoleInputA",4,0},{"KERNEL32.dll","ReadConsoleInputW",4,0},
    {"KERNEL32.dll","ReadConsoleW",5,0},{"KERNEL32.dll","RegisterWaitForSingleObject",6,0},
    {"KERNEL32.dll","RtlCaptureStackBackTrace",4,0},{"KERNEL32.dll","SetConsoleMode",2,0},
    {"KERNEL32.dll","SetEnvironmentVariableW",2,1},{"KERNEL32.dll","SetLocalTime",1,0},
    {"KERNEL32.dll","SetProcessAffinityMask",2,1},{"KERNEL32.dll","SignalObjectAndWait",4,0xFFFFFFFFu},
    {"KERNEL32.dll","SystemTimeToTzSpecificLocalTime",3,0},{"KERNEL32.dll","TerminateThread",2,0},
    {"KERNEL32.dll","UnregisterWait",1,1},{"KERNEL32.dll","UnregisterWaitEx",2,1},
    {"KERNEL32.dll","GetLogicalDrives",0,4},{"KERNEL32.dll","GetDriveTypeW",1,3},
    {"KERNEL32.dll","GetDiskFreeSpaceExW",4,0},{"KERNEL32.dll","CopyFileW",3,0},{"KERNEL32.dll","MoveFileExW",3,0},
    {"KERNEL32.dll","SetFileAttributesW",2,1},{"KERNEL32.dll","RemoveDirectoryW",1,0},
    {"KERNEL32.dll","SetCurrentDirectoryW",1,1},{"KERNEL32.dll","LocalFileTimeToFileTime",2,0},
    {"KERNEL32.dll","GetDateFormatW",6,0},{"KERNEL32.dll","GetTimeFormatW",6,0},
    {"KERNEL32.dll","GetModuleHandleExW",3,0},
    {"SHELL32.dll","DragAcceptFiles",2,0},{"SHELL32.dll","DragQueryFileA",4,0},{"SHELL32.dll","FindExecutableA",3,0},
    {"SHELL32.dll","SHBrowseForFolderA",1,0},{"SHELL32.dll","SHGetMalloc",1,0x80004005u},
    {"SHELL32.dll","SHGetPathFromIDListA",2,0},
    {"SHLWAPI.dll","PathIsRelativeW",1,1},{"SHLWAPI.dll","PathRelativePathToW",5,0},
    {"USER32.dll","ChangeDisplaySettingsExA",5,0},{"USER32.dll","CloseClipboard",0,1},{"USER32.dll","CreateCursor",7,0x00D30004u},
    {"USER32.dll","CreateDialogIndirectParamA",5,0},{"USER32.dll","CreateMenu",0,0x00D40004u},
    {"USER32.dll","CreatePopupMenu",0,0x00D40008u},{"USER32.dll","DeleteMenu",3,1},{"USER32.dll","DestroyCursor",1,1},
    {"USER32.dll","DestroyIcon",1,1},{"USER32.dll","DestroyMenu",1,1},{"USER32.dll","DrawEdge",4,1},
    {"USER32.dll","DrawFocusRect",2,1},{"USER32.dll","EmptyClipboard",0,1},{"USER32.dll","EndDialog",2,1},
    {"USER32.dll","FillRect",3,1},{"USER32.dll","GetClipboardData",1,0},
    {"USER32.dll","GetDlgItem",2,0},{"USER32.dll","GetDCEx",3,0x87000001u},{"USER32.dll","GetDlgItemTextA",4,0},{"USER32.dll","GetMenuItemCount",1,0},
    {"USER32.dll","GetMenuItemInfoA",4,0},{"USER32.dll","GetScrollInfo",3,0},{"USER32.dll","GetSysColorBrush",1,0x00D50004u},
    {"USER32.dll","GetWindowInfo",2,0},{"USER32.dll","GetWindowPlacement",2,0},{"USER32.dll","InsertMenuItemA",4,1},
    {"USER32.dll","OpenClipboard",1,0},{"USER32.dll","SendDlgItemMessageA",5,0},{"USER32.dll","SetClipboardData",2,0},
    {"USER32.dll","SetDlgItemTextA",3,1},{"USER32.dll","SetMenuItemInfoA",4,1},{"USER32.dll","SetParent",2,0},
    {"USER32.dll","SetScrollInfo",4,0},{"USER32.dll","SetScrollPos",4,0},{"USER32.dll","SetWindowPlacement",2,1},
    {"USER32.dll","TrackPopupMenu",7,0},{"USER32.dll","UnregisterClassA",2,1},{"USER32.dll","WindowFromPoint",2,0},
    {"WININET.dll","InternetCanonicalizeUrlA",4,0},
    // WINMM: no MIDI, CD audio, mixer or waveIn/waveOut device (Miles mixes
    // through DirectSound); opens fail with MMSYSERR_NODRIVER (6).
    {"WINMM.dll","auxGetDevCapsA",3,6},{"WINMM.dll","auxGetNumDevs",0,0},{"WINMM.dll","auxGetVolume",2,6},
    {"WINMM.dll","auxSetVolume",2,6},{"WINMM.dll","mciSendCommandA",4,6},{"WINMM.dll","midiOutClose",1,6},
    {"WINMM.dll","midiOutLongMsg",3,6},{"WINMM.dll","midiOutOpen",5,6},{"WINMM.dll","midiOutPrepareHeader",3,6},
    {"WINMM.dll","midiOutReset",1,6},{"WINMM.dll","midiOutShortMsg",2,6},{"WINMM.dll","midiOutUnprepareHeader",3,6},
    {"WINMM.dll","mixerClose",1,6},{"WINMM.dll","mixerGetControlDetailsA",3,6},{"WINMM.dll","mixerGetLineControlsA",3,6},
    {"WINMM.dll","mixerGetLineInfoA",3,6},{"WINMM.dll","mixerGetNumDevs",0,0},{"WINMM.dll","mixerOpen",5,6},
    {"WINMM.dll","mixerSetControlDetails",3,6},{"WINMM.dll","waveInAddBuffer",3,6},{"WINMM.dll","waveInClose",1,6},
    {"WINMM.dll","waveInOpen",6,6},{"WINMM.dll","waveInPrepareHeader",3,6},{"WINMM.dll","waveInReset",1,6},
    {"WINMM.dll","waveInStart",1,6},{"WINMM.dll","waveInUnprepareHeader",3,6},{"WINMM.dll","waveOutClose",1,6},
    {"WINMM.dll","waveOutGetDevCapsA",3,6},{"WINMM.dll","waveOutGetID",2,6},{"WINMM.dll","waveOutOpen",6,6},
    {"WINMM.dll","waveOutPrepareHeader",3,6},{"WINMM.dll","waveOutReset",1,6},{"WINMM.dll","waveOutUnprepareHeader",3,6},
    {"WINMM.dll","waveOutWrite",3,6},
};

// Guest wide string -> host narrow (ASCII range; WarCraft III paths are ASCII).
std::string wstr(Cpu& c, uint32_t p) {
    std::string a; if (!p) return a;
    for (uint32_t i = 0; i < 4096; ++i) { uint16_t ch = 0; c.read(p + 2 * i, &ch, 2); if (!ch) break; a += (char)(ch < 0x80 ? ch : '?'); }
    return a;
}
// Narrow copy of a guest string in a small rotating scratch pool, for
// forwarding a W call to the A shim.
uint32_t tmp_cstr(Cpu& c, const std::string& s) {
    static uint32_t pool = 0, at = 0; const uint32_t kPool = 0x4000;
    if (!pool) pool = wx86_scratch_alloc(kPool);
    if (at + s.size() + 1 > kPool) at = 0;
    uint32_t p = pool + at; c.write(p, s.c_str(), (uint32_t)s.size() + 1);
    at += ((uint32_t)s.size() + 4) & ~3u;
    return p;
}

// Windows mutexes are recursive: the owning thread may take one again and must
// release it as many times. The engine models a mutex as an auto-reset event,
// which is fine for StarCraft but deadlocks Miles (mss32), whose API lock
// nests: AIL_waveOutOpen takes it, then calls helpers that take it again.
// A mutex whose owner has exited is "abandoned" and goes to the next waiter.
struct W3Mutex : d2rt::Waitable {
    uint32_t owner = 0; int count = 0; d2rt::GuestThread* owner_t = nullptr;
    static uint32_t id_of(d2rt::GuestThread* t) { return t ? t->id : 1u; }
    bool abandoned() const { return owner && owner_t && owner_t->state == d2rt::GuestThread::State::Finished; }
    bool ready(d2rt::GuestThread* t) override { return !owner || owner == id_of(t) || abandoned(); }
    bool try_acquire(d2rt::GuestThread* t) override {
        if (!ready(t)) return false;
        if (owner != id_of(t)) { owner = id_of(t); owner_t = t; count = 0; }
        ++count; return true;
    }
    const char* kind() const override { return "mutex"; }
};
d2rt::GuestThread* cur_thread() { d2rt::ThreadScheduler* s = wx86_sched(); return s ? s->current() : nullptr; }

// Overlapped file I/O. Storm and Game.dll associate some file handles with an
// I/O completion port and read them with an OVERLAPPED: the read starts at the
// OVERLAPPED's offset, and its completion is reported through the OVERLAPPED,
// its event and a packet on the port, even when the read finished at once
// (which ours always do, since reads here are synchronous).
struct PortLink { uint32_t port, key; };
std::map<uint32_t, PortLink> g_port_of;   // file handle -> completion port
d2rt::Shim g_read_sync, g_write_sync;     // the engine's synchronous ReadFile/WriteFile

uint32_t overlapped_io(Cpu& c, const d2rt::Shim& sync, const char* what) {
    const uint32_t h = c.arg(0), n = c.arg(2), outp = c.arg(3), ov = c.arg(4);
    if (!ov) return sync.fn(c);
    const uint64_t off = c.read_u32(ov + 8) | (uint64_t)c.read_u32(ov + 12) << 32;
    auto& pos = wx86_file_pos();
    if (pos.count(h)) pos[h].logical = (long)off;
    // The engine's shim writes the byte count through arg 3; give it a slot
    // even when the caller passed NULL (allowed with an OVERLAPPED).
    static uint32_t s_count = 0; if (!s_count) s_count = wx86_scratch_alloc(16);
    const uint32_t slot = c.reg(d2rt::R_ESP) + 16;
    if (!outp) c.write_u32(slot, s_count);
    const uint32_t ok = sync.fn(c);
    if (!outp) c.write_u32(slot, 0);
    const uint32_t got = c.read_u32(outp ? outp : s_count);
    c.write_u32(ov + 0, ok ? 0 : 0xC0000011u);   // Internal: STATUS_SUCCESS / STATUS_END_OF_FILE
    c.write_u32(ov + 4, got);                     // InternalHigh: bytes transferred
    if (uint32_t ev = c.read_u32(ov + 16) & ~1u) {
        if (d2rt::Waitable* w = wx86_handle_find(ev))
            if (wx86_is_kind(w, "event")) { static_cast<WxEvent*>(w)->signaled = true;
                if (d2rt::ThreadScheduler* s = wx86_sched()) s->notify(w); }
    }
    auto it = g_port_of.find(h);
    if (it != g_port_of.end() && !(c.read_u32(ov + 16) & 1u)) {
        if (d2rt::Waitable* w = wx86_handle_find(it->second.port))
            if (wx86_is_kind(w, "iocp")) {
                static_cast<WxIocp*>(w)->q.push_back({ got, it->second.key, ov });
                if (d2rt::ThreadScheduler* s = wx86_sched()) s->notify(w);
            }
    }
    static int logged = 0;
    if (logged++ < 6) log("[not-bound] overlapped %s(0x%x @%llu, %u) -> %u%s", what, h, (unsigned long long)off, n, got,
                          it != g_port_of.end() ? " (completion queued on port)" : "");
    if (!ok) { wx86_set_lasterr(c, 38); return 0; }   // ERROR_HANDLE_EOF
    return 1;
}
}  // namespace

// Call another shim (by DLL and name) natively, with its arguments laid out
// on the guest stack below the current frame as a guest call would.
uint32_t call_shim(Cpu& c, const char* dll, const char* name, std::initializer_list<uint32_t> args) {
    const d2rt::Shim* s = g_br->find_shim(dll, name);
    if (!s) { log("[not-bound] [cause: missing shim] forward target %s!%s", dll, name); return 0; }
    const uint32_t esp0 = c.reg(d2rt::R_ESP);
    uint32_t esp = (esp0 - 0x200u) & ~15u;
    std::vector<uint32_t> a(args);
    for (size_t i = a.size(); i-- > 0;) { esp -= 4; c.write_u32(esp, a[i]); }
    esp -= 4; c.write_u32(esp, 0);                                 // return address slot
    c.set_reg(d2rt::R_ESP, esp);
    uint32_t r = s->fn(c);
    c.set_reg(d2rt::R_ESP, esp0);
    return r;
}

// The game's settings live in HKCU\Software\Blizzard Entertainment\Warcraft III.
// The port starts from these (the in-memory registry is empty otherwise).
void seed_registry() {
    const std::string k = "hkcu\\software\\blizzard entertainment\\warcraft iii";
    // The first-launch intro (TutorialIn) tears the 3D renderer down and
    // builds it again around the movie; there is no movie playback yet, so
    // mark it as seen.
    reg_seed_dword(k + "\\misc", "seenintromovie", 1);
    uint32_t w = 800, h = 600;
    if (const char* r = std::getenv("W3_RES")) std::sscanf(r, "%ux%u", &w, &h);
    reg_seed_dword(k + "\\video", "reswidth", w);
    reg_seed_dword(k + "\\video", "resheight", h);
    reg_seed_dword(k + "\\video", "colordepth", 32);
    reg_seed_dword(k + "\\video", "refreshrate", 60);
}

void install_w3(d2rt::Bridge& br) {
    seed_registry();
    if (const d2rt::Shim* r = br.find_shim("KERNEL32.dll", "ReadFile")) g_read_sync = *r;
    if (const d2rt::Shim* w = br.find_shim("KERNEL32.dll", "WriteFile")) g_write_sync = *w;
    R(br, "KERNEL32.dll", "ReadFile", 5, [](Cpu& c) { return overlapped_io(c, g_read_sync, "ReadFile"); });
    R(br, "KERNEL32.dll", "WriteFile", 5, [](Cpu& c) { return overlapped_io(c, g_write_sync, "WriteFile"); });
    R(br, "KERNEL32.dll", "CreateIoCompletionPort", 4, [](Cpu& c) -> uint32_t {
        const uint32_t file = c.arg(0), existing = c.arg(1), key = c.arg(2);
        uint32_t port = existing;
        if (!port) { WxIocp* p = new WxIocp(); p->cpu = &c; port = wx86_handle_add(p); }
        if (file != 0xFFFFFFFFu) {
            g_port_of[file] = { port, key };
            log("[not-bound] CreateIoCompletionPort: handle 0x%x -> port 0x%x key 0x%x", file, port, key);
        }
        return port;
    });
    R(br, "KERNEL32.dll", "GetOverlappedResult", 4, [](Cpu& c) -> uint32_t {
        const uint32_t ov = c.arg(1);
        if (!ov) return 0;
        if (c.arg(2)) c.write_u32(c.arg(2), c.read_u32(ov + 4));
        const uint32_t st = c.read_u32(ov);
        if (st == 0) return 1;
        wx86_set_lasterr(c, st == 0x103 ? 996 : 38);   // ERROR_IO_INCOMPLETE / ERROR_HANDLE_EOF
        return 0;
    });
    R(br, "KERNEL32.dll", "CancelIo", 1, [](Cpu&) -> uint32_t { return 1; });
    for (const char* n : { "CreateMutexA", "CreateMutexW" })
        R(br, "KERNEL32.dll", n, 3, [](Cpu& c) -> uint32_t {
            W3Mutex* m = new W3Mutex();
            if (c.arg(1)) { d2rt::GuestThread* t = cur_thread(); m->owner = W3Mutex::id_of(t); m->owner_t = t; m->count = 1; }
            uint32_t h = wx86_handle_add(m); wx86_set_lasterr(c, 0);
            return h;
        });
    R(br, "KERNEL32.dll", "ReleaseMutex", 1, [](Cpu& c) -> uint32_t {
        d2rt::Waitable* w = wx86_handle_find(c.arg(0));
        if (!w || !wx86_is_kind(w, "mutex")) { wx86_set_lasterr(c, 6); return 0; }   // ERROR_INVALID_HANDLE
        W3Mutex* m = static_cast<W3Mutex*>(w);
        if (m->owner != W3Mutex::id_of(cur_thread()) || m->count <= 0) { wx86_set_lasterr(c, 288); return 0; }  // ERROR_NOT_OWNER
        if (--m->count == 0) { m->owner = 0; m->owner_t = nullptr;
            if (d2rt::ThreadScheduler* s = wx86_sched()) s->notify(m); }
        return 1;
    });
    // ---- interlocked singly linked lists (MSVCR120's concurrency runtime) --
    // SLIST_HEADER (8 bytes): Next pointer, Depth (16), Sequence (16). Guest
    // threads are cooperative, so a plain update is atomic for them.
    R(br, "KERNEL32.dll", "InitializeSListHead", 1, [](Cpu& c) {
        c.write_u32(c.arg(0), 0); c.write_u32(c.arg(0) + 4, 0); return 0u;
    });
    R(br, "KERNEL32.dll", "InterlockedPushEntrySList", 2, [](Cpu& c) {
        uint32_t h = c.arg(0), e = c.arg(1), first = c.read_u32(h), ds = c.read_u32(h + 4);
        c.write_u32(e, first); c.write_u32(h, e);
        c.write_u32(h + 4, ((ds & 0xFFFF) + 1) | ((ds + 0x10000) & 0xFFFF0000u));
        return first;
    });
    R(br, "KERNEL32.dll", "InterlockedPopEntrySList", 1, [](Cpu& c) {
        uint32_t h = c.arg(0), first = c.read_u32(h), ds = c.read_u32(h + 4);
        if (!first) return 0u;
        c.write_u32(h, c.read_u32(first));
        c.write_u32(h + 4, (((ds & 0xFFFF) - 1) & 0xFFFF) | ((ds + 0x10000) & 0xFFFF0000u));
        return first;
    });
    R(br, "KERNEL32.dll", "InterlockedFlushSList", 1, [](Cpu& c) {
        uint32_t h = c.arg(0), first = c.read_u32(h), ds = c.read_u32(h + 4);
        c.write_u32(h, 0); c.write_u32(h + 4, (ds + 0x10000) & 0xFFFF0000u);
        return first;
    });
    R(br, "KERNEL32.dll", "QueryDepthSList", 1, [](Cpu& c) { return c.read_u32(c.arg(0) + 4) & 0xFFFF; });
    for (const Stub& st : kStubs) {
        std::string tag = std::string(st.dll) + "!" + st.name; uint32_t ret = st.ret;
        R(br, st.dll, st.name, st.argc, [tag, ret](Cpu&) {
            static std::set<std::string> seen;
            if (seen.insert(tag).second) log("[not-bound] [cause: stub] %s -> 0x%x", tag.c_str(), ret);
            return ret;
        });
    }
    // One display adapter, attached and primary (the GL renderer looks for it).
    R(br, "USER32.dll", "EnumDisplayDevicesA", 4, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(2);
        if (c.arg(1) != 0 || !p) return 0u;
        uint32_t cb = c.read_u32(p); if (cb < 4 + 32 + 128 + 4) return 0u;
        std::vector<uint8_t> z(cb - 4, 0); c.write(p + 4, z.data(), (uint32_t)z.size());
        const char* name = "\\\\.\\DISPLAY1"; const char* str = "Vita GPU";
        if (c.arg(0)) { name = "\\\\.\\DISPLAY1\\Monitor0"; str = "Vita LCD"; }
        c.write(p + 4, name, (uint32_t)std::strlen(name)); c.write(p + 36, str, (uint32_t)std::strlen(str));
        c.write_u32(p + 164, 0x1 | 0x4);                    // ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
        return 1u;
    });
    // Display modes (DEVMODEA: bpp @104, width @108, height @112, freq @120).
    // The game lists every mode and offers them in its video options.
    R(br, "USER32.dll", "EnumDisplaySettingsA", 3, [](Cpu& c) -> uint32_t {
        static const uint32_t modes[][2] = { {640, 480}, {800, 600}, {960, 544}, {1024, 768} };
        const uint32_t n = sizeof modes / sizeof modes[0];
        uint32_t i = c.arg(1), dm = c.arg(2);
        if (i == 0xFFFFFFFFu || i == 0xFFFFFFFEu) i = 1;      // ENUM_CURRENT/REGISTRY_SETTINGS: 800x600
        else if (i >= n) return 0u;
        if (!dm) return 0u;
        c.write_u32(dm + 40, 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u);   // BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY
        c.write_u32(dm + 104, 32); c.write_u32(dm + 108, modes[i][0]); c.write_u32(dm + 112, modes[i][1]);
        c.write_u32(dm + 116, 0); c.write_u32(dm + 120, 60);
        return 1u;
    });
    // ---- W file and module calls (MSVCR120): forwarded to the A shims ------
    R(br, "KERNEL32.dll", "CreateFileW", 7, [](Cpu& c) {
        return call_shim(c, "KERNEL32.dll", "CreateFileA", { tmp_cstr(c, wstr(c, c.arg(0))), c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5), c.arg(6) });
    });
    R(br, "KERNEL32.dll", "DeleteFileW", 1, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "DeleteFileA", { tmp_cstr(c, wstr(c, c.arg(0))) }); });
    R(br, "KERNEL32.dll", "CreateDirectoryW", 2, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "CreateDirectoryA", { tmp_cstr(c, wstr(c, c.arg(0))), c.arg(1) }); });
    R(br, "KERNEL32.dll", "GetFileAttributesW", 1, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "GetFileAttributesA", { tmp_cstr(c, wstr(c, c.arg(0))) }); });
    R(br, "KERNEL32.dll", "GetFileAttributesExW", 3, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "GetFileAttributesExA", { tmp_cstr(c, wstr(c, c.arg(0))), c.arg(1), c.arg(2) }); });
    R(br, "KERNEL32.dll", "MoveFileA", 2, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "MoveFileExA", { c.arg(0), c.arg(1), 1 }); });
    auto widen_out = [](Cpu& c, uint32_t out, uint32_t cap, uint32_t tmp, uint32_t n) -> uint32_t {
        // n = narrow length the A call reported; copy it out as UTF-16.
        if (n >= cap) return n + 1;
        for (uint32_t i = 0; i <= n; ++i) { uint8_t b = 0; c.read(tmp + i, &b, 1); uint16_t w = b; c.write(out + 2 * i, &w, 2); }
        return n;
    };
    R(br, "KERNEL32.dll", "GetModuleFileNameW", 3, [widen_out](Cpu& c) {
        uint32_t tmp = tmp_cstr(c, std::string(260, ' '));
        uint32_t n = call_shim(c, "KERNEL32.dll", "GetModuleFileNameA", { c.arg(0), tmp, 260 });
        return widen_out(c, c.arg(1), c.arg(2), tmp, n);
    });
    R(br, "KERNEL32.dll", "GetCurrentDirectoryW", 2, [widen_out](Cpu& c) {
        uint32_t tmp = tmp_cstr(c, std::string(260, ' '));
        uint32_t n = call_shim(c, "KERNEL32.dll", "GetCurrentDirectoryA", { 260, tmp });
        return widen_out(c, c.arg(1), c.arg(0), tmp, n);
    });
    R(br, "KERNEL32.dll", "GetTempPathW", 2, [widen_out](Cpu& c) {
        uint32_t tmp = tmp_cstr(c, "C:\\Warcraft III\\");
        return widen_out(c, c.arg(1), c.arg(0), tmp, 16);
    });
    R(br, "KERNEL32.dll", "GetFullPathNameW", 4, [widen_out](Cpu& c) {
        uint32_t tmp = tmp_cstr(c, std::string(260, ' '));
        uint32_t n = call_shim(c, "KERNEL32.dll", "GetFullPathNameA", { tmp_cstr(c, wstr(c, c.arg(0))), 260, tmp, 0 });
        uint32_t r = widen_out(c, c.arg(2), c.arg(1), tmp, n);
        if (c.arg(3)) c.write_u32(c.arg(3), 0);
        return r;
    });
    R(br, "KERNEL32.dll", "FindFirstFileW", 2, [](Cpu& c) { wx86_set_lasterr(c, 2); return 0xFFFFFFFFu; });
    R(br, "KERNEL32.dll", "FindFirstFileExW", 6, [](Cpu& c) { wx86_set_lasterr(c, 2); return 0xFFFFFFFFu; });
    R(br, "KERNEL32.dll", "FindNextFileW", 2, [](Cpu& c) { wx86_set_lasterr(c, 18); return 0u; });
    R(br, "KERNEL32.dll", "FindFirstFileExA", 6, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "FindFirstFileA", { c.arg(0), c.arg(2) }); });
    R(br, "KERNEL32.dll", "LoadLibraryW", 1, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "LoadLibraryA", { tmp_cstr(c, wstr(c, c.arg(0))) }); });
    R(br, "KERNEL32.dll", "LoadLibraryExW", 3, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "LoadLibraryA", { tmp_cstr(c, wstr(c, c.arg(0))) }); });
    R(br, "KERNEL32.dll", "LoadLibraryExA", 3, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "LoadLibraryA", { c.arg(0) }); });
    R(br, "KERNEL32.dll", "SetFilePointerEx", 5, [](Cpu& c) {
        static uint32_t hbuf = 0; if (!hbuf) hbuf = wx86_scratch_alloc(4);
        c.write_u32(hbuf, c.arg(2));
        uint32_t lo = call_shim(c, "KERNEL32.dll", "SetFilePointer", { c.arg(0), c.arg(1), hbuf, c.arg(4) });
        if (lo == 0xFFFFFFFFu && wx86_get_lasterr(c)) return 0u;
        if (c.arg(3)) { c.write_u32(c.arg(3), lo); c.write_u32(c.arg(3) + 4, c.read_u32(hbuf)); }
        return 1u;
    });
    R(br, "KERNEL32.dll", "GetFileInformationByHandle", 2, [](Cpu& c) {
        uint8_t z[52] = {}; c.write(c.arg(1), z, sizeof z);
        c.write_u32(c.arg(1), 0x80);                               // FILE_ATTRIBUTE_NORMAL
        uint32_t hbuf = tmp_cstr(c, "    ");
        uint32_t lo = call_shim(c, "KERNEL32.dll", "GetFileSize", { c.arg(0), hbuf });
        c.write_u32(c.arg(1) + 32, c.read_u32(hbuf)); c.write_u32(c.arg(1) + 36, lo);
        c.write_u32(c.arg(1) + 40, 1);                             // nNumberOfLinks
        return 1u;
    });
    R(br, "KERNEL32.dll", "GetFileTime", 4, [](Cpu& c) {
        for (int i = 1; i <= 3; ++i) if (c.arg(i)) { c.write_u32(c.arg(i), 0xD53E8000u); c.write_u32(c.arg(i) + 4, 0x01D16A2Au); }
        return 1u;
    });
    R(br, "KERNEL32.dll", "WaitForSingleObjectEx", 3, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "WaitForSingleObject", { c.arg(0), c.arg(1) }); });
    R(br, "KERNEL32.dll", "ResumeThread", 1, [](Cpu&) { return 1u; });
    R(br, "KERNEL32.dll", "GlobalLock", 1, [](Cpu& c) { return c.arg(0); });
    R(br, "KERNEL32.dll", "GlobalUnlock", 1, [](Cpu&) { return 1u; });
    R(br, "KERNEL32.dll", "GlobalSize", 1, [](Cpu& c) { return call_shim(c, "KERNEL32.dll", "HeapSize", { 0, 0, c.arg(0) }); });
    R(br, "KERNEL32.dll", "InterlockedCompareExchange", 3, [](Cpu& c) {
        uint32_t old = c.read_u32(c.arg(0)); if (old == c.arg(2)) c.write_u32(c.arg(0), c.arg(1)); return old;
    });
    R(br, "KERNEL32.dll", "TryEnterCriticalSection", 1, [](Cpu& c) {
        auto* cs = wx86_crit_for(c.arg(0));
        auto* s = wx86_sched(); auto* t = s ? s->current() : nullptr;
        return cs->try_acquire(t) ? 1u : 0u;
    });
    R(br, "KERNEL32.dll", "lstrcatA", 2, [](Cpu& c) {
        std::string a = wx86_gread_mb(c, c.arg(0), -1), b = wx86_gread_mb(c, c.arg(1), -1);
        c.write(c.arg(0) + (uint32_t)a.size(), b.c_str(), (uint32_t)b.size() + 1); return c.arg(0);
    });
}

}  // namespace sc
