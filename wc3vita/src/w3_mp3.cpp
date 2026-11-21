// w3_mp3.cpp — native MP3 decoding for Miles' Mp3dec.asi (the game's music).
//
// WarCraft III's music is MP3, decoded by Miles' Mp3dec.asi provider, an x87
// floating-point decoder. Through the dynarec it costs a large share of the
// one guest core: the build 6 Vita log has the Miles thread at ~17% of it,
// and the host profile puts three quarters of that in Mp3dec's DCT/synthesis
// blocks. Here the provider's six "ASI stream" entry points are pointed at a
// native decoder (minimp3, CC0, NEON on the Vita); Mp3dec.asi still loads and
// registers itself with Miles as usual, so the game and Miles see the same
// provider and the same attribute tokens.
//
// The entry points (Mp3dec.asi 1.27a, preferred base 0x26f00000, all stdcall):
//   open(user, fetch_cb, total_size) -> stream   0x26f01c90
//   process(stream, buffer, bytes)    -> bytes    0x26f01dd0
//   close(stream)                                 0x26f01da0
//   seek(stream, offset)                          0x26f01ec0
//   attribute(stream, token)          -> value    0x26f01f30
//   set_preference(stream, token, &v) -> old      0x26f021c0
// fetch_cb(user, dest, bytes, offset) is Miles' (guest) reader, stdcall;
// offset -1 = continue where the last read ended.
//
// Calling the guest reader from native code would need a nested guest run, so
// open and process are small x86 loops instead: they call a native "step",
// and when the step asks for input (returns -1) they call the reader with the
// arguments the step left in the stream block, then step again.
//
// W3_MP3=0 keeps Mp3dec.asi's own decoder.
#include "sc_host.h"

#include "runtime/guest_scratch.h"

#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {
namespace {

constexpr uint32_t kPref = 0x26f00000u;          // Mp3dec.asi preferred base
constexpr uint32_t kOpen = 0x26f01c90u, kProcess = 0x26f01dd0u, kClose = 0x26f01da0u,
                   kSeek = 0x26f01ec0u, kAttrib = 0x26f01f30u, kSetPref = 0x26f021c0u;
// Stream block in guest memory: the reader's arguments, its result, its buffer.
constexpr uint32_t kUser = 0, kCb = 4, kDest = 8, kBytes = 12, kOff = 16, kGot = 20, kBuf = 32;
constexpr uint32_t kFetch = 8192;                // bytes asked of the reader at a time
constexpr uint32_t kWant = 4096;                 // input kept ahead of the decoder (> 1 frame + header)

struct Stream {
    uint32_t h = 0, total = 0;
    mp3dec_t dec;
    std::vector<uint8_t> in; size_t in_pos = 0;  // fetched, not yet decoded
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint32_t pcm_bytes = 0, pcm_pos = 0;
    bool have_info = false; int hz = 0, ch = 0, kbps = 0, layer = 3, version = 1;
    uint32_t pos = 0;           // stream offset of the next byte the decoder takes ("Position")
    int32_t next_off = -1;      // offset argument of the next read (-1 = continue)
    bool pending = false;       // a read was asked for; its result is at kGot
    bool src_dry = false;       // the reader gave nothing during this call
    uint32_t written = 0;       // bytes written so far in the current process call
    int32_t pref[3] = { -1, -1, -1 };   // Requested sample rate / bit width / # of channels
};
std::mutex g_mx;
std::map<uint32_t, Stream*> g_streams;
std::vector<uint32_t> g_free_blocks;   // guest stream blocks of closed streams (scratch memory is never freed)
uint64_t g_frames = 0, g_opens = 0;
// W3_MP3_DUMP=<file>: the PCM every process call returns, appended (both
// decoders), to compare them sample for sample on the host.
FILE* g_dump = nullptr;
const bool g_trace = std::getenv("W3_MP3_LOG") != nullptr;   // W3_MP3_LOG=1: the first calls, logged
int g_traced = 0;
#define MP3T(...) do { if (g_trace && g_traced < 200) { ++g_traced; log("[not-bound] mp3: " __VA_ARGS__); } } while (0)
void dump_pcm(Cpu& c, uint32_t buf, uint32_t n) {
    if (!g_dump || !n) return;
    std::vector<uint8_t> v(n); c.read(buf, v.data(), n); std::fwrite(v.data(), 1, n, g_dump); std::fflush(g_dump);
}

Stream* find(uint32_t h) { std::lock_guard<std::mutex> lk(g_mx); auto it = g_streams.find(h); return it == g_streams.end() ? nullptr : it->second; }

// Ask the x86 loop for input: the reader's arguments go into the block.
uint32_t ask(Cpu& c, Stream& s) {
    c.write_u32(s.h + kDest, s.h + kBuf); c.write_u32(s.h + kBytes, kFetch);
    c.write_u32(s.h + kOff, (uint32_t)s.next_off); c.write_u32(s.h + kGot, 0);
    s.pending = true;
    return 0xFFFFFFFFu;
}

// Keep a frame's samples in the stream's first channel layout.
uint32_t frame_pcm(Stream& s, int samples, int ch) {
    if (ch == s.ch || samples <= 0) return (uint32_t)(samples * ch * 2);
    if (ch == 1 && s.ch == 2) for (int i = samples - 1; i >= 0; --i) s.pcm[2 * i] = s.pcm[2 * i + 1] = s.pcm[i];
    else if (ch == 2 && s.ch == 1) for (int i = 0; i < samples; ++i) s.pcm[i] = (int16_t)((s.pcm[2 * i] + s.pcm[2 * i + 1]) / 2);
    return (uint32_t)(samples * s.ch * 2);
}

// One step of process (bytes > 0) or of open's first-frame probe (bytes == 0).
// Returns the bytes written (probe: 1 = a frame was found, 0 = none), or -1
// when the x86 loop must call the reader first.
uint32_t step(Cpu& c, uint32_t h, uint32_t buf, uint32_t bytes) {
    Stream* sp = find(h); if (!sp) return 0;
    Stream& s = *sp;
    if (s.pending) {           // back from the reader
        s.pending = false;
        const int32_t got = (int32_t)c.read_u32(h + kGot);
        if (got > 0) {
            const uint32_t n = (uint32_t)got < kFetch ? (uint32_t)got : kFetch;
            if (s.in_pos > 0 && s.in_pos == s.in.size()) { s.in.clear(); s.in_pos = 0; }
            const size_t old = s.in.size(); s.in.resize(old + n);
            c.read(h + kBuf, s.in.data() + old, n);
            s.next_off = -1;
        } else s.src_dry = true;
        MP3T("read -> %d", got);
    } else { s.written = 0; s.src_dry = false; }
    for (;;) {
        if (bytes && s.pcm_pos < s.pcm_bytes) {
            uint32_t n = s.pcm_bytes - s.pcm_pos; if (n > bytes - s.written) n = bytes - s.written;
            c.write(buf + s.written, (const uint8_t*)s.pcm + s.pcm_pos, n);
            s.pcm_pos += n; s.written += n;
        }
        if (bytes && s.written >= bytes) { dump_pcm(c, buf, s.written); MP3T("process %u -> %u (pos %u)", bytes, s.written, s.pos); return s.written; }
        if (!bytes && s.have_info) return 1;
        size_t avail = s.in.size() - s.in_pos;
        if (avail < kWant && !s.src_dry) return ask(c, s);
        if (!avail) break;
        mp3dec_frame_info_t info; std::memset(&info, 0, sizeof info);
        const int samples = mp3dec_decode_frame(&s.dec, s.in.data() + s.in_pos, (int)avail, s.pcm, &info);
        if (!info.frame_bytes) {   // no frame in what is here
            if (s.src_dry) { s.pos += (uint32_t)avail; s.in_pos = s.in.size(); break; }
            if (avail > 4) { s.pos += (uint32_t)(avail - 4); s.in_pos = s.in.size() - 4; }   // junk: keep a header's worth
            return ask(c, s);
        }
        s.in_pos += (size_t)info.frame_bytes; s.pos += (uint32_t)info.frame_bytes;
        if (s.in_pos >= 65536) { s.in.erase(s.in.begin(), s.in.begin() + (long)s.in_pos); s.in_pos = 0; }
        if (samples <= 0) continue;   // ID3 tag or other skipped bytes
        if (!s.have_info) {
            s.have_info = true; s.hz = info.hz; s.ch = info.channels; s.kbps = info.bitrate_kbps; s.layer = info.layer;
            s.version = info.hz >= 32000 ? 1 : 2;
        }
        s.kbps = info.bitrate_kbps;   // as Mp3dec: the bit rate of the frame being played
        s.pcm_bytes = frame_pcm(s, samples, info.channels); s.pcm_pos = 0;
        ++g_frames;
    }
    // The reader ran dry: as Mp3dec, silence for the rest, and the bytes decoded.
    if (!bytes) return 0;
    if (s.written < bytes) {
        std::vector<uint8_t> z(bytes - s.written, 0);
        c.write(buf + s.written, z.data(), (uint32_t)z.size());
    }
    dump_pcm(c, buf, s.written);
    MP3T("process %u -> %u, reader dry (pos %u)", bytes, s.written, s.pos);
    return s.written;
}

uint32_t s_open_fn(Cpu& c, uint32_t user, uint32_t cb, uint32_t total) {
    uint32_t h = 0;
    { std::lock_guard<std::mutex> lk(g_mx); if (!g_free_blocks.empty()) { h = g_free_blocks.back(); g_free_blocks.pop_back(); } }
    if (!h) h = wx86_scratch_alloc(kBuf + kFetch);
    if (!h) { log("[not-bound] [cause: guest scratch full] mp3: no room for a stream block"); return 0; }
    Stream* s = new Stream();
    s->h = h; s->total = total; mp3dec_init(&s->dec);
    c.write_u32(h + kUser, user); c.write_u32(h + kCb, cb);
    { std::lock_guard<std::mutex> lk(g_mx); g_streams[h] = s; }
    ++g_opens;
    MP3T("open total %u", total);
    return h;
}
uint32_t s_close_fn(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_streams.find(h); if (it == g_streams.end()) return 0;
    delete it->second; g_streams.erase(it); g_free_blocks.push_back(h);
    return 0;
}
// As Mp3dec: the decoder starts over; -2 keeps the samples already decoded,
// -1 continues reading where the reader is, >= 0 reads from that offset.
uint32_t s_seek_fn(uint32_t h, int32_t off) {
    Stream* s = find(h); if (!s) return 0;
    mp3dec_init(&s->dec); s->in.clear(); s->in_pos = 0; s->pending = false;
    MP3T("seek %d", off);
    if (off != -2) { s->pcm_bytes = s->pcm_pos = 0; }
    s->next_off = off == -2 ? -1 : off;
    if (off >= 0) s->pos = (uint32_t)off;
    return 0;
}
uint32_t attribute(uint32_t h, uint32_t tok) {
    Stream* sp = find(h); if (!sp) return 0xFFFFFFFFu;
    const Stream& s = *sp;
    MP3T("attribute 0x%x", tok);
    const uint32_t bitrate = (uint32_t)s.kbps * 1000u, hz = (uint32_t)s.hz, ch = (uint32_t)s.ch;
    switch (tok) {
    case 0x04: return bitrate;                                                  // Input bit rate
    case 0x05: return hz;                                                       // Input sample rate
    case 0x06: { const uint32_t r = bitrate ? hz * ch * 16u / bitrate : 0;     // Input sample width
                 return 16u / (r < 1 ? 1u : r); }
    case 0x07: return ch;                                                       // Input channels
    case 0x08: return hz * ch * 16u;                                            // Output bit rate
    case 0x09: return hz;                                                       // Output sample rate
    case 0x0a: return 16;                                                       // Output sample width
    case 0x0b: return ch;                                                       // Output channels
    case 0x0c: return s.pos;                                                    // Position
    case 0x0d: { if (!s.pos || !s.total) return 0;                             // Percent done (a float)
                 const float f = (float)s.pos * 100.0f / (float)s.total; uint32_t u; std::memcpy(&u, &f, 4); return u; }
    case 0x0e: return 0x1000;                                                   // Minimum input block size
    case 0x0f: return (uint32_t)s.version;                                      // MPEG version
    case 0x10: return (uint32_t)s.layer;                                        // MPEG layer
    case 0x11: case 0x12: case 0x13: return (uint32_t)s.pref[tok - 0x11];       // the preferences
    default: return 0xFFFFFFFFu;
    }
}
uint32_t set_pref(Cpu& c, uint32_t h, uint32_t tok, uint32_t vp) {
    Stream* s = find(h); if (!s || tok < 0x11 || tok > 0x13) return 0xFFFFFFFFu;
    const uint32_t old = (uint32_t)s->pref[tok - 0x11];
    s->pref[tok - 0x11] = (int32_t)c.read_u32(vp);
    return old;
}

// ---- the x86 side ----------------------------------------------------------
struct Asm {
    std::vector<uint8_t> b; uint32_t org = 0;
    void put(std::initializer_list<uint8_t> v) { b.insert(b.end(), v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void call(uint32_t to) { put({ 0xE8 }); u32(to - (org + (uint32_t)b.size() + 4)); }
    // The reader call, its result into the block: ebx = block.
    void fetch() {
        put({ 0xFF, 0x73, (uint8_t)kOff, 0xFF, 0x73, (uint8_t)kBytes, 0xFF, 0x73, (uint8_t)kDest, 0xFF, 0x33,
              0xFF, 0x53, (uint8_t)kCb, 0x89, 0x43, (uint8_t)kGot });
    }
};
// process(stream, buffer, bytes): loop step / reader until step is done.
std::vector<uint8_t> process_stub(uint32_t org, uint32_t t_step) {
    Asm a; a.org = org;
    a.put({ 0x53, 0x8B, 0x5C, 0x24, 0x08 });                   // push ebx; mov ebx,[esp+8]
    const size_t loop = a.b.size();
    a.put({ 0xFF, 0x74, 0x24, 0x10, 0xFF, 0x74, 0x24, 0x10, 0x53 });   // push bytes; push buffer; push ebx
    a.call(t_step);
    a.put({ 0x83, 0xF8, 0xFF, 0x75, 0x00 }); const size_t jne = a.b.size() - 1;   // cmp eax,-1; jne done
    a.fetch();
    a.put({ 0xEB, 0x00 }); a.b.back() = (uint8_t)(int8_t)((int)loop - (int)a.b.size());   // jmp loop
    a.b[jne] = (uint8_t)(a.b.size() - (jne + 1));
    a.put({ 0x5B, 0xC2, 0x0C, 0x00 });                         // pop ebx; ret 12
    return a.b;
}
// open(user, reader, total): native open, then probe the first frame (the
// game asks for the sample rate right after); no frame = no stream, as Mp3dec.
std::vector<uint8_t> open_stub(uint32_t org, uint32_t t_open, uint32_t t_step, uint32_t t_close) {
    Asm a; a.org = org;
    a.put({ 0xFF, 0x74, 0x24, 0x0C, 0xFF, 0x74, 0x24, 0x0C, 0xFF, 0x74, 0x24, 0x0C });   // the three arguments
    a.call(t_open);
    a.put({ 0x85, 0xC0, 0x74, 0x00 }); const size_t jz = a.b.size() - 1;              // test eax,eax; jz out
    a.put({ 0x53, 0x8B, 0xD8 });                               // push ebx; mov ebx,eax
    const size_t loop = a.b.size();
    a.put({ 0x6A, 0x00, 0x6A, 0x00, 0x53 });                   // step(ebx, 0, 0)
    a.call(t_step);
    a.put({ 0x83, 0xF8, 0xFF, 0x75, 0x00 }); const size_t jne = a.b.size() - 1;
    a.fetch();
    a.put({ 0xEB, 0x00 }); a.b.back() = (uint8_t)(int8_t)((int)loop - (int)a.b.size());
    a.b[jne] = (uint8_t)(a.b.size() - (jne + 1));
    a.put({ 0x85, 0xC0, 0x75, 0x08, 0x53 });                   // test eax,eax; jnz ok; push ebx
    a.call(t_close);                                           // close(ebx)
    a.put({ 0x31, 0xDB });                                     // xor ebx,ebx
    a.put({ 0x8B, 0xC3, 0x5B });                               // ok: mov eax,ebx; pop ebx
    a.b[jz] = (uint8_t)(a.b.size() - (jz + 1));
    a.put({ 0xC2, 0x0C, 0x00 });                               // out: ret 12
    return a.b;
}

uint32_t put_code(Cpu& c, const std::vector<uint8_t>& code, uint32_t va) {
    if (!va) return 0;
    c.write(va, code.data(), (uint32_t)code.size());
    return va;
}
void jmp5(Cpu& c, uint32_t at, uint32_t to) {
    uint8_t j[5] = { 0xE9, 0, 0, 0, 0 }; const uint32_t rel = to - (at + 5); std::memcpy(j + 1, &rel, 4);
    c.write(at, j, 5);
}

}  // namespace

void mp3_install(Cpu& c, uint32_t base) {
    const char* e = std::getenv("W3_MP3");
    auto at = [base](uint32_t pref) { return base + (pref - kPref); };
    if (const char* d = std::getenv("W3_MP3_DUMP")) g_dump = std::fopen(d, "wb");
    if (e && *e == '0') {
        log("[not-bound] mp3: Mp3dec.asi decodes (W3_MP3=0)");
        if (!g_dump) return;
        // Dump its output: process -> stub that calls the original (first 7
        // bytes moved to a trampoline) and hands the result to dump_pcm.
        Shim s; s.argc = 2; s.tag = "w3mp3!dump";
        s.fn = [](Cpu& c) { dump_pcm(c, c.arg(0), c.arg(1)); return c.arg(1); };
        g_br->register_shim("w3mp3", "dump", s);
        const uint32_t t_dump = g_br->shim_trap("w3mp3", "dump");
        const uint32_t tramp = wx86_scratch_alloc(16), stub = wx86_scratch_alloc(48);
        if (!t_dump || !tramp || !stub) return;
        uint8_t orig[7]; c.read(at(kProcess), orig, 7);
        Asm t; t.org = tramp; t.b.assign(orig, orig + 7);
        t.put({ 0xE9 }); t.u32(at(kProcess) + 7 - (tramp + (uint32_t)t.b.size() + 4));
        put_code(c, t.b, tramp);
        Asm a; a.org = stub;
        a.put({ 0xFF, 0x74, 0x24, 0x0C, 0xFF, 0x74, 0x24, 0x0C, 0xFF, 0x74, 0x24, 0x0C });
        a.call(tramp);
        a.put({ 0x50, 0x50, 0xFF, 0x74, 0x24, 0x10 });   // push eax; push eax (n); push buffer
        a.call(t_dump);
        a.put({ 0x58, 0xC2, 0x0C, 0x00 });               // pop eax; ret 12
        put_code(c, a.b, stub);
        jmp5(c, at(kProcess), stub);
        return;
    }
    // The entries must be the 1.27a Mp3dec.asi ones (their first bytes).
    struct Want { uint32_t va; uint8_t b[4]; } want[] = {
        { kOpen, { 0x53, 0x56, 0x57, 0x68 } }, { kProcess, { 0x51, 0x53, 0x55, 0x8B } },
        { kClose, { 0x56, 0x8B, 0x74, 0x24 } }, { kSeek, { 0x8B, 0x54, 0x24, 0x04 } },
        { kAttrib, { 0x83, 0xEC, 0x48, 0xB8 } }, { kSetPref, { 0x8B, 0x4C, 0x24, 0x08 } } };
    for (const Want& w : want) {
        uint8_t got[4]; c.read(at(w.va), got, 4);
        if (std::memcmp(got, w.b, 4)) { log("[not-bound] [cause: unknown Mp3dec.asi] mp3: entry 0x%08x differs; Mp3dec.asi decodes", w.va); return; }
    }
    auto reg = [](const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
        Shim s; s.fn = std::move(fn); s.argc = argc; s.stdcall_cleanup = true; s.tag = std::string("w3mp3!") + name;
        g_br->register_shim("w3mp3", name, s);
        return g_br->shim_trap("w3mp3", name);
    };
    const uint32_t t_open = reg("open", 3, [](Cpu& c) { return s_open_fn(c, c.arg(0), c.arg(1), c.arg(2)); });
    const uint32_t t_step = reg("step", 3, [](Cpu& c) { return step(c, c.arg(0), c.arg(1), c.arg(2)); });
    const uint32_t t_close = reg("close", 1, [](Cpu& c) { return s_close_fn(c.arg(0)); });
    const uint32_t t_seek = reg("seek", 2, [](Cpu& c) { return s_seek_fn(c.arg(0), (int32_t)c.arg(1)); });
    const uint32_t t_attr = reg("attribute", 2, [](Cpu& c) { return attribute(c.arg(0), c.arg(1)); });
    const uint32_t t_pref = reg("set_preference", 3, [](Cpu& c) { return set_pref(c, c.arg(0), c.arg(1), c.arg(2)); });
    if (!t_open || !t_step || !t_close || !t_seek || !t_attr || !t_pref) { log("[not-bound] [cause: trap slots] mp3: Mp3dec.asi decodes"); return; }
    const uint32_t s_open = wx86_scratch_alloc(96), s_proc = wx86_scratch_alloc(64);
    if (!put_code(c, open_stub(s_open, t_open, t_step, t_close), s_open) || !put_code(c, process_stub(s_proc, t_step), s_proc)) {
        log("[not-bound] [cause: guest scratch full] mp3: Mp3dec.asi decodes"); return;
    }
    jmp5(c, at(kOpen), s_open); jmp5(c, at(kProcess), s_proc);
    jmp5(c, at(kClose), t_close); jmp5(c, at(kSeek), t_seek);
    jmp5(c, at(kAttrib), t_attr); jmp5(c, at(kSetPref), t_pref);
    log("[not-bound] mp3: music decoded natively (minimp3); W3_MP3=0 for Mp3dec.asi's own decoder");
}

// For the 10 s log line.
void mp3_stats(uint64_t* opens, uint64_t* frames) { *opens = g_opens; *frames = g_frames; }

}  // namespace sc
