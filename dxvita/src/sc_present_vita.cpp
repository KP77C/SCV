// sc_present_vita.cpp — frame presentation on the PS Vita (K's decisions):
//
//   * the game's 8-bit 640x480 frame and its palette go to the GPU as a P8
//     texture (palette lookup in the texture unit), drawn once per vblank as
//     one quad at 725x544, pillarboxed on the 960x544 screen;
//   * one-pass sharp scaling: a fragment shader does "sharp bilinear" (crisp
//     source pixels, a one-output-pixel blend only where two source pixels
//     meet), with four point fetches so it never depends on how the GPU
//     filters paletted textures;
//   * every GPU buffer (display buffers, depth, textures, palettes) lives in
//     CDRAM (VRAM), which the CPU only ever writes here;
//   * 60 Hz locked (SC_FPS=30 locks 30): a display thread on its own core
//     owns the GPU; the guest thread never waits on vblank. The guest's
//     present() only records which surface/palette is current, ticks the
//     controller and hands over keyboard text.
//
// Shaders are compiled at start-up by libshacccg (K has it installed).
//
// Each 10 s window logs: display frames and misses (> 16.7 ms, or 33.4 ms
// at 30), guest presents/s, per-thread core load, free RAM/VRAM and their
// low-water mark, and a bound tag with its cause.
#ifdef __vita__
#include "sc_host.h"
#include "sc_input.h"

#include "runtime/ds_emul.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/host_clock.h"
#include "platform/vita_host.h"

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/shacccg.h>

#include <atomic>
#include "runtime/prof.h"
extern "C" {
extern uint64_t dyn86_jp_run_us, dyn86_jp_lookup_miss, dyn86_jp_created, dyn86_jp_recompiles, dyn86_jp_invalid, dyn86_jp_x86_insns, dyn86_sync_us;
}
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sc {

namespace {

constexpr int kScrW = 960, kScrH = 544, kStride = 960;
constexpr int kSrcW = 640, kSrcH = 480;
constexpr int kOutW = 725, kOutH = 544;                // K: 725x544, pillarboxed
constexpr int kBuffers = 3;                            // display buffers
constexpr int kSlots = 3;                              // texture/palette/overlay sets in flight

// ---- shaders --------------------------------------------------------------

const char* kVsTex =
    "void main(float2 aPosition, float2 aTexcoord,\n"
    "          out float4 vPosition : POSITION, out float2 vTexcoord : TEXCOORD0)\n"
    "{ vPosition = float4(aPosition, 0.0, 1.0); vTexcoord = aTexcoord; }\n";

// Sharp bilinear, scale baked in (725/640 across, 544/480 down). Texel
// centres sit at .5; `p` is the sharpened sample position, then a manual
// bilinear of four point fetches (the texture unit does the palette lookup).
const char* kFsTex =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex) : COLOR\n"
    "{\n"
    "  const float2 size = float2(640.0, 480.0);\n"
    "  const float2 scale = float2(725.0 / 640.0, 544.0 / 480.0);\n"
    "  float2 texel = vTexcoord * size;\n"
    "  float2 fl = floor(texel);\n"
    "  float2 cd = texel - fl - 0.5;\n"
    "  float2 rr = 0.5 - 0.5 / scale;\n"
    "  float2 f = (cd - clamp(cd, -rr, rr)) * scale + 0.5;\n"
    "  float2 q = fl + f - 0.5;\n"
    "  float2 i = floor(q);\n"
    "  float2 w = q - i;\n"
    "  float2 t0 = (i + 0.5) / size, t1 = (i + 1.5) / size;\n"
    "  float4 a = tex2D(tex, float2(t0.x, t0.y)), b = tex2D(tex, float2(t1.x, t0.y));\n"
    "  float4 c = tex2D(tex, float2(t0.x, t1.y)), d = tex2D(tex, float2(t1.x, t1.y));\n"
    "  float4 r = lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);\n"
    "  r.a = 1.0;\n"
    "  return r;\n"
    "}\n";

// Command-grid overlay: flat coloured quads, alpha-blended.
const char* kVsCol =
    "void main(float2 aPosition, float4 aColor,\n"
    "          out float4 vPosition : POSITION, out float4 vColor : COLOR0)\n"
    "{ vPosition = float4(aPosition, 0.0, 1.0); vColor = aColor; }\n";
const char* kFsCol =
    "float4 main(float4 vColor : COLOR0) : COLOR { return vColor; }\n";

// ---- GPU memory -------------------------------------------------------------

struct Block { SceUID uid = -1; void* p = nullptr; uint32_t size = 0; };

bool alloc_gpu(Block& b, SceKernelMemBlockType type, uint32_t size, SceGxmMemoryAttribFlags attr, const char* name) {
    const uint32_t gran = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256u * 1024u : 4096u;
    b.size = (size + gran - 1) & ~(gran - 1);
    b.uid = sceKernelAllocMemBlock(name, type, b.size, nullptr);
    if (b.uid < 0) { log("[VMEM-bound] [cause: AllocMemBlock %s %u KiB rc=0x%08x]", name, b.size >> 10, (unsigned)b.uid); return false; }
    sceKernelGetMemBlockBase(b.uid, &b.p);
    int rc = sceGxmMapMemory(b.p, b.size, attr);
    if (rc < 0) { log("[not-bound] [cause: sceGxmMapMemory %s rc=0x%08x]", name, (unsigned)rc); return false; }
    std::memset(b.p, 0, b.size);
    return true;
}
bool alloc_usse(Block& b, uint32_t size, bool fragment, unsigned* off, const char* name) {
    b.size = (size + 4095) & ~4095u;
    b.uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, b.size, nullptr);
    if (b.uid < 0) { log("[RAM-bound] [cause: AllocMemBlock %s rc=0x%08x]", name, (unsigned)b.uid); return false; }
    sceKernelGetMemBlockBase(b.uid, &b.p);
    int rc = fragment ? sceGxmMapFragmentUsseMemory(b.p, b.size, off) : sceGxmMapVertexUsseMemory(b.p, b.size, off);
    if (rc < 0) { log("[not-bound] [cause: Map%sUsseMemory rc=0x%08x]", fragment ? "Fragment" : "Vertex", (unsigned)rc); return false; }
    return true;
}

// ---- shader compiler ----------------------------------------------------------

const char* g_src_text = nullptr;
SceShaccCgSourceFile g_src_file;
SceShaccCgSourceFile* open_cb(const char*, const SceShaccCgSourceLocation*, const SceShaccCgCompileOptions*, const char**) {
    g_src_file.fileName = "<built-in>"; g_src_file.text = g_src_text; g_src_file.size = (SceUInt32)std::strlen(g_src_text);
    return &g_src_file;
}
void* cg_malloc(unsigned int n) { return std::malloc(n); }

bool g_compiler = false;
bool start_compiler() {
    static const char* paths[] = { "ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx" };
    for (const char* p : paths) {
        int st = 0;
        SceUID id = sceKernelLoadStartModule(p, 0, nullptr, 0, nullptr, &st);
        if (id >= 0) {
            sceShaccCgSetDefaultAllocator(cg_malloc, std::free);
            log("[not-bound] shader compiler: %s loaded", p);
            return g_compiler = true;
        }
    }
    log("[not-bound] [cause: libshacccg.suprx not found in ur0:data or ur0:data/external] no shader compiler");
    return false;
}

SceGxmProgram* compile(const char* text, bool fragment) {
    g_src_text = text;
    SceShaccCgCompileOptions o; sceShaccCgInitializeCompileOptions(&o);
    o.mainSourceFile = "<built-in>";
    o.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    o.entryFunctionName = "main";
    o.locale = SCE_SHACCCG_ENGLISH;
    o.optimizationLevel = 3;
    o.useFastmath = 1;
    SceShaccCgCallbackList cb; sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
    cb.openFile = open_cb;
    const SceShaccCgCompileOutput* out = sceShaccCgCompileProgram(&o, &cb, 0);
    if (!out || !out->programData) {
        for (int i = 0; out && i < out->diagnosticCount; ++i)
            log("[not-bound] [cause: shader compile] line %u: %s",
                out->diagnostics[i].location ? out->diagnostics[i].location->lineNumber : 0, out->diagnostics[i].message);
        if (out) sceShaccCgDestroyCompileOutput(out);
        return nullptr;
    }
    SceGxmProgram* p = (SceGxmProgram*)std::malloc(out->programSize);
    std::memcpy(p, out->programData, out->programSize);
    sceShaccCgDestroyCompileOutput(out);
    if (sceGxmProgramCheck(p) < 0) { log("[not-bound] [cause: sceGxmProgramCheck failed]"); return nullptr; }
    return p;
}

// ---- state ------------------------------------------------------------------------

struct DisplayData { void* addr; };

SceGxmContext* g_ctx = nullptr;
SceGxmRenderTarget* g_rt = nullptr;
SceGxmShaderPatcher* g_patcher = nullptr;
Block g_color_mem[kBuffers], g_depth_mem, g_tex_mem, g_vtx_mem, g_rings[3], g_usse_ring, g_pat_buf, g_pat_vusse, g_pat_fusse;
SceGxmColorSurface g_color[kBuffers];
SceGxmSyncObject* g_sync[kBuffers];
SceGxmDepthStencilSurface g_depth;
SceGxmVertexProgram* g_vp_tex = nullptr; SceGxmFragmentProgram* g_fp_tex = nullptr;
SceGxmVertexProgram* g_vp_col = nullptr; SceGxmFragmentProgram* g_fp_col = nullptr;
SceGxmTexture g_tex[kSlots];
uint8_t* g_tex_pix[kSlots]; uint32_t* g_tex_pal[kSlots];
struct ColVtx { float x, y; uint8_t r, g, b, a; };
ColVtx* g_ovl[kSlots];
uint16_t* g_quad_idx; float* g_quad_vtx; uint16_t* g_ovl_idx;
SceGxmNotification g_notif[kSlots];
int g_back = 0, g_front = kBuffers - 1;
int g_interval = 1;                    // vblanks per frame: 1 = 60 Hz, 2 = 30 Hz
bool g_ok = false;

// Guest -> display thread hand-off. The surface is read racily, exactly as a
// real card scans out a DirectDraw primary; the palette is double-buffered by
// sequence number.
std::atomic<uint32_t> g_src_va{0}, g_src_pitch{0}, g_pal_seq{0};
uint32_t g_pal[2][256];
std::atomic<uint64_t> g_guest_presents{0};
std::atomic<bool> g_grid_on{false}; std::atomic<int> g_grid_cell{4};

// On-screen keyboard: requested on the guest thread, run on the display
// thread (common dialogs are drawn into our frames), text handed back.
std::atomic<int> g_ime_state{0};       // 0 idle, 1 requested, 2 open, 3 text ready
SceWChar16 g_ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
std::string g_ime_text;

SceUID g_display_thread = -1, g_main_thread = -1;

// Real per-core load: one lowest-priority spinner per core. It only runs when
// nothing else on that core wants to, so its share of wall time is the core's
// idle time (thread run clocks are not trustworthy for a thread that never
// blocks). Costs nothing but idle power; SC_IDLEPROBE=0 turns it off.
std::atomic<uint64_t> g_idle_us[3];
bool g_probes_on = false;
int idle_main(SceSize, void* argp) {
    const int core = *(int*)argp;
    wx86_vita_pin_self(SCE_KERNEL_CPU_MASK_USER_0 << core, nullptr);
    uint64_t prev = wx86_now_us();
    for (;;) {
        const uint64_t now = wx86_now_us(), d = now - prev; prev = now;
        if (d < 200) g_idle_us[core].fetch_add(d, std::memory_order_relaxed);   // longer gaps = something else ran
    }
    return 0;
}

// What the guest thread is doing, seen by the kernel: sampled every ~1 ms
// from core 2 (otherwise idle). Running / ready (wants the CPU) / waiting,
// and for waits the kernel object it waits on. Answers "core 0 is idle but
// the game is slow": a wait we did not expect.
std::atomic<uint32_t> g_st_run{0}, g_st_ready{0}, g_st_wait{0}, g_st_other{0};
struct WaitSlot { std::atomic<uint32_t> type{0}; std::atomic<int> uid{0}; std::atomic<uint32_t> n{0}; };
WaitSlot g_waits[6];
std::atomic<int> g_quit{0};
std::atomic<bool> g_wipe_bars{false};
int sampler_main(SceSize, void*) {
    wx86_vita_pin_self(SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    while (!g_quit.load()) {
        sceKernelDelayThread(1000);
        if (g_main_thread < 0) continue;
        SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        if (sceKernelGetThreadInfo(g_main_thread, &ti) < 0) continue;
        if (ti.status & SCE_THREAD_RUNNING) g_st_run++;
        else if (ti.status & SCE_THREAD_READY) g_st_ready++;
        else if (ti.status & SCE_THREAD_WAITING) {
            g_st_wait++;
            int i = 0;
            for (; i < 6; ++i) if (g_waits[i].n.load() && g_waits[i].uid.load() == (int)ti.waitId && g_waits[i].type.load() == ti.waitType) break;
            if (i == 6) for (i = 0; i < 6; ++i) if (!g_waits[i].n.load()) { g_waits[i].uid = (int)ti.waitId; g_waits[i].type = ti.waitType; break; }
            if (i < 6) g_waits[i].n++;
        } else g_st_other++;
    }
    return 0;
}
// Best-effort name of the kernel object a thread waits on.
void wait_name(int uid, char* out, unsigned n) {
    out[0] = 0;
    { SceKernelSemaInfo i; std::memset(&i, 0, sizeof i); i.size = sizeof i; if (sceKernelGetSemaInfo(uid, &i) >= 0) { std::snprintf(out, n, "sema:%s", i.name); return; } }
    { SceKernelMutexInfo i; std::memset(&i, 0, sizeof i); i.size = sizeof i; if (sceKernelGetMutexInfo(uid, &i) >= 0) { std::snprintf(out, n, "mutex:%s", i.name); return; } }
    { SceKernelEventFlagInfo i; std::memset(&i, 0, sizeof i); i.size = sizeof i; if (sceKernelGetEventFlagInfo(uid, &i) >= 0) { std::snprintf(out, n, "evf:%s", i.name); return; } }
    if (uid == g_display_thread) { std::snprintf(out, n, "thread:sc_display"); return; }
    std::snprintf(out, n, "?");
}

// Where the guest is: its EIP sampled once per displayed frame.
const volatile uint32_t* g_ip = nullptr;
uint32_t g_samples[6];   // exe, storm, other DLLs, inside a shim (host code), generated/heap code, other
uint32_t g_storm_lo = 0, g_storm_hi = 0;

void display_cb(const void* data) {
    const DisplayData* d = (const DisplayData*)data;
    SceDisplayFrameBuf fb; std::memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb; fb.base = d->addr; fb.pitch = kStride;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; fb.width = kScrW; fb.height = kScrH;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStartMulti(g_interval);
}
void* patcher_alloc(void*, unsigned int n) { return std::malloc(n); }
void patcher_free(void*, void* p) { std::free(p); }

bool make_program(SceGxmShaderPatcher* sp, const char* vs, const char* fs, bool colour,
                  SceGxmVertexProgram** vp, SceGxmFragmentProgram** fp) {
    SceGxmProgram* v = compile(vs, false); SceGxmProgram* f = compile(fs, true);
    if (!v || !f) return false;
    SceGxmShaderPatcherId vid, fid;
    if (sceGxmShaderPatcherRegisterProgram(sp, v, &vid) < 0 || sceGxmShaderPatcherRegisterProgram(sp, f, &fid) < 0) return false;
    const SceGxmProgramParameter* pos = sceGxmProgramFindParameterByName(v, "aPosition");
    const SceGxmProgramParameter* two = sceGxmProgramFindParameterByName(v, colour ? "aColor" : "aTexcoord");
    if (!pos || !two) { log("[not-bound] [cause: shader attribute lookup]"); return false; }
    SceGxmVertexAttribute at[2];
    at[0].streamIndex = 0; at[0].offset = 0; at[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32; at[0].componentCount = 2;
    at[0].regIndex = sceGxmProgramParameterGetResourceIndex(pos);
    at[1].streamIndex = 0; at[1].offset = 8;
    at[1].format = colour ? SCE_GXM_ATTRIBUTE_FORMAT_U8N : SCE_GXM_ATTRIBUTE_FORMAT_F32;
    at[1].componentCount = colour ? 4 : 2;
    at[1].regIndex = sceGxmProgramParameterGetResourceIndex(two);
    SceGxmVertexStream st; st.stride = colour ? sizeof(ColVtx) : 16; st.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(sp, vid, at, 2, &st, 1, vp) < 0) return false;
    SceGxmBlendInfo blend; std::memset(&blend, 0, sizeof blend);
    blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
    blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD; blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE; blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    return sceGxmShaderPatcherCreateFragmentProgram(sp, fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                                                    colour ? &blend : nullptr, v, fp) >= 0;
}

bool gpu_init() {
    if (!start_compiler()) return false;
    SceGxmInitializeParams ip; std::memset(&ip, 0, sizeof ip);
    ip.flags = 0; ip.displayQueueMaxPendingCount = kBuffers - 1;
    ip.displayQueueCallback = display_cb; ip.displayQueueCallbackDataSize = sizeof(DisplayData);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int rc = sceGxmInitialize(&ip);
    if (rc < 0) { log("[not-bound] [cause: sceGxmInitialize rc=0x%08x]", (unsigned)rc); return false; }

    const auto RO = SCE_GXM_MEMORY_ATTRIB_READ;
    const auto RW = (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    const auto LP = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, CD = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    unsigned usse_off = 0;
    if (!alloc_gpu(g_rings[0], LP, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, RO, "sc_vdm") ||
        !alloc_gpu(g_rings[1], LP, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, RO, "sc_vring") ||
        !alloc_gpu(g_rings[2], LP, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, RO, "sc_fring") ||
        !alloc_usse(g_usse_ring, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, true, &usse_off, "sc_fusse_ring")) return false;
    SceGxmContextParams cp; std::memset(&cp, 0, sizeof cp);
    cp.hostMem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE); cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = g_rings[0].p; cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = g_rings[1].p; cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = g_rings[2].p; cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = g_usse_ring.p; cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = usse_off;
    if ((rc = sceGxmCreateContext(&cp, &g_ctx)) < 0) { log("[not-bound] [cause: sceGxmCreateContext rc=0x%08x]", (unsigned)rc); return false; }

    SceGxmRenderTargetParams rp; std::memset(&rp, 0, sizeof rp);
    rp.width = kScrW; rp.height = kScrH; rp.scenesPerFrame = 1; rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rp.driverMemBlock = -1;
    if ((rc = sceGxmCreateRenderTarget(&rp, &g_rt)) < 0) { log("[not-bound] [cause: sceGxmCreateRenderTarget rc=0x%08x]", (unsigned)rc); return false; }

    for (int i = 0; i < kBuffers; ++i) {
        if (!alloc_gpu(g_color_mem[i], CD, 4 * kStride * kScrH, RW, "sc_display")) return false;
        sceGxmColorSurfaceInit(&g_color[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                               SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, kScrW, kScrH, kStride, g_color_mem[i].p);
        sceGxmSyncObjectCreate(&g_sync[i]);
    }
    const uint32_t dw = (kScrW + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const uint32_t dh = (kScrH + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    if (!alloc_gpu(g_depth_mem, CD, 4 * dw * dh, RW, "sc_depth")) return false;
    sceGxmDepthStencilSurfaceInit(&g_depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, dw, g_depth_mem.p, nullptr);

    unsigned voff = 0, foff = 0;
    if (!alloc_gpu(g_pat_buf, LP, 64 * 1024, RO, "sc_patcher") ||
        !alloc_usse(g_pat_vusse, 64 * 1024, false, &voff, "sc_patcher_v") ||
        !alloc_usse(g_pat_fusse, 64 * 1024, true, &foff, "sc_patcher_f")) return false;
    SceGxmShaderPatcherParams pp; std::memset(&pp, 0, sizeof pp);
    pp.hostAllocCallback = patcher_alloc; pp.hostFreeCallback = patcher_free;
    pp.bufferMem = g_pat_buf.p; pp.bufferMemSize = g_pat_buf.size;
    pp.vertexUsseMem = g_pat_vusse.p; pp.vertexUsseMemSize = g_pat_vusse.size; pp.vertexUsseOffset = voff;
    pp.fragmentUsseMem = g_pat_fusse.p; pp.fragmentUsseMemSize = g_pat_fusse.size; pp.fragmentUsseOffset = foff;
    if ((rc = sceGxmShaderPatcherCreate(&pp, &g_patcher)) < 0) { log("[not-bound] [cause: sceGxmShaderPatcherCreate rc=0x%08x]", (unsigned)rc); return false; }
    if (!make_program(g_patcher, kVsTex, kFsTex, false, &g_vp_tex, &g_fp_tex)) { log("[not-bound] [cause: frame shader]"); return false; }
    if (!make_program(g_patcher, kVsCol, kFsCol, true, &g_vp_col, &g_fp_col)) { log("[not-bound] [cause: overlay shader]"); return false; }

    // Textures + palettes (CDRAM, CPU-write only), overlay vertices and the
    // static quad (uncached LPDDR, CPU-write only).
    const uint32_t pix = kSrcW * kSrcH, pal = 256 * 4;
    if (!alloc_gpu(g_tex_mem, CD, kSlots * (pix + pal), RO, "sc_frames")) return false;
    const uint32_t ovl_bytes = 9 * 4 * sizeof(ColVtx);
    if (!alloc_gpu(g_vtx_mem, LP, 4096 + kSlots * ovl_bytes, RO, "sc_vertices")) return false;
    uint8_t* tp = (uint8_t*)g_tex_mem.p;
    for (int i = 0; i < kSlots; ++i) {
        g_tex_pal[i] = (uint32_t*)(tp + i * pal);                         // 64-byte aligned
        g_tex_pix[i] = tp + kSlots * pal + i * pix;                       // 16-byte aligned
        sceGxmTextureInitLinear(&g_tex[i], g_tex_pix[i], SCE_GXM_TEXTURE_FORMAT_P8_ABGR, kSrcW, kSrcH, 0);
        sceGxmTextureSetPalette(&g_tex[i], g_tex_pal[i]);
        sceGxmTextureSetMinFilter(&g_tex[i], SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&g_tex[i], SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&g_tex[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&g_tex[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
    }
    uint8_t* vp = (uint8_t*)g_vtx_mem.p;
    g_quad_vtx = (float*)vp; g_quad_idx = (uint16_t*)(vp + 64); g_ovl_idx = (uint16_t*)(vp + 128);
    const float x1 = (float)kOutW / kScrW;                                // pillarbox in clip space
    const float q[16] = { -x1, 1, 0, 0,   x1, 1, 1, 0,   -x1, -1, 0, 1,   x1, -1, 1, 1 };
    std::memcpy(g_quad_vtx, q, sizeof q);
    const uint16_t qi[4] = { 0, 1, 2, 3 }; std::memcpy(g_quad_idx, qi, sizeof qi);
    for (int c = 0; c < 9; ++c) {
        const uint16_t b = (uint16_t)(c * 4), t[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2), (uint16_t)(b + 1), (uint16_t)(b + 3), (uint16_t)(b + 2) };
        std::memcpy(g_ovl_idx + c * 6, t, sizeof t);
    }
    for (int i = 0; i < kSlots; ++i) g_ovl[i] = (ColVtx*)(vp + 4096 + i * ovl_bytes);

    volatile unsigned int* nreg = sceGxmGetNotificationRegion();
    for (int i = 0; i < kSlots; ++i) { g_notif[i].address = nreg + i; g_notif[i].value = 0; *g_notif[i].address = 0; }
    return true;
}

// Command-card overlay in clip space. Card centres in game pixels match
// sc_input's grid (x = 522 + 46c, y = 376 + 39r), cells about 40x36.
void build_overlay(ColVtx* v, int sel) {
    const float sx = (float)kOutW / kSrcW, ox = (kScrW - kOutW) * 0.5f, sy = (float)kOutH / kSrcH;
    for (int c = 0; c < 9; ++c) {
        const float cx = 522.0f + 46.0f * (c % 3), cy = 376.0f + 39.0f * (c / 3);
        const float l = ox + (cx - 20) * sx, r = ox + (cx + 20) * sx, t = (cy - 18) * sy, b = (cy + 18) * sy;
        const float L = l / kScrW * 2 - 1, R = r / kScrW * 2 - 1, T = 1 - t / kScrH * 2, B = 1 - b / kScrH * 2;
        const uint8_t a = c == sel ? 110 : 40, g = c == sel ? 230 : 255, bl = c == sel ? 60 : 255;
        ColVtx q[4] = { { L, T, 255, g, bl, a }, { R, T, 255, g, bl, a }, { L, B, 255, g, bl, a }, { R, B, 255, g, bl, a } };
        std::memcpy(v + c * 4, q, sizeof q);
    }
}

// ---- display thread ----------------------------------------------------------------

struct Window {
    uint64_t start = 0, frames = 0, misses = 0, worst_us = 0, gpu_us = 0, last_presents = 0;
    uint64_t clk_main = 0, clk_disp = 0;
    int min_user_kb = 1 << 30, min_cdram_kb = 1 << 30;
} g_win;

uint64_t thread_clocks(SceUID uid) {
    SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
    return sceKernelGetThreadInfo(uid, &ti) == 0 ? ti.runClocks : 0;
}

void window_report(uint64_t now) {
    SceKernelFreeMemorySizeInfo fm; std::memset(&fm, 0, sizeof fm); fm.size = sizeof fm;
    sceKernelGetFreeMemorySize(&fm);
    const int ukb = fm.size_user >> 10, ckb = fm.size_cdram >> 10;
    if (ukb < g_win.min_user_kb) g_win.min_user_kb = ukb;
    if (ckb < g_win.min_cdram_kb) g_win.min_cdram_kb = ckb;
    if (!g_win.start) { g_win.start = now; g_win.clk_main = thread_clocks(g_main_thread); g_win.clk_disp = thread_clocks(g_display_thread); return; }
    const uint64_t span = now - g_win.start;
    if (span < 10000000) return;
    const uint64_t cm = thread_clocks(g_main_thread), cd = thread_clocks(g_display_thread);
    const double load_main = 100.0 * (cm - g_win.clk_main) / span, load_disp = 100.0 * (cd - g_win.clk_disp) / span;
    const uint64_t presents = g_guest_presents.load();
    const double gps = (presents - g_win.last_presents) * 1e6 / span;
    // Stall report: the game drew nothing for a whole window. Dump every guest
    // thread (what it waits on, who called the wait) so a deadlock can be
    // traced from the log alone (K, build 9: froze mid-game, music kept going).
    {
        static int stalled = 0, dumps = 0;
        if (presents == g_win.last_presents && presents > 0) {
            if (++stalled == 1 && dumps < 6) {
                ++dumps;
                log("[not-bound] [cause: guest stall] no frame for 10 s; guest threads:");
                static bool main_seen; main_seen = false;
                if (d2rt::ThreadScheduler* sc = wx86_sched()) sc->dump_threads_to([](const char* m) {
                    if (!std::strncmp(m, "thr 1 ", 6)) main_seen = true;
                    log("[not-bound]   %s", m); });
                if (!main_seen) {
                    // The game's main thread is gone (a guest fault, details in
                    // scvita_stdout.txt): nothing will ever draw again, so leave
                    // cleanly instead of hanging with the music playing.
                    log("[not-bound] [cause: main game thread ended] see scvita_stdout.txt; closing the game");
                    present_shutdown();
                    sceKernelExitProcess(0);
                }
            }
        } else stalled = 0;
    }
    const double miss_pct = g_win.frames ? 100.0 * g_win.misses / g_win.frames : 0;
    const double gpu_ms = g_win.frames ? g_win.gpu_us / 1000.0 / g_win.frames : 0;
    const char* tag = "[not-bound]"; char cause[96] = "no misses";
    if (miss_pct > 1.0) {
        if (load_main > 95.0) { tag = "[CPU-bound]"; std::snprintf(cause, sizeof cause, "guest x86 thread at %.0f%% of core 0", load_main); }
        else if (load_disp > 80.0) { tag = "[CPU-bound]"; std::snprintf(cause, sizeof cause, "display thread at %.0f%%", load_disp); }
        else { std::snprintf(cause, sizeof cause, "misses without a saturated core or GPU; unexplained"); }
    }
    log("%s [cause: %s] display %.1f fps (%s lock), %.2f%% missed (worst %.1f ms), submit+wait %.2f ms/frame, guest %.1f presents/s; "
        "core0 guest %.0f%%, core1 display %.0f%%; RAM free %d KiB (low %d), VRAM free %d KiB (low %d)",
        tag, cause, g_win.frames * 1e6 / span, g_interval == 1 ? "60" : "30", miss_pct, g_win.worst_us / 1000.0, gpu_ms, gps,
        load_main, load_disp, ukb, g_win.min_user_kb, ckb, g_win.min_cdram_kb);
    char line[600];
    {
        double busy[3];
        for (int i = 0; i < 3; ++i) { const uint64_t v = g_idle_us[i].exchange(0); busy[i] = 100.0 - 100.0 * v / span; if (busy[i] < 0) busy[i] = 0; }
        uint32_t tot = 0; for (uint32_t v : g_samples) tot += v; if (!tot) tot = 1;
        if (!g_probes_on) busy[0] = busy[1] = busy[2] = -1;   // probes off: no core measurement
        log("[not-bound] cores busy (idle-probe): core0 %.0f%%, core1 %.0f%%, core2 %.0f%%; guest EIP: exe %u%%, storm %u%%, other dll %u%%, in shims %u%%, generated %u%%, other %u%%",
            busy[0], busy[1], busy[2], g_samples[0] * 100 / tot, g_samples[1] * 100 / tot, g_samples[2] * 100 / tot,
            g_samples[3] * 100 / tot, g_samples[4] * 100 / tot, g_samples[5] * 100 / tot);
        std::memset(g_samples, 0, sizeof g_samples);
        if (g_br && g_br->prof_window(line, sizeof line, 8) > 0) log("[not-bound] shim time (10 s):%s", line);
    }
    {
        const uint32_t r = g_st_run.exchange(0), rd = g_st_ready.exchange(0), w = g_st_wait.exchange(0), o = g_st_other.exchange(0);
        const uint32_t t = (r + rd + w + o) ? (r + rd + w + o) : 1;
        int k = std::snprintf(line, sizeof line, "guest thread (kernel, %u samples): running %u%%, ready %u%%, waiting %u%%, other %u%%;",
                              t, r * 100 / t, rd * 100 / t, w * 100 / t, o * 100 / t);
        for (WaitSlot& ws : g_waits) {
            const uint32_t n = ws.n.exchange(0); if (!n || k >= (int)sizeof line - 80) continue;
            char nm[48]; wait_name(ws.uid.load(), nm, sizeof nm);
            k += std::snprintf(line + k, sizeof line - k, " wait type=0x%x uid=0x%x (%s) %u%%", (unsigned)ws.type.load(), (unsigned)ws.uid.load(), nm, n * 100 / t);
        }
        log("[not-bound] %s", line);
    }
#ifdef PROF_COUNTERS
    {
        static uint64_t p_run = 0, p_trap = 0, p_jrun = 0, p_miss = 0, p_new = 0, p_rec = 0, p_inv = 0, p_ins = 0, p_sync = 0;
        const uint64_t run = d2rt::prof::run_ns / 1000, trap = d2rt::prof::trap_ns / 1000;
        log("[not-bound] guest time (10 s): in cpu->run %llu ms (of which shims %llu ms), dynarec run %llu ms; JIT: %llu misses, %llu new blocks, %llu recompiles, %llu invalidated, %llu x86 insns translated, cache sync %llu ms",
            (unsigned long long)((run - p_run) / 1000), (unsigned long long)((trap - p_trap) / 1000),
            (unsigned long long)((dyn86_jp_run_us - p_jrun) / 1000), (unsigned long long)(dyn86_jp_lookup_miss - p_miss),
            (unsigned long long)(dyn86_jp_created - p_new), (unsigned long long)(dyn86_jp_recompiles - p_rec),
            (unsigned long long)(dyn86_jp_invalid - p_inv), (unsigned long long)(dyn86_jp_x86_insns - p_ins),
            (unsigned long long)((dyn86_sync_us - p_sync) / 1000));
        p_run = run; p_trap = trap; p_jrun = dyn86_jp_run_us; p_miss = dyn86_jp_lookup_miss; p_new = dyn86_jp_created;
        p_rec = dyn86_jp_recompiles; p_inv = dyn86_jp_invalid; p_ins = dyn86_jp_x86_insns; p_sync = dyn86_sync_us;
    }
#endif
    io_window_line(line, sizeof line, span / 1e6); log("[not-bound] %s", line);
    if (d2rt::dsound::stat_line(line, sizeof line) > 0) log("[not-bound] %s", line);
    wx86_vita_core_window_line();
    g_win.start = now; g_win.frames = g_win.misses = g_win.worst_us = g_win.gpu_us = 0; g_win.last_presents = presents;
    g_win.clk_main = cm; g_win.clk_disp = cd;
}

void ime_step() {
    int st = g_ime_state.load();
    if (st == 1) {
        static const char16_t title16[] = u"StarCraft";
        const SceWChar16* title = (const SceWChar16*)title16;
        SceImeDialogParam p; sceImeDialogParamInit(&p);
        p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH; p.languagesForced = SCE_FALSE;
        p.type = SCE_IME_TYPE_DEFAULT; p.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
        p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
        p.title = title; p.maxTextLength = 80;
        static SceWChar16 empty[1] = { 0 };
        p.initialText = empty; std::memset(g_ime_buf, 0, sizeof g_ime_buf); p.inputTextBuffer = g_ime_buf;
        int rc = sceImeDialogInit(&p);
        if (rc < 0) { log("[not-bound] [cause: sceImeDialogInit rc=0x%08x]", (unsigned)rc); g_ime_state = 0; }
        else g_ime_state = 2;
    } else if (st == 2 && sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult r; std::memset(&r, 0, sizeof r); sceImeDialogGetResult(&r);
        std::string s;
        if (r.button == SCE_IME_DIALOG_BUTTON_ENTER)
            for (int i = 0; g_ime_buf[i]; ++i) if (g_ime_buf[i] >= 0x20 && g_ime_buf[i] < 0x7F) s += (char)g_ime_buf[i];
        sceImeDialogTerm();
        g_wipe_bars.store(true);
        g_ime_text = s;
        g_ime_state = 3;
    }
}

int display_main(SceSize, void*) {
    wx86_vita_pin_self(SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    uint64_t seq = 0, last = 0;
    int slot = 0;
    while (!g_quit.load()) {
        ime_step();
        const uint32_t va = g_src_va.load(), pitch = g_src_pitch.load();
        const uint8_t* src = va ? (const uint8_t*)g_cpu->hostptr(va, pitch * kSrcH) : nullptr;
        if (!src) { sceKernelDelayThread(4000); continue; }
        // The keyboard dialog draws over the whole screen, and we only ever
        // redraw the 725-wide game area: once it closes, wipe every buffer so
        // the side bars go back to black (K saw them white, build 6).
        if (g_wipe_bars.exchange(false)) {
            sceGxmDisplayQueueFinish(); sceGxmFinish(g_ctx);
            for (int i = 0; i < kBuffers; ++i) std::memset(g_color_mem[i].p, 0, 4 * kStride * kScrH);
        }

        // Reuse a texture set only once the GPU has finished the scene that read it.
        sceGxmNotificationWait(&g_notif[slot]);
        if (pitch == kSrcW) std::memcpy(g_tex_pix[slot], src, kSrcW * kSrcH);
        else for (int y = 0; y < kSrcH; ++y) std::memcpy(g_tex_pix[slot] + y * kSrcW, src + y * pitch, kSrcW);
        const uint32_t* pal = g_pal[g_pal_seq.load() & 1];
        for (int i = 0; i < 256; ++i) g_tex_pal[slot][i] = pal[i] | 0xFF000000u;   // 0x00BBGGRR -> ABGR, opaque
        const bool grid = g_grid_on.load();
        if (grid) build_overlay(g_ovl[slot], g_grid_cell.load());

        const uint64_t t0 = wx86_now_us();
        sceGxmBeginScene(g_ctx, 0, g_rt, nullptr, nullptr, g_sync[g_back], &g_color[g_back], &g_depth);
        sceGxmSetFrontDepthFunc(g_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(g_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
        sceGxmSetVertexProgram(g_ctx, g_vp_tex); sceGxmSetFragmentProgram(g_ctx, g_fp_tex);
        sceGxmSetFragmentTexture(g_ctx, 0, &g_tex[slot]);
        sceGxmSetVertexStream(g_ctx, 0, g_quad_vtx);
        sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, g_quad_idx, 4);
        if (grid) {
            sceGxmSetVertexProgram(g_ctx, g_vp_col); sceGxmSetFragmentProgram(g_ctx, g_fp_col);
            sceGxmSetVertexStream(g_ctx, 0, g_ovl[slot]);
            sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, g_ovl_idx, 54);
        }
        g_notif[slot].value = (unsigned)++seq;
        sceGxmEndScene(g_ctx, nullptr, &g_notif[slot]);
        if (g_ime_state.load() == 2) {
            SceCommonDialogUpdateParam up; std::memset(&up, 0, sizeof up);
            up.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8; up.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
            up.renderTarget.width = kScrW; up.renderTarget.height = kScrH; up.renderTarget.strideInPixels = kStride;
            up.renderTarget.colorSurfaceData = g_color_mem[g_back].p; up.renderTarget.depthSurfaceData = g_depth_mem.p;
            up.displaySyncObject = g_sync[g_back];
            sceCommonDialogUpdate(&up);
        }
        DisplayData dd = { g_color_mem[g_back].p };
        sceGxmDisplayQueueAddEntry(g_sync[g_front], g_sync[g_back], &dd);
        g_front = g_back; g_back = (g_back + 1) % kBuffers;

        // Frame timing: the interval between submissions is the frame time
        // the player sees (AddEntry blocks once the queue is full).
        const uint64_t now = wx86_now_us();
        if (last) {
            const uint64_t dt = now - last, budget = 16667u * g_interval + 1500u;   // 1.5 ms vblank jitter
            ++g_win.frames; if (dt > budget) ++g_win.misses; if (dt > g_win.worst_us) g_win.worst_us = dt;
        }
        g_win.gpu_us += now - t0;
        if (g_ip) {
            const uint32_t ip = *g_ip;
            const int k = (ip >= 0x00400000u && ip < 0x006EC000u) ? 0 : (ip >= g_storm_lo && ip < g_storm_hi) ? 1
                        : (ip >= 0x01800000u && ip < 0x01C00000u) ? 2 : (ip >= 0x09800000u && ip < 0x09900000u) ? 3
                        : (ip >= 0x00800000u && ip < 0x08400000u) ? 4 : 5;
            ++g_samples[k];
        }
        last = now;
        slot = (slot + 1) % kSlots;
        window_report(now);
    }
    return 0;
}

}  // namespace

// Called once from boot before the guest starts.
bool present_init() {
    const char* f = std::getenv("SC_FPS");
    g_interval = (f && std::atoi(f) == 30) ? 2 : 1;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    SceAppUtilInitParam ai; SceAppUtilBootParam bi;
    std::memset(&ai, 0, sizeof ai); std::memset(&bi, 0, sizeof bi);
    sceAppUtilInit(&ai, &bi);
    SceCommonDialogConfigParam cfg; sceCommonDialogConfigParamInit(&cfg); sceCommonDialogSetConfigParam(&cfg);
    g_ok = gpu_init();
    if (!g_ok) return false;
    g_main_thread = sceKernelGetThreadId();
    g_display_thread = sceKernelCreateThread("sc_display", display_main, 0x40, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (g_display_thread < 0) { log("[not-bound] [cause: display thread rc=0x%08x]", (unsigned)g_display_thread); return false; }
    wx86_vita_core_register("display", g_display_thread, SCE_KERNEL_CPU_MASK_USER_1, 0);
    sceKernelStartThread(g_display_thread, 0, nullptr);
    const char* ip = std::getenv("SC_IDLEPROBE");
    if (ip && std::atoi(ip) != 0) {   // off by default: the spinners show as 100% load and add heat
        g_probes_on = true;
        static int cores[3] = { 0, 1, 2 };
        for (int i = 0; i < 3; ++i) {
            SceUID t = sceKernelCreateThread("sc_idle", idle_main, 191, 0x2000, 0, SCE_KERNEL_CPU_MASK_USER_0 << i, nullptr);
            if (t >= 0) sceKernelStartThread(t, sizeof(int), &cores[i]);
        }
    }
    { SceUID t = sceKernelCreateThread("sc_sampler", sampler_main, 0x60, 0x2000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
      if (t >= 0) sceKernelStartThread(t, 0, nullptr); }
    log("[not-bound] presenter: %dx%d -> %dx%d pillarboxed, %s Hz lock, %d display buffers in VRAM",
        kSrcW, kSrcH, kOutW, kOutH, g_interval == 1 ? "60" : "30", kBuffers);
    return true;
}

// Clean exit (ExitProcess/TerminateProcess). Leaving with the display thread
// inside GXM and the audio thread inside sceAudioOut froze the whole console
// (K, build 4). Stop both, let the GPU drain, then end the process properly.
void present_shutdown() {
    g_quit.store(1);
    if (g_display_thread >= 0) { SceUInt to = 500000; sceKernelWaitThreadEnd(g_display_thread, nullptr, &to); }
    sceGxmDisplayQueueFinish();
    sceGxmFinish(g_ctx);
    d2rt::dsound::shutdown();
    log_flush();
}

// Guest thread, once per DirectDraw present.
void present(d2rt::Cpu& c, uint32_t pixels, uint32_t pitch, uint32_t w, uint32_t h, const uint32_t* pal) {
    static uint64_t last_us = 0;
    const uint64_t now = wx86_now_us();
    g_guest_presents.fetch_add(1);
    if (!g_ip) {
        g_ip = c.ip_ptr();
        if (d2rt::PeImage* st = g_br ? g_br->module("storm.dll") : nullptr) { g_storm_lo = st->load_base(); g_storm_hi = g_storm_lo + st->image_size(); }
    }
    if (w == (uint32_t)kSrcW && h == (uint32_t)kSrcH) {
        const uint32_t next = g_pal_seq.load() + 1;
        std::memcpy(g_pal[next & 1], pal, sizeof g_pal[0]);
        g_pal_seq.store(next);
        g_src_pitch.store(pitch); g_src_va.store(pixels);
    }
    d2rt::dsound::frame_pump(c);

    SceCtrlData cd; std::memset(&cd, 0, sizeof cd);
    sceCtrlPeekBufferPositive(0, &cd, 1);
    Pad p; p.buttons = cd.buttons; p.lx = cd.lx; p.ly = cd.ly; p.rx = cd.rx; p.ry = cd.ry;
    {   // Front panel reports 1920x1088 for the 960x544 screen; the game
        // area is the centred 725x544 quad, i.e. 640x480 scaled.
        SceTouchData td; std::memset(&td, 0, sizeof td);
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) > 0 && td.reportNum > 0) {
            const double sx = td.report[0].x / 2.0 - (960 - kOutW) / 2.0;
            const double sy = td.report[0].y / 2.0;
            if (sx >= 0 && sx < kOutW) {
                p.touch = true;
                p.tx = (int)(sx * 640.0 / kOutW);
                p.ty = (int)(sy * 480.0 / kOutH);
                if (p.ty > 479) p.ty = 479; if (p.ty < 0) p.ty = 0;
            }
        }
    }
    if (g_ime_state.load() == 2) p = Pad();            // the keyboard owns the pad while it is open
    uint64_t dt = last_us ? now - last_us : 16667; if (dt > 100000) dt = 100000;
    input_tick(p, dt);
    last_us = now;
    int cell = 4; g_grid_on.store(input_grid_active(&cell)); g_grid_cell.store(cell);
    if (input_take_ime_request() && g_ime_state.load() == 0) g_ime_state.store(1);
    if (g_ime_state.load() == 3) { input_text(g_ime_text); g_ime_state.store(0); }
}

}  // namespace sc
#endif  // __vita__
