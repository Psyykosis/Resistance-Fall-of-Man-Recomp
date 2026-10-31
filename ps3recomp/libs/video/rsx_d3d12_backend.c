/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include <time.h>
#include "ps3emu/log_first.h"
#include "rsx_d3d12_backend.h"
#include "rsx_primitives.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
/* Resistance port: getenv() is called on hot paths (per draw / per method);
 * the UCRT version takes a lock and scans the environment every time, which
 * profiled at ~20% of the render thread. Cached by call-site string pointer. */
char* rsx_getenv(const char* name);
#define getenv(n) rsx_getenv(n)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* We need these GUIDs — define them here to avoid uuid.lib dependency.
 * initguid.h only turns DEFINE_GUID into a definition for headers included
 * after it, so it has to come first. */
#include <initguid.h>

/* D3D12 headers */
#include <d3d12.h>
#include <d3d12sdklayers.h>   /* ID3D12Debug / ID3D12InfoQueue */
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <intrin.h>   /* __rdtsc: PERF2 replay timers */
#include "ps3emu/ps3_ftrace.h"
/* Resistance port: frame-trace spans for the render side's one-off costs, so a
 * hitch can be pinned on them: kind 17 shader (a = 1 cache hit / 0 FXC, b = HLSL
 * bytes), 18 PSO creation (a = fp addr), 19 texture upload (a = guest offset,
 * b = w << 16 | h). who = 64. Spans under 50 us are dropped. */
static inline uint64_t rft0(void) { return PS3_FT_ON() ? ps3_ft_now() : 0; }
static inline void rft(int kind, uint32_t a, uint32_t b, uint64_t t0)
{
    if (!t0) return;
    const uint64_t t1 = ps3_ft_now();
    if (t1 - t0 >= 50000u) ps3_ft_span(kind, 64u, a, b, t0, t1);
}

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Resistance port: on-disk shader cache.
 *
 * Every guest vertex/fragment program is decompiled to HLSL and run through
 * D3DCompile (FXC) the first time it is seen -- tens of milliseconds for the
 * big ones (the particle VS is 46 KB of HLSL), each on the FIFO or render
 * thread, so a new area (walking out of York's crater) stalled for seconds.
 * The bytecode is now stored under shadercache\ keyed by a 64-bit hash of
 * (target, entry, HLSL text): FXC runs once per program ever. The driver keeps
 * its own cache of the pipelines built from it. RSX_SHADERCACHE=0 disables.
 * -------------------------------------------------------------------------*/
static volatile LONG s_shc_hits, s_shc_miss, s_pso_new;
static double s_pso_ms;                       /* time inside CreateGraphicsPipelineState (guest FP PSOs) */
/* PERF2: why each batch was submitted, and where the replay loop's cycles go. */
enum { RFR_PRESENT, RFR_CLEAR, RFR_MIDV, RFR_MIDD, RFR_FLIP, RFR_N };
static volatile LONG s_rfr[RFR_N];
static u64 s_rp_cyc[6];                       /* rt switch, pso, state, draw */
static double s_shc_ms;                       /* time inside D3DCompile */
/* v126: render_frame's share of the last PERF window (percent), -1 = PERF off.
 * The debug console's auto-profile fires when the render thread is pinned. */
static volatile int s_perf_load_pct = -1;
static u64 shc_hash(const char* a, size_t n, u64 h)
{
    for (size_t i = 0; i < n; i++) { h ^= (u8)a[i]; h *= 1099511628211ull; }
    return h;
}
/* v128: which shadercache\ key a compiled shader came from, by the DXBC
 * header's own content checksum -- the pipeline manifest (see PsoRec) names
 * its shaders by cache key, and a blob does not carry one. */
#define SHK_N 16384u
static u64 s_shk_sum[SHK_N], s_shk_key[SHK_N];
static SRWLOCK s_shk_lock = SRWLOCK_INIT;
static u64 dxbc_sum64(const void* p, SIZE_T n)
{
    const u8* b = (const u8*)p;
    if (!b || n < 20 || memcmp(b, "DXBC", 4) != 0) return 0;
    u64 x, y; memcpy(&x, b + 4, 8); memcpy(&y, b + 12, 8);
    x ^= y * 0x9E3779B97F4A7C15ull;
    return x ? x : 1;
}
static u32 shk_home(u64 s) { return (u32)((s * 0x9E3779B97F4A7C15ull) >> 50) & (SHK_N - 1u); }
static void shk_put(ID3DBlob* blob, u64 key)
{
    if (!blob || !key) return;
    const u64 s = dxbc_sum64(blob->lpVtbl->GetBufferPointer(blob), blob->lpVtbl->GetBufferSize(blob));
    if (!s) return;
    AcquireSRWLockExclusive(&s_shk_lock);
    for (u32 i = shk_home(s), k = 0; k < SHK_N; k++, i = (i + 1u) & (SHK_N - 1u)) {
        if (s_shk_sum[i] == s) { s_shk_key[i] = key; break; }
        if (!s_shk_sum[i]) { s_shk_sum[i] = s; s_shk_key[i] = key; break; }
    }
    ReleaseSRWLockExclusive(&s_shk_lock);
}
static u64 shk_get(ID3DBlob* blob)
{
    if (!blob) return 0;
    const u64 s = dxbc_sum64(blob->lpVtbl->GetBufferPointer(blob), blob->lpVtbl->GetBufferSize(blob));
    if (!s) return 0;
    u64 key = 0;
    AcquireSRWLockShared(&s_shk_lock);
    for (u32 i = shk_home(s), k = 0; k < SHK_N; k++, i = (i + 1u) & (SHK_N - 1u)) {
        if (s_shk_sum[i] == s) { key = s_shk_key[i]; break; }
        if (!s_shk_sum[i]) break;
    }
    ReleaseSRWLockShared(&s_shk_lock);
    return key;
}
/* cache_only: return E_FAIL instead of running FXC on a cache miss. */
static HRESULT shc_compile_ex(const char* src, size_t len, const char* name, const char* entry,
                              const char* target, ID3DBlob** code, ID3DBlob** err, int cache_only);
static HRESULT shc_compile(const char* src, size_t len, const char* name, const char* entry,
                           const char* target, ID3DBlob** code, ID3DBlob** err)
{ return shc_compile_ex(src, len, name, entry, target, code, err, 0); }
static HRESULT shc_compile_ex(const char* src, size_t len, const char* name, const char* entry,
                              const char* target, ID3DBlob** code, ID3DBlob** err, int cache_only)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_SHADERCACHE"); en = e ? atoi(e) : 1;
                  if (en) CreateDirectoryA("shadercache", NULL); }
    *code = NULL; if (err) *err = NULL;
    char path[64] = "";
    u64 shk_h = 0;                       /* v128: this source's cache key */
    if (en) {
        u64 h = 14695981039346656037ull;
        h = shc_hash("rfom-shc-v1", 11, h);
        h = shc_hash(target, strlen(target) + 1, h);
        h = shc_hash(entry, strlen(entry) + 1, h);
        h = shc_hash(src, len, h);
        snprintf(path, sizeof path, "shadercache\\%016llX.dxbc", (unsigned long long)h);
        shk_h = h;
        const uint64_t ft0 = rft0();
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz; ID3DBlob* b = NULL; DWORD got = 0;
            if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64 << 20) &&
                SUCCEEDED(D3DCreateBlob((SIZE_T)sz.QuadPart, &b)) && b &&
                ReadFile(f, b->lpVtbl->GetBufferPointer(b), (DWORD)sz.QuadPart, &got, NULL) &&
                got == (DWORD)sz.QuadPart) {
                CloseHandle(f);
                InterlockedIncrement(&s_shc_hits);
                rft(17, 1u, (uint32_t)len, ft0);
                *code = b;
                shk_put(b, h);
                return S_OK;
            }
            if (b) b->lpVtbl->Release(b);
            CloseHandle(f);
        }
    }
    if (cache_only) return E_FAIL;
    const uint64_t ftc = rft0();
    LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0);
    /* Resistance port: FXC's own optimiser is most of its ~30 ms per guest
     * program, and the driver re-optimises the bytecode into GPU code anyway.
     * RSX_FXC_OPT (0 = D3DCOMPILE_OPTIMIZATION_LEVEL0; default 1 = FXC's
     * default level -- level 0 measured SLOWER on York's big programs, 290 ms
     * vs 32 ms average). A program that fails at level 0 is retried at the
     * default. The cache key is unchanged, so programs already cached keep
     * their fully optimised bytecode. */
    static int fxo = -1;
    if (fxo < 0) { const char* e = getenv("RSX_FXC_OPT"); fxo = e ? atoi(e) : 1; }
    HRESULT hr = D3DCompile(src, len, name, NULL, NULL, entry, target, fxo ? 0u : (1u << 14), 0, code, err);
    if (FAILED(hr) && !fxo) {
        if (*code) { (*code)->lpVtbl->Release(*code); *code = NULL; }
        if (err && *err) { (*err)->lpVtbl->Release(*err); *err = NULL; }
        hr = D3DCompile(src, len, name, NULL, NULL, entry, target, 0, 0, code, err);
    }
    rft(17, 0u, (uint32_t)len, ftc);
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
    s_shc_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    InterlockedIncrement(&s_shc_miss);
    /* v142: keep the HLSL of every program FXC had to compile -- next to its
     * bytecode (shadercache\<key>.hlsl) and in logs\shmiss\ for this run. The
     * same intro recompiled ~15 programs on every launch, so some part of the
     * generated source differs between runs; two runs of these files show
     * which. RSX_SHMISS_SAVE=0 off. */
    { static int sv = -1;
      if (sv < 0) { const char* e2 = getenv("RSX_SHMISS_SAVE"); sv = e2 ? atoi(e2) : 1;
                    if (sv) CreateDirectoryA("logs\\shmiss", NULL); }
      if (sv && en && shk_h) {
          char fn[96];
          for (int w = 0; w < 2; w++) {
              snprintf(fn, sizeof fn, w ? "logs\\shmiss\\%016llX_%s.hlsl" : "shadercache\\%016llX.hlsl",
                       (unsigned long long)shk_h, target);
              FILE* hf = fopen(fn, "wb");
              if (hf) { fwrite(src, 1, len, hf); fclose(hf); }
          }
          static volatile LONG nmiss = 0;
          const LONG k = InterlockedIncrement(&nmiss);
          if (k <= 200) fprintf(stderr, "[shmiss] %016llX %s %s %u bytes (%.0f ms)%c", (unsigned long long)shk_h,
                                name ? name : "?", target, (unsigned)len,
                                (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart, 10);
      } }
    if (en && SUCCEEDED(hr) && *code) {
        shk_put(*code, shk_h);
        char tmp[80]; snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
        HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD put = 0; const DWORD n = (DWORD)(*code)->lpVtbl->GetBufferSize(*code);
            const BOOL ok = WriteFile(f, (*code)->lpVtbl->GetBufferPointer(*code), n, &put, NULL) && put == n;
            CloseHandle(f);
            if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
    return hr;
}

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
#define MAX_VERTICES    786432  /* v135: 393216 -> 786432 (wide FOV overflowed it: midflush + GPU wait). per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
/* Resistance port: 2048 -> 6144. York's opening area records ~4-5k draws a
 * frame, so every frame was cut into 2-3 mid-frame flushes, and each flush
 * waits for the render thread and for the GPU to finish the batch before
 * last (only two upload halves): the FIFO thread, render thread and GPU ran
 * one after another instead of overlapping. */
#define MAX_DRAWS         10240 /* v172: 6144 -> 10240. York's interiors draw ~6200 a frame: every frame split into two batches and the second waited for the GPU to finish the first (GPU 2%..74%, 22 fps). per-frame draw records. Sized from the
                                 * measured worst case (~1841, FRAME_BUDGET);
                                 * the per-draw constant buffer is
                                 * VP_CB_STRIDE * MAX_DRAWS * 2 bytes, so 16384
                                 * asked for a 264 MB upload allocation. */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B) rounded up to D3D12's 256-byte CBV alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
/* Resistance port: + vp_branch (16 B) + the GPU vertex fetch table (vp_gfa,
 * vp_gfc, vp_gfx: 528 B) = 8768 B -> 8960. */
#define VP_CB_STRIDE      8960
/* b1 per-draw FP slot: rsx_texscale[4] + rsx_alphatest + fp_k[64]
 * = 69 float4 = 1104 B, rounded to D3D12's 256-byte CBV alignment. */
#define VP_FPCB_STRIDE    1280
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */

#define RSX_UNITS 16  /* see DRAW_SRV_BASE. 8 -> 16: York's soft particles sample unit 15 */
/* Resistance port: indexed VP draws (see vp_indexed_fetch). */
/* v146: 4M -> 16M. York's rope-drop street issues more than 4M indices a
 * frame; once the frame's index buffer was full EVERY later triangle draw
 * left the GPU vertex-fetch path (3M fallbacks, gf_begin exit #3) and was
 * converted on the CPU -- 52% of the render thread, 18-23 fps. The buffer is
 * upload-heap system memory: 2 x 64 MB. */
#define VP_MAX_INDICES (16u << 20)
struct ID3D12Resource;
static struct ID3D12Resource* s_vp_ib;
static u32* s_vp_ib_mapped;
static u32 s_vp_ib_off;
static u32 s_pend_ib_first, s_pend_ib_count;
static int s_ib_force_expand; static u32 s_ib_delta;  /* chained-batch merge (d3d12_draw_indexed) */
static int s_ib_joining;   /* this batch continues the previous indexed draw: must stay indexed */
/* GPU vertex fetch state (gf_prepare, further down). */
#define GF_CAP        (160u << 20)        /* raw bytes per frame half */
#define GF_CB_OFF     8240u               /* vp_gfa[16] after vp_branch */
static struct ID3D12Resource* s_gf_buf;
static u8* s_gf_mapped;
static u32 s_gf_off;
static int s_gf_rec;                      /* the record being built uses GF */
static u32 s_gf_ib_first;
static u64 s_gf_draws, s_gf_bytes, s_gf_fallback;
static u32 s_gf_gen;                      /* v190: bumped whenever s_gf_off restarts (dedup validity) */
static u64 s_gf_dup_bytes, s_gf_dup_changed;   /* v190 stats: bytes reused, reuse refused (data changed) */
static int gf_dedup_on(void);

static int gf_enabled(void)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_GPUFETCH"); en = e ? atoi(e) : 1; }
    return en;
}

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 ib_first;       /* Resistance port: indexed VP draw -- first index in vp_ib */
    u32 ib_count;       /* 0 = non-indexed (expanded) draw */
    u32 topology;       /* rsx_topology (NOT a D3D value; see topo_to_d3d) */
    int textured;       /* 1 = sample the bound font/atlas texture (dbgfont) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4) */
    /* VP path per-draw shader/texture state, captured at draw_arrays time. */
    u32 fp_addr;        /* SET_SHADER_PROGRAM value (guest FP ucode location)   */
    int fp_exp32;       /* SET_SHADER_CONTROL 32-bit-exports bit at draw time   */
    u32 alpha_ctl;      /* alpha test: enable<<16 | (func&0xFF)<<8 | ref */
    u32 begin_epoch;    /* SET_BEGIN_END generation, for batch concatenation */
    u32 cull;           /* packed face culling: bit0 enable, bit1 cull FRONT
                         * (else BACK), bit2 front face is CCW. RSX culls back
                         * faces on most solid geometry; rendering everything
                         * double-sided lets a shell's interior faces show
                         * through and shade unlit (black). */
    u32 cmask;          /* D3D write mask from SET_COLOR_MASK at draw time
                         * (wave's sim passes write single lanes of the height
                         * maps; ignoring the mask stomped persistent state) */
    /* Per-unit textures (t0-t3): decompiled FPs sample up to 4 units
     * (demosaic's interpolation passes read 3). Captured at draw time. */
    struct {
        u32 off;        /* resolved vm offset (guest upload source), 0 = none */
        u32 raw;        /* raw RSX offset (offscreen-RT matching) */
        u32 w, h, fmt;  /* dims + RSX base format */
        u32 ctrl1;      /* NV4097 TEXTURE_CONTROL1: component remap crossbar */
        u32 pitch;      /* NV4097 TEXTURE_CONTROL3 row pitch (linear images) */
        u32 mips;       /* SET_TEXTURE_FORMAT bits 16..31: mipmap level count.
                         * Cube faces sit one whole mip pyramid apart, so this
                         * is what sets the face stride. */
        int cube;       /* SET_TEXTURE_FORMAT bit 2: a cube texture. Sampled
                         * with a 3-component direction, not a 2D uv -- treating
                         * one as 2D is what makes an environment-mapped chrome
                         * surface come out with black patches and banding. */
        int set;
        u32 xmask;      /* rsx_texture_expand_mask: lanes re-centred to signed */
        u32 freg, filter, address, ctrl0;   /* raw regs for depth-compare (shadow) decode */
    } tex[RSX_UNITS];
    int tex_rt[RSX_UNITS];      /* pre-pass: OffRT index sampled by unit, -1 = none */
    int tex_slot;       /* legacy single-slot path (atlas); -1 = none */
    int vs_idx;         /* VPVSEntry slot for this draw's vertex program (-1 = primary) */
    int blend;          /* guest blend enable at draw time */
    u32 blend_key;      /* packed guest blend state (factors+equation), PSO key */
    /* Render-to-texture: which colour surface this op targets. 0 = a display
     * buffer (the swap-chain backbuffer); else the resolved vm offset of an
     * offscreen surface (demosaic chains its effect passes through local-
     * memory buffers and composites from them). */
    u32 rt_off;
    /* Which per-draw constant slot this record's constants were written to.
     * vp_record_cb writes them keyed by the RECORD index, so any pass that
     * reorders or compacts T_DRAWS (DRAW_KEEP_TEX, DRAW_LAST_TEX,
     * DRAW_LIMIT) would otherwise make every surviving draw read some other
     * object's MVP -- silently transforming it somewhere else entirely. */
    u32 cb_slot;
    u32 rt_mrt[3];      /* colour targets B,C,D (MRT1/2/3), 0 = none. Deferred
                         * shading G-buffers write 3-4 targets in one pass. */
    u32 rt_w, rt_h;     /* surface clip dims at record time (offscreen RT size) */
    u32 rt_fmt;         /* RSX surface colour format (SET_SURFACE_FORMAT [4:0]) */
    /* Guest viewport rect at draw time (target pixels). Sub-viewport layouts
     * (wave's debug tiles) position quads with this, not with constants --
     * forcing full-target viewports drew every tile window-sized. */
    u32 vp_x, vp_y, vp_w, vp_h;
    /* Ordered clear op (offscreen surfaces only; display clears stay the
     * frame-start backbuffer clear). is_clear records also set is_vp so the
     * legacy replay pass skips them. */
    int   is_clear;
    float cc[4];
    u32   clear_mask;   /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 colour */
    float clear_z;
    u8    clear_s;
    int   gf;           /* Resistance port: GPU vertex fetch (raw buffer + SV_VertexID) */
    u32   gf_seq;       /* GPU fetch: which open-record generation this is (gf_finalize) */
    u32   srv_win;      /* Resistance port: SRV window this draw binds (its own, or an
                         * earlier draw's with identical bindings) */
    u32   zeta_off;     /* Resistance port: SET_SURFACE_ZETA_OFFSET at record time */
    u32   disp_tag;     /* Resistance port: display buffer drawn into (offset+1), 0 = none */
    u32   samp_disp;    /* Resistance port: display buffer sampled (offset+1), 0 = none */
    int   rt_slot;      /* v126: the off_rt slot (size variant) this op targets, -1 none */
    int   rt_mrt_slot[3];
    /* v165 FSR (rsx_fsr.inc): 1 = jittered scene draw; its record frame, the
     * byte offset of its previous-frame constants in the history buffer, and
     * the clip-space jitter added to it. */
    int   fsr;
    u32   stencil_ref;  /* v178: front stencil reference (dynamic; the rest of the stencil state is in cull >> 24) */
    u32   fsr_frame, fsr_prev;
    float fsr_jnx, fsr_jny;
    u8 fsr_mt; u16 fsr_chain; float fsr_bd; u32 fsr_slot, fsr_pslot;   /* v184 diag: how the previous frame was matched */
    /* v189: NV3089 copy out of a GPU surface (is_clear record, clear_mask 0):
     * source surface offset and image size, start and step in source pixels. */
    int   is_blit;
    u32   bl_src, bl_in_w, bl_in_h;
    u32   bl_dst;       /* v189c: destination offset (a copy texture, not a render target) */
    u16   gbc_mask;     /* v189c: units bound to a copy texture */
    float bl_u0, bl_v0, bl_du, bl_dv;
} D3D12DrawRecord;
int fsr_mode(void);
static void fsr_record(D3D12DrawRecord* dr, int vs_idx, char* dst, const char* csrc, const float* epi);
static void fsr_record_frame_end(void);

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    ID3D12Resource*       res;
    ID3D12Resource*       up;          /* init-upload staging (kept alive) */
    u32                   off, w, h;   /* raw RSX offset + dims */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
    u64                   last;        /* v126: record-order stamp of its last use as a target */
    u64                   clast;       /* v189: stamp of its last COLOUR write (draw with cmask, colour clear) */
} OffRT;
#define MAX_OFF_RTS  256 /* v171: 64 -> 256. York packs dozens of small shadow maps per frame at changing offsets and sizes; with 64 slots ~26 surfaces a frame were destroyed and re-created (30-40 fps). Slots must stay < 256: tex_rt packs them as 0x1000|slot etc. and unpacks with & 0xFF. Was: demosaic: 6-surface chain x2; Fall of Man: ~20 shadow/FX surfaces a frame, plus their size variants (v126: 48 -> 64) */
#define RT_SRV_BASE  5   /* SRV heap slots 5..(5+MAX_OFF_RTS-1) hold offscreen-RT SRVs */
/* Per-draw SRV windows: each VP draw gets 4 consecutive descriptors (t0-t3)
 * so multi-unit fragment programs see all their textures (demosaic's
 * interpolation passes sample 3 units). */
/* Resistance port: texture units carried per draw (was 4). Fall of Man's
 * world shaders sample units 4 and 7; with a 4-entry SRV table and four
 * static samplers their pipelines failed (E_INVALIDARG) and the scene drew
 * black. */
/* Resistance port: above RT_SRV_BASE + MAX_OFF_RTS AND above the per-texture
 * SRVs vp_upload_tex_slot writes at 1 + slot (VP_TEX_SLOTS = 768), and two
 * windows per draw -- one per batch parity -- so recording batch N+1 never
 * rewrites descriptors batch N is still reading (RSX_PIPELINE). */
#define DRAW_SRV_BASE 4160   /* above 1 + VP_TEX_SLOTS (4096) */
/* v181: per-batch upload streams are TRIPLE-buffered (NPAR halves, "parity" =
 * batch index mod NPAR): the FIFO thread no longer waits for the GPU to finish
 * the batch just handed off before it records the next one -- only the one
 * before that. F11 in York: FIFO thread 23% in that fence wait, main thread
 * 17-31% waiting on it in turn (occlusion-report wait). */
#define NPAR 3
#define SRV_HEAP_SIZE (DRAW_SRV_BASE + NPAR * MAX_DRAWS * RSX_UNITS)

/* Per-frame VP texture slot: a guest texture uploaded for this frame's VP
 * draws (re-uploaded every frame -- gcm/cube's plasma animates in guest
 * memory). SRV lives at heap index 1+slot. */
typedef struct {
    ID3D12Resource* res;
    ID3D12Resource* up;
    u32 off, w, h, fmt; /* current contents (resource reused when dims match) */
    u32 csum;           /* sparse checksum of the source bytes last uploaded */
    u32 csum_q;         /* Resistance port: 128-sample quick checksum (per-frame check) */
    u32 csum_n;         /* checks since the last full (1024-sample) one */
    int cube;           /* resource is a 6-face cube, sampled as TextureCube */
    u32 key;            /* the ORIGINAL bound offset -- the cache lookup key.
                         * off is the RESOLVED source after TEX_OFF_BIAS/TEX_REMAP
                         * shift it, so keying on off never matches the raw offset
                         * a later draw arrives with, and every draw re-uploads. */
    int used;           /* referenced this frame */
    u32 nmips;          /* Resistance port: mip levels in res (1 = base only) */
    u32 up_batch;       /* Resistance port: batch whose copy last read `up` (RSX_PIPELINE) */
    u64 last_use;       /* Resistance port: LRU stamp (s_tex_clock) */
    int next;           /* hash chain on key (index+1, 0 = end) */
    int chained;        /* currently linked into s_tex_head */
    u32 dxfmt;          /* DXGI format the decoded image was uploaded as */
} VPTexSlot;
/* Distinct textures uploadable per frame. Four was enough for a UI/atlas-style
 * frame but not for a scene: this title binds ~20 distinct textures per frame,
 * so every draw past the fourth got slot -1 from vp_upload_tex_slot ("out of
 * slots") and drew UNTEXTURED. That reads as a correctly-lit but solid black
 * object, which is indistinguishable from a missing texture upload. */
/* Resistance port: 96 -> 768. York binds far more distinct textures per frame
 * than 96, and the old "first slot not used in this batch" eviction recycled a
 * texture the very next batch wanted -- every draw re-created D3D resources
 * (CreateCommittedResource, ~30% of the render thread) and a batch with >96
 * textures got NULL SRVs (grey/black objects). Lookup is now hashed and
 * eviction is LRU, preferring a victim with the same size and format so its
 * resources can be reused as-is. */
/* Resistance port: 768 -> 4096. York's overhead/long views reference well over
 * 768 distinct textures a frame; the LRU then evicted textures still needed
 * later in the same frame and re-uploaded ~600 a frame (80% of the render
 * thread, 3 fps after a respawn that showed the whole city). */
#define VP_TEX_SLOTS 4096
#define VP_TEX_HASH  8192
static int s_tex_head[VP_TEX_HASH];    /* slot index + 1, 0 = empty */
static u64 s_tex_clock = 0;
static u32 tex_bucket(u32 key) { key ^= key >> 15; key *= 0x2C1B3C6Du; key ^= key >> 12;
                                 return key & (VP_TEX_HASH - 1); }

/* Decompiled-VS cache: one entry per distinct vertex-program ucode seen at
 * draw time (hashed). Apps switch VPs between draws (gcm/cube: its MVP cube VP
 * vs dbgfont-gcm's text VP); compiling only the first left later draws
 * transformed by the wrong program (text offscreen). */
typedef struct {
    u32 hash;               /* FNV-1a of the ucode */
    ID3DBlob* vs;
    int uses_c03;
    u32 inmask;             /* attributes the program reads (rsx_vp_analyze_inputs) */
    ID3DBlob* vs_gf;        /* GPU-vertex-fetch variant (gf_compile_vs), NULL = none */
    /* Resistance port: the constant rows the program reads, as runs, so the
     * per-draw slot copies only those (c_all: relative addressing -> all). */
    int c_all; u16 crun_n; u16 crun[48][2];
    volatile LONG pending;  /* Resistance port: compile queued on a worker (RSX_VS_ASYNC) */
    char* src;              /* v165: decompiled HLSL, kept for the FSR motion-vector variant */
} VPVSEntry;
#define VP_VS_CACHE 128 /* wave uses 5+ distinct VPs; at 4 the cache
                         * thrashed every frame and eviction shifted slots
                         * under recorded vs_idx values -- the display mesh
                         * drew with the WRONG vertex program (sub-rect +
                         * edge slivers instead of fullscreen). A fight in
                         * SoulCalibur II HD uses more than 16. */

/* Compiled guest-FP pipeline cache: decompiled VS (by cache slot) + decompiled
 * PS (fragment ucode at fp_addr). */
typedef struct {
    u32 fp_addr;
    int vs_idx;             /* VPVSEntry slot this PSO's VS came from */
    u32 vs_hash;            /* validates the slot hasn't been evicted */
    u32 gen;
    u32 blend;              /* packed guest blend key (enable+factors+eq, PSO key) */
    int nrt;                /* bound colour target count (PSO key)       */
    u32 rtfmt;              /* DXGI format of the colour targets (PSO key) */
    int exp32;              /* 32-bit-exports control bit (PSO key)       */
    u32 ucode_hash;         /* FNV-1a of the FP ucode: apps re-patch inline
                             * constants per frame (wave's stamp position/
                             * amplitude) -- address-only keying served the
                             * stale compile forever. */
    u32 cmask;              /* colour write mask (PSO key) */
    u32 cull;               /* packed face culling (PSO key) */
    u32 unorm_key;          /* Resistance port: unnormalised units + their sizes */
    u32 cube_mask;          /* which units are cube textures (PSO key): the HLSL
                             * declares those samplers as TextureCube, so a cube
                             * and a 2D variant of the same program are different
                             * pipelines and must not share a cache entry. */
    ID3D12PipelineState* pso;
    int next;               /* Resistance port: hash chain (index+1, 0 = end) */
} VPFPEntry;
/* Guest-FP PSO cache. 16 entries thrashed badly: this title needs far more
 * distinct (fp, vs, blend, rt, cmask, ucode-hash) combinations than that in a
 * single frame, so the FIFO evicted entries that were needed again immediately
 * and ~17 shaders were recompiled EVERY frame -- about half the frame's CPU
 * time. Entries are small (a PSO pointer plus key fields). */
/* Resistance port: 256 -> 8192 with a hashed lookup. York needs well over 256
 * (vs, fp, blend, target, mask, cull) combinations, so the FIFO evicted
 * pipelines the next batch needed and ~25% of the render thread went to
 * D3DCompile + CreateGraphicsPipelineState, every frame, forever. The FIFO
 * eviction also Released PSOs still referenced by the batch being recorded.
 * When the table fills it is now flushed whole at the next batch boundary
 * (after the GPU wait), never mid-batch. */
#define VP_FP_CACHE 8192
#define VP_FP_HASH  16384
static int s_fp_head[VP_FP_HASH];          /* entry index + 1 */
static int s_fp_flush_pending = 0;
static ID3D12PipelineState* s_fp_grave[256]; static int s_fp_grave_n = 0;
/* Compiled pixel-shader blobs, keyed on what the HLSL text depends on. A new
 * pipeline for a known program (different blend/target/VS) reuses the blob
 * instead of decompiling and running D3DCompile again. */
typedef struct { u32 uhash, exp32, unorm, cube, used; ID3DBlob* blob; volatile LONG pending; } FPBlobEntry;
#define FP_BLOB_CACHE 4096
static FPBlobEntry s_fp_blob[FP_BLOB_CACHE];
static u32 fp_blob_slot(u32 uh, u32 e, u32 un, u32 cu) {
    u32 h = uh * 0x9E3779B1u ^ (e * 0x85EBCA77u) ^ (un * 0xC2B2AE3Du) ^ (cu * 0x27D4EB2Fu);
    return (h ^ (h >> 15)) & (FP_BLOB_CACHE - 1); }
static int s_perf_fp_compiles = 0;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    u32  width;
    u32  height;
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12CommandAllocator*    cmd_allocators[NPAR > FRAME_COUNT ? NPAR : FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. */
    ID3D12Resource*       readback_buf;
    u32                   readback_pitch;
    int                   dump_frames_left;
    int                   dump_skip_left;   /* CELLMARK_DUMP_SKIP: presents to ignore first */

    /* Textured pipeline (dbgfont / 2D atlas quads). The font atlas is an 8-bit
     * coverage texture uploaded as R8_UNORM and sampled in the pixel shader. */
    ID3D12PipelineState*  pipeline_state_tex;   /* triangle + texture + blend */
    ID3D12DescriptorHeap* srv_heap;             /* shader-visible, 1 SRV slot  */
    ID3D12Resource*       tex_resource;         /* current atlas texture       */
    ID3D12Resource*       tex_upload;           /* staging buffer for uploads  */
    u32                   tex_w, tex_h;         /* dims of tex_resource         */
    int                   tex_ready;            /* 1 once an atlas is uploaded  */
    u32                   tex_src_offset;       /* guest RSX offset of atlas    */
    int                   tex_bound;            /* a texture is bound for draws */
    int                   tex_dirty;            /* re-upload needed this frame  */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    ID3D12RootSignature*  vp_root_sig;          /* CBV(b0) + SRV table + sampler */
    ID3D12PipelineState*  pipeline_state_vp;    /* decompiled VS + atlas PS      */
    ID3D12PipelineState*  pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    ID3D12Resource*       vp_vb;                /* raw float4 attrib0, per-frame */
    void*                 vp_vb_mapped;
    u32                   vp_vb_offset;
    ID3D12Resource*       vp_cb;                /* per-draw VP constant slots    */
    void*                 vp_cb_mapped;
    ID3D12Resource*       vp_fpcb;              /* per-draw FP texscale (b1)     */
    void*                 vp_fpcb_mapped;
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    ID3DBlob*             vp_vs_blob;           /* kept for guest-FP PSO builds  */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* per-frame VP texture slots    */
    VPVSEntry             vp_vs[VP_VS_CACHE];   /* per-draw decompiled VS cache  */
    int                   vp_vs_n;
    VPFPEntry             vp_fp[VP_FP_CACHE];   /* guest-FP PSO cache            */
    int                   vp_fp_n;
    u32                   srv_inc;              /* CBV_SRV_UAV descriptor size   */
    /* VP path: latest texture bound per unit (t0-t3). */
    struct { u32 off, raw, w, h, fmt, ctrl1, pitch, mips; int cube; int set; u32 xmask; u32 freg, filter, address, ctrl0; } cur_texs[RSX_UNITS];

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
    /* DRAW_ARRAYS batch merging (see d3d12_draw_arrays): the vertex index one
     * past the last batch, and whether the current BEGIN/END may still be
     * extended. */
    u32                   merge_first_end;
    int                   merge_prev_draw;
    ID3D12DescriptorHeap* rt_rtv_heap;          /* MAX_OFF_RTS RTVs (CPU only)   */

    /* Frame-parity double buffering for the per-draw upload streams (vp_vb
     * vertices, vp_cb constants, vp_fpcb texscales): records for frame N+1
     * are written while frame N's GPU work may still be reading -- writes go
     * to the other half. Toggled at the end of render_frame. */
    int                   vp_parity;

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    D3D12DrawRecord       draws[MAX_DRAWS];

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;

/* Resistance port (v133): display output decoupled from the swap chain.
 * The guest renders into render_targets[] exactly as before, but those are now
 * our own 1280x720 ("virtual backbuffer") textures; at present time a small
 * pass scales the finished frame into the real swap chain, whose buffers match
 * the window's client area. That is what lets the window be resized, go
 * borderless fullscreen at the monitor's resolution (Alt+Enter, or
 * RSX_DISPLAY=borderless) and use tearing / FreeSync presents, without the
 * renderer knowing. Settings:
 *   RSX_DISPLAY = windowed (default) | borderless
 *   RSX_WINDOW  = <w>x<h>   windowed client size (default the guest size)
 *   RSX_SHARPEN = 0..1      sharpening after the upscale (default 0)
 *   RSX_FILTER  = linear (default) | point   upscale filter */
static ID3D12Resource*       s_sc_buf[FRAME_COUNT];
static ID3D12DescriptorHeap* s_sc_rtv_heap;
static u32                   s_sc_w, s_sc_h, s_sc_rtv_inc;
static UINT                  s_sc_flags;
static int                   s_tearing;
static volatile LONG         s_sc_resize;
static ID3D12RootSignature*  s_blit_rs;
static ID3D12PipelineState*  s_blit_pso;
static ID3D12DescriptorHeap* s_blit_srv_heap;
static u32                   s_blit_srv_inc;
static ID3D12CommandAllocator*    s_blit_alloc[FRAME_COUNT];
static ID3D12GraphicsCommandList* s_blit_list;
static ID3D12Fence*          s_blit_fence;
static HANDLE                s_blit_ev;
static u64                   s_blit_fv[FRAME_COUNT], s_blit_ctr;
static u32                   s_vbb_index;          /* virtual backbuffer of the frame being drawn */
static int                   s_disp_borderless;
static RECT                  s_disp_windowed_rect; /* window rect to restore from borderless */
static float                 s_blit_sharpen;

/* Resistance port (v136): internal render resolution. RSX_RES_SCALE=<f>
 * (1 = the PS3's 1280x720; 1.5 = 1920x1080; 2 = 2560x1440; 3 = 3840x2160).
 * Every GPU surface the guest renders into -- colour targets, depth/zeta
 * surfaces, the virtual backbuffer and the copies taken of them -- is created
 * at scale, and viewports/scissors/copy boxes are scaled to match, while all
 * bookkeeping (off_rt[].w/h, s_zt[].w/h, s_d3d.width/height) stays in guest
 * pixels. Sampling is normalised, so the guest's own texture coordinates keep
 * working; the fragment decompiler divides WPOS by the scale. */
static float s_rs_scale;
static float rs_scale(void)
{
    if (s_rs_scale <= 0.0f) {
        const char* e = getenv("RSX_RES_SCALE");
        float v = e ? (float)atof(e) : 1.0f;
        if (!(v >= 1.0f)) v = 1.0f;
        if (v > 4.0f) v = 4.0f;
        s_rs_scale = v;
        if (v != 1.0f) fprintf(stderr, "[display] internal resolution x%.2f%c", v, 10);
    }
    return s_rs_scale;
}
static u32 SC(u32 v)
{
    if (!v) return 0;
    const u32 r = (u32)((float)v * rs_scale() + 0.5f);
    return r ? r : 1u;
}
static float SCF(float v) { return v * rs_scale(); }
/* Resistance port: the batch being RECORDED (FIFO thread) and the batch being
 * RENDERED (render thread, RSX_ASYNC) are different, so the per-batch draw
 * list, its count and its upload-half parity are per thread. Single-threaded
 * (RSX_ASYNC=0) both are the FIFO thread's. */
static D3D12DrawRecord s_draws_buf[2][MAX_DRAWS];
static __declspec(thread) D3D12DrawRecord* T_DRAWS = s_draws_buf[0];
static __declspec(thread) u32 T_DRAWCNT;
static __declspec(thread) int T_PARITY;
char g_rsx_title_base[128] = "ps3recomp";
static u32 s_dbg_last_draws = 0;
static u64 s_req_verts = 0, s_req_draws = 0, s_drop_draws = 0;
/* Raw bind offset of the duck's texture, identified by CONTENT (duck.tga is
 * overwhelmingly yellow, avg 243,191,23). VRAM offsets move between runs, so a
 * hard-coded offset in a filter silently matches nothing and the run looks like
 * "renders nothing" -- which cost real time. DRAW_KEEP_TEX=duck resolves here. */
/* Copy of the last completed frame, bound wherever a draw samples a
 * DISPLAY-SIZED texture. On RSX this title renders its reflection into a corner
 * of the render surface and then samples that surface as a texture; our backend
 * renders into a D3D backbuffer, so the guest memory behind that sampler is
 * never written and it reads empty -- which is why the water had no reflection.
 * Feeding it the previous frame costs one frame of latency, which for a water
 * reflection is not visible. */
static ID3D12Resource* s_screen_copy = NULL;
/* Sub-viewport render-to-texture. This title renders its reflection/refraction
 * pre-pass into a REGION of the same surface (vp 0,208 512x512 and 0,208
 * 1024x512, cmask=F) and then samples a texture of exactly that size. On
 * hardware the pass lands in the buffer that texture points at; our backend
 * renders into D3D resources, so that guest buffer is never written and every
 * fluid sampler reads zero. Capture each region into a matching resource and
 * bind it for samplers whose guest source is empty and whose size matches.
 *
 * SUBVP_RTT=1 to enable. OFF by default because it cannot be validated on this
 * title yet: the fluid draws that would consume these captures never rasterize
 * (see below), so binding them changes nothing observable. The capture and
 * binding themselves are confirmed working -- SUBVP_DBG shows the 512x512 and
 * 256x256 regions captured and handed to fp 0x5DB81/0x5EA81/0x5EF81/0x5E381. */
#define SUBVP_SLOTS 4
static struct { ID3D12Resource* res; u32 x, y, w, h; } s_subvp[SUBVP_SLOTS];
static int s_subvp_n = 0;

static void subvp_note(u32 x, u32 y, u32 w, u32 h)
{
    if (!w || !h || w > s_d3d.width || h > s_d3d.height) return;
    if (w == s_d3d.width && h == s_d3d.height) return;      /* full-screen pass */
    for (int i = 0; i < s_subvp_n; i++)
        if (s_subvp[i].w == w && s_subvp[i].h == h) { s_subvp[i].x = x; s_subvp[i].y = y; return; }
    if (s_subvp_n >= SUBVP_SLOTS) return;
    s_subvp[s_subvp_n].x = x; s_subvp[s_subvp_n].y = y;
    s_subvp[s_subvp_n].w = w; s_subvp[s_subvp_n].h = h;
    s_subvp[s_subvp_n].res = NULL;
    s_subvp_n++;
}

/* Copy each noted region out of the backbuffer. Called at the same moment as
 * screen_copy_capture -- after the reduced-viewport passes and before the
 * full-screen scene overwrites them. */
static void subvp_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SUBVP_RTT"); en = e ? atoi(e) : 0; }
    if (!en || !s_d3d.device || !s_subvp_n) return;
    for (int i = 0; i < s_subvp_n; i++) {
        if (!s_subvp[i].res) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = {0};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = SC(s_subvp[i].w); td.Height = SC(s_subvp[i].h);
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                    &IID_ID3D12Resource, (void**)&s_subvp[i].res)))
                { s_subvp[i].res = NULL; continue; }
        }
        D3D12_RESOURCE_BARRIER bb[2] = {0};
        bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb[0].Transition.pResource   = s_d3d.render_targets[fi];
        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb[1] = bb[0];
        bb[1].Transition.pResource   = s_subvp[i].res;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

        D3D12_TEXTURE_COPY_LOCATION dstl = {0}, srcl = {0};
        dstl.pResource = s_subvp[i].res;
        dstl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstl.SubresourceIndex = 0;
        srcl.pResource = s_d3d.render_targets[fi];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        D3D12_BOX box; box.left = SC(s_subvp[i].x); box.top = SC(s_subvp[i].y);
        box.front = 0; box.right = box.left + SC(s_subvp[i].w);
        box.bottom = box.top + SC(s_subvp[i].h); box.back = 1;
        if (box.right > SC(s_d3d.width))  box.right  = SC(s_d3d.width);
        if (box.bottom > SC(s_d3d.height)) box.bottom = SC(s_d3d.height);
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dstl, 0, 0, 0, &srcl, &box);

        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
        { static int _n = 0; if (getenv("SUBVP_DBG") && PS3_LOG_FIRST(_n, 8))
            fprintf(stderr, "[SUBVP] captured %ux%u from (%u,%u)%c",
                    s_subvp[i].w, s_subvp[i].h, s_subvp[i].x, s_subvp[i].y, 10); }
    }
}
static void screen_copy_capture(u32 fi);   /* fwd */
static int  s_sc_dump_pending = 0;
static u32 s_duck_raw = 0;   /* as BOUND (what draw records carry) */
static u32 s_duck_off = 0;   /* as RESOLVED (what the uploader sees)   */
/* PERF=1: where a frame's CPU time actually goes. Guessing at this is how you
 * spend an afternoon optimising the wrong loop. */
static double s_perf_tex = 0.0, s_perf_frame = 0.0, s_perf_vtx = 0.0, s_perf_rf = 0.0;
static double s_perf_gpu = 0.0, s_perf_pre = 0.0, s_perf_srv = 0.0, s_perf_pso = 0.0;
static double s_perf_present = 0.0, s_perf_wait = 0.0, s_perf_vdrain = 0.0;   /* Resistance port */
static int s_perf_pso_calls = 0, s_perf_pso_miss = 0, s_perf_pso_hashbytes = 0;
static u64    s_perf_nverts = 0;
static u64    s_perf_texbytes = 0;
static int    s_perf_ntex = 0;
static double perf_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
/* VP_SUBMIT=<N> logs the first N; VP_SUBMIT_EVERY=<P> then repeats a burst of N
 * every P, so a scene reached minutes after boot (a cinematic) is covered too. */
static int vps_gate(int* n, int cap)
{
    static int every = -1;
    if (every < 0) { const char* e = getenv("VP_SUBMIT_EVERY"); every = e ? atoi(e) : 0; }
    int k = (*n)++;
    if (k < cap) return 1;
    return every > cap && (k % every) < cap;
}

static int perf_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PERF"); v = e ? atoi(e) : 0; }
    return v;
}
/* DBG_LOCK: running clip-space centroid of the tracked mesh (RSX_DBG_VTX's
 * texture), so the debug camera can follow it. The ducks are driven by the
 * physics sim and drift every frame, so a fixed DBG_CENTER loses them as soon
 * as the magnification is high enough to make one recognisable. */
static float s_lock_x = 0.0f, s_lock_y = 0.0f;
static int   s_lock_valid = 0;
/* Accumulated over one frame, published at the frame boundary. Averaging as the
 * frame is recorded makes the aim wander while it is being used, which at high
 * magnification walks the target off-screen entirely. */
static double s_lock_sx = 0.0, s_lock_sy = 0.0;
static u32    s_lock_n  = 0;

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

/* Borderless fullscreen on the monitor the window is on, or back to the
 * saved windowed rect. Runs on the window's thread (from the wndproc). */
static void disp_set_borderless(HWND hwnd, int on)
{
    if (on) {
        if (!s_disp_borderless) GetWindowRect(hwnd, &s_disp_windowed_rect);
        MONITORINFO mi; mi.cbSize = sizeof mi;
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongPtrA(hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
        const RECT r = s_disp_windowed_rect;
        if (r.right > r.left && r.bottom > r.top)
            SetWindowPos(hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        else
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    s_disp_borderless = on;
    InterlockedExchange(&s_sc_resize, 1);
    fprintf(stderr, "[display] %s%c", on ? "borderless fullscreen" : "windowed", 10);
}

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        InterlockedExchange(&s_sc_resize, 1);   /* the render thread resizes the swap chain */
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {   /* Alt+Enter */
            if (!(lp & (1 << 30))) disp_set_borderless(hwnd, !s_disp_borderless);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;   /* no "ding" on Alt+Enter */
        break;
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        /* Resistance port: Esc is the game's pause button now (controls.txt);
         * ESC_CLOSES=1 restores closing the window on it. */
        if (wp == VK_ESCAPE && getenv("ESC_CLOSES")) {
            s_d3d.window_closed = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_INPUT: {
        /* Raw mouse deltas for the keyboard+mouse pad (libs/input/cellPad.c). */
        extern volatile long g_kbm_mouse_dx, g_kbm_mouse_dy, g_kbm_wheel, g_kbm_ml_dx, g_kbm_ml_dy;
        RAWINPUT ri; UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedExchangeAdd(&g_kbm_mouse_dx, ri.data.mouse.lLastX);
                InterlockedExchangeAdd(&g_kbm_mouse_dy, ri.data.mouse.lLastY);
                /* v149: the direct mouse look keeps its own count (stubs.cpp) */
                InterlockedExchangeAdd(&g_kbm_ml_dx, ri.data.mouse.lLastX);
                InterlockedExchangeAdd(&g_kbm_ml_dy, ri.data.mouse.lLastY);
            }
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                InterlockedExchangeAdd(&g_kbm_wheel, (short)ri.data.mouse.usButtonData);
        }
        break; }
    case WM_SETCURSOR: {
        extern volatile int g_kbm_capture;
        if (g_kbm_capture && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break; }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    /* v133: RSX_WINDOW=<w>x<h> sets the windowed client size (the guest still
     * renders at width x height; the present pass scales). */
    u32 cw = width, ch = height;
    { const char* e = getenv("RSX_WINDOW"); unsigned a = 0, b = 0;
      if (e && sscanf(e, "%ux%u", &a, &b) == 2 && a >= 320 && b >= 200 && a <= 16384 && b <= 16384) { cw = a; ch = b; } }
    RECT wr = {0, 0, (LONG)cw, (LONG)ch};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    /* Keep the display awake (see rsx_null_backend.c). */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    HWND hw = CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    /* Resistance port: raw mouse input for mouse-look (cellPad.c). */
    if (hw) {
        extern void* g_kbm_hwnd;
        g_kbm_hwnd = hw;
        RAWINPUTDEVICE rid = {0};
        rid.usUsagePage = 0x01; rid.usUsage = 0x02;   /* generic desktop / mouse */
        rid.dwFlags = 0; rid.hwndTarget = hw;
        if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
            printf("[D3D12] raw mouse input unavailable (%lu)\n", GetLastError());
        /* v133: RSX_DISPLAY=borderless starts in borderless fullscreen. */
        const char* dm = getenv("RSX_DISPLAY");
        if (dm && (!_stricmp(dm, "borderless") || !_stricmp(dm, "fullscreen") || !strcmp(dm, "1")))
            disp_set_borderless(hw, 1);
    }
    return hw;
}

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * v133 present pass: virtual backbuffer -> real swap chain (see s_sc_buf)
 * -----------------------------------------------------------------------*/
static const char s_blit_hlsl[] =
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "cbuffer C : register(b0) { float4 k; };\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  float3 c = t0.Sample(s0, i.uv).rgb;\n"
    "  if (k.x > 0) {\n"
    "    float3 n = t0.Sample(s0, i.uv + float2(0, -k.z)).rgb + t0.Sample(s0, i.uv + float2(0, k.z)).rgb\n"
    "             + t0.Sample(s0, i.uv + float2(-k.y, 0)).rgb + t0.Sample(s0, i.uv + float2(k.y, 0)).rgb;\n"
    "    c = saturate(c + k.x * (c * 4 - n) * 0.5);\n"
    "  }\n"
    "  return float4(c, 1); }\n";

static int sc_get_buffers(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_sc_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_sc_rtv_heap, &h);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (FAILED(s_d3d.swap_chain->lpVtbl->GetBuffer(s_d3d.swap_chain, i, &IID_ID3D12Resource,
                                                      (void**)&s_sc_buf[i]))) return -1;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_sc_buf[i], NULL, h);
        h.ptr += s_sc_rtv_inc;
    }
    return 0;
}

static int blit_init(void)
{
    ID3D12Device* d = s_d3d.device;
    HRESULT hr;
    { const char* e = getenv("RSX_SHARPEN"); s_blit_sharpen = e ? (float)atof(e) : 0.0f;
      if (s_blit_sharpen < 0.0f) s_blit_sharpen = 0.0f; if (s_blit_sharpen > 2.0f) s_blit_sharpen = 2.0f; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.NumDescriptors = FRAME_COUNT; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(d->lpVtbl->CreateDescriptorHeap(d, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_sc_rtv_heap))) return -1;
    s_sc_rtv_inc = d->lpVtbl->GetDescriptorHandleIncrementSize(d, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (sc_get_buffers()) return -1;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d->lpVtbl->CreateDescriptorHeap(d, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_blit_srv_heap))) return -1;
    s_blit_srv_inc = d->lpVtbl->GetDescriptorHandleIncrementSize(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    { D3D12_CPU_DESCRIPTOR_HANDLE h;
      s_blit_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_blit_srv_heap, &h);
      for (u32 i = 0; i < FRAME_COUNT; i++) {
          D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
          sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          sv.Texture2D.MipLevels = 1;
          d->lpVtbl->CreateShaderResourceView(d, s_d3d.render_targets[i], &sv, h);
          h.ptr += s_blit_srv_inc;
      } }

    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2] = {0};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0; rp[0].Constants.Num32BitValues = 4;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sm = {0};
    { const char* e = getenv("RSX_FILTER");
      sm.Filter = (e && !_stricmp(e, "point")) ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR; }
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.MaxLOD = D3D12_FLOAT32_MAX; sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 2; rd.pParameters = rp; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &sm;
    ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) { if (err) err->lpVtbl->Release(err); return -1; }
    hr = d->lpVtbl->CreateRootSignature(d, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                        &IID_ID3D12RootSignature, (void**)&s_blit_rs);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) return -1;

    ID3DBlob *vsb = NULL, *psb = NULL;
    if (FAILED(D3DCompile(s_blit_hlsl, sizeof s_blit_hlsl - 1, "present_blit", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, &err))) {
        if (err) { printf("[D3D12] present blit VS: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }
        return -1; }
    if (FAILED(D3DCompile(s_blit_hlsl, sizeof s_blit_hlsl - 1, "present_blit", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        if (err) { printf("[D3D12] present blit PS: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }
        vsb->lpVtbl->Release(vsb); return -1; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_blit_rs;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb); pd.VS.BytecodeLength = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb); pd.PS.BytecodeLength = psb->lpVtbl->GetBufferSize(psb);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = d->lpVtbl->CreateGraphicsPipelineState(d, &pd, &IID_ID3D12PipelineState, (void**)&s_blit_pso);
    vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb);
    if (FAILED(hr)) { printf("[D3D12] present blit PSO failed (0x%08lX)\n", hr); return -1; }

    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (FAILED(d->lpVtbl->CreateCommandAllocator(d, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       &IID_ID3D12CommandAllocator, (void**)&s_blit_alloc[i]))) return -1;
    if (FAILED(d->lpVtbl->CreateCommandList(d, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_blit_alloc[0], NULL,
                   &IID_ID3D12GraphicsCommandList, (void**)&s_blit_list))) return -1;
    s_blit_list->lpVtbl->Close(s_blit_list);
    if (FAILED(d->lpVtbl->CreateFence(d, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s_blit_fence))) return -1;
    s_blit_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return 0;
}

static void blit_wait(u64 v)
{
    if (v && s_blit_fence->lpVtbl->GetCompletedValue(s_blit_fence) < v) {
        s_blit_fence->lpVtbl->SetEventOnCompletion(s_blit_fence, v, s_blit_ev);
        WaitForSingleObject(s_blit_ev, 2000);
    }
}

/* The window changed size (or went borderless): match the swap chain to the
 * client area. Everything that used the old buffers is flushed first. */
static void sc_resize(void)
{
    RECT cr;
    if (!GetClientRect(s_d3d.hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return;   /* minimised */
    const u32 w = (u32)cr.right, h = (u32)cr.bottom;
    if (w == s_sc_w && h == s_sc_h) return;
    const u64 v = ++s_blit_ctr;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_blit_fence, v);
    blit_wait(v);
    for (u32 i = 0; i < FRAME_COUNT; i++) if (s_sc_buf[i]) { s_sc_buf[i]->lpVtbl->Release(s_sc_buf[i]); s_sc_buf[i] = NULL; }
    HRESULT hr = s_d3d.swap_chain->lpVtbl->ResizeBuffers(s_d3d.swap_chain, FRAME_COUNT, w, h,
                                                         DXGI_FORMAT_R8G8B8A8_UNORM, s_sc_flags);
    if (FAILED(hr)) fprintf(stderr, "[display] ResizeBuffers %ux%u failed (0x%08lX)%c", w, h, hr, 10);
    else { s_sc_w = w; s_sc_h = h; }
    sc_get_buffers();
    for (u32 i = 0; i < FRAME_COUNT; i++) s_blit_fv[i] = 0;
    fprintf(stderr, "[display] output %ux%u%c", s_sc_w, s_sc_h, 10);
}

/* Scale the finished guest frame (virtual backbuffer s_d3d.frame_index) into
 * the swap chain, letterboxed to its aspect, and present. */
static void present_blit(int vs)
{
    const u32 fi = s_d3d.frame_index;
    if (InterlockedExchange(&s_sc_resize, 0)) sc_resize();
    const UINT bi = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
    if (bi >= FRAME_COUNT || !s_sc_buf[bi]) return;
    blit_wait(s_blit_fv[bi]);
    ID3D12GraphicsCommandList* cl = s_blit_list;
    s_blit_alloc[bi]->lpVtbl->Reset(s_blit_alloc[bi]);
    cl->lpVtbl->Reset(cl, s_blit_alloc[bi], s_blit_pso);

    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = s_d3d.render_targets[fi];
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1] = b[0];
    b[1].Transition.pResource = s_sc_buf[bi];
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    s_sc_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_sc_rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)bi * s_sc_rtv_inc;
    static const float black[4] = {0, 0, 0, 1};
    cl->lpVtbl->ClearRenderTargetView(cl, rtv, black, 0, NULL);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);

    /* letterbox / pillarbox to the guest frame's aspect */
    const float ga = (float)s_d3d.width / (float)s_d3d.height, sa = (float)s_sc_w / (float)s_sc_h;
    D3D12_VIEWPORT vp = {0};
    if (sa > ga) { vp.Height = (float)s_sc_h; vp.Width = (float)s_sc_h * ga; vp.TopLeftX = ((float)s_sc_w - vp.Width) * 0.5f; }
    else         { vp.Width = (float)s_sc_w; vp.Height = (float)s_sc_w / ga; vp.TopLeftY = ((float)s_sc_h - vp.Height) * 0.5f; }
    vp.MaxDepth = 1.0f;
    D3D12_RECT sc = {0, 0, (LONG)s_sc_w, (LONG)s_sc_h};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_blit_rs);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_blit_srv_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    s_blit_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_blit_srv_heap, &g);
    g.ptr += (UINT64)fi * s_blit_srv_inc;
    const float k[4] = { s_blit_sharpen, 1.0f / (float)SC(s_d3d.width), 1.0f / (float)SC(s_d3d.height), 0.0f };
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 4, k, 0);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, g);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
    cl->lpVtbl->Close(cl);
    ID3D12CommandList* lists[1] = { (ID3D12CommandList*)cl };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, lists);
    const u64 v = ++s_blit_ctr;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_blit_fence, v);
    s_blit_fv[bi] = v;
    s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, (UINT)vs,
                                      (vs == 0 && s_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    s_vbb_index = (s_vbb_index + 1) % FRAME_COUNT;
}

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    /* Enable debug layer in debug builds */
    /* Debug layer: on in debug builds, or in any build when D3D12_DBG is set
     * (so a Release run can capture exact PSO/validation errors). */
    if (
#ifdef NDEBUG
        getenv("D3D12_DBG")
#else
        1
#endif
    ) {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        } else {
            printf("[D3D12] Debug layer requested but unavailable (0x%08lX) -- "
                   "install 'Graphics Tools' optional feature\n", hr);
        }
    }

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device. On a dual-GPU laptop a NULL adapter usually lands on
     * the integrated GPU; explicitly pick the high-performance (discrete)
     * adapter via IDXGIFactory6. Set CELLMARK_IGPU to force the low-power one
     * (for A/B testing a suspected iGPU-driver stall). */
    {
        IDXGIFactory6* factory6 = NULL;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory6, (void**)&factory6))) {
            DXGI_GPU_PREFERENCE pref = getenv("CELLMARK_IGPU")
                ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            IDXGIAdapter1* adapter = NULL;
            for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                     factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                     != DXGI_ERROR_NOT_FOUND; ai++) {
                DXGI_ADAPTER_DESC1 ad;
                adapter->lpVtbl->GetDesc1(adapter, &ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,
                        D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                        (void**)&s_d3d.device))) {
                    printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
                           (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                    adapter->lpVtbl->Release(adapter);
                    break;
                }
                adapter->lpVtbl->Release(adapter);
                adapter = NULL;
            }
            factory6->lpVtbl->Release(factory6);
        }
    }
    if (!s_d3d.device) {
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&s_d3d.device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
            factory->lpVtbl->Release(factory);
            return -1;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Create swap chain -- v133: sized to the window's client area (the guest
     * frame is scaled into it at present time), tearing allowed when the
     * system supports it (FreeSync / uncapped presents). */
    { IDXGIFactory5* f5 = NULL; BOOL allow = FALSE;
      if (SUCCEEDED(factory->lpVtbl->QueryInterface(factory, &IID_IDXGIFactory5, (void**)&f5)) && f5) {
          if (FAILED(f5->lpVtbl->CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
              allow = FALSE;
          f5->lpVtbl->Release(f5);
      }
      s_tearing = allow ? 1 : 0; }
    { RECT cr; if (GetClientRect(s_d3d.hwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
          s_sc_w = (u32)cr.right; s_sc_h = (u32)cr.bottom; }
      else { s_sc_w = width; s_sc_h = height; } }
    s_sc_flags = s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = s_sc_w;
    sc_desc.Height = s_sc_h;
    sc_desc.Flags = s_sc_flags;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    s_d3d.frame_index = 0; s_vbb_index = 0;
    fprintf(stderr, "[display] output %ux%u, guest frame %ux%u rendered at %ux%u, tearing/VRR presents %s%c",
            s_sc_w, s_sc_h, width, height, SC(width), SC(height), s_tearing ? "available" : "unavailable", 10);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    for (u32 i = 0; i < FRAME_COUNT; i++) {
        /* v133: the guest's "backbuffers" are our own textures, in the PRESENT
         * (= COMMON) state a swap-chain buffer starts in. */
        D3D12_HEAP_PROPERTIES vhp = {0}; vhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC vd = {0};
        vd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        vd.Width = SC(width); vd.Height = SC(height); vd.DepthOrArraySize = 1; vd.MipLevels = 1;
        vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; vd.SampleDesc.Count = 1;
        vd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        vd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &vhp, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_PRESENT, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) { printf("[D3D12] ERROR: virtual backbuffer (0x%08lX)\n", hr); return -1; }

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }
    if (blit_init() != 0) { printf("[D3D12] ERROR: present pass setup failed\n"); return -1; }

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        /* Sized to cover the LARGEST render target, not just the window:
         * D3D12 clips rasterization to the smallest bound attachment, and
         * this DSV is shared by every pass -- with a window-sized depth
         * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
         * image) were silently cropped to the window rect. */
        depth_desc.Width              = SC(2048);
        depth_desc.Height             = SC(2048);
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < (NPAR > FRAME_COUNT ? NPAR : FRAME_COUNT); i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = D3DCompile(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = D3DCompile(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = D3DCompile(vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, NULL, "main", "vs_5_0", 0, 0, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = D3DCompile(ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = SRV_HEAP_SIZE;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.srv_heap);
            if (FAILED(hr)) printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            else {
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC nv = {0};
                nv.Format = DXGI_FORMAT_R8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE hh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &hh);
                for (int _i = 0; _i < SRV_HEAP_SIZE; _i++) {
                    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &nv, hh);
                    hh.ptr += s_d3d.srv_inc;
                }
            }
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0 + static sampler s0), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* 4-descriptor SRV table (t0-t3) so decompiled fragment programs can
         * sample up to 4 texture units; the hardcoded atlas/colour PSs use only
         * t0 and are unaffected. Matching 4 static samplers s0-s3. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = RSX_UNITS;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[4] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;   /* t30 = GPU vertex fetch raw data */
        rp[3].Descriptor.ShaderRegister = 30;
        rp[3].Descriptor.RegisterSpace = 0;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC samp[RSX_UNITS * 2] = {0};
        /* v133: RSX_ANISO=2/4/8/16 = anisotropic texture filtering (sharper
         * ground and walls at an angle). 0/1 = trilinear as before. */
        static int aniso = -1;
        if (aniso < 0) { const char* e = getenv("RSX_ANISO"); aniso = e ? atoi(e) : 0;
                         if (aniso > 16) aniso = 16;
                         if (aniso > 1) fprintf(stderr, "[D3D12] anisotropic filtering %dx%c", aniso, 10); }
        for (int _s = 0; _s < RSX_UNITS; _s++) {
            samp[_s].Filter = aniso > 1 ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp[_s].MaxAnisotropy = aniso > 1 ? (UINT)aniso : 0;
            samp[_s].AddressU = samp[_s].AddressV = samp[_s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp[_s].ShaderRegister = (UINT)_s;
            samp[_s].MaxLOD = D3D12_FLOAT32_MAX;
            samp[_s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        /* s0 keeps point/clamp: the dbgfont atlas PS samples glyph cells and
         * linear filtering bleeds neighbouring glyphs. */
        /* Resistance port: ...but every guest fragment program samples its
         * DIFFUSE map through unit 0, and clamping it smeared the edge texels of
         * any tiled texture into long streaks (York's trench walls, the far
         * ground). Guest draws get wrap/linear on s0 too; RSX_S0_POINT=1
         * restores the dbgfont setting. */
        { const char* e = getenv("RSX_S0_POINT");
          if (e && atoi(e)) {
              samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
              samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
          } }

        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        /* v137: s16..s31 = the same samplers with CLAMP addressing (see
         * s_fp_clamp_mask). */
        for (int _s = 0; _s < RSX_UNITS; _s++) {
            samp[RSX_UNITS + _s] = samp[_s];
            samp[RSX_UNITS + _s].AddressU = samp[RSX_UNITS + _s].AddressV =
                samp[RSX_UNITS + _s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp[RSX_UNITS + _s].ShaderRegister = (UINT)(RSX_UNITS + _s);
        }
        rd.NumParameters = 4; rd.pParameters = rp;
        rd.NumStaticSamplers = RSX_UNITS * 2; rd.pStaticSamplers = samp;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        bd.Width = (u64)MAX_VERTICES * 256 * NPAR;  /* generic VP vertex: 16 float4 slots,
                                          * DOUBLE-buffered by frame parity so a
                                          * new frame's upload never overwrites
                                          * vertices the in-flight GPU frame is
                                          * still reading (was: torn/missing
                                          * triangles mixing stale+new verts) */
        /* Failure here leaves vp_vb_mapped NULL and every VP draw silently
         * declines to upload -- the scene just does not render, with no error
         * anywhere. Report the size too: this buffer is
         * MAX_VERTICES * 256 * 2 bytes and grows fast. */
        fprintf(stderr, "[VPVB] per-frame vertex buffer: %llu MB (%u verts, x2 parity)%c",
                (unsigned long long)(bd.Width >> 20), (unsigned)MAX_VERTICES, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);
        else
            fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);
        /* Resistance port: index buffer for deduplicated indexed draws. */
        { static int en = -1; if (en < 0) { const char* e = getenv("VP_INDEXED"); en = e ? atoi(e) : 1; }
          u64 w0 = bd.Width; bd.Width = (u64)VP_MAX_INDICES * 4 * NPAR;
          if (en && SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                  D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                  &IID_ID3D12Resource, (void**)&s_vp_ib))) {
              void* m = NULL; s_vp_ib->lpVtbl->Map(s_vp_ib, 0, &nr, &m); s_vp_ib_mapped = (u32*)m;
              if (!m) { s_vp_ib->lpVtbl->Release(s_vp_ib); s_vp_ib = NULL; }
          }
          bd.Width = w0; }
        /* Resistance port: GPU vertex fetch raw-data buffer, x2 parity. */
        if (gf_enabled()) {
          u64 w0 = bd.Width; bd.Width = (u64)GF_CAP * NPAR;
          if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                  D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                  &IID_ID3D12Resource, (void**)&s_gf_buf))) {
              void* m = NULL; s_gf_buf->lpVtbl->Map(s_gf_buf, 0, &nr, &m); s_gf_mapped = (u8*)m;
              if (!m) { s_gf_buf->lpVtbl->Release(s_gf_buf); s_gf_buf = NULL; }
          }
          fprintf(stderr, "[GPUFETCH] raw vertex buffer %s (%u MB x2)%c", s_gf_mapped ? "ready" : "FAILED", GF_CAP >> 20, 10);
          bd.Width = w0; }

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * NPAR;   /* per-draw constant slots, x2 parity */
        /* A failure here is silent and catastrophic: vp_record_cb returns early,
         * every draw loses its constants, and the whole scene transforms by
         * zeros -- indistinguishable from a broken vertex program. Say so. */
        fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%d draws)%c",
                (unsigned long long)(bd.Width >> 20), MAX_DRAWS, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)VP_FPCB_STRIDE * MAX_DRAWS * NPAR;  /* per-draw FP slots (b1), x2 parity */
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb)))
            s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);

        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

/* Resistance port: deferred release. A depth buffer recreated mid-frame (the
 * game reuses one address for surfaces of different sizes within a frame) may
 * still be referenced by draws already RECORDED in the open command list --
 * wait_for_gpu() only covers work already submitted. Releasing it then is a
 * use-after-free on the GPU: DXGI_ERROR_DEVICE_REMOVED, a frozen picture while
 * the game runs on. Dead resources wait here until a submission that recorded
 * them has completed. */
static u32 s_submit_no;
static ID3D12Resource* s_grave[512];
static u32 s_grave_stamp[512];
static int s_grave_n;
static void grave_defer(ID3D12Resource* r)
{
    if (!r) return;
    if (s_grave_n < 512) { s_grave[s_grave_n] = r; s_grave_stamp[s_grave_n] = s_submit_no; s_grave_n++; }
    /* full: leak it rather than free something the GPU may still read */
}
/* Resistance port: one monotonic fence for everything, and per-batch fence
 * values, so a batch can be waited for individually. s_done_no = highest batch
 * number known complete (batches are numbered by s_submit_no after their
 * ExecuteCommandLists). */
static u64 s_fence_ctr;
static u64 s_batch_fence[8];
static u32 s_done_no;
static void grave_flush(void)
{
    int k = 0;
    for (int i = 0; i < s_grave_n; i++) {
        if (s_done_no > s_grave_stamp[i]) s_grave[i]->lpVtbl->Release(s_grave[i]);
        else { s_grave[k] = s_grave[i]; s_grave_stamp[k] = s_grave_stamp[i]; k++; }
    }
    s_grave_n = k;
}
/* Wait until the fence reaches v. Bounded: a device removal (TDR) leaves the
 * fence unsignaled forever, and this thread also drives the guest's vblank
 * and FIFO -- degrade loudly instead of freezing the emulation. */
static int fence_wait_(u64 v, const char* who);
static int fence_wait(u64 v, const char* who)
{
    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) >= v) return 1;
    double _t = perf_on() ? perf_now() : 0.0;
    int r = fence_wait_(v, who);
    if (perf_on()) s_perf_wait += perf_now() - _t;
    return r;
}
static int fence_wait_(u64 v, const char* who)
{
    s_d3d.fence->lpVtbl->SetEventOnCompletion(s_d3d.fence, v, s_d3d.fence_event);
    for (int tries = 0; tries < 3; tries++) {
        if (WaitForSingleObject(s_d3d.fence_event, 2000) != WAIT_TIMEOUT) return 1;
        HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
        printf("[D3D12] %s STUCK %ds: want %llu got %llu removed=0x%08lX\n", who,
               2 * (tries + 1), (unsigned long long)v,
               (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence), (long)rr);
        fflush(stdout);
        if (rr != 0) break;
    }
    return 0;
}
static void wait_for_gpu(void)
{
    const u64 v = ++s_fence_ctr;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, v);
    if (fence_wait(v, "wait_for_gpu")) { s_done_no = s_submit_no; grave_flush(); }
}
/* Batch b (an s_submit_no value) has finished on the GPU. */
static void wait_batch(u32 b)
{
    if (b <= s_done_no || b > s_submit_no) return;   /* done, or not submitted yet */
    if (b + 8 <= s_submit_no) return;                  /* fence slot reused: wait_batch on a newer one covers it */
    if (fence_wait(s_batch_fence[b & 7], "wait_batch")) { s_done_no = b; grave_flush(); }
}
/* Resistance port: RSX_PIPELINE (default 1) lets the CPU record batch N+1
 * while the GPU runs batch N, instead of draining the GPU after every batch.
 * Fall of Man's frame is ~5 mid-frame batches, and the full sync after each
 * left the FIFO thread waiting on the GPU about half its time -- and the PPU,
 * which waits for the RSX at the end of every frame, waiting with it. Per-batch
 * resources (vertex/index/constant halves, command allocator, descriptor
 * windows) are split by vp_parity, so only batch N-1 must be done before
 * N+1 starts reusing its half. =0 restores the full sync. */
static int rsx_pipeline(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_PIPELINE"); e = v ? atoi(v) : 1; }
    return e;
}
static void move_to_next_frame(void)
{
    const u64 v = ++s_fence_ctr;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, v);
    s_batch_fence[s_submit_no & 7] = v;          /* the batch just executed */
    s_d3d.frame_index = s_vbb_index;   /* v133: advanced by present_blit, as the swap chain did */
    if (rsx_pipeline()) {
        if (s_submit_no >= (u32)(NPAR - 1)) wait_batch(s_submit_no - (u32)(NPAR - 1));   /* its third is reused next */
    } else if (fence_wait(v, "move_to_next_frame")) { s_done_no = s_submit_no; grave_flush(); }
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
/* Name prefix for the next readback dump: lets the same writer emit the
 * backbuffer and the screen-copy texture under different filenames. */
static const char* s_dump_name = NULL;

static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)s_d3d.readback_pitch * s_d3d.height};
    if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &rr, &mapped)) || !mapped)
        return;

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    snprintf(path, sizeof(path), "%s%s%s_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", s_dump_name ? s_dump_name : "frame", idx++);
    FILE* f = fopen(path, "wb");
    if (f) {
        u32 w = s_d3d.width, h = s_d3d.height;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u)\n", path, s_d3d.width, s_d3d.height);
    }
    D3D12_RANGE wr = {0, 0};
    s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr);
}

/* ---------------------------------------------------------------------------
 * RSX frame capture ("rsxcap") -- a purpose-built GPU debugger. RSX_CAP=start
 * [:count[:stride]] snapshots whole frames to <RSX_CAP_DIR>/frame_NN/: a text
 * manifest of every op (in submission order, with full surface/shader/texture
 * state) plus a BMP of the backbuffer and every offscreen colour RT used that
 * frame. One frame, captured atomically -- no cross-frame guessing.
 * -----------------------------------------------------------------------*/

/* Shared 24-bit BMP writer for readbacks. R8G8B8A8_UNORM straight; the half/
 * float RT formats are |v|-tonemapped so signed/HDR data is visible. */
static void cap_write_bmp(const char* path, const void* mapped, u32 w, u32 h,
                          u32 pitch, u32 dxgi)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 rowb = (w * 3 + 3) & ~3u, datasz = rowb * h;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54; *(u32*)(hdr+14) = 40;
    *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
    *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)malloc(rowb);
    if (line) for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
        const u8* srow = (const u8*)mapped + (u64)y * pitch;
        memset(line, 0, rowb);
        for (u32 x = 0; x < w; x++) {
            float rv, gv, bv;
            if (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const u16* hp = (const u16*)(srow + (u64)x * 8);
                float v[3];
                for (int c = 0; c < 3; c++) {
                    u16 hv = hp[c]; u32 s = (hv>>15)&1, e = (hv>>10)&0x1F, m = hv&0x3FF; float fv;
                    if (e == 0) fv = (float)m / 16777216.0f;
                    else { u32 fb = (s<<31)|((e-15+127)<<23)|(m<<13); memcpy(&fv,&fb,4); }
                    v[c] = fv;
                }
                rv = v[0]; gv = v[1]; bv = v[2];
            } else if (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                const float* fp = (const float*)(srow + (u64)x * 16);
                rv = fp[0]; gv = fp[1]; bv = fp[2];
            } else {
                const u8* p = srow + (u64)x * 4;
                rv = p[0]/255.0f; gv = p[1]/255.0f; bv = p[2]/255.0f;
            }
            float ar = rv<0?-rv:rv, ag = gv<0?-gv:gv, ab = bv<0?-bv:bv;
            if (ar>1) ar=1; if (ag>1) ag=1; if (ab>1) ab=1;
            line[x*3+0] = (u8)(ab*255.0f);
            line[x*3+1] = (u8)(ag*255.0f);
            line[x*3+2] = (u8)(ar*255.0f);
        }
        fwrite(line, 1, rowb, f);
    }
    free(line);
    fclose(f);
}

/* Copy one GPU resource into a fresh readback buffer and write it as a BMP.
 * Self-contained: waits, resets the frame command list, copies, executes,
 * waits, maps. `prior` is the resource's current tracked state (restored after
 * the copy so the caller's state tracking stays valid). */
static void cap_readback_write(ID3D12Resource* res, u32 w, u32 h, u32 dxgi,
                               D3D12_RESOURCE_STATES prior, u32 fi, const char* path)
{
    if (!res || !w || !h) return;
    /* v179: the resource's real size -- at RSX_RES_SCALE != 1 it is larger than
     * the guest size passed in, the whole-subresource copy below then failed
     * and no BMP was written. */
    { D3D12_RESOURCE_DESC rdsc; res->lpVtbl->GetDesc(res, &rdsc);
      if (rdsc.Width && rdsc.Height) { w = (u32)rdsc.Width; h = rdsc.Height; } }
    u32 bpp = (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
              (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
    u32 pitch = (w * bpp + 255) & ~255u;

    ID3D12Resource* rb = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (u64)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&rb)) || !rb)
        return;

    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = prior;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)dxgi;
    dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        /* TEXUP_DBG=1: texture uploads per second. A cache that misses every
         * draw re-uploads megabytes per frame, which is the kind of GPU work
         * that trips a 2-second TDR while looking like nothing in a draw count. */
        { static int td = -1; if (td < 0) td = getenv("TEXUP_DBG") ? 1 : 0;
          if (td) { static long long n = 0, bytes = 0; static clock_t t0 = 0;
              n++; bytes += (long long)pitch * h;
              const clock_t now = clock();
              if (!t0) t0 = now;
              if (now - t0 >= CLOCKS_PER_SEC) {
                  fprintf(stderr, "[TEXUP] %lld uploads/s, %.1f MB/s\n",
                          n, bytes / 1048576.0);
                  n = 0; bytes = 0; t0 = now; } } }

    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = prior;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    s_submit_no++;
    wait_for_gpu();

    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        if (strstr(path, ".raw")) {   /* v184: raw dump (FSR motion / depth) */
            FILE* rf = fopen(path, "wb");
            if (rf) { const u32 hd[4] = { w, h, pitch, dxgi }; fwrite(hd, 4, 4, rf); fwrite(mp, 1, (size_t)pitch * h, rf); fclose(rf); }
        } else
        cap_write_bmp(path, mp, w, h, pitch, dxgi);
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    rb->lpVtbl->Release(rb);
}

/* Resistance port: a frame split by mid-frame flushes is several batches; the
 * capture used to see only the last. Earlier batches of a captured frame now
 * write manifest_b<N>.txt (manifest only, no images) into the same folder. */
static int s_cap_manifest_only = 0, s_cap_batch_n = 0;
static void rsx_capture_frame(u32 fi, u32 ndraws, u32 capidx)
{
    char dir[300]; const char* base = getenv("RSX_CAP_DIR");
    if (!base || !*base) base = "rsxcap";
    CreateDirectoryA(base, NULL);
    snprintf(dir, sizeof dir, "%s/frame_%02u", base, capidx);
    CreateDirectoryA(dir, NULL);
    char path[512];

    /* Manifest: every op in submission order with full state. */
    if (s_cap_manifest_only) snprintf(path, sizeof path, "%s/manifest_b%d.txt", dir, s_cap_batch_n);
    else snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE* mf = fopen(path, "w");
    if (mf) {
        fprintf(mf, "frame_count=%llu draws=%u backbuffer=%ux%u\n",
                (unsigned long long)s_d3d.frame_count, ndraws, s_d3d.width, s_d3d.height);
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].used)
                fprintf(mf, "offrt[%d] off=0x%08X %ux%u dxgi=%u\n", i, s_d3d.off_rt[i].off,
                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi);
        for (int i = 0; i < s_d3d.vp_vs_n; i++)
            fprintf(mf, "vs[%d] hash=%08X\n", i, s_d3d.vp_vs[i].hash);
        fprintf(mf, "--- ops ---\n");
        for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* r = &T_DRAWS[d];
            if (r->is_clear)
                fprintf(mf, "op%03u CLEAR rt=0x%08X mrt=0x%X/0x%X/0x%X cc=(%.3f,%.3f,%.3f,%.3f)\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->cc[0], r->cc[1], r->cc[2], r->cc[3]);
            else {
                fprintf(mf, "op%03u DRAW  rt=0x%08X mrt=0x%X/0x%X/0x%X vp=%u,%u,%ux%u fp=0x%08X vs=%d "
                            "n=%u cmask=%X blend=%d bk=0x%X t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->vp_x, r->vp_y, r->vp_w, r->vp_h,
                        r->fp_addr, r->vs_idx, r->vertex_count, r->cmask, r->blend, r->blend_key,
                        r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw);
                /* v182 diag: depth state (cull key: bit9 test, bit10 write, bits12-14 func, 16-23 poly
                 * offset idx, 24-31 stencil) and the VS epilogue z offset this draw got. */
                { float zo = 0.0f;
                  if (s_d3d.vp_cb_mapped) {
                      const u32 par = (u32)((T_PARITY + NPAR - 1) % NPAR);
                      memcpy(&zo, (const u8*)s_d3d.vp_cb_mapped + ((u64)par * MAX_DRAWS + r->cb_slot) * VP_CB_STRIDE + 8192 + 24, 4); }
                  fprintf(mf, "      depth: cull=0x%08X test=%u write=%u func=%u zoff=%g\n", r->cull,
                          (r->cull >> 9) & 1u, (r->cull >> 10) & 1u, (r->cull >> 12) & 7u, zo); }
                for (int u = 0; u < RSX_UNITS; u++)
                    if (r->tex[u].set)
                        fprintf(mf, "      t%d off=0x%08X fmt=0x%02X (%s%s) %ux%u remap=0x%04X\n", u,
                                r->tex[u].off, r->tex[u].fmt,
                                (r->tex[u].fmt & 0x20) ? "LN" : "SZ",
                                (r->tex[u].fmt & 0x40) ? ",NR" : "",
                                r->tex[u].w, r->tex[u].h, r->tex[u].ctrl1 & 0xFFFF);
                /* v180 diag: GPU-fetch draws -- the attribute table and the
                 * first 4 vertices exactly as the GPU will fetch them. */
                if (r->gf && s_d3d.vp_cb_mapped && s_gf_mapped && s_vp_ib_mapped && r->ib_count) {
                    const u32 par = (u32)((T_PARITY + NPAR - 1) % NPAR);   /* render_frame_impl advances T_PARITY before the capture */
                    const u8* cb = (const u8*)s_d3d.vp_cb_mapped + ((u64)par * MAX_DRAWS + r->cb_slot) * VP_CB_STRIDE + GF_CB_OFF;
                    u32 ta[16][4]; float tc[16][4]; u32 xb[4];
                    memcpy(ta, cb, sizeof ta); memcpy(tc, cb + 256, sizeof tc); memcpy(xb, cb + 512, 16);
                    const u8* half = s_gf_mapped + (size_t)par * GF_CAP;
                    const u32* ib = s_vp_ib_mapped + (size_t)par * VP_MAX_INDICES + r->ib_first;
                    fprintf(mf, "      gf base=%u idx=%u,%u,%u,%u,%u,%u\n", xb[0], ib[0], ib[1], ib[2],
                            r->ib_count > 3 ? ib[3] : 0, r->ib_count > 4 ? ib[4] : 0, r->ib_count > 5 ? ib[5] : 0);
                    for (int a2 = 0; a2 < 4; a2++) {
                        if (!(ta[a2][2] & 0x10000u)) {
                            fprintf(mf, "      a%d const (%g %g %g %g)\n", a2, tc[a2][0], tc[a2][1], tc[a2][2], tc[a2][3]);
                            continue;
                        }
                        const u32 ty = ta[a2][2] & 0xFF, nn = (ta[a2][2] >> 8) & 0xFF;
                        fprintf(mf, "      a%d off=%u stride=%u type=%u n=%u:", a2, ta[a2][0], ta[a2][1], ty, nn);
                        for (int v = 0; v < 4 && v < 6; v++) {
                            const u64 o = (u64)ta[a2][0] + (u64)(ib[v] + xb[0]) * ta[a2][1];
                            if (o + 16 > (u64)GF_CAP) { fprintf(mf, " [oob]"); continue; }
                            fprintf(mf, " (");
                            for (u32 c = 0; c < nn && c < 4; c++) {
                                if (ty == 2) { u32 w; memcpy(&w, half + o + c * 4, 4); w = _byteswap_ulong(w);
                                    float fv; memcpy(&fv, &w, 4); fprintf(mf, "%s%g", c ? " " : "", fv); }
                                else fprintf(mf, "%s%02X", c ? " " : "", half[o + c]);
                            }
                            fprintf(mf, ")");
                        }
                        fprintf(mf, "\n");
                    }
                }
            }
        }
        /* Per-draw VP constant banks for the first few offscreen-RT geometry
         * draws: c[0..3] is the projection row-major for most CG VPs; also scan
         * for the largest non-zero slot so a mis-placed MVP is obvious. Records
         * live at slot==draw-index; parity is post-^1 here (capture is after the
         * frame's vp_parity flip), so read the other half. */
        /* Resistance port: for the first draw of every fragment program, its
         * vertex constants c[0..47] and the first three vertices as the VS
         * receives them (attributes 0..7), so a wrong input or constant can
         * be read straight off the capture. */
        if (s_d3d.vp_cb_mapped && s_d3d.vp_vb_mapped) {
            u32 par = (u32)((T_PARITY + NPAR - 1) % NPAR);
            u32 seen[256]; int ns = 0;
            fprintf(mf, "--- per-fp first draw: constants + vertices ---\n");
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* r = &T_DRAWS[d];
                if (r->is_clear || !r->fp_addr || !r->is_vp) continue;
                int f = 0; for (int i = 0; i < ns; i++) if (seen[i] == r->fp_addr) f = 1;
                if (f || ns >= 256) continue;
                seen[ns++] = r->fp_addr;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * MAX_DRAWS + r->cb_slot) * VP_CB_STRIDE);
                fprintf(mf, "fp=0x%08X op%03u vs=%d n=%u ib=%u\n", r->fp_addr, d, r->vs_idx, r->vertex_count, r->ib_count);
                for (int k = 0; k < 128; k++) {   /* v163: was 48 */
                    const float* v = c + k * 4;
                    if (v[0] || v[1] || v[2] || v[3])
                        fprintf(mf, "   c[%2d] = %12.6g %12.6g %12.6g %12.6g\n", k, v[0], v[1], v[2], v[3]);
                }
                const float* vb = (const float*)((const u8*)s_d3d.vp_vb_mapped
                    + (u64)par * MAX_VERTICES * 256u + r->vb_byte_offset);
                for (u32 vi = 0; vi < 3 && vi < r->vertex_count; vi++)
                    for (int a = 0; a < 8; a++) {
                        const float* v = vb + (vi * 16 + a) * 4;
                        fprintf(mf, "   v%u.a%d = %12.6g %12.6g %12.6g %12.6g\n", vi, a, v[0], v[1], v[2], v[3]);
                    }
            }
        }
        /* v162d: every non-indexed blended draw (particles): attributes 1..3 of vertices 0 and 3 */
        if (s_d3d.vp_vb_mapped) {
            u32 par = (u32)((T_PARITY + NPAR - 1) % NPAR);
            fprintf(mf, "--- particle-like draws (no index buffer, blended): v0/v3 a1..a3 ---\n");
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* r = &T_DRAWS[d];
                if (r->is_clear || !r->is_vp || r->ib_count || !r->blend) continue;
                const float* vb = (const float*)((const u8*)s_d3d.vp_vb_mapped
                    + (u64)par * MAX_VERTICES * 256u + r->vb_byte_offset);
                fprintf(mf, "op%03u fp=0x%08X n=%u t0=0x%08X", d, r->fp_addr, r->vertex_count, r->tex[0].raw);
                for (u32 vi = 0; vi < 4 && vi < r->vertex_count; vi += 3)
                    for (int a2 = 1; a2 <= 3; a2++) { const float* v = vb + (vi * 16 + a2) * 4;
                        fprintf(mf, " v%u.a%d=(%.3g %.3g %.3g %.3g)", vi, a2, v[0], v[1], v[2], v[3]); }
                fprintf(mf, "\n");
            }
        }
        if (s_d3d.vp_cb_mapped) {
            u32 par = (u32)((T_PARITY + NPAR - 1) % NPAR);
            fprintf(mf, "--- vp constants (first offscreen geometry draws) ---\n");
            int shown = 0;
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS && shown < 4; d++) {
                const D3D12DrawRecord* r = &T_DRAWS[d];
                if (r->is_clear || !r->rt_off) continue;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * MAX_DRAWS + d) * VP_CB_STRIDE);
                int last_nz = -1; float maxabs = 0.0f;
                for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++) {
                    float a = c[i] < 0 ? -c[i] : c[i];
                    if (a > 1e-9f) last_nz = i;
                    if (a > maxabs) maxabs = a;
                }
                fprintf(mf, "op%03u vs=%d lastNZslot=%d maxabs=%.3f\n",
                        d, r->vs_idx, last_nz / 4, maxabs);
                if (shown == 0) {   /* full non-zero slot dump for the 1st draw */
                    for (int s = 0; s <= last_nz / 4; s++) {
                        const float* v = c + s * 4;
                        if ((v[0]||v[1]||v[2]||v[3]))
                            fprintf(mf, "   c[%3d] = %9.4f %9.4f %9.4f %9.4f\n",
                                    s, v[0], v[1], v[2], v[3]);
                    }
                }
                shown++;
            }
        }
        fclose(mf);
    }
    if (s_cap_manifest_only) return;
    {
    }

    /* Backbuffer (in PRESENT state at capture time) + every used offscreen RT. */
    snprintf(path, sizeof path, "%s/backbuffer.bmp", dir);
    cap_readback_write(s_d3d.render_targets[fi], s_d3d.width, s_d3d.height,
                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, fi, path);
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->used) continue;
        snprintf(path, sizeof path, "%s/rt_%08X.bmp", dir, r->off);
        cap_readback_write(r->res, r->w, r->h, r->dxgi, r->st, fi, path);
    }
    /* v184: FSR motion vectors + depth of the last scene */
    { extern void fsr_cap_dump(const char* dir, u32 fi, u32 nd); fsr_cap_dump(dir, fi, ndraws); }
    printf("[RSXCAP] frame %llu -> %s (%u ops)\n",
           (unsigned long long)s_d3d.frame_count, dir, ndraws);
}

/* Decompile the captured RSX vertex program to HLSL and build the VP PSO
 * (decompiled VS + atlas alpha-test PS). One-shot per program. */
static void compile_vp(void)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || st->vp_ucode_bytes < 16 || !s_d3d.vp_root_sig) return;

    static char hlsl[64 * 1024];
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    /* Does this VP read vp_c[0..3]? Gates the garbage-projection fallback:
     * programs keeping their MVP elsewhere (gcm/cube: c[256..259]) must not
     * have c[0..3] stomped nor the viewport z-lane overridden. */
    s_d3d.vp_uses_c03 = (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
                         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    if (ni <= 0) { printf("[VP] decompile failed (%d)\n", ni); s_d3d.vp_compiled_bytes = st->vp_ucode_bytes; return; }
    if (getenv("VP_DUMP")) {
        FILE* vf = fopen("vp_dump.hlsl", "w");
        if (vf) { fwrite(hlsl, 1, strlen(hlsl), vf); fclose(vf);
                  printf("[VP] dumped vp_dump.hlsl (%d instrs)\n", ni); }
        printf("[VPRAW] first 3 instrs (%u ucode bytes):\n", st->vp_ucode_bytes);
        for (u32 _q = 0; _q < 48 && _q < st->vp_ucode_bytes; _q += 16)
            printf("[VPRAW]  d0=%02X%02X%02X%02X d1=%02X%02X%02X%02X d2=%02X%02X%02X%02X d3=%02X%02X%02X%02X\n",
                st->vp_ucode[_q+0],st->vp_ucode[_q+1],st->vp_ucode[_q+2],st->vp_ucode[_q+3],
                st->vp_ucode[_q+4],st->vp_ucode[_q+5],st->vp_ucode[_q+6],st->vp_ucode[_q+7],
                st->vp_ucode[_q+8],st->vp_ucode[_q+9],st->vp_ucode[_q+10],st->vp_ucode[_q+11],
                st->vp_ucode[_q+12],st->vp_ucode[_q+13],st->vp_ucode[_q+14],st->vp_ucode[_q+15]);
    }

    /* Pixel shader mirrors dbgfont's FP: sample the atlas coverage at TEXCOORD0
     * (VP output o[7]), alpha-test at 0.5, output the vertex color (o[1]). */
    static const char ps[] =
        "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
        "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
        "float4 main(PSIn i):SV_TARGET{ float cov = tex.Sample(smp, i.t0.xy).r;\n"
        "  if (cov <= 0.5) discard; return float4(i.col0.rgb, 1); }\n";

    ID3DBlob *vb=NULL,*pb=NULL,*e=NULL;
    HRESULT hr = shc_compile(hlsl, strlen(hlsl), "vp", "main", "vs_5_0", &vb, &e);
    if (FAILED(hr)) { printf("[VP] VS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }
    hr = D3DCompile(ps, sizeof(ps)-1, "vpps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr)) { printf("[VP] PS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); if(vb)vb->lpVtbl->Release(vb); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }

    /* The decompiled VS declares inputs a0:ATTR0 .. a15:ATTR15 and the HLSL
     * compiler keeps whichever the program body reads. D3D12 requires EVERY VS
     * input to have a matching input-layout element (by semantic name+index),
     * so declare all 16 ATTR slots. (The old single "POSITION" element matched
     * NO VS input once the decompiler switched to ATTR semantics -> PSO failed
     * with E_INVALIDARG for every VP, blanking cellmark's text and vkcube.)
     * The VP path uploads only attrib0 to vp_vb (one float4/vertex), so every
     * slot reads that same float4 at offset 0; attributes other than position
     * therefore alias attrib0 for now (colours/uv wrong until the VP path
     * uploads multiple attributes), but geometry is correct and the PSO is
     * valid. */
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _e = 0; _e < 16; _e++) {
        il[_e].SemanticName = "ATTR";
        il[_e].SemanticIndex = _e;
        il[_e].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[_e].InputSlot = 0;
        /* 256-byte generic VP vertex: every ATTRi is a float4 slot at i*16
         * (read_vp_vertex converts each enabled RSX attrib by type; disabled
         * slots hold (0,0,0,1)). Covers tiny3d (a0/a3/a8), SDK gcm samples
         * (a0/a1/a2), dbgfont -- no aliasing. */
        il[_e].AlignedByteOffset = (UINT)(_e * 16);
        il[_e].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_e].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Depth test ON (LESS_EQUAL, matching the guest's rsxtiny_DepthTestFunc 515 /
     * CELL_GCM_LEQUAL). Without it the cube's faces drew in submission order and
     * back faces overwrote the front -> you saw through the near face to the
     * darker interior. The depth buffer is bound + cleared to 1.0 each frame. */
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_vp);

    /* Second PSO: same VS but a COLOUR-only PS (no texture sample, no alpha
     * discard) for untextured 3D geometry (vkcube's cube). The atlas PS above
     * samples a texture and discards where coverage<=0.5, which blanks any draw
     * with no atlas bound. */
    {
        static const char ps_col[] =
            "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
            "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
            "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
            "float4 main(PSIn i):SV_TARGET{ return float4(i.col0.rgb, 1); }\n";
        ID3DBlob *pcb=NULL,*ec=NULL;
        if (SUCCEEDED(D3DCompile(ps_col, sizeof(ps_col)-1, "vppsc", NULL, NULL,
                                 "main", "ps_5_0", 0, 0, &pcb, &ec)) && pcb) {
            pd.PS.pShaderBytecode = pcb->lpVtbl->GetBufferPointer(pcb);
            pd.PS.BytecodeLength  = pcb->lpVtbl->GetBufferSize(pcb);
            HRESULT hr2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)&s_d3d.pipeline_state_vp_color);
            printf(SUCCEEDED(hr2) ? "[VP] colour pipeline ready\n"
                                  : "[VP] colour PSO FAIL (0x%08lX)\n", hr2);
            pcb->lpVtbl->Release(pcb);
        }
        if (ec) ec->lpVtbl->Release(ec);
    }
    /* Keep the VS bytecode for guest-FP PSO builds (vp_get_fp_pso); bump the
     * generation so cached FP PSOs built against the old VS are rebuilt. */
    if (s_d3d.vp_vs_blob) s_d3d.vp_vs_blob->lpVtbl->Release(s_d3d.vp_vs_blob);
    s_d3d.vp_vs_blob = vb;
    s_d3d.vp_gen++;
    pb->lpVtbl->Release(pb);
    s_d3d.vp_compiled_bytes = st->vp_ucode_bytes;
    if (SUCCEEDED(hr)) { s_d3d.vp_ready = 1; printf("[VP] pipeline ready (%d instrs)\n", ni); }
    else {
        printf("[VP] PSO creation FAIL (0x%08lX)\n", hr);
        /* Drain the debug layer's message queue to get the EXACT validation
         * reason (E_INVALIDARG is otherwise opaque). One-shot. */
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            for (UINT64 mi = 0; mi < n; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)))
                    printf("[VP][DBG] %s\n", m->pDescription);
                free(m);
            }
            iq->lpVtbl->Release(iq);
        }
    }
}

/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static void rt_wait_idle(void);
static int s_vsm_idx = -1; static u32 s_vsm_gen, s_vsm_start, s_vsm_bytes;
static const rsx_state* s_vsm_st;
static int vp_get_vs_uncached(const rsx_state* st);
/* Resistance port: memoised. Every draw looked its VS up by hashing the whole
 * program (twice with the attribute mask below); the program only changes when
 * the guest writes microcode, which bumps vp_ucode_gen. VP_VSMEMO=0 off. */
static int vp_get_vs(const rsx_state* st)
{
    static int en = -1; if (en < 0) { const char* e = getenv("VP_VSMEMO"); en = e ? atoi(e) : 1; }
    if (!st) return -1;
    if (en && s_vsm_idx >= 0 && s_vsm_st == st && s_vsm_gen == st->vp_ucode_gen &&
        s_vsm_start == st->transform_program_start && s_vsm_bytes == st->vp_ucode_bytes &&
        s_vsm_idx < s_d3d.vp_vs_n)
        return s_vsm_idx;
    int r = vp_get_vs_uncached(st);
    s_vsm_idx = r; s_vsm_st = st; s_vsm_gen = st->vp_ucode_gen;
    s_vsm_start = st->transform_program_start; s_vsm_bytes = st->vp_ucode_bytes;
    return r;
}
/* ===========================================================================
 * Resistance port: GPU vertex fetch ("GF").
 *
 * The VP path converted every vertex on the CPU into 16 float4 slots (256
 * bytes) and uploaded that: in York's opening area the RSX thread spent ~66 of
 * every ~69 ms frame on it (plus the worker pool and its malloc/lock traffic),
 * while the GPU idled. Instead, for indexed triangle lists, the guest's RAW
 * vertex bytes are copied (one memcpy per contiguous range, big-endian as
 * they are) into a per-frame raw buffer, the guest indices go straight into
 * the index buffer, and the vertex shader fetches and converts each attribute
 * itself from a ByteAddressBuffer (root SRV t30) using a per-draw
 * attribute table in the VP constant slot. Anything unusual -- instanced or
 * immediate attributes, primitive restart, an index range that wraps or is
 * too sparse, a shader variant that did not compile -- takes the old path.
 * RSX_GPUFETCH=0 disables.
 * =========================================================================*/
static u32 gf_elem_size(u32 type, u32 n)
{
    switch (type) {
    case 2: return 4u * n;          /* float */
    case 3: return 2u * n;          /* half */
    case 4: case 7: return n;       /* UB, UB256 */
    case 1: case 5: return 2u * n;  /* S1, S32K */
    case 6: return 4u;              /* CMP 11:11:10 */
    }
    return 0;
}

typedef struct { int space; u64 s, e; } GFRange;

/* Decode `count` guest indices at `first` into ib[]; range into *plo/*phi
 * (merged with what they hold). 0 = a primitive-restart index was hit. */
static int gf_decode(const rsx_state* st, u32 first, u32 count, u32* ib, u32* plo, u32* phi)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    const int local = ((st->index_array_dma & 0xF) == 0);
    const u32 r = cellGcmResolveLocated(local, st->index_array_offset);
    if (r == 0xFFFFFFFFu) return 0;
    const u8* ip = vm_base + r;
    const int is16 = ((st->index_array_dma >> 4) & 0xF) == 1;
    const int rs = st->restart_index_enable && !(is16 && st->restart_index > 0xFFFFu);
    const u32 rv = st->restart_index;
    u32 lo = *plo, hi = *phi;
    if (is16) {
        const u16* p = (const u16*)(ip + (u64)first * 2);
        u32 k = 0;
        /* v190: 8 indices at a time (byte swap, restart check, min/max). It was
         * ~8% of the FIFO thread in a Titan fight. A vector holding the restart
         * index drops to the scalar loop, which stops on it exactly as before. */
        if (count >= 8) {
            const __m128i vrv = _mm_set1_epi16((short)rv), flip = _mm_set1_epi16((short)0x8000), z = _mm_setzero_si128();
            __m128i vmn = _mm_set1_epi16(0x7FFF), vmx = _mm_set1_epi16((short)0x8000);
            for (; k + 8u <= count; k += 8u) {
                __m128i x = _mm_loadu_si128((const __m128i*)(p + k));
                x = _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8));
                if (rs && _mm_movemask_epi8(_mm_cmpeq_epi16(x, vrv))) break;
                const __m128i xf = _mm_xor_si128(x, flip);
                vmn = _mm_min_epi16(vmn, xf); vmx = _mm_max_epi16(vmx, xf);
                _mm_storeu_si128((__m128i*)(ib + k), _mm_unpacklo_epi16(x, z));
                _mm_storeu_si128((__m128i*)(ib + k + 4), _mm_unpackhi_epi16(x, z));
            }
            if (k) {
                vmn = _mm_min_epi16(vmn, _mm_shuffle_epi32(vmn, 0x4E)); vmx = _mm_max_epi16(vmx, _mm_shuffle_epi32(vmx, 0x4E));
                vmn = _mm_min_epi16(vmn, _mm_shuffle_epi32(vmn, 0xB1)); vmx = _mm_max_epi16(vmx, _mm_shuffle_epi32(vmx, 0xB1));
                vmn = _mm_min_epi16(vmn, _mm_srli_si128(vmn, 2));       vmx = _mm_max_epi16(vmx, _mm_srli_si128(vmx, 2));
                const u32 mn = ((u32)_mm_extract_epi16(vmn, 0) ^ 0x8000u) & 0xFFFFu, mx = ((u32)_mm_extract_epi16(vmx, 0) ^ 0x8000u) & 0xFFFFu;
                if (mn < lo) lo = mn; if (mx > hi) hi = mx;
            }
        }
        for (; k < count; k++) {
            const u32 x = (u32)(u16)((p[k] >> 8) | (p[k] << 8));
            if (rs && x == rv) return 0;
            ib[k] = x; if (x < lo) lo = x; if (x > hi) hi = x;
        }
    } else {
        const u32* p = (const u32*)(ip + (u64)first * 4);
        for (u32 k = 0; k < count; k++) {
            const u32 x = _byteswap_ulong(p[k]);
            if (rs && x == rv) return 0;
            ib[k] = x; if (x < lo) lo = x; if (x > hi) hi = x;
        }
    }
    *plo = lo; *phi = hi;
    return 1;
}

/* The open GF record. Its vertex bytes are copied only when the chain of
 * batches that feeds it ends (gf_finalize): the game sends one mesh as a
 * chain of 256-index DRAW_INDEX_ARRAY batches inside one BEGIN/END, and as
 * separate records every batch paid a draw call, a constant-slot copy and a
 * pipeline lookup (~15k records a York frame instead of ~5k). */
static struct {
    int active; u32 seq;
    u32 rec, parity; const D3D12DrawRecord* tbl;
    u32 lo, hi, base, ib_end, epoch, fp;
    int na; int ai[16]; u64 off0[16]; u32 stride[16], esz[16]; int space[16];
    u32 tab_a[16][4]; float tab_c[16][4];
} s_gfp;
int g_drawlog_left = 0;   /* v180 diag */
static u32 s_gf_seq;

/* Would the vertex range [lo, hi] be copyable? (wrap, local 256 MB, size, room) */
static int gf_range_ok(u32 lo, u32 hi)
{
    if ((u64)hi + s_gfp.base > 0xFFFFFu) return 0;
    const u64 eLo = (u64)lo + s_gfp.base, eHi = (u64)hi + s_gfp.base;
    u64 need = 0;
    for (int k = 0; k < s_gfp.na; k++) {
        const u64 s = s_gfp.off0[k] + eLo * s_gfp.stride[k], e = s_gfp.off0[k] + eHi * s_gfp.stride[k] + s_gfp.esz[k];
        if (s_gfp.space[k] && e > 0x10000000ull) return 0;
        if (e - s > (32u << 20)) return 0;
        need += (e - s) + 32;
    }
    return (u64)s_gf_off + need <= GF_CAP;
}

/* First batch of a GF record: plan + indices (no vertex copy yet). */
/* v145 diag: why a triangle draw left the GPU-fetch path (index = the
 * n-th early exit of gf_begin, in source order). */
static u64 s_gf_why[24];
#define GF_FAIL(n) do { s_gf_why[(n) < 24 ? (n) : 23]++; return 0; } while (0)
static int gf_begin(const rsx_state* st, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    extern const float (*g_rsx_imm_verts)[RSX_MAX_VERTEX_ATTRIBS][4];
    if (!gf_enabled() || !st || !vm_base || !s_gf_mapped || !s_vp_ib_mapped || !s_vp_ib || !count) GF_FAIL(1);
    if (g_rsx_imm_verts) GF_FAIL(2);
    if (s_vp_ib_off + count > VP_MAX_INDICES) GF_FAIL(3);
    const int vi = vp_get_vs(st);
    if (vi < 0 || vi >= s_d3d.vp_vs_n || (!s_d3d.vp_vs[vi].vs_gf && !s_d3d.vp_vs[vi].pending)) GF_FAIL(4);
    const u32 inmask = s_d3d.vp_vs[vi].inmask;
    s_gfp.active = 0; s_gfp.na = 0;
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        memcpy(s_gfp.tab_c[i], st->vertex_data4f[i], 16);
        s_gfp.tab_a[i][0] = s_gfp.tab_a[i][1] = s_gfp.tab_a[i][2] = s_gfp.tab_a[i][3] = 0;
        if (!(inmask & (1u << i))) continue;
        if (!a->enabled || a->stride == 0) continue;
        if (a->frequency > 1 || a->type < 1 || a->type > 7) GF_FAIL(5);
        u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
        const int space = (a->offset & 0x80000000u) ? 0 : 1;
        u64 o0 = (u64)(a->offset & 0x7FFFFFFFu) + st->vertex_data_base_offset;
        if (space) o0 &= 0x0FFFFFFFu;
        const int k = s_gfp.na++;
        s_gfp.ai[k] = i; s_gfp.off0[k] = o0; s_gfp.stride[k] = a->stride;
        s_gfp.esz[k] = gf_elem_size(a->type, n); s_gfp.space[k] = space;
        s_gfp.tab_a[i][1] = a->stride;
        s_gfp.tab_a[i][2] = a->type | (n << 8) | (1u << 16);
    }
    s_gfp.base = st->vertex_data_base_index;
    u32* ib = s_vp_ib_mapped + (size_t)T_PARITY * VP_MAX_INDICES + s_vp_ib_off;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    if (!gf_decode(st, first, count, ib, &lo, &hi)) GF_FAIL(6);
    if (!gf_range_ok(lo, hi)) GF_FAIL(7);
    s_gfp.lo = lo; s_gfp.hi = hi;
    s_gf_ib_first = s_vp_ib_off;
    s_vp_ib_off += count;
    s_gfp.ib_end = s_vp_ib_off;
    s_gfp.epoch = st->begin_epoch; s_gfp.fp = st->shader_program;
    s_gf_draws++;
    if ((s_gf_draws % 50000u) == 0)
        fprintf(stderr, "[GPUFETCH] %llu records on the GPU-fetch path, %llu fell back, %llu MB raw copied%c",
                (unsigned long long)s_gf_draws, (unsigned long long)s_gf_fallback,
                (unsigned long long)(s_gf_bytes >> 20), 10);
    if ((s_gf_draws % 50000u) == 0 && gf_dedup_on())
        fprintf(stderr, "[GPUFETCH] dedup: %llu MB reused instead of copied, %llu repeats had changed data%c",
                (unsigned long long)(s_gf_dup_bytes >> 20), (unsigned long long)s_gf_dup_changed, 10);
    if ((s_gf_draws % 50000u) == 0) {
        char wb[256]; int wp = 0;
        for (int w = 1; w < 24; w++) if (s_gf_why[w]) wp += snprintf(wb + wp, sizeof wb - wp, " #%d:%llu", w, (unsigned long long)s_gf_why[w]);
        if (wp) fprintf(stderr, "[GPUFETCH] fallback reasons (gf_begin exit #: count):%s%c", wb, 10);
    }
    return 1;   /* gf_open() marks it active once the record exists */
}
static void gf_open(u32 rec)
{
    s_gfp.active = 1; s_gfp.rec = rec; s_gfp.parity = (u32)T_PARITY; s_gfp.tbl = T_DRAWS;
    s_gfp.seq = ++s_gf_seq;
    T_DRAWS[rec].gf_seq = s_gfp.seq;
}

/* A batch continuing the open record: append its indices. */
static int gf_extend(const rsx_state* st, u32 first, u32 count)
{
    if (!s_gfp.active || !st || s_gfp.ib_end != s_vp_ib_off || s_vp_ib_off + count > VP_MAX_INDICES) return 0;
    if (st->begin_epoch != s_gfp.epoch || st->shader_program != s_gfp.fp) return 0;
    if (s_gfp.tbl != T_DRAWS || s_gfp.parity != (u32)T_PARITY || s_gfp.rec + 1 != T_DRAWCNT) return 0;
    u32* ib = s_vp_ib_mapped + (size_t)T_PARITY * VP_MAX_INDICES + s_vp_ib_off;
    u32 lo = s_gfp.lo, hi = s_gfp.hi;
    if (!gf_decode(st, first, count, ib, &lo, &hi)) return 0;
    if (!gf_range_ok(lo, hi)) return 0;
    s_gfp.lo = lo; s_gfp.hi = hi;
    s_vp_ib_off += count; s_gfp.ib_end = s_vp_ib_off;
    return 1;
}

/* v190: copy src -> dst (dst may be NULL = hash only) and return a 64-bit
 * hash of src. dst is 16-byte aligned upload-heap memory (write-combined), so
 * the copy uses streaming stores. xxh3-style lanes with a position-dependent
 * key: moved or swapped vertices change the hash. */
static u64 gf_copy_hash(u8* dst, const u8* src, size_t len)
{
    __m128i a0 = _mm_set_epi64x(0x165667B19E3779F9ll, 0x27D4EB2F165667C5ll), a1 = _mm_set_epi64x(0x7FB5D329728E5A1Bll, 0x2545F4914F6CDD1Dll);
    __m128i a2 = _mm_set_epi64x(0x61C8864680B583EBll, 0x5851F42D4C957F2Dll), a3 = _mm_set_epi64x(0x14057B7EF767814Fll, 0x0A0761D6478BD642ll);
    __m128i kk = _mm_set_epi64x(0x9E3779B97F4A7C15ll, 0xC2B2AE3D27D4EB4Fll);
    const __m128i ks = _mm_set_epi64x(0x632BE59BD9B4E019ll, 0x8EBC6AF09C88C6E3ll);
    size_t i = 0;
#define GF_LANE(A, V) do { const __m128i _x = _mm_xor_si128((V), kk); \
        A = _mm_add_epi64(A, _mm_add_epi64(_mm_mul_epu32(_x, _mm_srli_epi64(_x, 32)), _mm_shuffle_epi32((V), 0x4E))); \
        kk = _mm_add_epi64(kk, ks); } while (0)
    if (dst) {
        for (; i + 64 <= len; i += 64) {
            const __m128i v0 = _mm_loadu_si128((const __m128i*)(src + i)), v1 = _mm_loadu_si128((const __m128i*)(src + i + 16));
            const __m128i v2 = _mm_loadu_si128((const __m128i*)(src + i + 32)), v3 = _mm_loadu_si128((const __m128i*)(src + i + 48));
            _mm_stream_si128((__m128i*)(dst + i), v0); _mm_stream_si128((__m128i*)(dst + i + 16), v1);
            _mm_stream_si128((__m128i*)(dst + i + 32), v2); _mm_stream_si128((__m128i*)(dst + i + 48), v3);
            GF_LANE(a0, v0); GF_LANE(a1, v1); GF_LANE(a2, v2); GF_LANE(a3, v3);
        }
    } else {
        for (; i + 64 <= len; i += 64) {
            const __m128i v0 = _mm_loadu_si128((const __m128i*)(src + i)), v1 = _mm_loadu_si128((const __m128i*)(src + i + 16));
            const __m128i v2 = _mm_loadu_si128((const __m128i*)(src + i + 32)), v3 = _mm_loadu_si128((const __m128i*)(src + i + 48));
            GF_LANE(a0, v0); GF_LANE(a1, v1); GF_LANE(a2, v2); GF_LANE(a3, v3);
        }
    }
#undef GF_LANE
    if (dst && i < len) memcpy(dst + i, src + i, len - i);
    u64 t[8];
    _mm_storeu_si128((__m128i*)&t[0], a0); _mm_storeu_si128((__m128i*)&t[2], a1);
    _mm_storeu_si128((__m128i*)&t[4], a2); _mm_storeu_si128((__m128i*)&t[6], a3);
    u64 h = 0xCBF29CE484222325ull ^ (u64)len;
    for (int q = 0; q < 8; q++) { h ^= t[q]; h *= 0x100000001B3ull; h ^= h >> 29; }
    for (; i < len; i++) { h ^= src[i]; h *= 0x100000001B3ull; }
    h ^= h >> 32; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 29;
    return h;
}

/* v190b: a sampled hash for the dedup check: the first and last 256 bytes
 * plus 64 bytes of every 2 KB in between. The full-length hash (gf_copy_hash)
 * cost more than the copy it saved -- the source is usually still in cache --
 * and over a Titan fight not one repeat range had changed mid-frame, so a
 * sample is enough to catch a buffer the game really rewrote. */
static u64 gf_sample_hash(const u8* src, size_t len)
{
    u64 h = 0xCBF29CE484222325ull ^ ((u64)len * 0x9E3779B97F4A7C15ull);
#define GF_SH(P, N) do { const u8* _p = (P); for (size_t _i = 0; _i + 8 <= (N); _i += 8) { u64 _w; memcpy(&_w, _p + _i, 8); \
        h = (h ^ _w) * 0x100000001B3ull; h ^= h >> 31; } } while (0)
    if (len <= 1024) { GF_SH(src, len & ~(size_t)7); }
    else {
        GF_SH(src, 256);
        for (size_t o = 2048; o + 64 <= len - 256; o += 2048) GF_SH(src + o, 64);
        GF_SH(src + ((len - 256) & ~(size_t)7), 256);
    }
#undef GF_SH
    for (size_t i = len & ~(size_t)7; i < len; i++) { h ^= src[i]; h *= 0x100000001B3ull; }
    h ^= h >> 32; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 29;
    return h;
}

/* v190: per-frame dedup of the vertex copies. A skinned mesh (a Titan, the
 * Chimera crowd) is drawn several times a frame -- shadow cascades, depth
 * pre-pass, the scene -- and every draw copied the same vertex range again:
 * up to ~9 KB a draw, the FIFO thread's biggest single cost in a Titan fight.
 * A repeat of a (space, start, length) range seen earlier in this half is
 * hashed instead of copied, and reuses the earlier copy only if the bytes
 * hash the same (a buffer the game rewrote mid-frame is copied again).
 * RSX_GF_DEDUP=0 = old memcpy path. */
typedef struct { u64 gs; u32 len, dst, gen, par; u64 h; u32 space; } GfDup;
#define GF_DUP_N 8192u
static GfDup s_gf_dup[GF_DUP_N];
static int gf_dedup_on(void)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_GF_DEDUP"); en = e ? atoi(e) : 1; }
    return en;
}

/* Close the open record: copy its vertex ranges and write its attribute table. */
static void gf_finalize(void)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!s_gfp.active) return;
    s_gfp.active = 0;
    if (s_gfp.tbl != T_DRAWS || s_gfp.parity != (u32)T_PARITY || s_gfp.rec >= T_DRAWCNT) return;
    D3D12DrawRecord* dr = &T_DRAWS[s_gfp.rec];
    if (!dr->gf || dr->gf_seq != s_gfp.seq) return;
    const u64 eLo = (u64)s_gfp.lo + s_gfp.base, eHi = (u64)s_gfp.hi + s_gfp.base;
    u64 rs[16], re[16]; int gsp[16], grp[16], ng = 0; u64 gs[16], ge[16];
    for (int k = 0; k < s_gfp.na; k++) {
        rs[k] = s_gfp.off0[k] + eLo * s_gfp.stride[k];
        re[k] = s_gfp.off0[k] + eHi * s_gfp.stride[k] + s_gfp.esz[k];
        int g = -1;
        for (int q = 0; q < ng; q++)
            if (gsp[q] == s_gfp.space[k] && rs[k] <= ge[q] + 256 && re[k] + 256 >= gs[q]) { g = q; break; }
        if (g < 0) { g = ng++; gsp[g] = s_gfp.space[k]; gs[g] = rs[k]; ge[g] = re[k]; }
        else { if (rs[k] < gs[g]) gs[g] = rs[k]; if (re[k] > ge[g]) ge[g] = re[k]; }
        grp[k] = g;
    }
    for (int a2 = 0; a2 < ng; a2++) for (int b2 = a2 + 1; b2 < ng; b2++)
        if (gsp[a2] >= 0 && gsp[a2] == gsp[b2] && gs[b2] <= ge[a2] + 256 && ge[b2] + 256 >= gs[a2]) {
            if (gs[b2] < gs[a2]) gs[a2] = gs[b2]; if (ge[b2] > ge[a2]) ge[a2] = ge[b2];
            gsp[b2] = -1;
            for (int k = 0; k < s_gfp.na; k++) if (grp[k] == b2) grp[k] = a2;
        }
    u64 need = 0;
    for (int g = 0; g < ng; g++) if (gsp[g] >= 0) need += (ge[g] - gs[g]) + 32;
    if ((u64)s_gf_off + need > GF_CAP) {      /* out of room after all: drop the draw */
        dr->ib_count = 0; dr->vertex_count = 0; s_gf_fallback++;
        return;
    }
    u8* half = s_gf_mapped + (size_t)T_PARITY * GF_CAP;
    u32 dst[16];
    const int dd = gf_dedup_on();
    int streamed = 0;
    for (int g = 0; g < ng; g++) {
        if (gsp[g] < 0) continue;
        u32 d = (s_gf_off + 15u) & ~15u;
        const u64 len = ge[g] - gs[g];
        /* v190: one contiguous host source -> dedup / streaming copy */
        const u8* src = NULL;
        if (dd) {
            if (gsp[g] == 1) { const u32 r = cellGcmResolveLocated(1, (u32)gs[g]); if (r != 0xFFFFFFFFu) src = vm_base + r; }
            else if (len && (gs[g] >> 20) == ((ge[g] - 1u) >> 20)) {
                const u32 r = cellGcmResolveLocated(0, (u32)(gs[g] & ~0xFFFFFull));
                if (r != 0xFFFFFFFFu) src = vm_base + r + (u32)(gs[g] & 0xFFFFFu);
            }
        }
        if (src) {
            GfDup* slot = NULL; GfDup* hit = NULL;
            if (len >= 512) {
                u64 kh = (gs[g] * 0x9E3779B97F4A7C15ull) ^ (len * 0xC2B2AE3D27D4EB4Full) ^ (u64)gsp[g];
                kh ^= kh >> 31;
                for (u32 n = 0, i = (u32)kh & (GF_DUP_N - 1u); n < 16; n++, i = (i + 1u) & (GF_DUP_N - 1u)) {
                    GfDup* e = &s_gf_dup[i];
                    if (e->gen != s_gf_gen || e->par != (u32)T_PARITY) { if (!slot) slot = e; break; }
                    if (e->gs == gs[g] && e->len == (u32)len && e->space == (u32)gsp[g]) { hit = e; break; }
                    if (!slot && n == 15) slot = e;
                }
            }
            const int full = gf_dedup_on() >= 2;   /* RSX_GF_DEDUP=2: whole-range hash (v190) */
            if (hit) {
                const u64 h = full ? gf_copy_hash(NULL, src, (size_t)len) : gf_sample_hash(src, (size_t)len);
                if (h == hit->h) { dst[g] = hit->dst; s_gf_dup_bytes += len; continue; }
                s_gf_dup_changed++;
                slot = hit;
            }
            u64 h;
            if (full) { h = gf_copy_hash(half + d, src, (size_t)len); streamed = 1; }
            else { memcpy(half + d, src, (size_t)len); h = slot ? gf_sample_hash(src, (size_t)len) : 0; }
            if (slot) { slot->gs = gs[g]; slot->len = (u32)len; slot->space = (u32)gsp[g];
                        slot->gen = s_gf_gen; slot->par = (u32)T_PARITY; slot->h = h; slot->dst = d; }
        } else if (gsp[g] == 1) {
            const u32 r = cellGcmResolveLocated(1, (u32)gs[g]);
            if (r == 0xFFFFFFFFu) memset(half + d, 0, (size_t)len);
            else memcpy(half + d, vm_base + r, (size_t)len);
        } else {
            for (u64 o = gs[g]; o < ge[g]; ) {
                u64 pe = (o | 0xFFFFFull) + 1; if (pe > ge[g]) pe = ge[g];
                const u32 r = cellGcmResolveLocated(0, (u32)(o & ~0xFFFFFull));
                if (r == 0xFFFFFFFFu) memset(half + d + (o - gs[g]), 0, (size_t)(pe - o));
                else memcpy(half + d + (o - gs[g]), vm_base + r + (u32)(o & 0xFFFFFu), (size_t)(pe - o));
                o = pe;
            }
        }
        memset(half + d + len, 0, 16);
        dst[g] = d;
        s_gf_off = d + (u32)len + 16u;
        s_gf_bytes += len;
    }
    if (streamed) _mm_sfence();   /* v190: streaming stores visible before the GPU reads them */
    for (int k = 0; k < s_gfp.na; k++) {
        const int g = grp[k];
        const long long b = (long long)dst[g] + (long long)s_gfp.off0[k] - (long long)gs[g];
        s_gfp.tab_a[s_gfp.ai[k]][0] = (u32)(int)b;
    }
    if (g_drawlog_left > 0) {   /* v180 diag: what the copy actually got */
        for (int k = 0; k < s_gfp.na; k++) {
            const u8* q = half + s_gfp.tab_a[s_gfp.ai[k]][0] + (u64)(s_gfp.lo + s_gfp.base) * s_gfp.stride[k];
            float f[3]; for (int c = 0; c < 3; c++) { u32 w; memcpy(&w, q + c * 4, 4); w = _byteswap_ulong(w); memcpy(&f[c], &w, 4); }
            fprintf(stderr, "[DRAWLOG]    copied rec %u a%d (lo=%u): (%g %g %g)%c", s_gfp.rec, s_gfp.ai[k], s_gfp.lo, f[0], f[1], f[2], 10);
        }
    }
    if (s_d3d.vp_cb_mapped) {
        u8* cb = (u8*)s_d3d.vp_cb_mapped + ((u64)T_PARITY * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE + GF_CB_OFF;
        memcpy(cb, s_gfp.tab_a, sizeof s_gfp.tab_a);
        memcpy(cb + 256, s_gfp.tab_c, sizeof s_gfp.tab_c);
        u32 x[4] = { s_gfp.base, 0, 0, 0 };
        memcpy(cb + 512, x, 16);
    }
    { static int n = 0; if (n < 3) { n++;
        fprintf(stderr, "[GPUFETCH] record closed: %u indices idx=[%u..%u] base=%u ranges=%d raw=%u B%c",
                dr->ib_count, s_gfp.lo, s_gfp.hi, s_gfp.base, ng, s_gf_off, 10); } }
}

/* GPU vertex fetch: the vertex-shader side (see gf_prepare). The decompiled
 * program is reused as-is; only its inputs change -- "input.aN" becomes a
 * fetch from the raw buffer for SV_VertexID. */
static const char s_gf_hlsl_helpers[] =
"ByteAddressBuffer vp_raw : register(t30);\n"   /* vs_5_0: no register spaces */
"uint vp_b8(uint a) { return (vp_raw.Load(a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu; }\n"
"uint vp_be32(uint a) {\n"
"    uint al = a & ~3u, sh = (a & 3u) * 8u;\n"
"    uint w = vp_raw.Load(al);\n"
"    if (sh != 0u) w = (w >> sh) | (vp_raw.Load(al + 4u) << (32u - sh));\n"
"    return (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24);\n"
"}\n"
"uint vp_be16(uint a) { return (vp_b8(a) << 8) | vp_b8(a + 1u); }\n"
"float4 vp_gf(uint i, uint vid) {\n"
"    uint4 d = vp_gfa[i]; float4 r = vp_gfc[i];\n"
"    if ((d.z >> 16) != 1u) return r;\n"
"    uint ty = d.z & 0xFFu, n = (d.z >> 8) & 0xFFu;\n"
"    uint ei = (vid + vp_gfx.x) & 0xFFFFFu;\n"
"    uint a = (uint)((int)d.x + (int)(ei * d.y));\n"
"    if (ty == 6u) {\n"
"        uint w = vp_be32(a);\n"
"        int x = (int)(w << 21) >> 21, y = (int)(w << 10) >> 21, z = (int)w >> 22;\n"
"        return float4((float)x / 1023.0, (float)y / 1023.0, (float)z / 511.0, 1.0);\n"
"    }\n"
"    float c[4] = { r.x, r.y, r.z, r.w };\n"
"    [unroll] for (uint k = 0u; k < 4u; k++) {\n"
"        if (k < n) {\n"
"            float v;\n"
"            if (ty == 2u)      v = asfloat(vp_be32(a + 4u * k));\n"
"            else if (ty == 3u) v = f16tof32(vp_be16(a + 2u * k));\n"
"            else if (ty == 4u) v = (float)vp_b8(a + k) * (1.0 / 255.0);\n"
"            else if (ty == 1u) v = (float)((int)(vp_be16(a + 2u * k) << 16) >> 16) / 32767.0;\n"
"            else if (ty == 5u) v = (float)((int)(vp_be16(a + 2u * k) << 16) >> 16);\n"
"            else               v = (float)vp_b8(a + k);\n"
"            c[k] = v;\n"
"        }\n"
"    }\n"
"    return float4(c[0], c[1], c[2], c[3]);\n"
"}\n";

static ID3DBlob* gf_compile_vs_ex(const char* hlsl, int cache_only);
static ID3DBlob* gf_compile_vs(const char* hlsl) { return gf_compile_vs_ex(hlsl, 0); }
static char* gf_make_text(const char* hlsl, size_t* olen)
{
    if (!hlsl) return NULL;
    static const char sig[] = "VSOutput main(VSInput input) {";
    const char* m = strstr(hlsl, sig);
    if (!m) return NULL;
    const size_t cap = strlen(hlsl) * 2 + sizeof s_gf_hlsl_helpers + 4096;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    memcpy(out, hlsl, (size_t)(m - hlsl)); o = (size_t)(m - hlsl);
    memcpy(out + o, s_gf_hlsl_helpers, sizeof s_gf_hlsl_helpers - 1); o += sizeof s_gf_hlsl_helpers - 1;
    static const char nsig[] = "VSOutput main(uint _vid : SV_VertexID) {";
    memcpy(out + o, nsig, sizeof nsig - 1); o += sizeof nsig - 1;
    for (const char* p = m + sizeof sig - 1; *p && o + 64 < cap; ) {
        if (p[0] == 'i' && !strncmp(p, "input.a", 7) && p[7] >= '0' && p[7] <= '9') {
            const char* q = p + 7; unsigned idx = 0;
            while (*q >= '0' && *q <= '9') idx = idx * 10 + (unsigned)(*q++ - '0');
            o += (size_t)snprintf(out + o, cap - o, "vp_gf(%uu, _vid)", idx);
            p = q;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    if (olen) *olen = o;
    return out;
}
static ID3DBlob* gf_compile_vs_ex(const char* hlsl, int cache_only)
{
    if (!gf_enabled() || !hlsl) return NULL;
    size_t o = 0;
    char* out = gf_make_text(hlsl, &o);
    if (!out) return NULL;
    ID3DBlob* vb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = shc_compile_ex(out, o, "guest_vp_gf", "main", "vs_5_0", &vb, &e, cache_only);
    if (cache_only && FAILED(hr)) { if (e) e->lpVtbl->Release(e); free(out); return NULL; }
    if (FAILED(hr) || !vb) {
        static int n = 0; if (n < 4) { n++;
            fprintf(stderr, "[GPUFETCH] VS variant compile FAIL: %s%c",
                    e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?", 10);
            FILE* f = fopen("logs/gf_vs_fail.hlsl", "w"); if (f) { fputs(out, f); fclose(f); } }
        vb = NULL;
    }
    if (e) e->lpVtbl->Release(e);
    free(out);
    return vb;
}

/* Which vp_c[] rows a decompiled program reads: static "vp_c[N]" indexes become
 * runs (gaps under 8 rows merged); any "vp_c[(" (address-register relative
 * read) means the whole bank. VP_CONST_RUNS=0 copies everything. */
static void vs_const_runs(VPVSEntry* e, const char* hlsl)
{
    static int en = -1;
    if (en < 0) { const char* v = getenv("VP_CONST_RUNS"); en = v ? atoi(v) : 1; }
    e->c_all = 1; e->crun_n = 0;
    if (!en || !hlsl) return;
    static u8 used[512];
    memset(used, 0, sizeof used);
    const char* body = strstr(hlsl, "VSOutput main(");   /* past the cbuffer's vp_c[512] */
    if (!body) return;
    /* v184f: a relative read still means "copy the whole bank" (c_all), but the
     * static rows are kept in crun anyway -- the FSR instance signature uses
     * them (it fell back to rows 0..15 = the shared camera, and every instance
     * of a skinned / indexed mesh tied). Copy paths check c_all first. */
    int rel = 0;
    for (const char* p = strstr(body, "vp_c["); p; p = strstr(p + 5, "vp_c[")) {
        const char* q = p + 5;
        if (*q < '0' || *q > '9') { rel = 1; continue; }   /* relative (or unknown) read */
        unsigned n = 0; while (*q >= '0' && *q <= '9') n = n * 10 + (unsigned)(*q++ - '0');
        if (n >= 512) { rel = 1; continue; }
        used[n] = 1;
        if (*q != ']') rel = 1;                       /* vp_c[N + a0...]: relative from N */
    }
    int n = 0, i = 0, frag = 0;
    while (i < 512) {
        if (!used[i]) { i++; continue; }
        int st = i, last = i;
        for (int j = i + 1; j < 512 && j <= last + 8; j++) if (used[j]) last = j;
        if (n >= 48) { frag = 1; break; }            /* too fragmented: copy all */
        e->crun[n][0] = (u16)st; e->crun[n][1] = (u16)(last - st + 1); n++;
        i = last + 1;
    }
    e->crun_n = (u16)n; e->c_all = (rel || frag) ? 1 : 0;
}

/* Resistance port: asynchronous vertex-program compiles. A new program used to
 * run FXC on the FIFO thread at record time -- twice (base + GPU-fetch
 * variant), ~30-300 ms each -- and the guest waited on the FIFO meanwhile:
 * walking into a new area of York stalled for seconds (one 13 s freeze was 39
 * compiles). Now a program whose bytecode is not in shadercache\ gets its slot
 * at once, marked pending, and worker threads compile it; draws using a
 * pending program are skipped (the object pops in a few frames later, once
 * per program ever -- the disk cache makes later runs synchronous and
 * instant). RSX_VS_ASYNC=0 restores the synchronous compile. */
typedef struct { u32 hash; char* hlsl; int kind;        /* 0 VS, 1 FP */
                 FPBlobEntry* be; u32 k_uhash, k_exp32, k_unorm, k_cube; } VsJob;
#define VSJ_Q 512
static VsJob s_vsj_q[VSJ_Q];
static u32 s_vsj_h, s_vsj_t;
static SRWLOCK s_vsj_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_vsj_cv = CONDITION_VARIABLE_INIT;
static SRWLOCK s_vs_lock = SRWLOCK_INIT;     /* vp_vs[] publish vs eviction */
static volatile LONG s_vsj_done, s_vsj_queued;
static int vs_async(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_VS_ASYNC"); e = v ? atoi(v) : 1; }
    return e;
}
static DWORD WINAPI vsj_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        AcquireSRWLockExclusive(&s_vsj_lock);
        while (s_vsj_h == s_vsj_t)
            SleepConditionVariableSRW(&s_vsj_cv, &s_vsj_lock, INFINITE, 0);
        VsJob j = s_vsj_q[s_vsj_h % VSJ_Q]; s_vsj_h++;
        ReleaseSRWLockExclusive(&s_vsj_lock);
        if (j.kind == 1) {   /* fragment program */
            ID3DBlob* pb = NULL; ID3DBlob* pe = NULL;
            HRESULT hr = shc_compile(j.hlsl, strlen(j.hlsl), "guest_fp", "main", "ps_5_0", &pb, &pe);
            if (FAILED(hr)) { if (pb) pb->lpVtbl->Release(pb); pb = NULL;
                static volatile LONG nf = 0; if (InterlockedIncrement(&nf) <= 8)
                    fprintf(stderr, "[FP] async PS compile FAIL: %s%c",
                            pe ? (const char*)pe->lpVtbl->GetBufferPointer(pe) : "?", 10); }
            if (pe) pe->lpVtbl->Release(pe);
            int took = 0;
            AcquireSRWLockExclusive(&s_vs_lock);
            FPBlobEntry* be = j.be;
            if (be->pending && be->used && be->uhash == j.k_uhash && be->exp32 == j.k_exp32 &&
                be->unorm == j.k_unorm && be->cube == j.k_cube) {
                be->blob = pb;   /* NULL = known not to compile */
                MemoryBarrier();
                be->pending = 0; took = 1;
            }
            ReleaseSRWLockExclusive(&s_vs_lock);
            if (!took && pb) pb->lpVtbl->Release(pb);
            InterlockedIncrement(&s_vsj_done);
            free(j.hlsl);
            continue;
        }
        ID3DBlob* vb = NULL; ID3DBlob* e = NULL;
        HRESULT hr = shc_compile(j.hlsl, strlen(j.hlsl), "guest_vp2", "main", "vs_5_0", &vb, &e);
        if (e) e->lpVtbl->Release(e);
        if (FAILED(hr)) { if (vb) vb->lpVtbl->Release(vb); vb = NULL;
            static volatile LONG nf = 0; if (InterlockedIncrement(&nf) <= 4)
                fprintf(stderr, "[VP] async VS compile FAIL (hash=0x%08X)%c", j.hash, 10); }
        ID3DBlob* gf = gf_compile_vs(j.hlsl);
        int found = 0;
        AcquireSRWLockExclusive(&s_vs_lock);
        for (int i = 0; i < s_d3d.vp_vs_n; i++)
            if (s_d3d.vp_vs[i].hash == j.hash && s_d3d.vp_vs[i].pending) {
                s_d3d.vp_vs[i].vs = vb; s_d3d.vp_vs[i].vs_gf = gf;
                MemoryBarrier();
                s_d3d.vp_vs[i].pending = 0;
                found = 1; break;
            }
        ReleaseSRWLockExclusive(&s_vs_lock);
        if (!found) { if (vb) vb->lpVtbl->Release(vb); if (gf) gf->lpVtbl->Release(gf); }
        InterlockedIncrement(&s_vsj_done);
        free(j.hlsl);
    }
    return 0;
}
/* 1 = queued (the caller marks its slot pending), 0 = compile it yourself. */
static int vsj_submit_job(const VsJob* jin);
static int vsj_submit(u32 hash, const char* hlsl)
{
    VsJob j; memset(&j, 0, sizeof j); j.hash = hash; j.hlsl = (char*)hlsl; j.kind = 0;
    return vsj_submit_job(&j);
}
static int vsj_submit_job(const VsJob* jin)
{
    const char* hlsl = jin->hlsl;
    static volatile LONG started = 0;   /* VS jobs come from the FIFO thread, FP jobs from the render thread */
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int n = (int)si.dwNumberOfProcessors / 4; if (n < 2) n = 2; if (n > 4) n = 4;
        { const char* e = getenv("RSX_VS_ASYNC_THREADS"); if (e && atoi(e) > 0) n = atoi(e); }
        for (int i = 0; i < n; i++) {
            HANDLE h = CreateThread(NULL, 1u << 20, vsj_worker, NULL, 0, NULL);
            if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
        }
        fprintf(stderr, "[VP] async shader compile: %d worker threads%c", n, 10);
    }
    const size_t len = strlen(hlsl);
    char* copy = (char*)malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, hlsl, len + 1);
    AcquireSRWLockExclusive(&s_vsj_lock);
    if (s_vsj_t - s_vsj_h >= VSJ_Q) { ReleaseSRWLockExclusive(&s_vsj_lock); free(copy); return 0; }
    s_vsj_q[s_vsj_t % VSJ_Q] = *jin; s_vsj_q[s_vsj_t % VSJ_Q].hlsl = copy; s_vsj_t++;
    WakeConditionVariable(&s_vsj_cv);
    ReleaseSRWLockExclusive(&s_vsj_lock);
    InterlockedIncrement(&s_vsj_queued);
    return 1;
}

static int vp_get_vs_uncached(const rsx_state* st)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour. */
    u32 vstart = 0;
    { static int off = -1; if (off < 0) off = getenv("VP_START_OFF") ? 1 : 0;
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = st->vp_ucode_bytes - vstart;
    /* Hash the program's own bytes, up to its end bit, not everything behind it in the store (as the Metal
     * backend does). A title that keeps several programs resident and uploads another one behind the program it
     * draws with changed that hash on every upload, so nearly every draw missed the cache and ran D3DCompile. */
    { extern u32 rsx_vp_program_size_instrs(const u8*, u32);
      const u32 instrs = rsx_vp_program_size_instrs(vuc, vlen);
      if (instrs) vlen = instrs * 16u; }
    u32 hash = vp_hash_ucode(vuc, vlen);
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) return i;

    static char hlsl[262144];
    int ni = rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);
    if (ni <= 0) return -1;
    if (getenv("VP_DUMP")) { static int _d=0; if (PS3_LOG_FIRST(_d, 4)) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
    /* RSX_DBG_VP=<hex tex0 offset>: write the vertex program belonging to the draws
     * that bind that texture. vp2_dump.hlsl holds whichever four programs were
     * compiled first, which need not include the one under investigation -- and
     * reading the wrong program sends you chasing the wrong constants. */
    { static const char* dp = (const char*)1; static u32 wantp = 0; static int done = 0;
      if (dp == (const char*)1) { dp = getenv("RSX_DBG_VP");
                                  wantp = dp ? (u32)strtoul(dp, NULL, 16) : 0; }
      if (wantp && !done && s_d3d.cur_texs[0].raw == wantp) {
          done = 1;
          FILE* f = fopen("duck_vp.hlsl", "w");
          if (f) { fprintf(f, "/* duck VP, %d instrs, tex0=0x%X */%s", ni, wantp, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKVP] wrote duck_vp.hlsl (%d instrs)%c", ni, 10);
      } }
    /* VP_BYPASS=1: replace the guest transform with a direct map of attribute 0
     * into clip space. If geometry appears with this on, everything from the
     * input layout through raster/depth/present is sound and the fault is the
     * transform (constants or the decompiled program); if it stays blank, the
     * fault is upstream of the shader -- the attribute binding itself. Patched
     * in place, space-padded to the original length so offsets stay valid. */
    /* VP_W_ONE=1: keep the guest transform but force the clip-space w to 1.
     * Different from VP_BYPASS, which replaces the whole position: this tests
     * only the hypothesis that w is wrong. This title's VP ends
     *   { float4 _v = (float4)((vp_c[467]).xxxx); o[0].w = _v.w; }
     * with c467 = (-1, 1, 0, 0), so w = -1 and the clipper discards every
     * vertex. Patched in place, space-padded so offsets stay valid. */
    if (getenv("VP_W_ONE")) {
        static const char anchor[] = "o[0].w = _v.w;";
        static const char repl[]   = "o[0].w = 1.0;";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    if (getenv("VP_BYPASS")) {
        static const char anchor[] =
            "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        static const char repl[] = "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    ID3DBlob* vb = NULL; ID3DBlob* e = NULL; ID3DBlob* vgf = NULL;
    int pend = 0;
    if (vs_async()) {
        /* Both variants on disk: load them now (no FXC, no pop-in). */
        HRESULT hc = shc_compile_ex(hlsl, strlen(hlsl), "guest_vp2", "main", "vs_5_0", &vb, &e, 1);
        if (e) { e->lpVtbl->Release(e); e = NULL; }
        if (SUCCEEDED(hc) && vb && gf_enabled()) {
            vgf = gf_compile_vs_ex(hlsl, 1);
            if (!vgf) { vb->lpVtbl->Release(vb); vb = NULL; }
        }
        if (!vb) pend = vsj_submit(hash, hlsl);
    }
    if (!vb && !pend) {
        HRESULT hr = shc_compile(hlsl, strlen(hlsl), "guest_vp2", "main", "vs_5_0", &vb, &e);
        if (e) e->lpVtbl->Release(e);
        if (FAILED(hr) || !vb) {
            static int _e=0; if (PS3_LOG_FIRST(_e, 4)) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
            return -1;
        }
        vgf = gf_compile_vs(hlsl);
    }
    int slot;
    AcquireSRWLockExclusive(&s_vs_lock);
    if (s_d3d.vp_vs_n < VP_VS_CACHE) slot = s_d3d.vp_vs_n++;
    else {  /* evict slot 0 */
        ReleaseSRWLockExclusive(&s_vs_lock);
        rt_wait_idle();   /* RSX_ASYNC: the render thread indexes vp_vs by slot */
        AcquireSRWLockExclusive(&s_vs_lock);
        if (s_d3d.vp_vs[0].vs) s_d3d.vp_vs[0].vs->lpVtbl->Release(s_d3d.vp_vs[0].vs);
        if (s_d3d.vp_vs[0].vs_gf) s_d3d.vp_vs[0].vs_gf->lpVtbl->Release(s_d3d.vp_vs[0].vs_gf);
        free(s_d3d.vp_vs[0].src);   /* v165 (render thread is idle: rt_wait_idle above) */
        memmove(&s_d3d.vp_vs[0], &s_d3d.vp_vs[1], sizeof(VPVSEntry)*(VP_VS_CACHE-1));
        slot = VP_VS_CACHE - 1;
        s_vsm_idx = -1;   /* indices shifted */
    }
    s_d3d.vp_vs[slot].hash = hash;
    s_d3d.vp_vs[slot].vs   = vb;
    s_d3d.vp_vs[slot].vs_gf = vgf;
    s_d3d.vp_vs[slot].pending = pend;
    s_d3d.vp_vs[slot].src = fsr_mode() ? _strdup(hlsl) : NULL;   /* v165 */
    ReleaseSRWLockExclusive(&s_vs_lock);
    vs_const_runs(&s_d3d.vp_vs[slot], hlsl);
    { rsx_vp_input_analysis an; an.input_mask = 0xFFFFu;
      if (rsx_vp_analyze_inputs(vuc, vlen, &an) < 0) an.input_mask = 0xFFFFu;
      s_d3d.vp_vs[slot].inmask = an.input_mask ? an.input_mask : 0xFFFFu; }
    /* VS_SAVE=1: keep every compiled vertex program as logs/vs_<hash>.hlsl
     * (pairs with the logs/psofail_*.hlsl dumps). */
    { static int vsv = -1; if (vsv < 0) vsv = getenv("VS_SAVE") ? 1 : 0;
      if (vsv) { char fn[64]; snprintf(fn, sizeof fn, "logs/vs_%08X.hlsl", hash);
                 FILE* f = fopen(fn, "w"); if (f) { fputs(hlsl, f); fclose(f); } } }
    s_d3d.vp_vs[slot].uses_c03 =
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    { static int _n=0; if (PS3_LOG_FIRST(_n, 6)) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
    /* Build the base VP pipeline here if it does not exist yet. render_frame's
     * trigger reads s_d3d.current_rsx_state, which by frame end no longer has
     * the microcode -- so for a title that only ever compiles per-draw VS the
     * base pipeline was never built, s_d3d.vp_ready stayed 0, and the VP draw
     * pass it gates dropped every recorded is_vp draw. Here we are at RECORD
     * time with a live state that definitely has microcode. */
    if (!s_d3d.vp_ready && st && st->vp_ucode_bytes >= 16) {
        const rsx_state* prev = s_d3d.current_rsx_state;
        s_d3d.current_rsx_state = st;
        compile_vp();
        s_d3d.current_rsx_state = prev;
    }
    return slot;
}

/* --- Guest blend state -> packed PSO key ----------------------------------
 * RSX blend factors/equation carry GL enums; each SFACTOR/DFACTOR/EQUATION
 * word packs (alpha<<16)|colour. The key packs D3D12 enums so the PSO cache
 * distinguishes real blend modes: bit0 enable, [5:1] srcC, [10:6] dstC,
 * [15:11] srcA, [20:16] dstA, [23:21] opC, [26:24] opA. DeferredShading's
 * light accumulation is additive (ONE,ONE) -- the old hardcoded straight-
 * alpha PSO turned each light pass into a screen-blanking flash. */
static u8 gl_blend_factor_d3d(u32 f, int is_alpha)
{
    switch (f & 0xFFFF) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA     : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA     : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_COLOR   */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_ALPHA ~ */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static u8 gl_blend_op_d3d(u32 e)
{
    switch (e & 0xFFFF) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}
/* Pack the guest's face-culling state for the PSO key.
 * bit0 = cull enabled, bit1 = cull FRONT (else BACK), bit2 = front face is CCW.
 * RSX/GL enums: CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408;
 * FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face/front_face with
 * plain 1/0 before any register arrives, so treat those defaults as BACK/CW. */
static float s_pofs[256][2]; static int s_pofs_n = 1;   /* polygon offset table (rsx_cull_key) */
/* v178: stencil. RSX stencil state (minus the reference, which is dynamic)
 * packed as D3D12 values; rsx_cull_key carries its index in bits 24..31.
 *   bits 0..7 read mask, 8..15 write mask, then 4-bit fields: front func,
 *   fail, zfail, zpass (16..31), back func, fail, zfail, zpass (32..47).
 * The game masks window cracks to the remaining glass with it; with stencil
 * always off the cracks floated over the holes. RSX_STENCIL=0 off. */
static u64 s_stn[256]; static int s_stn_n = 1;
static u32 stn_func(u32 f) { return (f >= 0x200u && f <= 0x207u) ? (f & 7u) + 1u : 8u; }   /* D3D12 NEVER=1..ALWAYS=8 */
static u32 stn_op(u32 o)
{
    switch (o) {
    case 0x0000u: return 2u;   /* ZERO */
    case 0x1E00u: return 1u;   /* KEEP */
    case 0x1E01u: return 3u;   /* REPLACE */
    case 0x1E02u: return 4u;   /* INCR (saturate) */
    case 0x1E03u: return 5u;   /* DECR (saturate) */
    case 0x150Au: return 6u;   /* INVERT */
    case 0x8507u: return 7u;   /* INCR_WRAP */
    case 0x8508u: return 8u;   /* DECR_WRAP */
    default:      return 1u;
    }
}
static u32 rsx_stencil_index(const rsx_state* st)
{
    static int en = -1; if (en < 0) { const char* e = getenv("RSX_STENCIL"); en = e ? atoi(e) : 1; }
    if (!en || !st || !st->stencil_test_enable) return 0;
    const int two = st->two_sided_stencil;
    u64 k = (u64)(st->stencil_mask & 0xFFu) | ((u64)(st->stencil_write_mask & 0xFFu) << 8)
          | ((u64)stn_func(st->stencil_func) << 16) | ((u64)stn_op(st->stencil_op_fail) << 20)
          | ((u64)stn_op(st->stencil_op_zfail) << 24) | ((u64)stn_op(st->stencil_op_zpass) << 28);
    if (two) k |= ((u64)stn_func(st->back_stencil_func) << 32) | ((u64)stn_op(st->back_stencil_op_fail) << 36)
                | ((u64)stn_op(st->back_stencil_op_zfail) << 40) | ((u64)stn_op(st->back_stencil_op_zpass) << 44);
    else      k |= ((k >> 16) & 0xFFFFull) << 32;
    for (int i = 1; i < s_stn_n; i++) if (s_stn[i] == k) return (u32)i;
    if (s_stn_n >= 256) return 0;
    s_stn[s_stn_n] = k;
    if (s_stn_n <= 16) fprintf(stderr, "[STENCIL] state #%d: func %u fail %u zfail %u zpass %u rmask %02X wmask %02X%s%c",
                                s_stn_n, (unsigned)((k >> 16) & 15), (unsigned)((k >> 20) & 15), (unsigned)((k >> 24) & 15),
                                (unsigned)((k >> 28) & 15), (unsigned)(k & 0xFF), (unsigned)((k >> 8) & 0xFF), two ? " (two-sided)" : "", 10);
    return (u32)s_stn_n++;
}
static u32 rsx_cull_key(const rsx_state* st)
{
    if (!st) return 0;
    u32 k = 0;
    if (st->cull_face_enable) {
        k = 1u;
        if (st->cull_face == 0x0404u || st->cull_face == 0x0408u) k |= 2u;  /* FRONT */
        if (st->front_face == 0x0901u) k |= 4u;                             /* CCW */
    }
    /* Resistance port: the guest depth state rides in the same PSO key.
     * bit8 = key carries depth, bit9 = test enable, bit10 = write enable,
     * bits12..14 = func (GL 0x200..0x207 low bits). The VP PSO used to force
     * depth test+write LEQUAL on every draw, so a full-screen pass the game
     * draws with depth OFF filled the buffer and hid all UI drawn after it. */
    {
        u32 f = st->depth_func;
        u32 fl = (f >= 0x200u && f <= 0x207u) ? (f & 7u) : 1u;   /* default LESS */
        k |= 0x100u
           | (st->depth_test_enable ? 0x200u : 0u)
           | (st->depth_mask ? 0x400u : 0u)
           | (fl << 12);
    }
    /* Polygon offset: bits 16..23 index a table of distinct (scale, bias). */
    { static int en = -1;
      if (en < 0) { const char* e = getenv("RSX_POLY_OFFSET"); en = e ? atoi(e) : 1; }
      /* Quantised: the game varies scale/bias continuously per decal (bias
       * tracks ~256*scale), which overflowed a table of exact values within
       * seconds. Scale to 1/16, bias to 8 D24 units keeps it to a few dozen. */
      const float qs = floorf(st->poly_offset_scale * 16.0f + 0.5f) / 16.0f;
      const float qb = floorf(st->poly_offset_bias / 8.0f + 0.5f) * 8.0f;
      if (en == 1 && st->poly_offset_fill && (qs != 0.0f || qb != 0.0f)) {
          int idx = 0;
          for (int i = 1; i < s_pofs_n; i++)
              if (s_pofs[i][0] == qs && s_pofs[i][1] == qb) { idx = i; break; }
          if (!idx && s_pofs_n < 255) {
              idx = s_pofs_n++;
              s_pofs[idx][0] = qs; s_pofs[idx][1] = qb;
              if (idx <= 8) fprintf(stderr, "[POLYOFS] #%d scale=%g bias=%g%c", idx, st->poly_offset_scale, st->poly_offset_bias, 10);
          }
          k |= (u32)idx << 16;
      } }
    k |= rsx_stencil_index(st) << 24;   /* v178 */
    return k;
}

static u32 rsx_blend_key(const rsx_state* st, int enable)
{
    { static int _bd = -1; if (_bd < 0) _bd = getenv("BLENDDBG") ? 1 : 0;
      if (_bd) { static u32 seen[32]; static int ns=0;
        u32 sk = (enable?0x80000000u:0u) | (st ? (st->blend_sfactor & 0xFFFFu) : 0u);
        int f=0; for (int k2=0;k2<ns;k2++) if (seen[k2]==sk) f=1;
        if (!f && ns<32) { seen[ns++]=sk;
            fprintf(stderr, "[BLEND] enable=%d sfactor=0x%X dfactor=0x%X%c", enable,
                    st?st->blend_sfactor:0, st?st->blend_dfactor:0, 10); } } }
    if (!enable) return 0;
    /* Factors never programmed: keep the legacy straight-alpha behaviour
     * (dbgfont-style text enables blending without setting factors). */
    if (!st || (st->blend_sfactor == 0 && st->blend_dfactor == 0))
        return 1u
             | ((u32)D3D12_BLEND_SRC_ALPHA     << 1)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 6)
             | ((u32)D3D12_BLEND_ONE           << 11)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16)
             | ((u32)D3D12_BLEND_OP_ADD << 21) | ((u32)D3D12_BLEND_OP_ADD << 24);
    return 1u
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor,       0) << 1)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor,       0) << 6)
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor >> 16, 1) << 11)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor >> 16, 1) << 16)
         | ((u32)gl_blend_op_d3d(st->blend_equation)       << 21)
         | ((u32)gl_blend_op_d3d(st->blend_equation >> 16) << 24);
}

/* Bound colour target count for a draw record: 1 (target A) + the contiguous
 * prefix of MRT B/C/D offsets. */
static int dr_num_rts(const D3D12DrawRecord* dr)
{
    int n = 1;
    while (n < 4 && dr->rt_mrt[n - 1]) n++;
    return n;
}

/* Replace every occurrence of `find` with `repl` inside a NUL-terminated buffer,
 * shifting the tail. Used to retarget generated HLSL (2D sampler -> cube). */
static void hlsl_replace_all(char* buf, size_t cap, const char* find, const char* repl)
{
    size_t fl = strlen(find), rl = strlen(repl);
    if (!fl) return;
    char* at = buf;
    while ((at = strstr(at, find)) != NULL) {
        size_t used = strlen(buf) + 1;
        if (used + rl - fl > cap) return;
        memmove(at + rl, at + fl, used - (size_t)(at - buf) - fl);
        memcpy(at, repl, rl);
        at += rl;
    }
}

/* Which of a draw's texture units are cube textures, as a 4-bit mask. */
static u32 dr_cube_mask(const D3D12DrawRecord* dr)
{
    /* ON by default (RSX_CUBE_TEX=0 to disable). This was off while the cube path
     * cost 2262 PSO misses per 20 frames and 0.38 fps -- both caused by inline
     * fragment-program constants baking into the shader source. With those
     * constants hoisted into b1 the cube path costs 0 PSO misses, and with the
     * per-face stride fixed (each face is a whole mip pyramid, not one mip-0
     * image) all six faces decode as clean environment art. */
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_CUBE_TEX"); en = e ? atoi(e) : 1; }
    if (!en) return 0;
    u32 m = 0;
    for (int u = 0; u < RSX_UNITS; u++) if (dr->tex[u].set && dr->tex[u].cube) m |= 1u << u;
    return m;
}

/* Resistance port: texel-space (UNNORM, format bit 0x40) units of the draw
 * whose pipeline is being fetched. The decompiler samples with the raw
 * coordinate, so a texture addressed in texels -- Fall of Man's movie quad,
 * texcoords 0..1280 x 0..720 -- sampled one wrapped corner texel and the whole
 * cinematic came out one flat colour. rsx_fp_apply_unnorm_scale divides the
 * coordinate by the size in the HLSL; the sizes are baked in, so they are part
 * of the pipeline key. Set by fp_set_unnorm() right before vp_get_fp_pso.
 * RSX_UNNORM=0 disables. */
static int off_rt_find(u32 off);   /* fwd */
static u32 s_fp_unnorm_mask = 0, s_fp_unnorm_key = 0;
/* v137: units whose guest address mode clamps (S and T both CLAMP_TO_EDGE /
 * BORDER / CLAMP) sample through the clamp sampler set s16..s31. The static
 * samplers are WRAP; at 1x a pixel-aligned HUD quad never reaches its edge
 * texels' neighbours, but rendered at a higher resolution the bilinear
 * footprint at the quad's border wraps to the texture's far edge: thin lines
 * around every HUD element. On by default when RSX_RES_SCALE > 1
 * (RSX_TEX_CLAMP=0/1 forces). */
static u32 s_fp_clamp_mask = 0;
/* v184: programs whose ucode has a fragment kill (KIL = alpha cutout:
 * grass, fences, foliage). Set at decompile, read by the FSR motion pass. */
#define FP_KIL_N 8192u
static u32 s_fp_kil_a[FP_KIL_N]; static u8 s_fp_kil_v[FP_KIL_N];
static void fp_kil_note(u32 a, int k)
{
    if (!a) return;
    for (u32 n = 0, i = (a * 2654435761u) & (FP_KIL_N - 1u); n < FP_KIL_N; n++, i = (i + 1u) & (FP_KIL_N - 1u))
        if (!s_fp_kil_a[i] || s_fp_kil_a[i] == a) { s_fp_kil_v[i] = (u8)(k ? 1 : 0); s_fp_kil_a[i] = a; return; }
}
static int fp_kil_has(u32 a)
{
    for (u32 n = 0, i = (a * 2654435761u) & (FP_KIL_N - 1u); n < FP_KIL_N; n++, i = (i + 1u) & (FP_KIL_N - 1u)) {
        if (!s_fp_kil_a[i]) break;
        if (s_fp_kil_a[i] == a) return s_fp_kil_v[i];
    }
    /* not compiled through the decompile path yet (blob cache / worker):
     * decompile once here just to look for a kill. */
    extern uint8_t* vm_base; extern u32 cellGcmResolveLocated(int, u32);
    if (!a || !vm_base) return 0;
    const u32 off = cellGcmResolveLocated((a & 0x3u) == 1, a & ~0x3u);
    if (off == 0xFFFFFFFFu) { fp_kil_note(a, 0); return 0; }
    static char kb[RSX_FP_HLSL_BUFFER_SIZE];
    const int n = rsx_fp_decompile(vm_base + off, 4096, 0u, kb, sizeof kb);
    const int k = n > 0 && strstr(kb, "discard") != NULL;
    fp_kil_note(a, k);
    return k;
}
static int tex_clamp_on(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_TEX_CLAMP"); e = v ? atoi(v) : 2; }   /* v156c: back to scale > 1 only */
    return e == 2 ? (rs_scale() != 1.0f) : e;
}
static int tex_addr_clamps(u32 m) { return m == 3u || m == 4u || m == 5u; }
/* rsx_samp[u] -> rsx_samp[u+16] for each unit in mask, in place, and the
 * sampler array declared 32 long. */
static int fp_str_replace(char* h, size_t cap, const char* from, const char* to)
{
    int n = 0;
    const size_t lf = strlen(from), lt = strlen(to);
    for (char* q = strstr(h, from); q; q = strstr(q + lt, from)) {
        const size_t len = strlen(h);
        if (len + lt - lf + 1 > cap) return -1;
        memmove(q + lt, q + lf, len - (size_t)(q - h) - lf + 1);
        memcpy(q, to, lt);
        n++;
    }
    return n;
}
/* v156e: clamp-to-edge by coordinate, not by sampler. The v137 way (a second
 * bank of CLAMP samplers at s16..s31) can't compile: ps_5_0 has 16 sampler
 * registers ("X4510 maximum sampler register index exceeded"), so every
 * clamped program failed and its draws vanished (menu plates and icons
 * missing). Now each clamped unit's Sample() goes through rsx_sampc, which
 * keeps the coordinate half a texel inside the edge -- with bilinear filtering
 * that is exactly CLAMP_TO_EDGE. */
static int fp_apply_clamp_samplers(char* h, size_t cap, u32 mask)
{
    int n = 0;
    if (fp_str_replace(h, cap, "SamplerState rsx_samp[16] : register(s0);",
            "SamplerState rsx_samp[16] : register(s0);\n"
            "float4 rsx_sampc(Texture2D t, SamplerState s, float2 uv) { uint w, h; t.GetDimensions(w, h);"
            " float2 hp = 0.5 / float2(max(w, 1u), max(h, 1u)); return t.Sample(s, clamp(uv, hp, 1.0 - hp)); }") != 1) return -1;
    for (u32 u = 0; u < 16; u++) {
        if (!(mask & (1u << u))) continue;
        char from[64], to[64], decl[48];
        snprintf(from, sizeof from, "rsx_tex[%u].Sample(rsx_samp[%u], ", u, u);
        snprintf(to, sizeof to, "rsx_sampc(rsx_tex[%u], rsx_samp[%u], ", u, u);
        int k = fp_str_replace(h, cap, from, to);
        if (k < 0) return -1;
        n += k;
        snprintf(decl, sizeof decl, "Texture2D   rsx_tex%u :", u);   /* separate 2D declaration */
        if (strstr(h, decl)) {
            snprintf(from, sizeof from, "rsx_tex%u.Sample(rsx_samp[%u], ", u, u);
            snprintf(to, sizeof to, "rsx_sampc(rsx_tex%u, rsx_samp[%u], ", u, u);
            k = fp_str_replace(h, cap, from, to);
            if (k < 0) return -1;
            n += k;
        }
    }
    return n;
}
static u32 s_fp_unnorm_dim[16][2];
static u32 s_fp_unnorm_decl[16][2];   /* v189: declared size when the target is larger (clamp) */
static u8 s_fp_expand[16];   /* per-unit sign-expand lanes for the next compile */
static rsx_fp_shadow_desc s_fp_shadow[16];   /* per-unit depth compare for the next compile */
static void fp_set_shadow(const D3D12DrawRecord* dr, u32* key);
static void fp_set_unnorm_inner(const D3D12DrawRecord* dr);
/* v189: MIRROR / MIRROR_ONCE addressing. The static samplers only wrap or
 * clamp, so a mirrored texture wrapped instead: the Fareye scope's reticle is
 * one quadrant drawn over the whole lens with mirrored coordinates, and its
 * right and bottom arms came out as a big wrapped "L" from the centre. The
 * coordinate is mirrored in the shader (rsx_sampm). Per unit: bit0 u mirror,
 * bit1 v mirror, bit2 u mirror-once, bit3 v mirror-once. RSX_MIRROR=0 off. */
static u8 s_fp_mirror[16]; static u32 s_fp_mirror_mask;
static void fp_set_unnorm(const D3D12DrawRecord* dr)
{
    fp_set_unnorm_inner(dr);
    s_fp_mirror_mask = 0;
    { static int en = -1; if (en < 0) { const char* e = getenv("RSX_MIRROR"); en = e ? atoi(e) : 1; }
      if (en && dr) {
          u32 h = s_fp_unnorm_key ? s_fp_unnorm_key : 2166136261u;
          for (int u = 0; u < RSX_UNITS && u < 16; u++) {
              s_fp_mirror[u] = 0;
              if (!dr->tex[u].set || dr->tex[u].cube || (dr->tex[u].fmt & 0x40u)) continue;
              const u32 a = dr->tex[u].address, mu = a & 0xFu, mv = (a >> 8) & 0xFu;
              u8 f = 0;
              if (mu == 2u) f |= 1u; if (mv == 2u) f |= 2u;
              if (mu >= 6u && mu <= 8u) f |= 4u; if (mv >= 6u && mv <= 8u) f |= 8u;
              if (!f) continue;
              s_fp_mirror[u] = f; s_fp_mirror_mask |= 1u << u;
              h ^= 0x3A000000u | ((u32)u << 8) | f; h *= 16777619u;
          }
          if (s_fp_mirror_mask) s_fp_unnorm_key = h | 1u;
      } }
    s_fp_clamp_mask = 0;
    static int npot = -1;
    if (npot < 0) { const char* v = getenv("RSX_NPOT_CLAMP"); npot = v ? atoi(v) : 1; }
    const int gclamp = tex_clamp_on();
    if (!dr || (!gclamp && !npot)) return;
    /* v139c: only the 2D passes drawn straight onto the display surface (HUD,
     * loading icon, menus). World programs sampling their clamp-mode units
     * through CLAMP washed the floors out white. */
    /* v183: blended world draws (blood/scorch decals) honour the game's own
     * CLAMP setting: forced WRAP repeated the decal's opposite edge along its
     * quad's border (Grimsby bloodstains: hard-edged slivers around the blob).
     * The v139c whitewash came from the old s16+ sampler bank, which never
     * compiled; the rsx_sampc coordinate clamp is safe. RSX_WORLD_CLAMP=0 off. */
    static int wclamp = -1;
    if (wclamp < 0) { const char* v = getenv("RSX_WORLD_CLAMP"); wclamp = v ? atoi(v) : 1; }
    const int world = dr->rt_off != 0;
    if (world && !(wclamp && dr->blend)) return;
    u32 h = s_fp_unnorm_key ? s_fp_unnorm_key : 2166136261u;
    for (int u = 0; u < RSX_UNITS && u < 16; u++) {
        if (!dr->tex[u].set || dr->tex[u].cube) continue;
        /* v137b: plain textures only. Render targets (the scene composite
         * 0x1643041 reads the AA scene surface) and texel-addressed units
         * went black clamped. */
        if (dr->tex_rt[u] >= 0) continue;
        if (s_fp_unnorm_mask & (1u << u)) continue;
        const u32 a = dr->tex[u].address;
        /* v184j: world draws only ever clamp game-CLAMP units: test that before
         * the render-target search (fp_set_unnorm was 15% of the render thread). */
        if (world && !(tex_addr_clamps(a & 0xFu) && tex_addr_clamps((a >> 8) & 0xFu))) continue;
        if (off_rt_find(dr->tex[u].raw) >= 0) continue;
        /* v156: HUD hairlines remained at 1x too (left edge at 720p, right at
         * 1080p): HUD textures are set to WRAP by the game, so the bilinear
         * footprint at a panel's edge pulls in the opposite edge's texels.
         * On the display surface every plain 2D texture is clamped now.
         * RSX_HUD_CLAMP_ALL=0 = only the ones the game itself sets to clamp. */
        /* v156d: HUD hairlines (F12 capture, op3066+: the ammo panel / health
         * bar are pieced from small LINEAR non-power-of-two textures, 174x14,
         * 18x14, 3x23 ..., all through fp 0x1640001). The RSX can't wrap a
         * linear NPOT texture -- it clamps -- but our static samplers wrap, so
         * a piece's edge pulled in its opposite edge: a 1-pixel line whose side
         * depends on the resolution's rounding. Clamp exactly those, at every
         * scale. Swizzled power-of-two textures (scrolling scanlines, tiled
         * plates) keep wrapping -- clamping all of them broke the menu.
         * RSX_NPOT_CLAMP=0 disables. */
        const u32 tw = dr->tex[u].w, th = dr->tex[u].h;
        const int lin_npot = (dr->tex[u].fmt & 0x20u) && tw <= 512u && th <= 512u &&   /* not full-screen images (v156b black screen) */
                             ((tw & (tw - 1u)) || (th & (th - 1u)));
        const int gc = tex_addr_clamps(a & 0xFu) && tex_addr_clamps((a >> 8) & 0xFu);
        if (world) { if (gc) s_fp_clamp_mask |= 1u << u; continue; }
        if ((npot && lin_npot) ||
            (gclamp && gc)) s_fp_clamp_mask |= 1u << u;
    }
    if (s_fp_clamp_mask) { h ^= 0xC1A40000u | s_fp_clamp_mask; h *= 16777619u; s_fp_unnorm_key = h | 1u; }
}
static void fp_set_unnorm_inner(const D3D12DrawRecord* dr)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_UNNORM"); en = e ? atoi(e) : 1; }
    s_fp_unnorm_mask = 0; s_fp_unnorm_key = 0;
    memset(s_fp_expand, 0, sizeof s_fp_expand);
    if (!dr) return;
    if (!en) { /* unnorm scaling off: still honour sign expansion */
        u32 hx = 2166136261u; int anyx = 0;
        for (int u = 0; u < RSX_UNITS && u < 16; u++) {
            s_fp_expand[u] = dr->tex[u].set ? (u8)dr->tex[u].xmask : 0;
            if (s_fp_expand[u]) { anyx = 1; hx ^= 0x9E000000u | ((u32)u << 8) | s_fp_expand[u]; hx *= 16777619u; }
        }
        fp_set_shadow(dr, &hx);
        for (int u = 0; u < 16; u++) if (s_fp_shadow[u].enabled) anyx = 1;
        if (anyx) s_fp_unnorm_key = hx | 1u;
        return;
    }
    u32 h = 2166136261u;
    for (int u = 0; u < RSX_UNITS; u++) {
        s_fp_unnorm_dim[u][0] = s_fp_unnorm_dim[u][1] = 0;
        s_fp_unnorm_decl[u][0] = s_fp_unnorm_decl[u][1] = 0;
        if (!dr->tex[u].set || !(dr->tex[u].fmt & 0x40u) || dr->tex[u].cube) continue;
        if (!dr->tex[u].w || !dr->tex[u].h) continue;
        s_fp_unnorm_mask |= 1u << u;
        u32 dw = dr->tex[u].w, dh = dr->tex[u].h;
        /* Resistance port: a texel-addressed sampler on an offscreen target
         * whose D3D resource is LARGER than the declared texture (an 80x45
         * blur pass in an 80x48 surface; the surface is allocated at its clip
         * size). Texel coordinates must be divided by the resource's size, or
         * each pass reads row y*48/45 and the image shrinks 6% per pass --
         * seven bloom passes squashed the bloom/death blur into the top 64%
         * of the screen. RSX_UNNORM_RTSIZE=0 = declared size. */
        { static int rs = -1;
          if (rs < 0) { const char* e = getenv("RSX_UNNORM_RTSIZE"); rs = e ? atoi(e) : 1; }
          const u32 dbf = dr->tex[u].fmt & 0x9Fu;
          if (rs && !(dbf >= 0x90 && dbf <= 0x93)) {
              int rt = (dr->tex_rt[u] >= 0 && dr->tex_rt[u] < MAX_OFF_RTS) ? dr->tex_rt[u]
                                                                          : off_rt_find(dr->tex[u].raw);
              if (rt >= 0 && s_d3d.off_rt[rt].w >= dw && s_d3d.off_rt[rt].h >= dh &&
                  (s_d3d.off_rt[rt].w != dw || s_d3d.off_rt[rt].h != dh)) {
                  /* v189: ...and clamp to the declared size: the RSX clamps a
                   * 320x180 texture at row 179; the 320x192 target's rows 180+
                   * hold other data, and the bloom blur pulled it in as blue
                   * lines along the top and bottom of the screen. */
                  { static int cl = -1; if (cl < 0) { const char* e = getenv("RSX_UNNORM_CLAMP"); cl = e ? atoi(e) : 1; }
                    if (cl) { s_fp_unnorm_decl[u][0] = dw; s_fp_unnorm_decl[u][1] = dh; } }
                  dw = s_d3d.off_rt[rt].w; dh = s_d3d.off_rt[rt].h;
              }
          } }
        s_fp_unnorm_dim[u][0] = dw; s_fp_unnorm_dim[u][1] = dh;
        if (s_fp_unnorm_decl[u][0]) { h ^= 0xC1A3u ^ (s_fp_unnorm_decl[u][0] << 12) ^ s_fp_unnorm_decl[u][1]; h *= 16777619u; }
        h ^= (u32)u; h *= 16777619u; h ^= dw; h *= 16777619u;
        h ^= dh; h *= 16777619u;
    }
    /* Sign expansion changes the compiled text too: fold it into the key. */
    int anyx = 0;
    for (int u = 0; u < RSX_UNITS && u < 16; u++) {
        s_fp_expand[u] = dr->tex[u].set ? (u8)dr->tex[u].xmask : 0;
        if (s_fp_expand[u]) { anyx = 1; h ^= 0x9E000000u | ((u32)u << 8) | s_fp_expand[u]; h *= 16777619u; }
    }
    fp_set_shadow(dr, &h);
    for (int u = 0; u < 16; u++) if (s_fp_shadow[u].enabled) anyx = 1;
    if (s_fp_unnorm_mask || anyx) s_fp_unnorm_key = h | 1u;
}
static void fp_set_shadow(const D3D12DrawRecord* dr, u32* key)
{
    memset(s_fp_shadow, 0, sizeof s_fp_shadow);
    if (!dr) return;
    for (int u = 0; u < RSX_UNITS && u < 16; u++) {
        if (!(dr->tex_rt[u] >= 0 && (dr->tex_rt[u] & 0x3000))) continue;   /* real shadow maps + the lit stand-in */
        const u32 fr = dr->tex[u].freg;
        rsx_fp_shadow_desc_from_texture(&s_fp_shadow[u], (fr >> 8) & 0xFFu, (fr >> 4) & 0xFu,
            dr->tex[u].cube ? 1u : 0u, dr->tex[u].mips, dr->tex[u].filter, dr->tex[u].address, dr->tex[u].ctrl0);
        if (s_fp_shadow[u].enabled) {
            *key ^= 0x5D000000u | ((u32)u << 16) | ((u32)s_fp_shadow[u].func << 8) | ((u32)s_fp_shadow[u].linear << 4) | s_fp_shadow[u].enabled;
            *key *= 16777619u;
        }
    }
}

static int dr_vs_key(const D3D12DrawRecord* dr)
{ return (dr->gf && dr->vs_idx >= 0) ? dr->vs_idx + VP_VS_CACHE : dr->vs_idx; }
/* ---------------------------------------------------------------------------
 * Resistance port (v128): pipeline pre-build.
 *
 * Where a playthrough meets a new pipeline the driver compiles it inside
 * CreateGraphicsPipelineState on the render thread: about 1 ms with the
 * driver's own cache warm, tens of ms cold -- ~1070 of them in one York
 * session, bursts of 100 at once entering an area. Shader BYTECODE was already
 * kept (shadercache\), but a pipeline is bytecode plus fixed-function state and
 * nothing remembered which combinations the game uses. Now every guest
 * pipeline built here is described in shadercache\pso_manifest_game.bin
 * (_fe.bin for the front end; the two
 * shaders' cache keys + the state, 40 bytes each); the first pipeline request
 * of a run loads the manifest and a background thread builds every pipeline
 * it lists, so the render thread takes a finished one instead of compiling
 * (PERF2 "pre-built N"). The record is also what the desc is built FROM, so a
 * pre-built pipeline is exactly the one that would have been compiled.
 * RSX_PSO_PREWARM=0: no pre-build (still records). RSX_PSO_MANIFEST=0: no
 * recording either.
 * -------------------------------------------------------------------------*/
typedef struct {
    u64 vs_key, ps_key;           /* shadercache\<key>.dxbc */
    u32 rtfmt;                    /* DXGI format of every target */
    u32 blend;                    /* packed guest blend key (rsx_blend_key) */
    float slope_bias;             /* polygon offset */
    int32_t depth_bias;
    u8 gfv, cull_mode, front_ccw, nrt, cmask, depth_enable, depth_write, depth_func;
    u64 stencil;                  /* v178: s_stn-packed stencil state, 0 = off */
} PsoRec;
typedef char psorec_size_check[(sizeof(PsoRec) == 48) ? 1 : -1];
static const u8 s_psom_magic[8] = { 'R', 'F', 'P', 'S', 'O', 2, 48, 0 };
#define PSOW_N 65536u
typedef struct { PsoRec rec; u64 h; ID3D12PipelineState* pso; int state; } PsoWarm;
/* state: 0 empty, 1 listed (in the manifest, not built yet), 2 built, 3 claimed
 * by the render thread (it took the built one, or compiled its own) */
static PsoWarm* s_psow;
static SRWLOCK s_psow_lock = SRWLOCK_INIT;
static PsoRec* s_psow_recs; static u32 s_psow_nrec;
static volatile LONG s_psow_hits;
static FILE* s_psom_f;
/* One manifest per program: the front end's menus and the game's levels use
 * different pipelines, and each process would otherwise pre-build both. */
static const char* psom_path(void)
{
    const char* bp = getenv("PS3_BOOT_PATH");
    return (bp && strstr(bp, "game.self")) ? "shadercache\\pso_manifest_game2.bin"   /* v178: 48-byte records */
                                           : "shadercache\\pso_manifest_fe2.bin";
}
static int pso_manifest_on(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_PSO_MANIFEST"); const char* c = getenv("RSX_SHADERCACHE");
                 e = (v ? atoi(v) : 1) && (c ? atoi(c) : 1); }
    return e;
}
static int pso_prewarm_on(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_PSO_PREWARM"); e = (v ? atoi(v) : 1) && pso_manifest_on(); }
    return e;
}
static u64 pso_rec_hash(const PsoRec* r)
{
    const u8* p = (const u8*)r; u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *r; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}
/* Caller holds s_psow_lock. insert: an absent record gets a slot with state 0
 * that the caller fills in at once. NULL when absent (or the table is full). */
static PsoWarm* psow_slot(const PsoRec* r, u64 h, int insert)
{
    if (!s_psow) {
        if (!insert) return NULL;
        s_psow = (PsoWarm*)calloc(PSOW_N, sizeof(PsoWarm));
        if (!s_psow) return NULL;
    }
    for (u32 i = (u32)(h >> 40) & (PSOW_N - 1u), k = 0; k < PSOW_N; k++, i = (i + 1u) & (PSOW_N - 1u)) {
        PsoWarm* e = &s_psow[i];
        if (!e->state) {
            if (!insert) return NULL;
            e->rec = *r; e->h = h; return e;
        }
        if (e->h == h && !memcmp(&e->rec, r, sizeof *r)) return e;
    }
    return NULL;
}
/* The pipeline desc for a record. il: 16 entries the desc points into. */
static void pso_desc_from_rec(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, const PsoRec* r,
                              const void* vs, SIZE_T vsn, const void* ps, SIZE_T psn,
                              D3D12_INPUT_ELEMENT_DESC* il)
{
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_i].InstanceDataStepRate = 0;
    }
    memset(pd, 0, sizeof *pd);
    pd->pRootSignature = s_d3d.vp_root_sig;
    pd->VS.pShaderBytecode = vs; pd->VS.BytecodeLength = vsn;
    pd->PS.pShaderBytecode = ps; pd->PS.BytecodeLength = psn;
    pd->InputLayout.pInputElementDescs = r->gfv ? NULL : il; pd->InputLayout.NumElements = r->gfv ? 0 : 16;
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd->RasterizerState.CullMode = (D3D12_CULL_MODE)r->cull_mode;
    pd->RasterizerState.FrontCounterClockwise = r->front_ccw ? TRUE : FALSE;
    pd->RasterizerState.SlopeScaledDepthBias = r->slope_bias;
    pd->RasterizerState.DepthBias = (INT)r->depth_bias;
    pd->RasterizerState.DepthBiasClamp = 0.0f;
    /* Blend per the guest's packed key (see rsx_blend_key): dbgfont text needs
     * straight alpha; demosaic's effect passes blend OFF; DeferredShading's
     * light accumulation is additive ONE,ONE. */
    const u32 blend = r->blend;
    D3D12_RENDER_TARGET_BLEND_DESC* b0 = &pd->BlendState.RenderTarget[0];
    b0->BlendEnable    = (blend & 1) ? TRUE : FALSE;
    b0->SrcBlend       = (blend & 1) ? (D3D12_BLEND)((blend >> 1)  & 0x1F) : D3D12_BLEND_ONE;
    b0->DestBlend      = (blend & 1) ? (D3D12_BLEND)((blend >> 6)  & 0x1F) : D3D12_BLEND_ZERO;
    b0->BlendOp        = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 21) & 0x7) : D3D12_BLEND_OP_ADD;
    b0->SrcBlendAlpha  = (blend & 1) ? (D3D12_BLEND)((blend >> 11) & 0x1F) : D3D12_BLEND_ONE;
    b0->DestBlendAlpha = (blend & 1) ? (D3D12_BLEND)((blend >> 16) & 0x1F) : D3D12_BLEND_ZERO;
    b0->BlendOpAlpha   = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 24) & 0x7) : D3D12_BLEND_OP_ADD;
    b0->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd->SampleMask = UINT_MAX;
    pd->PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd->NumRenderTargets = (UINT)r->nrt;
    for (int _r = 0; _r < (int)r->nrt && _r < 8; _r++) {
        pd->RTVFormats[_r] = (DXGI_FORMAT)r->rtfmt;
        /* Zero-init leaves RenderTargetWriteMask 0 on secondary targets --
         * every MRT-B write would be masked off. Mirror RT0's blend state. */
        pd->BlendState.RenderTarget[_r] = *b0;
    }
    /* Guest colour write mask (RGBA nibble, already D3D-ordered); a literal 0
     * is a depth-only pass and is honoured. */
    for (int _r = 0; _r < (int)r->nrt && _r < 8; _r++)
        pd->BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(r->cmask & 0xF);
    pd->DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd->DepthStencilState.DepthEnable = r->depth_enable ? TRUE : FALSE;
    pd->DepthStencilState.DepthWriteMask = r->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd->DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)r->depth_func;
    pd->DepthStencilState.StencilEnable = FALSE;
    if (r->stencil) {   /* v178 */
        const u64 k = r->stencil;
        D3D12_DEPTH_STENCIL_DESC* ds = &pd->DepthStencilState;
        ds->StencilEnable = TRUE;
        ds->StencilReadMask = (UINT8)(k & 0xFF); ds->StencilWriteMask = (UINT8)((k >> 8) & 0xFF);
        ds->FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)((k >> 16) & 15);
        ds->FrontFace.StencilFailOp = (D3D12_STENCIL_OP)((k >> 20) & 15);
        ds->FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)((k >> 24) & 15);
        ds->FrontFace.StencilPassOp = (D3D12_STENCIL_OP)((k >> 28) & 15);
        ds->BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)((k >> 32) & 15);
        ds->BackFace.StencilFailOp = (D3D12_STENCIL_OP)((k >> 36) & 15);
        ds->BackFace.StencilDepthFailOp = (D3D12_STENCIL_OP)((k >> 40) & 15);
        ds->BackFace.StencilPassOp = (D3D12_STENCIL_OP)((k >> 44) & 15);
        /* D3D12 runs the stencil test only with DepthEnable; keep depth as-is
         * but enabled (ALWAYS, no write) when the guest had it off. */
        if (!ds->DepthEnable) { ds->DepthEnable = TRUE; ds->DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; ds->DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; }
    }
    pd->SampleDesc.Count = 1;
}
static void psom_append(const PsoRec* r)
{
    if (!pso_manifest_on()) return;
    static int failed = 0;
    if (!s_psom_f && !failed) {
        CreateDirectoryA("shadercache", NULL);
        s_psom_f = fopen(psom_path(), "ab");
        if (!s_psom_f) { failed = 1; return; }
        fseek(s_psom_f, 0, SEEK_END);
        if (ftell(s_psom_f) == 0) fwrite(s_psom_magic, 1, sizeof s_psom_magic, s_psom_f);
    }
    if (!s_psom_f) return;
    fwrite(r, sizeof *r, 1, s_psom_f);
    fflush(s_psom_f);
}
/* A shader's bytecode for the builder thread, read once per key. */
typedef struct { u64 key; void* p; SIZE_T n; } PsowBlob;
static int psow_blob(PsowBlob* tab, u32 cap, u64 key, const void** p, SIZE_T* n)
{
    u32 i = (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) % cap;
    for (u32 k = 0; k < cap; k++, i = (i + 1u) % cap) {
        if (tab[i].key == key) { *p = tab[i].p; *n = tab[i].n; return tab[i].p != NULL; }
        if (!tab[i].key) break;
    }
    if (!tab[i].key) {
        char path[64]; snprintf(path, sizeof path, "shadercache\\%016llX.dxbc", (unsigned long long)key);
        void* buf = NULL; SIZE_T sz = 0;
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li; DWORD got = 0;
            if (GetFileSizeEx(f, &li) && li.QuadPart > 0 && li.QuadPart < (64 << 20) &&
                (buf = malloc((size_t)li.QuadPart)) != NULL &&
                ReadFile(f, buf, (DWORD)li.QuadPart, &got, NULL) && got == (DWORD)li.QuadPart)
                sz = (SIZE_T)li.QuadPart;
            else { free(buf); buf = NULL; }
            CloseHandle(f);
        }
        tab[i].key = key; tab[i].p = buf; tab[i].n = sz;
        *p = buf; *n = sz;
        return buf != NULL;
    }
    return 0;   /* table full */
}
static DWORD WINAPI psow_thread(LPVOID unused)
{
    (void)unused;
    const u32 cap = 8192;
    PsowBlob* tab = (PsowBlob*)calloc(cap, sizeof(PsowBlob));
    if (!tab) return 0;
    LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0);
    u32 built = 0, missing = 0, failed = 0, taken = 0;
    for (u32 i = 0; i < s_psow_nrec; i++) {
        const PsoRec* r = &s_psow_recs[i];
        const u64 h = pso_rec_hash(r);
        AcquireSRWLockShared(&s_psow_lock);
        PsoWarm* e = psow_slot(r, h, 0);
        const int st = e ? e->state : 0;
        ReleaseSRWLockShared(&s_psow_lock);
        if (st != 1) { if (st == 3) taken++; continue; }
        const void* vs; SIZE_T vsn; const void* ps; SIZE_T psn;
        if (!psow_blob(tab, cap, r->vs_key, &vs, &vsn) || !psow_blob(tab, cap, r->ps_key, &ps, &psn)) {
            missing++; continue;
        }
        D3D12_INPUT_ELEMENT_DESC il[16];
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        pso_desc_from_rec(&pd, r, vs, vsn, ps, psn, il);
        ID3D12PipelineState* pso = NULL;
        if (FAILED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso)) || !pso) { failed++; continue; }
        AcquireSRWLockExclusive(&s_psow_lock);
        e = psow_slot(r, h, 0);
        if (e && e->state == 1) { e->pso = pso; e->state = 2; pso = NULL; built++; }
        ReleaseSRWLockExclusive(&s_psow_lock);
        if (pso) { pso->lpVtbl->Release(pso); taken++; }   /* the render thread built its own meanwhile */
    }
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
    fprintf(stderr, "[PSOWARM] %u pipelines pre-built in %.0f ms (%u already compiled by the render thread,"
                    " %u with a shader missing from shadercache, %u failed)%c",
            built, (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart, taken, missing, failed, 10);
    for (u32 i = 0; i < cap; i++) free(tab[i].p);
    free(tab);
    return 0;
}
/* First pipeline request of the run: list the manifest (synchronously, so a
 * pipeline met before the builder gets to it is not recorded twice), then
 * start the builder. */
static void psow_start(void)
{
    static volatile LONG started = 0;
    if (started || InterlockedCompareExchange(&started, 1, 0) != 0) return;
    if (!pso_manifest_on()) return;
    FILE* f = fopen(psom_path(), "rb");
    if (!f) { fprintf(stderr, "[PSOWARM] no pipeline manifest yet (%s): recording one%c", psom_path(), 10); return; }
    u8 hdr[8]; u32 cap = 0, dup = 0;
    if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, s_psom_magic, 8)) {
        PsoRec r;
        while (fread(&r, sizeof r, 1, f) == 1) {
            const u64 h = pso_rec_hash(&r);
            AcquireSRWLockExclusive(&s_psow_lock);
            PsoWarm* e = psow_slot(&r, h, 1);
            const int fresh = e && !e->state;
            if (fresh) e->state = 1;
            ReleaseSRWLockExclusive(&s_psow_lock);
            if (!fresh) { dup++; continue; }
            if (s_psow_nrec == cap) {
                const u32 nc = cap ? cap * 2 : 1024;
                PsoRec* q = (PsoRec*)realloc(s_psow_recs, (size_t)nc * sizeof(PsoRec));
                if (!q) break;
                s_psow_recs = q; cap = nc;
            }
            s_psow_recs[s_psow_nrec++] = r;
        }
    } else fprintf(stderr, "[PSOWARM] %s has a different format: not used%c", psom_path(), 10);
    fclose(f);
    fprintf(stderr, "[PSOWARM] pipeline manifest: %u pipelines (%u duplicates)%s%c", s_psow_nrec, dup,
            pso_prewarm_on() ? ", pre-building them in the background" : " -- RSX_PSO_PREWARM=0, not pre-built", 10);
    if (pso_prewarm_on() && s_psow_nrec) {
        HANDLE t = CreateThread(NULL, 1u << 20, psow_thread, NULL, 0, NULL);
        if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
    }
}
/* The render thread's side: a pre-built pipeline for this record, or NULL.
 * *is_new: the record was in no manifest -- the caller appends it once built. */
static ID3D12PipelineState* psow_claim(const PsoRec* r, int* is_new)
{
    *is_new = 0;
    if (!pso_manifest_on()) return NULL;
    psow_start();
    const u64 h = pso_rec_hash(r);
    ID3D12PipelineState* p = NULL;
    AcquireSRWLockExclusive(&s_psow_lock);
    PsoWarm* e = psow_slot(r, h, 1);
    if (!e) *is_new = 1;
    else if (!e->state) { e->state = 3; *is_new = 1; }
    else if (e->state == 2) { p = e->pso; e->pso = NULL; e->state = 3; }
    else if (e->state == 1) e->state = 3;     /* the builder has not reached it: compile here */
    ReleaseSRWLockExclusive(&s_psow_lock);
    return p;
}

/* v142: a program whose constants read fp_k[] (RSX_FP_KSLOTS) needs fp_k in
 * its b1 declaration. The decompiler and the alpha-test patcher each declare
 * the parts of the per-draw b1 slot they use (shadow scale, alpha test) under
 * one cbuffer; replace whichever of their forms is present with the whole
 * layout vp_record_cb writes: texscale[4], alphatest, fp_k[64], shscale. */
static void fp_unify_b1(char* hlsl, size_t cap)
{
    if (!strstr(hlsl, "fp_k[")) return;
    static const char* const forms[] = {
        "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[69]; float4 rsx_shscale; };\n",
        "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[4]; float4 rsx_alphatest; float4 _rsx_b1pad2[64]; float4 rsx_shscale; };\n",
        "cbuffer RsxAlphaTest : register(b1) { float4 _rsx_atpad[4]; float4 rsx_alphatest; };\n",
    };
    static const char uni[] =
        "cbuffer RsxB1 : register(b1) { float4 rsx_texscale[4]; float4 rsx_alphatest; float4 fp_k[64]; float4 rsx_shscale; };\n";
    /* v145: the unified block goes where the FIRST removed declaration was --
     * the shadow helpers (_rsx_shadowN) sit between it and main() and read
     * rsx_shscale, so inserting at main() left them with an undeclared
     * identifier and the program failed to compile (8 shadow-receiving
     * programs in York: their geometry was never drawn). */
    size_t first_off = (size_t)-1;
    for (int f = 0; f < 3; f++) {
        char* at;
        while ((at = strstr(hlsl, forms[f])) != NULL) {
            const size_t n = strlen(forms[f]);
            if ((size_t)(at - hlsl) < first_off) first_off = (size_t)(at - hlsl);
            memmove(at, at + n, strlen(at + n) + 1);
        }
    }
    if (strstr(hlsl, "register(b1)")) {
        static int warned = 0;
        if (warned < 4 && ++warned) fprintf(stderr, "[FPK] unknown b1 declaration left in a program using fp_k%c", 10);
    }
    char* ins = NULL;
    const char* marks[] = { "struct PSOutput", "float4 main(", "PSOutput main(" };
    for (int m = 0; m < 3; m++) { char* q = strstr(hlsl, marks[m]); if (q && (!ins || q < ins)) ins = q; }
    if (first_off != (size_t)-1 && first_off <= strlen(hlsl) && (!ins || hlsl + first_off < ins)) ins = hlsl + first_off;
    if (!ins) return;
    const size_t len = strlen(hlsl), ul = sizeof uni - 1;
    if (len + ul + 1 > cap) return;
    memmove(ins + ul, ins, strlen(ins) + 1);
    memcpy(ins, uni, ul);
}

/* v142: pipelines built on worker threads (RSX_PSO_ASYNC, default 1).
 * CreateGraphicsPipelineState for a pipeline the driver has not seen costs
 * 3-30 ms, and it ran on the render thread in the middle of a frame: Grimsby's
 * intro camera move met ~180 new pipelines within half a second, the render
 * thread stalled for ~400 ms, the FIFO backed up behind it, and the main
 * thread's wait for an occlusion report (func_00091CA0) ran into its 200 ms
 * timeout -- the stutter. A new pipeline is now queued to a worker and its
 * draws are skipped until it is ready (the object pops in a frame or two
 * later), the same rule the shader compiles already follow. PsoWarm states
 * added: 4 queued/building, 5 failed. */
typedef struct { PsoRec rec; u64 h; ID3DBlob* vs; ID3DBlob* ps; u32 fp; int is_new; } PsoaJob;
#define PSOA_Q 2048u
static PsoaJob s_psoa_q[PSOA_Q];
static u32 s_psoa_head, s_psoa_tail;
static SRWLOCK s_psoa_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_psoa_cv = CONDITION_VARIABLE_INIT;
static SRWLOCK s_psom_wlock = SRWLOCK_INIT;
static volatile LONG s_psoa_built, s_psoa_skip, s_psoa_fail;
static int pso_async_on(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_PSO_ASYNC"); e = (v ? atoi(v) : 1) && pso_manifest_on(); }
    return e;
}
static DWORD WINAPI psoa_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        AcquireSRWLockExclusive(&s_psoa_lock);
        while (s_psoa_head == s_psoa_tail)
            SleepConditionVariableSRW(&s_psoa_cv, &s_psoa_lock, INFINITE, 0);
        PsoaJob j = s_psoa_q[s_psoa_tail % PSOA_Q];
        s_psoa_tail++;
        ReleaseSRWLockExclusive(&s_psoa_lock);
        D3D12_INPUT_ELEMENT_DESC il[16];
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        pso_desc_from_rec(&pd, &j.rec, j.vs->lpVtbl->GetBufferPointer(j.vs), j.vs->lpVtbl->GetBufferSize(j.vs),
                          j.ps->lpVtbl->GetBufferPointer(j.ps), j.ps->lpVtbl->GetBufferSize(j.ps), il);
        ID3D12PipelineState* pso = NULL;
        const uint64_t ftp = rft0();
        LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0);
        const HRESULT hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
            s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
        rft(18, j.fp, 1u, ftp);
        const int ok = SUCCEEDED(hr) && pso;
        AcquireSRWLockExclusive(&s_psow_lock);
        PsoWarm* e = psow_slot(&j.rec, j.h, 0);
        if (e && e->state == 4) {
            if (ok) { e->pso = pso; e->state = 2; pso = NULL; }
            else e->state = 5;
        }
        ReleaseSRWLockExclusive(&s_psow_lock);
        if (pso) pso->lpVtbl->Release(pso);
        if (ok) {
            InterlockedIncrement(&s_psoa_built);
            InterlockedIncrement(&s_pso_new);
            if (j.is_new) { AcquireSRWLockExclusive(&s_psom_wlock); psom_append(&j.rec); ReleaseSRWLockExclusive(&s_psom_wlock); }
        } else {
            InterlockedIncrement(&s_psoa_fail);
            static volatile LONG nf = 0;
            if (InterlockedIncrement(&nf) <= 8)
                fprintf(stderr, "[PSOASYNC] pipeline for fp=0x%08X failed (0x%08lX)%c", j.fp, (long)hr, 10);
        }
        j.vs->lpVtbl->Release(j.vs); j.ps->lpVtbl->Release(j.ps);
    }
    return 0;
}
static void psoa_start(void)
{
    static volatile LONG st = 0;
    if (st || InterlockedCompareExchange(&st, 1, 0) != 0) return;
    const char* v = getenv("RSX_PSO_ASYNC_THREADS");
    int n = v ? atoi(v) : 2; if (n < 1) n = 1; if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        HANDLE t = CreateThread(NULL, 1u << 20, psoa_worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    fprintf(stderr, "[PSOASYNC] %d pipeline worker(s)%c", n, 10);
}
/* 0 = *out is a ready pipeline, 1 = queued or building (skip the draw),
 * 2 = it failed to build, 3 = build it here (the old way). */
static int psoa_claim(const PsoRec* r, ID3DBlob* vs, ID3DBlob* ps, u32 fp, ID3D12PipelineState** out)
{
    *out = NULL;
    psow_start();
    psoa_start();
    const u64 h = pso_rec_hash(r);
    int res = 3, is_new = 0;
    AcquireSRWLockExclusive(&s_psow_lock);
    PsoWarm* e = psow_slot(r, h, 1);
    if (e) {
        switch (e->state) {
        case 2: *out = e->pso; e->pso = NULL; e->state = 3; res = 0; break;
        case 4: res = 1; break;
        case 5: res = 2; break;
        case 0: is_new = 1; /* fall through */
        case 1:
            AcquireSRWLockExclusive(&s_psoa_lock);
            if (s_psoa_head - s_psoa_tail < PSOA_Q) {
                PsoaJob* j = &s_psoa_q[s_psoa_head % PSOA_Q];
                j->rec = *r; j->h = h; j->vs = vs; j->ps = ps; j->fp = fp; j->is_new = is_new;
                vs->lpVtbl->AddRef(vs); ps->lpVtbl->AddRef(ps);
                s_psoa_head++;
                WakeConditionVariable(&s_psoa_cv);
                e->state = 4; res = 1;
            } else { e->state = 3; res = 3; }
            ReleaseSRWLockExclusive(&s_psoa_lock);
            break;
        default: res = 3; break;   /* 3: taken earlier -- the in-memory cache lost it */
        }
    }
    ReleaseSRWLockExclusive(&s_psow_lock);
    if (res == 1) InterlockedIncrement(&s_psoa_skip);
    return res;
}
static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, int exp32, u32 cmask, u32 cull,
                                          u32 cube_mask)
{
    if (nrt < 1) nrt = 1; if (nrt > 4) nrt = 4;
    if (rtfmt == 0) rtfmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || !vm_base) return NULL;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return NULL; }
    /* Resolve the VS: the draw's cached per-VP blob, else the primary. */
    ID3DBlob* vsb = NULL; u32 vs_hash = 0;
    /* vs_idx >= VP_VS_CACHE: the GPU-vertex-fetch variant of slot vs_idx - VP_VS_CACHE
     * (kept distinct in the pipeline key by the index itself). */
    const int gfv = vs_idx >= VP_VS_CACHE;
    { const int vreal = gfv ? vs_idx - VP_VS_CACHE : vs_idx;
      if (vreal >= 0 && vreal < s_d3d.vp_vs_n) {
          if (s_d3d.vp_vs[vreal].pending) return NULL;   /* compiling: skip the draw */
          vsb = gfv ? s_d3d.vp_vs[vreal].vs_gf : s_d3d.vp_vs[vreal].vs;
          vs_hash = s_d3d.vp_vs[vreal].hash;
      } }
    if (!vsb && gfv) return NULL;
    if (!vsb) { vsb = s_d3d.vp_vs_blob; vs_idx = -1; }
    if (!vsb) return NULL;

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return NULL;

    /* Content hash: inline constants are patched in place per frame (wave's
     * stamp), so identity is the BYTES, not the address. */
    /* Hash the CODE, not the constants: with fp_k[] hoisted into the per-draw
     * constant buffer the compiled shader is invariant under constant changes,
     * so hashing them made the pipeline key move every draw. */
    u32 uhash;
    /* Resistance port: once per program per batch. Every draw re-scanned and
     * re-hashed its program's microcode, twice (prepass + replay) -- ~20% of
     * the render thread at ~5k draws a frame. RSX_FPHASH_MEMO=0 off. */
    static u32 s_fhm_key[1024], s_fhm_val[1024], s_fhm_bat[1024];
    static int s_fhm_en = -1;
    if (s_fhm_en < 0) { const char* e = getenv("RSX_FPHASH_MEMO"); s_fhm_en = e ? atoi(e) : 1; }
    const u32 fhm_i = ((off >> 4) * 2654435761u) >> 22;
    const u32 fhm_bat = s_submit_no + 1u;
    if (s_fhm_en && s_fhm_key[fhm_i] == off + 1u && s_fhm_bat[fhm_i] == fhm_bat)
        uhash = s_fhm_val[fhm_i];
    else
    {
        u32 usz = rsx_fp_program_size(vm_base + off, 4096);
        if (usz == 0) usz = 64;
        s_perf_pso_hashbytes += (int)usz;
        if (getenv("FP_CONSTBUF") && getenv("FP_CONSTBUF")[0] == '0') {
            uhash = 2166136261u;
            const u8* up = vm_base + off;
            for (u32 i = 0; i < usz; i++) { uhash ^= up[i]; uhash *= 16777619u; }
        } else {
            uhash = rsx_fp_code_hash(vm_base + off, 4096);
        }
        if (s_fhm_en) { s_fhm_key[fhm_i] = off + 1u; s_fhm_val[fhm_i] = uhash; s_fhm_bat[fhm_i] = fhm_bat; }
    }

    s_perf_pso_calls++;
    /* Keyed on the code, not on where it lives. The compiled shader reads its
     * inline constants from the draw's own fp_addr (fp_k[], refilled per draw),
     * so a program's pipeline is the same wherever the title puts it. God of
     * War HD uploads its fragment programs into a ring every frame: keyed on the
     * address, every draw of every frame missed and went through a full
     * decompile + D3DCompile (179 draws -> seconds per frame on its menu). */
    u32 _kb;
    { u32 h = 2166136261u;
      #define FPK(x) { h ^= (u32)(x); h *= 16777619u; }
      FPK(uhash) FPK(vs_hash) FPK(vs_idx) FPK(blend) FPK(nrt) FPK(rtfmt) FPK(exp32)
      FPK(cmask) FPK(cull) FPK(cube_mask) FPK(s_fp_unnorm_key) FPK(s_d3d.vp_gen)
      #undef FPK
      _kb = (h ^ (h >> 16)) & (VP_FP_HASH - 1); }
    for (int i = s_fp_head[_kb] - 1; i >= 0; i = s_d3d.vp_fp[i].next - 1)
        if (s_d3d.vp_fp[i].ucode_hash != uhash) continue;   /* cheap reject first */
        else if (s_d3d.vp_fp[i].vs_idx == vs_idx &&
            s_d3d.vp_fp[i].vs_hash == vs_hash && s_d3d.vp_fp[i].gen == s_d3d.vp_gen &&
            s_d3d.vp_fp[i].blend == blend && s_d3d.vp_fp[i].nrt == nrt &&
            s_d3d.vp_fp[i].rtfmt == (u32)rtfmt && s_d3d.vp_fp[i].exp32 == exp32 &&
            s_d3d.vp_fp[i].ucode_hash == uhash && s_d3d.vp_fp[i].cmask == cmask &&
            s_d3d.vp_fp[i].cull == cull &&
            s_d3d.vp_fp[i].cube_mask == cube_mask &&
            s_d3d.vp_fp[i].unorm_key == s_fp_unnorm_key)
            return s_d3d.vp_fp[i].pso;
    s_perf_pso_miss++;      /* falls through to a full decompile + D3DCompile */
    /* PSOMISSDBG=1: on a miss, name the key field that differs from an existing
     * entry for the same program. "the cache misses" is not actionable; "it
     * misses on ucode_hash" is. */
    /* HASHDBG=1: how many DISTINCT ucode hashes each program produces. Inline
     * constants are patched into the shader bytes, so a program whose constants
     * animate yields a new hash -- and a new pipeline -- every time. */
    { static int hd = -1;
      if (hd < 0) { const char* e = getenv("HASHDBG"); hd = e ? atoi(e) : 0; }
      if (hd) { static u32 fps[32]; static u32 cnt[32]; static int nf = 0;
        int idx = -1;
        for (int i = 0; i < nf; i++) if (fps[i] == fp_addr) idx = i;
        if (idx < 0 && nf < 32) { idx = nf++; fps[idx] = fp_addr; cnt[idx] = 0; }
        if (idx >= 0) { cnt[idx]++;
          if ((cnt[idx] % 200) == 0)
            fprintf(stderr, "[HASHDBG] fp=0x%X has produced %u distinct compiles%c",
                    fp_addr, cnt[idx], 10); } } }
    { static int md = -1;
      if (md < 0) { const char* e = getenv("PSOMISSDBG"); md = e ? atoi(e) : 0; }
      if (md) { static int n = 0;
        for (int i = 0; i < s_d3d.vp_fp_n && n < 12; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->fp_addr != fp_addr) continue;
            n++;
            fprintf(stderr, "[PSOMISS] fp=0x%X differs:%s%s%s%s%s%s%s%s%c", fp_addr,
                    q->vs_idx != vs_idx        ? " vs_idx"     : "",
                    q->vs_hash != vs_hash      ? " vs_hash"    : "",
                    q->gen != s_d3d.vp_gen     ? " gen"        : "",
                    q->blend != blend          ? " blend"      : "",
                    q->nrt != nrt              ? " nrt"        : "",
                    q->rtfmt != (u32)rtfmt     ? " rtfmt"      : "",
                    q->ucode_hash != uhash     ? " ucode_hash" : "",
                    q->cube_mask != cube_mask  ? " cube_mask"  : "", 10);
            break;
        } } }
    /* CUBEKEYDBG=1: which (program, cube mask) pairs are being compiled. If one
     * program shows several masks, the mask is unstable and is the reason the
     * pipeline cache thrashes with cube support on. */
    { static int ck = -1;
      if (ck < 0) { const char* e = getenv("CUBEKEYDBG"); ck = e ? atoi(e) : 0; }
      if (ck) { static u32 seen[64][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++)
            if (seen[i][0] == fp_addr && seen[i][1] == cube_mask) known = 1;
        if (!known && ns < 64) { seen[ns][0] = fp_addr; seen[ns][1] = cube_mask; ns++;
            fprintf(stderr, "[CUBEKEY] fp=0x%X cube_mask=0x%X (distinct pairs=%d)%c",
                    fp_addr, cube_mask, ns, 10); } } }
    static char hlsl[RSX_FP_HLSL_BUFFER_SIZE];
    ID3DBlob* pb = NULL; ID3DBlob* e = NULL; HRESULT hr;
    FPBlobEntry* _be = &s_fp_blob[fp_blob_slot(uhash, (u32)exp32, s_fp_unnorm_key, cube_mask)];
    { static int nob = -1; if (nob < 0) nob = getenv("FP_BLOB_OFF") ? 1 : 0;
      if (!nob && _be->used && _be->uhash == uhash && _be->exp32 == (u32)exp32 &&
          _be->unorm == s_fp_unnorm_key && _be->cube == cube_mask) {
          if (_be->pending) return NULL;        /* compiling on a worker: skip the draw */
          if (!_be->blob) return NULL;          /* known not to compile */
          pb = _be->blob; pb->lpVtbl->AddRef(pb);
          goto have_ps;
      } }
    AcquireSRWLockExclusive(&s_vs_lock);      /* an async FP job may publish into this slot */
    _be->used = 1; _be->uhash = uhash; _be->exp32 = (u32)exp32;
    _be->unorm = s_fp_unnorm_key; _be->cube = cube_mask;
    _be->pending = 0;
    if (_be->blob) { _be->blob->lpVtbl->Release(_be->blob); _be->blob = NULL; }
    ReleaseSRWLockExclusive(&s_vs_lock);
    s_perf_fp_compiles++;
    /* The third parameter is the SET_SHADER_CONTROL word; its 0x40 bit picks
     * the export registers (r0/r2/r3/r4 for 32-bit, h0/h4/h6/h8 for 16-bit).
     * Each draw records that bit (fp_exp32, also part of the pipeline key), so
     * pass it rather than letting the decompiler guess from "was r0 written".
     * The guess fails on a program that uses r0 as a temp and exports through
     * h0: God of War HD's 2x-AA resolve computes (B-A)/2 in r0 and the average
     * in h0, and with r0 exported every 3D frame came out black. The depth
     * export bits aren't recorded, so they stay off as before.
     * RSX_FP_CTRL_AUTO=1 restores the guess. */
    static int ctrl_auto = -1;
    if (ctrl_auto < 0) ctrl_auto = getenv("RSX_FP_CTRL_AUTO") ? 1 : 0;
    u32 fp_ctrl = ctrl_auto ? RSX_FP_CTRL_AUTO
                            : (exp32 ? CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS : 0u);
    int anyx = 0; for (int u = 0; u < 16; u++) anyx |= s_fp_expand[u];
    int anys = 0; for (int u = 0; u < 16; u++) anys |= s_fp_shadow[u].enabled;
    extern int g_rsx_fp_kslot_mode;
    { static int ks = -1; if (ks < 0) { const char* e = getenv("RSX_FP_KSLOTS"); ks = e ? atoi(e) : 1; }
      g_rsx_fp_kslot_mode = ks; }
    int n = anys ? rsx_fp_decompile_shadow_expand_ex(vm_base + off, 4096, fp_ctrl, 0u, s_fp_shadow, s_fp_expand, hlsl, sizeof(hlsl))
          : anyx ? rsx_fp_decompile_expand_ex(vm_base + off, 4096, fp_ctrl, 0u, s_fp_expand, hlsl, sizeof(hlsl))
                 : rsx_fp_decompile(vm_base + off, 4096, fp_ctrl, hlsl, sizeof(hlsl));
    g_rsx_fp_kslot_mode = 0;
    if (n > 0) fp_kil_note(fp_addr, strstr(hlsl, "discard") != NULL);   /* v184: before the alpha-test suffix */
    { static int ns = 0; if (anys && ns < 6) { ns++;
        fprintf(stderr, "[SHADOWMAP] fp=0x%08X compiled with depth compare on units:", fp_addr);
        for (int u = 0; u < 16; u++) if (s_fp_shadow[u].enabled)
            fprintf(stderr, " t%d(func=%u lin=%u mode=%u)", u, s_fp_shadow[u].func, s_fp_shadow[u].linear, s_fp_shadow[u].enabled);
        fputc(10, stderr); } }
    { static int nx = 0; if (anyx && nx < 8) { nx++;
        fprintf(stderr, "[TEXEXPAND] fp=0x%08X compiled with sign expansion on units:", fp_addr);
        for (int u = 0; u < 16; u++) if (s_fp_expand[u]) fprintf(stderr, " t%d=0x%X", u, s_fp_expand[u]);
        fputc(10, stderr); } }
    if (n <= 0) { static int _e=0; if(PS3_LOG_FIRST(_e, 16)) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return NULL; }
    /* FP_LIST=1: every program the title actually compiles, with its size. A
     * fragment program that hangs the GPU shows up here as an implausible
     * instruction count long before it shows up as a TDR. */
    { static int fl = -1; if (fl < 0) fl = getenv("FP_LIST") ? 1 : 0;
      if (fl) fprintf(stderr, "[FPLIST] fp=0x%08X instrs=%d hlsl=%u bytes%c",
                      fp_addr, n, (unsigned)strlen(hlsl), 10); }
    /* FP_DUMP=<n>: the first n compiled programs (1 = the old default of 4). */
    if (getenv("FP_DUMP")) { static int _d=0, _cap=-1;
      if (_cap < 0) { _cap = atoi(getenv("FP_DUMP")); if (_cap <= 1) _cap = 4; }
      if (PS3_LOG_FIRST(_d, _cap)) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            const u8* uc = vm_base + off;
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (int _b = 0; _b < 32; _b++) fprintf(f, " %02X", uc[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* FP_PICK=<hex fp_addr>: write just that program, whatever order it compiles
     * in. fp_dump.hlsl keeps only the first four, same trap as vp2_dump.hlsl. */
    { static const char* fpk = (const char*)1; static u32 wantf = 0; static int done = 0;
      if (fpk == (const char*)1) { fpk = getenv("FP_PICK");
                                   wantf = fpk ? (u32)strtoul(fpk, NULL, 16) : 0; }
      if (wantf && !done && fp_addr == wantf) { done = 1;
          FILE* f = fopen("duck_fp.hlsl", "w");
          if (f) { fprintf(f, "/* fp_addr=0x%08X, %d instrs */%s", fp_addr, n, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKFP] wrote duck_fp.hlsl (fp=0x%08X, %d instrs)%c",
                  fp_addr, n, 10);
      } }

    /* v182: FP_PICKS=<hex,hex,...>: write each listed program once to
     * logs\\fp_<addr>.hlsl (needs FP_BLOB_OFF=1 so cached programs decompile). */
    { static const char* lst = (const char*)1; static u32 want[16]; static int nw = 0, done[16];
      if (lst == (const char*)1) { lst = getenv("FP_PICKS");
          for (const char* q = lst; q && *q && nw < 16; ) { want[nw++] = (u32)strtoul(q, (char**)&q, 16); while (*q == ',' || *q == ' ') q++; } }
      for (int i = 0; i < nw; i++) if (!done[i] && fp_addr == want[i]) { done[i] = 1;
          char fn[64]; snprintf(fn, sizeof fn, "logs\\fp_%08X.hlsl", fp_addr);
          FILE* f = fopen(fn, "w"); if (f) { fprintf(f, "/* fp_addr=0x%08X */\n%s", fp_addr, hlsl); fclose(f); } } }
    if (s_fp_unnorm_mask) {   /* v189: clamp texel coordinates to the declared size first */
        for (u32 u = 0; u < 16; u++) {
            if (!(s_fp_unnorm_mask & (1u << u)) || !s_fp_unnorm_decl[u][0]) continue;
            char nd[48]; snprintf(nd, sizeof nd, "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
            for (char* at = strstr(hlsl, nd); at; at = strstr(at + 1, nd)) {
                char* arg = at + strlen(nd);
                char* open = strchr(at, '('); int depth = 0; char* q = open;
                for (; *q; q++) { if (*q == '(') depth++; else if (*q == ')') { if (!--depth) break; } }
                if (*q != ')') break;
                char suf[96]; snprintf(suf, sizeof suf, "), float2(0.5, 0.5), float2(%.1f, %.1f))",
                                       (double)s_fp_unnorm_decl[u][0] - 0.5, (double)s_fp_unnorm_decl[u][1] - 0.5);
                static const char pre[] = " clamp((";
                const size_t sl = strlen(suf), pl = sizeof pre - 1, len = strlen(hlsl);
                if (len + sl + pl + 1 > sizeof hlsl) break;
                memmove(q + sl, q, strlen(q) + 1); memcpy(q, suf, sl);
                memmove(arg + pl, arg, strlen(arg) + 1); memcpy(arg, pre, pl);
            }
        }
        int np = rsx_fp_apply_unnorm_scale(hlsl, sizeof hlsl, s_fp_unnorm_mask,
                                           (const u32 (*)[2])s_fp_unnorm_dim, cube_mask);
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 6))
            fprintf(stderr, "[UNNORM] fp=0x%X units 0x%X: %d sample(s) rescaled%c",
                    fp_addr, s_fp_unnorm_mask, np, 10);
    }
    /* v137: after the unnorm rescale, whose needles name rsx_samp[u]. */
    if (s_fp_clamp_mask) {
        int nc = fp_apply_clamp_samplers(hlsl, sizeof hlsl, s_fp_clamp_mask);
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[TEXCLAMP] fp=0x%X units 0x%X: %d sample(s) clamped%c", fp_addr, s_fp_clamp_mask, nc, 10);
    }
    if (s_fp_mirror_mask) {   /* v189: mirrored addressing (see s_fp_mirror) */
        int nm = 0;
        if (fp_str_replace(hlsl, sizeof hlsl, "SamplerState rsx_samp[16] : register(s0);",
                "SamplerState rsx_samp[16] : register(s0);\n"
                "float4 rsx_sampm(Texture2D t, SamplerState s, float2 uv, float4 m) {"
                " float2 mr = 1.0 - abs((uv - 2.0 * floor(uv * 0.5)) - 1.0); float2 mo = saturate(abs(uv));"
                " float2 a = uv; a = (m.xy > 0.5) ? mr : a; a = (m.zw > 0.5) ? mo : a; return t.Sample(s, a); }") == 1) {
            for (u32 u = 0; u < 16; u++) {
                if (!(s_fp_mirror_mask & (1u << u))) continue;
                char nd[48]; snprintf(nd, sizeof nd, "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
                const u8 f = s_fp_mirror[u];
                char tail[64]; snprintf(tail, sizeof tail, ", float4(%d, %d, %d, %d))", f & 1, (f >> 1) & 1, (f >> 2) & 1, (f >> 3) & 1);
                char rpl[64]; snprintf(rpl, sizeof rpl, "rsx_sampm(rsx_tex[%u], rsx_samp[%u],", u, u);
                const size_t tl = strlen(tail), rl = strlen(rpl), nl = strlen(nd);
                for (char* at = strstr(hlsl, nd); at; at = strstr(hlsl, nd)) {
                    char* open = strchr(at, '('); int depth = 0; char* q = open;
                    for (; *q; q++) { if (*q == '(') depth++; else if (*q == ')') { if (!--depth) break; } }
                    if (*q != ')') break;
                    if (strlen(hlsl) + tl + rl + 4 > sizeof hlsl) break;
                    memmove(q + tl, q + 1, strlen(q + 1) + 1); memcpy(q, tail, tl);   /* ")" -> ", float4(..))" */
                    memmove(at + rl, at + nl, strlen(at + nl) + 1); memcpy(at, rpl, rl);
                    nm++;
                }
            }
        }
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[MIRROR] fp=0x%X units 0x%X: %d sample(s)%c", fp_addr, s_fp_mirror_mask, nm, 10);
    }
    /* Resistance port: the alpha test. The per-draw b1 slot has carried
     * rsx_alphatest (enable/ref/func) all along, but no program read it, so
     * every alpha-tested draw was solid: York's weeds as opaque crumpled
     * cards, cut-out fences, decal edges. Applied to every program, driven by
     * the draw's state. RSX_ALPHATEST=0 off. */
    { static int at = -1; if (at < 0) { const char* e = getenv("RSX_ALPHATEST"); at = e ? atoi(e) : 1; }
      if (at) { int r = rsx_fp_apply_alpha_test_dynamic(hlsl, sizeof hlsl);
          static int _n = 0; if (r < 0 && PS3_LOG_FIRST(_n, 8))
              fprintf(stderr, "[ALPHATEST] fp=0x%X: could not add the alpha test%c", fp_addr, 10); } }
    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          /* Match the assignment, not one exact spelling of it: the final
           * register is r[N] for some programs and h[N] for others, and the old
           * fixed "= r[0];" anchor silently did nothing for every h[] shader --
           * so this switch appeared to have no effect and was useless for
           * identifying which program draws what. Rewrite the expression with a
           * proper length-changing splice. */
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) {
              char* semi = strchr(rp, ';');
              if (semi) {
                  static const char rep[] = "_po.c0 = (1.0).xxxx";
                  size_t oldlen = (size_t)(semi - rp), newlen = sizeof(rep) - 1;
                  size_t tail = strlen(semi) + 1;
                  if ((rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                      memmove(rp + newlen, semi, tail);
                      memcpy(rp, rep, newlen);
                  }
                  (void)oldlen;
              }
          }
      } }
    /* FP_TEX=1: make every program output its unit-0 texture sample verbatim.
     * Separates "the texture is wrong" from "the shading tints it" in one run,
     * without having to identify which program draws which surface first. */
    /* FP_SHOW=<hlsl expr>: replace a program's colour output with an arbitrary
     * expression (e.g. FP_SHOW=h[1]) so an intermediate register can be looked
     * at directly. Restrict with FP_TEX_FP=<hex>. Reading a 69-instruction
     * shader to guess which term carries a bad value is slower and less certain
     * than displaying the terms. */
    /* FP_KILL=<hex fp_addr>: make that program discard every fragment, so what
     * it covers becomes visible. Distinguishes "this surface shades black" from
     * "this surface is correct and something else is missing behind it". */
    { const char* kf = getenv("FP_KILL");
      if (kf && (u32)strtoul(kf, NULL, 16) == fp_addr) {
          char* rp = strstr(hlsl, "PSOut _po;");
          if (rp) {
              const char* ins = "discard; ";
              size_t nl = strlen(ins), tail = strlen(rp) + 1;
              if ((size_t)(rp - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(rp + nl, rp, tail);
                  memcpy(rp, ins, nl);
                  fprintf(stderr, "[FPKILL] fp=0x%X discards%c", fp_addr, 10);
              }
          }
      } }
    { const char* sh = getenv("FP_SHOW");
      const char* only = getenv("FP_TEX_FP");
      if (sh && (!only || (u32)strtoul(only, NULL, 16) == fp_addr)) {
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) { char* semi = strchr(rp, ';');
            if (semi) {
                char rep[256];
                snprintf(rep, sizeof rep, "_po.c0 = float4((%s).xyz, 1.0)", sh);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPSHOW] fp=0x%X output replaced with %s%c",
                            fp_addr, sh, 10);
                }
            } }
      } }
    /* Retarget the samplers the guest bound as CUBE textures. The decompiler
     * always emits 2D samplers; a cube is sampled with a 3-component direction
     * and no texel-scale, and the emitted coordinate always ends in the fixed
     * tail ".xy * rsx_texscale[N].xy", so both edits are exact replacements. */
    if (cube_mask) {
        for (int _u = 0; _u < RSX_UNITS; _u++) {
            if (!(cube_mask & (1u << _u))) continue;
            char f1[64], r1[64], f2[64], r2[64];
            snprintf(f1, sizeof f1, "Texture2D    rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube  rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f1, sizeof f1, "Texture2D rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f2, sizeof f2, ".xy * rsx_texscale[%d].xy", _u);
            snprintf(r2, sizeof r2, ".xyz");
            hlsl_replace_all(hlsl, sizeof hlsl, f2, r2);
        }
        { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[CUBE] fp=0x%X compiled with cube units mask 0x%X%c",
                    fp_addr, cube_mask, 10); }
    }
    /* FP_IDCOLOR=1: give every fragment program a distinct flat colour derived
     * from its address. One frame then shows which program paints which surface,
     * instead of one run per candidate to test them by elimination. */
    if (getenv("FP_IDCOLOR")) {
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                /* Mix properly: every fp address in a title tends to share low
                 * bits (they all end 0x01/0x81 here), so a single multiply left
                 * one channel constant across all programs and made distinct
                 * shaders look like the same colour. */
                u32 hsh = fp_addr * 2654435761u;
                hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
                /* Force the top bit on in each channel: a hash that happens to
                 * land near black is indistinguishable from "this surface was
                 * never painted", which is the exact question the mode exists
                 * to answer. */
                float cr = (float)(((hsh >> 16) & 0x7F) | 0x80) / 255.0f;
                float cg = (float)(((hsh >>  8) & 0x7F) | 0x80) / 255.0f;
                float cb = (float)(((hsh      ) & 0x7F) | 0x80) / 255.0f;
                char rep[128];
                snprintf(rep, sizeof rep, "_po.c0 = float4(%.3f,%.3f,%.3f,1.0)", cr, cg, cb);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPID] fp=0x%X -> rgb(%.0f,%.0f,%.0f)%c",
                            fp_addr, cr*255, cg*255, cb*255, 10);
                }
            }
        }
    }
    /* FP_TEX=<unit>: output that unit's sample verbatim. FP_TEX_FP=<hex> limits
     * it to one program, so one surface can be inspected without repainting the
     * whole scene. FP_TEX_UV=1 samples with the same texcoord the program uses
     * for that unit rather than tc0. */
    if (getenv("FP_TEX") && (!getenv("FP_TEX_FP") ||
        (u32)strtoul(getenv("FP_TEX_FP"), NULL, 16) == fp_addr)) {
        int _un = atoi(getenv("FP_TEX"));
        if (_un < 0 || _un > 3) _un = 0;
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                char rep[160];
                /* FP_TEX_UV=1: sample at a fixed centre UV instead of the
                 * interpolated texcoord. If the texture is populated but the
                 * texcoords never arrive, the normal path samples texel (0,0)
                 * -- usually black -- and looks exactly like an empty texture.
                 * This tells the two apart. */
                if (getenv("FP_TEX_UV"))
                    snprintf(rep, sizeof rep,
                             "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, float2(0.5, 0.5))",
                             _un, _un);
                else
                snprintf(rep, sizeof rep,
                         "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, input.tc0.xy * rsx_texscale[%d].xy)",
                         _un, _un, _un);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                fprintf(stderr, "[FP_TEX] program 0x%X rewritten to sample unit %d%c",
                        fp_addr, _un, 10);
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                }
            }
        }
    }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        char* rp = strstr(hlsl, "return r[0];");
        if (rp) memcpy(rp, "return float4(1,0,1,1)", 23);
    }
    fp_unify_b1(hlsl, sizeof hlsl);
    { static int fs = -1; if (fs < 0) fs = getenv("FP_SAVE") ? 1 : 0;
      if (fs) { char fn[64]; snprintf(fn, sizeof fn, "logs/fp_%08X_%08X.hlsl", fp_addr, uhash);
                FILE* f = fopen(fn, "w"); if (f) { fputs(hlsl, f); fclose(f); } } }
    /* Resistance port: a program not in shadercache\ compiles on a worker
     * (see vsj_worker); its draws are skipped until it lands. */
    if (vs_async()) {
        hr = shc_compile_ex(hlsl, strlen(hlsl), "guest_fp", "main", "ps_5_0", &pb, &e, 1);
        if (FAILED(hr)) {
            if (e) { e->lpVtbl->Release(e); e = NULL; }
            if (pb) { pb->lpVtbl->Release(pb); pb = NULL; }
            VsJob j; memset(&j, 0, sizeof j);
            j.kind = 1; j.hlsl = hlsl; j.be = _be;
            j.k_uhash = uhash; j.k_exp32 = (u32)exp32; j.k_unorm = s_fp_unnorm_key; j.k_cube = cube_mask;
            _be->pending = 1;
            if (vsj_submit_job(&j)) return NULL;
            _be->pending = 0;
            hr = shc_compile(hlsl, strlen(hlsl), "guest_fp", "main", "ps_5_0", &pb, &e);
        }
    } else
    hr = shc_compile(hlsl, strlen(hlsl), "guest_fp", "main", "ps_5_0", &pb, &e);
    if (FAILED(hr) || !pb) {
        static int _e2=0; if (PS3_LOG_FIRST(_e2, 16))
            printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr,
                   e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?");
        if (e) e->lpVtbl->Release(e);
        return NULL;
    }
    if (e) e->lpVtbl->Release(e);
    _be->blob = pb; pb->lpVtbl->AddRef(pb);   /* the cache keeps its own ref */
have_ps:;

    /* v128: the pipeline as a record (PsoRec) -- the desc is built from it, so
     * one pre-built from the manifest is exactly the pipeline compiled here. */
    PsoRec rec; memset(&rec, 0, sizeof rec);
    rec.gfv = gfv ? 1 : 0;
    /* Guest face culling. CULL_OFF=1 forces double-sided, the old
     * unconditional behaviour -- geometry that vanishes under culling is a
     * winding/front-face problem, not a missing draw. */
    { static int _co = -1; if (_co < 0) _co = getenv("CULL_OFF") ? 1 : 0;
      rec.cull_mode = (u8)((!_co && (cull & 1u))
          ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
          : D3D12_CULL_MODE_NONE);
      /* CULL_FLIP=1: invert the front-face convention. Our VP path may hand
       * D3D the opposite winding from the guest's, in which case honouring
       * CULL_FACE culls exactly the faces it should keep. */
      static int _cfl = -1; if (_cfl < 0) _cfl = getenv("CULL_FLIP") ? 1 : 0;
      rec.front_ccw = (u8)((((cull & 4u) ? 1 : 0) ^ _cfl) ? 1 : 0); }
    { const u32 si = (cull >> 24) & 0xFFu;   /* v178 */
      rec.stencil = (si && (int)si < s_stn_n) ? s_stn[si] : 0; }
    { const u32 pi = (cull >> 16) & 0xFFu;
      if (pi && (int)pi < s_pofs_n) {
          /* v182: the slope term is per PIXEL of depth slope; at a render
           * scale above 1 every pixel covers less depth, so the guest's
           * factor (tuned for 720p) offset decals by a fraction of what it
           * did on the PS3 and floor decals cut out in patches where their
           * re-triangulated pieces sat a hair behind the floor (York blood
           * pools). Scale it with the render scale. RSX_POFS_SLOPE_MUL
           * overrides the multiplier (0 = no scaling). */
          static float mul = -1.0f;
          if (mul < 0.0f) { const char* e = getenv("RSX_POFS_SLOPE_MUL"); mul = e ? (float)atof(e) : 0.0f; }
          const float sm = mul > 0.0f ? mul : (rs_scale() > 1.0f ? rs_scale() : 1.0f);
          rec.slope_bias = s_pofs[pi][0] * sm;
          rec.depth_bias = (int32_t)(INT)s_pofs[pi][1];
      } }
    rec.blend = blend;
    rec.nrt = (u8)nrt;
    rec.rtfmt = (u32)rtfmt;
    /* CMASK_FORCE=1: ignore the guest colour mask (write RGBA on every draw).
     * A mis-decoded mask makes geometry rasterize correctly and write nothing,
     * which is indistinguishable from "the draw never happened". cmask is 0xF
     * by default (unset colour_mask register decodes to all-on); a literal 0
     * therefore means the guest EXPLICITLY masked every channel -- a depth-only
     * pass (e.g. DeferredShading's shadow-map generation). Honour it. */
    { static int _cf = -1; if (_cf < 0) _cf = getenv("CMASK_FORCE") ? 1 : 0;
      if (_cf) cmask = 0xF; }
    rec.cmask = (u8)(cmask & 0xF);
    /* Resistance port: honour the guest depth state packed by rsx_cull_key.
     * D3D12 needs DepthEnable for writes too, so "test off, write on" maps to
     * ALWAYS + write. RSX_DEPTH_FORCE=1 restores the old forced behaviour.
     * DEPTH_OFF=1: drop the depth test for guest-FP draws, so submission order
     * alone decides what is on top (diagnostic). */
    { int de = 1, dw = 1, df = (int)D3D12_COMPARISON_FUNC_LESS_EQUAL;
      static int dforce = -1;
      if (dforce < 0) dforce = getenv("RSX_DEPTH_FORCE") ? 1 : 0;
      if (!dforce && (cull & 0x100u)) {
          int test = (cull & 0x200u) != 0, write = (cull & 0x400u) != 0;
          if (!test && !write) { de = 0; dw = 0; }
          else {
              de = 1; dw = write ? 1 : 0;
              df = test ? (int)(((cull >> 12) & 7u) + 1u) : (int)D3D12_COMPARISON_FUNC_ALWAYS;
          }
      }
      static int doff = -1;
      if (doff < 0) { const char* de_env = getenv("DEPTH_OFF"); doff = de_env ? atoi(de_env) : 0; }
      if (doff) { de = 0; dw = 0; }
      rec.depth_enable = (u8)de; rec.depth_write = (u8)dw; rec.depth_func = (u8)df; }
    rec.vs_key = shk_get(vsb);
    rec.ps_key = shk_get(pb);
    const int rec_ok = rec.vs_key != 0 && rec.ps_key != 0;
    D3D12_INPUT_ELEMENT_DESC il[16];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    pso_desc_from_rec(&pd, &rec, vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb),
                      pb->lpVtbl->GetBufferPointer(pb), pb->lpVtbl->GetBufferSize(pb), il);

    ID3D12PipelineState* pso = NULL;
    int rec_new = 0, async_failed = 0;
    if (rec_ok && pso_async_on()) {
        const int st = psoa_claim(&rec, vsb, pb, fp_addr, &pso);
        if (st == 1) { pb->lpVtbl->Release(pb); return NULL; }   /* building on a worker: skip the draw */
        if (st == 2) async_failed = 1;
        if (st == 3) pso = psow_claim(&rec, &rec_new);
    } else if (rec_ok) pso = psow_claim(&rec, &rec_new);
    if (pso) { hr = S_OK; InterlockedIncrement(&s_psow_hits); }
    else if (async_failed) hr = E_FAIL;
    else
    { LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0); const uint64_t ftp = rft0();
      hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
          s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
      rft(18, fp_addr, 0u, ftp);
      QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
      s_pso_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
      InterlockedIncrement(&s_pso_new);
      if (rec_ok && rec_new && SUCCEEDED(hr) && pso) { AcquireSRWLockExclusive(&s_psom_wlock); psom_append(&rec); ReleaseSRWLockExclusive(&s_psom_wlock); } }
    pb->lpVtbl->Release(pb);
    if (FAILED(hr)) {
        static int _e3=0; if (PS3_LOG_FIRST(_e3, 16)) printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX)\n", fp_addr, hr);
        /* Resistance port: keep the failing program for inspection. */
        { static u32 dumped[16]; static int nd = 0; int seen = 0;
          for (int _i = 0; _i < nd; _i++) if (dumped[_i] == fp_addr) seen = 1;
          if (!seen && nd < 16) { dumped[nd++] = fp_addr;
              char fn[64]; snprintf(fn, sizeof fn, "logs/psofail_%08X.hlsl", fp_addr);
              FILE* f = fopen(fn, "w");
              if (f) { fprintf(f, "/* fp=0x%08X nrt=%d rtfmt=%u cmask=0x%X cull=0x%X vs_idx=%d */\n%s\n",
                               fp_addr, nrt, (unsigned)rtfmt, cmask, cull, vs_idx, hlsl);
                       if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) fprintf(f, "/* vs hash 0x%08X */\n", s_d3d.vp_vs[vs_idx].hash);
                       fclose(f); } } }
        pso = NULL;          /* cached as a failure: don't rebuild every draw */
    } else
    { static int _ok=0; if (PS3_LOG_FIRST(_ok, 32)) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }

    /* insert. Full: keep this PSO alive until the batch is done and flush the
     * whole table at the next batch boundary (fp_cache_flush). */
    if (s_d3d.vp_fp_n >= VP_FP_CACHE) {
        if (pso && s_fp_grave_n < 256) s_fp_grave[s_fp_grave_n++] = pso;
        s_fp_flush_pending = 1;
        return pso;
    }
    s_d3d.vp_fp[s_d3d.vp_fp_n].next = s_fp_head[_kb];
    s_fp_head[_kb] = s_d3d.vp_fp_n + 1;
    s_d3d.vp_fp[s_d3d.vp_fp_n].fp_addr = fp_addr;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_idx  = vs_idx;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_hash = vs_hash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].gen     = s_d3d.vp_gen;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend   = blend;
    s_d3d.vp_fp[s_d3d.vp_fp_n].nrt     = nrt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].rtfmt   = (u32)rtfmt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].exp32   = exp32;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ucode_hash = uhash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cmask   = cmask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cull    = cull;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cube_mask = cube_mask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].unorm_key = s_fp_unnorm_key;
    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;
    s_d3d.vp_fp_n++;
    return pso;
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap 1+slot) or -1. Must run while the
 * command list is open, before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * The crossbar decode itself is RSX semantics and lives in
 * rsx_texture_layout.c; all that is left here is packing the four selectors
 * into D3D12's word. Its force-zero/force-one encodings are 4 and 5, which is
 * what RSX_REMAP_ZERO/ONE already are, so the selectors pass straight through.
 *
 * out[] arrives in the crossbar's field order A,R,G,B; D3D12 wants
 * destR | destG<<3 | destB<<6 | destA<<9 | valid<<12. */
static u32 rsx_remap_to_d3d(u32 c1, u32 basef)
{
    u8 out[4];
    rsx_texture_component_remap(c1, basef, out);
    /* TEX_REMAP_ID=<n>: override the derived crossbar with a fixed mapping, to
     * test channel order directly. The resource holds the guest's A8R8G8B8
     * bytes straight through, so its components are (R=A, G=R, B=G, A=B) and
     * the shader needs destR=1, destG=2, destB=3, destA=0 to see real RGBA.
     *   1 = identity   2 = rotate (the ARGB fix)   3 = BGRA swap */
    { static int fixed = -1;
      if (fixed < 0) { const char* e = getenv("TEX_REMAP_ID"); fixed = e ? atoi(e) : 0; }
      if (fixed == 1) return 0u | (1u<<3) | (2u<<6) | (3u<<9) | (1u<<12);
      if (fixed == 2) return 1u | (2u<<3) | (3u<<6) | (0u<<9) | (1u<<12);
      if (fixed == 3) return 2u | (1u<<3) | (0u<<6) | (3u<<9) | (1u<<12); }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("REMAPDBG") ? 1 : 0;
      if (_rd) { static u32 seen[32]; static int ns=0;
        u32 k = (basef << 16) | (c1 & 0xFFFFu); int f=0;
        for (int i2=0;i2<ns;i2++) if (seen[i2]==k) f=1;
        if (!f && ns<32) { seen[ns++]=k;
            fprintf(stderr, "[REMAP] basef=0x%02X c1=0x%04X -> destR=%u destG=%u destB=%u destA=%u%c",
                    basef, c1 & 0xFFFFu, out[1], out[2], out[3], out[0], 10); } } }
    return out[1] | (out[2] << 3) | (out[3] << 6) | (out[0] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */

/* Sparse FNV-1a over a texture's source bytes -- enough to notice an animated
 * surface changing, cheap enough to run on every bind. Kept a function rather
 * than a statement-expression macro so the file still compiles under MSVC;
 * ps3recomp builds with clang-cl, but titles like Tokyo Jungle drive the build
 * through the Visual Studio generator. */
/* Resistance port: 128 samples instead of 1024 for the per-frame "did it
 * change" test -- each sample of a large texture is its own cache miss, and
 * the test runs for every texture every frame (~2 ms of a ~12 ms render
 * batch). Every 16th check of a slot still uses the full 1024-sample sum, so
 * a small partial update is still noticed within a fraction of a second.
 * RSX_TEXCSUM_QUICK=0 = always the full sum. */
static u32 tex_csum_n(const u8* base, u32 nbytes, u32 samples)
{
    u32 h = 2166136261u;
    u32 step = nbytes > samples * 4u ? nbytes / samples : 4u;
    for (u32 i = 0; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}
static u32 tex_csum(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    for (u32 i = 0; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* Resistance port: texture upload ring. Every texture slot used to own a
 * committed UPLOAD buffer: a new texture paid two CreateCommittedResource calls
 * (the upload buffer zeroed by the OS), and a re-upload into a slot whose buffer
 * the previous batch had not finished copying from waited for the GPU. The
 * trace of York's walk out of the crater put ~1.3 ms on EVERY upload, 16x16 or
 * 512x512 alike -- 560 ms in the first frame of the level, ~4 s a minute.
 * Uploads now stage in one persistently mapped ring, split per batch like the
 * other per-batch buffers (half = batch parity; the half's previous user, batch
 * N-2, is waited on first, normally long done). A texture that does not fit
 * falls back to its own buffer. RSX_TEX_RING=0 restores the old path. */
#define TEX_RING_CAP (96u << 20)          /* bytes per batch half */
static struct ID3D12Resource* s_tr_buf;
static u8* s_tr_mapped;
static u32 s_tr_off, s_tr_batch;
static volatile LONG s_tr_hits, s_tr_full;
static int tex_ring_alloc(u64 bytes, u64* out_off)
{
    static int en = -1, failed = 0;
    if (en < 0) { const char* e = getenv("RSX_TEX_RING"); en = e ? atoi(e) : 1; }
    if (!en || failed || !s_d3d.device) return 0;
    if (!s_tr_buf) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (u64)TEX_RING_CAP * 2;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0}; void* m = NULL;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_tr_buf)) ||
            FAILED(s_tr_buf->lpVtbl->Map(s_tr_buf, 0, &nr, &m)) || !m) {
            if (s_tr_buf) { s_tr_buf->lpVtbl->Release(s_tr_buf); s_tr_buf = NULL; }
            failed = 1;
            fprintf(stderr, "[TEXRING] could not create the upload ring: per-texture buffers%c", 10);
            return 0;
        }
        s_tr_mapped = (u8*)m;
        fprintf(stderr, "[TEXRING] texture upload ring ready (%u MB x2)%c", TEX_RING_CAP >> 20, 10);
    }
    const u32 batch = s_submit_no + 1;            /* the batch being recorded copies from it */
    if (batch != s_tr_batch) {
        s_tr_batch = batch; s_tr_off = 0;
        if (batch >= 3) wait_batch(batch - 2);    /* this half's previous user */
    }
    const u64 o = ((u64)s_tr_off + 511u) & ~511ull;
    if (o + bytes > TEX_RING_CAP) { InterlockedIncrement(&s_tr_full); return 0; }
    s_tr_off = (u32)(o + bytes);
    *out_off = (u64)(batch & 1u) * TEX_RING_CAP + o;
    InterlockedIncrement(&s_tr_hits);
    return 1;
}
static int vp_upload_tex_slot_impl(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips, u32 tpitch);
static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips, u32 tpitch)
{
    const uint64_t ft0 = rft0();
    const int r = vp_upload_tex_slot_impl(off, w, h, fmt, cube, mips, tpitch);
    rft(19, off, (w << 16) | (h & 0xFFFFu), ft0);
    return r;
}
/* v160k: the front end's button pictures. Its menus draw the PS3 buttons from
 * one 512x256 DXT5 icon atlas (decoded-pixel hash 0xE82A7DC8; Circle at
 * 379..419 x 15..55, Cross at 467..507 x 15..55). In ResistanceFoM.exe those
 * two cells are redrawn as key caps -- "ESC" (back) and "SPACE" (select) --
 * and re-encoded to DXT5 in the upload buffer. RSX_FE_KEYCAPS=0 disables. */
/* v160m: drawn at any cell size S (44 at full size; each smaller mip level is
 * drawn directly at its own size, so the letters stay crisp instead of being
 * shrunk from the big one), letters as large as fit, narrowed to fit. */
static void fe_keycap_rgba(u8* out, int S, const char* label)
{
    /* v160r: no cap background (the icon area is only ~20 px on screen):
     * white symbol with a dark outline filling the cell. "ESC" as letters
     * (shrunk to fit, never narrowed); "SPACE" as the space-bar sign. */
    memset(out, 0, (size_t)S * S * 4);
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!dc || !bm || !bits) { if (bm) DeleteObject(bm); if (dc) DeleteDC(dc); return; }
    HGDIOBJ ob = SelectObject(dc, bm);
    memset(bits, 0, (size_t)S * S * 4);
    const int o = S >= 16 ? 2 : 1;                    /* outline width */
    const int x0 = S * 3 / 44, x1 = S - S * 3 / 44;   /* the original icon's span inside the cell */
    if (!strcmp(label, "SPACE")) {
        /* the space-bar sign: a wide U, drawn as outline (red) then fill (green) */
        const int th = S / 8 > 1 ? S / 8 : 1;
        const int by = S * 25 / 44, ty = S * 14 / 44;   /* v160u: centred on the cell (was low) */
        for (int pass = 0; pass < 2; pass++) {
            const int g = pass ? 0 : o;
            HBRUSH br = CreateSolidBrush(pass ? RGB(0, 255, 0) : RGB(255, 0, 0));
            RECT r1 = { x0 + o - g, by - g, x1 - o + g, by + th + g };          /* bottom bar */
            RECT r2 = { x0 + o - g, ty - g, x0 + o + th + g, by + th + g };     /* left end   */
            RECT r3 = { x1 - o - th - g, ty - g, x1 - o + g, by + th + g };     /* right end  */
            FillRect(dc, &r1, br); FillRect(dc, &r2, br); FillRect(dc, &r3, br);
            DeleteObject(br);
        }
    } else {
        const int len = (int)strlen(label);
        /* v160s: a condensed DIN face (Bahnschrift ships with Windows 10/11 and
         * matches the menu's own lettering) so three letters can be tall */
        int fh = S * 40 / 44; HFONT fo = NULL; SIZE sz = { 0, 0 };
        for (; fh >= 5; fh--) {
            if (fo) DeleteObject(fo);
            fo = CreateFontA(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift SemiBold Condensed");
            SelectObject(dc, fo);
            GetTextExtentPoint32A(dc, label, len, &sz);
            if (sz.cx <= (x1 - x0)) break;   /* v160t: no outline on the letters -- full width, crisper */
        }
        SetBkMode(dc, TRANSPARENT);
        RECT rc = { 0, 0, S, S };
        SetTextColor(dc, RGB(255, 0, 0));
        for (int dy = -o; dy <= o && 0; dy++) for (int dx = -o; dx <= o; dx++) {
            if (!dx && !dy) continue;
            RECT r = { rc.left + dx, rc.top + dy, rc.right + dx, rc.bottom + dy };
            DrawTextA(dc, label, len, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        }
        SetTextColor(dc, RGB(0, 255, 0));
        DrawTextA(dc, label, len, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        if (fo) DeleteObject(fo);
    }
    GdiFlush();
    const u8* px = (const u8*)bits;
    for (int i = 0; i < S * S; i++) {
        const u8 ol = px[i * 4 + 2], tx = px[i * 4 + 1];   /* BGRA: red = outline, green = fill */
        u8* d = out + i * 4;
        const int a = ol > tx ? ol : tx;
        const int v = (tx * 250) / 255 + 8;
        d[0] = d[1] = d[2] = (u8)(v > 255 ? 255 : v); d[3] = (u8)a;
    }
    SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc);
}
static u16 fe_565(const u8* c) { return (u16)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)); }
static void fe_bc3_block(u8* dst, const u8 px[16][4])
{
    int amin = 255, amax = 0, lmin = 1 << 30, lmax = -1, imin = 0, imax = 0;
    for (int i = 0; i < 16; i++) {
        if (px[i][3] < amin) amin = px[i][3];
        if (px[i][3] > amax) amax = px[i][3];
        const int l = px[i][0] * 3 + px[i][1] * 6 + px[i][2];
        if (l < lmin) { lmin = l; imin = i; }
        if (l > lmax) { lmax = l; imax = i; }
    }
    /* alpha: 8-level mode (a0 > a1) */
    u8 a0 = (u8)amax, a1 = (u8)amin; int lv[8];
    lv[0] = a0; lv[1] = a1; for (int i = 1; i < 7; i++) lv[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    uint64_t ab = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) { const int d = abs(lv[k] - px[i][3]); if (d < bd) { bd = d; best = k; } }
        if (a0 == a1) best = 0;
        ab |= (uint64_t)best << (3 * i);
    }
    dst[0] = a0; dst[1] = a1; for (int i = 0; i < 6; i++) dst[2 + i] = (u8)(ab >> (8 * i));
    /* colour: two endpoints, 4-colour mode */
    u16 c0 = fe_565(px[imax]), c1 = fe_565(px[imin]);
    if (c0 < c1) { u16 t = c0; c0 = c1; c1 = t; }
    u8 pal[4][3];
    pal[0][0] = (u8)(((c0 >> 11) & 31) * 255 / 31); pal[0][1] = (u8)(((c0 >> 5) & 63) * 255 / 63); pal[0][2] = (u8)((c0 & 31) * 255 / 31);
    pal[1][0] = (u8)(((c1 >> 11) & 31) * 255 / 31); pal[1][1] = (u8)(((c1 >> 5) & 63) * 255 / 63); pal[1][2] = (u8)((c1 & 31) * 255 / 31);
    for (int c = 0; c < 3; c++) { pal[2][c] = (u8)((2 * pal[0][c] + pal[1][c]) / 3); pal[3][c] = (u8)((pal[0][c] + 2 * pal[1][c]) / 3); }
    u32 ci = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
            const int d = abs(pal[k][0] - px[i][0]) + abs(pal[k][1] - px[i][1]) + abs(pal[k][2] - px[i][2]);
            if (d < bd) { bd = d; best = k; }
        }
        if (c0 == c1) best = 0;
        ci |= (u32)best << (2 * i);
    }
    dst[8] = (u8)c0; dst[9] = (u8)(c0 >> 8); dst[10] = (u8)c1; dst[11] = (u8)(c1 >> 8);
    dst[12] = (u8)ci; dst[13] = (u8)(ci >> 8); dst[14] = (u8)(ci >> 16); dst[15] = (u8)(ci >> 24);
}
/* v160l: the patched level 0 (RGBA), so the smaller mip levels -- what the
 * menu actually samples at its icon size -- can be rebuilt from it. */
static u8 s_fe_atlas[512 * 256 * 4]; static int s_fe_atlas_pending = 0;
/* cell: 44x44 at (x0, y0), both multiples of 4 */
static void fe_put_keycap(u8* map, u32 pitch, u32 x0, u32 y0, const char* label)
{
    static u8 img[44 * 44 * 4];
    fe_keycap_rgba(img, 44, label);
    for (u32 y = 0; y < 44; y++) memcpy(s_fe_atlas + ((y0 + y) * 512 + x0) * 4, img + y * 44 * 4, 44 * 4);
    for (u32 by = 0; by < 11; by++) for (u32 bx = 0; bx < 11; bx++) {
        u8 px[16][4];
        for (int i = 0; i < 16; i++) memcpy(px[i], img + ((by * 4 + i / 4) * 44 + bx * 4 + (i & 3)) * 4, 4);
        fe_bc3_block(map + (u64)((y0 >> 2) + by) * pitch + ((x0 >> 2) + bx) * 16, px);
    }
}
static int vp_upload_tex_slot_impl(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips, u32 tpitch)
{
    /* TEX_BUDGET=<n>: cap texture uploads per frame. Off by default -- it was
     * tried against the TDR and does not help, because PERF shows the upload
     * path costing 0.01s for 775 calls. Kept because it is the right lever if a
     * title ever does hit an upload-bound frame. */
    { static int budget = -1;
      if (budget < 0) { const char* e = getenv("TEX_BUDGET"); budget = e ? atoi(e) : 0; }
      if (budget > 0) {
          static u64 last_frame = ~0ull; static int used = 0;
          if (last_frame != (u64)s_d3d.frame_count) { last_frame = (u64)s_d3d.frame_count; used = 0; }
          if (++used > budget) return -1;   /* try again next frame */
      } }
    extern uint8_t* vm_base;

    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    /* Format classes (base = fmt & 0x9F). The LBP loading screen uses:
     * 0x85 A8R8G8B8 (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear FONT
     * atlas -- without it no text renders at all), 0x86/87/88 DXT1/23/45
     * (512x512 detail/LUT layers bound at t1/t3 on every draw). */
    /* Format class, row sizes and swizzling are RSX semantics: rsx_texture_
     * layout.c owns them so the Metal backend can read a guest texture without
     * re-deriving any of it. Only the DXGI mapping below is ours. */
    rsx_tex_layout tl;
    /* Resistance port: honour TEXTURE_CONTROL3's pitch for linear images
     * (its 1280x720 linear DXT5 menu backdrop is stored with a wider row
     * stride than its block row -- read packed, it came out striped). */
    rsx_texture_layout_pitched(fmt, w, h, (cube || mips > 1) ? 0 : tpitch, &tl);
    u32 basef = fmt & 0x9F;              /* still needed for the SRV remap */
    /* Resistance port diagnostic: TEX_DUMP_RAW=<hex vm offset> writes the
     * guest bytes of that texture (3 snapshots, 1 s apart) to
     * texdump_<off>_<n>.bin and logs format/size/pitch. */
    { static const char* e = (const char*)1; static u32 want = 0; static int nd = 0;
      static u64 last_ms = 0;
      if (e == (const char*)1) { e = getenv("TEX_DUMP_RAW"); want = e ? (u32)strtoul(e, NULL, 16) : 0; }
      if (want && off == want && nd < 3) {
          u64 now = GetTickCount64();
          if (!last_ms || now - last_ms > 1000) { last_ms = now;
              u32 nbytes = tl.row_bytes * tl.rows;
              char fn[64]; snprintf(fn, sizeof fn, "texdump_%08X_%d.bin", off, nd);
              FILE* f = fopen(fn, "wb");
              if (f) { fwrite(vm_base + off, 1, nbytes, f); fclose(f); }
              fprintf(stderr, "[TEXDUMP] %s fmt=0x%02X %ux%u pitch=%u row_bytes=%u rows=%u comp=%d swz=%d%c",
                      fn, fmt, w, h, tpitch, tl.row_bytes, tl.rows, tl.compressed, tl.swizzled, 10);
              nd++; } } }
    int argb = (tl.fmt == RSX_TEXFMT_R8G8B8A8);
    int dxt  = tl.compressed;
    /* Compressed rows are counted in blocks, so keep bpp at 1 for the byte
     * arithmetic the diagnostics below do. */
    u32 bpp = tl.compressed ? 1u : tl.bytes_per_texel;
    /* The DECODED image's format, which is not always the guest's: the packed
     * 16-bit colour formats arrive here unpacked to RGBA8, and DEPTH24_D8 as a
     * float. rsx_texture_layout.h says which class each format byte lands in. */
    DXGI_FORMAT dxfmt = (tl.fmt == RSX_TEXFMT_R8G8B8A8) ? DXGI_FORMAT_R8G8B8A8_UNORM
                      : (tl.fmt == RSX_TEXFMT_R8G8)     ? DXGI_FORMAT_R8G8_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC1)      ? DXGI_FORMAT_BC1_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC2)      ? DXGI_FORMAT_BC2_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC3)      ? DXGI_FORMAT_BC3_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16)      ? DXGI_FORMAT_R16_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16G16)   ? DXGI_FORMAT_R16G16_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16G16F)  ? DXGI_FORMAT_R16G16_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R16G16B16A16F) ? DXGI_FORMAT_R16G16B16A16_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R32F)     ? DXGI_FORMAT_R32_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R32G32B32A32F) ? DXGI_FORMAT_R32G32B32A32_FLOAT
                      : DXGI_FORMAT_R8_UNORM;
    /* DXT data is stored as linear 4x4 block rows (compressed formats are
     * never Morton-swizzled on RSX). Row of blocks = (w/4)*blocksize. */
    u32 blkrow  = dxt ? tl.dst_row_bytes : 0;
    u32 blkrows = dxt ? tl.rows      : 0;
    const u32 key_off = off;         /* lookup key: the offset as bound */
    /* Sparse checksum of a texture's source bytes -- enough to notice an
     * animated surface changing, cheap enough to run on every bind. */
    /* (portable helper tex_csum() -- see above; a statement-expression macro
     * here was a GCC/Clang extension MSVC rejects.) */
    #define TEX_CSUM(base, nbytes) tex_csum((base), (nbytes))
    int slot = -1, freeslot = -1;
    for (int i = s_tex_head[tex_bucket(key_off)] - 1; i >= 0; i = s_d3d.vp_tex[i].next - 1) {
        VPTexSlot* c = &s_d3d.vp_tex[i];
        if (c->res && c->key == key_off && c->w == w && c->h == h && c->fmt == fmt
            && c->cube == cube) {
            c->last_use = ++s_tex_clock;
            if (c->used) return i;                /* already bound this frame */
            { static int nocache = -1;            /* TEX_NOCACHE=1: always re-upload */
              if (nocache < 0) { const char* e = getenv("TEX_NOCACHE");
                                 nocache = e ? atoi(e) : 0; }
              if (nocache) { slot = i; break; } }
            /* Held from an earlier frame: re-upload only if the guest bytes
             * changed. Most of this scene's textures are static, and converting
             * every one of them every frame was ~70% of the frame's CPU time. */
            u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
            static int quick = -1;
            if (quick < 0) { const char* e = getenv("RSX_TEXCSUM_QUICK"); quick = e ? atoi(e) : 1; }
            if (quick && ++c->csum_n < 16u) {
                if (tex_csum_n(vm_base + c->off, nb, 128u) == c->csum_q) { c->used = 1; return i; }
            } else {
                c->csum_n = 0;
                u32 cs = TEX_CSUM(vm_base + c->off, nb);
                if (cs == c->csum) { c->used = 1; return i; }
            }
            slot = i; break;                      /* stale: fall through and redo */
        }
    }
    if (slot < 0) {   /* miss: pick a victim -- empty, else LRU same-shape, else LRU */
        int same = -1, any = -1; u64 bs = ~0ull, ba = ~0ull;
        for (int i = 0; i < VP_TEX_SLOTS; i++) {
            VPTexSlot* c = &s_d3d.vp_tex[i];
            if (c->used) continue;
            if (!c->res) { freeslot = i; break; }
            if (c->last_use < ba) { ba = c->last_use; any = i; }
            if (c->w == w && c->h == h && c->fmt == fmt && c->cube == cube &&
                c->last_use < bs) { bs = c->last_use; same = i; }
        }
        if (freeslot < 0) {
            /* A same-shape victim avoids re-creating resources, but don't let it
             * evict something far hotter than the global LRU entry. */
            freeslot = (same >= 0 && (any < 0 || bs <= ba + 4096)) ? same : any;
        }
    }
    /* TEX_OFF_BIAS=1: sample the image one whole level-0 BELOW the bound offset.
     * Measured on the Rubber Ducky demo: at the bind offset every texture reads
     * as all-zero, while [off - w*h*bpp, off) holds the image (3800-4160 of 4298
     * sampled bytes non-zero, for four different textures at three sizes). So
     * the offset the guest programs points PAST level 0 rather than at it. */
    /* TEX_OFF_BIAS: 1 = one level-0 below the bound offset, 2 = one full MIP
     * CHAIN above it. The measured upload-to-bind deltas on the Rubber Ducky
     * demo are 0x2AAB00 for a 2MB level 0 and 0x555580 for a 4MB one -- both
     * exactly the mipmap pyramid total (L0 * 4/3), which is what the guest
     * reserves per texture. */
    { static int bias = -1;
      if (bias < 0) { const char* e = getenv("TEX_OFF_BIAS"); bias = e ? atoi(e) : 0; }
      if (bias == 1) { u32 sz = w * h * bpp; if (off > sz) off -= sz; }
      else if (bias >= 2) {
          u32 chain = 0;
          for (u32 mw = w, mh = h; mw && mh; mw >>= 1, mh >>= 1) {
              chain += mw * mh * bpp;
              if (mw == 1 && mh == 1) break;
          }
          /* bias 3: only shift when the bound offset really is empty, so a
           * texture that IS bound correctly keeps its own data. Applying the
           * chain unconditionally fixes the mis-bound majority but breaks any
           * correctly-bound minority, which shows up as objects going black. */
          int shift = 1;
          if (bias >= 3) {
              u32 nz = 0, sz = w * h * bpp;
              for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
              shift = (nz == 0);
          }
          if (shift) off += chain;
      } }
    /* TEX_REMAP=1: if the bound offset holds nothing, look the image up by size
     * in the VRAM upload registry (cellGcmSys). For a title whose upload address
     * and SET_TEXTURE_OFFSET disagree this puts the real bytes under the sampler
     * without guessing a delta. */
    { static int remap = -1;
      if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
      if (remap) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
        if (!nz) {
            extern u32 rsx_find_vram_upload(u32);
            u32 alt = rsx_find_vram_upload(sz);
            if (alt) {
                static int _n = 0;
                if (PS3_LOG_FIRST(_n, 8)) fprintf(stderr, "[TEXREMAP] 0x%08X (%ux%u, %u bytes) -> 0x%08X%c",
                                      off, w, h, sz, alt, 10);
                off = alt;
            }
        } } }
    /* A stale cache entry (slot >= 0) re-uses its own slot; otherwise take a
     * free one. Without this the stale path fell through to freeslot and either
     * duplicated the texture into a second slot or bailed when none was free. */
    if (slot < 0 && freeslot < 0) return -1;      /* out of slots this frame */
    /* TEX_SRCDBG=<N>: is the guest memory this slot uploads FROM actually
     * populated? An empty source and a broken sampler both render flat. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("TEX_SRCDBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap) { n++;
        u32 nz = 0, tot = 0;
        for (u32 i = 0; i < w * h * bpp && i < 0x40000u; i += 61) { tot++; if (vm_base[off + i]) nz++; }
        /* Several of this title's textures are bound at EXACTLY the end address
         * of an upload, so also sample the block immediately BELOW the bind
         * offset: if that holds the image, the bind is one whole texture high. */
        u32 nzb = 0, totb = 0;
        { u32 sz = w * h * bpp; u32 base = (off > sz) ? off - sz : 0;
          for (u32 i = 0; i < sz && i < 0x40000u; i += 61) { totb++; if (vm_base[base + i]) nzb++; } }
        long nearest = 0; u32 found = 0;
        if (!nz) {   /* empty: where IS the data? scan out from the bind offset */
            for (long d = 0x1000; d <= 0x1000000 && !found; d += 0x1000) {
                for (int s = 0; s < 2 && !found; s++) {
                    long a = (long)off + (s ? -d : d);
                    if (a < 0x1000) continue;
                    u32 c = 0;
                    for (u32 i = 0; i < 0x1000u; i += 61) if (vm_base[a + i]) c++;
                    if (c > 8) { nearest = (s ? -d : d); found = c; }
                }
            }
        }
        fprintf(stderr, "[TEXSRC] off=0x%08X %ux%u fmt=0x%02X bpp=%u nonzero=%u/%u"
                        "%s%+ld (page nonzero=%u)\n",
                off, w, h, fmt, bpp, nz, tot,
                nz ? "" : "  nearest data at ", nz ? 0L : nearest, found);
        fprintf(stderr, "[TEXSRC]    block BELOW bind (off-%u): nonzero=%u/%u%c",
                w * h * bpp, nzb, totb, 10);
        /* Walk DOWN in 4KB steps to the first populated page whose predecessor
         * is empty: that is where this texture's data actually begins, and the
         * delta from the bound offset is the rule we are missing. */
        { u32 step = 0x1000, prev_pop = 0; long start_delta = 0;
          for (long d = 0; d >= -(long)(w * h * bpp) * 2; d -= step) {
              u32 a = (u32)((long)off + d); u32 pop = 0;
              if (a < 0x1000) break;
              for (u32 i = 0; i < step; i += 53) if (vm_base[a + i]) pop++;
              if (!pop && prev_pop) { start_delta = d + step; break; }
              prev_pop = pop;
          }
          fprintf(stderr, "[TEXSRC]    data starts at off%+ld (level0=%u bytes)%c",
                  start_delta, w * h * bpp, 10); } } }
    if (slot < 0) slot = freeslot;
    if (slot < 0) return -1;
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    if (t->chained) {          /* unlink from its current key's chain */
        int* pp = &s_tex_head[tex_bucket(t->key)];
        while (*pp && *pp - 1 != slot) pp = &s_d3d.vp_tex[*pp - 1].next;
        if (*pp) *pp = t->next;
        t->next = 0; t->chained = 0;
    }
    /* The upload buffer's rows hold the DECODED image, so they are sized from
     * dst_row_bytes -- w * bpp is the source row, which is narrower for any
     * format rsx_texture_decode unpacks. */
    u32 pitch = (tl.dst_row_bytes + 255) & ~255u;
    int fresh = 0;
    /* Resistance port: upload the guest's mip chain. Base-level-only textures
     * shimmered and moired badly at a distance (York's ground and walls).
     * 2D swizzled or DXT textures only (their levels are packed back to back);
     * DXT stops at the last level that is still a whole 4x4 block.
     * RSX_TEX_MIPS=0 disables. */
    u32 nm = 1;
    u32 lvl_off[16] = {0}, lvl_pitch[16] = {0}, lvl_rows[16] = {0};
    u64 up_total = (u64)pitch * (dxt ? blkrows : h) * (cube ? 6 : 1);
    { static int en = -1; if (en < 0) { const char* e = getenv("RSX_TEX_MIPS"); en = e ? atoi(e) : 1; }
      if (en && !cube && mips > 1 && (dxt || tl.swizzled)) {
          u32 lw = w, lh = h;
          u64 o = up_total;
          for (u32 L = 1; L < mips && L < 16; L++) {
              lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
              if (dxt && (lw < 4 || lh < 4 || (lw & 3) || (lh & 3))) break;
              rsx_tex_layout tll;
              rsx_texture_layout_pitched(fmt, lw, lh, 0, &tll);
              o = (o + 511u) & ~511ull;               /* placement alignment */
              lvl_off[L] = (u32)o;
              lvl_pitch[L] = (tll.dst_row_bytes + 255) & ~255u;
              lvl_rows[L] = dxt ? tll.rows : lh;
              o += (u64)lvl_pitch[L] * lvl_rows[L];
              nm = L + 1;
          }
          if (nm > 1) up_total = o;
      } }

    /* Also recreate when the CUBE-ness changes: a slot holding a 2D texture of
     * the same dimensions would otherwise be reused with 1 array slice while the
     * cube path copies 6 subresources and writes a 6x upload buffer -- which
     * crashes. */
    if (t->res && (t->w != w || t->h != h || t->fmt != fmt || t->cube != cube ||
                   t->nmips != nm)) {
        /* deferred: a batch still in flight may sample res / copy from up */
        grave_defer(t->res); t->res = NULL;
        if (t->up) { grave_defer(t->up); t->up = NULL; }
        t->up_batch = 0;
    }
    if (!t->res) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = cube ? 6 : 1;   /* cube = 6 array slices */
        td.MipLevels = (UINT16)nm;
        td.Format = dxfmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        /* Resistance port: skip the zero-fill of the new texture's memory
         * (D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, 0x1000) -- the copy below writes
         * every texel. Older runtimes reject the flag: retry without it. */
        { static int nz = 1;
          HRESULT hrc = E_FAIL;
          if (nz) {
              hrc = s_d3d.device->lpVtbl->CreateCommittedResource(
                  s_d3d.device, &hp, (D3D12_HEAP_FLAGS)0x1000, &td,
                  D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                  &IID_ID3D12Resource, (void**)&t->res);
              if (FAILED(hrc)) { nz = 0; t->res = NULL; }
          }
          if (FAILED(hrc) && FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                  s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                  &IID_ID3D12Resource, (void**)&t->res)))
              return -1; }
        fresh = 1;
    }

    /* Stage the decoded texels: in the batch's slice of the upload ring, else
     * (ring full / off) in this slot's own upload buffer. */
    u64 ring_off = 0;
    const int use_ring = tex_ring_alloc(up_total, &ring_off);
    void* mapped = NULL; D3D12_RANGE nr = {0,0};
    ID3D12Resource* up_src = NULL; u64 up_base = 0;
    if (use_ring) {
        mapped = s_tr_mapped + ring_off; up_src = s_tr_buf; up_base = ring_off;
    } else {
        if (!t->up) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = up_total;
            bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&t->up))) {
                t->up = NULL;
                if (fresh) { t->res->lpVtbl->Release(t->res); t->res = NULL; }
                return -1;
            }
            t->up_batch = 0;
        }
        /* The previous batch may not have run its copy out of `up` yet. */
        if (t->up_batch && rsx_pipeline()) wait_batch(t->up_batch);
        t->up_batch = s_submit_no + 1;   /* the batch being recorded copies from it */
        if (FAILED(t->up->lpVtbl->Map(t->up, 0, &nr, &mapped)) || !mapped) return -1;
        up_src = t->up;
    }
    { static int _tp=0; if (getenv("RTT_DUMP") && PS3_LOG_FIRST(_tp, 6)) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  row240:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+_b]);
        fprintf(stderr, "  mid:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+320*bpp+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    /* Swizzled (LN bit 0x20 clear): texels live in Morton/Z-order, not rows.
     * Uploading them as linear rows produced the diagonal-stripe garbage on
     * LBP's loading screen (every texture there is 0x85 swizzled). POT dims
     * only -- the hardware requires that for swizzled textures anyway. */
    int swz = tl.swizzled;
    /* Cube textures store their 6 faces consecutively. Convert each into its own
     * slice of the upload buffer; the conversion below is unchanged and simply
     * runs once per face with off/mapped rebased. */
    const u32 _face_rows  = dxt ? blkrows : h;
    /* The face stride is NOT one mip-0 image: RSX stores each cube face as its
     * own complete mip pyramid, so face f starts a whole pyramid (128-byte
     * aligned) in. Assuming mip-0-sized strides made face 1 land inside face 0's
     * mip chain -- which is exactly what the dumps showed, every face after the
     * first a progressively smaller copy of the first. */
    u32 _face_bytes = dxt ? (blkrow * blkrows) : (w * h * bpp);
    if (cube) {
        u32 total = 0;
        for (u32 lw = w, lh = h, l = 0; l < (mips ? mips : 1u); l++) {
            total += dxt ? (((lw + 3) / 4) * (bpp == 8 ? 16u : 8u) * ((lh + 3) / 4))
                         : (lw * lh * bpp);
            if (lw > 1) lw >>= 1;
            if (lh > 1) lh >>= 1;
        }
        _face_bytes = (total + 127u) & ~127u;
    }
    const u32 _nfaces     = cube ? 6u : 1u;
    const u32 _off0 = off; u8* const _map0 = (u8*)mapped;
    for (u32 _f = 0; _f < _nfaces; _f++) {
    off    = _off0 + _f * _face_bytes;
    if (cube && getenv("CUBEDBG")) { u32 _s=0; const u8* _p = vm_base + off;
        for (u32 _i = 0; _i < _face_bytes; _i += 97) _s = _s*131 + _p[_i];
        fprintf(stderr, "[CUBEFACE] f=%u off=0x%X bytes=%u bpp=%u swz=%d csum=%08X%c",
                _f, off, _face_bytes, bpp, swz, _s, 10); }
    mapped = _map0 + (u64)_f * pitch * _face_rows;
    /* Pixel conversion -- deswizzle, channel order, block copy -- is RSX
     * semantics and lives in rsx_texture_layout.c, so a second backend gets
     * it by calling this rather than porting the loops again. */
    rsx_texture_decode(mapped, pitch, vm_base + off, w, h, &tl,
                       rsx_texture_argb_is_rgba());

    /* The diagnostics below only apply to the linear 8-bit case (Bink video
     * planes); they used to sit inside that branch of the conversion. */
    if (!tl.compressed && tl.bytes_per_texel == 1 && !tl.swizzled) {
        /* MOVIE_FIND=1: when a 640x360 movie plane uploads ZERO, scan a wide
         * guest window for the REAL frame (a 640-wide region with content) so
         * we learn where the video decode actually wrote vs where the texture
         * points -- ends the frame-buffer-address guessing. */
        if (getenv("MOVIE_FIND") && w == 640 && h == 360) {
            extern uint8_t* vm_base;
            const u8* here = vm_base + off;
            u32 hz = 0; for (u32 i=0;i<w*h;i+=137) if (here[i]) { hz=1; break; }
            static int _mf = 0;
            if (!hz && PS3_LOG_FIRST(_mf, 3)) {
                fprintf(stderr, "[movie-find] bound plane ea=0x%X is ZERO; full scan...\n", off);
                /* Scan FULL VRAM + main heap in 0x8000 steps for a 640x360
                 * content block -- NO early cap (the previous found<6 stopped in
                 * the low-VRAM display buffers before ever reaching the movie
                 * region ~0x40E80000). Report the near-plane hits explicitly so
                 * ring-desync (frame present at a DIFFERENT slot) is separable
                 * from decode-never-ran (region entirely zero). */
                struct { u32 lo, hi; const char* tag; } rng[] = {
                    {0x40000000u, 0x41000000u, "VRAM"}, {0x00100000u, 0x10000000u, "MAIN"},
                    {0x48000000u, 0x4A000000u, "BINK"} };  /* Bink working set (handle ~0x4849E760, SPU I/O 0x4847/0x4945) */
                u32 plane_lo = (off > 0x400000u) ? off - 0x400000u : 0;
                u32 plane_hi = off + 0x400000u;
                int found = 0, nearc = 0;
                for (int r = 0; r < (int)(sizeof(rng)/sizeof(rng[0])); r++) {
                    u32 best_a = 0, best_nz = 0; u32 best_span = 0;
                    for (u32 a = rng[r].lo; a + w*h < rng[r].hi; a += 0x8000) {
                        const u8* p = vm_base + a;
                        u32 nz = 0; u8 mn2 = 255, mx2 = 0;
                        for (u32 i = 0; i < w*h; i += 257) { u8 v = p[i]; if (v) nz++; if (v<mn2) mn2=v; if (v>mx2) mx2=v; }
                        u32 span = mx2 - mn2;
                        if (nz > best_nz) { best_nz = nz; best_a = a; best_span = span; }
                        if (nz > 400 && span > 60) {  /* looks like image content */
                            int isnear = (a >= plane_lo && a <= plane_hi);
                            if (isnear) nearc++;
                            if (found < 40)
                                fprintf(stderr, "[movie-find]   CONTENT @0x%08X (%s) nz=%u span=%u%s\n",
                                        a, rng[r].tag, nz, span, isnear ? " <== NEAR bound plane" : "");
                            found++;
                        }
                    }
                    /* Always report the densest block in this range even below the
                     * content threshold -- the MM intro fades in from black, so the
                     * real first frames are low-span and would otherwise be invisible. */
                    fprintf(stderr, "[movie-find]   %s best @0x%08X nz=%u span=%u%s\n",
                            rng[r].tag, best_a, best_nz, best_span,
                            (best_a >= plane_lo && best_a <= plane_hi) ? " <== NEAR bound plane" : "");
                    /* Dump the densest block as a 640x360 grayscale BMP so the
                     * "is this the decoded movie frame" question is answered by
                     * eye. One dump per range per process. */
                    if (best_nz > 400 && best_span > 40) {
                        static int _bd[4] = {0,0,0,0};
                        if (r < 4 && !_bd[r]) { _bd[r] = 1;
                            const u8* sp = vm_base + best_a;
                            char pn[128];
                            snprintf(pn, sizeof(pn), "binkscan_%s_%08X.bmp", rng[r].tag, best_a);
                            FILE* f = fopen(pn, "wb");
                            if (f) {
                                u32 rowb = w*3, rowp = (rowb+3)&~3u, fsz = 54 + rowp*h;
                                u8 hd[54] = {'B','M'};
                                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                                hd[10]=54; hd[14]=40;
                                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                                hd[26]=1; hd[28]=24;
                                fwrite(hd,1,54,f);
                                for (int y=(int)h-1; y>=0; y--) {
                                    const u8* srow = sp + (u64)y*w;
                                    for (u32 x=0;x<w;x++){ u8 px[3]; px[0]=px[1]=px[2]=srow[x]; fwrite(px,1,3,f); }
                                    { u8 z[3]={0,0,0}; fwrite(z,1,rowp-rowb,f); }
                                }
                                fclose(f);
                                fprintf(stderr, "[movie-find]   dumped %s\n", pn);
                            }
                        }
                    }
                }
                fprintf(stderr, "[movie-find]   total=%d near-plane=%d (window 0x%X..0x%X)\n",
                        found, nearc, plane_lo, plane_hi);
                if (!found) fprintf(stderr, "[movie-find]   no 640x360 content block (>thresh) found anywhere\n");
            }
        }
        /* TEX_SAVE=1: also dump wide B8 uploads (Bink video planes are B8 --
         * the Y plane IS the movie frame in grayscale) + a content stat, so
         * "is the decoder producing pixels" is answerable by looking at a BMP. */
        if (getenv("TEX_SAVE") && w >= 256) {
            static int _bs = 0;
            const u8* sp = vm_base + off;
            u32 nz = 0; u8 mn = 255, mx = 0;
            for (u32 i = 0; i < w * h; i += 17) {
                u8 v = sp[i]; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            fprintf(stderr, "[TEXB8] off=0x%X %ux%u nz=%u/%u min=%u max=%u\n",
                    off, w, h, nz, (w * h) / 17, mn, mx);
            if (_bs < 8 && mx > mn) { _bs++;
                char pn[128];
                snprintf(pn, sizeof(pn), "texb8_%08X_%ux%u_%d.bmp", off, w, h, _bs);
                FILE* f = fopen(pn, "wb");
                if (f) {
                    u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                    u32 fsz = 54 + rowp * h;
                    u8 hd[54] = {'B','M'};
                    hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const u8* srow = sp + (u64)y * w;
                        for (u32 x = 0; x < w; x++) {
                            u8 px[3]; px[0]=px[1]=px[2]=srow[x];
                            fwrite(px, 1, 3, f);
                        }
                        { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                    }
                    fclose(f);
                    fprintf(stderr, "[TEX_SAVE] %s\n", pn);
                }
            }
        }
    }
    /* v160i diag: front end (ResistanceFoM.exe) small textures -> logs\fe_icons\
     * once each, named by a hash of their pixels, composited on grey -- to
     * find the X / O button pictures of the menus and swap them for key names.
     * RSX_FE_ICONS=0 disables. */
    { static int fe = -1; static int s_fe_dump = 0;
      if (fe < 0) { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
                    const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
                    const char* e = getenv("RSX_FE_ICONS");
                    fe = !_stricmp(b, "ResistanceFoM.exe") ? 1 : 0;
                    s_fe_dump = e && atoi(e);
                    if (s_fe_dump) CreateDirectoryA("logs\\fe_icons", NULL); }
      /* v162c: in-game (pause/options menus) too, but only the atlas shape -- no decode cost elsewhere */
      /* v162d diag: particle textures only seen when shooting enemies -> logs\\ptex_<off>.bmp once */
      const int ptex = (off & 0x0FFFFFFFu) == 0x0DB4AA00u || (off & 0x0FFFFFFFu) == 0x01C63C80u || (off & 0x0FFFFFFFu) == 0x0DB72B80u ||
                       (off & 0x0FFFFFFFu) == 0x0DB7EC00u || (off & 0x0FFFFFFFu) == 0x0DAE2400u || (off & 0x0FFFFFFFu) == 0x0DADCD00u ||
                       (off & 0x0FFFFFFFu) == 0x0DC24380u || (off & 0x0FFFFFFFu) == 0x0DB3E980u;
      if ((fe || ptex || (dxt && w == 512 && h == 256 && tl.fmt == RSX_TEXFMT_BC3)) && (argb || dxt || (!dxt && tl.bytes_per_texel == 1 && dxfmt == DXGI_FORMAT_R8_UNORM)) && _nfaces == 1 && w >= 8 && h >= 8 && w <= 1024 && h <= 1024 && !(w & 3) && !(h & 3)) {   /* v160j: up to 1024 -- the X/O are in an atlas */
          /* RGBA view of the upload (DXT decoded) */
          static u8 rgba[1024 * 1024 * 4];
          if (argb) { for (u32 y = 0; y < h; y++) memcpy(rgba + y * w * 4, _map0 + (u64)y * pitch, w * 4); }
          else if (!dxt) { for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) { const u8 v = _map0[(u64)y * pitch + x]; u8* d = rgba + (y * w + x) * 4; d[0] = d[1] = d[2] = 255; d[3] = v; } }
          else {
              const int bc = tl.fmt == RSX_TEXFMT_BC1 ? 1 : tl.fmt == RSX_TEXFMT_BC2 ? 2 : 3, bs = bc == 1 ? 8 : 16;
              for (u32 by = 0; by < h / 4; by++) for (u32 bx = 0; bx < w / 4; bx++) {
                  const u8* b = _map0 + (u64)by * pitch + bx * bs;
                  const u8* cb = bc == 1 ? b : b + 8;
                  const u32 c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
                  u8 pal[4][4];
                  pal[0][0] = (u8)(((c0 >> 11) & 31) * 255 / 31); pal[0][1] = (u8)(((c0 >> 5) & 63) * 255 / 63); pal[0][2] = (u8)((c0 & 31) * 255 / 31); pal[0][3] = 255;
                  pal[1][0] = (u8)(((c1 >> 11) & 31) * 255 / 31); pal[1][1] = (u8)(((c1 >> 5) & 63) * 255 / 63); pal[1][2] = (u8)((c1 & 31) * 255 / 31); pal[1][3] = 255;
                  for (int c = 0; c < 3; c++) {
                      if (c0 > c1 || bc != 1) { pal[2][c] = (u8)((2 * pal[0][c] + pal[1][c]) / 3); pal[3][c] = (u8)((pal[0][c] + 2 * pal[1][c]) / 3); }
                      else { pal[2][c] = (u8)((pal[0][c] + pal[1][c]) / 2); pal[3][c] = 0; }
                  }
                  pal[2][3] = 255; pal[3][3] = (bc == 1 && c0 <= c1) ? 0 : 255;
                  const u32 idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((u32)cb[7] << 24);
                  u8 al[16];
                  if (bc == 2) { for (int i = 0; i < 16; i++) al[i] = (u8)(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17); }
                  else if (bc == 3) {
                      u8 a[8]; a[0] = b[0]; a[1] = b[1];
                      if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (u8)(((7 - i) * a[0] + i * a[1]) / 7);
                      else { for (int i = 1; i < 5; i++) a[i + 1] = (u8)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
                      uint64_t ab = 0; for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
                      for (int i = 0; i < 16; i++) al[i] = a[(ab >> (3 * i)) & 7];
                  } else for (int i = 0; i < 16; i++) al[i] = 255;
                  for (int i = 0; i < 16; i++) {
                      const u8* c = pal[(idx >> (2 * i)) & 3];
                      u8* d = rgba + ((by * 4 + i / 4) * w + bx * 4 + (i & 3)) * 4;
                      d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = bc == 1 ? c[3] : al[i];
                  }
              }
          }
          u32 hsh = 2166136261u;
          for (u32 x = 0; x < w * h * 4; x++) { hsh ^= rgba[x]; hsh *= 16777619u; }
          { static int kc = -1; if (kc < 0) { const char* e = getenv("RSX_FE_KEYCAPS"); kc = e ? atoi(e) : 1; }   /* v160r: on again (drawn signs) */   /* v160p: icons untouched by default (hints are rewritten as words) */
            if (kc && hsh == 0xE82A7DC8u && w == 512 && h == 256 && tl.fmt == RSX_TEXFMT_BC3) {
                memcpy(s_fe_atlas, rgba, sizeof s_fe_atlas); s_fe_atlas_pending = 1;
                fe_put_keycap(_map0, pitch, 376, 12, "ESC");     /* Circle = back   */
                fe_put_keycap(_map0, pitch, 464, 12, "SPACE");   /* Cross  = select */
                static int said = 0; if (said < 3 && ++said) fprintf(stderr, "[keycaps] menu button pictures -> ESC / SPACE (%u mip levels, off 0x%08X)%c", nm, off, 10);
            } }
          if (!fe) { static u32 lh[16]; static int nl = 0; int k = 0; for (; k < nl; k++) if (lh[k] == hsh) break;
                     if (k == nl && nl < 16) { lh[nl++] = hsh; fprintf(stderr, "[keycaps] game 512x256 BC3 atlas hash 0x%08X%s%c", hsh, hsh == 0xE82A7DC8u ? " (menu icons)" : "", 10); } }
          if (ptex) { static u32 done[16]; static int nd = 0; int k = 0; for (; k < nd; k++) if (done[k] == off) break;
              if (k == nd && nd < 16) { done[nd++] = off;
                  char pn[96]; snprintf(pn, sizeof pn, "logs\\ptex_%08X_%ux%u.bmp", off, w, h);
                  FILE* f = fopen(pn, "wb");
                  if (f) { const u32 W = w * 2, H = h * 4, rowp = (W * 3 + 3) & ~3u, fsz = 54 + rowp * H;   /* left: rgb, right: alpha; x2 */
                      u8 hd[54] = {'B','M'}; hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[10]=54; hd[14]=40;
                      hd[18]=(u8)(W*2); hd[19]=(u8)((W*2)>>8); hd[22]=(u8)H; hd[23]=(u8)(H>>8); hd[26]=1; hd[28]=24;
                      const u32 rp2 = (W * 2 * 3 + 3) & ~3u, fs2 = 54 + rp2 * H; hd[2]=(u8)fs2; hd[3]=(u8)(fs2>>8); hd[4]=(u8)(fs2>>16);
                      fwrite(hd, 1, 54, f);
                      for (int Y = (int)H - 1; Y >= 0; Y--) { u32 nb = 0;
                          for (u32 X = 0; X < W * 2; X++) { const u32 sx = (X % W) / 2, sy = (u32)Y / 4 % h;
                              const u8* q = rgba + ((u64)sy * w + sx) * 4; u8 px[3];
                              if (X < W) { px[0] = q[2]; px[1] = q[1]; px[2] = q[0]; } else px[0] = px[1] = px[2] = q[3];
                              fwrite(px, 1, 3, f); nb += 3; }
                          while (nb % 4) { fputc(0, f); nb++; } }
                      fclose(f); fprintf(stderr, "[ptex] dumped %s fmt=0x%X%c", pn, tl.fmt, 10); } } }
          static u32 seen[512]; static int ns = 0; int dup = 0;
          for (int i = 0; i < ns; i++) if (seen[i] == hsh) { dup = 1; break; }
          if (s_fe_dump && !dup && ns < 512) {   /* v160k: dumping only with RSX_FE_ICONS=1 */
              seen[ns++] = hsh;
              char pn[128]; snprintf(pn, sizeof pn, "logs\\fe_icons\\ic_%08X_%ux%u_fmt%02X.bmp", hsh, w, h, fmt & 0xFFu);
              FILE* f = fopen(pn, "wb");
              if (f) {
                  const u32 S = (w <= 128 && h <= 128) ? 4u : 1u;   /* x4 so small icons are visible */
                  const u32 W = w * S, H = h * S, rowb = W * 3, rowp = (rowb + 3) & ~3u, fsz = 54 + rowp * H;
                  u8 hd[54] = {'B','M'};
                  hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                  hd[10]=54; hd[14]=40; hd[18]=(u8)W; hd[19]=(u8)(W>>8); hd[22]=(u8)H; hd[23]=(u8)(H>>8); hd[26]=1; hd[28]=24;
                  fwrite(hd, 1, 54, f);
                  for (int Y = (int)H - 1; Y >= 0; Y--) {
                      const u8* r = rgba + (u64)(Y / S) * w * 4;
                      for (u32 X = 0; X < W; X++) {
                          const u8* q = r + (X / S) * 4; const u32 al = q[3];
                          u8 px[3]; for (int c = 0; c < 3; c++) px[c] = (u8)((q[2 - c] * al + 96 * (255 - al)) / 255);
                          fwrite(px, 1, 3, f);
                      }
                      { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                  }
                  fclose(f);
              }
          }
      } }
    /* TEX_SAVE=1: dump the first few converted ARGB uploads as BMPs (rgb +
     * alpha channel separately) -- ground-truth for "is the guest texture
     * wrong or is the sampling wrong" questions (wave's hue palette). */
    /* TEX_SAVE_MAIN=<every>: also dump every <every>-th 1280x720 upload from
     * MAIN memory (movie frames written by cellVdec), up to 6 of them. */
    int _tsm = 0;
    { static int every = -1, k = 0, saved = 0;
      if (every < 0) { const char* e = getenv("TEX_SAVE_MAIN"); every = e ? atoi(e) : 0; }
      if (every > 0 && argb && _nfaces == 1 && w == 1280 && h == 720 && off < 0xC0000000u &&
          saved < 6 && (k++ % every) == every - 1) { saved++; _tsm = 1; } }
    if (argb && _f + 1 == _nfaces && (_tsm || getenv("TEX_SAVE"))) { static int _ts = 0; if (_tsm || _ts < 8) { if (!_tsm) _ts++;
        /* For a cube, write every face: the point of the dump is to check the
         * assumed 6-face layout against the game's cubemap art. */
        for (u32 _face = 0; _face < _nfaces; _face++)
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
            if (_nfaces > 1)
                snprintf(pn, sizeof(pn), "cube_%08X_%ux%u_f%u_%s.bmp", _off0, w, h,
                         _face, pass ? "a" : "rgb");
            else
            { static int _seq = 0; if (pass == 0) _seq++;
                snprintf(pn, sizeof(pn), "logs/tex_%02d_%08X_%ux%u_%s.bmp", _seq, off, w, h,
                     pass ? "a" : "rgb"); }
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                u32 fsz = 54 + rowp * h;
                u8 hd[54] = {'B','M'};
                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                hd[10]=54; hd[14]=40;
                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                hd[26]=1; hd[28]=24;
                fwrite(hd, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = _map0 + (u64)_face * pitch * _face_rows
                                     + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        u8 px[3];
                        if (pass) { px[0]=px[1]=px[2]=srow[x*4+3]; }
                        else { px[0]=srow[x*4+2]; px[1]=srow[x*4+1]; px[2]=srow[x*4+0]; }
                        fwrite(px, 1, 3, f);
                    }
                    { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                }
                fclose(f);
                fprintf(stderr, "[TEX_SAVE] %s\n", pn);
            }
        }
    } }
    /* TEX_AVGDBG=1: one line per distinct bound texture offset with its average
     * source colour. duck.tga is overwhelmingly yellow (avg ~243,191,23), so this
     * says whether the duck's image is bound at all -- and if it is, the duck is
     * being drawn somewhere and the question is where, not whether. */
    { static int avgd = -1;
      if (avgd < 0) { const char* e = getenv("TEX_AVGDBG"); avgd = e ? atoi(e) : 0; }
      if (avgd && !dxt && bpp == 4) {
        static u32 seen[64]; static int nseen = 0;
        int known = 0;
        for (int i2 = 0; i2 < nseen; i2++) if (seen[i2] == off) { known = 1; break; }
        if (!known && nseen < 64) {
            seen[nseen++] = off;
            /* Report the source as R,G,B,A -- the layout TEX_RGBA uploads (PSGL
             * writes GL_RGBA/UNSIGNED_INT_8_8_8_8, so bytes land R,G,B,A). Alpha
             * matters as much as colour here: a blended surface whose alpha is a
             * flat 255 renders opaque no matter how correct the blend state is. */
            u64 sr = 0, sg = 0, sb = 0, sa = 0; u32 cnt = 0, amin = 255, amax = 0;
            for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
                const u8* q = vm_base + off + i2;
                sr += q[0]; sg += q[1]; sb += q[2]; sa += q[3]; cnt++;
                if (q[3] < amin) amin = q[3];
                if (q[3] > amax) amax = q[3];
            }
            if (cnt) fprintf(stderr, "[TEXAVG] raw=0x%08X %4ux%-4u fmt=0x%02X"
                                     " avgRGBA=(%3u,%3u,%3u,%3u) alpha[%u..%u]%c",
                             key_off, w, h, fmt, (u32)(sr/cnt), (u32)(sg/cnt),
                             (u32)(sb/cnt), (u32)(sa/cnt), amin, amax, 10);
        }
      } }
    /* Content-identify the duck's texture so filters can say "duck". */
    if (!dxt && bpp == 4 && w == 512 && h == 512 && s_duck_raw != key_off) {
        u64 ar = 0, ag = 0, ab = 0; u32 n = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 211) {
            const u8* q = vm_base + off + i2;
            ar += q[0]; ag += q[1]; ab += q[2]; n++;
        }
        if (n && ar/n > 200 && ag/n > 140 && ab/n < 90) {
            s_duck_off = key_off;
            { static u32 last = 0; if (last != key_off) { last = key_off;
                fprintf(stderr, "[DUCKTEX] resolved=0x%08X identified by content%c", key_off, 10); } }
        }
    }
    /* TEX_HILITE=1: paint the duck's texture pure red. duck.tga is overwhelmingly
     * yellow (avg 243,191,23); with the R,G,B,A source order that is bytes
     * s[0] high, s[1] mid, s[2] low. Flooding it with an unmistakable colour
     * answers "where on screen is the duck drawn" directly, which neither the
     * vertex data nor a pixel count can. Diagnostic only. */
    { static int hil = -1;
      if (hil < 0) { const char* e = getenv("TEX_HILITE"); hil = e ? atoi(e) : 0; }
      if (hil && !dxt && bpp == 4 && w == 512 && h == 512) {
        u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
            const u8* q = vm_base + off + i2;
            sr += q[0]; sg += q[1]; sb += q[2]; cnt++;
        }
        if (cnt) {
            u32 ar = (u32)(sr/cnt), ag = (u32)(sg/cnt), ab = (u32)(sb/cnt);
            if (ar > 200 && ag > 140 && ab < 90) {
                for (u32 y = 0; y < h; y++) {
                    u8* drow = (u8*)mapped + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                        drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                    }
                }
                { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
                    fprintf(stderr, "[TEXHILITE] duck texture off=0x%08X avg=(%u,%u,%u) -> red%c",
                            off, ar, ag, ab, 10); }
            }
        }
      } }
    /* TEX_MARKEMPTY=1: paint any texture whose guest source is entirely zero a
     * flat magenta instead of uploading the empty bytes. An unresolved texture
     * and a legitimately black surface are indistinguishable on screen, so this
     * says WHICH geometry is missing its image -- diagnostic only. */
    { static int mark = -1;
      if (mark < 0) { const char* e = getenv("TEX_MARKEMPTY"); mark = e ? atoi(e) : 0; }
      if (mark && !dxt) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i2 = 0; i2 < sz && i2 < 0x40000u; i2 += 61) if (vm_base[off + i2]) nz++;
        if (!nz) {
            for (u32 y = 0; y < h; y++) {
                u8* drow = (u8*)mapped + (u64)y * pitch;
                for (u32 x = 0; x < w; x++) {
                    /* Pure RED. Magenta collided with this title's wall tint,
                     * so the marker was indistinguishable from real geometry. */
                    drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                    drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                }
            }
            { static int _n = 0; if (PS3_LOG_FIRST(_n, 8))
                fprintf(stderr, "[TEXEMPTY] off=0x%08X %ux%u fmt=0x%02X -> magenta%c",
                        off, w, h, fmt, 10); }
        }
      } }
    }   /* end per-face conversion */
    off = _off0; mapped = _map0;
    if (nm > 1) {   /* decode the guest's lower levels, packed after level 0 */
        u32 goff = off, lw = w, lh = h;
        for (u32 L = 1; L < nm; L++) {
            goff += dxt ? (((lw + 3) / 4) * ((lh + 3) / 4) * (tl.block_bytes ? tl.block_bytes : 16u))
                        : (lw * lh * bpp);
            lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
            rsx_tex_layout tll;
            rsx_texture_layout_pitched(fmt, lw, lh, 0, &tll);
            rsx_texture_decode((u8*)_map0 + lvl_off[L], lvl_pitch[L], vm_base + goff, lw, lh, &tll,
                               rsx_texture_argb_is_rgba());
            /* v160l: rebuild the key-cap cells in this level from the patched level 0 */
            if (s_fe_atlas_pending && dxt && lw >= 4 && lh >= 4) {
                static const u32 cells[2][2] = { { 376, 12 }, { 464, 12 } };
                static const char* const labels[2] = { "ESC", "SPACE" };
                const u32 sc = 1u << L;
                for (int c = 0; c < 2; c++) {
                    const int cs = (int)(44 / sc); static u8 capimg[44 * 44 * 4];
                    const u32 cx0 = cells[c][0] / sc, cy0 = cells[c][1] / sc;
                    if (cs >= 6) fe_keycap_rgba(capimg, cs, labels[c]);
                    const u32 rx0 = (cells[c][0] / sc) & ~3u, ry0 = (cells[c][1] / sc) & ~3u;
                    u32 rx1 = ((cells[c][0] + 44 + sc - 1) / sc + 3) & ~3u, ry1 = ((cells[c][1] + 44 + sc - 1) / sc + 3) & ~3u;
                    if (rx1 > lw) rx1 = lw; if (ry1 > lh) ry1 = lh;
                    for (u32 by = ry0; by < ry1; by += 4) for (u32 bx = rx0; bx < rx1; bx += 4) {
                        u8 px[16][4];
                        for (int i = 0; i < 16; i++) {
                            const u32 X = (bx + (i & 3)) * sc, Y = (by + i / 4) * sc;
                            u32 acc[4] = { 0, 0, 0, 0 };
                            for (u32 yy = 0; yy < sc; yy++) for (u32 xx = 0; xx < sc; xx++) {
                                const u8* q = s_fe_atlas + (((Y + yy) % 256) * 512 + ((X + xx) % 512)) * 4;
                                for (int k = 0; k < 4; k++) acc[k] += q[k];
                            }
                            for (int k = 0; k < 4; k++) px[i][k] = (u8)(acc[k] / (sc * sc));
                            const u32 mx = bx + (i & 3), my = by + i / 4;
                            if (cs >= 6 && mx >= cx0 && my >= cy0 && mx < cx0 + (u32)cs && my < cy0 + (u32)cs)
                                memcpy(px[i], capimg + ((my - cy0) * (u32)cs + (mx - cx0)) * 4, 4);
                        }
                        fe_bc3_block((u8*)_map0 + lvl_off[L] + (u64)(by / 4) * lvl_pitch[L] + (bx / 4) * 16, px);
                    }
                }
            }
        }
    }
    s_fe_atlas_pending = 0;
    if (!use_ring) t->up->lpVtbl->Unmap(t->up, 0, NULL);

    if (!fresh) {   /* reused resource: PSR -> COPY_DEST first */
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up_src; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = dxfmt;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    for (u32 _f = 0; _f < _nfaces; _f++) {
        dst.SubresourceIndex        = _f * nm;  /* cube face = array slice */
        src.PlacedFootprint.Offset  = up_base + (u64)_f * pitch * _face_rows;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    }
    for (u32 L = 1; L < nm; L++) {
        u32 lw = w >> L, lh = h >> L; if (!lw) lw = 1; if (!lh) lh = 1;
        dst.SubresourceIndex = L;
        src.PlacedFootprint.Offset = up_base + lvl_off[L];
        src.PlacedFootprint.Footprint.Width = lw;
        src.PlacedFootprint.Footprint.Height = lh;
        src.PlacedFootprint.Footprint.RowPitch = lvl_pitch[L];
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    }
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    /* SRV at heap slot 1+slot */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = dxfmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* RSX B8 replicates the byte into all four channels (dbgfont's FP reads
     * coverage from .w); DXGI R8 defaults to (r,0,0,1), so swizzle (R,R,R,R)
     * = encoded 0x1000 (component 0 in all lanes + always-set bit). ARGB8
     * keeps the identity mapping. */
    sv.Shader4ComponentMapping = (basef == 0x81) ? 0x1000 : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = nm;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(1 + slot) * s_d3d.srv_inc;
    if (cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = 1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, t->res, &sv, sh);

    t->off = off; t->key = key_off; t->w = w; t->h = h; t->fmt = fmt;
    t->cube = cube; t->used = 1; t->last_use = ++s_tex_clock; t->dxfmt = (u32)dxfmt;
    t->nmips = nm;
    { u32 b = tex_bucket(key_off); t->next = s_tex_head[b]; s_tex_head[b] = slot + 1;
      t->chained = 1; }
    { u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
      t->csum = TEX_CSUM(vm_base + off, nb);
      t->csum_q = tex_csum_n(vm_base + off, nb, 128u); t->csum_n = 0; }
    #undef TEX_CSUM
    return slot;
}

/* ---------------------------------------------------------------------------
 * Render-to-texture: offscreen RT pool.
 *
 * Draws/clears targeting a non-display surface (SET_SURFACE_COLOR_xOFFSET
 * not registered via cellGcmSetDisplayBuffer) render into a pooled RGBA8
 * texture keyed by the surface's resolved vm offset; a later draw binding a
 * texture at that offset samples the RT directly instead of guest memory.
 * RGBA8 for every RT (even half-float guest surfaces) keeps the existing
 * PSO/RTV formats -- values clamp to [0,1], which demosaic's RGB data fits.
 * -----------------------------------------------------------------------*/

/* Snapshot the VP constant bank + viewport epilogue for one draw into its
 * vp_cb slot. Runs at record time so every draw keeps the constants that
 * were live when the guest issued it. */
/* Resistance port: one stderr line per distinct (VS, FP, vertex layout):
 * "[VSATTR] vs=<idx> hash=<vs hash> fp=<addr> a<n>:t<type>s<size>@<stride>[M]".
 * Ties a decompiled program to the attribute formats feeding it. */
static void vs_attr_log(const rsx_state* st, int vs_idx, u32 fp)
{
    static int on = -1; if (on < 0) { const char* e = getenv("VSATTR_LOG"); on = e ? atoi(e) : 0; }   /* v189: diag, off by default (per draw) */
    if (!on || !st) return;
    u32 h = 2166136261u;
    h ^= (u32)vs_idx; h *= 16777619u; h ^= fp; h *= 16777619u;
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        if (!a->enabled) continue;
        h ^= (u32)i | (a->type << 8) | (a->size << 12) | (a->stride << 16); h *= 16777619u;
    }
    /* v181: open-addressed set (was a linear scan of up to 1024 per draw) */
    static u32 seen[4096]; static int ns = 0;
    if (!h) h = 1;
    { u32 i = (h * 2654435761u) >> 20;
      for (;; i = (i + 1) & 4095u) { if (seen[i] == h) return; if (!seen[i]) break; }
      if (ns >= 2048) return;
      seen[i] = h; ns++; }
    char buf[1024]; int n = snprintf(buf, sizeof buf, "[VSATTR] vs=%d hash=%08X fp=0x%08X", vs_idx,
        (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) ? s_d3d.vp_vs[vs_idx].hash : 0, fp);
    for (int i = 0; i < 16 && n < (int)sizeof buf - 80; i++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        if (!a->enabled) continue;
        n += snprintf(buf + n, sizeof buf - n, " a%d:t%us%u@%u%s", i, a->type, a->size, a->stride,
                      (a->offset & 0x80000000u) ? "M" : "");
        { extern uint8_t* vm_base; extern u32 cellGcmResolveLocated(int, u32);
          u32 off = (a->offset & 0x7FFFFFFFu) + st->vertex_data_base_offset;
          const u8* p = vm_base + cellGcmResolveLocated((a->offset & 0x80000000u) ? 0 : 1, off);
          n += snprintf(buf + n, sizeof buf - n, "[o=%X f=%u %02X%02X%02X%02X%02X%02X%02X%02X]",
                        a->offset, a->frequency, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); }
    }
    fprintf(stderr, "%s bo=%X bi=%u div=%X%c", buf, st->vertex_data_base_offset,
            st->vertex_data_base_index, st->frequency_divider_op, 10);
}

static u32 s_fpnat[256]; static int s_fpnat_n = 0;   /* v163: "noat" in logs\fpkill.txt = alpha test off for these FPs */
static int fp_in(const u32* l, int n, u32 fp);
static void vp_record_cb(u32 slot, int vs_idx, const D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!s_d3d.vp_cb_mapped || !st || slot >= MAX_DRAWS) return;
    /* FP texcoord scale (b1): 1/size for UNnormalized textures (fmt bit
     * 0x40 -- wave samples everything in texel space), 1.0 otherwise. */
    if (s_d3d.vp_fpcb_mapped) {
        float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
            + ((u64)T_PARITY * MAX_DRAWS + slot) * VP_FPCB_STRIDE);
        for (int _u = 0; _u < 4; _u++) {
            float sx = 1.0f, sy = 1.0f;
            if (dr && dr->tex[_u].set && (dr->tex[_u].fmt & 0x40) &&
                dr->tex[_u].w && dr->tex[_u].h) {
                sx = 1.0f / (float)dr->tex[_u].w;
                sy = 1.0f / (float)dr->tex[_u].h;
            }
            ts[_u*4+0] = sx; ts[_u*4+1] = sy; ts[_u*4+2] = 0.0f; ts[_u*4+3] = 0.0f;
        }
        /* rsx_shscale (float4 #69): shadow-map sub-rect scale, 1 unless the
         * pre-pass binds a larger depth buffer (see [SHADOWMAP]). */
        ts[276] = 1.0f; ts[277] = 1.0f; ts[278] = 0.0f; ts[279] = 0.0f;
        /* v125: WPOS origin for the fragment program (rsx_shscale.zw): flip y
         * when the guest's SHADER_WINDOW origin is the bottom edge. RSX_WPOS=0
         * keeps D3D's top-down SV_POSITION. */
        { static int wp = -1;
          if (wp < 0) { const char* e = getenv("RSX_WPOS"); wp = e ? atoi(e) : 1; }
          const u32 sw = st->shader_window;
          if (wp && sw && ((sw >> 12) & 0xFu) == 1u) {
              ts[278] = 1.0f;
              ts[279] = (float)(sw & 0xFFFu);
          } }
        /* rsx_alphatest (b1[4]): x=enable, y=ref/255, z=func-0x200. D3D12
         * has no fixed alpha test; guest FPs discard on it dynamically
         * (wave's colour wheel is a disc via alpha test -- without it the
         * palette drew as an opaque square). */
        static int s_no_at = -1; if (s_no_at < 0) s_no_at = getenv("NO_ALPHATEST") ? 1 : 0;   /* v163: was a getenv per draw */
        if (dr && !s_no_at && !(s_fpnat_n && dr->fp_addr && fp_in(s_fpnat, s_fpnat_n, dr->fp_addr))) {
            { static int _at = 0;
              if (((dr->alpha_ctl >> 16) & 1u) && PS3_LOG_FIRST(_at, 4))
                  printf("[ALPHATEST] enable func=%u ref=%u\n",
                         (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); }
            { static u32 seen[96][2]; static int ns = 0; int k = 0;   /* v163b diag: alpha test per FP */
              for (; k < ns; k++) if (seen[k][0] == dr->fp_addr && seen[k][1] == dr->alpha_ctl) break;
              if (k == ns && ns < 96) { seen[ns][0] = dr->fp_addr; seen[ns][1] = dr->alpha_ctl; ns++;
                  FILE* f = fopen("logs\\alphactl.txt", ns == 1 ? "w" : "a");
                  if (f) { fprintf(f, "fp=0x%08X enable=%u func=0x%X ref=%u\n", dr->fp_addr, (dr->alpha_ctl >> 16) & 1u,
                                   (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); fclose(f); } } }
            ts[16] = (float)((dr->alpha_ctl >> 16) & 1u);
            ts[17] = (float)(dr->alpha_ctl & 0xFFu) / 255.0f;
            ts[18] = (float)((dr->alpha_ctl >> 8) & 0x07u);
            ts[19] = 0.0f;
        } else {
            ts[16] = 0.0f; ts[17] = 0.0f; ts[18] = 7.0f; ts[19] = 0.0f;
        }

        /* fp_k[]: the program's inline fragment constants, re-read from the
         * guest ucode every draw. The decompiler emits fp_k[i] lookups instead
         * of baking these in as literals, so the compiled shader no longer
         * changes when the title re-patches them -- which is what forced the
         * pipeline cache to key on a hash of the ucode bytes. */
        if (dr && dr->fp_addr) {
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int, u32);
            u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1,
                                             dr->fp_addr & ~0x3u);
            if (vm_base && foff != 0xFFFFFFFFu)
                rsx_fp_extract_consts(vm_base + foff, 4096, &ts[20], 64);   /* fp_k[64] */
            /* FPK_DBG=1: the fragment constants as the shader will see them.
             * A program whose only output is fp_k[0].xxxx renders black if
             * those constants never arrive, which looks identical from the
             * outside to geometry that never rasterised. */
            { static int fk = -1; if (fk < 0) fk = getenv("FPK_DBG") ? 1 : 0;
              static int n = 0;
              if (fk && n < 12 && dr->fp_addr != 0x00EC2B02u) { n++;
                  fprintf(stderr, "[FPK] tex0=0x%08X fp=0x%08X k0=(%g %g %g %g)"
                                  " k1=(%g %g %g %g)%c",
                          dr->tex[0].raw, dr->fp_addr,
                          ts[20], ts[21], ts[22], ts[23],
                          ts[24], ts[25], ts[26], ts[27], 10);
                  const u32 raw = dr->fp_addr & ~0x3u;
                  const u32 ea_main  = cellGcmResolveLocated(0, raw);
                  const u32 ea_local = cellGcmResolveLocated(1, raw);
                  fprintf(stderr, "[FPK]   ucode main@0x%08X:", ea_main);
                  for (int b = 0; b < 48; b++) fprintf(stderr, "%s%02X", (b%16)?" ":"  |", vm_base[ea_main + b]);
                  fputc(10, stderr); }
              /* distinct fragment programs seen -- if every draw shares one,
               * SET_SHADER_PROGRAM is not being tracked per draw. */
              if (fk) { static u32 seen[16]; static int ns = 0; int f = 0;
                  for (int q = 0; q < ns; q++) if (seen[q] == dr->fp_addr) f = 1;
                  if (!f && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[FPK] distinct fp #%d = 0x%08X%c", ns, dr->fp_addr, 10); } } }
        }
    }
    char* dst = (char*)s_d3d.vp_cb_mapped
        + ((u64)T_PARITY * MAX_DRAWS + slot) * VP_CB_STRIDE;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && !s_d3d.vp_vs[vs_idx].c_all) {
        const VPVSEntry* ve = &s_d3d.vp_vs[vs_idx];
        for (u32 r = 0; r < ve->crun_n; r++)
            memcpy(dst + (size_t)ve->crun[r][0] * 16, st->vertex_constants[ve->crun[r][0]], (size_t)ve->crun[r][1] * 16);
    } else
        memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    /* VP_MVP=<N>: the constant bank as the shader will see it, for the first N
     * draws. A snapshot taken from a stale rsx_state looks exactly like a broken
     * vertex program from the outside -- both give zero fragments. */
    { static int cap = -1, seen = 0;
      if (cap < 0) { const char* e = getenv("VP_MVP"); cap = e ? atoi(e) : 0; }
      if (cap && seen < cap) { seen++;
        const float* c = (const float*)dst;
        int last_nz = -1;
        for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++)
            if (c[i] > 1e-9f || c[i] < -1e-9f) last_nz = i / 4;
        fprintf(stderr, "[VPMVP] slot=%u lastNZ=c%d  c260=(%g %g %g %g) c467=(%g %g %g %g)\n",
                slot, last_nz,
                c[260*4+0], c[260*4+1], c[260*4+2], c[260*4+3],
                c[467*4+0], c[467*4+1], c[467*4+2], c[467*4+3]); } }
    /* RSX_DBG_CB=<hex tex0 offset>: the constants as they land in the per-draw
     * constant buffer for that texture's draws -- i.e. exactly what the shader
     * samples, not what the CPU-side rsx_state holds. Those two agreeing is an
     * assumption worth checking directly when a draw transforms to nothing. */
    const float* vpx_dbg = (const float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    { static const char* dc = (const char*)1; static u32 wantc = 0; static int n = 0;
      if (dc == (const char*)1) { dc = getenv("RSX_DBG_CB");
                                  wantc = dc ? (u32)strtoul(dc, NULL, 16) : 0; }
      if (wantc && dr && dr->tex[0].raw == wantc && n < 4) { n++;
          const float* c = (const float*)dst;
          fprintf(stderr, "[DUCKCB] slot=%u vs_idx=%d posscale=(%g %g %g) posoffset=(%g %g %g)%c",
                  slot, vs_idx, vpx_dbg[0], vpx_dbg[1], vpx_dbg[2],
                  vpx_dbg[4], vpx_dbg[5], vpx_dbg[6], 10);
          for (int r = 256; r <= 259; r++)
              fprintf(stderr, "[DUCKCB]   c%d=(%g %g %g %g)%c", r,
                      c[r*4+0], c[r*4+1], c[r*4+2], c[r*4+3], 10);
      } }

    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    /* v181: built in a local and stored once -- dst is write-combined upload
     * memory, and the += below read it back (uncached) on every draw. */
    float vpx[12];
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
    /* vp_branch (vpx[8..11]): the transform branch bits for BRB/CLB. */
    { u32* vb = (u32*)(vpx + 8); vb[0] = st->transform_branch_bits; vb[1] = vb[2] = vb[3] = 0; }
    /* Resistance port: polygon offset applied per draw in the vertex shader
     * (RSX_POLY_OFFSET=2, default): the game varies scale/bias per decal, far
     * too many distinct values for PSO keys. Window depth moves by `bias`
     * D24 units; the slope term is approximated as a fixed 32-unit gradient.
     * Decals/soot/blob shadows z-fought the ground without it. */
    { static int en = -1;
      if (en < 0) { const char* e = getenv("RSX_POLY_OFFSET"); en = e ? atoi(e) : 1; }
      if (en == 2 && st->poly_offset_fill) {
          static float slope_k = -1.0f;
          if (slope_k < 0.0f) { const char* e = getenv("RSX_POLY_SLOPE"); slope_k = e ? (float)atof(e) : 32.0f; }
          vpx[6] += (st->poly_offset_bias + st->poly_offset_scale * slope_k) * (1.0f / 16777216.0f);
      } }
    /* DBG_ZOOM=<k> with DBG_CENTER="cx,cy": a debug camera applied in CLIP space,
     * not a framebuffer crop -- the geometry is re-rasterized at full resolution.
     * The shader epilogue already computes
     *     pos.xyz = _p.xyz * vp_posscale + _p.w * vp_posoffset
     * so recentring NDC point c and magnifying by k is exactly
     *     pos.xy = _p.xy * k + _p.w * (-c * k).
     * Rubber Ducky frames its ducks near the bottom edge of the viewport and its
     * camera only moves from the analog sticks, so without this there is no way
     * to look at the subject of the demo. It changes the view, nothing else:
     * same draws, same transform, same textures. */
    { static int z = -1; static float k = 1.0f, cx = 0.0f, cy = 0.0f;
      if (z < 0) { const char* e = getenv("DBG_ZOOM");
                   k = e ? (float)atof(e) : 1.0f;
                   z = (e && k > 0.0f) ? 1 : 0;
                   const char* ce = getenv("DBG_CENTER");
                   if (ce) { double a = 0, b = 0; sscanf(ce, "%lf,%lf", &a, &b);
                             cx = (float)a; cy = (float)b; } }
      if (z) { float ux = cx, uy = cy;
               static int lock = -1;
               if (lock < 0) { const char* le = getenv("DBG_LOCK"); lock = le ? atoi(le) : 0; }
               if (lock && s_lock_valid) { ux = s_lock_x; uy = s_lock_y; }
               vpx[0] = k; vpx[1] = k; vpx[4] = -ux * k; vpx[5] = -uy * k; } }

    /* Garbage-projection fallback (vkcube; see the original comment). */
    int uses_c03 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)
                       ? s_d3d.vp_vs[vs_idx].uses_c03 : s_d3d.vp_uses_c03;
    float* c = (float*)dst;
    float c00 = st->vertex_constants[0][0];   /* v181: not read back from dst (write-combined) */
    int garbage = !(c00 > 0.0f && c00 < 8.0f);
    /* Resistance port: opt-in only (VP_GARBAGEFIX=1). A legitimate transform
     * with c[0] <= 0 -- a camera turned 90+ degrees, Fall of Man's death camera
     * looking down at the body -- was replaced by a made-up projection and the
     * death screen rendered black. */
    { static int gf = -1; if (gf < 0) { const char* e = getenv("VP_GARBAGEFIX"); gf = e ? atoi(e) : 0; }
      if (!gf) garbage = 0; }
    static int s_nofix = -1, s_fix = -1;   /* v181: were two getenv calls per draw */
    if (s_nofix < 0) { s_nofix = getenv("VP_NOFIXPROJ") ? 1 : 0; s_fix = getenv("VP_FIXPROJ") ? 1 : 0; }
    if (s_nofix) garbage = 0;
    if ((garbage && uses_c03) || s_fix) {
        { static int _fb = 0; if (PS3_LOG_FIRST(_fb, 6))
            printf("[VPFB] fallback proj on draw slot %u (c00=%g vs_idx=%d)\n",
                   slot, c00, vs_idx); }
        for (int _i = 0; _i < 16; _i++) c[_i] = 0.0f;
        c[0]=1.358f; c[5]=2.414f; c[11]=1.0f;
        c[10]=1.0f/99.0f; c[14]=-1.0f/99.0f;
        vpx[2] = 1.0f; vpx[6] = 0.0f;
    }
    if (dr) ((D3D12DrawRecord*)dr)->stencil_ref = st->stencil_ref & 0xFFu;   /* v178 */
    /* v165: FSR -- history copy (un-jittered) + sub-pixel jitter for scene draws. */
    memcpy(dst + RSX_MAX_VERTEX_CONSTANTS * 16, vpx, sizeof vpx);
    if (dr) { if (fsr_mode()) fsr_record((D3D12DrawRecord*)dr, vs_idx, dst, (const char*)st->vertex_constants, vpx); else ((D3D12DrawRecord*)dr)->fsr = 0; }
}

/* Which offscreen surface (if any) does the current RSX state render to?
 * Returns 0 for a display buffer (backbuffer), else the surface's RAW RSX
 * offset. RTs are keyed by raw offset -- a texture bound at the same raw
 * offset is the same buffer (surface and texture registers share the offset
 * space; location bits differ but one title doesn't alias local vs main at
 * one offset), which sidesteps guessing the surface's context DMA location. */
/* Resistance port: which display buffer the bound colour surface is, as
 * offset+1 (0 = not a display buffer). Both display buffers collapse onto the
 * one swapchain backbuffer (rt_off 0), but the death screen renders the scene
 * into display buffer B (0x3A0000) and then draws a black-and-white pass that
 * SAMPLES B into display buffer A. Knowing which one each draw wrote lets the
 * replay hand that pass a snapshot of the backbuffer. */
extern int cellGcmOffsetIsDisplay(u32 offset);
static u32 current_disp_tag(void)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return 0;
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    return cellGcmOffsetIsDisplay(raw) ? raw + 1u : 0u;
}
static int disp_tex_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_DISP_AS_TEX"); v = e ? atoi(e) : 1; }
    return v;
}

static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3])
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_mrt) out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B),
     * 0x17 = MRT2 (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    if (out_mrt) {
        if (st->color_target >= 0x13) out_mrt[0] = st->surface_color_offset[1];
        if (st->color_target >= 0x17) out_mrt[1] = st->surface_color_offset[2];
        if (st->color_target >= 0x1F) out_mrt[2] = st->surface_color_offset[3];
    }
    { static int _rs = -1; if (_rs < 0) _rs = getenv("RT_SEQDBG") ? 1 : 0;
      static u32 _last = 0; static int _n2 = 0;
      if (_rs && st->surface_color_offset[0] != _last && _n2 < 400000) {
          _last = st->surface_color_offset[0]; _n2++;
          fprintf(stderr, "[RTSEQ] #%d -> 0x%X%c", _n2, _last, 10); } }
    { static int _ro = -1; if (_ro < 0) _ro = getenv("RT_OFFDBG") ? 1 : 0;
      static u32 _seen[32]; static int _ns = 0;
      if (_ro) { u32 _k = st->surface_color_offset[0] ^ (st->color_target << 24)
                        ^ (st->surface_clip_w << 8) ^ st->surface_format;
        int _f = 0; for (int _i = 0; _i < _ns; _i++) if (_seen[_i] == _k) _f = 1;
        if (!_f && _ns < 32) { _seen[_ns++] = _k;
        fprintf(stderr, "[RTOFF] fmt=0x%X tgt=0x%X off[0]=0x%X off[1]=0x%X zeta=0x%X clip=%ux%u disp=%d -> 0x%X\n",
                st->surface_format, st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h,
                cellGcmOffsetIsDisplay(raw), cellGcmOffsetIsDisplay(raw) ? 0 : raw); } } }
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* RT_DISPLAY_BY_SIZE=1: also treat a single-target surface whose clip
     * exactly matches the display resolution as the backbuffer.
     *
     * cellGcmSetDisplayBuffer only registers the buffers the FLIP may point at;
     * a guest is free to render into a different surface of the same size and
     * flip to it later (PSGL does). Matching offsets alone then classifies every
     * draw as offscreen, has_display stays 0, and render_frame() -- which is
     * what draws ALL recorded geometry -- is never called. The backbuffer shows
     * only the clear, which looks exactly like "nothing rasterizes". */
    { static int _bs = -1; if (_bs < 0) _bs = getenv("RT_DISPLAY_BY_SIZE") ? 1 : 0;
      if (_bs && st->color_target < 0x13 &&
          st->surface_clip_w == s_d3d.width && st->surface_clip_h == s_d3d.height)
          return 0; }
    /* Surface clip dims when sane; else the window size. Any size works --
     * passes draw normalized full-surface quads -- this only picks resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return raw;
}

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> DXGI. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static DXGI_FORMAT rsx_surface_dxgi(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return DXGI_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return DXGI_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return DXGI_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

/* v126: render-target size variants. Fall of Man renders its shadow passes
 * with the colour surface left at an earlier pass's offset and the clip at
 * each shadow map's size, so one offset is targeted at several sizes a frame
 * (0x1680000: 288x288, then 320x192, every frame). The pool kept one resource
 * per offset and rebuilt it on every size change -- wait_for_gpu() plus a new
 * committed resource, twice or more a frame (the render thread's single
 * largest cost: ~half of render_frame in York). Now every (offset, size,
 * format) keeps its own slot; an offset resolves to the variant targeted most
 * recently in record order (s_rt_clock stamps), which is what a later pass
 * sampling that address reads on the RSX. Retired slots go to the grave instead
 * of stalling the GPU. RSX_RT_VARIANTS=0 restores rebuild-in-place. */
static u64 s_rt_clock;
static u32 s_rt_created, s_rt_evicted;   /* stats, PERF2 */
/* v129: a pool slot retargeted in place instead of a new resource (see
 * off_rt_get / zeta_dsv), and depth surfaces created -- PERF2. */
static u32 s_rt_reused, s_zt_created, s_zt_reused;
static u64 s_rt_clock_mark;   /* s_rt_clock when the previous batch began stamping */
static int surf_reuse(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_SURF_REUSE"); e = v ? atoi(v) : 1; }
    return e;
}
static int rt_variants(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_RT_VARIANTS"); e = v ? atoi(v) : 1; }
    return e;
}
/* v189: bloom filter of the offsets render targets sit at. off_rt_find scanned
 * all 256 slots for every texture unit of every draw (several times a draw), and
 * almost every texture is not a render target: ~20% of the render thread in
 * Nottingham. Rebuilt from the slots every batch; bits are added as slots get an
 * offset. A miss is exact, a hit falls through to the scan. */
static u64 s_rt_bloom[1024];
static inline u32 rt_bloom_h(u32 off) { return (off * 2654435761u) >> 16; }
static inline void rt_bloom_add(u32 off) { const u32 h = rt_bloom_h(off); s_rt_bloom[h >> 6] |= 1ull << (h & 63u); }
static inline int rt_bloom_has(u32 off) { const u32 h = rt_bloom_h(off); return (int)((s_rt_bloom[h >> 6] >> (h & 63u)) & 1u); }
static void rt_bloom_rebuild(void)
{
    memset(s_rt_bloom, 0, sizeof s_rt_bloom);
    for (int i = 0; i < MAX_OFF_RTS; i++) if (s_d3d.off_rt[i].res) rt_bloom_add(s_d3d.off_rt[i].off);
}
static int off_rt_find(u32 off)
{
    if (!off || !rt_bloom_has(off)) return -1;
    int best = -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off) {
            if (!rt_variants()) return i;
            if (best < 0 || s_d3d.off_rt[i].last > s_d3d.off_rt[best].last) best = i;
        }
    return best;
}
/* v189: the variant at `off` whose COLOUR was written most recently. A pass
 * that only targets a surface for depth (cmask=0 -- the flare occlusion boxes
 * on the 32x32 variant of 0x1680000) leaves the colour memory at that address
 * as the last colour pass wrote it; on the RSX a colour texture there reads
 * that, not the depth-only variant's untouched (black) colour. The Auger's
 * through-wall effect (fp 0x16764C1) multiplies by that texture: black aura.
 * Falls back to off_rt_find when no variant has a colour write. RSX_RT_COLOUR=0 off. */
static int off_rt_find_col(u32 off)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_RT_COLOUR"); en = e ? atoi(e) : 1; }
    if (!off || !en || !rt_bloom_has(off)) return -1;
    int best = -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off && s_d3d.off_rt[i].clast &&
            (best < 0 || s_d3d.off_rt[i].clast > s_d3d.off_rt[best].clast)) best = i;
    return best;
}
/* Mark slot `slot` as the one its offset now resolves to (record order). */
static void off_rt_stamp(int slot)
{
    if (slot >= 0 && slot < MAX_OFF_RTS && s_d3d.off_rt[slot].res)
        s_d3d.off_rt[slot].last = ++s_rt_clock;
}

/* Ensure an RT resource exists for this surface; (re)creates the RTV at
 * rt_rtv_heap[i] and the SRV at srv_heap[RT_SRV_BASE+i]. */
static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !s_d3d.rt_rtv_heap || !s_d3d.srv_heap) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    DXGI_FORMAT want_fmt = rsx_surface_dxgi(rsx_fmt);
    /* RT_FORCE_RGBA8=1: allocate every offscreen target as R8G8B8A8 rather than
     * its native format. A title whose final composite reads a half-float
     * surface cannot be shown with a plain CopyTextureRegion, and giving it an
     * 8-bit target costs HDR precision but makes the composite a same-format
     * copy. A stopgap for looking at the frame, not a rendering fix. */
    { static int f8 = -1;
      if (f8 < 0) { const char* e = getenv("RT_FORCE_RGBA8"); f8 = e ? atoi(e) : 0; }
      if (f8) want_fmt = DXGI_FORMAT_R8G8B8A8_UNORM; }
    int slot = off_rt_find(off);
    if (slot >= 0 && rt_variants()) {
        /* the exact variant, else a new slot for it (the others stay) */
        slot = -1;
        for (int i = 0; i < MAX_OFF_RTS; i++) {
            OffRT* q = &s_d3d.off_rt[i];
            if (q->res && q->off == off && q->w == w && q->h == h && q->dxgi == (u32)want_fmt) {
                q->used = 1; return i;
            }
        }
    }
    if (rt_variants() && slot < 0) {
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!s_d3d.off_rt[i].res) { slot = i; break; }
        if (slot < 0 && surf_reuse()) {
            /* v129: the pool is full. A slot not targeted this frame with this
             * very size and format is retargeted in place -- no new resource:
             * nothing recorded this frame references it (the SRV loop that
             * samples RTs runs after this one, and resolves offsets afresh),
             * earlier batches run first on the queue, and its RTV/SRV stay
             * valid. York's shadow passes meet a new shadow-map size every few
             * frames as characters move; each used to be a CreateCommittedResource. */
            /* ...and not targeted in the previous batch either: a frame split
             * by a mid-frame flush samples in its second batch what its first
             * one rendered. */
            int same = -1;
            for (int i = 0; i < MAX_OFF_RTS; i++) {
                const OffRT* q = &s_d3d.off_rt[i];
                if (!q->used && q->res && q->w == w && q->h == h && q->dxgi == (u32)want_fmt &&
                    q->last <= s_rt_clock_mark &&
                    (same < 0 || q->last < s_d3d.off_rt[same].last)) same = i;
            }
            if (same >= 0) {
                OffRT* q = &s_d3d.off_rt[same];
                q->off = off; q->used = 1; q->last = 0; q->clast = 0; rt_bloom_add(off);
                s_rt_reused++;
                return same;
            }
        }
        if (slot < 0) {   /* retire the least recently targeted slot not used this frame */
            for (int i = 0; i < MAX_OFF_RTS; i++)
                if (!s_d3d.off_rt[i].used && (slot < 0 || s_d3d.off_rt[i].last < s_d3d.off_rt[slot].last)) slot = i;
            if (slot < 0) return -1;
            grave_defer(s_d3d.off_rt[slot].res); s_d3d.off_rt[slot].res = NULL;
            if (s_d3d.off_rt[slot].up) { grave_defer(s_d3d.off_rt[slot].up); s_d3d.off_rt[slot].up = NULL; }
            s_rt_evicted++;
        }
        s_d3d.off_rt[slot].last = 0; s_d3d.off_rt[slot].clast = 0;
    } else if (slot >= 0) {
        OffRT* r = &s_d3d.off_rt[slot];
        if (r->w == w && r->h == h && r->dxgi == (u32)want_fmt) { r->used = 1; return slot; }
        /* The frame in flight still references this resource -- releasing it here
         * is "was deleted prior to closing the command list" (D3D12 message 921),
         * and every draw recorded against it silently lands nowhere while the
         * replacement reads back empty. Let the GPU finish first. Recreations are
         * rare (a surface changing size), so the stall costs nothing. */
        wait_for_gpu();
        r->res->lpVtbl->Release(r->res); r->res = NULL;   /* dims/format changed */
        if (r->up) { r->up->lpVtbl->Release(r->up); r->up = NULL; }
    } else {
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!s_d3d.off_rt[i].res) { slot = i; break; }
        if (slot < 0) {   /* evict an entry not used this frame */
            for (int i = 0; i < MAX_OFF_RTS; i++)
                if (!s_d3d.off_rt[i].used) { slot = i; break; }
            if (slot < 0) return -1;
            wait_for_gpu();   /* same hazard as above: the frame may still use it */
            s_d3d.off_rt[slot].res->lpVtbl->Release(s_d3d.off_rt[slot].res);
            s_d3d.off_rt[slot].res = NULL;
            if (s_d3d.off_rt[slot].up) {
                s_d3d.off_rt[slot].up->lpVtbl->Release(s_d3d.off_rt[slot].up);
                s_d3d.off_rt[slot].up = NULL;
            }
        }
    }
    OffRT* r = &s_d3d.off_rt[slot];
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = SC(w); td.Height = SC(h); td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = want_fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = td.Format;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, &cv,
            &IID_ID3D12Resource, (void**)&r->res)))
        return -1;
    r->off = off; r->w = w; r->h = h; r->dxgi = (u32)want_fmt; rt_bloom_add(off);
    r->st = D3D12_RESOURCE_STATE_COPY_DEST;
    r->used = 1;
    s_rt_created++;
    /* v171 diag: which surfaces get (re)created -- York churns hundreds a second */
    { static FILE* f = NULL; static int n = 0;
      if (n < 4000) { if (!f) f = fopen("logs\\rtmake.txt", "w");
          if (f) { fprintf(f, "%llu RT off=0x%08X %ux%u fmt=%u slot=%d\n", (unsigned long long)GetTickCount64(), off, w, h, (unsigned)want_fmt, slot);
                   if ((++n & 63) == 0) fflush(f); } } }

    /* Populate the new RT from guest memory: titles CPU-initialise their
     * render-target buffers (wave fills the height fields with texels of
     * (0,0,0, 1.0f) -- the .w is 'inverse of mass'; from an all-zero GPU
     * resource the water simulation can never boot). Guest data is
     * big-endian and tightly packed. */
    {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveOffset(u32);
        u32 bpp = (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == DXGI_FORMAT_R32_FLOAT) ? 4u : 4u;
        u32 pitch = (w * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off);
        /* Resistance port: seeding a render target from guest VRAM is only right
         * when the CPU wrote that memory. Fall of Man's scene target (0x740000,
         * AA surface, pitch 2x the clip width) is only ever written by the RSX,
         * so guest VRAM there is stale garbage -- read with the wrong pitch it
         * became the blue striped menu background. Default off for this port;
         * RT_GUEST_INIT=1 restores the seeding. */
        static int s_rt_seed = -1;
        if (s_rt_seed < 0) { const char* e = getenv("RT_GUEST_INIT"); s_rt_seed = (e && *e != '0') ? 1 : 0; }
        if (s_rt_seed && vm_base && src != 0xFFFFFFFFu && rs_scale() == 1.0f) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (u64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&r->up))) {
                void* mp = NULL; D3D12_RANGE nr = {0,0};
                if (SUCCEEDED(r->up->lpVtbl->Map(r->up, 0, &nr, &mp)) && mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < h; y++) {
                        const u8* srow = sp + (u64)y * w * bpp;
                        u8* drow = (u8*)mp + (u64)y * pitch;
                        if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                drow[x*4+0] = srow[x*4+1];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+3];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        } else if (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            for (u32 x = 0; x < w * 4; x++) {   /* u16 halves, BE */
                                drow[x*2+0] = srow[x*2+1];
                                drow[x*2+1] = srow[x*2+0];
                            }
                        } else {                                 /* u32 floats, BE */
                            u32 nw = (w * bpp) / 4;
                            for (u32 x = 0; x < nw; x++) {
                                drow[x*4+0] = srow[x*4+3];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+1];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        }
                    }
                    r->up->lpVtbl->Unmap(r->up, 0, NULL);
                    D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                    cdst.pResource = r->res;
                    cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    csrc.pResource = r->up;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    csrc.PlacedFootprint.Footprint.Format   = want_fmt;
                    csrc.PlacedFootprint.Footprint.Width    = w;
                    csrc.PlacedFootprint.Footprint.Height   = h;
                    csrc.PlacedFootprint.Footprint.Depth    = 1;
                    csrc.PlacedFootprint.Footprint.RowPitch = pitch;
                    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                }
            }
        }
        {
            D3D12_RESOURCE_BARRIER b = {0};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = r->res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            r->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->res, NULL, rh);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = td.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(RT_SRV_BASE + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r->res, &sv, sh);

    static int _log = 0;
    if (PS3_LOG_FIRST(_log, 8))
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u (render-to-texture)\n",
               slot, off, w, h);
    return slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
}

/* Resistance port: one depth buffer PER colour target. On the RSX the zeta
 * surface keeps its contents when the pass switches to another target and
 * back; the backend used ONE shared depth buffer and cleared it at every
 * target switch. Fall of Man interleaves its scene with shadow passes and the
 * mid-frame flush starts each new batch with a switch, so the scene's depth was
 * wiped mid-frame and the sky/fog pass (drawn where depth is far) painted over
 * every opaque draw before it: York's "ghost town". Each offscreen slot now
 * gets its own persistent depth buffer, cleared only by the guest's own CLEAR
 * (and once when created). RSX_DEPTH_PER_RT=0 restores the shared buffer. */
static ID3D12DescriptorHeap* s_rtdsv_heap;
static UINT s_rtdsv_inc;
static ID3D12Resource* s_rtdepth[MAX_OFF_RTS];
static u32 s_rtdepth_w[MAX_OFF_RTS], s_rtdepth_h[MAX_OFF_RTS];
static D3D12_RESOURCE_STATES s_rtdepth_st[MAX_OFF_RTS];
/* Depth buffer `slot` -> state `to` (DEPTH_WRITE to draw into, DEPTH_READ |
 * PIXEL_SHADER_RESOURCE to sample as a shadow map). */
static void rt_depth_transition(int slot, D3D12_RESOURCE_STATES to)
{
    if (slot < 0 || slot >= MAX_OFF_RTS || !s_rtdepth[slot] || s_rtdepth_st[slot] == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s_rtdepth[slot];
    b.Transition.StateBefore = s_rtdepth_st[slot];
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    s_rtdepth_st[slot] = to;
}
static D3D12_CPU_DESCRIPTOR_HANDLE rt_dsv_handle_peek(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = {0};
    if (!s_rtdsv_heap) return h;
    s_rtdsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_rtdsv_heap, &h);
    h.ptr += (SIZE_T)slot * s_rtdsv_inc;
    return h;
}
/* A 1x1 depth texture holding 1.0 (the far plane): what a shadow map we have
 * no rendered depth for samples as -- fully lit, instead of fully shadowed or
 * a stale colour texture. */
static ID3D12Resource* s_lit_depth;
static int s_lit_depth_ready;
static ID3D12Resource* lit_depth_res(void)
{
    if (s_lit_depth) return s_lit_depth;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC dd = {0};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = 1; dd.Height = 1; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R24G8_TYPELESS; dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
            &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void**)&s_lit_depth)))
        s_lit_depth = NULL;
    return s_lit_depth;
}
/* Called in the draw loop (command list open): clear it to 1.0 once and leave
 * it readable for good. */
static void lit_depth_prepare(void)
{
    if (!s_lit_depth || s_lit_depth_ready) return;
    static ID3D12DescriptorHeap* hp;
    if (!hp) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd,
                &IID_ID3D12DescriptorHeap, (void**)&hp))) { hp = NULL; return; }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    hp->lpVtbl->GetCPUDescriptorHandleForHeapStart(hp, &h);
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, s_lit_depth, &dv, h);
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, h,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s_lit_depth;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    s_lit_depth_ready = 1;
}
static int tex_fmt_all(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_TEXFMT_ALL"); e = v ? atoi(v) : 1; }
    return e;
}
static int depth_tex_enabled(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_SHADOWMAP"); e = v ? atoi(v) : 1; }
    return e;
}
/* Texture units a fragment program samples, cached per program address. */
static u32 fp_used_units(u32 fp_addr)
{
    static int en = -1; if (en < 0) { const char* e = getenv("RSX_SRV_MASK"); en = e ? atoi(e) : 1; }
    if (!en || !fp_addr) return 0xFFFFu;
    enum { N = 4096 };
    static u32 key[N], val[N], sig[N];
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int, u32);
    u32 off = cellGcmResolveLocated((fp_addr & 3u) == 1, fp_addr & ~3u);
    if (!vm_base || off == 0xFFFFFFFFu) return 0xFFFFu;
    /* the program's first 64 bytes: a different program uploaded to the same
     * address must not inherit the old unit mask */
    u32 sg = 0x9E3779B9u;
    { const u32* w = (const u32*)(vm_base + off);
      for (int q = 0; q < 16; q++) sg = (sg ^ w[q]) * 16777619u; }
    u32 h = ((fp_addr >> 4) * 2654435761u) & (N - 1);
    for (u32 k = 0; k < 8; k++) {
        u32 i = (h + k) & (N - 1);
        if (key[i] == fp_addr && sig[i] == sg) return val[i];
        if (!key[i] || key[i] == fp_addr) {
            u32 m = rsx_fp_texture_units(vm_base + off, 4096);
            key[i] = fp_addr; val[i] = m; sig[i] = sg;
            return m;
        }
    }
    return rsx_fp_texture_units(vm_base + off, 4096);
}
static int depth_per_rt(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_DEPTH_PER_RT"); e = v ? atoi(v) : 1; }
    return e;
}
static D3D12_CPU_DESCRIPTOR_HANDLE rt_dsv(int slot, D3D12_CPU_DESCRIPTOR_HANDLE shared)
{
    if (slot < 0 || slot >= MAX_OFF_RTS || !depth_per_rt() || !s_d3d.off_rt[slot].res) return shared;
    if (!s_rtdsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = MAX_OFF_RTS;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd,
                &IID_ID3D12DescriptorHeap, (void**)&s_rtdsv_heap))) { s_rtdsv_heap = NULL; return shared; }
        s_rtdsv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
            s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_rtdsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_rtdsv_heap, &h);
    h.ptr += (SIZE_T)slot * s_rtdsv_inc;
    const u32 w = s_d3d.off_rt[slot].w, ht = s_d3d.off_rt[slot].h;
    if (!s_rtdepth[slot] || s_rtdepth_w[slot] != w || s_rtdepth_h[slot] != ht) {
        if (s_rtdepth[slot]) { grave_defer(s_rtdepth[slot]); s_rtdepth[slot] = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC dd = {0};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = w ? SC(w) : 1; dd.Height = ht ? SC(ht) : 1;
        dd.DepthOrArraySize = 1; dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;   /* DSV D24S8 + SRV R24 (shadow maps) */
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {0};
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
                &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void**)&s_rtdepth[slot]))) {
            s_rtdepth[slot] = NULL; return shared;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, s_rtdepth[slot], &dv, h);
        s_rtdepth_w[slot] = w; s_rtdepth_h[slot] = ht;
        s_rtdepth_st[slot] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, h,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
        { static int n = 0; if (n < 16) { n++;
            fprintf(stderr, "[DEPTH] own depth buffer for RT slot %d (0x%08X) %ux%u%c", slot, s_d3d.off_rt[slot].off, w, ht, 10); } }
    }
    rt_depth_transition(slot, D3D12_RESOURCE_STATE_DEPTH_WRITE);   /* drawing into it again */
    return h;
}

/* Resistance port: depth buffers keyed by the ZETA surface (offset + size)
 * instead of by the colour target they were drawn with. On the RSX a depth
 * surface is its own memory at SET_SURFACE_ZETA_OFFSET; Fall of Man renders
 * each character/object shadow map depth-only into its own zeta surface while
 * the colour offset is whatever the previous pass left behind, then samples the
 * map at the zeta address. Keyed by colour slot, the map lived in the depth of
 * some unrelated colour surface: found only when the leftover colour offset
 * happened to equal the zeta offset, and shared (overwritten) between maps that
 * inherited the same colour offset -- shadows popping in and out with camera
 * and draw order. RSX_ZETA_KEYED=0 restores per-colour-target depth. */
#define MAX_ZETA 256   /* v171: 64 -> 256 (see MAX_OFF_RTS); must stay <= 256 (& 0xFF) */
static struct { u32 off, w, h; ID3D12Resource* res; D3D12_RESOURCE_STATES st; u64 last; } s_zt[MAX_ZETA];
/* v129: the batch (s_submit_no + 1 while recording) that last targeted or
 * sampled each depth surface -- only one this batch never touched may be
 * retargeted in place. */
static u32 s_zt_used[MAX_ZETA];
static void zt_mark_used(int z) { if (z >= 0 && z < MAX_ZETA) s_zt_used[z] = s_submit_no + 1u; }
static u8 s_zt_dirty[MAX_ZETA];   /* zeta z may have changed since its colour copy (zc, below) */
static ID3D12DescriptorHeap* s_zt_heap;
static UINT s_zt_inc;
static u64 s_zt_clock;
static int zeta_keyed(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_ZETA_KEYED"); e = v ? atoi(v) : 1; }
    return e && depth_per_rt();
}
static D3D12_CPU_DESCRIPTOR_HANDLE zt_handle(int z)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = {0};
    if (!s_zt_heap) return h;
    s_zt_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_zt_heap, &h);
    h.ptr += (SIZE_T)z * s_zt_inc;
    return h;
}
static void zt_transition(int z, D3D12_RESOURCE_STATES to)
{
    if (z < 0 || z >= MAX_ZETA || !s_zt[z].res || s_zt[z].st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s_zt[z].res;
    b.Transition.StateBefore = s_zt[z].st;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    s_zt[z].st = to;
}
/* The depth surface a depth TEXTURE at `off` (w x h) reads: same address, a
 * buffer at least that big (a map may be the top-left of a larger surface),
 * the one targeted most recently -- the prepass stamps surfaces in record
 * order, so that is the last one drawn before the sampling draw. An older
 * exact-size buffer at the same address holds a previous frame's map. */
static int zt_find_tex(u32 off, u32 w, u32 h)
{
    int best = -1;
    for (int z = 0; z < MAX_ZETA; z++) {
        if (!s_zt[z].res || s_zt[z].off != off || s_zt[z].w < w || s_zt[z].h < h) continue;
        if (best < 0 || s_zt[z].last > s_zt[best].last) best = z;
    }
    return best;
}
static D3D12_CPU_DESCRIPTOR_HANDLE zeta_dsv(u32 zoff, u32 w, u32 h, D3D12_CPU_DESCRIPTOR_HANDLE shared, int* out_z)
{
    *out_z = -1;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!s_zt_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = MAX_ZETA;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd,
                &IID_ID3D12DescriptorHeap, (void**)&s_zt_heap))) { s_zt_heap = NULL; return shared; }
        s_zt_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
            s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    int z = -1, freez = -1, lru = -1, same = -1;
    const u32 cur_batch = s_submit_no + 1u;
    for (int i = 0; i < MAX_ZETA; i++) {
        if (s_zt[i].res && s_zt[i].off == zoff && s_zt[i].w == w && s_zt[i].h == h) { z = i; break; }
        if (!s_zt[i].res) { if (freez < 0) freez = i; }
        else {
            if (lru < 0 || s_zt[i].last < s_zt[lru].last) lru = i;
            /* untouched by this batch and the one before it (a mid-frame
             * flush splits a frame: its shadow maps are drawn in one batch and
             * sampled in the next) */
            if (s_zt[i].w == w && s_zt[i].h == h && s_zt_used[i] + 1u < cur_batch &&
                (same < 0 || s_zt[i].last < s_zt[same].last)) same = i;
        }
    }
    /* v129: full, and a surface of this size that this batch has not touched
     * exists: retarget it in place (see off_rt_get) instead of creating one.
     * Its old address is gone from the table either way -- the same as LRU
     * eviction -- and the clear below comes before anything this batch draws. */
    if (z < 0 && freez < 0 && same >= 0 && surf_reuse()) {
        z = same;
        s_zt[z].off = zoff;
        zt_transition(z, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, zt_handle(z),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
        s_zt_dirty[z] = 1;                  /* its colour copy (zc) is stale */
        s_zt_reused++;
    }
    if (z < 0) {
        s_zt_created++;
        z = freez >= 0 ? freez : lru;
        if (z < 0) return shared;
        if (s_zt[z].res) { grave_defer(s_zt[z].res); s_zt[z].res = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC dd = {0};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = SC(w); dd.Height = SC(h);
        dd.DepthOrArraySize = 1; dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;   /* DSV D24S8 + SRV R24 (shadow maps) */
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {0};
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
                &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void**)&s_zt[z].res))) {
            s_zt[z].res = NULL; return shared;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, s_zt[z].res, &dv, zt_handle(z));
        s_zt[z].off = zoff; s_zt[z].w = w; s_zt[z].h = h;
        s_zt[z].st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, zt_handle(z),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
        { static FILE* f = NULL; static int n2 = 0;   /* v171 diag */
          if (n2 < 4000) { if (!f) f = fopen("logs\\zetamake.txt", "w");
              if (f) { fprintf(f, "%llu ZETA off=0x%08X %ux%u slot=%d\n", (unsigned long long)GetTickCount64(), zoff, w, h, z);
                       if ((++n2 & 63) == 0) fflush(f); } } }
        { static int n = 0; if (n < 24) { n++;
            fprintf(stderr, "[ZETA] depth surface %d: zeta 0x%08X %ux%u%c", z, zoff, w, h, 10); } }
    }
    s_zt[z].last = ++s_zt_clock;
    s_zt_used[z] = cur_batch;
    zt_transition(z, D3D12_RESOURCE_STATE_DEPTH_WRITE);   /* drawing into it again */
    *out_z = z;
    return zt_handle(z);
}

/* v125: a depth surface sampled as an A8R8G8B8 COLOUR texture. Fall of Man's
 * ground fog (fp 0x01676801) and soft particles (0x01675CC1, 0x01676001)
 * rebuild scene depth from the main zeta surface (0xEC0000, read through
 * format 0xE5 = A8R8G8B8|LN|UN at its 2x-AA size 2560x720, crossbar 0x6AE4):
 * on the RSX that read sees the Z24S8 word as A=d23..16 R=d15..8 G=d7..0
 * B=stencil. Depth here lives only in the D3D zeta buffer -- guest memory at
 * that address is all zeros -- so the fog read depth 0 and drew solid: a flat
 * opaque "river" over dry ground. The depth plane is copied (through a buffer:
 * D3D12 will not copy a depth plane straight into a colour texture) into an
 * RGBA8 texture whose bytes are d7..0, d15..8, d23..16, x, and the SRV's
 * component mapping puts each byte where the guest's crossbar expects it.
 * Re-copied only when the surface was drawn, cleared or rebound since the last
 * copy. RSX_ZETA_AS_COLOR=0 = the old guest-memory upload. */
static struct {
    ID3D12Resource* tex;      /* R8G8B8A8_UNORM, w x h, PIXEL_SHADER_RESOURCE */
    ID3D12Resource* buf;      /* the depth plane's copyable footprint, COPY_DEST */
    ID3D12Resource* src;      /* the zeta resource it was built for */
    u32 w, h;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    int ok;
} s_zc[MAX_ZETA];
static u32 s_zc_conv;             /* copies made (stats) */
static int zc_enabled(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_ZETA_AS_COLOR"); e = v ? atoi(v) : 1; }
    return e && zeta_keyed();
}
/* The zeta surface a colour read of `raw` (declared tw x th) sees: the same
 * address at that size or half of it per axis (the AA surfaces declare their
 * sample grid, our buffers are at pixel size), the one stamped last. */
static int zc_find(u32 raw, u32 tw, u32 th)
{
    int best = -1;
    for (int z = 0; z < MAX_ZETA; z++) {
        if (!s_zt[z].res || s_zt[z].off != raw) continue;
        if (s_zt[z].w != tw && s_zt[z].w * 2u != tw) continue;
        if (s_zt[z].h != th && s_zt[z].h * 2u != th) continue;
        if (best < 0 || s_zt[z].last > s_zt[best].last) best = z;
    }
    return best;
}
static int zc_prepare(int z)
{
    if (z < 0 || z >= MAX_ZETA || !s_zt[z].res) return 0;
    if (s_zc[z].tex && s_zc[z].src == s_zt[z].res &&
        s_zc[z].w == s_zt[z].w && s_zc[z].h == s_zt[z].h) return s_zc[z].ok;
    if (s_zc[z].tex) { grave_defer(s_zc[z].tex); s_zc[z].tex = NULL; }
    if (s_zc[z].buf) { grave_defer(s_zc[z].buf); s_zc[z].buf = NULL; }
    s_zc[z].ok = 0;
    s_zc[z].src = s_zt[z].res; s_zc[z].w = s_zt[z].w; s_zc[z].h = s_zt[z].h;
    s_zt_dirty[z] = 1;
    D3D12_RESOURCE_DESC dd = {0};             /* the zeta buffer's own desc (zeta_dsv) */
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = SC(s_zt[z].w); dd.Height = SC(s_zt[z].h);
    dd.DepthOrArraySize = 1; dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    UINT rows = 0; UINT64 rowsz = 0, total = 0;
    memset(&s_zc[z].fp, 0, sizeof s_zc[z].fp);
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device, &dd, 0, 1, 0,
                                                &s_zc[z].fp, &rows, &rowsz, &total);
    { static int n = 0; if (n < 4) { n++;
        fprintf(stderr, "[ZETATEX] zeta %d %ux%u depth plane footprint fmt=%d rowpitch=%u rowbytes=%llu rows=%u total=%llu%c",
                z, s_zt[z].w, s_zt[z].h, (int)s_zc[z].fp.Footprint.Format,
                s_zc[z].fp.Footprint.RowPitch, (unsigned long long)rowsz, rows,
                (unsigned long long)total, 10); } }
    if (!total || rowsz != (UINT64)SC(s_zt[z].w) * 4u || rows != SC(s_zt[z].h)) return 0;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
            &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_zc[z].buf))) {
        s_zc[z].buf = NULL; return 0;
    }
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = SC(s_zt[z].w); td.Height = SC(s_zt[z].h);
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
            &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&s_zc[z].tex))) {
        s_zc[z].tex = NULL; grave_defer(s_zc[z].buf); s_zc[z].buf = NULL; return 0;
    }
    s_zc[z].ok = 1;
    return 1;
}
/* rsx_remap_to_d3d's selectors index a normal A8R8G8B8 upload, components
 * (R,G,B,A) = guest (R,G,B,A). Here the guest's R is d15..8 (our G), G is
 * d7..0 (our R), B the stencil (read as 1) and A d23..16 (our B). */
static u32 zc_mapping(u32 ctrl1)
{
    static const u32 M[4] = {1u, 0u, 5u, 2u};
    const u32 m = rsx_remap_to_d3d(ctrl1, 0x85u);
    u32 out = 1u << 12;
    for (int d = 0; d < 4; d++) {
        u32 sel = (m >> (3 * d)) & 7u;
        if (sel < 4u) sel = M[sel];
        out |= sel << (3 * d);
    }
    return out;
}
static void zc_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
}
/* Replay, right before a draw that reads zeta z as colour. */
static void zc_convert(int z)
{
    if (z < 0 || z >= MAX_ZETA || !s_zt[z].res || !s_zc[z].ok || s_zc[z].src != s_zt[z].res) return;
    if (!s_zt_dirty[z]) return;
    const D3D12_RESOURCE_STATES prev = s_zt[z].st;
    zt_transition(z, D3D12_RESOURCE_STATE_COPY_SOURCE);
    zc_barrier(s_zc[z].tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = s_zc[z].buf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = s_zc[z].fp;
    src.pResource = s_zt[z].res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;                 /* plane 0: depth */
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    zc_barrier(s_zc[z].buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst2 = {0}, src2 = {0};
    dst2.pResource = s_zc[z].tex; dst2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst2.SubresourceIndex = 0;
    src2.pResource = s_zc[z].buf; src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src2.PlacedFootprint = s_zc[z].fp;
    src2.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst2, 0, 0, 0, &src2, NULL);
    zc_barrier(s_zc[z].buf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    zc_barrier(s_zc[z].tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    zt_transition(z, prev);
    s_zt_dirty[z] = 0;
    s_zc_conv++;
    { static int n = 0; if (n < 4) { n++;
        fprintf(stderr, "[ZETATEX] copied zeta %d (0x%08X %ux%u) for a colour read%c",
                z, s_zt[z].off, s_zt[z].w, s_zt[z].h, 10); } }
}

/* Write a texture SRV (or a null SRV when res == NULL) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t3 descriptor windows. */
/* Resistance port: per-draw SRV staging. The prepass used to create every
 * used unit's SRV for every draw (CreateShaderResourceView -> driver, ~5k
 * draws x 1-4 units a frame in York); consecutive draws mostly bind the same
 * textures. While staging, writes into the current draw's window are only
 * recorded; srv_stage_commit() then either points the draw at the previous
 * draw's window (identical bindings) or creates them. RSX_SRV_REUSE=0 off. */
typedef struct { ID3D12Resource* res; DXGI_FORMAT fmt; UINT mapping; int cube; u32 unit; } SrvStage;
static int s_srv_stage_on; static u32 s_srv_stage_base;
static SrvStage s_srv_stage[RSX_UNITS]; static u32 s_srv_stage_n;
static void srv_create(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping, int cube);
static void srv_write_ex(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt,
                         UINT mapping, int cube)
{
    if (s_srv_stage_on && heap_slot >= s_srv_stage_base && heap_slot < s_srv_stage_base + RSX_UNITS) {
        const u32 u = heap_slot - s_srv_stage_base;
        for (u32 i = 0; i < s_srv_stage_n; i++)
            if (s_srv_stage[i].unit == u) { SrvStage t = { res, fmt, mapping, cube, u }; s_srv_stage[i] = t; return; }
        if (s_srv_stage_n < RSX_UNITS) { SrvStage t = { res, fmt, mapping, cube, u }; s_srv_stage[s_srv_stage_n++] = t; }
        return;
    }
    srv_create(heap_slot, res, fmt, mapping, cube);
}
static void srv_create(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping, int cube)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = (UINT)-1;   /* all the resource has (guest mip chains) */
    if (cube) {   /* must match the TextureCube declaration in the HLSL */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = 1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
}

static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, res, fmt, mapping, 0);
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

/* Set by the present entry point: 1 = this batch ends with a swapchain
 * Present, 0 = execute the recorded draws only. An OFFSCREEN-only batch (a
 * render-to-texture pass) must still run -- it is what fills the texture a
 * later batch samples -- but presenting it would show a half-built frame. */
static __declspec(thread) int s_present_this_frame = 1;   /* per thread: see T_DRAWS */

/* Resolve a DRAW_*_TEX value: a hex raw offset, or the literal "duck" for the
 * content-identified duck texture. */
static u32 draw_filter_tex(const char* e)
{
    if (!e) return 0;
    if (e[0] == 'd' && e[1] == 'u') return s_duck_raw;
    return (u32)strtoul(e, NULL, 16);
}

/* Copy the current backbuffer into s_screen_copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static int s_sc_force = 0;   /* (unused since v106c) */

/* Resistance port: the two guest display buffers, emulated on one backbuffer.
 * s_bb_tag says which buffer (offset+1) the backbuffer currently holds; when
 * draws switch to the other buffer, or the frame ends, or a draw samples the
 * buffer the backbuffer holds, the backbuffer is saved into that buffer's
 * copy. A draw sampling a display buffer binds its copy -- the death screen
 * (scene drawn into B, black-and-white pass samples B into A) and the main
 * menu's TV-signal overlay (samples last frame's buffer). */
static ID3D12Resource* s_disp_copy[2] = {NULL, NULL};
static u32 s_disp_copy_tag[2] = {0, 0};
static u32 s_bb_tag = 0;
static int s_bb_dirty = 0;
static volatile int s_disp_sampled = 0;
static int disp_copy_idx(u32 tag)
{
    for (int i = 0; i < 2; i++) if (s_disp_copy_tag[i] == tag) return i;
    for (int i = 0; i < 2; i++) if (!s_disp_copy_tag[i]) { s_disp_copy_tag[i] = tag; return i; }
    return -1;
}
static ID3D12Resource* disp_copy_res(u32 tag)
{
    int i = disp_copy_idx(tag);
    if (i < 0 || !s_d3d.device) return NULL;
    if (!s_disp_copy[i]) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = SC(s_d3d.width); td.Height = SC(s_d3d.height);
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                &IID_ID3D12Resource, (void**)&s_disp_copy[i])))
            s_disp_copy[i] = NULL;
    }
    return s_disp_copy[i];
}
/* backbuffer (RENDER_TARGET) -> copy of display buffer `tag` */
static void disp_copy_save(u32 fi, u32 tag)
{
    ID3D12Resource* dst = disp_copy_res(tag);
    if (!dst) return;
    D3D12_RESOURCE_BARRIER bb[2] = {0};
    bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[0].Transition.pResource   = s_d3d.render_targets[fi];
    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[1] = bb[0];
    bb[1].Transition.pResource   = dst;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
    s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, dst, s_d3d.render_targets[fi]);
    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
}
/* Frame end (before present): the backbuffer's buffer is final. The next
 * frame's backbuffer is another swapchain image, so nothing is held after. */
static void disp_frame_end(u32 fi)
{
    if (s_disp_sampled && s_bb_tag && s_bb_dirty) disp_copy_save(fi, s_bb_tag);
    s_bb_tag = 0; s_bb_dirty = 0;
}
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
    if ((!en && !s_sc_force) || !s_d3d.device) return;
            if (!s_screen_copy) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td = {0};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = SC(s_d3d.width); td.Height = SC(s_d3d.height);
                td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                        s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                        &IID_ID3D12Resource, (void**)&s_screen_copy)))
                    s_screen_copy = NULL;
            }
            if (s_screen_copy) {
                D3D12_RESOURCE_BARRIER bb[2] = {0};
                bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb[0].Transition.pResource   = s_d3d.render_targets[fi];
                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb[1] = bb[0];
                bb[1].Transition.pResource   = s_screen_copy;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,
                    s_screen_copy, s_d3d.render_targets[fi]);

                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it. */
                { static int sd = -1;
                  if (sd < 0) { const char* e = getenv("SCREENCOPY_DUMP"); sd = e ? atoi(e) : 0; }
                  if (sd && s_d3d.readback_buf) {
                      D3D12_RESOURCE_BARRIER t = {0};
                      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                      t.Transition.pResource   = s_screen_copy;
                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);

                      D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                      cd.pResource = s_d3d.readback_buf;
                      cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      cd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
                      cd.PlacedFootprint.Footprint.Width    = s_d3d.width;
                      cd.PlacedFootprint.Footprint.Height   = s_d3d.height;
                      cd.PlacedFootprint.Footprint.Depth    = 1;
                      cd.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
                      cs.pResource = s_screen_copy;
                      cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                      s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);

                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);
                      s_sc_dump_pending = 1;
                  } }
            }

}

/* rsx_topology -> D3D_PRIMITIVE_TOPOLOGY. Spelled out rather than relying on
 * the two enums happening to agree: the draw record carries the neutral value
 * so a second backend can read it, and this is the one place it becomes a D3D
 * one. */
static D3D12_PRIMITIVE_TOPOLOGY topo_to_d3d(u32 t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RSX_TOPOLOGY_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RSX_TOPOLOGY_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case RSX_TOPOLOGY_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                          return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* v189: an NV3089 copy whose SOURCE is a surface the GPU renders (the scene).
 * cellGcmSys does the copy in guest memory, where that surface was never
 * written, so the destination got stale bytes. Fall of Man copies a window of
 * the screen around each Auger round into scratch memory at 0x1680000+ and
 * refracts it (fp 0x16764C1): black blobs. Recorded in draw order instead; the
 * replay renders the window into a render target at the destination address,
 * which is what a texture at that address then samples. RSX_GPU_BLIT=0 off. */
int rsx_d3d12_blit_from_surface(u32 src_off, u32 in_w, u32 in_h, u32 in_pitch, u32 in_uv,
                                u32 ds_dx, u32 dt_dy, u32 dst_off, u32 dst_pitch,
                                u32 out_x, u32 out_y, u32 out_w, u32 out_h)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_GPU_BLIT"); en = e ? atoi(e) : 0; }   /* v189e: OFF by default -- broke the frame (black/beige screens, flashing, 2 fps) when the Auger fired; see NOTES */
    if (!en || !src_off || !dst_off || out_x || out_y || !out_w || !out_h || out_w > 4096 || out_h > 4096) return 0;
    if (dst_pitch != out_w * 4u) return 0;   /* a whole, tightly packed image: a texture of exactly this size */
    /* v189b: the source offset is the START OF THE COPIED REGION inside the
     * surface (base + y*pitch + x*4), and IMAGE_IN_SIZE is that region's size,
     * not the surface's. v189 matched only regions starting at the surface's
     * first pixel and normalised by the region size, so each strip held the
     * whole screen squashed (the blue bands of the lens/heat effects) and every
     * other copy was skipped. Find the surface containing it; coordinates are
     * in its guest pixels (pitch/4 wide: the 2x-AA scene is 2560 samples wide
     * in a 1280-wide target). */
    int si = -1;
    if (!in_pitch) return 0;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        const OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || r->w * 4u > in_pitch || src_off < r->off || src_off >= r->off + in_pitch * r->h) continue;
        if (si < 0 || r->last > s_d3d.off_rt[si].last) si = i;
    }
    if (si < 0 || T_DRAWCNT >= MAX_DRAWS) return 0;
    const u32 sbase = s_d3d.off_rt[si].off, srt_w = s_d3d.off_rt[si].w, srt_h = s_d3d.off_rt[si].h;
    const u32 d0 = src_off - sbase, ry = d0 / in_pitch, rx = (d0 % in_pitch) / 4u;
    /* guest width of the surface: its width times the horizontal AA factor the
     * pitch implies (1, 2 or 4); any other pitch is padding */
    u32 gw = srt_w;
    { const u32 f = in_pitch / (srt_w * 4u);
      if ((f == 2u || f == 4u) && in_pitch == f * srt_w * 4u) gw = srt_w * f; }
    (void)in_w; (void)in_h;
    D3D12DrawRecord* dr = &T_DRAWS[T_DRAWCNT++];
    memset(dr, 0, sizeof(*dr));
    dr->is_vp = 1; dr->is_clear = 1; dr->is_blit = 1; dr->clear_mask = 0; dr->tex_slot = -1;
    for (int u = 0; u < RSX_UNITS; u++) dr->tex_rt[u] = -1;
    /* v189c: the copy lands in a private copy texture, NOT a render target at
     * dst_off: 0x1680000 is also the bloom source and a shadow/occlusion target,
     * and making the copies render targets there swapped those passes' targets
     * (colours flashing, the gun vanishing when the Auger fired). */
    dr->rt_off = 0; dr->bl_dst = dst_off; dr->rt_w = out_w; dr->rt_h = out_h;
    dr->bl_src = sbase;
    dr->bl_in_w = gw; dr->bl_in_h = srt_h;
    dr->bl_u0 = (float)rx + (float)(in_uv & 0xFFFFu) / 16.0f; dr->bl_v0 = (float)ry + (float)(in_uv >> 16) / 16.0f;
    dr->bl_du = (float)ds_dx / 1048576.0f;         dr->bl_dv = (float)dt_dy / 1048576.0f;
    { static int n = 0; if (n < 8) { n++;
        fprintf(stderr, "[GPUBLIT] 0x%08X in surface 0x%08X (%ux%u guest px) from %.1f,%.1f step %.3f,%.3f -> 0x%08X %ux%u%c",
                src_off, sbase, gw, srt_h, dr->bl_u0, dr->bl_v0, dr->bl_du, dr->bl_dv, dst_off, out_w, out_h, 10); } }
    return 1;
}
static ID3D12RootSignature* s_gb_rs; static ID3D12PipelineState* s_gb_pso; static int s_gb_fail;
static const char s_gb_hlsl[] =
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "cbuffer C : register(b0) { float4 k; };\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return t0.SampleLevel(s0, k.xy + k.zw * i.uv, 0); }\n";
static int gb_init(void)
{
    if (s_gb_pso) return 1;
    if (s_gb_fail) return 0;
    s_gb_fail = 1;
    ID3D12Device* d = s_d3d.device;
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1; rg.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &rg;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 4;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sm = {0};
    sm.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.MaxLOD = D3D12_FLOAT32_MAX; sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 2; rd.pParameters = rp; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &sm;
    ID3DBlob *sig = NULL, *err = NULL, *vsb = NULL, *psb = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { if (err) err->lpVtbl->Release(err); return 0; }
    HRESULT hr = d->lpVtbl->CreateRootSignature(d, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                                &IID_ID3D12RootSignature, (void**)&s_gb_rs);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) return 0;
    if (FAILED(D3DCompile(s_gb_hlsl, sizeof s_gb_hlsl - 1, "gpu_blit", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, &err))) { if (err) err->lpVtbl->Release(err); return 0; }
    if (FAILED(D3DCompile(s_gb_hlsl, sizeof s_gb_hlsl - 1, "gpu_blit", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, &err))) { if (err) err->lpVtbl->Release(err); vsb->lpVtbl->Release(vsb); return 0; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_gb_rs;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb); pd.VS.BytecodeLength = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb); pd.PS.BytecodeLength = psb->lpVtbl->GetBufferSize(psb);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = d->lpVtbl->CreateGraphicsPipelineState(d, &pd, &IID_ID3D12PipelineState, (void**)&s_gb_pso);
    vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb);
    if (FAILED(hr)) { s_gb_pso = NULL; fprintf(stderr, "[GPUBLIT] pipeline failed%c", 10); return 0; }
    s_gb_fail = 0;
    return 1;
}
static int off_rt_find_col(u32 off);   /* fwd */
/* v189c: copy textures -- (offset, w, h) -> an R8G8B8A8 texture the GPU copy
 * renders into; a texture of exactly that address and size samples it. */
#define GBC_N 64
typedef struct { u32 off, w, h; u64 last; ID3D12Resource* res; D3D12_RESOURCE_STATES st; } GbcTex;
static GbcTex s_gbc[GBC_N]; static ID3D12DescriptorHeap* s_gbc_rtv; static u64 s_gbc_clock;
static int gbc_find(u32 off, u32 w, u32 h)
{
    for (int i = 0; i < GBC_N; i++) if (s_gbc[i].res && s_gbc[i].off == off && s_gbc[i].w == w && s_gbc[i].h == h) return i;
    return -1;
}
static int gbc_get(u32 off, u32 w, u32 h)
{
    int i = gbc_find(off, w, h);
    if (i >= 0) { s_gbc[i].last = ++s_gbc_clock; return i; }
    if (!s_gbc_rtv) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.NumDescriptors = GBC_N; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_gbc_rtv))) return -1;
    }
    i = 0;
    for (int k = 0; k < GBC_N; k++) { if (!s_gbc[k].res) { i = k; break; } if (s_gbc[k].last < s_gbc[i].last) i = k; }
    if (s_gbc[i].res) { grave_defer(s_gbc[i].res); s_gbc[i].res = NULL; }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = SC(w); td.Height = SC(h);
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&s_gbc[i].res))) { s_gbc[i].res = NULL; return -1; }
    s_gbc[i].off = off; s_gbc[i].w = w; s_gbc[i].h = h; s_gbc[i].last = ++s_gbc_clock;
    s_gbc[i].st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_CPU_DESCRIPTOR_HANDLE rh; s_gbc_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_gbc_rtv, &rh);
    rh.ptr += (u64)i * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_gbc[i].res, NULL, rh);
    return i;
}
static void gbc_transition(int i, D3D12_RESOURCE_STATES to)
{
    if (s_gbc[i].st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = s_gbc[i].res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = s_gbc[i].st; b.Transition.StateAfter = to;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    s_gbc[i].st = to;
}
/* Replay side: render dr's window of its source surface into its copy texture. */
static void gpu_blit_replay(const D3D12DrawRecord* dr)
{
    if (!gb_init()) return;
    const int gi = gbc_find(dr->bl_dst, dr->rt_w, dr->rt_h);
    if (gi < 0) return;
    int src = off_rt_find_col(dr->bl_src);
    if (src < 0) src = off_rt_find(dr->bl_src);
    if (src < 0 || !s_d3d.off_rt[src].res) return;
    const float iw = (float)(dr->bl_in_w ? dr->bl_in_w : s_d3d.off_rt[src].w);
    const float ih = (float)(dr->bl_in_h ? dr->bl_in_h : s_d3d.off_rt[src].h);
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    off_rt_transition(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gbc_transition(gi, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh; s_gbc_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_gbc_rtv, &rh);
    rh.ptr += (u64)gi * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rh, FALSE, NULL);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_gb_rs);
    cl->lpVtbl->SetPipelineState(cl, s_gb_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE gh;
    s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
    gh.ptr += (u64)(RT_SRV_BASE + src) * s_d3d.srv_inc;
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, gh);
    const float k[4] = { dr->bl_u0 / iw, dr->bl_v0 / ih,
                         (float)dr->rt_w * dr->bl_du / iw, (float)dr->rt_h * dr->bl_dv / ih };
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 4, k, 0);
    D3D12_VIEWPORT vp = {0, 0, (float)SC(dr->rt_w), (float)SC(dr->rt_h), 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)SC(dr->rt_w), (LONG)SC(dr->rt_h)};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    gbc_transition(gi, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.vp_root_sig);
    cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, 0, s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb));
    if (s_gf_buf)
        cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 3, s_gf_buf->lpVtbl->GetGPUVirtualAddress(s_gf_buf) + (u64)T_PARITY * GF_CAP);
    { D3D12_GPU_DESCRIPTOR_HANDLE g0; s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &g0);
      cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, g0); }
}

/* GCM_GUEST_FB=<off>,<w>,<h>,<pitch> -- present the guest display buffer.
 *
 * A title that composites its final image with the RSX 2D engine assembles it
 * in GUEST memory: nv3089_blit is a CPU copy and never touches D3D12, and the
 * draw pass below only runs when a recorded draw targets the display surface,
 * which for such a title never happens. The frame is finished, in the wrong
 * memory, and the swapchain shows only the clear. Copy it across.
 *
 * Off unless the offset and geometry are given, so nothing else changes. */
/* GPU-side composite of an offscreen RT onto the backbuffer.
 *
 * A title that finishes its frame with the RSX 2D engine blits a rendered
 * surface into the display buffer. That surface lives in guest RAM, which the
 * D3D12 path never writes -- the scene is on the GPU -- so the CPU copy in
 * nv3089_blit faithfully moves zeros. The backend already holds the same
 * surface as a texture, so do the composite here instead. Triggered from
 * cellGcmSys under GCM_COMPOSITE_RT. */
static u32 s_composite_src = 0;
static int s_last_drawn_slot = -1;
static u32 s_last_drawn_off  = 0;

void rsx_d3d12_composite_from_offscreen(u32 raw_off) { s_composite_src = raw_off; }

static void composite_present(u32 fi)
{
    u32 want = s_composite_src;
    s_composite_src = 0;
    if (!want) return;
    /* want == 2 means "whatever this frame's draws targeted" -- the one thing
     * guaranteed to hold geometry, and immune to the offset collision above. */
    int i;
    if (want == 2u) { i = s_last_drawn_slot; want = s_last_drawn_off; }
    else            { i = off_rt_find(want); }
    if (i < 0 || !s_d3d.off_rt[i].res) return;
    OffRT* r = &s_d3d.off_rt[i];
    /* Same-format copy only; a half-float source would need a converting draw. */
    if (r->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static int w1 = 0;
        if (PS3_LOG_FIRST(w1, 2))
            fprintf(stderr, "[COMPOSITE] RT 0x%08X is dxgi=%u, not R8G8B8A8 -- needs a draw%c",
                    want, r->dxgi, 10);
        return;
    }
    u32 cw = r->w < s_d3d.width  ? r->w : s_d3d.width;
    u32 ch = r->h < s_d3d.height ? r->h : s_d3d.height;
    if (!cw || !ch) return;
    { static int n = 0;
      if (PS3_LOG_FIRST(n, 3)) fprintf(stderr, "[COMPOSITE] RT 0x%08X %ux%u -> backbuffer%c",
                           want, cw, ch, 10); }

    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource   = r->res;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = r->st;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource   = s_d3d.render_targets[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = r->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
    D3D12_BOX box = {0};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = SC(cw); box.bottom = SC(ch); box.back = 1;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, &box);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = r->st;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    static ID3D12Resource* up = NULL;
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width  = (u64)up_pitch * gh; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&up)))
            up = NULL;
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
    if (!up) return;
    { extern u8* vm_base;
      if (!vm_base) return;
      /* RSX local memory is mapped at guest 0xC0000000. */
      const u8* srow = vm_base + 0xC0000000u + off;
      void* m = NULL; D3D12_RANGE nr = {0, 0};
      if (FAILED(up->lpVtbl->Map(up, 0, &nr, &m)) || !m) return;
      for (u32 y = 0; y < gh; y++) {
          const u8* s = srow + (size_t)y * gpitch;
          u8* dp = (u8*)m + (size_t)y * up_pitch;
          for (u32 x = 0; x < gw; x++) {   /* guest B,G,R,A -> backbuffer R,G,B,A */
              dp[x*4+0] = s[x*4+2]; dp[x*4+1] = s[x*4+1];
              dp[x*4+2] = s[x*4+0]; dp[x*4+3] = 0xFF;
          }
      }
      D3D12_RANGE wr = {0, (SIZE_T)up_pitch * gh};
      up->lpVtbl->Unmap(up, 0, &wr); }

    D3D12_RESOURCE_BARRIER gb = {0};
    gb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gb.Transition.pResource   = s_d3d.render_targets[fi];
    gb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = 0;
    sl.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width    = gw;
    sl.PlacedFootprint.Footprint.Height   = gh;
    sl.PlacedFootprint.Footprint.Depth    = 1;
    sl.PlacedFootprint.Footprint.RowPitch = up_pitch;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, NULL);

    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);
}


/* Resistance port: drop the whole guest-FP pipeline table. Only called right
 * after wait_for_gpu, when no recorded command list references any PSO. */
/* Resistance port: live draw filter for bisecting broken draws while the game
 * runs. logs\\fpkill.txt holds hex fragment-program addresses (whitespace
 * separated); draws using them are skipped. "invert" as the first word keeps
 * ONLY the listed programs. Re-read once a second; delete the file to clear. */
static u32 s_fpkill[256]; static int s_fpkill_n = 0, s_fpkill_inv = 0;
static u32 s_fpnb[256], s_fpnd[256]; static int s_fpnb_n = 0, s_fpnd_n = 0;
/* v163: s_fpnat ("noat" = alpha test off for these FPs) is declared above render_frame */
static int fp_in(const u32* l, int n, u32 fp) { for (int i = 0; i < n; i++) if (l[i] == fp || l[i] == 0xFFFFFFFFu) return 1; return 0; }
static void fpkill_reload(void)
{
    static ULONGLONG last = 0; ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    s_fpkill_n = 0; s_fpkill_inv = 0; s_fpnb_n = 0; s_fpnd_n = 0; s_fpnat_n = 0;
    FILE* f = fopen("logs\\fpkill.txt", "r");
    if (!f) return;
    char tok[64];
    /* Tokens after "noblend" / "nodepth" go to those lists instead
     * (blending forced off / depth test forced off for those FPs); "kill"
     * switches back. Lets one capture tell transparency from occlusion. */
    int mode = 0;
    while (s_fpkill_n < 256 && fscanf(f, "%63s", tok) == 1) {
        if (!strcmp(tok, "invert")) { s_fpkill_inv = 1; continue; }
        if (!strcmp(tok, "kill")) { mode = 0; continue; }
        if (!strcmp(tok, "noblend")) { mode = 1; continue; }
        if (!strcmp(tok, "nodepth")) { mode = 2; continue; }
        if (!strcmp(tok, "noat")) { mode = 3; continue; }
        u32 v = (u32)strtoul(tok, NULL, 16);
        if (mode == 1) { if (s_fpnb_n < 256) s_fpnb[s_fpnb_n++] = v; }
        else if (mode == 2) { if (s_fpnd_n < 256) s_fpnd[s_fpnd_n++] = v; }
        else if (mode == 3) { if (s_fpnat_n < 256) s_fpnat[s_fpnat_n++] = v; }
        else s_fpkill[s_fpkill_n++] = v;
    }
    fclose(f);
}
static int fpkill_match(u32 fp)
{
    if (!s_fpkill_n && !s_fpkill_inv) return 0;
    int hit = 0;
    for (int i = 0; i < s_fpkill_n; i++) if (s_fpkill[i] == fp) { hit = 1; break; }
    return s_fpkill_inv ? !hit : hit;
}

static void fp_cache_flush(void)
{
    for (int i = 0; i < s_d3d.vp_fp_n; i++)
        if (s_d3d.vp_fp[i].pso) s_d3d.vp_fp[i].pso->lpVtbl->Release(s_d3d.vp_fp[i].pso);
    for (int i = 0; i < s_fp_grave_n; i++) s_fp_grave[i]->lpVtbl->Release(s_fp_grave[i]);
    s_fp_grave_n = 0; s_d3d.vp_fp_n = 0;
    memset(s_fp_head, 0, sizeof s_fp_head);
    s_fp_flush_pending = 0;
    fprintf(stderr, "[FP] pipeline cache full -- flushed%c", 10);
}

/* Resistance port (v132): frame-time readout. RivaTuner's overlay no longer
 * shows in this port, so the present-to-present interval is measured here:
 *   - title bar, every second: FPS, average frame time, the slowest 1% of
 *     frames ("1% low") and the single worst frame;
 *   - run.log, every 5 s: "[present]" with the same plus counts of frames
 *     over 20 / 33 / 50 ms;
 *   - logs/frametimes.csv: one line per present (seconds since start,
 *     frame ms) for graphing. RSX_FRAMETIME_CSV=0 turns the file off.
 * Also, ~10 s in, "[overlay]" lists the non-Windows DLLs in the process --
 * whether RivaTuner's hook DLL (RTSSHooks64.dll) got injected at all. */
static char s_ft_title[128];
static float s_ft_1s[4096]; static unsigned s_ft_1n;
static float s_ft_5s[16384]; static unsigned s_ft_5n;
static int ft_cmp(const void* a, const void* b)
{ const float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
/* avg, 99th-percentile frame time (the "1% low") and max of v[0..n) */
static void ft_stats(const float* v, unsigned n, double* avg, double* p99, double* mx)
{
    static float tmp[16384];
    double sum = 0; float m = 0;
    if (n > 16384) n = 16384;
    for (unsigned i = 0; i < n; i++) { tmp[i] = v[i]; sum += v[i]; if (v[i] > m) m = v[i]; }
    qsort(tmp, n, sizeof(float), ft_cmp);
    unsigned k = n ? (unsigned)((n - 1) * 0.99 + 0.5) : 0;
    *avg = n ? sum / n : 0; *p99 = n ? tmp[k] : 0; *mx = m;
}
static void ft_overlay_check(void)
{
    typedef BOOL (WINAPI *enum_t)(HANDLE, HMODULE*, DWORD, LPDWORD);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    enum_t en = k32 ? (enum_t)(void*)GetProcAddress(k32, "K32EnumProcessModules") : NULL;
    fprintf(stderr, "[overlay] RivaTuner hook DLL (RTSSHooks64.dll): %s%c",
            GetModuleHandleA("RTSSHooks64.dll") ? "LOADED" : "not loaded", 10);
    if (!en) return;
    static HMODULE mods[1024]; DWORD need = 0;
    if (!en(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    unsigned n = need / sizeof(HMODULE); if (n > 1024) n = 1024;
    for (unsigned i = 0; i < n; i++) {
        char path[MAX_PATH]; path[0] = 0;
        GetModuleFileNameA(mods[i], path, sizeof path);
        char low[MAX_PATH]; size_t j = 0;
        for (; path[j] && j < sizeof low - 1; j++) low[j] = (char)((path[j] >= 'A' && path[j] <= 'Z') ? path[j] + 32 : path[j]);
        low[j] = 0;
        if (strstr(low, "\\windows\\")) continue;
        fprintf(stderr, "[overlay]   module %s%c", path, 10);
    }
    fflush(stderr);
}
static void ft_note_present(void)
{
    static LARGE_INTEGER fq; static long long t_start = 0, prev = 0;
    static ULONGLONG t1 = 0, t5 = 0; static unsigned n_pres = 0;
    static FILE* csv = NULL; static int csv_on = -1;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!fq.QuadPart) { QueryPerformanceFrequency(&fq); t_start = now.QuadPart; }
    if (++n_pres == 600) ft_overlay_check();
    if (prev) {
        const float d = (float)((double)(now.QuadPart - prev) * 1000.0 / (double)fq.QuadPart);
        if (s_ft_1n < 4096) s_ft_1s[s_ft_1n++] = d;
        if (s_ft_5n < 16384) s_ft_5s[s_ft_5n++] = d;
        if (csv_on < 0) {
            const char* e = getenv("RSX_FRAMETIME_CSV"); csv_on = e ? atoi(e) : 1;
            if (csv_on) {
                csv = fopen("logs/frametimes.csv", "w");
                if (csv) fprintf(csv, "seconds,frame_ms\n");
            }
        }
        if (csv) {
            fprintf(csv, "%.3f,%.2f\n", (double)(now.QuadPart - t_start) / (double)fq.QuadPart, d);
            if ((n_pres & 63) == 0) fflush(csv);
        }
    }
    prev = now.QuadPart;
    const ULONGLONG tk = GetTickCount64();
    if (!t1) t1 = t5 = tk;
    if (tk - t1 >= 1000 && s_ft_1n) {
        double a, p, m; ft_stats(s_ft_1s, s_ft_1n, &a, &p, &m);
        snprintf(s_ft_title, sizeof s_ft_title, " | frame %.1f ms, 1%% low %.1f ms, worst %.1f ms", a, p, m);
        s_ft_1n = 0; t1 = tk;
    }
    if (tk - t5 >= 5000 && s_ft_5n) {
        double a, p, m; ft_stats(s_ft_5s, s_ft_5n, &a, &p, &m);
        unsigned o20 = 0, o33 = 0, o50 = 0;
        for (unsigned i = 0; i < s_ft_5n; i++) {
            if (s_ft_5s[i] > 20.0f) o20++;
            if (s_ft_5s[i] > 33.4f) o33++;
            if (s_ft_5s[i] > 50.0f) o50++;
        }
        fprintf(stderr, "[present] 5 s: %u frames, %.1f fps | frame avg %.2f ms, 1%% low %.2f ms (%.0f fps), worst %.2f ms | over 20 ms: %u, over 33 ms: %u, over 50 ms: %u%c",
                s_ft_5n, s_ft_5n * 1000.0 / (double)(tk - t5), a, p, p > 0 ? 1000.0 / p : 0.0, m, o20, o33, o50, 10);
        s_ft_5n = 0; t5 = tk;
    }
}

static void vp_async_drain(void);
/* Mid-frame flush bookkeeping (see d3d12_maybe_midframe_flush). */
static __declspec(thread) int s_in_midflush = 0, s_batch_cont = 0;
static __declspec(thread) int t_render_thread = 0;   /* 1 on the RSX_ASYNC render thread */
#include "rsx_fsr.inc"   /* v165: AMD FSR 3.1 */
static void render_frame_impl(void)
{
    double _rf0 = perf_on() ? perf_now() : 0.0;
    u32 fi = s_d3d.frame_index;
    if (!t_render_thread) vp_async_drain();   /* queued vertex conversions land before anything reads them */

    /* Drain the GPU before touching shared upload resources (vp_vb vertices,
     * vp_cb constants, per-frame texture staging): the previous frame's draws
     * may still be reading them, and overwriting mid-flight tears geometry
     * (gcm/cube: triangles mixing stale and new vertices -> missing/sliver
     * polygons). These workloads are a few draws/frame, so full serialisation
     * costs little. Resistance port: with RSX_PIPELINE the previous batch's end
     * already waited for the one whose half this batch reuses. */
    if (!rsx_pipeline()) wait_for_gpu();
    if (s_fp_flush_pending) fp_cache_flush();
    fpkill_reload();
    /* Resistance port: keep the window's message queue serviced during a long
     * FIFO drain (one York frame is several mid-frame batches); main.cpp only
     * pumps between drains, so Windows flagged the window "Not Responding".
     * Only on the thread that owns the window. WM_CLOSE just sets a flag. */
    { static ULONGLONG last = 0; ULONGLONG now = GetTickCount64();
      if (s_d3d.hwnd && now - last > 50 &&
          GetWindowThreadProcessId(s_d3d.hwnd, NULL) == GetCurrentThreadId()) {
          last = now; MSG msg;
          while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
              if (msg.message == WM_QUIT) { s_d3d.window_closed = 1; break; }
              TranslateMessage(&msg); DispatchMessageA(&msg);
          }
      } }

    /* Compile the real vertex program once its microcode is captured, and keep
     * the constant bank uploaded for the VS. */
    /* The base VP pipeline gates the ENTIRE vertex-program draw pass below
     * (`if (s_d3d.vp_ready ...)`). The old trigger also required the microcode
     * size to differ from the last compile, so a title whose first captured
     * program has the same byte count as vp_compiled_bytes never built the base
     * pipeline -- and then every is_vp draw record was silently dropped: draws
     * recorded, constants uploaded, per-draw VS compiled and cached, and not one
     * of them submitted. While !vp_ready there is nothing to be stale against,
     * so only the "have microcode" test belongs here. */
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16)
        compile_vp();
    /* Per-draw VP constants are snapshotted at record time (vp_record_cb). */

    /* Lazily create the readback buffer the first time a dump is requested. */
    if (s_d3d.dump_frames_left > 0 && !s_d3d.readback_buf) {
        s_d3d.readback_pitch = (s_d3d.width * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * s_d3d.height;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list. Resistance port: the allocator is
     * picked by batch parity, not by swap-chain buffer -- mid-frame batches
     * never present, so the buffer index stays put while batches alternate. */
    { const u32 ai = rsx_pipeline() ? (u32)T_PARITY : fi;
      s_d3d.cmd_allocators[ai]->lpVtbl->Reset(s_d3d.cmd_allocators[ai]);
      s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[ai], NULL); }

    /* Upload the bound font atlas into an R8_UNORM texture and create its SRV.
     * The atlas is a linear 8-bit coverage map, so a straight row copy (no
     * deswizzle) suffices. It is a DYNAMIC glyph cache -- the game rasterizes
     * new glyphs into it over time -- so re-upload whenever it is dirty (set on
     * every bind), not just once. Uploading only on first bind left every glyph
     * added after frame 1 sampling stale texels -> torn text. The backend is
     * synchronous (wait_for_gpu each frame), so recreating the resource per
     * dirty frame is safe. */
    if (s_d3d.tex_bound && (!s_d3d.tex_ready || s_d3d.tex_dirty) && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
        extern uint8_t* vm_base;
        u32 w = s_d3d.tex_w, h = s_d3d.tex_h;
        u32 pitch = (w + 255) & ~255u;   /* D3D12 requires 256-byte row pitch */

        if (s_d3d.tex_resource) { s_d3d.tex_resource->lpVtbl->Release(s_d3d.tex_resource); s_d3d.tex_resource = NULL; }
        if (s_d3d.tex_upload)   { s_d3d.tex_upload->lpVtbl->Release(s_d3d.tex_upload);     s_d3d.tex_upload = NULL; }

        D3D12_HEAP_PROPERTIES hp_def = {0}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HRESULT thr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp_def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.tex_resource);

        D3D12_HEAP_PROPERTIES hp_up = {0}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {0};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = (u64)pitch * h; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(thr))
            thr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp_up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_upload);

        if (SUCCEEDED(thr) && vm_base) {
            void* mapped = NULL;
            D3D12_RANGE nr = {0, 0};
            if (SUCCEEDED(s_d3d.tex_upload->lpVtbl->Map(s_d3d.tex_upload, 0, &nr, &mapped)) && mapped) {
                const u8* srcbase = vm_base + s_d3d.tex_src_offset;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)mapped + (u64)y * pitch, srcbase + (u64)y * w, w);
                s_d3d.tex_upload->lpVtbl->Unmap(s_d3d.tex_upload, 0, NULL);

                D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
                dst.pResource = s_d3d.tex_resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = s_d3d.tex_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width    = w;
                src.PlacedFootprint.Footprint.Height   = h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = s_d3d.tex_resource;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &tb);

                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
                s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.tex_resource, &sv, sh);

                s_d3d.tex_ready = 1;
                s_d3d.tex_dirty = 0;   /* content now in sync with guest atlas */
                { static int _au = 0; if (PS3_LOG_FIRST(_au, 3))
                    printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h); }
            }
        }
    }

    /* Frames with no ops (init/boot presents) don't count against the cap --
     * PNG-decode-heavy titles burn hundreds of empty presents before the first
     * real draw. */
    if (getenv("RTT_DUMP") && T_DRAWCNT > 0) { static int _f=0; int _cap = atoi(getenv("RTT_DUMP")); if (_cap < 2) _cap = 14; if (PS3_LOG_FIRST(_f, _cap)) {
        fprintf(stderr, "[RTT] frame %d: %u ops\n", _f, T_DRAWCNT);
        for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &T_DRAWS[_d];
            fprintf(stderr, "[RTT]  op%02u %s rt=0x%X mrt=0x%X/0x%X/0x%X t0=0x%X fp=0x%X n=%u cmask=%X vp=%u,%u %ux%u blend=%d\n",
                _d, r->is_clear?"CLR ":(r->is_vp?"draw":"leg "), r->rt_off,
                r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                r->tex[0].raw, r->fp_addr, r->vertex_count,
                r->cmask, r->vp_x, r->vp_y, r->vp_w, r->vp_h, r->blend);
        }
    } }

    /* Debug: RTT_PASS=N shows pass N's output directly on screen (drops later
     * ops, retargets pass N to the backbuffer). */
    { const char* rp = getenv("RTT_PASS");
      if (rp) {
        int keep = atoi(rp), seen = 0; u32 cut = T_DRAWCNT;
        int viewrt = getenv("RTT_VIEWRT") != NULL;
        for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &T_DRAWS[_d];
            if (r->is_clear || !r->is_vp) continue;
            if (seen == keep) {
                /* Alone: retarget op N to the backbuffer. With RTT_VIEWRT:
                 * keep op N intact and pull the display composite forward so
                 * the chosen RT is shown as of this point in the chain. */
                if (!viewrt) { r->rt_off = 0; r->rt_mrt[0] = r->rt_mrt[1] = r->rt_mrt[2] = 0; }
                cut = _d + 1;
                break;
            }
            seen++;
        }
        if (viewrt) {
            for (u32 _j = cut; _j < T_DRAWCNT && _j < MAX_DRAWS; _j++) {
                D3D12DrawRecord* r = &T_DRAWS[_j];
                if (r->is_vp && !r->is_clear && r->rt_off == 0) {
                    if (cut < MAX_DRAWS) T_DRAWS[cut++] = *r;
                    break;
                }
            }
        }
        T_DRAWCNT = (cut < T_DRAWCNT) ? cut : T_DRAWCNT;
      } }

    /* DRAW_LIMIT=<N>: keep only the first N recorded ops this frame. The duck's
     * draws are ops 00..~03 of every frame, so a small limit shows the ducks
     * alone -- separating "the ducks never rasterize" from "later geometry is
     * painted over them". Diagnostic only. */
    { static int lim = -1;
      if (lim < 0) { const char* e = getenv("DRAW_LIMIT"); lim = e ? atoi(e) : 0; }
      if (lim > 0 && T_DRAWCNT > (u32)lim) T_DRAWCNT = (u32)lim; }
    /* Resistance port: DRAW_LIMIT_STEP=<frames> walks the draw limit 1..26
     * every <frames> frames, so an RSX_CAP series with the same stride
     * captures the frame built up one op at a time. */
    { static int step = -1;
      if (step < 0) { const char* e = getenv("DRAW_LIMIT_STEP"); step = e ? atoi(e) : 0; }
      if (step > 0) {
          u32 lim = 1u + (u32)((s_d3d.frame_count / (u64)step) % 26u);
          if (T_DRAWCNT > lim) T_DRAWCNT = lim; } }

    /* DRAW_SKIP_TEX=<hex raw offset>: drop every op binding that texture on
     * unit 0. The ducks sit behind the water's slab geometry, so removing the
     * occluder is the way to see whether the duck itself renders. Diagnostic. */
    { static const char* sk = (const char*)1; static u32 want = 0;
      if (sk == (const char*)1) sk = getenv("DRAW_SKIP_TEX");
      want = draw_filter_tex(sk);
      if (want) {
        u32 keep = 0;
        for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++) {
            if (!T_DRAWS[_d].is_clear && T_DRAWS[_d].tex[0].raw == want) continue;
            if (keep != _d) T_DRAWS[keep] = T_DRAWS[_d];
            keep++;
        }
        T_DRAWCNT = keep;
      } }

    /* DRAW_KEEP_TEX=<hex raw offset>: the inverse -- keep only the ops binding
     * that texture (plus clears). Isolates one object from everything drawn over
     * it, which is what finally shows the duck on its own. Diagnostic. */
    { static const char* kp = (const char*)1; static u32 want = 0;
      if (kp == (const char*)1) kp = getenv("DRAW_KEEP_TEX");
      want = draw_filter_tex(kp);
      /* DRAW_KEEP_NOCLEAR=1: also drop the clears. Keeping them preserves their
       * ORIGINAL position in the batch, so a clear that originally ran after the
       * kept draws still runs after them -- and wipes the very geometry being
       * isolated. That makes an object that renders fine look like it renders
       * nothing. */
      static int noclr = -1;
      if (noclr < 0) { const char* e = getenv("DRAW_KEEP_NOCLEAR"); noclr = e ? atoi(e) : 0; }
      /* DRAW_KEEP_BLEND=0|1: further restrict DRAW_KEEP_TEX to draws with that
       * blend flag. A mesh drawn twice -- once opaque, once blended -- is two
       * different passes (body vs reflection shell) that a texture filter alone
       * cannot separate. */
      static int kblend = -2;
      if (kblend == -2) { const char* e = getenv("DRAW_KEEP_BLEND");
                          kblend = e ? atoi(e) : -1; }
      if (want) {
        /* Emit the CLEARS FIRST, then the kept draws. Preserving the original
         * order means a clear that ran after them still runs after them and
         * wipes the isolated geometry; dropping the clears entirely (noclr)
         * leaves stale DEPTH that rejects most of its fragments. Neither shows
         * the object properly -- clear, then draw, does. */
        static D3D12DrawRecord kept[MAX_DRAWS];
        u32 nk = 0;
        if (!noclr)
            for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++)
                if (T_DRAWS[_d].is_clear && nk < MAX_DRAWS) kept[nk++] = T_DRAWS[_d];
        for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++)
            if (!T_DRAWS[_d].is_clear && T_DRAWS[_d].tex[0].raw == want
                && (kblend < 0 || T_DRAWS[_d].blend == kblend)
                && nk < MAX_DRAWS)
                kept[nk++] = T_DRAWS[_d];
        for (u32 _k = 0; _k < nk; _k++) T_DRAWS[_k] = kept[_k];
        T_DRAWCNT = nk;
      } }

    /* DRAW_LAST_TEX=<hex raw offset>: move the ops binding that texture to the
     * END of the batch so nothing is drawn over them. The duck's draws are ops
     * 00..03 of every frame and the tub wall is drawn afterwards across the same
     * screen area, so anything that defeats the depth sort hides the duck
     * completely. Reordering separates "occluded" from "not rendered". */
    { static const char* lt = (const char*)1; static u32 want = 0;
      if (lt == (const char*)1) lt = getenv("DRAW_LAST_TEX");
      want = draw_filter_tex(lt);
      if (want && T_DRAWCNT > 1) {
        static D3D12DrawRecord moved[MAX_DRAWS];
        u32 nm = 0, keep = 0;
        for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++) {
            if (!T_DRAWS[_d].is_clear && T_DRAWS[_d].tex[0].raw == want) {
                if (nm < MAX_DRAWS) moved[nm++] = T_DRAWS[_d];
                continue;
            }
            if (keep != _d) T_DRAWS[keep] = T_DRAWS[_d];
            keep++;
        }
        for (u32 _m = 0; _m < nm && keep < MAX_DRAWS; _m++)
            T_DRAWS[keep++] = moved[_m];
        T_DRAWCNT = keep;
      } }

    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists for
     * every non-display surface targeted this frame (so draws binding it as a
     * texture can resolve to it below, whatever the op order). */
    for (int _i = 0; _i < MAX_OFF_RTS; _i++) s_d3d.off_rt[_i].used = 0;
    rt_bloom_rebuild();   /* v189 */
    for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &T_DRAWS[_d];
        dr->rt_slot = (dr->is_vp && dr->rt_off)
                    ? off_rt_get(dr->rt_off, dr->rt_w, dr->rt_h, dr->rt_fmt) : -1;
        if (dr->is_blit) (void)gbc_get(dr->bl_dst, dr->rt_w, dr->rt_h);   /* v189c */
        for (int _m = 0; _m < 3; _m++)
            dr->rt_mrt_slot[_m] = (dr->is_vp && dr->rt_mrt[_m])
                                ? off_rt_get(dr->rt_mrt[_m], dr->rt_w, dr->rt_h, dr->rt_fmt) : -1;
    }

    /* Per-frame VP textures + guest-FP pipelines: for each VP draw, upload the
     * texture it had bound at submit time into a slot (SRV heap 1+slot; plasma
     * animates so contents re-upload every frame) and pre-build its FP PSO.
     * A texture whose offset matches an offscreen RT samples the RT directly
     * (tex_slot 1000+idx) -- no guest-memory upload. */
    double _pre0 = perf_on() ? perf_now() : 0.0;
    for (int _i = 0; _i < VP_TEX_SLOTS; _i++) s_d3d.vp_tex[_i].used = 0;
    for (u32 _d = 0; _d < T_DRAWCNT && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &T_DRAWS[_d];
        dr->srv_win = _d;
        dr->samp_disp = 0;
        /* Resistance port: give this op's zeta surface its depth buffer now,
         * in record order, so a later draw in this batch that samples it as a
         * shadow map finds THIS frame's surface. Created only at replay, a
         * map whose size changed this frame (the game resizes them as the
         * camera moves) was missed or read from an older same-address buffer
         * -- shadows flickering while turning. */
        /* v126: this op's targets are now what their offsets resolve to */
        if (_d == 0) s_rt_clock_mark = s_rt_clock;   /* v129: see off_rt_get's reuse */
        if (rt_variants()) {
            off_rt_stamp(dr->rt_slot);
            for (int _m = 0; _m < 3; _m++) off_rt_stamp(dr->rt_mrt_slot[_m]);
            if (dr->is_vp && (dr->is_clear ? (dr->clear_mask & 0xF0u) : (dr->cmask & 0xFu))) {   /* v189 */
                if (dr->rt_slot >= 0) s_d3d.off_rt[dr->rt_slot].clast = s_d3d.off_rt[dr->rt_slot].last;
                for (int _m = 0; _m < 3; _m++) if (dr->rt_mrt_slot[_m] >= 0)
                    s_d3d.off_rt[dr->rt_mrt_slot[_m]].clast = s_d3d.off_rt[dr->rt_mrt_slot[_m]].last;
            }
        }
        if (dr->is_vp && dr->zeta_off && dr->rt_off && zeta_keyed()) {
            const int rs = rt_variants() ? dr->rt_slot : off_rt_find(dr->rt_off);
            if (rs >= 0) { int zi; D3D12_CPU_DESCRIPTOR_HANDLE none = {0};
                (void)zeta_dsv(dr->zeta_off, s_d3d.off_rt[rs].w, s_d3d.off_rt[rs].h, none, &zi); }
        }
        if (!dr->is_vp || dr->is_clear) continue;
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { static const char* vr = (const char*)1;     /* hoisted: this runs per DRAW */
          if (vr == (const char*)1) vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* SCREENTEX=1: report draws that sample a DISPLAY-SIZED texture. On RSX
         * a title renders a reflection/refraction into a surface and then
         * samples it; our backend renders into a D3D backbuffer, so the guest
         * memory that sampler reads is never written and the texture comes back
         * empty. Finding which geometry does it is the first step to feeding it
         * the rendered image instead. */
        /* EMPTYTEX=1: which geometry binds a texture whose source is all zero.
         * An unresolved texture renders untextured, so this names the surfaces
         * affected instead of leaving it to guesswork. */
        { static int ed = -1;
          if (ed < 0) { const char* e = getenv("EMPTYTEX"); ed = e ? atoi(e) : 0; }
          extern uint8_t* vm_base;
          if (ed && vm_base) for (int _u = 0; _u < RSX_UNITS; _u++) {
              if (!dr->tex[_u].set || !dr->tex[_u].off) continue;
              u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
              for (u32 i2 = 0; i2 < sz && i2 < 0x20000u; i2 += 997)
                  if (vm_base[dr->tex[_u].off + i2]) { nz = 1; break; }
              if (!nz) {
                  static u32 seen[24]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 24) { seen[ns++] = dr->fp_addr;
                      /* Scan a window around the resolved address: if the pass
                       * that fills this surface ran but landed elsewhere, the
                       * data is nearby; if the whole window is zero it was never
                       * produced. */
                      /* Same offset through BOTH context DMAs: a texture the
                       * guest put in main memory but that we resolve as LOCAL
                       * reads as untouched VRAM, i.e. all zeros. */
                      { extern u32 cellGcmResolveLocated(int, u32);
                        u32 la = cellGcmResolveLocated(1, dr->tex[_u].raw);
                        u32 ma = cellGcmResolveLocated(0, dr->tex[_u].raw);
                        u32 ln = 0, mn = 0;
                        for (u32 i4 = 0; i4 < 0x20000u; i4 += 997) {
                            if (vm_base[la + i4]) ln++;
                            if (vm_base[ma + i4]) mn++;
                        }
                        fprintf(stderr, "[TEXLOC] raw=0x%08X LOCAL@0x%08X nz=%u  MAIN@0x%08X nz=%u%c",
                                dr->tex[_u].raw, la, ln, ma, mn, 10); }
                      { u32 base = dr->tex[_u].off & ~0xFFFFFu;
                        for (u32 w = 0; w < 0x400000u; w += 0x100000u) {
                            u32 nzc = 0;
                            for (u32 i3 = 0; i3 < 0x100000u; i3 += 1021)
                                if (vm_base[base + w + i3]) nzc++;
                            fprintf(stderr, "[EMPTYSCAN] 0x%08X..+1MB nonzero %u/1027%c",
                                    base + w, nzc, 10);
                        } }
                      fprintf(stderr, "[EMPTYTEX] fp=0x%X unit=%d raw=0x%08X res=0x%08X %ux%u"
                                      " fmt=0x%02X verts=%u vp=%u,%u %ux%u%c",
                              dr->fp_addr, _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                              dr->tex[_u].h, dr->tex[_u].fmt, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              }
          } }
        { static int sd = -1;
          if (sd < 0) { const char* e = getenv("SCREENTEX"); sd = e ? atoi(e) : 0; }
          if (sd) for (int _u = 0; _u < RSX_UNITS; _u++)
              if (dr->tex[_u].set && dr->tex[_u].w == s_d3d.width &&
                  dr->tex[_u].h == s_d3d.height) {
                  static u32 seen[16]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[SCREENTEX] unit=%d raw=0x%08X %ux%u fmt=0x%02X"
                                      " fp=0x%X verts=%u vp=%u,%u %ux%u%c",
                              _u, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h,
                              dr->tex[_u].fmt, dr->fp_addr, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              } }
        /* Fill this draw's t0-t3 SRV window (DRAW_SRV_BASE + d*4): each unit
         * resolves to an offscreen RT (sampled directly), an uploaded guest
         * texture, or a null SRV. */
        double _sv0 = perf_on() ? perf_now() : 0.0;
        const u32 _umask = fp_used_units(dr->fp_addr);
        static int s_reuse = -1;
        if (s_reuse < 0) { const char* e = getenv("RSX_SRV_REUSE"); s_reuse = e ? atoi(e) : 1; }
        dr->srv_win = _d;
        s_srv_stage_on = s_reuse; s_srv_stage_n = 0;
        s_srv_stage_base = DRAW_SRV_BASE + ((u32)T_PARITY * MAX_DRAWS + _d) * RSX_UNITS;
        dr->gbc_mask = 0;
        for (int _u = 0; _u < RSX_UNITS; _u++) {
            u32 wslot = DRAW_SRV_BASE + ((u32)T_PARITY * MAX_DRAWS + _d) * RSX_UNITS + (u32)_u;
            dr->tex_rt[_u] = -1;
            /* Resistance port: a unit the fragment program never samples needs
             * no upload, no checksum and no descriptor (its HLSL does not even
             * declare it). York's programs read 1-4 of 16, and the 16-wide fill
             * was ~17% of the render thread. RSX_SRV_MASK=0 fills all. */
            if (!(_umask & (1u << _u))) continue;
            /* v180: a crossbar that forces all four channels to 0/1 never reads
             * the texture (the menu fade quad: t0 = local offset 0 as a 16x16
             * DXT1, remap 0x55FF = all ONE). Bound through the RT / upload paths
             * it sampled the display buffer's bytes as DXT1 for a frame -- the
             * static flash after the boot intro. Bind any resource with the
             * forced mapping instead. RSX_REMAP_CONST=0 off. */
            { static int en = -1;
              if (en < 0) { const char* e = getenv("RSX_REMAP_CONST"); en = e ? atoi(e) : 1; }
              static ID3D12Resource* s_const_tex = NULL;   /* 1x1, contents never read */
              if (en && !s_const_tex && s_d3d.device) {
                  D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                  D3D12_RESOURCE_DESC td = {0};
                  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = 4; td.Height = 4;
                  td.DepthOrArraySize = 1; td.MipLevels = 1;
                  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                  if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                          s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                          &IID_ID3D12Resource, (void**)&s_const_tex)))
                      { s_const_tex = NULL; en = 0; }
              }
              if (en && dr->tex[_u].set && s_const_tex) {
                  u8 o4[4]; rsx_texture_component_remap(dr->tex[_u].ctrl1, dr->tex[_u].fmt, o4);
                  if (o4[0] >= RSX_REMAP_ZERO && o4[1] >= RSX_REMAP_ZERO &&
                      o4[2] >= RSX_REMAP_ZERO && o4[3] >= RSX_REMAP_ZERO) {
                      { static int n = 0; if (n < 4) { n++;
                          fprintf(stderr, "[REMAPCONST] fp=0x%X unit %d off=0x%08X fmt=0x%02X remap=0x%04X: constant, texture not read%c",
                                  dr->fp_addr, _u, dr->tex[_u].off, dr->tex[_u].fmt, dr->tex[_u].ctrl1 & 0xFFFFu, 10); } }
                      srv_write(wslot, s_const_tex, DXGI_FORMAT_R8G8B8A8_UNORM,
                                rsx_remap_to_d3d(dr->tex[_u].ctrl1, dr->tex[_u].fmt & 0x9Fu));
                      continue;
                  } } }
            /* Resistance port: a sampler pointing AT a display buffer binds
             * that buffer's copy (disp_copy_*; filled in replay order). */
            /* LOCAL memory only (resolved 0xC0000000 + offset), display-sized:
             * a main-memory texture at the same raw offset is something else. */
            if (dr->tex[_u].set && disp_tex_on() && cellGcmOffsetIsDisplay(dr->tex[_u].raw) &&
                dr->tex[_u].off == 0xC0000000u + dr->tex[_u].raw &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height &&
                (dr->tex[_u].fmt & 0x9Fu) == 0x85u /* A8R8G8B8, like the display */) {
                ID3D12Resource* dc = disp_copy_res(dr->tex[_u].raw + 1u);
                if (dc) {
                    s_disp_sampled = 1;
                    dr->samp_disp = dr->tex[_u].raw + 1u;
                    { static u32 seen[48]; static int ns = 0; int k2 = 0;
                      u32 key = dr->fp_addr ^ ((u32)_u << 28);
                      for (int q = 0; q < ns; q++) if (seen[q] == key) k2 = 1;
                      if (!k2 && ns < 48) { seen[ns++] = key;
                        fprintf(stderr, "[DISPTEX] fp=0x%X unit %d samples display buffer 0x%X"
                                        " (rt=0x%X disp_tag=0x%X vp=%u,%u %ux%u n=%u)%c",
                                dr->fp_addr, _u, dr->tex[_u].raw, dr->rt_off, dr->disp_tag,
                                dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, dr->vertex_count, 10); } }
                    srv_write(wslot, dc, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* Display-sized sampler source -> the rendered frame. */
            if (dr->tex[_u].set && s_screen_copy &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height) {
                static int en = -1;
                if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
                if (en) {
                    { static int _n = 0; if (PS3_LOG_FIRST(_n, 3))
                        fprintf(stderr, "[SCREENTEX] bound frame copy at unit %d for"
                                        " fp=0x%X (%ux%u)%c", _u, dr->fp_addr,
                                dr->tex[_u].w, dr->tex[_u].h, 10); }
                    srv_write(wslot, s_screen_copy, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* A sampler whose guest buffer was never written, at the size of a
             * reduced-viewport pass we captured: that pass IS its producer. */
            if (dr->tex[_u].set && dr->tex[_u].off && s_subvp_n) {
                int hit = -1;
                for (int i = 0; i < s_subvp_n; i++)
                    if (s_subvp[i].res && s_subvp[i].w == dr->tex[_u].w &&
                        s_subvp[i].h == dr->tex[_u].h) { hit = i; break; }
                if (hit >= 0) {
                    extern uint8_t* vm_base;
                    u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
                    for (u32 i5 = 0; i5 < sz && i5 < 0x20000u; i5 += 997)
                        if (vm_base[dr->tex[_u].off + i5]) { nz = 1; break; }
                    if (!nz) {
                        { static int _n = 0; if (getenv("SUBVP_DBG") && PS3_LOG_FIRST(_n, 8))
                            fprintf(stderr, "[SUBVP] unit %d fp=0x%X <- captured %ux%u%c",
                                    _u, dr->fp_addr, dr->tex[_u].w, dr->tex[_u].h, 10); }
                        srv_write(wslot, s_subvp[hit].res, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                        continue;
                    }
                }
            }
            if (dr->tex[_u].set && (dr->tex[_u].fmt & 0x9Fu) == 0x85u && !dr->tex[_u].cube) {   /* v189c copy texture */
                const int gi = gbc_find(dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h);
                if (gi >= 0 && s_gbc_clock - s_gbc[gi].last < 256u) {
                    dr->gbc_mask |= (u16)(1u << _u);
                    srv_write(wslot, s_gbc[gi].res, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            if (dr->tex[_u].set) {
                int rt = off_rt_find(dr->tex[_u].raw);
                /* v125: a zeta surface read as A8R8G8B8 (zc_convert). */
                if (rt < 0 && (dr->tex[_u].fmt & 0x9Fu) == 0x85u && !dr->tex[_u].cube &&
                    dr->tex[_u].off == 0xC0000000u + dr->tex[_u].raw && zc_enabled()) {
                    const int z = zc_find(dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h);
                    if (z >= 0 && zc_prepare(z)) {
                        dr->tex_rt[_u] = 0x8000 | z;
                        zt_mark_used(z);
                        const u32 map = zc_mapping(dr->tex[_u].ctrl1);
                        srv_write_ex(wslot, s_zc[z].tex, DXGI_FORMAT_R8G8B8A8_UNORM, map, 0);
                        { static u32 seen[16]; static int ns = 0; int k = 0;
                          const u32 key = dr->fp_addr ^ ((u32)_u << 27);
                          for (int q = 0; q < ns; q++) if (seen[q] == key) k = 1;
                          if (!k && ns < 16) { seen[ns++] = key;
                              fprintf(stderr, "[ZETATEX] unit %d fp=0x%08X reads zeta %d (0x%08X %ux%u) as colour:"
                                              " declared %ux%u fmt=0x%02X remap=0x%04X srvmap=0x%04X%c",
                                      _u, dr->fp_addr, z, s_zt[z].off, s_zt[z].w, s_zt[z].h,
                                      dr->tex[_u].w, dr->tex[_u].h, dr->tex[_u].fmt & 0xFFu,
                                      dr->tex[_u].ctrl1 & 0xFFFFu, map, 10); } }
                        continue;
                    }
                }
                /* Resistance port: a DEPTH texture (shadow map) at an offscreen
                 * target's address samples that target's depth buffer -- the
                 * game renders its character shadow maps depth-only (Z16) and
                 * reads them back with a depth compare. RSX_SHADOWMAP=0 off. */
                { static int smen = -1;
                  if (smen < 0) { const char* e = getenv("RSX_SHADOWMAP"); smen = e ? atoi(e) : 1; }
                  const u32 dbf = dr->tex[_u].fmt & 0x9Fu;
                  if (smen && zeta_keyed() && dbf >= 0x90 && dbf <= 0x93 && dr->tex[_u].w && dr->tex[_u].h) {
                      const int z = zt_find_tex(dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h);
                      if (z >= 0) {
                          if (s_d3d.vp_fpcb_mapped) {
                              float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
                                  + ((u64)T_PARITY * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
                              ts[276] = (float)dr->tex[_u].w / (float)s_zt[z].w;
                              ts[277] = (float)dr->tex[_u].h / (float)s_zt[z].h;
                          }
                          dr->tex_rt[_u] = 0x5000 | z;
                          zt_mark_used(z);
                          srv_write(wslot, s_zt[z].res, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                          { static int n = 0; if (n < 6) { n++;
                              fprintf(stderr, "[SHADOWMAP] unit %d fp=0x%08X samples zeta surface %d (0x%08X %ux%u)%c",
                                      _u, dr->fp_addr, z, s_zt[z].off, s_zt[z].w, s_zt[z].h, 10); } }
                          continue;
                      }
                  }
                  if (smen && rt >= 0 && dbf >= 0x90 && dbf <= 0x93 && rt < MAX_OFF_RTS && s_rtdepth[rt] &&
                      /* the map is this surface or its top-left sub-rect (a 256x160
                       * map inside a 288x160 surface); a SMALLER buffer is a
                       * different surface that reuses the address */
                      s_rtdepth_w[rt] >= dr->tex[_u].w && s_rtdepth_h[rt] >= dr->tex[_u].h &&
                      dr->tex[_u].w && dr->tex[_u].h) {
                      if (s_d3d.vp_fpcb_mapped) {
                          float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
                              + ((u64)T_PARITY * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
                          ts[276] = (float)dr->tex[_u].w / (float)s_rtdepth_w[rt];
                          ts[277] = (float)dr->tex[_u].h / (float)s_rtdepth_h[rt];
                      }
                      dr->tex_rt[_u] = 0x1000 | rt;
                      srv_write(wslot, s_rtdepth[rt], DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                      { static int n = 0; if (n < 6) { n++;
                          fprintf(stderr, "[SHADOWMAP] unit %d fp=0x%08X samples depth of RT slot %d (0x%08X)%c", _u, dr->fp_addr, rt, s_d3d.off_rt[rt].off, 10); } }
                      continue;
                  }
                  if (smen && dbf >= 0x90 && dbf <= 0x93) {
                      /* No depth rendered at this address: sample "far" (lit). */
                      { static int n = 0; if (n < 12) { n++;
                          fprintf(stderr, "[SHADOWMAP] miss: unit %d fp=0x%08X depth tex raw=0x%08X %ux%u fmt=0x%02X pitch=0x%X; RTs:",
                                  _u, dr->fp_addr, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h, dr->tex[_u].fmt, dr->tex[_u].pitch);
                          for (int q = 0; q < MAX_OFF_RTS; q++) if (s_d3d.off_rt[q].res)
                              fprintf(stderr, " %d:0x%X/%ux%u%s", q, s_d3d.off_rt[q].off, s_d3d.off_rt[q].w, s_d3d.off_rt[q].h, s_rtdepth[q] ? "z" : "");
                          fprintf(stderr, "%c", 10); } }
                      if (lit_depth_res()) {
                          dr->tex_rt[_u] = 0x2000;
                          srv_write(wslot, s_lit_depth, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                      } else {
                          srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                      }
                      continue;
                  } }
                if (rt >= 0) {
                    { const int rc = off_rt_find_col(dr->tex[_u].raw);   /* v189 */
                      if (rc >= 0 && rc != rt) {
                          { static int n = 0; if (n < 6) { n++;
                              fprintf(stderr, "[RTCOL] unit %d fp=0x%08X 0x%08X: colour from slot %d %ux%u, not depth-only slot %d %ux%u%c",
                                      _u, dr->fp_addr, dr->tex[_u].raw, rc, s_d3d.off_rt[rc].w, s_d3d.off_rt[rc].h,
                                      rt, s_d3d.off_rt[rt].w, s_d3d.off_rt[rt].h, 10); } }
                          rt = rc;
                      } }
                    dr->tex_rt[_u] = rt;
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
                u32 _bf = dr->tex[_u].fmt & 0x9F;
                /* Resistance port: every format rsx_texture_layout decodes, not
                 * just the five the LBP/menu path needed. York's packed 16-bit
                 * (R5G6B5, A4R4G4B4, A1R5G5B5), HILO and float textures used to
                 * bind a NULL SRV and draw as solid black. RSX_TEXFMT_ALL=0
                 * restores the old list. */
                static int _all = -1;
                if (_all < 0) { const char* e = getenv("RSX_TEXFMT_ALL"); _all = e ? atoi(e) : 1; }
                int _fmt_ok = (_bf == 0x81 || _bf == 0x85 || _bf == 0x8B ||
                               (_bf >= 0x86 && _bf <= 0x88));
                if (_all && !_fmt_ok)
                    switch (_bf) {
                    case 0x82: case 0x83: case 0x84: case 0x97: case 0x9D: case 0x9E:
                    case 0x8C: case 0x8D: case 0x90: case 0x92: case 0x94: case 0x95:
                    case 0x9A: case 0x9B: case 0x9C: case 0x9F: _fmt_ok = 1; break;
                    }
                if (dr->tex[_u].off && _fmt_ok) {
                    double _tt = perf_on() ? perf_now() : 0.0;
                    int _cube = dr->tex[_u].cube && (dr_cube_mask(dr) & (1u << _u));
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt,
                                                _cube, dr->tex[_u].mips, dr->tex[_u].pitch);
                    if (perf_on()) { s_perf_tex += perf_now() - _tt; s_perf_ntex++;
                        s_perf_texbytes += (u64)dr->tex[_u].w * dr->tex[_u].h * 4u; }
                    /* Both forms of the offset are in hand here; the filters
                     * compare the BOUND one. */
                    if (s_duck_off && dr->tex[_u].off == s_duck_off)
                        s_duck_raw = dr->tex[_u].raw;
                    if (ts >= 0) {
                        DXGI_FORMAT sf = s_d3d.vp_tex[ts].dxfmt ? (DXGI_FORMAT)s_d3d.vp_tex[ts].dxfmt :
                            (_bf == 0x85) ? DXGI_FORMAT_R8G8B8A8_UNORM :
                            (_bf == 0x8B) ? DXGI_FORMAT_R8G8_UNORM :
                            (_bf == 0x86) ? DXGI_FORMAT_BC1_UNORM :
                            (_bf == 0x87) ? DXGI_FORMAT_BC2_UNORM :
                            (_bf == 0x88) ? DXGI_FORMAT_BC3_UNORM :
                                            DXGI_FORMAT_R8_UNORM;
                        srv_write_ex(wslot, s_d3d.vp_tex[ts].res, sf,
                                  (_bf == 0x81) ? 0x1000
                                                : rsx_remap_to_d3d(dr->tex[_u].ctrl1, _bf),
                                  _cube);
                        continue;
                    }
                }
            }
            /* TEX_BINDDBG=<N>: a unit that ends up with a NULL SRV. The draw is
             * still recorded, lit and transformed -- it just samples nothing,
             * which renders as a solid black object. */
            { static int cap = -1, n = 0;
              if (cap < 0) { const char* e = getenv("TEX_BINDDBG"); cap = e ? atoi(e) : 0; }
              if (cap && n < cap && dr->tex[_u].set) { n++;
                fprintf(stderr, "[TEXBIND] null SRV unit=%d raw=0x%X off=0x%X %ux%u fmt=0x%02X%c",
                        _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                        dr->tex[_u].h, dr->tex[_u].fmt, 10); } }
            srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        }
        if (s_srv_stage_on) {
            s_srv_stage_on = 0;
            static SrvStage prev[RSX_UNITS]; static u32 prev_n = 0, prev_mask = 0, prev_win = 0xFFFFFFFFu;
            static u32 prev_batch = 0xFFFFFFFFu; static const D3D12DrawRecord* prev_tbl = NULL;
            const u32 batch = s_submit_no ^ ((u32)T_PARITY << 31);
            int same = prev_win != 0xFFFFFFFFu && prev_batch == batch && prev_tbl == T_DRAWS &&
                       prev_mask == _umask && prev_n == s_srv_stage_n;
            for (u32 i = 0; same && i < s_srv_stage_n; i++)
                if (memcmp(&prev[i], &s_srv_stage[i], sizeof(SrvStage)) != 0) same = 0;
            if (same) dr->srv_win = prev_win;
            else {
                for (u32 i = 0; i < s_srv_stage_n; i++)
                    srv_create(s_srv_stage_base + s_srv_stage[i].unit, s_srv_stage[i].res,
                               s_srv_stage[i].fmt, s_srv_stage[i].mapping, s_srv_stage[i].cube);
                memcpy(prev, s_srv_stage, sizeof(SrvStage) * s_srv_stage_n);
                prev_n = s_srv_stage_n; prev_mask = _umask; prev_win = _d;
                prev_batch = batch; prev_tbl = T_DRAWS;
            }
        }
        if (perf_on()) s_perf_srv += perf_now() - _sv0;
        double _ps0 = perf_on() ? perf_now() : 0.0;
        if (dr->fp_addr) fp_set_unnorm(dr);
        if (dr->fp_addr) vp_get_fp_pso(dr_vs_key(dr), dr->fp_addr, dr->blend_key,
                                       dr_num_rts(dr),
                                       dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       dr->fp_exp32, dr->cmask, dr->cull, dr_cube_mask(dr));
        if (perf_on()) s_perf_pso += perf_now() - _ps0;
    }

    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);

    if (perf_on()) s_perf_pre += perf_now() - _pre0;
    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth -- but only at the start of a guest frame. A
     * batch that continues a frame after a mid-frame flush must keep them:
     * clearing the shared depth buffer there let Fall of Man's sky/fog pass
     * (drawn where depth is still far) paint over every opaque draw of the
     * first batch -- the "ghost town" in York. RSX_MIDFLUSH_KEEP=0 restores
     * the old clear-every-batch behaviour. */
    { static int keep = -1;
      if (keep < 0) { const char* e = getenv("RSX_MIDFLUSH_KEEP"); keep = e ? atoi(e) : 1; }
      const int cont = keep && s_batch_cont;
      if (!s_in_midflush) s_batch_cont = 0;   /* the frame's last batch: next one starts fresh */
      if (!cont) {
        s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
            s_d3d.cmd_list, rtv_handle, s_d3d.clear_color, 0, NULL);
        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
            s_d3d.cmd_list, dsv_handle,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f, 0, 0, NULL);
      } else { static int n = 0; if (n < 3) { n++;
          fprintf(stderr, "[MIDFLUSH] continuing batch: colour/depth kept%c", 10); } } }

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)SC(s_d3d.width), (float)SC(s_d3d.height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)SC(s_d3d.width), (LONG)SC(s_d3d.height)};
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && T_DRAWCNT > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Make the atlas SRV heap current and bind its table (t0) for textured
         * draws. Safe to set even if no draw is textured. */
        if (s_d3d.tex_ready && s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
        }

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* D3D12_IQ=1: dump exactly what the GPU is about to see -- the MVP root
         * constants and the first uploaded host vertices. The legacy path draws
         * with no validation errors yet produces zero pixels, so the geometry
         * must be degenerate/off-screen post-VS. */
        { static int _vd = -1;
          if (_vd < 0) { const char* e = getenv("D3D12_IQ"); _vd = e ? 1 : 0; }
          static int _n = 0;
          if (_vd && _n < 3) {
            _n++;
            fprintf(stderr, "[D3D12-DBG] have_mvp=%d draw_count=%u vb_offset=%u\n",
                    have_mvp, T_DRAWCNT, s_d3d.vb_offset);
            fprintf(stderr, "[D3D12-DBG] mvp rows: [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]"
                            "[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
                    mvp[0],mvp[1],mvp[2],mvp[3], mvp[4],mvp[5],mvp[6],mvp[7],
                    mvp[8],mvp[9],mvp[10],mvp[11], mvp[12],mvp[13],mvp[14],mvp[15]);
            if (s_d3d.vb_mapped) {
                /* host vertex = 9 floats (pos3 + col4 + uv2), VERTEX_STRIDE=36 */
                const float* bv = (const float*)s_d3d.vb_mapped;
                for (int k = 0; k < 3; k++) {
                    const float* v = bv + k * 9;
                    fprintf(stderr, "[D3D12-DBG]  hostvert[%d] pos=(%.3f,%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
                            k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                }
            }
            for (u32 d = 0; d < T_DRAWCNT && d < 3; d++) {
                const D3D12DrawRecord* dr = &T_DRAWS[d];
                fprintf(stderr, "[D3D12-DBG]  draw[%u] is_vp=%d is_clear=%d topo=%u cnt=%u vbofs=%u startv=%u rt=%u\n",
                        d, dr->is_vp, dr->is_clear, dr->topology, dr->vertex_count,
                        dr->vb_byte_offset, dr->vb_byte_offset / VERTEX_STRIDE, dr->rt_off);
            }
            fflush(stderr);
          } }

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). The PSO class
         * must match the topology or D3D12 rejects the draw. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = T_DRAWCNT;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &T_DRAWS[d];
            if (dr->is_vp) continue; /* drawn by the VP pass below */

            /* Select PSO: textured triangles (dbgfont) use the atlas PSO;
             * otherwise pick by topology class. */
            ID3D12PipelineState* target_pso = s_d3d.pipeline_state; /* default triangle */
            if (dr->textured && s_d3d.tex_ready && s_d3d.pipeline_state_tex) {
                target_pso = s_d3d.pipeline_state_tex;
            } else if (dr->topology == RSX_TOPOLOGY_POINTS) {
                target_pso = s_d3d.pipeline_state_points
                             ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
            } else if (dr->topology == RSX_TOPOLOGY_LINES ||
                       dr->topology == RSX_TOPOLOGY_LINE_STRIP) {
                target_pso = s_d3d.pipeline_state_lines
                             ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, topo_to_d3d(dr->topology));
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / VERTEX_STRIDE;
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }

    /* VP pass: real decompiled vertex program + atlas alpha-test PS. Feeds raw
     * float4 attrib0 from vp_vb and the vp_c[] constant bank. */
    if (s_d3d.vp_ready && T_DRAWCNT > 0) {
        int any = 0;
        for (u32 d = 0; d < T_DRAWCNT && d < MAX_DRAWS; d++)
            if (T_DRAWS[d].is_vp) { any = 1; break; }
        /* Textured geometry (dbgfont atlas) uses the sampling PS; untextured 3D
         * (vkcube) uses the colour-only PS. Fall back to whichever exists. */
        ID3D12PipelineState* vpso =
            (s_d3d.tex_ready && s_d3d.pipeline_state_vp) ? s_d3d.pipeline_state_vp
                                                         : s_d3d.pipeline_state_vp_color;
        if (!vpso) vpso = s_d3d.pipeline_state_vp;
        { static int cap = -1, n = 0;
          if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
          if (cap && vps_gate(&n, cap)) {
            u32 nvp = 0, nclr = 0;
            for (u32 d2 = 0; d2 < T_DRAWCNT && d2 < MAX_DRAWS; d2++) {
                if (T_DRAWS[d2].is_vp)    nvp++;
                if (T_DRAWS[d2].is_clear) nclr++;
            }
            fprintf(stderr, "[VPPASS] records=%u is_vp=%u clears=%u any=%d vpso=%p rootsig=%p\n",
                    T_DRAWCNT, nvp, nclr, any,
                    (void*)vpso, (void*)s_d3d.vp_root_sig); } }
        if (any && vpso) {
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
            s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, vpso);
            s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb));
            if (s_gf_buf)   /* GPU vertex fetch: this batch's raw-data half */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootShaderResourceView(s_d3d.cmd_list, 3,
                    s_gf_buf->lpVtbl->GetGPUVirtualAddress(s_gf_buf) + (u64)T_PARITY * GF_CAP);
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
            D3D12_VERTEX_BUFFER_VIEW vbv;
            vbv.BufferLocation = s_d3d.vp_vb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_vb)
                               + (u64)T_PARITY * MAX_VERTICES * 256;
            vbv.SizeInBytes    = MAX_VERTICES * 256;
            vbv.StrideInBytes  = 256;   /* 16 float4 attrib slots */
            s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
            if (s_vp_ib) {
                D3D12_INDEX_BUFFER_VIEW ibv;
                ibv.BufferLocation = s_vp_ib->lpVtbl->GetGPUVirtualAddress(s_vp_ib)
                                   + (u64)T_PARITY * VP_MAX_INDICES * 4;
                ibv.SizeInBytes = VP_MAX_INDICES * 4;
                ibv.Format = DXGI_FORMAT_R32_UINT;
                s_d3d.cmd_list->lpVtbl->IASetIndexBuffer(s_d3d.cmd_list, &ibv);
            }
            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh_base);
            int cur_rt = -1;                       /* target A: -1 = backbuffer */
            D3D12_CPU_DESCRIPTOR_HANDLE cur_dsv = dsv_handle;   /* depth of the bound target */
            float vp_last_w = 0, vp_last_h = 0;    /* for DRAWARGS */
            float dvp_x = 0, dvp_y = 0, dvp_w = 0, dvp_h = 0;  /* per-draw, for DRAWARGS */
            int cur_m[3] = {-1, -1, -1};           /* MRT B/C/D: -1 = unbound   */
            int mv_seg = -1;                       /* v165 FSR: first draw of the open scene run */
            u32 cur_zoff = 0;                      /* zeta surface of cur_dsv (zeta-keyed depth) */
            int cur_zi = -1;                       /* its s_zt slot, -1 = not a zeta buffer */
            double _rec0 = perf_on() ? perf_now() : 0.0;
            int seen_small_vp = 0, captured = 0;
            const int _p2 = perf_on();
            u64 _cA = _p2 ? __rdtsc() : 0;
            for (u32 d = 0; d < T_DRAWCNT && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &T_DRAWS[d];
                if (!dr->is_vp) continue;
                /* v189: GPU-side NV3089 copy; rebinds its own target, so the next op rebinds */
                if (dr->is_blit) { gpu_blit_replay(dr); cur_rt = -3; continue; }
                if (_p2) { const u64 t = __rdtsc(); s_rp_cyc[3] += t - _cA; _cA = t; }   /* previous draw's call + tail */
                /* The reduced-viewport passes (this title's reflection, drawn
                 * into a region of the same target) finish here -- snapshot
                 * before the full-screen scene overwrites them. */
                if (!dr->is_clear) {
                    if (dr->vp_w && dr->vp_w < s_d3d.width) {
                        seen_small_vp = 1;
                        subvp_note(dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h);
                    }
                    else if (seen_small_vp && !captured &&
                             dr->vp_w == s_d3d.width && dr->rt_off == 0) {
                        captured = 1;
                        screen_copy_capture(fi);
                        subvp_capture(fi);
                        s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                            s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                    }
                }
                /* Resistance port: two display buffers on one backbuffer
                 * (see disp_copy_save). Switching buffers saves the one held;
                 * sampling the held buffer saves it first. */
                if (s_disp_sampled && disp_tex_on()) {
                    int saved = 0;
                    if (dr->disp_tag && dr->disp_tag != s_bb_tag) {
                        if (s_bb_tag && s_bb_dirty) { disp_copy_save(fi, s_bb_tag); saved = 1; }
                        s_bb_tag = dr->disp_tag; s_bb_dirty = 0;
                    }
                    if (!dr->is_clear && dr->samp_disp && dr->samp_disp == s_bb_tag && s_bb_dirty) {
                        disp_copy_save(fi, s_bb_tag); s_bb_dirty = 0; saved = 1;
                    }
                    if (dr->disp_tag) s_bb_dirty = 1;
                    if (saved) {
                        if (cur_rt < 0)
                            s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                                s_d3d.cmd_list, 1, &rtv_handle, FALSE, &cur_dsv);
                        else
                            cur_rt = -3;   /* force the rebind below */
                    }
                }
                /* Render-to-texture: retarget when this op's surfaces differ.
                 * Depth is a single shared buffer, so clear it per switch. */
                int want  = !dr->rt_off ? -1 : rt_variants() ? dr->rt_slot : off_rt_find(dr->rt_off);
                /* Remember where geometry actually went: off_rt_find() matches
                 * on offset alone and slots share offsets at different sizes,
                 * so the composite cannot just look the source up by address. */
                if (want >= 0) { s_last_drawn_slot = want; s_last_drawn_off = dr->rt_off; }
                int wantm[3];
                for (int _m = 0; _m < 3; _m++)
                    wantm[_m] = !dr->rt_mrt[_m] ? -1 : rt_variants() ? dr->rt_mrt_slot[_m]
                                                            : off_rt_find(dr->rt_mrt[_m]);
                const u32 want_zoff = (zeta_keyed() && want >= 0) ? dr->zeta_off : 0;
                /* v165: the first draw that reads the finished scene gets it through FSR
                 * (before this draw binds its own target: the merge pass rebinds). */
                if (s_fsr_slot >= 0 && !s_fsr_done && fsr_mode() && !dr->is_clear && want != s_fsr_slot) {
                    int samp = 0;
                    for (int _u = 0; _u < RSX_UNITS; _u++) if (dr->tex_rt[_u] == s_fsr_slot) samp = 1;
                    if (samp) {
                        if (mv_seg >= 0) { fsr_mv_replay((u32)mv_seg, d, cur_dsv, s_fsr_slot); mv_seg = -1; }
                        if (s_fsr_mv_started) { fsr_dispatch(); cur_rt = -3; }
                    }
                }
                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2] || want_zoff != cur_zoff) {
                    if (mv_seg >= 0) { fsr_mv_replay((u32)mv_seg, d, cur_dsv, s_fsr_slot); mv_seg = -1; }   /* v165 */
                    D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                    UINT nrt = 1;
                    rh[0] = rtv_handle;
                    D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                    if (want >= 0) {
                        off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[0] = off_rt_rtv(want);
                        vp.Width  = (float)s_d3d.off_rt[want].w;
                        vp.Height = (float)s_d3d.off_rt[want].h;
                    }
                    for (int _m = 0; _m < 3 && wantm[_m] >= 0; _m++) {
                        off_rt_transition(wantm[_m], D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[nrt++] = off_rt_rtv(wantm[_m]);
                    }
                    vp_last_w = vp.Width; vp_last_h = vp.Height;
                    vp.Width = (float)SC((u32)vp.Width); vp.Height = (float)SC((u32)vp.Height);
                    D3D12_RECT sc = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
                    if (want_zoff) {
                        int zi = -1;
                        cur_dsv = zeta_dsv(want_zoff, s_d3d.off_rt[want].w, s_d3d.off_rt[want].h, dsv_handle, &zi);
                        if (zi < 0) cur_dsv = rt_dsv(want, dsv_handle);
                        cur_zi = zi;
                    } else {
                        cur_dsv = rt_dsv(want, dsv_handle);
                        cur_zi = -1;
                    }
                    if (cur_zi >= 0) s_zt_dirty[cur_zi] = 1;   /* v125: zc_convert */
                    cur_zoff = want_zoff;
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, nrt, rh, FALSE, &cur_dsv);
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
                    /* Shared-buffer mode: the old reset-on-switch (the shared
                     * buffer cannot hold two targets' depth at once). */
                    if (!depth_per_rt() || cur_dsv.ptr == dsv_handle.ptr)
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, cur_dsv,
                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                    /* RT_CLEARDBG=2: magenta-clear the surface the DRAWS bind,
                     * at the moment they bind it. The guest's own clears may go
                     * to a different surface entirely (they do here: clears to
                     * 0xAB0000, draws to 0x1BE0000), so clearing on the clear
                     * path tests the wrong target. Anything not magenta
                     * afterwards was rasterised. */
                    { static int cd2 = -1;
                      if (cd2 < 0) { const char* e = getenv("RT_CLEARDBG"); cd2 = e ? atoi(e) : 0; }
                      if (cd2 >= 2 && want >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
                              s_d3d.cmd_list, off_rt_rtv(want), mag, 0, NULL);
                      } }
                    /* VP_TESTTRI=1: our own triangle, through the game's exact
                     * pipeline -- same vpso, same root signature, same vertex
                     * buffer, same RTV just bound. Written at vertex 1000 so it
                     * cannot collide with the guest's data. Pair with
                     * VP_BYPASS=1 so the shader is a trivial passthrough.
                     *
                     * If this renders, the GPU does read vp_vb and the fault is
                     * in the guest draws' own state. If it does not, nothing
                     * this backend draws reaches a render target at all. */
                    { static int tt = -1;
                      if (tt < 0) { const char* e = getenv("VP_TESTTRI"); tt = e ? atoi(e) : 0; }
                      if (tt && want >= 0 && s_d3d.vp_vb_mapped) {
                          float* vtx = (float*)((char*)s_d3d.vp_vb_mapped
                              + (u64)T_PARITY * MAX_VERTICES * 256u
                              + (u64)1000 * 256u);
                          static const float tri[3][4] = {
                              {-2.0f, -2.0f, 0.0f, 1.0f},
                              { 2.0f, -2.0f, 0.0f, 1.0f},
                              { 0.0f,  2.0f, 0.0f, 1.0f} };
                          for (int _v = 0; _v < 3; _v++) {
                              float* dstv = vtx + (size_t)_v * (256u / 4u);
                              dstv[0] = tri[_v][0]; dstv[1] = tri[_v][1];
                              dstv[2] = tri[_v][2]; dstv[3] = tri[_v][3];
                          }
                          s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, 3, 1, 1000, 0);
                          { static int _n = 0;
                            if (PS3_LOG_FIRST(_n, 3)) fprintf(stderr, "[TESTTRI] issued on rt=%d off=0x%08X%c",
                                                  want, s_d3d.off_rt[want].off, 10); }
                      } }
                    cur_rt = want;
                    for (int _m = 0; _m < 3; _m++) cur_m[_m] = wantm[_m];
                }
                if (fsr_mode() && !dr->is_clear) {   /* v165 FSR bookkeeping */
                    if (want >= 0 && s_d3d.off_rt[want].w == s_d3d.width && s_d3d.off_rt[want].h == s_d3d.height)
                        s_fsr_cnt[want]++;
                    if (dr->fsr == 1 && want >= 0) { fsr_note_scene_draw(dr, want); if (mv_seg < 0) mv_seg = (int)d; }
                }
                if (dr->is_clear) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =
                        (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                    /* RT_CLEARDBG=1: clear offscreen targets to magenta rather
                     * than the guest colour. "0.000% nonzero" cannot tell an
                     * untouched surface from one written black, and that
                     * ambiguity invalidates every read of an offscreen target.
                     * If the surface comes back magenta the clears land and the
                     * draws are writing black; if it stays black, nothing
                     * reaches it at all. */
                    /* Depth/stencil part: one depth buffer serves every
                     * target, and it is reset whenever the pass switches
                     * targets; a clear inside a pass has to land here. */
                    if (dr->clear_mask & 0x3u) {
                        D3D12_CLEAR_FLAGS zf = (D3D12_CLEAR_FLAGS)
                            (((dr->clear_mask & 0x1u) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                             ((dr->clear_mask & 0x2u) ? D3D12_CLEAR_FLAG_STENCIL : 0));
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, cur_dsv,
                            zf, dr->clear_z, dr->clear_s, 0, NULL);
                        if (cur_zi >= 0) s_zt_dirty[cur_zi] = 1;
                    }
                    if (!(dr->clear_mask & 0xF0u))
                        continue;
                    { static int cd = -1;
                      if (cd < 0) { const char* e = getenv("RT_CLEARDBG"); cd = e ? atoi(e) : 0; }
                      if (cd && cur_rt >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, mag, 0, NULL);
                      } else
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, dr->cc, 0, NULL); }
                    for (int _m = 0; _m < 3; _m++)
                        if (cur_m[_m] >= 0) {
                            D3D12_CPU_DESCRIPTOR_HANDLE rhm = off_rt_rtv(cur_m[_m]);
                            s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rhm, dr->cc, 0, NULL);
                        }
                    continue;
                }
                /* Resistance port: live draw filter (logs\\fpkill.txt). */
                if (_p2) { const u64 t = __rdtsc(); s_rp_cyc[0] += t - _cA; _cA = t; }
                if (dr->fp_addr && fpkill_match(dr->fp_addr)) continue;
                /* v189: the Auger's through-wall shimmer (fp 0x16764C1) refracts a copy
                 * of the screen around the round (unit 1, copied by NV3089 -- see
                 * rsx_d3d12_blit_from_surface). Drawn only when that copy was made on
                 * the GPU (unit 1 is a target of exactly the texture's size); otherwise
                 * it read stale memory and drew a black blob. RSX_AUGER_FX=0 never
                 * draws it, 2 always does. */
                { static int ax = -1; if (ax < 0) { const char* e = getenv("RSX_AUGER_FX"); ax = e ? atoi(e) : 1; }
                  if (dr->fp_addr == 0x016764C1u && ax != 2) {
                      if (!ax || !(dr->gbc_mask & 2u)) continue;   /* v189c: unit 1 must be a GPU copy */
                  } }
                if (dr->fp_addr && s_fpnb_n && fp_in(s_fpnb, s_fpnb_n, dr->fp_addr)) ((D3D12DrawRecord*)dr)->blend_key = 0;
                if (dr->fp_addr && s_fpnd_n && fp_in(s_fpnd, s_fpnd_n, dr->fp_addr)) ((D3D12DrawRecord*)dr)->cull &= ~0x200u;
                /* Per-draw pipeline: prefer the guest's own compiled FP; fall
                 * back to the hardcoded atlas/colour PS pair. */
                fp_set_unnorm(dr);
                ID3D12PipelineState* dpso =
                    dr->fp_addr ? vp_get_fp_pso(dr_vs_key(dr), dr->fp_addr, dr->blend_key,
                                                dr_num_rts(dr),
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                           : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                dr->fp_exp32, dr->cmask, dr->cull,
                                                dr_cube_mask(dr)) : NULL;
                /* Resistance port: a guest FP whose pipeline failed used to fall
                 * back to the built-in colour shader and drew a solid black
                 * shape. Skip it instead (RSX_FAILSKIP=0 restores). */
                { static int fs = -1;
                  if (fs < 0) { const char* e = getenv("RSX_FAILSKIP"); fs = e ? atoi(e) : 1; }
                  if (fs && dr->fp_addr && !dpso) continue; }
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,
                                                         dpso ? dpso : vpso);
                if (_p2) { const u64 t = __rdtsc(); s_rp_cyc[1] += t - _cA; _cA = t; }
                /* Per-draw viewport: the guest rect when sane, else the
                 * full target. Scissor tracks the same rect. */
                {
                    float tw = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].w : (float)s_d3d.width;
                    float th = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].h : (float)s_d3d.height;
                    D3D12_VIEWPORT dvp = {0, 0, tw, th, 0.0f, 1.0f};
                    if (dr->vp_w >= 2 && dr->vp_h >= 2 &&
                        (float)(dr->vp_x + dr->vp_w) <= tw + 0.5f &&
                        (float)(dr->vp_y + dr->vp_h) <= th + 0.5f) {
                        dvp.TopLeftX = (float)dr->vp_x;
                        dvp.TopLeftY = (float)dr->vp_y;
                        dvp.Width    = (float)dr->vp_w;
                        dvp.Height   = (float)dr->vp_h;
                    }
                    dvp_x = dvp.TopLeftX; dvp_y = dvp.TopLeftY;
                    dvp_w = dvp.Width;    dvp_h = dvp.Height;
                    if (rs_scale() != 1.0f) {   /* v136: guest pixels -> render pixels */
                        dvp.TopLeftX = SCF(dvp.TopLeftX); dvp.TopLeftY = SCF(dvp.TopLeftY);
                        dvp.Width = SCF(dvp.Width);       dvp.Height = SCF(dvp.Height);
                    }
                    D3D12_RECT dsc = {(LONG)(dvp.TopLeftX + 0.5f), (LONG)(dvp.TopLeftY + 0.5f),
                                      (LONG)(dvp.TopLeftX + dvp.Width + 0.5f),
                                      (LONG)(dvp.TopLeftY + dvp.Height + 0.5f)};
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &dvp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &dsc);
                }
                /* Per-draw textures: bind this draw's t0-t3 SRV window.
                 * Any sampled offscreen RT transitions to PSR first (never
                 * one of the currently-bound colour targets). */
                for (int _u = 0; _u < RSX_UNITS; _u++) {
                    int rt = dr->tex_rt[_u];
                    if (rt >= 0 && (rt & 0x8000)) { zc_convert(rt & 0xFF); continue; }
                    if (rt == 0x2000) { lit_depth_prepare(); continue; }
                    if (rt >= 0 && (rt & 0x4000)) {
                        const int z = rt & 0xFF;
                        if (z < MAX_ZETA && s_zt[z].res && cur_dsv.ptr != zt_handle(z).ptr)
                            zt_transition(z, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        continue;
                    }
                    if (rt >= 0 && (rt & 0x1000)) {
                        const int ds = rt & 0xFF;
                        if (s_rtdepth[ds] && cur_dsv.ptr != rt_dsv_handle_peek(ds).ptr)
                            rt_depth_transition(ds, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        continue;
                    }
                    if (rt >= 0 && rt != cur_rt &&
                        rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2])
                        off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE + ((u32)T_PARITY * MAX_DRAWS + dr->srv_win) * RSX_UNITS) * s_d3d.srv_inc;
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
                /* Per-draw constants: this draw's vp_cb + FP texscale slots. */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                    s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb)
                    + ((u64)T_PARITY * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
                if (s_d3d.vp_fpcb)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2,
                        s_d3d.vp_fpcb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_fpcb)
                        + ((u64)T_PARITY * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
                /* DRAWARGS=<N>: the arguments actually handed to the GPU, plus
                 * the target they are aimed at. A zero vertex count, or a start
                 * vertex past the buffer, rasterises nothing while every other
                 * check upstream still says the draw is fine. */
                { static int dcap = -1, dn = 0;
                  if (dcap < 0) { const char* e = getenv("DRAWARGS"); dcap = e ? atoi(e) : 0; }
                  if (dcap && dn < dcap) { dn++;
                      D3D12_CPU_DESCRIPTOR_HANDLE _rh =
                          (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      fprintf(stderr, "[DRAWARGS] verts=%u start=%u rt=%d rec=0x%08X bound=0x%08X"
                              " tex0=0x%08X texrt=%d dvp=%.0f,%.0f %.0fx%.0f%c",
                              dr->vertex_count, dr->vb_byte_offset / 256, cur_rt,
                              dr->rt_off,
                              (cur_rt >= 0) ? s_d3d.off_rt[cur_rt].off : 0u,
                              dr->tex[0].raw, dr->tex_rt[0],
                              (double)dvp_x, (double)dvp_y,
                              (double)dvp_w, (double)dvp_h, 10); } }
                /* RT_REBIND=1: re-issue OMSetRenderTargets immediately before
                 * every draw instead of only when the target changes. A clear
                 * proves the RTV descriptor is good but not that the binding is
                 * still live at draw time -- ClearRenderTargetView writes
                 * through the descriptor and never consults the OM. If anything
                 * between the retarget and here drops the binding, this restores
                 * it. */
                { static int rb = -1;
                  if (rb < 0) { const char* e = getenv("RT_REBIND"); rb = e ? atoi(e) : 0; }
                  if (rb) {
                      D3D12_CPU_DESCRIPTOR_HANDLE brh[4];
                      UINT bn = 1;
                      brh[0] = (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      for (int _m = 0; _m < 3; _m++)
                          if (cur_m[_m] >= 0) brh[bn++] = off_rt_rtv(cur_m[_m]);
                      s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                          s_d3d.cmd_list, bn, brh, FALSE, &cur_dsv);
                  } }
                if (_p2) { const u64 t = __rdtsc(); s_rp_cyc[2] += t - _cA; _cA = t; }
                if (dr->cull >> 24) s_d3d.cmd_list->lpVtbl->OMSetStencilRef(s_d3d.cmd_list, dr->stencil_ref);   /* v178 */
                if (dr->ib_count && s_vp_ib)
                    s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list,
                        dr->ib_count, 1, dr->ib_first, dr->gf ? 0 : (INT)(dr->vb_byte_offset / 256), 0);
                else
                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
                /* v125: depth may have been written (zc_convert re-copies). */
                if (cur_zi >= 0 && (!(dr->cull & 0x100u) || (dr->cull & 0x400u)))
                    s_zt_dirty[cur_zi] = 1;
                /* VP_SUBMIT=<N>: prove the VP pass actually reaches the GPU.
                 * "records exist" and "draws were submitted" are different
                 * claims, and every blank-output investigation conflates them. */
                { static int cap = -1, n = 0;
                  if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
                  if (cap && vps_gate(&n, cap)) {
                    fprintf(stderr, "[VPSUBMIT] draw[%u] verts=%u pso=%s vp=%ux%u tex0=0x%X(set=%d) tex1=0x%X slot=%d fp=0x%X rt=0x%X t0=%ux%u/f%02X off=0x%08X%c",
                            d, dr->vertex_count, dpso ? "guest-fp" : "fallback",
                            dr->vp_w, dr->vp_h, dr->tex[0].raw, dr->tex[0].set,
                            dr->tex[1].raw, dr->tex_slot, dr->fp_addr, dr->rt_off,
                            dr->tex[0].w, dr->tex[0].h, dr->tex[0].fmt & 0xFF, dr->tex[0].off, 10);
                    fprintf(stderr, "            vs_idx=%d cb_slot=%u vbofs=%u cull=%u blend=%d%c",
                            dr->vs_idx, dr->cb_slot, dr->vb_byte_offset / 256,
                            dr->cull, dr->blend, 10); } }
            }
            if (perf_on()) s_perf_gpu += perf_now() - _rec0;   /* reuse: record time */
            if (mv_seg >= 0) { fsr_mv_replay((u32)mv_seg, T_DRAWCNT, cur_dsv, s_fsr_slot); mv_seg = -1; }   /* v165 */
            /* Leave the backbuffer bound for the dump/present epilogue. */
            if (cur_rt >= 0) {
                s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                D3D12_VIEWPORT vp = {0, 0, (float)SC(s_d3d.width), (float)SC(s_d3d.height), 0.0f, 1.0f};
                D3D12_RECT sc = {0, 0, (LONG)SC(s_d3d.width), (LONG)SC(s_d3d.height)};
                s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
            }
        }
    }

    if (perf_on()) s_perf_rf += perf_now() - _rf0;
    /* MEMPEEK=<hex addr>:<n>: n big-endian floats at a guest address each frame,
     * with a min/max so a buffer of constants is distinguishable from real data. */
    { const char* mp = getenv("MEMPEEK");
      if (mp) { extern uint8_t* vm_base; static int _n = 0;
        u32 addr = 0; int cnt = 8; sscanf(mp, "%x:%d", &addr, &cnt);
        if (vm_base && addr && PS3_LOG_FIRST(_n, 6)) {
            float lo = 1e30f, hi = -1e30f; u32 nz = 0;
            for (int i = 0; i < cnt; i++) {
                const u8* q = vm_base + addr + i * 4;
                u32 bits = ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3];
                float f; memcpy(&f, &bits, 4);
                if (f == f && f < 1e30f && f > -1e30f) { if (f < lo) lo = f; if (f > hi) hi = f; }
                if (bits) nz++;
            }
            fprintf(stderr, "[MEMPEEK] 0x%08X n=%d nonzero=%u range[%g..%g]%c",
                    addr, cnt, nz, lo, hi, 10);
        } } }
    /* VRAMSCAN=1: which 64 KB blocks of guest memory change between frames.
     * "nothing writes the buffer we read" is only half an answer -- this says
     * where the producer IS writing, so the two can be matched up. */
    { static int vs = -1; if (vs < 0) vs = getenv("VRAMSCAN") ? 1 : 0;
      if (vs) { extern uint8_t* vm_base;
        static u32 prev[512]; static int have = 0; static int reported = 0;
        u32 base = 0xC6000000u; u32 nblk = 512;          /* 32 MB window */
        const char* bs = getenv("VRAMSCAN_BASE");
        if (bs) base = (u32)strtoul(bs, NULL, 0);
        if (vm_base && reported < 6) {
            u32 changed = 0; char list[512]; int ln = 0;
            for (u32 b = 0; b < nblk; b++) {
                const u8* p = vm_base + base + b * 0x10000u;
                u32 h = 2166136261u;
                for (u32 i = 0; i < 0x10000u; i += 61) h = (h ^ p[i]) * 16777619u;
                if (have && h != prev[b]) { changed++;
                    if (ln < 400) ln += snprintf(list + ln, sizeof(list) - ln,
                                                 " +0x%X", b * 0x10000u); }
                prev[b] = h;
            }
            if (have && changed) { reported++;
                fprintf(stderr, "[VRAMSCAN] base=0x%08X %u/512 blocks changed:%s%c",
                        base, changed, list, 10); }
            have = 1;
        } } }
    /* FRAME_BUDGET=1: geometry a frame asked for vs what the batch can hold.
     * Reported from render_frame so it works under RSX_ACCUM_FRAME too -- it
     * used to live in the clear-boundary present path, which accum bypasses, so
     * it silently produced nothing in exactly the configuration being used. */
    { static int fb = -1;
      if (fb < 0) { const char* e = getenv("FRAME_BUDGET"); fb = e ? atoi(e) : 0; }
      if (fb && s_req_draws) { static int n = 0; if (PS3_LOG_FIRST(n, 40))
        fprintf(stderr, "[BUDGET] frame: requested %llu verts / %llu draws, "
                        "buffer holds %u verts, %llu draws truncated%c",
                (unsigned long long)s_req_verts, (unsigned long long)s_req_draws,
                MAX_VERTICES, (unsigned long long)s_drop_draws, 10); }
      s_req_verts = 0; s_req_draws = 0; s_drop_draws = 0; }
    if (perf_on()) {
        static double s_t_prev = 0.0; static int s_n = 0;
        double now = perf_now();
        if (s_t_prev > 0.0) s_perf_frame += now - s_t_prev;
        s_t_prev = now;
        if (++s_n % 20 == 0) {
            double fr = s_perf_frame > 0 ? s_perf_frame : 1;
            s_perf_load_pct = (int)(100.0 * s_perf_rf / fr);
            fprintf(stderr, "[PERF] %.2f fps | tex %.2fs (%.0f%%, %d calls) | vtx %.2fs"
                            " (%.0f%%, %.0fk verts) | render_frame %.2fs (%.0f%%)"
                            " | prepass %.2fs (%.0f%%) [srv %.2fs %.0f%% | pso %.2fs %.0f%% %d calls %d MISS %dKB hashed] | guest %.2fs (%.0f%%)"
                            " | present %.3fs gpuwait %.3fs vtxdrain %.3fs%c",
                    20.0 / fr,
                    s_perf_tex, 100.0 * s_perf_tex / fr, s_perf_ntex,
                    s_perf_vtx, 100.0 * s_perf_vtx / fr,
                    (double)s_perf_nverts / 1000.0,
                    s_perf_rf, 100.0 * s_perf_rf / fr,
                    s_perf_pre, 100.0 * s_perf_pre / fr,
                    s_perf_srv, 100.0 * s_perf_srv / fr,
                    s_perf_pso, 100.0 * s_perf_pso / fr,
                    s_perf_pso_calls, s_perf_pso_miss, s_perf_pso_hashbytes / 1024,
                    fr - s_perf_rf, 100.0 * (fr - s_perf_rf) / fr,
                    s_perf_present, s_perf_wait, s_perf_vdrain, 10);
            s_perf_present = 0.0; s_perf_wait = 0.0; s_perf_vdrain = 0.0;
            { const double mc = 1e-6;
              fprintf(stderr, "[PERF2] batches: present=%ld clear=%ld midflush(vtx)=%ld midflush(draws)=%ld flip=%ld"
                              " | replay Mcyc: rt %.0f pso %.0f state %.0f draw+tail %.0f"
                              " | shaders compiled %ld (%.0f ms) cache hits %ld, new PSOs %ld (%.0f ms)"
                              " | tex ring %ld full %ld | vs async queued %ld done %ld | RT made %u retired %u reused %u"
                              " | zeta made %u reused %u | PSOs pre-built %ld | async PSOs built %ld, draws skipped %ld%c",
                      (long)s_rfr[RFR_PRESENT], (long)s_rfr[RFR_CLEAR], (long)s_rfr[RFR_MIDV],
                      (long)s_rfr[RFR_MIDD], (long)s_rfr[RFR_FLIP],
                      s_rp_cyc[0] * mc, s_rp_cyc[1] * mc, s_rp_cyc[2] * mc, s_rp_cyc[3] * mc,
                      (long)s_shc_miss, s_shc_ms, (long)s_shc_hits, (long)s_pso_new, s_pso_ms,
                      (long)s_tr_hits, (long)s_tr_full, (long)s_vsj_queued, (long)s_vsj_done,
                      s_rt_created, s_rt_evicted, s_rt_reused, s_zt_created, s_zt_reused, (long)s_psow_hits,
                      (long)InterlockedExchange(&s_psoa_built, 0), (long)InterlockedExchange(&s_psoa_skip, 0), 10);
              s_tr_hits = 0; s_tr_full = 0; s_rt_created = 0; s_rt_evicted = 0; s_psow_hits = 0;
              s_rt_reused = 0; s_zt_created = 0; s_zt_reused = 0;
              for (int k = 0; k < RFR_N; k++) s_rfr[k] = 0;
              for (int k = 0; k < 6; k++) s_rp_cyc[k] = 0;
              s_shc_miss = 0; s_shc_ms = 0; s_shc_hits = 0; s_pso_new = 0; s_pso_ms = 0; }
            s_perf_frame = 0.0; s_perf_tex = 0.0; s_perf_vtx = 0.0; s_perf_rf = 0.0;
            s_perf_gpu = 0.0; s_perf_pre = 0.0; s_perf_srv = 0.0; s_perf_pso = 0.0;
            s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;
            s_perf_ntex = 0; s_perf_texbytes = 0; s_perf_nverts = 0;
        }
    }
    { static int dt = -1;
      if (dt < 0) { const char* e = getenv("RSX_DBG_TRACK"); dt = e ? atoi(e) : 0; }
      if (dt) { static int fr = 0; fr++;
        if (s_lock_n)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: %u duck draws, ndc=(%.3f,%.3f)%c",
                    fr, s_lock_n, (float)(s_lock_sx/s_lock_n), (float)(s_lock_sy/s_lock_n), 10);
        else if (fr % 20 == 0)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: NO duck draws%c", fr, 10);
      } }
    if (s_lock_n) {                       /* publish this frame's centroid */
        s_lock_x = (float)(s_lock_sx / s_lock_n);
        s_lock_y = (float)(s_lock_sy / s_lock_n);
        s_lock_valid = 1;
        s_lock_sx = s_lock_sy = 0.0; s_lock_n = 0;
    }
    s_dbg_last_draws = T_DRAWCNT;
    if (!t_render_thread) {   /* producer state: the FIFO thread's (render_frame resets it under RSX_ASYNC) */
        s_d3d.vb_offset  = 0; /* reset for next frame */
        s_d3d.vp_vb_offset = 0; s_vp_ib_off = 0; s_gf_off = 0; s_gf_gen++;
    }
    T_DRAWCNT = 0;
    T_PARITY = (T_PARITY + 1) % NPAR;  /* next batch's records go to the next third */

    /* Debug: RTT_SAVERT=<hex raw offset>[:frame] copies that offscreen RT
     * into a readback buffer this frame and writes rt_save.bmp (half-float
     * RTs are tonemapped |v| -> byte). */
    static ID3D12Resource* s_rtsave_buf = NULL;
    static u32 s_rtsave_state = 0;   /* 1 = copy queued this frame */
    static u32 s_rtsave_w, s_rtsave_h, s_rtsave_pitch, s_rtsave_dxgi;
    { const char* sv = getenv("RTT_SAVERT");
      static int _done = 0;
      static u32 _skip = 0xFFFFFFFFu;
      if (_skip == 0xFFFFFFFFu) { const char* sk = getenv("RTT_SAVERT_SKIP");
          _skip = sk ? (u32)strtoul(sk, NULL, 0) : 0; }   /* frame 60 is far too
          early for a title that takes a minute to reach its menu */
      /* RTT_SAVERT_AFTER=<seconds> triggers on wall time instead of a frame
       * count. This title runs its menu at a few frames a second, so a frame
       * number is not a usable way to say "once it has settled". */
      static u32 _after = 0xFFFFFFFFu;
      if (_after == 0xFFFFFFFFu) { const char* sa = getenv("RTT_SAVERT_AFTER");
          _after = sa ? (u32)strtoul(sa, NULL, 0) : 0; }
      static clock_t _t0 = 0;
      if (!_t0) _t0 = clock();
      const int _due = _after
          ? ((u32)((clock() - _t0) / CLOCKS_PER_SEC) >= _after)
          : (s_d3d.frame_count > 60 + _skip);
      { static int _p1 = 0;
        if (sv && _due && !_p1) { _p1 = 1;
            printf("[RTT_SAVERT] due: want=0x%lX found_rt=%d frame=%u\n",
                   strtoul(sv, NULL, 16), off_rt_find((u32)strtoul(sv, NULL, 16)),
                   (unsigned)s_d3d.frame_count); fflush(stdout); } }
      if (sv && !_done && _due) {
        int rt = off_rt_find((u32)strtoul(sv, NULL, 16));
        fprintf(stderr, "[RTT_SAVERT] rt=%d res=%p\n", rt, rt >= 0 ? (void*)s_d3d.off_rt[rt].res : NULL);
        if (rt >= 0 && s_d3d.off_rt[rt].res) {
            OffRT* r = &s_d3d.off_rt[rt];
            u32 bpp = (r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
                      (r->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
            u32 pitch = (r->w * bpp + 255) & ~255u;
            fprintf(stderr, "[RTT_SAVERT] rt=%d w=%u h=%u bpp=%u pitch=%u dxgi=%d bytes=%llu\n",
                    rt, r->w, r->h, bpp, pitch, (int)r->dxgi,
                    (unsigned long long)((u64)pitch * r->h));
            if (!s_rtsave_buf) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (u64)pitch * r->h; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&s_rtsave_buf);
            }
            if (!s_rtsave_buf) fprintf(stderr, "[RTT_SAVERT] readback buffer alloc FAILED\n");
            if (s_rtsave_buf) {
                off_rt_transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                cdst.pResource = s_rtsave_buf;
                cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cdst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
                cdst.PlacedFootprint.Footprint.Width    = r->w;
                cdst.PlacedFootprint.Footprint.Height   = r->h;
                cdst.PlacedFootprint.Footprint.Depth    = 1;
                cdst.PlacedFootprint.Footprint.RowPitch = pitch;
                csrc.pResource = r->res;
                csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                s_rtsave_state = 1;
                s_rtsave_w = r->w; s_rtsave_h = r->h;
                s_rtsave_pitch = pitch; s_rtsave_dxgi = r->dxgi;
                fprintf(stderr, "[RTT_SAVERT] copy queued: rt=%d %ux%u pitch=%u\n",
                        rt, r->w, r->h, pitch);
                _done = 1;
            }
        }
      } }

    /* CELLMARK_DUMP_MINDRAWS=<N>: only consider frames carrying at least N draw
     * records. Most render_frame calls come from the guest flip with a batch the
     * clear boundary already drained, so they present an empty backbuffer -- and
     * sampling those wastes every dump on a black image while the frames that do
     * hold the scene go uncaptured. */
    { static int mind = -1;
      if (mind < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                      mind = e ? atoi(e) : 0; }
      /* draw_count is already reset by here; s_dbg_last_draws holds the
       * count this frame actually rendered. */
      if (mind > 0 && s_dbg_last_draws < (u32)mind) goto skip_dump_consider; }
    if (s_d3d.dump_skip_left > 0 && s_d3d.dump_frames_left > 0) s_d3d.dump_skip_left--;
skip_dump_consider: ;
    disp_frame_end(fi);
    guest_fb_present(fi);
    composite_present(fi);
    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf
                   && s_d3d.dump_skip_left == 0);
    { static int mind2 = -1;
      if (mind2 < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                       mind2 = e ? atoi(e) : 0; }
      if (mind2 > 0 && s_dbg_last_draws < (u32)mind2) dumping = 0; }
    /* CELLMARK_DUMP_EVERY=N: after each dump, skip N-1 frames -- samples the whole
     * run instead of one contiguous window, so a short burst of real content
     * can't fall between the dumped frames. */
    if (dumping) {
        static int s_every = -1;
        if (s_every < 0) { const char* e = getenv("CELLMARK_DUMP_EVERY");
                           s_every = e ? atoi(e) : 0; }
        if (s_every > 1) s_d3d.dump_skip_left = s_every - 1;
    }
    if (dumping) {
        /* RT -> COPY_SOURCE, copy into the readback buffer, then -> PRESENT. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_d3d.readback_buf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = s_d3d.width;
        dst.PlacedFootprint.Footprint.Height   = s_d3d.height;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
        src.pResource = s_d3d.render_targets[fi];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    } else {
        /* End-of-frame fallback snapshot: used when the frame never contained a
         * reduced-viewport pass to capture mid-frame. */
        screen_copy_capture(fi);
        /* Transition render target to PRESENT state */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    }

    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);
    s_submit_no++;

    /* D3D12_IQ=1: drain the debug layer's message queue after submitting the
     * frame. The legacy (is_vp=0) DrawInstanced path silently produced ZERO
     * pixels in flOw's render injection while ClearRenderTargetView worked, and
     * every input (verts, offsets, layout, MVP, PSO, root sig, depth, cull) was
     * verified correct -- so the only remaining explanation is a validation
     * error the debug layer is swallowing. Off unless the env var is set. */
    { static int _iq = -1;
      if (_iq < 0) { const char* e = getenv("D3D12_IQ"); _iq = e ? 1 : 0; }
      if (_iq && s_d3d.device) {
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            static int _printed = 0;
            for (UINT64 mi = 0; mi < n && _printed < 80; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len))) {
                    fprintf(stderr, "[D3D12-IQ][sev=%d id=%d] %s\n",
                            (int)m->Severity, (int)m->ID, m->pDescription);
                    _printed++;
                }
                free(m);
            }
            if (n) iq->lpVtbl->ClearStoredMessages(iq);
            iq->lpVtbl->Release(iq);
            fflush(stderr);
        }
      } }

    if (s_sc_dump_pending) {
        s_sc_dump_pending = 0;
        wait_for_gpu();
        s_dump_name = "screencopy";
        dump_backbuffer_bmp();
        s_dump_name = NULL;
    }
    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        fprintf(stderr, "[RTT_SAVERT] writing rt_save.bmp %ux%u\n", s_rtsave_w, s_rtsave_h);
        if (!dumping) { double _g = perf_on() ? perf_now() : 0.0;
                        wait_for_gpu();
                        if (perf_on()) s_perf_gpu += perf_now() - _g; }
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_rtsave_pitch * s_rtsave_h};
        if (SUCCEEDED(s_rtsave_buf->lpVtbl->Map(s_rtsave_buf, 0, &rr, &mp)) && mp) {
            FILE* f = fopen("rt_save.bmp", "wb");
            if (f) {
                u32 w = s_rtsave_w, h = s_rtsave_h;
                u32 rowb = (w * 3 + 3) & ~3u;
                u32 datasz = rowb * h;
                u8 hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54;
                *(u32*)(hdr+14) = 40; *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
                *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
                fwrite(hdr, 1, 54, f);
                u8* line = (u8*)malloc(rowb);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = (const u8*)mp + (u64)y * s_rtsave_pitch;
                    memset(line, 0, rowb);
                    for (u32 x = 0; x < w; x++) {
                        float rv, gv, bv;
                        if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                            /* crude half->float: sign|exp|mant */
                            float v[3];
                            for (int c2 = 0; c2 < 3; c2++) {
                                u16 hv = hp16[c2];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                float fv;
                                if (exp == 0) fv = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&fv, &fb, 4); }
                                v[c2] = fv;
                            }
                            rv = v[0]; gv = v[1]; bv = v[2];
                        } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                            const float* fp32 = (const float*)(srow + (u64)x * 16);
                            rv = fp32[0]; gv = fp32[1]; bv = fp32[2];
                        } else {
                            const u8* p8 = srow + (u64)x * 4;
                            rv = p8[0] / 255.0f; gv = p8[1] / 255.0f; bv = p8[2] / 255.0f;
                        }
                        /* RTT_SAVEA=1: show the alpha lane in RED (gates
                         * like wave's mask.w live there). */
                        if (getenv("RTT_SAVEA")) {
                            float av;
                            if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                                u16 hv = hp16[3];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                if (exp == 0) av = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&av, &fb, 4); }
                            } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                                av = ((const float*)(srow + (u64)x * 16))[3];
                            } else {
                                av = (srow + (u64)x * 4)[3] / 255.0f;
                            }
                            rv = av;
                        }
                        /* |v| tonemap so signed heights are visible */
                        float ar = rv < 0 ? -rv : rv, ag = gv < 0 ? -gv : gv, ab = bv < 0 ? -bv : bv;
                        if (ar > 1) ar = 1; if (ag > 1) ag = 1; if (ab > 1) ab = 1;
                        line[x*3+0] = (u8)(ab * 255.0f);
                        line[x*3+1] = (u8)(ag * 255.0f);
                        line[x*3+2] = (u8)(ar * 255.0f);
                    }
                    fwrite(line, 1, rowb, f);
                }
                free(line);
                fclose(f);
                printf("[D3D12] wrote rt_save.bmp (%ux%u dxgi=%u)\n", w, h, s_rtsave_dxgi);
            }
            s_rtsave_buf->lpVtbl->Unmap(s_rtsave_buf, 0, NULL);
        }
        s_rtsave_state = 0;
    }

    /* rsxcap: snapshot whole frames (backbuffer + every offscreen RT + a draw
     * manifest) at RSX_CAP=start[:count[:stride]] frame_counts. Runs before the
     * present transition would matter -- the backbuffer is in PRESENT here and
     * is restored to PRESENT after each readback. */
    /* Resistance port: live capture. Creating logs\cap.now while the game
     * runs snapshots the next 3 presented frames to rsxcap/frame_50.. (the
     * trigger file is renamed to cap.done). Checked once a second. */
    { static ULONGLONG lastchk = 0; static u32 live_left = 0, live_idx = 0;
      ULONGLONG now = GetTickCount64();
      /* Resistance port: F12 also captures (checked every frame -- a death
       * screen lasts a few seconds). */
      /* v165f diag: Ctrl+F10 stalls the render thread 800 ms (what a long shader
       * compile does), to test whether a hitch alone drops the player through the floor. */
      { static int was = 0; const int dn = (GetAsyncKeyState(VK_F10) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        if (dn && !was) { fprintf(stderr, "[hitch] Ctrl+F10: render thread stall 800 ms%c", 10); Sleep(800); }
        was = dn; }
      /* v179b: RSX_CAP_ONSPACE=<n>: capture n frames when Space is pressed (the
       * intro skip) -- the front end's intro is not a cellVdec movie. Once. */
      { static int n = -1, was = 0, done = 0;
        if (n < 0) { const char* e = getenv("RSX_CAP_ONSPACE"); n = e ? atoi(e) : 0; }
        const int dn = ((GetAsyncKeyState(VK_SPACE) | GetAsyncKeyState(VK_RETURN)) & 0x8000) != 0;   /* Enter skips the boot intro */
        if (n > 0 && dn && !was && !done && !live_left) { live_left = (u32)n; done = 1;
            { extern int g_drawlog_left; const char* e2 = getenv("RSX_DRAWLOG_ONSPACE"); if (e2) g_drawlog_left = atoi(e2); }
            fprintf(stderr, "[RSXCAP] Space/Enter: capturing %d frames%c", n, 10); }
        was = dn; }
      { extern volatile int g_rsx_cap_request;   /* v179: cellGcmSys, a movie just ended */
        if (!live_left && g_rsx_cap_request > 0) { live_left = (u32)g_rsx_cap_request; g_rsx_cap_request = 0;
            fprintf(stderr, "[RSXCAP] movie ended: capturing %u frames%c", live_left, 10); } }
      if (!live_left && (GetAsyncKeyState(VK_F12) & 0x8000) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
          live_left = 3;
          fprintf(stderr, "[RSXCAP] F12 capture requested%c", 10);
      }
      if (!live_left && now - lastchk > 1000) {
          lastchk = now;
          if (GetFileAttributesA("logs\\cap.now") != INVALID_FILE_ATTRIBUTES) {
              live_left = 3;
              MoveFileExA("logs\\cap.now", "logs\\cap.done", MOVEFILE_REPLACE_EXISTING);
              fprintf(stderr, "[RSXCAP] live capture requested%c", 10);
          }
      }
      if (live_left && s_present_this_frame) {
          rsx_capture_frame(fi, s_dbg_last_draws, 50 + live_idx++);
          live_left--;
          s_cap_batch_n = 0;
      } else if (live_left && s_in_midflush) {
          s_cap_manifest_only = 1;
          rsx_capture_frame(fi, s_dbg_last_draws, 50 + live_idx);
          s_cap_manifest_only = 0;
          s_cap_batch_n++;
      } }
    { const char* cap = getenv("RSX_CAP");
      if (cap) {
        static int s_cap_parsed = 0;
        static u32 s_cap_start = 0, s_cap_count = 1, s_cap_stride = 1, s_cap_done = 0;
        if (!s_cap_parsed) {
            s_cap_parsed = 1;
            u32 a = 0, b = 1, c = 1;
            int n = sscanf(cap, "%u:%u:%u", &a, &b, &c);
            s_cap_start  = (n >= 1 && a) ? a : 200;
            s_cap_count  = (n >= 2 && b) ? b : 1;
            s_cap_stride = (n >= 3 && c) ? c : 1;
        }
        if (s_cap_done < s_cap_count) {
            u32 target = s_cap_start + s_cap_done * s_cap_stride;
            /* RSX_CAP_MINDRAWS=<n>: only frames with at least n draws count
             * (skip loading-screen frames and catch the first real scene). */
            static int mind = -1;
            if (mind < 0) { const char* e = getenv("RSX_CAP_MINDRAWS"); mind = e ? atoi(e) : 1; }
            if (s_d3d.frame_count >= target && (int)s_dbg_last_draws >= mind) {
                rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done);
                s_cap_done++;
            }
        }
      } }

    if (s_present_this_frame && !s_in_midflush) fsr_frame_end();   /* v165 */
    /* v173 diag: Ctrl+F12 logs a one-line summary of each of the next 900
     * presented frames to logs\framelog.txt (draw counts per target, distinct
     * fragment programs, a hash of the backbuffer draws), to catch a flicker
     * that lasts a few frames without stopping the game. */
    { static int left = 0, was = 0; static FILE* f = NULL;
      const int dn = (GetAsyncKeyState(VK_F12) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
      if (dn && !was && !left) { left = 900; f = fopen("logs\\framelog.txt", "w");
          fprintf(stderr, "[framelog] logging 900 frames%c", 10); }
      was = dn;
      if (left && f && s_present_this_frame) {
          u32 nbb = 0, noff = 0, ncl = 0; u64 hbb = 1469598103934665603ull, hall = hbb;
          u32 offs[8] = {0}, ocnt[8] = {0}; int no = 0;
          for (u32 d = 0; d < s_dbg_last_draws && d < MAX_DRAWS; d++) {
              const D3D12DrawRecord* dr = &T_DRAWS[d];
              if (dr->is_clear) { ncl++; continue; }
              if (!dr->is_vp) continue;
              const u64 k = ((u64)dr->fp_addr << 32) ^ dr->tex[0].raw ^ ((u64)dr->blend_key << 7) ^ dr->vertex_count ^ ((u64)dr->ib_count << 20);
              hall = (hall ^ k) * 1099511628211ull;
              if (!dr->rt_off) { nbb++; hbb = (hbb ^ k) * 1099511628211ull; }
              else { noff++; int q = 0; for (; q < no; q++) if (offs[q] == dr->rt_off) break;
                     if (q == no && no < 8) { offs[no] = dr->rt_off; ocnt[no] = 0; no++; }
                     if (q < 8) ocnt[q]++; }
          }
          fprintf(f, "%llu draws=%u clears=%u bb=%u off=%u bbhash=%016llX allhash=%016llX midflush=%d |",
                  (unsigned long long)GetTickCount64(), s_dbg_last_draws, ncl, nbb, noff,
                  (unsigned long long)hbb, (unsigned long long)hall, s_batch_cont);
          for (int q = 0; q < no; q++) fprintf(f, " %08X:%u", offs[q], ocnt[q]);
          fputc(10, f);
          /* v173b: when the backbuffer draw list changes, list it (and the previous one) */
          { static char prevl[8192]; static u64 prevh = 0; char cur[8192]; int cn = 0; cur[0] = 0;
            for (u32 d = 0; d < s_dbg_last_draws && d < MAX_DRAWS && cn < 8000; d++) {
                const D3D12DrawRecord* dr = &T_DRAWS[d];
                if (dr->is_clear || !dr->is_vp || dr->rt_off) continue;
                cn += snprintf(cur + cn, sizeof cur - cn, "   fp=%08X t0=%08X t1=%08X n=%u/%u vp=%u,%u %ux%u blend=%X cm=%X at=%X\n",
                               dr->fp_addr, dr->tex[0].raw, dr->tex[1].raw, dr->vertex_count, dr->ib_count,
                               dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, dr->blend_key, dr->cmask, dr->alpha_ctl);
            }
            if (prevh && hbb != prevh) fprintf(f, "  -- previous backbuffer list:\n%s  -- this frame:\n%s", prevl, cur);
            memcpy(prevl, cur, (size_t)cn + 1); prevh = hbb; }
          if (--left == 0) { fclose(f); f = NULL; fprintf(stderr, "[framelog] done%c", 10); }
      } }
    /* Present */
    if (s_present_this_frame)
    { /* Resistance port: RSX_VSYNC (default 0). With vsync the present blocks
       * the FIFO thread until the next vblank -- up to 16 ms a frame -- and the
       * guest waits for the FIFO at the end of every frame, so the whole game
       * paid it. The game runs far below 60 fps here, so vsync bought nothing
       * but that stall. RSX_VSYNC=1 restores it. */
      static int vs = -1;
      /* v132: the value is the DXGI sync interval (1-4): on a 165 Hz screen
       * RSX_VSYNC=3 with RFOM_FPS=55 shows every frame for exactly 3 refreshes
       * (18.2 ms), where a free-running 60 alternates 2 and 3 (judder). */
      if (vs < 0) { const char* e = getenv("RSX_VSYNC"); vs = e ? atoi(e) : 0;
                    if (vs < 0) vs = 0; if (vs > 4) vs = 4;
                    if (vs) fprintf(stderr, "[D3D12] vsync on, sync interval %d%c", vs, 10); }
      double _pt = perf_on() ? perf_now() : 0.0;
      present_blit(vs);
      ft_note_present();
      if (perf_on()) s_perf_present += perf_now() - _pt; }   /* skipped for an offscreen-only batch */

    move_to_next_frame();

    s_d3d.frame_count++;

    /* RPCS3-style titlebar stats, refreshed once a second: presented FPS,
     * draw count of the last frame, and the backbuffer size. */
    {
        static ULONGLONG s_tt0 = 0;
        static u64 s_tframes = 0;
        if (s_present_this_frame) s_tframes++;   /* real presents, not mid-frame batches */
        ULONGLONG tnow = GetTickCount64();
        if (s_tt0 == 0) s_tt0 = tnow;
        if (tnow - s_tt0 >= 1000 && s_d3d.hwnd) {
            extern char g_rsx_title_base[128];
            char tb[400];
            snprintf(tb, sizeof(tb), "%s | FPS: %.2f%s | draws: %u | %ux%u",
                     g_rsx_title_base,
                     s_tframes * 1000.0 / (double)(tnow - s_tt0), s_ft_title,
                     s_dbg_last_draws, s_d3d.width, s_d3d.height);
            SetWindowTextA(s_d3d.hwnd, tb);
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
}

/* Resistance port: RSX_ASYNC (default 1). Recording a batch into D3D12
 * commands (prepass, PSOs, descriptors, ~2500 draws a York frame) took ~26 ms
 * of the FIFO thread per frame, and the FIFO thread IS the frame rate here --
 * the guest waits for it at the end of every frame. The batch is now handed to
 * a render thread and the FIFO thread goes straight back to parsing the next
 * one: draw lists are double-buffered (T_DRAWS), the upload halves alternate
 * by parity as before. One batch in flight: the FIFO waits only if the render
 * thread is still on the previous batch. RSX_ASYNC=0 = single-threaded. */
static HANDLE s_rt_go, s_rt_done;
static volatile LONG s_rt_busy;
static struct { D3D12DrawRecord* draws; u32 count; int parity, present, midflush, cont; } s_rt_job;
static int rsx_async(void)
{
    static int e = -1;
    if (e < 0) { const char* v = getenv("RSX_ASYNC"); e = v ? atoi(v) : 1; }
    return e;
}
/* Resistance port (v124): host thread ids of the RSX FIFO thread (the one that
 * calls render_frame) and the RSX_ASYNC render thread, for the debug console's
 * auto-profile. */
static volatile unsigned long s_rsx_fifo_tid = 0, s_rsx_render_tid = 0;
unsigned long rsx_fifo_host_tid(void) { return s_rsx_fifo_tid; }
int rsx_perf_render_load(void) { return s_perf_load_pct; }
unsigned long rsx_render_host_tid(void) { return s_rsx_render_tid; }
static DWORD WINAPI rt_main(LPVOID p)
{
    (void)p;
    t_render_thread = 1;
    s_rsx_render_tid = GetCurrentThreadId();
    for (;;) {
        WaitForSingleObject(s_rt_go, INFINITE);
        T_DRAWS = s_rt_job.draws; T_DRAWCNT = s_rt_job.count; T_PARITY = s_rt_job.parity;
        s_present_this_frame = s_rt_job.present; s_in_midflush = s_rt_job.midflush;
        s_batch_cont = s_rt_job.cont;
        render_frame_impl();
        InterlockedExchange(&s_rt_busy, 0);
        SetEvent(s_rt_done);
    }
    return 0;
}
/* Wait for the render thread to finish its batch. Keeps the window's message
 * queue serviced if this thread owns it (Present can need the window thread). */
static void rt_wait_idle(void)
{
    if (!s_rt_go) return;
    const int owner = s_d3d.hwnd && GetWindowThreadProcessId(s_d3d.hwnd, NULL) == GetCurrentThreadId();
    while (s_rt_busy) {
        if (owner) {
            MsgWaitForMultipleObjects(1, &s_rt_done, FALSE, 2, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        } else WaitForSingleObject(s_rt_done, 2);
    }
}
static void render_frame(void)
{
    /* v165b: FSR jitter phase / history frame advances once per presented frame
     * (d3d12_present is not the only path that presents a frame). */
    if (!t_render_thread && s_present_this_frame && !s_in_midflush) fsr_record_frame_end();
    if (!t_render_thread) { gf_finalize();   /* the open GPU-fetch record's vertices */
                            if (!s_rsx_fifo_tid) s_rsx_fifo_tid = GetCurrentThreadId(); }
    if (!rsx_async() || t_render_thread) { render_frame_impl(); return; }
    if (!s_rt_go) {
        s_rt_go = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_rt_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 4u << 20, rt_main, NULL, 0, NULL);
        fprintf(stderr, "[RSX_ASYNC] render thread started%c", 10);
    }
    vp_async_drain();          /* this batch's vertex conversions are all in */
    rt_wait_idle();            /* previous batch recorded and submitted */
    s_rt_job.draws = T_DRAWS; s_rt_job.count = T_DRAWCNT; s_rt_job.parity = T_PARITY;
    s_rt_job.present = s_present_this_frame; s_rt_job.midflush = s_in_midflush;
    s_rt_job.cont = s_batch_cont;
    /* The half we are about to refill was read by the batch BEFORE the one
     * being handed off; it was submitted (render thread idle), make sure the
     * GPU is done with it. */
    { const u32 prev = s_submit_no;   /* v181: that half's last user is batch prev-(NPAR-2) */
      if (prev > (u32)(NPAR - 2)) { const u32 b = prev - (u32)(NPAR - 2);
          const u64 v = s_batch_fence[b & 7]; if (v) fence_wait(v, "rsx_async"); } }
    InterlockedExchange(&s_rt_busy, 1);
    SetEvent(s_rt_go);
    /* Producer side starts the next batch in the other buffers. */
    T_DRAWS = (T_DRAWS == s_draws_buf[0]) ? s_draws_buf[1] : s_draws_buf[0];
    T_DRAWCNT = 0;
    s_d3d.vb_offset = 0; s_d3d.vp_vb_offset = 0; s_vp_ib_off = 0; s_gf_off = 0; s_gf_gen++;
    T_PARITY = (T_PARITY + 1) % NPAR;
    if (!s_in_midflush) s_batch_cont = 0;   /* as render_frame_impl does for the frame's last batch */
}

/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static u32 s_dbg_clears_since_present = 0;   /* CELLMARK_BLINKDBG */
static u32 s_clear_presents = 0;   /* presents issued at clear (frame boundary) */

/* FRAME_BUDGET=1: how much geometry a guest frame actually asks for, versus
 * what the per-frame vertex buffer can hold. A frame that overflows is silently
 * truncated -- the tail of the scene never renders -- which reads as "the object
 * is missing" rather than "the batch is too small". */

static int blink_dbg(void)
{
    static int v = -1;
    if (v < 0) v = getenv("CELLMARK_BLINKDBG") ? 1 : 0;
    return v;
}

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u\n",
               T_DRAWCNT, s_dbg_clears_since_present);
    s_dbg_clears_since_present = 0;

    /* An accumulated batch with draws but NONE targeting a display buffer is
     * offscreen pass work only (demosaic flips once per effect pass): showing
     * it would strobe the bare backbuffer clear. Keep accumulating -- the
     * composite draw that targets the display presents the whole chain, in
     * order, in one command list. Empty batches still present (boot/idle). */
    int flip_has_display = (T_DRAWCNT == 0);
    for (u32 _i = 0; _i < T_DRAWCNT && _i < MAX_DRAWS; _i++)
        if (!T_DRAWS[_i].is_clear && T_DRAWS[_i].rt_off == 0) {
            flip_has_display = 1;
            break;
        }

    if (s_d3d.initialized && flip_has_display) {
        InterlockedIncrement(&s_rfr[RFR_PRESENT]);
        render_frame();
    }

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    gf_finalize();
    (void)ud;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    float cc[4];
    cc[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    cc[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    cc[2] = (color & 0xFF) / 255.0f;          /* B */
    cc[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
    /* CLEAR_RGB=r,g,b: override the guest clear colour. Black holes (geometry
     * that never rasterized) and black pixels a shader really wrote look
     * identical against a black clear; this separates them in one run. */
    { static int _cinit = 0; static float _co[3]; static int _con = 0;
      if (!_cinit) { _cinit = 1; const char* e = getenv("CLEAR_RGB");
          if (e && sscanf(e, "%f,%f,%f", &_co[0], &_co[1], &_co[2]) == 3) _con = 1; }
      if (_con) { cc[0] = _co[0]; cc[1] = _co[1]; cc[2] = _co[2]; } }

    u32 rt_w = 0, rt_h = 0, mrt[3] = {0, 0, 0};
    u32 rt = current_rt_off(&rt_w, &rt_h, mrt);

    /* An OFFSCREEN clear is just an ordered op in the current frame's pass
     * chain (demosaic clears each effect pass's surface) -- record it, don't
     * touch the frame boundary. So is a clear with no colour bits, wherever
     * it lands: God of War HD clears depth (mask 0x1/0x3) between compositing
     * its scene into the final surface and drawing the HUD over it, and
     * clearing colour there wiped the scene -- the menu came out black. It is
     * not a frame boundary either. */
    if (rt != 0 || !(flags & 0xF0u)) {
        if (T_DRAWCNT < MAX_DRAWS) {
            D3D12DrawRecord* dr = &T_DRAWS[T_DRAWCNT++];
            memset(dr, 0, sizeof(*dr));
            dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
            dr->rt_off = rt; memcpy(dr->rt_mrt, mrt, sizeof(mrt));
            dr->disp_tag = rt ? 0u : current_disp_tag();
            dr->zeta_off = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_zeta_offset : 0;
            dr->rt_w = rt_w; dr->rt_h = rt_h;
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            memcpy(dr->cc, cc, sizeof(cc));
            dr->clear_mask = flags;
            dr->clear_z = depth;
            dr->clear_s = stencil;
        }
        return;
    }

    memcpy(s_d3d.clear_color, cc, sizeof(cc));

    /* A DISPLAY clear marks the start of a new visible frame. If a completed
     * frame is still accumulated (the drain gulped across a frame boundary --
     * guaranteed at the FIFO ring wrap, where rest-of-frame-N + clear-N+1
     * arrive in one batch), PRESENT it now instead of discarding it. Only a
     * batch that actually contains DISPLAY draws is a completed frame: a
     * batch of offscreen pass work (render-to-texture) must keep accumulating
     * until its composite draw arrives, or the screen strobes intermediates. */
    int have_display_draws = 0;
    for (u32 i = 0; i < T_DRAWCNT && i < MAX_DRAWS; i++)
        if (!T_DRAWS[i].is_clear && T_DRAWS[i].rt_off == 0) {
            have_display_draws = 1;
            break;
        }
    if (!have_display_draws)
        return;   /* keep accumulating the in-progress frame */

    /* A title that DOUBLE-BUFFERS the display (DeferredShading clears both
     * 0x0 and 0x440000 per frame, plus a HUD pass) issues several display
     * clears per real frame -- treating each as a boundary presented the
     * 1-2-draw intermediates, strobing black between the full 146-draw
     * frames. Anchor the boundary to the FLIP instead: only present once a
     * cellGcmSetFlip has landed since the last present. Titles that flip once
     * per frame (wave/cellmark/gcmcube) are unaffected -- their single
     * display clear still follows their single flip. Fall back to the old
     * clear-only heuristic for titles that never flip (fc stays 0). */
    static u32 s_frame_draws_max = 0;   /* running max frame size (typical full frame) */
    if (T_DRAWCNT > s_frame_draws_max) s_frame_draws_max = T_DRAWCNT;
    {
        extern unsigned cellGcm_flip_request_count(void);
        static unsigned s_last_present_flip = 0;
        unsigned fc = cellGcm_flip_request_count();
        if (fc != 0) {
            if (fc == s_last_present_flip)
                return;   /* no flip since last present -> not a real boundary */
            /* A double-buffered title issues several display clears per flip
             * (DeferredShading: clear back-buffer, HUD pass, ...). They can
             * arrive in either order, so a small batch here may be an
             * intermediate that precedes the full frame's clear. Don't present
             * (or consume the flip) until the batch is a substantial fraction
             * of a full frame -- keep accumulating so the complete frame lands
             * in one present. Gauged against the running MAX frame size (not
             * the last, which a leaked tiny batch would poison), so it
             * self-scales per title with no fixed threshold. */
            if (s_frame_draws_max > 16 && T_DRAWCNT < s_frame_draws_max / 4)
                return;   /* intermediate: keep accumulating, flip stays pending */
            s_last_present_flip = fc;
        }
    }

    /* RSX_ACCUM_FRAME=1: never present at a clear boundary, so every draw in a
     * guest frame accumulates into one image. A title that clears several times
     * per frame (render-to-texture passes) otherwise gets presented mid-scene,
     * and each captured frame holds only a slice of the geometry -- the floor in
     * one, a wall in the next -- which makes a single model impossible to see. */
    { static int accum = -1;
      if (accum < 0) { const char* e = getenv("RSX_ACCUM_FRAME"); accum = e ? atoi(e) : 0; }
      if (accum) return; }
    if (s_d3d.initialized) {
        if (blink_dbg())
            printf("[CLEAR] presenting %u accumulated draws at frame boundary\n",
                   T_DRAWCNT);
        InterlockedIncrement(&s_rfr[RFR_CLEAR]);
        render_frame();
        s_clear_presents++;
    }
    s_dbg_clears_since_present++;
    vp_async_drain();
    T_DRAWCNT   = 0;
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0; s_vp_ib_off = 0; s_gf_off = 0; s_gf_gen++;
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;


/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + cellGcmResolveOffset(pos->offset + vindex * pos->stride);
        out->x = rsx_rd_bef(src);
        out->y = rsx_rd_bef(src + 4);
        if (pos->size >= 3) out->z = rsx_rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rsx_rd_bef(src + 8);   /* U */
                out->v = rsx_rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + cellGcmResolveOffset(col->offset + vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
typedef struct { float v[4]; } VPSlot;
#define VP_VERT_STRIDE (16 * sizeof(VPSlot))   /* 256 */


void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n);

/* Fill all 16 attribute slots for one vertex. The per-attribute work lives in
 * rsx_vertex_fetch.c so the Metal and null backends read guest vertices the
 * same way this one does, rather than each porting the logic again. */
/* Resistance port: per-draw vertex fetch plan. rsx_fetch_attrib re-derives
 * everything (constant register, enable, divisor, DMA resolve, type switch) for
 * each of 16 attributes of every index -- 15% of the render thread in York.
 * vp_fast_prep() resolves each attribute once per draw; LOCAL per-vertex arrays
 * then read straight from a host pointer. MAIN-memory (IO-mapped), instanced
 * and immediate-mode attributes keep the full rsx_fetch_attrib path.
 * VP_FASTFETCH=0 disables. */
typedef struct { const u8* p0; u32 stride, type, n, mode;
                 u32 off0, page; const u8* pbase; } VPFastAttr;  /* mode 0 const, 1 local, 2 slow, 3 main */
static VPFastAttr s_vfa[16];
static VPSlot s_vcst[16];
static int s_vfa_ok = 0;
static void vp_fast_prep(const rsx_state* st)
{
    extern uint8_t* vm_base;
    extern const float (*g_rsx_imm_verts)[RSX_MAX_VERTEX_ATTRIBS][4];
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    static int en = -1;
    if (en < 0) { const char* e = getenv("VP_FASTFETCH"); en = e ? atoi(e) : 1;
                  if (getenv("VTX_POS")) en = 0; }
    s_vfa_ok = 0;
    if (!en || !st || !vm_base || g_rsx_imm_verts) return;
    /* Resistance port: skip the attributes this draw's vertex program never
     * reads -- no decode and no 16-byte write into the upload heap for them.
     * York's programs read ~4-6 of 16, so most of the 256-byte slot was
     * converted and streamed for nothing. VP_INMASK=0 off. */
    u32 inmask = 0xFFFFu;
    { static int im = -1; if (im < 0) { const char* e = getenv("VP_INMASK"); im = e ? atoi(e) : 1; }
      if (im) { int vi = vp_get_vs(st); if (vi >= 0 && vi < s_d3d.vp_vs_n) inmask = s_d3d.vp_vs[vi].inmask; } }
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        memcpy(s_vcst[i].v, st->vertex_data4f[i], 16);
        VPFastAttr* f = &s_vfa[i];
        if (!(inmask & (1u << i))) { f->mode = 4; continue; }
        if (!a->enabled || a->stride == 0) { f->mode = 0; continue; }
        if (a->frequency > 1 || a->type < 1 || a->type > 7) { f->mode = 2; continue; }
        f->stride = a->stride; f->type = a->type;
        f->n = a->size ? a->size : 4; if (f->n > 4) f->n = 4;
        if (a->offset & 0x80000000u) {
            /* MAIN (IO-mapped): the IO table maps 1MB pages, so resolve once
             * per page instead of once per attribute per vertex. */
            f->mode = 3; f->page = 0xFFFFFFFFu; f->pbase = NULL;
            f->off0 = (a->offset & 0x7FFFFFFFu) + st->vertex_data_base_offset;
            continue;
        }
        f->mode = 1;
        /* LOCAL wraps at 256MB -- see rsx_fetch_attrib. If the array itself
         * would straddle the wrap, take the exact path. */
        { u32 lo = ((a->offset & 0x7FFFFFFFu) + st->vertex_data_base_offset) & 0x0FFFFFFFu;
          if (lo + (st->vertex_data_base_index + 0x100000u) * (u64)a->stride > 0x10000000ull)
              { f->mode = 2; continue; }
          f->p0 = vm_base + cellGcmResolveLocated(1, lo); }
        f->stride = a->stride; f->type = a->type;
        f->n = a->size ? a->size : 4; if (f->n > 4) f->n = 4;
    }
    s_vfa_ok = 1;
}

static void vp_fast_vertex_tb(const rsx_state* st, u32 base, u32 vi, VPSlot* o, VPFastAttr* tab)
{
    u32 ei = (vi + base) & 0x000FFFFFu;   /* st is read only by mode 2 (never on the async path) */
    for (int i = 0; i < 16; i++) {
        const VPFastAttr* f = &tab[i];
        float* out = o[i].v;
        if (f->mode == 4) continue;   /* not read by the vertex program */
        if (f->mode == 2) { rsx_fetch_attrib(st, i, vi, out); continue; }
        memcpy(out, s_vcst[i].v, 16);
        if (f->mode == 0) continue;
        const u8* p;
        u8 tmp[16];
        if (f->mode == 3) {
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int local, u32 offset);
            VPFastAttr* fm = &tab[i];
            u32 off = f->off0 + ei * f->stride;
            if ((off >> 20) != fm->page) {
                fm->page = off >> 20;
                fm->pbase = vm_base + cellGcmResolveLocated(0, off & ~0xFFFFFu);
            }
            p = fm->pbase + (off & 0xFFFFFu);
            /* An element straddling a page boundary: gather its bytes page by
             * page (no live state needed, so the async path can take it). */
            if ((off & 0xFFFFFu) + 16u > 0x100000u) {
                for (u32 k = 0; k < 16; k++) {
                    u32 o2 = off + k;
                    u32 r2 = cellGcmResolveLocated(0, o2 & ~0xFFFFFu);
                    tmp[k] = (r2 == 0xFFFFFFFFu) ? 0 : vm_base[r2 + (o2 & 0xFFFFFu)];
                }
                p = tmp;
            }
        } else p = f->p0 + (u64)ei * f->stride;
        u32 n = f->n;
        switch (f->type) {
        case 2: for (u32 k = 0; k < n; k++) out[k] = rsx_rd_bef(p + k * 4); break;
        case 3: for (u32 k = 0; k < n; k++) out[k] = rsx_rd_half_be(p + k * 2); break;
        case 4: for (u32 k = 0; k < n; k++) out[k] = p[k] * (1.0f / 255.0f); break;
        case 1: for (u32 k = 0; k < n; k++)
                    out[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]) / 32767.0f;
                break;
        case 5: for (u32 k = 0; k < n; k++)
                    out[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]);
                break;
        case 6: { u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
                  int x = (int)(w << 21) >> 21, y = (int)(w << 10) >> 21, z = (int)w >> 22;
                  out[0] = (float)x / 1023.0f; out[1] = (float)y / 1023.0f;
                  out[2] = (float)z / 511.0f; out[3] = 1.0f; }
                break;
        case 7: for (u32 k = 0; k < n; k++) out[k] = (float)p[k]; break;
        }
    }
}
static void vp_fast_vertex_t(const rsx_state* st, u32 vi, VPSlot* o, VPFastAttr* tab)
{ vp_fast_vertex_tb(st, st->vertex_data_base_index, vi, o, tab); }
static void vp_fast_vertex(const rsx_state* st, u32 vi, VPSlot* o) { vp_fast_vertex_t(st, vi, o, s_vfa); }

/* Resistance port: parallel vertex conversion. The render thread was ~50%
 * vertex decode in York's busy views while 3+ cores idled. Large indexed draws
 * hand their unique-vertex list to a small worker pool; each worker keeps its
 * own copy of the per-draw attribute plan (the MAIN-memory page cache in it is
 * per-thread state). VP_THREADS=<n> (default 4, 0 = off). */
static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16);
typedef struct {
    const rsx_state* st; const u32* uniq; u32 n, lo; VPSlot* out; int fast;
    VPFastAttr fa[16];
    volatile LONG next, active;
} VPConvJob;
static VPConvJob s_vjob;
static volatile LONG s_vjob_gen = 0;
static int s_vpool_n = -1;
static SRWLOCK s_vpool_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_vpool_cv = CONDITION_VARIABLE_INIT;
static void vp_conv_run(VPConvJob* j)
{
    VPFastAttr fa[16]; memcpy(fa, j->fa, sizeof fa);
    for (;;) {
        LONG b = InterlockedExchangeAdd(&j->next, 512);
        if ((u32)b >= j->n) break;
        u32 e = (u32)b + 512u; if (e > j->n) e = j->n;
        for (u32 k = (u32)b; k < e; k++) {
            u32 r = j->uniq[k];
            if (j->fast) vp_fast_vertex_t(j->st, j->lo + r, &j->out[(size_t)r * 16], fa);
            else         read_vp_vertex(j->st, j->lo + r, &j->out[(size_t)r * 16]);
        }
    }
}
static DWORD WINAPI vp_conv_worker(LPVOID p)
{
    (void)p; LONG seen = 0;
    for (;;) {
        AcquireSRWLockShared(&s_vpool_lock);
        while (s_vjob_gen == seen) SleepConditionVariableSRW(&s_vpool_cv, &s_vpool_lock, INFINITE, CONDITION_VARIABLE_LOCKMODE_SHARED);
        seen = s_vjob_gen;
        ReleaseSRWLockShared(&s_vpool_lock);
        vp_conv_run(&s_vjob);
        InterlockedDecrement(&s_vjob.active);
    }
    return 0;
}
/* Resistance port: ASYNC vertex conversion. The render thread converted each
 * indexed draw's unique vertices itself (~45% of its time) before parsing the
 * next FIFO command, and only draws of 1024+ unique vertices used the pool.
 * The output slots are reserved at record time, so the conversion can finish
 * any time before the batch is submitted: queue it, let workers run it while
 * the FIFO keeps flowing, and drain at render_frame. Only draws whose
 * attributes all take the fast path (no per-vertex rsx_fetch_attrib, which
 * reads live state) go async. VP_ASYNC=<threads> (default 4, 0 = off). */
typedef struct { VPFastAttr fa[16]; u32 base; u32* uniq; u32 n, lo; VPSlot* out; } VPAJob;
#define VPA_CAP 16384
static VPAJob s_vpa[VPA_CAP];
static volatile LONG s_vpa_head = 0, s_vpa_tail = 0, s_vpa_out = 0;
static SRWLOCK s_vpa_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_vpa_cv = CONDITION_VARIABLE_INIT;
static int s_vpa_n = -1;
static void vp_fast_vertex_b(u32 base, u32 vi, VPSlot* o, VPFastAttr* tab)
{
    vp_fast_vertex_tb(NULL, base, vi, o, tab);
}
static int vpa_pop_run(void)
{
    VPAJob j;
    AcquireSRWLockExclusive(&s_vpa_lock);
    if (s_vpa_tail == s_vpa_head) { ReleaseSRWLockExclusive(&s_vpa_lock); return 0; }
    j = s_vpa[s_vpa_tail % VPA_CAP]; s_vpa_tail++;
    ReleaseSRWLockExclusive(&s_vpa_lock);
    for (u32 k = 0; k < j.n; k++) {
        u32 r = j.uniq[k];
        vp_fast_vertex_b(j.base, j.lo + r, &j.out[(size_t)r * 16], j.fa);
    }
    free(j.uniq);
    InterlockedDecrement(&s_vpa_out);
    return 1;
}
static DWORD WINAPI vpa_worker(LPVOID p)
{
    (void)p;
    for (;;) {
        AcquireSRWLockExclusive(&s_vpa_lock);
        while (s_vpa_tail == s_vpa_head)
            SleepConditionVariableSRW(&s_vpa_cv, &s_vpa_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&s_vpa_lock);
        while (vpa_pop_run()) {}
    }
    return 0;
}
/* Wait for every queued conversion (the render thread helps). */
static void vp_async_drain(void)
{
    if (s_vpa_out <= 0) return;
    double _t = perf_on() ? perf_now() : 0.0;
    while (s_vpa_out > 0) { if (!vpa_pop_run()) YieldProcessor(); }
    if (perf_on()) s_perf_vdrain += perf_now() - _t;
}
/* v180: called by the FIFO walker right before it writes a label/semaphore
 * the PPU can see. Anything the backend still has to READ from guest memory
 * for draws already parsed (the open GPU-fetch record's vertex copy, queued
 * CPU vertex conversions) must happen first: once the label lands, the game
 * may reuse those buffers for the next frame. The menu writes its quads into
 * one small ring every frame; when a frame inserts a quad, every later slot
 * shifts, and the late reads picked up the NEXT frame's (shifted) vertices --
 * the one-frame "static" right after the boot intro. RSX_LABEL_FLUSH=0 off. */
void rsx_backend_flush_reads(void)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_LABEL_FLUSH"); en = e ? atoi(e) : 1; }
    if (!en || t_render_thread) return;
    gf_finalize();
    vp_async_drain();
}
static int vp_async_try(const rsx_state* st, const u32* uniq, u32 n, u32 lo, VPSlot* out)
{
    if (s_vpa_n < 0) {
        const char* e = getenv("VP_ASYNC"); s_vpa_n = e ? atoi(e) : 4;
        if (s_vpa_n > 8) s_vpa_n = 8;
        for (int i = 0; i < s_vpa_n; i++) CreateThread(NULL, 0, vpa_worker, NULL, 0, NULL);
    }
    if (s_vpa_n <= 0 || !s_vfa_ok || !n) return 0;
    for (int i = 0; i < 16; i++) if (s_vfa[i].mode == 2) return 0;
    const u32 CH = 2048;
    for (u32 b = 0; b < n; b += CH) {
        u32 m = n - b < CH ? n - b : CH;
        u32* cp = (u32*)malloc((size_t)m * 4);
        if (!cp) {   /* out of memory: convert the rest here */
            for (u32 k = b; k < n; k++)
                vp_fast_vertex_b(st->vertex_data_base_index, lo + uniq[k], &out[(size_t)uniq[k] * 16], s_vfa);
            return 1;
        }
        memcpy(cp, uniq + b, (size_t)m * 4);
        for (;;) {   /* queue full: help until there is room */
            AcquireSRWLockExclusive(&s_vpa_lock);
            if (s_vpa_head - s_vpa_tail < VPA_CAP) break;
            ReleaseSRWLockExclusive(&s_vpa_lock);
            vpa_pop_run();
        }
        VPAJob* j = &s_vpa[s_vpa_head % VPA_CAP];
        memcpy(j->fa, s_vfa, sizeof s_vfa);
        j->base = st->vertex_data_base_index; j->uniq = cp; j->n = m; j->lo = lo; j->out = out;
        InterlockedIncrement(&s_vpa_out);
        s_vpa_head++;
        ReleaseSRWLockExclusive(&s_vpa_lock);
        WakeConditionVariable(&s_vpa_cv);
    }
    return 1;
}
/* Convert uniq[0..n) (vertex = lo + uniq[k], written at out[uniq[k]*16]). */
static void vp_conv_unique(const rsx_state* st, const u32* uniq, u32 n, u32 lo, VPSlot* out)
{
    if (vp_async_try(st, uniq, n, lo, out)) return;
    if (s_vpool_n < 0) {
        const char* e = getenv("VP_THREADS"); s_vpool_n = e ? atoi(e) : 4;
        if (s_vpool_n > 8) s_vpool_n = 8;
        for (int i = 0; i < s_vpool_n; i++) CreateThread(NULL, 0, vp_conv_worker, NULL, 0, NULL);
    }
    if (s_vpool_n <= 0 || n < 1024) {
        for (u32 k = 0; k < n; k++) read_vp_vertex(st, lo + uniq[k], &out[(size_t)uniq[k] * 16]);
        return;
    }
    s_vjob.st = st; s_vjob.uniq = uniq; s_vjob.n = n; s_vjob.lo = lo; s_vjob.out = out;
    s_vjob.fast = s_vfa_ok; memcpy(s_vjob.fa, s_vfa, sizeof s_vfa);
    s_vjob.next = 0; s_vjob.active = s_vpool_n;
    AcquireSRWLockExclusive(&s_vpool_lock);
    InterlockedIncrement(&s_vjob_gen);
    ReleaseSRWLockExclusive(&s_vpool_lock);
    WakeAllConditionVariable(&s_vpool_cv);
    vp_conv_run(&s_vjob);
    while (s_vjob.active > 0) YieldProcessor();
}

static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (s_vfa_ok) { vp_fast_vertex(state, vi, out16); return; }
    for (int i = 0; i < 16; i++) {
        rsx_fetch_attrib(state, i, vi, out16[i].v);
        if (i == 0) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[0];
            u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
            rsx_vtx_pos_dbg(state, out16[0].v, n);
        }
    }
}

/* VTX_POS=<N>: print the first fetched position of the first N draws. A guest
 * vertex array that resolves to the wrong memory reads as garbage/denormals, and
 * that is indistinguishable from "the shader is wrong" without seeing the input. */
void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n)
{
    static int cap = -1, seen = 0;
    if (cap < 0) { const char* e = getenv("VTX_POS"); cap = e ? atoi(e) : 0; }
    if (!cap || seen >= cap) return;
    seen++;
    const rsx_vertex_attrib* a = &state->vertex_attribs[0];
    fprintf(stderr, "[VTXPOS] a0 off=0x%X stride=%u size=%u type=%u -> (%g, %g, %g, %g)\n",
            a->offset, a->stride, a->size, a->type,
            v[0], v[1], v[2], v[3]);   /* all four: masking by `size` hid whether w survived */
}

static void vp_attrs_dbg(const rsx_state* state)
{
    { static int va = -1; if (va < 0) va = getenv("VP_ATTRS") ? 1 : 0; if (!va) return; }
    /* VP_ATTRS_FP=<hex shader_program>: dump only the draws that use one
     * fragment program. Without it the first six draws are whatever the frame
     * happens to start with, which is never the mesh being investigated. */
    { static const char* e = (const char*)1; static u32 want = 0;
      if (e == (const char*)1) { e = getenv("VP_ATTRS_FP");
          want = e ? (u32)strtoul(e, NULL, 0) : 0; }
      if (want && (!state || state->shader_program != want)) return; }
    /* Dedupe by the set of enabled attributes rather than printing the first
     * few draws: one fragment program can be used by meshes with different
     * vertex layouts, and a mesh missing an attribute takes the constant
     * register instead -- which is exactly the case worth seeing. */
    { u32 mask = 0;
      for (int i = 0; i < 16; i++) if (state->vertex_attribs[i].enabled) mask |= 1u << i;
      static u32 seen[16]; static int ns = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == mask) return;
      if (ns >= 16) return;
      seen[ns++] = mask; }
    fprintf(stderr, "[VPATTR] --- fp=0x%X ---%c", state->shader_program, 10);
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
    /* VP_ATTRS_DUMP=<attr>:<n>: read n entries of that attribute straight out of
     * guest memory. Answers "did the guest write these values, or did our fetch
     * mangle them" without inferring it from the rendered image. */
    { const char* e = getenv("VP_ATTRS_DUMP"); if (!e) return;
      int ai = 0, cnt = 8; sscanf(e, "%d:%d", &ai, &cnt);
      const rsx_vertex_attrib* a = &state->vertex_attribs[ai];
      if (!a->enabled || !a->stride) return;
      extern uint8_t* vm_base;
      extern u32 cellGcmResolveLocated(int, u32);
      u32 nbad = 0, nzero = 0;
      for (int v = 0; v < cnt; v++) {
          u32 aoff = (a->offset & 0x7FFFFFFFu) + (u32)v * a->stride;
          const u8* q = vm_base + ((a->offset & 0x80000000u)
                        ? cellGcmResolveLocated(0, aoff) : cellGcmResolveLocated(1, aoff));
          /* Also read the same offset through the OTHER context-DMA. An array
           * the guest put in main memory but whose offset we resolve as LOCAL
           * reads as untouched VRAM, i.e. all zeros -- indistinguishable from
           * "the producer never ran". */
          if (v == 0) {
              u32 la = cellGcmResolveLocated(1, aoff), ma = cellGcmResolveLocated(0, aoff);
              const u8* lp = vm_base + la; const u8* mp = vm_base + ma;
              const u8* rp = vm_base + (a->offset & 0x7FFFFFFFu);   /* raw, unresolved */
              fprintf(stderr, "[VPDUMP] a%d off=0x%X  LOCAL@0x%X=(%g,%g,%g)  MAIN@0x%X=(%g,%g,%g)"
                              "  RAW@0x%X=(%g,%g,%g)%c",
                      ai, aoff, la, rsx_rd_bef(lp), rsx_rd_bef(lp+4), rsx_rd_bef(lp+8),
                      ma, rsx_rd_bef(mp), rsx_rd_bef(mp+4), rsx_rd_bef(mp+8),
                      (a->offset & 0x7FFFFFFFu), rsx_rd_bef(rp), rsx_rd_bef(rp+4), rsx_rd_bef(rp+8), 10);
          }
          u32 nc = a->size ? a->size : 3; if (nc > 3) nc = 3;
          float f[3] = {0,0,0};
          for (u32 k = 0; k < nc; k++) f[k] = rsx_rd_bef(q + k * 4);
          if (v < 12)
              fprintf(stderr, "[VPDUMP] a%d[%d] = (%g, %g, %g)%c", ai, v, f[0], f[1], f[2], 10);
          int allz = 1; for (u32 k = 0; k < nc; k++) if (f[k] != 0.f) allz = 0;
          if (allz) nzero++;
          else if (f[2] < -0.9f && f[0] > -0.1f && f[0] < 0.1f) nbad++;
      }
      fprintf(stderr, "[VPDUMP] a%d over %d verts: %u zero, %u near (0,0,-1)%c",
              ai, cnt, nzero, nbad, 10);
      /* How many entries differ from the first? An all-identical position array
       * is a degenerate mesh: it rasterizes nothing, which looks the same as
       * "the draw never happened". */
      { float f0[3]; u32 o0 = (a->offset & 0x7FFFFFFFu);
        const u8* q0 = vm_base + ((a->offset & 0x80000000u)
                       ? cellGcmResolveLocated(0, o0) : cellGcmResolveLocated(1, o0));
        for (int k = 0; k < 3; k++) f0[k] = rsx_rd_bef(q0 + k * 4);
        u32 diff = 0;
        for (int v = 1; v < cnt; v++) {
            u32 ao = o0 + (u32)v * a->stride;
            const u8* q = vm_base + ((a->offset & 0x80000000u)
                          ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
            for (int k = 0; k < 3; k++)
                if (rsx_rd_bef(q + k * 4) != f0[k]) { diff++; break; }
        }
        fprintf(stderr, "[VPDUMP] a%d: %u/%d entries differ from entry 0 (%g,%g,%g)%c",
                ai, diff, cnt, f0[0], f0[1], f0[2], 10);
        /* Extent: an isosurface with little fluid is a speck, and a speck that
         * rasterizes nothing is correct, not a bug. */
        { float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (f < lo[k]) lo[k] = f; if (f > hi[k]) hi[k] = f; } }
          u32 nnan = 0, nfin = 0;
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              int bad = 0;
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (!(f == f) || f > 1e30f || f < -1e30f) bad = 1; }
              if (bad) nnan++; else nfin++;
          }
          fprintf(stderr, "[VPDUMP] a%d extent x[%g..%g] y[%g..%g] z[%g..%g]"
                          "  finite=%u nan/inf=%u%c",
                  ai, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], nfin, nnan, 10); } } }
}


/* Resistance port: a VP draw needs SOME fetched attribute, not attribute 0 in
 * particular -- a vertex program may take its position from any input. The
 * old "attribute 0 enabled" test dropped such draws without a word. Logged
 * once per fragment program so a missing pass shows up. RSX_ATTR0_REQ=1
 * restores the old rule. */
static int vp_has_attrs(const rsx_state* st)
{
    static int req = -1;
    if (req < 0) { const char* e = getenv("RSX_ATTR0_REQ"); req = e ? atoi(e) : 0; }
    if (st->vertex_attribs[0].enabled) return 1;
    int any = 0;
    for (int i = 1; i < 16; i++) if (st->vertex_attribs[i].enabled) { any = 1; break; }
    { static u32 seen[64]; static int ns = 0; int f = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == st->shader_program) f = 1;
      if (!f && ns < 64) { seen[ns++] = st->shader_program;
          u32 m = 0; for (int i = 0; i < 16; i++) if (st->vertex_attribs[i].enabled) m |= 1u << i;
          printf("[ATTR0] fp=0x%08X draws with attribute 0 disabled (enabled mask 0x%04X)%s%c",
                 st->shader_program, m, (req || !any) ? " -- DROPPED" : "", 10); } }
    return req ? 0 : any;
}
static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    vp_fast_prep(state);
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!vp_has_attrs(state)) return 0;
    vp_attrs_dbg(state);
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)T_PARITY * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (PS3_LOG_FIRST(_n, 4)) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) { for (u32 k = 0; k < 4; k++)
                fprintf(f, "q%02d v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", _n, k,
                    c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                    c[k][8].v[0],c[k][8].v[1]);
              fclose(f); } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    vp_fast_prep(state);
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!vp_has_attrs(state)) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && PS3_LOG_FIRST(_n, 12))
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)T_PARITY * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, first + k, &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: vertex range for that program's draws. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (PS3_LOG_FIRST(_n, 12))
          fprintf(stderr, "[IDXDBG] tris first=%u count=%u a0off=0x%X stride=%u%c",
                  first, count, state->vertex_attribs[0].offset,
                  state->vertex_attribs[0].stride, 10); } }
    /* RSX_DBG_VTX=<hex tex0 offset>: dump attribute-0 positions for the draws that
     * bind that texture. The duck's texture resolves and its 9960 draws all
     * target the backbuffer, yet none of its texels reach the screen -- so the
     * question is whether its vertices are where they should be. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 6) { n++;
        fprintf(stderr, "[DUCKVTX] first=%u count=%u", first, count);
        for (u32 k = 0; k < count && k < 4; k++)
            fprintf(stderr, "  v%u=(%.2f,%.2f,%.2f)", k,
                    out[k*16].v[0], out[k*16].v[1], out[k*16].v[2]);
        { int lo = -1, hi = -1, nz = 0;
          for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
              const float* m = state->vertex_constants[r];
              if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
          }
          fprintf(stderr, "  | constants: %d non-zero, slots %d..%d", nz, lo, hi); }
        fprintf(stderr, "%c", 10);
      } }
    if (getenv("VTX_DUMP") && state->surface_color_offset[0] == 0xCC0000) {
        static int _n=0; if (PS3_LOG_FIRST(_n, 1)) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { fprintf(f, "G-buffer draw surf0=0x%X count=%u first=%u\n",
                         state->surface_color_offset[0], count, first);
          for (u32 k = 0; k < count && k < 30; k++)
            fprintf(f, "v%02u a0=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) a9=(%.4f,%.4f,%.4f,%.4f)\n", k,
                out[k*16].v[0],out[k*16].v[1],out[k*16].v[2],
                out[k*16+2].v[0],out[k*16+2].v[1],out[k*16+2].v[2],
                out[k*16+9].v[0],out[k*16+9].v[1],out[k*16+9].v[2],out[k*16+9].v[3]);
          fclose(f); } } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Non-indexed TRIANGLE_STRIP (fan=0) / TRIANGLE_FAN (fan=1) through the VP
 * path, expanded to a triangle list. LBP's Bink movie draws its YUV quad as a
 * non-indexed 4-vertex strip (prim 6); without this it fell to the fixed-
 * function fallback with is_vp=0 (no vertex-program transform -> the quad
 * collapsed to a line) and textured=0 (the Y/U/V planes never sampled). */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count, int fan)
{
    vp_fast_prep(state);
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!vp_has_attrs(state)) return 0;
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)T_PARITY * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        /* strip winding alternates per triangle to keep facing consistent */
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex(state, first + i0, &out[o*16]); o++;
        read_vp_vertex(state, first + i1, &out[o*16]); o++;
        read_vp_vertex(state, first + i2, &out[o*16]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    vp_fast_prep(state);
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!vp_has_attrs(state)) return 0;
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)T_PARITY * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, read_guest_index(state, first + q*4 + k), c[k]);
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (PS3_LOG_FIRST(_n, 6)) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Resistance port: indexed fetch with vertex reuse. A mesh's index list names
 * each vertex ~6 times; converting it once into a cached (non-write-combined)
 * scratch and copying the 256-byte result for each repeat is far cheaper than
 * 16 attribute decodes per index. Falls back to straight per-index fetch when
 * the index range is sparse. VP_IDXCACHE=0 disables. */
/* Returns 0 when it expanded one vertex per index into out[k*16]; otherwise it
 * wrote the UNIQUE vertices at out[(idx-lo)*16], queued `count` indices in the
 * index buffer (s_pend_ib_*), and returns the number of vertex slots used. */
static u32 vp_indexed_core(const rsx_state* st, const u32* idx, u32 count, VPSlot* out, u32 maxv);
/* Decode `count` guest indices starting at `first` into a reusable buffer. */
static u32* vp_decode_indices(const rsx_state* st, u32 first, u32 count)
{
    static u32* idx = NULL; static u32 idx_cap = 0;
    if (count > idx_cap) { free(idx); idx_cap = count + 4096; idx = (u32*)malloc((size_t)idx_cap * 4); }
    if (!idx) { idx_cap = 0; return NULL; }
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    { int local = ((st->index_array_dma & 0xF) == 0);
      const u8* ip = vm_base + cellGcmResolveLocated(local, st->index_array_offset);
      if (((st->index_array_dma >> 4) & 0xF) == 1) {
          ip += (u64)first * 2;
          for (u32 k = 0; k < count; k++) idx[k] = ((u32)ip[k*2] << 8) | ip[k*2+1];
      } else {
          ip += (u64)first * 4;
          for (u32 k = 0; k < count; k++)
              idx[k] = ((u32)ip[k*4] << 24) | ((u32)ip[k*4+1] << 16) |
                       ((u32)ip[k*4+2] << 8) | ip[k*4+3];
      } }
    return idx;
}

static u32 vp_indexed_fetch(const rsx_state* st, u32 first, u32 count, VPSlot* out, u32 maxv)
{
    if (!count) return 0;
    u32* idx = vp_decode_indices(st, first, count);
    if (!idx) {
        for (u32 k = 0; k < count; k++)
            read_vp_vertex(st, read_guest_index(st, first + k), &out[k*16]);
        return 0; }
    /* Primitive restart inside a triangle list: degenerate the triangle that
     * holds the cut (keeps the index count, so chained batches stay in phase)
     * instead of fetching vertex 0xFFFF. */
    if (st->restart_index_enable && !(((st->index_array_dma >> 4) & 0xF) == 1 && st->restart_index > 0xFFFFu)) {
        const u32 rv = st->restart_index; u32 nc = 0;
        for (u32 k = 0; k < count; k++) if (idx[k] == rv) {
            u32 rep = rv;
            for (u32 j = k; j-- > 0;) if (idx[j] != rv) { rep = idx[j]; break; }
            if (rep == rv) for (u32 j = k + 1; j < count; j++) if (idx[j] != rv) { rep = idx[j]; break; }
            idx[k] = rep; nc++;
        }
        { static int lg = 0; if (nc && lg < 3) { lg++;
            printf("[RESTART] list draw count=%u had %u cut indices%c", count, nc, 10); } }
    }
    return vp_indexed_core(st, idx, count, out, maxv);
}

/* The shared part: idx[] holds `count` decoded indices of a triangle list. */
static u32 vp_indexed_core(const rsx_state* st, const u32* idx, u32 count, VPSlot* out, u32 maxv)
{
    static VPSlot* scr = NULL; static u32* stamp = NULL; static u32 scr_cap = 0;
    static u32 gen = 0;
    static int en = -1;
    if (en < 0) { const char* e = getenv("VP_IDXCACHE"); en = e ? atoi(e) : 1; }
    if (!count) return 0;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    for (u32 k = 0; k < count; k++) { if (idx[k] < lo) lo = idx[k]; if (idx[k] > hi) hi = idx[k]; }
    u32 range = hi - lo + 1;
    /* Resistance port: a batch that continues the previous indexed draw must
     * take the indexed path whatever its size or spread -- expanded, it became
     * a separate draw and its first indices (the tail of a triangle the last
     * batch started) assembled with the wrong partners: York's spiked window
     * frames and the bent thumb. RSX_IB_JOIN_STRICT=0 restores the old test. */
    static int strict = -1;
    if (strict < 0) { const char* e = getenv("RSX_IB_JOIN_STRICT"); strict = e ? atoi(e) : 1; }
    const int must_index = strict && s_ib_joining && range <= maxv && range <= 262144u;
    if (!must_index && (!en || count < 12 || range > 262144u || range > count * 4u)) {
        for (u32 k = 0; k < count; k++) read_vp_vertex(st, idx[k], &out[k*16]);
        return 0;
    }
    /* Resistance port: real indexed draw. Writing a 256-byte expanded vertex
     * per INDEX into the upload heap was ~0.5 GB/frame of write-combined
     * traffic in York; unique vertices once + 4-byte indices is ~1/6 of it.
     * VP_INDEXED=0 disables (creates no index buffer). */
    if (!s_ib_force_expand && s_vp_ib_mapped && range <= maxv && s_vp_ib_off + count <= VP_MAX_INDICES) {
        if (range > scr_cap) {
            free(stamp); scr_cap = range + 1024; stamp = (u32*)calloc(scr_cap, 4); gen = 0;
            free(scr); scr = NULL;
            if (!stamp) { scr_cap = 0; for (u32 k = 0; k < count; k++) read_vp_vertex(st, idx[k], &out[k*16]); return 0; }
        }
        if (++gen == 0) { memset(stamp, 0, (size_t)scr_cap * 4); gen = 1; }
        u32* ib = s_vp_ib_mapped + (size_t)T_PARITY * VP_MAX_INDICES + s_vp_ib_off;
        static u32* uq = NULL; static u32 uq_cap = 0;
        if (range > uq_cap) { free(uq); uq_cap = range + 1024; uq = (u32*)malloc((size_t)uq_cap * 4); }
        u32 nu = 0;
        for (u32 k = 0; k < count; k++) {
            u32 r = idx[k] - lo;
            if (stamp[r] != gen) { stamp[r] = gen; if (uq) uq[nu++] = r;
                                   else read_vp_vertex(st, idx[k], &out[(size_t)r * 16]); }
            ib[k] = r + s_ib_delta;
        }
        if (uq) vp_conv_unique(st, uq, nu, lo, out);
        s_pend_ib_first = s_vp_ib_off; s_pend_ib_count = count; s_vp_ib_off += count;
        return range;
    }
    if (range > scr_cap || !scr) {
        free(scr); free(stamp);
        scr_cap = range + 1024;
        scr = (VPSlot*)malloc((size_t)scr_cap * 16 * sizeof(VPSlot));
        stamp = (u32*)calloc(scr_cap, 4);
        gen = 0;
        if (!scr || !stamp) { free(scr); free(stamp); scr = NULL; stamp = NULL; scr_cap = 0;
            for (u32 k = 0; k < count; k++) read_vp_vertex(st, idx[k], &out[k*16]);
            return 0; }
    }
    if (++gen == 0) { memset(stamp, 0, (size_t)scr_cap * 4); gen = 1; }
    for (u32 k = 0; k < count; k++) {
        u32 r = idx[k] - lo;
        VPSlot* c = &scr[(size_t)r * 16];
        if (stamp[r] != gen) { read_vp_vertex(st, idx[k], c); stamp[r] = gen; }
        memcpy(&out[k*16], c, 16 * sizeof(VPSlot));
    }
    return 0;
}

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    vp_fast_prep(state);
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!vp_has_attrs(state)) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && PS3_LOG_FIRST(_n, 12))
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)T_PARITY * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      u32 nv = vp_indexed_fetch(state, first, count, out, maxv);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; }
      if (nv) { s_d3d.vp_vb_offset += nv * VP_VERT_STRIDE; return nv; } }
    /* IDXDBG=<hex shader_program>: index range for that program's draws. An
     * index past the vertex array reads unmapped guest memory as zero, which
     * shows up as a spike triangle with a zero texcoord, not as a missing draw. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (PS3_LOG_FIRST(_n, 12)) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          fprintf(stderr, "[IDXDBG] first=%u count=%u idx=[%u..%u] dma=0x%X a0off=0x%X stride=%u%c",
                  first, count, lo, hi, state->index_array_dma,
                  state->vertex_attribs[0].offset, state->vertex_attribs[0].stride, 10); } } }
    /* DBG_LOCK tracking: projected centroid of this mesh, updated every draw. */
    { static const char* dl = (const char*)1; static u32 wantl = 0; static int mvpb = 256;
      if (dl == (const char*)1) { dl = getenv("RSX_DBG_VTX");
                                  wantl = dl ? (u32)strtoul(dl, NULL, 16) : 0;
                                  const char* mb = getenv("MVP_BASE");
                                  if (mb) mvpb = atoi(mb); }
      /* RSX_DBG_TRACK=1 tracks the content-identified duck without needing its
       * offset passed in (VRAM offsets move between runs). Re-read every call:
       * s_duck_raw is filled by the texture upload, which happens AFTER the
       * first draws, so caching it once left the tracker watching offset 0. */
      { static int dtrk = -1;
        if (dtrk < 0) { const char* e = getenv("RSX_DBG_TRACK"); dtrk = e ? atoi(e) : 0; }
        if (dtrk && s_duck_raw) wantl = s_duck_raw; }
      /* RSX_DBG_PICK=<first>: track ONLY the draw with that starting index. The
       * mesh is one big vertex buffer sliced into 256-index chunks, so averaging
       * across all of them converges on the centre of the whole field -- which is
       * where there is nothing in particular. A single slice is a stable target
       * and can be magnified without drifting off it. */
      static int pick = -1;
      if (pick < 0) { const char* e = getenv("RSX_DBG_PICK"); pick = e ? atoi(e) : -2; }
      if (wantl && s_d3d.cur_texs[0].raw == wantl && count
          && (pick == -2 || (int)first == pick)) {
          double sx = 0, sy = 0, sz = 0;
          for (u32 k = 0; k < count; k++) {
              sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
          }
          float v[4] = { (float)(sx/count), (float)(sy/count), (float)(sz/count), 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[mvpb + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          if (clip[3] > 1e-6f) {
              float nx = clip[0]/clip[3], ny = clip[1]/clip[3];
              if (nx > -2.0f && nx < 2.0f && ny > -2.0f && ny < 2.0f) {
                  s_lock_sx += nx; s_lock_sy += ny; s_lock_n++;
                  if (!s_lock_valid) {        /* seed so frame 1 is usable */
                      s_lock_x = nx; s_lock_y = ny; s_lock_valid = 1;
                  }
              }
          }
      } }
    /* RSX_DBG_SCALE=<f>: multiply attribute-0 positions for the draws binding
     * RSX_DBG_VTX's texture. The duck's object-space bbox is ~0.2 units and its
     * draws rasterize only a few dozen pixels per frame -- sub-pixel. Growing it
     * about its own origin tests the rest of the chain (index buffer, texture,
     * per-draw transform): if a duck-shaped, duck-coloured object appears, only
     * the scale is wrong. Diagnostic. */
    { static const char* ds = (const char*)1; static float sc = 0.0f;
      if (ds == (const char*)1) { ds = getenv("RSX_DBG_SCALE");
                                  sc = ds ? (float)atof(ds) : 0.0f; }
      static const char* dv2 = (const char*)1; static u32 want2 = 0;
      if (dv2 == (const char*)1) { dv2 = getenv("RSX_DBG_VTX");
                                   want2 = dv2 ? (u32)strtoul(dv2, NULL, 16) : 0; }
      if (sc > 0.0f && want2 && s_d3d.cur_texs[0].raw == want2 && count) {
        /* Magnify about a FIXED point captured from the first tracked draw, not
         * about each chunk's own centre -- recentring per chunk would stack every
         * chunk of the mesh on top of the others. Combined with VP_BYPASS (which
         * maps attribute 0 straight to clip space) this renders the mesh's real
         * geometry and texture at a readable size. */
        /* RSX_DBG_RECENTER=1: recentre EVERY chunk on its own centroid, overlaying
         * them. If each chunk is one instance of the same mesh they align into a
         * single silhouette; if they are arbitrary slices of one big mesh they
         * smear. Either way the answer is visible at a glance. */
        static int recen = -1;
        if (recen < 0) { const char* e = getenv("RSX_DBG_RECENTER"); recen = e ? atoi(e) : 0; }
        static int have_c = 0; static float ox = 0, oy = 0, oz = 0;
        if (!have_c || recen) {
            double sx = 0, sy = 0, sz = 0;
            for (u32 k = 0; k < count; k++) {
                sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
            }
            ox = (float)(sx/count); oy = (float)(sy/count); oz = (float)(sz/count);
            have_c = 1;
        }
        for (u32 k = 0; k < count; k++) {
            /* RSX_DBG_SHIFT="dx,dy": nudge after scaling. VP_BYPASS applies the
             * guest's posoffset, so a recentred mesh does not land at screen
             * centre -- without this the magnified geometry sits clipped against
             * the top edge. */
            static int shf = -1; static float dx = 0.0f, dy = 0.0f;
            if (shf < 0) { const char* e = getenv("RSX_DBG_SHIFT");
                           if (e) { double a=0,b=0; sscanf(e, "%lf,%lf", &a, &b);
                                    dx = (float)a; dy = (float)b; }
                           shf = 1; }
            out[k*16].v[0] = (out[k*16].v[0] - ox) * sc + dx;
            out[k*16].v[1] = (out[k*16].v[1] - oy) * sc + dy;
            /* Leave z alone: under VP_BYPASS the object z maps straight to clip
             * z, so scaling it walks the geometry out of the depth range and
             * everything clips away. */
            (void)oz;
        }
      } }
    /* RSX_DBG_VTX=<hex tex0 offset>: attribute-0 positions for the draws binding
     * that texture (see upload_tris_vp). The duck's meshes are indexed, so this
     * is the copy that actually fires for it. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 8) { n++;
        float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
        for (u32 k = 0; k < count; k++) {
            float x=out[k*16].v[0], y=out[k*16].v[1], z=out[k*16].v[2];
            if (x<mnx)mnx=x; if (y<mny)mny=y; if (z<mnz)mnz=z;
            if (x>mxx)mxx=x; if (y>mxy)mxy=y; if (z>mxz)mxz=z;
        }
        const rsx_vertex_attrib* _a3 = &state->vertex_attribs[3];
        fprintf(stderr, "[DUCKVTX] a3(en=%d type=%u size=%u stride=%u off=0x%08X freq=%u)"
                        " -> col0 v0=(%.3f %.3f %.3f %.3f)%c",
                _a3->enabled, _a3->type, _a3->size, _a3->stride, _a3->offset,
                _a3->frequency,
                out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3], 10);
        const rsx_vertex_attrib* _a0 = &state->vertex_attribs[0];
        fprintf(stderr, "[DUCKVTX] first=%u count=%u a0(type=%u size=%u stride=%u off=0x%08X)"
                        " obj-bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]%c",
                first, count, _a0->type, _a0->size, _a0->stride, _a0->offset,
                mnx,mxx, mny,mxy, mnz,mxz, 10);
        /* Project the object-space bbox corners through the first four transform
         * constants (the conventional MVP rows) and report NDC. Sub-pixel output
         * with correct vertices means the placement transform is the problem, and
         * the NDC extent says by how much. */
        { float cx=(mnx+mxx)*0.5f, cy=(mny+mxy)*0.5f, cz=(mnz+mxz)*0.5f;
          /* This title's Cg programs keep their MVP at c[256..259], not c[0..3]
           * (see the vp_uses_c03 note); projecting through c0 reported zeros. */
          static int MVP = -1;
          if (MVP < 0) { const char* e = getenv("MVP_BASE"); MVP = e ? atoi(e) : 256; }
          float v[4] = { cx, cy, cz, 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[MVP + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          float ex[4];
          { float e[4] = { mxx-cx, mxy-cy, mxz-cz, 0.0f };
            for (int r = 0; r < 4; r++) {
                const float* m = state->vertex_constants[MVP + r];
                ex[r] = m[0]*e[0] + m[1]*e[1] + m[2]*e[2];
            } }
          { int lo = -1, hi = -1, nz = 0;
            for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
                const float* m = state->vertex_constants[r];
                if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
            }
            fprintf(stderr, "[DUCKVTX]   constants: %d non-zero, slots %d..%d;", nz, lo, hi);
            for (int r = (lo < 0 ? 0 : lo); r < (lo < 0 ? 0 : lo) + 4 && r < RSX_MAX_VERTEX_CONSTANTS; r++)
                fprintf(stderr, " c%d=(%.3f %.3f %.3f %.3f)", r,
                        state->vertex_constants[r][0], state->vertex_constants[r][1],
                        state->vertex_constants[r][2], state->vertex_constants[r][3]);
            fprintf(stderr, "%c", 10); }
          fprintf(stderr, "[DUCKVTX]   mvp0=(%.3f %.3f %.3f %.3f) clip=(%.3f %.3f %.3f w=%.3f)",
                  state->vertex_constants[MVP][0], state->vertex_constants[MVP][1],
                  state->vertex_constants[MVP][2], state->vertex_constants[MVP][3],
                  clip[0], clip[1], clip[2], clip[3]);
          if (clip[3] > 1e-6f)
              fprintf(stderr, " ndc=(%.4f %.4f) half-extent-px=(%.2f %.2f)",
                      clip[0]/clip[3], clip[1]/clip[3],
                      ex[0]/clip[3]*640.0f, ex[1]/clip[3]*360.0f);
          fprintf(stderr, "%c", 10); }
      } }
    /* IDX_DBG=<N>: the first indices and the position they resolve to. An index
     * buffer read with the wrong element size or base yields huge indices and
     * degenerate triangles -- an INDEXED mesh vanishes while non-indexed
     * geometry in the same scene renders fine. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("IDX_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && count >= 3) { n++;
        extern u32 cellGcmResolveOffset(u32);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        u32 vbase = cellGcmResolveOffset(a0->offset & 0x7FFFFFFFu);
        u32 nz = 0; for (u32 q = 0; q < 0x1000u; q += 29) if (vm_base[vbase + q]) nz++;
        fprintf(stderr, "[IDX] idxoff=0x%X idx: %u %u %u | a0 off=0x%08X(->0x%08X nz=%u/142)"
                        " stride=%u size=%u type=%u  pos0=(%g %g %g)%c",
                state->index_array_offset,
                read_guest_index(state, first), read_guest_index(state, first+1),
                read_guest_index(state, first+2),
                a0->offset, vbase, nz, a0->stride, a0->size, a0->type,
                out[0].v[0], out[0].v[1], out[0].v[2], 10); } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Indexed TRIANGLE_STRIP / TRIANGLE_FAN, expanded CPU-side into a triangle
 * list (the whole VP path draws lists). DeferredShading's spline tube meshes
 * -- the octopus tentacles -- are indexed strips. Odd strip triangles swap
 * two vertices, as in upload_strip_vp: guest culling is honoured now, and
 * without the swap every second triangle comes out back-facing (God of War
 * HD's fullscreen 4-index quads lost their lower-right half). */
static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    vp_fast_prep(state);
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!vp_has_attrs(state)) return 0;
    if (count < 3) return 0;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)T_PARITY * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    /* Resistance port: honour primitive restart, then draw the strip as an
     * indexed triangle list through the shared unique-vertex path. A 16-bit
     * index buffer cannot hold a restart value above 0xFFFF (RPCS3 rule). */
    u32* src = vp_decode_indices(state, first, count);
    if (!src) return 0;
    static u32* tl = NULL; static u32 tl_cap = 0;
    u32 need = (count - 2) * 3;
    if (need > tl_cap) { free(tl); tl_cap = need + 4096; tl = (u32*)malloc((size_t)tl_cap * 4); }
    if (!tl) { tl_cap = 0; return 0; }
    const int is16 = ((state->index_array_dma >> 4) & 0xF) == 1;
    int rs = -1; static int rsoff = -1;
    if (rsoff < 0) { const char* e = getenv("RSX_RESTART"); rsoff = (e && atoi(e) == 0) ? 1 : 0; }
    if (state->restart_index_enable && !rsoff && !(is16 && state->restart_index > 0xFFFFu)) rs = 1;
    const u32 rv = state->restart_index;
    u32 n = 0, seg = 0;            /* seg = index position within the current strip */
    u32 a = 0, b = 0, c0 = 0;      /* c0: fan centre */
    static u32 s_cuts = 0;
    for (u32 k = 0; k < count; k++) {
        u32 ix = src[k];
        if (rs > 0 && ix == rv) { seg = 0; s_cuts++; continue; }
        if (fan) {
            if (seg == 0) c0 = ix;
            else if (seg >= 2) { tl[n++] = c0; tl[n++] = b; tl[n++] = ix; }
            b = ix;
        } else {
            if (seg >= 2) {
                if (seg & 1) { tl[n++] = b; tl[n++] = a; tl[n++] = ix; }
                else         { tl[n++] = a; tl[n++] = b; tl[n++] = ix; }
            }
            a = b; b = ix;
        }
        seg++;
    }
    { static int lg = 0; if (rs > 0 && s_cuts && lg < 3) { lg++;
        printf("[RESTART] strip draw count=%u -> %u list indices (cuts so far %u)%c", count, n, s_cuts, 10); } }
    if (!n) return 0;
    if (n > maxv) n = maxv - maxv % 3;
    u32 nv = vp_indexed_core(state, tl, n, out, maxv);
    if (nv) { s_d3d.vp_vb_offset += nv * VP_VERT_STRIDE; return nv; }
    s_d3d.vp_vb_offset += n * VP_VERT_STRIDE;
    return n;
}

/* Resistance port: mid-frame flush. A Fall of Man gameplay frame redraws its
 * geometry into several shadow maps and the scene -- well over the per-frame
 * vertex buffer (MAX_VERTICES) -- and everything recorded after it filled was
 * silently dropped: the frame stopped at ~360 draws, before the scene was
 * finished and before any post pass or HUD, so the screen stayed black.
 * Before a draw that may not fit, execute what is recorded so far (offscreen
 * results persist on the GPU; no present) and start a fresh batch.
 * RSX_MIDFLUSH=0 disables. */
static void render_frame(void);
static void d3d12_maybe_midframe_flush(u32 primitive, u32 count)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_MIDFLUSH"); en = e ? atoi(e) : 1; }
    if (!en || !s_d3d.initialized || !T_DRAWCNT) return;
    u32 need = count * 3u + 6u;          /* worst case: strip/fan/quad expansion */
    u32 used = s_d3d.vp_vb_offset / (u32)(16 * 16);
    if (used + need <= MAX_VERTICES && T_DRAWCNT + 8 < MAX_DRAWS) return;
    InterlockedIncrement(&s_rfr[(used + need > MAX_VERTICES) ? RFR_MIDV : RFR_MIDD]);
    { static int n = 0; if (n < 4) { n++;
        fprintf(stderr, "[MIDFLUSH] %u draws / %u verts recorded -> executing before a %u-vertex draw%c",
                T_DRAWCNT, used, count, 10); } }
    int prev = s_present_this_frame;
    s_present_this_frame = 0;
    s_in_midflush = 1;
    render_frame();
    s_in_midflush = 0;
    s_batch_cont = 1;   /* the next batch continues this frame: keep its depth */
    s_present_this_frame = prev;
    (void)primitive;
}

static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    gf_finalize();
    d3d12_maybe_midframe_flush(primitive, count);
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] arrays callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;

    /* QUADS (prim 8) have no D3D12 topology; expand to a triangle list.
     * dbgfont draws all text as quads. Prefer the real vertex-program path
     * (exact position + texcoord): upload raw float4 attrib0 into vp_vb and
     * mark the draw is_vp; render_frame compiles the VP and draws it. Fall
     * back to the frac-approximation textured path if VP resources are absent. */
    if (primitive == RSX_PRIMITIVE_QUADS) {
        /* Prefer the real vertex-program path whenever VP resources exist --
         * NOT only when a texture is bound. Requiring tex_bound routed vkcube's
         * UNtextured cube to the fixed-function fallback, which never applies the
         * VP's MVP transform, so the cube was drawn in object space and clipped
         * off-screen. Untextured VP draws now render via the colour PSO. */
        if (s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
            u32 rec = s_d3d.vp_vb_offset;
            u32 emitted = upload_quads_vp(s_d3d.current_rsx_state, first, count);
            if (emitted && T_DRAWCNT < MAX_DRAWS) {
                D3D12DrawRecord* dr = &T_DRAWS[T_DRAWCNT]; dr->gf = 0;
                dr->vb_byte_offset = rec;
                dr->vertex_count   = emitted;
                dr->ib_count       = 0;
                dr->topology       = RSX_TOPOLOGY_TRIANGLES;
                dr->textured       = s_d3d.tex_bound;
                dr->is_vp          = 1;
                dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
                dr->fp_exp32 = s_d3d.current_rsx_state ?
                    ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
                dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
                dr->cmask = 0xF;
                if (s_d3d.current_rsx_state) {
                    u32 _cm = s_d3d.current_rsx_state->color_mask;
                    dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                              | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                              | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                              | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
                }
                dr->alpha_ctl = 0;
                if (s_d3d.current_rsx_state)
                    dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                                  | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                                  | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
                for (int _u = 0; _u < RSX_UNITS; _u++) {
                    dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                    dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                    dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                    dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                    dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1; dr->tex[_u].xmask = s_d3d.cur_texs[_u].xmask; dr->tex[_u].freg = s_d3d.cur_texs[_u].freg; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].address = s_d3d.cur_texs[_u].address; dr->tex[_u].ctrl0 = s_d3d.cur_texs[_u].ctrl0;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch;
                    dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                    dr->tex_rt[_u]  = -1;
                }
                dr->tex_slot = -1;
                dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
                vs_attr_log(s_d3d.current_rsx_state, dr->vs_idx, s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0);
                dr->is_clear = 0;
                dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
                dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
                if (getenv("BLENDDBG")) { static u32 seen[32]; static int ns=0;
                    u32 k = (dr->blend ? 0x80000000u : 0u) | dr->blend_key;
                    int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
                    if (!f && ns<32) { seen[ns++]=k;
                        fprintf(stderr, "[BLEND] enable=%d key=0x%X%c",
                                dr->blend, dr->blend_key, 10); } }
                dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
                dr->disp_tag = dr->rt_off ? 0u : current_disp_tag();
                dr->zeta_off = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_zeta_offset : 0;
                dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
                if (s_d3d.current_rsx_state) {
                    dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                    dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                    dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                    dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
                dr->cb_slot = T_DRAWCNT;
                vp_record_cb(T_DRAWCNT, dr->vs_idx, dr);
                T_DRAWCNT++;
            }
            return;
        }
        u32 record_offset = s_d3d.vb_offset;
        u32 emitted = upload_quads_from_rsx(first, count);
        if (emitted == 0) return;
        if (T_DRAWCNT < MAX_DRAWS) {
            T_DRAWS[T_DRAWCNT].vb_byte_offset = record_offset;
            T_DRAWS[T_DRAWCNT].vertex_count   = emitted;
            T_DRAWS[T_DRAWCNT].topology       = RSX_TOPOLOGY_TRIANGLES;
            T_DRAWS[T_DRAWCNT].textured       = s_d3d.tex_bound;
            T_DRAWS[T_DRAWCNT].is_vp          = 0;
            T_DRAWS[T_DRAWCNT].is_clear       = 0;
            T_DRAWS[T_DRAWCNT].rt_off         = 0;
            T_DRAWCNT++;
        }
        return;
    }

    /* TRIANGLES (prim 5): same VP-path preference as quads -- the guest's
     * vertex program does the MVP transform (gcm/cube draws its cube this way);
     * the fixed-function fallback below applies no transform, so 3D geometry
     * ends up in object space (invisible/garbage). */
    /* Route TRIANGLES/STRIP/FAN through the VP path (sagemono: LBP draws
     * non-indexed strips/fans), but ONLY when the guest ACTUALLY HAS a vertex
     * program (flOw). The VP path transforms via the guest's VP microcode; with
     * no microcode loaded it transforms nothing and the draw silently produces
     * zero pixels (it also uploads to vp_vb, leaving the fixed-function vb empty).
     * Geometry already in clip space with no VP -- flOw's injected scene -- must
     * go down the fixed-function passthrough below instead. */
    if ((primitive == RSX_PRIMITIVE_TRIANGLES || primitive == RSX_PRIMITIVE_TRIANGLE_STRIP
         || primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig &&
        s_d3d.current_rsx_state && s_d3d.current_rsx_state->vp_ucode_bytes >= 16) {
        /* Resistance port: triangle-phase tracker. One triangle list arrives as
             * several DRAW batches inside one BEGIN/END and the RSX assembles them as a
             * single stream. Joining batches into one record keeps the phase, but any
             * batch that starts a NEW record (a mid-frame flush emptied the record
             * list, the index buffer filled, a join test failed) used to restart
             * triangle assembly at its own first index: when the run so far was not a
             * multiple of 3 every later triangle took the wrong corners -- the jagged
             * bus / corpses / thumb that came and went with camera position (the
             * flush and batch boundaries move with what is on screen). Such a batch is
             * now re-read from first - P (P = run length mod 3); the previous record's
             * trailing P indices are an incomplete triangle D3D ignores anyway.
             * RSX_TRI_PHASE=0 off. */
            static int s_tp_en = -1;
            if (s_tp_en < 0) { const char* e = getenv("RSX_TRI_PHASE"); s_tp_en = e ? atoi(e) : 1; }
            static u32 s_tpa_epoch = 0xFFFFFFFFu, s_tpa_end = 0, s_tpa_total = 0;
            u32 run_P = 0;
            { const u32 ep_now = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
              if (primitive == RSX_PRIMITIVE_TRIANGLES && ep_now == s_tpa_epoch && first == s_tpa_end) run_P = s_tpa_total % 3u;
              else s_tpa_total = 0;
              s_tpa_epoch = (primitive == RSX_PRIMITIVE_TRIANGLES) ? ep_now : 0xFFFFFFFFu;
              s_tpa_end = first + count; s_tpa_total += count;
              if (!s_tp_en || first < run_P) run_P = 0; }
        u32 rec = s_d3d.vp_vb_offset;
        u32 emitted = (primitive == RSX_PRIMITIVE_TRIANGLES)
            ? upload_tris_vp(s_d3d.current_rsx_state, first, count)
            : upload_strip_vp(s_d3d.current_rsx_state, first, count, primitive == RSX_PRIMITIVE_TRIANGLE_FAN);
        /* RSX splits one primitive stream across several DRAW_ARRAYS entries
         * inside a single BEGIN/END (each carries at most 256 vertices), and the
         * hardware concatenates them before assembling primitives. Recording a
         * draw per batch instead regroups the triangles: this title's tub mesh
         * arrives as 128 then 256,256,..., neither a multiple of 3, so every
         * batch after the first was assembled one vertex out of phase -- which
         * rendered as spike triangles with a zero texcoord along the tub rim.
         * The batches land contiguously in the vertex buffer, so extending the
         * previous record is all that is needed to restore the stream. */
        if (emitted && s_d3d.merge_prev_draw && T_DRAWCNT > 0 &&
            s_d3d.merge_first_end == first) {
            D3D12DrawRecord* pv = &T_DRAWS[T_DRAWCNT - 1];
            u32 fpnow = s_d3d.current_rsx_state
                        ? s_d3d.current_rsx_state->shader_program : 0;
            if (pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
                pv->fp_addr == fpnow && primitive == RSX_PRIMITIVE_TRIANGLES &&
                pv->begin_epoch == (s_d3d.current_rsx_state
                                    ? s_d3d.current_rsx_state->begin_epoch : 0) &&
                !pv->ib_count && pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
                pv->vertex_count += emitted;
                s_d3d.merge_first_end = first + count;
                return;
            }
        }
        if (run_P && emitted) {
            vp_async_drain();
            s_d3d.vp_vb_offset = rec;
            emitted = upload_tris_vp(s_d3d.current_rsx_state, first - run_P, count + run_P);
            { static int n = 0; if (n < 8) { n++;
                fprintf(stderr, "[TRIPHASE] arrays batch first=%u count=%u restarted %u back (new record)%c", first, count, run_P, 10); } }
        }
        if (emitted && T_DRAWCNT < MAX_DRAWS) {
            D3D12DrawRecord* dr = &T_DRAWS[T_DRAWCNT]; dr->gf = 0;
            dr->vb_byte_offset = rec;
            dr->vertex_count   = emitted;
            dr->ib_count       = 0;
            dr->topology       = RSX_TOPOLOGY_TRIANGLES;
            dr->textured       = s_d3d.tex_bound;
            dr->is_vp          = 1;
            dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
            dr->fp_exp32 = s_d3d.current_rsx_state ?
                ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
            dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->cmask = 0xF;
            if (s_d3d.current_rsx_state) {
                u32 _cm = s_d3d.current_rsx_state->color_mask;
                dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                          | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                          | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                          | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
            }
            dr->alpha_ctl = 0;
            if (s_d3d.current_rsx_state)
                dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                              | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                              | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
            for (int _u = 0; _u < RSX_UNITS; _u++) {
                dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1; dr->tex[_u].xmask = s_d3d.cur_texs[_u].xmask; dr->tex[_u].freg = s_d3d.cur_texs[_u].freg; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].address = s_d3d.cur_texs[_u].address; dr->tex[_u].ctrl0 = s_d3d.cur_texs[_u].ctrl0;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch;
                dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                dr->tex_rt[_u]  = -1;
            }
            dr->tex_slot = -1;
            dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
            vs_attr_log(s_d3d.current_rsx_state, dr->vs_idx, s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0);
            dr->is_clear = 0;
            dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
            dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
            dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
            dr->disp_tag = dr->rt_off ? 0u : current_disp_tag();
                dr->zeta_off = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_zeta_offset : 0;
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            if (s_d3d.current_rsx_state) {
                dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                dr->vp_h = s_d3d.current_rsx_state->viewport_h;
            } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
            dr->cb_slot = T_DRAWCNT;
                vp_record_cb(T_DRAWCNT, dr->vs_idx, dr);
            T_DRAWCNT++;
            /* Anchor for merging the next DRAW_ARRAYS batch of this stream. */
            s_d3d.merge_prev_draw  = (primitive == RSX_PRIMITIVE_TRIANGLES);
            s_d3d.merge_first_end  = first + count;
        }
        return;
    }
    s_d3d.merge_prev_draw = 0;   /* any other primitive breaks the stream */

    u32 topo = (u32)rsx_primitive_topology(primitive);
    if (topo == RSX_TOPOLOGY_UNSUPPORTED) {
        /* Other primitives still needing index-buffer conversion
         * (line loops, triangle fans) — skip rather than draw wrong. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (T_DRAWCNT < MAX_DRAWS) {
        T_DRAWS[T_DRAWCNT].vb_byte_offset = record_offset;
        T_DRAWS[T_DRAWCNT].vertex_count   = actual_count;
        T_DRAWS[T_DRAWCNT].topology       = topo;
        T_DRAWS[T_DRAWCNT].textured       = 0;
        T_DRAWS[T_DRAWCNT].is_vp          = 0;
        T_DRAWS[T_DRAWCNT].is_clear       = 0;
        T_DRAWS[T_DRAWCNT].rt_off         = 0;
        T_DRAWCNT++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 first, u32 count)
{
    /* A GPU-fetch draw takes no vertex-buffer space: only the draw-count
     * limit applies (its own fallback re-checks the vertex room below). */
    const int gf_try = primitive == RSX_PRIMITIVE_TRIANGLES && gf_enabled();
    d3d12_maybe_midframe_flush(primitive, gf_try ? 0u : count);
    /* v180 diag: RSX_DRAWLOG_ONSPACE=<n>: after the first Space/Enter, log n
     * indexed draws as the FIFO issues them (texture, attribute mask, a1). */
    { extern int g_drawlog_left; const int left = g_drawlog_left;   /* armed by the Space/Enter capture */
      if (left > 0) g_drawlog_left = left - 1;
      if (left > 0 && s_d3d.current_rsx_state) {
          const rsx_state* st = s_d3d.current_rsx_state; u32 am = 0;
          for (int i = 0; i < 16; i++) if (st->vertex_attribs[i].enabled && st->vertex_attribs[i].stride) am |= 1u << i;
          fprintf(stderr, "[DRAWLOG] par=%d gfo=%u fc=%llu rec=%u prim=%u first=%u cnt=%u ep=%u fp=%08X t0=%08X(%d) am=%04X a1c=(%.3f %.3f %.3f %.3f)%c",
                  (int)T_PARITY, s_gf_off, (unsigned long long)s_d3d.frame_count, T_DRAWCNT, primitive, first, count, st->begin_epoch, st->shader_program,
                  s_d3d.cur_texs[0].raw, s_d3d.cur_texs[0].set, am,
                  st->vertex_data4f[1][0], st->vertex_data4f[1][1], st->vertex_data4f[1][2], st->vertex_data4f[1][3], 10);
          /* guest bytes the draw will read, as of NOW (FIFO parse time) */
          { extern uint8_t* vm_base; extern u32 cellGcmResolveLocated(int local, u32 offset);
            for (int ai = 0; ai < 3; ai += 2) {
              const rsx_vertex_attrib* va = &st->vertex_attribs[ai];
              if (!va->enabled || !va->stride) continue;
              const int loc = (va->offset & 0x80000000u) ? 0 : 1;
              u32 o0 = (va->offset & 0x7FFFFFFFu) + st->vertex_data_base_offset; if (loc) o0 &= 0x0FFFFFFFu;
              const u32 r = loc ? cellGcmResolveLocated(1, o0) : (cellGcmResolveLocated(0, o0 & ~0xFFFFFu) + (o0 & 0xFFFFFu));
              if (!vm_base || r == 0xFFFFFFFFu) continue;
              float f[6];
              for (int k = 0; k < 6; k++) { u32 w; memcpy(&w, vm_base + r + (k < 3 ? k * 4 : va->stride * 2 + (k - 3) * 4), 4); w = _byteswap_ulong(w); memcpy(&f[k], &w, 4); }
              fprintf(stderr, "[DRAWLOG]    a%d @%c%08X stride %u: v0=(%g %g %g) v2=(%g %g %g)%c", ai, loc ? 'L' : 'M', o0, va->stride,
                      f[0], f[1], f[2], f[3], f[4], f[5], 10);
            } }
      } }
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] indexed callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    static int log_count = 0;
    if (log_count < 8) {
        printf("[D3D12] draw_indexed(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        log_count++;
    }
    if (!s_d3d.pipeline_ready) return;
    if (count == 0 || count > MAX_VERTICES) return;
    if (!s_d3d.vp_vb_mapped || !s_d3d.vp_root_sig) return;

    /* Expand through the VP path (indices resolved CPU-side): QUADS -> two
     * triangles per quad, TRIANGLES straight through, STRIP/FAN -> triangle
     * list. Other primitives are skipped rather than drawn wrong. */
    u32 emitted = 0;
    u32 rec = s_d3d.vp_vb_offset;
    /* Resistance port: a batch that CONTINUES the previous one (first ==
     * merge_first_end) must join its triangle list -- 256-index batches are
     * not multiples of 3, so as separate draws every batch after the first
     * assembles out of phase (jagged, stretched meshes). Continue an indexed
     * draw by offsetting the new indices to its base vertex; continue an
     * expanded draw by staying expanded. */
    /* Resistance port: triangle-phase tracker. One triangle list arrives as
     * several DRAW batches inside one BEGIN/END and the RSX assembles them as a
     * single stream. Joining batches into one record keeps the phase, but any
     * batch that starts a NEW record (a mid-frame flush emptied the record
     * list, the index buffer filled, a join test failed) used to restart
     * triangle assembly at its own first index: when the run so far was not a
     * multiple of 3 every later triangle took the wrong corners -- the jagged
     * bus / corpses / thumb that came and went with camera position (the
     * flush and batch boundaries move with what is on screen). Such a batch is
     * now re-read from first - P (P = run length mod 3); the previous record's
     * trailing P indices are an incomplete triangle D3D ignores anyway.
     * RSX_TRI_PHASE=0 off. */
    static int s_tp_en = -1;
    if (s_tp_en < 0) { const char* e = getenv("RSX_TRI_PHASE"); s_tp_en = e ? atoi(e) : 1; }
    static u32 s_tpi_epoch = 0xFFFFFFFFu, s_tpi_end = 0, s_tpi_total = 0;
    u32 run_P = 0;
    { const u32 ep_now = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
      if (primitive == RSX_PRIMITIVE_TRIANGLES && ep_now == s_tpi_epoch && first == s_tpi_end) run_P = s_tpi_total % 3u;
      else s_tpi_total = 0;
      s_tpi_epoch = (primitive == RSX_PRIMITIVE_TRIANGLES) ? ep_now : 0xFFFFFFFFu;
      s_tpi_end = first + count; s_tpi_total += count;
      if (!s_tp_en || first < run_P) run_P = 0; }
    int rephased = 0;
    D3D12DrawRecord* ib_join = NULL;
    u32 ib_first = 0, ib_count = 0;
    /* Resistance port: GPU vertex fetch for indexed triangle lists. A batch
     * that continues the open GF record (same BEGIN/END, first == the last
     * batch's end) appends its indices to it; its vertices are copied when
     * the chain ends (gf_finalize). Anything else closes the open record and
     * starts a new one on the triangle boundary (run_P). */
    s_gf_rec = 0;
    if (primitive == RSX_PRIMITIVE_TRIANGLES && s_gfp.active && T_DRAWCNT > 0 &&
        s_d3d.merge_first_end == first && gf_extend(s_d3d.current_rsx_state, first, count)) {
        D3D12DrawRecord* pv = &T_DRAWS[T_DRAWCNT - 1];
        pv->ib_count += count; pv->vertex_count += count;
        s_d3d.merge_first_end = first + count;
        return;
    }
    gf_finalize();
    if (primitive == RSX_PRIMITIVE_TRIANGLES &&
        gf_begin(s_d3d.current_rsx_state, first - run_P, count + run_P)) {
        emitted = count + run_P; ib_first = s_gf_ib_first; ib_count = emitted; s_gf_rec = 1;
        goto make_record;
    }
    if (gf_try) {
        s_gf_fallback++;
        /* the CPU path needs vertex room after all */
        const u32 n0 = T_DRAWCNT;
        d3d12_maybe_midframe_flush(primitive, count);
        if (T_DRAWCNT != n0) rec = s_d3d.vp_vb_offset;   /* flushed: offsets restarted */
    }
    s_ib_force_expand = 0; s_ib_delta = 0; s_ib_joining = 0;
    if (primitive == RSX_PRIMITIVE_TRIANGLES && s_d3d.merge_prev_draw && T_DRAWCNT > 0 &&
        s_d3d.merge_first_end == first) {
        D3D12DrawRecord* pv = &T_DRAWS[T_DRAWCNT - 1];
        u32 fpnow = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
        if (pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES && pv->fp_addr == fpnow &&
            pv->begin_epoch == (s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0)) {
            if (!pv->ib_count) s_ib_force_expand = 1;
            else if (pv->ib_first + pv->ib_count == s_vp_ib_off &&
                     pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
                ib_join = pv; s_ib_delta = pv->vertex_count; s_ib_joining = 1;
            } else s_ib_force_expand = 1;   /* cannot join: keep the phase by expanding */
        }
    }
    if (primitive == RSX_PRIMITIVE_QUADS)             emitted = upload_quads_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLES)    emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_STRIP) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 0);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_FAN)   emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 1);
    else {
        static int _skip = 0;
        if (PS3_LOG_FIRST(_skip, 3))
            printf("[D3D12] draw_indexed: skipping prim=%u (not wired)\n", primitive);
        return;
    }
    ib_first = s_pend_ib_first; ib_count = s_pend_ib_count;
    s_pend_ib_count = 0; s_ib_force_expand = 0; s_ib_delta = 0; s_ib_joining = 0;
    if (ib_join && !(ib_count && emitted)) {
        /* Could not join (index range too wide for the indexed path, or the
         * index buffer full). Re-phase instead: the previous draw ends with
         * P = ib_count % 3 indices of a triangle this batch completes. Drop
         * them from the previous draw and redo this batch from first - P,
         * expanded, so it starts on a triangle boundary. Without this a few
         * meshes still came out jagged/spiked, flipping with camera angle as
         * batch splits moved. RSX_IB_REPHASE=0 off. */
        static int rp = -1;
        if (rp < 0) { const char* e = getenv("RSX_IB_REPHASE"); rp = e ? atoi(e) : 1; }
        const u32 P = ib_join->ib_count % 3u;
        if (rp && P && first >= P && ib_join->ib_count > P) {
            vp_async_drain();                         /* no conversion may still be writing there */
            s_d3d.vp_vb_offset = rec;                 /* discard what was just written */
            if (ib_count) s_vp_ib_off = ib_first;
            ib_join->ib_count -= P;
            s_ib_force_expand = 1;
            emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first - P, count + P);
            s_pend_ib_count = 0; s_ib_force_expand = 0;
            ib_count = 0; ib_first = 0; rephased = 1;
            { static int n = 0; if (n < 8) { n++;
                fprintf(stderr, "[IBJOIN] rephased continuation: P=%u count=%u -> emitted=%u%c", P, count, emitted, 10); } }
        } else { static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[IBJOIN] continuation batch could not join (count=%u emitted=%u P=%u) -- may assemble out of phase%c", count, emitted, P, 10); } }
    }
    if (ib_join && ib_count && emitted) {
        ib_join->ib_count += ib_count;
        ib_join->vertex_count += emitted;
        s_d3d.merge_first_end = first + count;
        return;
    }
    /* Same BEGIN/END batch concatenation as the non-indexed path: this title's
     * fluid arrives as ~3000 DRAW_INDEX_ARRAY batches of 256, and 256 is not a
     * multiple of 3, so every batch after the first assembled out of phase. */
    if (emitted && s_d3d.merge_prev_draw && T_DRAWCNT > 0 &&
        s_d3d.merge_first_end == first && primitive == RSX_PRIMITIVE_TRIANGLES) {
        D3D12DrawRecord* pv = &T_DRAWS[T_DRAWCNT - 1];
        u32 fpnow = s_d3d.current_rsx_state
                    ? s_d3d.current_rsx_state->shader_program : 0;
        if (!ib_count && pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
            pv->fp_addr == fpnow &&
            pv->begin_epoch == (s_d3d.current_rsx_state
                                ? s_d3d.current_rsx_state->begin_epoch : 0) &&
            !pv->ib_count && pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
            pv->vertex_count += emitted;
            s_d3d.merge_first_end = first + count;
            return;
        }
    }
    if (run_P && !rephased && emitted) {
        /* starts a new record mid-run: restart on the triangle boundary */
        vp_async_drain();
        s_d3d.vp_vb_offset = rec;
        if (ib_count) s_vp_ib_off = ib_first;
        s_pend_ib_count = 0; s_ib_force_expand = 0; s_ib_delta = 0; s_ib_joining = 0;
        emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first - run_P, count + run_P);
        ib_first = s_pend_ib_first; ib_count = s_pend_ib_count; s_pend_ib_count = 0;
        { static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[TRIPHASE] indexed batch first=%u count=%u restarted %u back (new record, emitted=%u)%c", first, count, run_P, emitted, 10); } }
    }
make_record:
    if (emitted && T_DRAWCNT < MAX_DRAWS) {
        D3D12DrawRecord* dr = &T_DRAWS[T_DRAWCNT]; dr->gf = 0;
        dr->vb_byte_offset = rec;
        dr->vertex_count   = emitted;
        dr->ib_first       = ib_first;
        dr->ib_count       = ib_count;
        dr->topology       = RSX_TOPOLOGY_TRIANGLES;
        dr->textured       = s_d3d.tex_bound;
        dr->is_vp          = 1;
        dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
        dr->fp_exp32 = s_d3d.current_rsx_state ?
            ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
        dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
        dr->cmask = 0xF;
        if (s_d3d.current_rsx_state) {
            u32 _cm = s_d3d.current_rsx_state->color_mask;
            dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                      | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                      | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                      | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
        }
        dr->alpha_ctl = 0;
        if (s_d3d.current_rsx_state)
            dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                          | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                          | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
        for (int _u = 0; _u < RSX_UNITS; _u++) {
            dr->tex[_u].off = s_d3d.cur_texs[_u].off;
            dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
            dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
            dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
            dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1; dr->tex[_u].xmask = s_d3d.cur_texs[_u].xmask; dr->tex[_u].freg = s_d3d.cur_texs[_u].freg; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].address = s_d3d.cur_texs[_u].address; dr->tex[_u].ctrl0 = s_d3d.cur_texs[_u].ctrl0;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch;
            dr->tex[_u].set = s_d3d.cur_texs[_u].set;
            dr->tex_rt[_u]  = -1;
        }
        dr->tex_slot = -1;
        dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
        vs_attr_log(s_d3d.current_rsx_state, dr->vs_idx, s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0);
        dr->is_clear = 0;
        dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
        dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
        dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
        dr->disp_tag = dr->rt_off ? 0u : current_disp_tag();
                dr->zeta_off = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_zeta_offset : 0;
        dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
        if (s_d3d.current_rsx_state) {
            dr->vp_x = s_d3d.current_rsx_state->viewport_x;
            dr->vp_y = s_d3d.current_rsx_state->viewport_y;
            dr->vp_w = s_d3d.current_rsx_state->viewport_w;
            dr->vp_h = s_d3d.current_rsx_state->viewport_h;
        } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
        dr->cb_slot = T_DRAWCNT;
        dr->gf = s_gf_rec && dr->vs_idx >= 0;
                vp_record_cb(T_DRAWCNT, dr->vs_idx, dr);
        if (dr->gf) gf_open(T_DRAWCNT);
        T_DRAWCNT++;
        /* Anchor for merging the next batch of this indexed stream (never
         * into a GPU-fetch record). */
        s_d3d.merge_prev_draw = (primitive == RSX_PRIMITIVE_TRIANGLES) && !s_gf_rec;
        s_gf_rec = 0;
        s_d3d.merge_first_end = first + count;
    }
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    /* CUBEDBG=1: NV4097_SET_TEXTURE_FORMAT bit 2 is the cubemap flag, and the
     * line above throws it away with the rest of the low byte. Nothing in this
     * backend handles cube textures, so a cubemap bound for an environment
     * reflection is sampled as a plain 2D image -- which is what the chrome
     * faucet's black patches and banded escutcheons look like. */
    { static int cd = -1;
      if (cd < 0) { const char* e = getenv("CUBEDBG"); cd = e ? atoi(e) : 0; }
      if (cd && (tex->format & 4)) {
          static u32 seen[8]; static int ns = 0; int known = 0;
          for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
          if (!known && ns < 8) { seen[ns++] = offset;
              fprintf(stderr, "[CUBE] unit=%u offset=0x%08X %ux%u fmt=0x%02X"
                              " raw_format=0x%08X (cubemap bit SET)%c",
                      unit, offset, width, height, format, tex->format, 10); }
      } }

    static int log_count = 0;
    if (log_count < 10) {
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u)\n",
               unit, offset, format, width, height);
        log_count++;
    }
    /* MOVIE_BIND=1: trace movie-plane binds (640x360 Y / 320x180 U/V) with the
     * resolved EA + a content probe -- used to diagnose the Bink frame-buffer
     * ring mismatch (the draw binds a cleared buffer 0x4D80 before the one the
     * decoder actually fills). */
    if (getenv("MOVIE_BIND") &&
        ((width == 640 && height == 360) || (width == 320 && height == 180))) {
        extern u32 cellGcmResolveLocated(int local, u32 offset);
        static int _mv = 0; if (PS3_LOG_FIRST(_mv, 24)) {
            /* Resolve the SAME raw offset both ways -- as LOCAL (VRAM) and as MAIN
             * (IO-mapped) -- and count nonzero bytes over the whole plane for each.
             * If the plane the game declares (loc bits fmt&3) is zero but the OTHER
             * pool has content, the decoder wrote to a different memory space than
             * our texture resolve picked (the local-vs-main mismatch). */
            u32 loc = tex->format & 3;               /* 1=LOCAL, 2=MAIN */
            u32 ea_loc  = cellGcmResolveLocated(1, offset);
            u32 ea_main = cellGcmResolveLocated(0, offset);
            u32 plane = width * height;
            u32 nz_loc = 0, nz_main = 0;
            if (vm_base) for (u32 i = 0; i < plane; i += 137) {
                if (vm_base[ea_loc  + i]) nz_loc++;
                if (vm_base[ea_main + i]) nz_main++;
            }
            fprintf(stderr, "[mv-bind] unit=%u fmt=0x%02X loc=%u %ux%u off=0x%X | LOCAL ea=0x%08X nz=%u | MAIN ea=0x%08X nz=%u\n",
                    unit, format, loc, width, height, offset,
                    ea_loc, nz_loc, ea_main, nz_main);
        }
    }

    if (!vm_base || width == 0 || height == 0) return;

    /* Record the currently-bound atlas so subsequent quad draws sample it. The
     * actual GPU upload happens in render_frame (we have no open command list
     * here). Only the 8-bit single-channel font atlas (B8, RSX fmt base 0x81 /
     * as-seen 0xA1 with the LN flag) is wired up so far; other formats fall
     * back to untextured. */
    u32 base_fmt = format & 0x9F;   /* strip LN(0x20)/UN(0x40) flags */
    { static u32 seen[32]; static int ns = 0;   /* formats this backend never records */
      const int known = base_fmt == 0x81 || base_fmt == 0x85 || base_fmt == 0x8B ||
          (base_fmt >= 0x86 && base_fmt <= 0x88) || base_fmt == 0x9A ||
          (base_fmt >= 0x90 && base_fmt <= 0x93) ||
          (tex_fmt_all() && (base_fmt == 0x82 || base_fmt == 0x83 || base_fmt == 0x84 ||
           base_fmt == 0x8C || base_fmt == 0x8D || base_fmt == 0x94 || base_fmt == 0x95 ||
           base_fmt == 0x97 || (base_fmt >= 0x9B && base_fmt <= 0x9F)));
      if (!known) { int f = 0; for (int k = 0; k < ns; k++) if (seen[k] == base_fmt) f = 1;
        if (!f && ns < 32) { seen[ns++] = base_fmt;
          fprintf(stderr, "[TEXDROP] unit %u: format 0x%02X (%ux%u off=0x%X) not handled -- unit keeps its old texture%c",
                  unit, base_fmt, width, height, offset, 10); } } }
    /* VP path: record the latest bound texture (any supported format) so draws
     * can carry it per-draw. Location bits (format[1:0]): 1 = LOCAL, 2 = MAIN. */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (unit < RSX_UNITS &&
        (base_fmt == 0x81 /* B8 */ || base_fmt == 0x85 /* A8R8G8B8 */ ||
         base_fmt == 0x8B /* G8B8: LBP's font atlas */ ||
         (base_fmt >= 0x86 && base_fmt <= 0x88) /* DXT1/23/45 */ ||
         base_fmt == 0x9A /* W16Z16Y16X16 half-float: RTT intermediates */ ||
         /* Resistance port: every other format rsx_texture_layout decodes.
          * A bind in a format missing here was DROPPED and the unit kept the
          * previous draw's texture -- so what a surface sampled depended on
          * draw order, which changes as the camera turns: York flipping
          * between two lightings/materials pixel by pixel of mouse movement.
          * RSX_TEXFMT_ALL=0 restores the old list. */
         (tex_fmt_all() && (base_fmt == 0x82 || base_fmt == 0x83 || base_fmt == 0x84 ||
                            base_fmt == 0x8C || base_fmt == 0x8D || base_fmt == 0x94 ||
                            base_fmt == 0x95 || base_fmt == 0x97 || base_fmt == 0x9B ||
                            base_fmt == 0x9C || base_fmt == 0x9D || base_fmt == 0x9E ||
                            base_fmt == 0x9F)) ||
         /* Resistance port: DEPTH24/DEPTH16 (+float) -- shadow maps. Without
          * this the bind was dropped and the shadow pass sampled whatever the
          * unit held before: York's dark triangles under every NPC. */
         (base_fmt >= 0x90 && base_fmt <= 0x93 && depth_tex_enabled()))) {
        /* TEX_RESOLVE_AUTO=1: resolve through the page tables (local-page map
         * then IO table) instead of trusting the format's location bits. A
         * texture the guest built in main memory but tagged local resolves to
         * untouched VRAM and samples as all-zero -- geometry renders, flat. */
        { static int _ra = -1; if (_ra < 0) _ra = getenv("TEX_RESOLVE_AUTO") ? 1 : 0;
          extern u32 cellGcmResolveIO(u32);
          u32 _r = 0;
          if (_ra) _r = cellGcmResolveIO(offset);          /* IO table first */
          if (!_r) _r = cellGcmResolveLocated((tex->format & 3) == 1, offset);
          /* TEX_PROBE=1: the location bits are not always right. This title
           * binds its menu textures with a location that resolves into main
           * memory, where the bytes are all zero, while the same raw offset
           * resolved as LOCAL lands on real data -- so 100k menu draws sample
           * black and the menu never appears. When the chosen resolution is
           * empty over a sample and the other one is not, take the other. */
          { static int _tp = -1;
            if (_tp < 0) _tp = getenv("TEX_PROBE") ? 1 : 0;
            if (_tp && vm_base && _r) {
                u32 alt = cellGcmResolveLocated((tex->format & 3) != 1, offset);
                if (alt && alt != _r && alt < 0xE0000000u && _r < 0xE0000000u) {
                    u32 span = width * height / 2;
                    if (span > 4096) span = 4096;
                    u32 nz_r = 0, nz_a = 0;
                    for (u32 i = 0; i < span; i += 7) {
                        if (vm_base[_r  + i]) nz_r++;
                        if (vm_base[alt + i]) nz_a++;
                    }
                    if (nz_r == 0 && nz_a > 0) {
                        static u32 seen[32]; static int ns = 0; int known = 0;
                        for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
                        if (!known && ns < 32) { seen[ns++] = offset;
                            fprintf(stderr, "[TEX_PROBE] 0x%08X: %s empty, using %s"
                                            " (%u non-zero)%c", offset,
                                    ((tex->format & 3) == 1) ? "local" : "main",
                                    ((tex->format & 3) == 1) ? "main" : "local",
                                    nz_a, 10); }
                        _r = alt;
                    }
                }
            } }
          s_d3d.cur_texs[unit].off = _r; }
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].ctrl1 = tex->control1;
        s_d3d.cur_texs[unit].freg = tex->format; s_d3d.cur_texs[unit].filter = tex->filter;
        s_d3d.cur_texs[unit].address = tex->address; s_d3d.cur_texs[unit].ctrl0 = tex->control0;
        { static int xe = -1;
          if (xe < 0) { const char* e = getenv("RSX_TEX_EXPAND"); xe = e ? atoi(e) : 1; }
          s_d3d.cur_texs[unit].xmask = xe ? rsx_texture_expand_mask(format, tex->control1, tex->filter, tex->address) : 0; }
        s_d3d.cur_texs[unit].pitch = tex->control3 & 0xFFFFFu;
        s_d3d.cur_texs[unit].cube  = (tex->format & 4) ? 1 : 0;
        s_d3d.cur_texs[unit].mips  = (tex->format >> 16) & 0xFFFFu;
        if (getenv("TEXFMTDBG")) { static u32 seen[64]; static int ns=0;
            u32 k = (unit<<24) | (((tex->format>>8) & 0xFFu)<<16) | (width & 0xFFFFu);
            int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
            if (!f && ns < 64) { seen[ns++]=k;
                fprintf(stderr, "[TEXFMT] unit=%u fmt=0x%02X %ux%u mips=%u cube=%d off=0x%X%c",
                        unit, (tex->format >> 8) & 0xFFu, width, height,
                        (tex->format>>16)&0xFFFFu, (tex->format&4)?1:0, offset, 10); } }
        s_d3d.cur_texs[unit].set = 1;
    }
    if (base_fmt == 0x81 /* B8 */) {
        s_d3d.tex_src_offset = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        if (s_d3d.tex_w != width || s_d3d.tex_h != height) {
            /* dims changed -> resource must be (re)created in render_frame */
            s_d3d.tex_ready = 0;
        }
        s_d3d.tex_w     = width;
        s_d3d.tex_h     = height;
        s_d3d.tex_bound = 1;
        s_d3d.tex_dirty = 1;
    } else {
        s_d3d.tex_bound = 0;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    /* Log enabled vertex attributes for debugging. RSX_VTXDBG=<N> raises the
     * cap: the fixed 5 were all consumed by boot-time setup, so the layouts the
     * real draws use were never printed. */
    static int log_count = 0, log_cap = -1;
    if (log_cap < 0) { const char* e = getenv("RSX_VTXDBG"); log_cap = e ? atoi(e) : 5; }
    if (log_count < log_cap) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X, vp_load=%u, output_mask=0x%08X\n",
               state->fragment_program_addr, state->transform_program_load,
               state->vertex_attrib_output_mask);
        log_count++;
    }

    /* TODO: Look up or compile a PSO matching this shader combination.
     * For now we use the basic vertex-colored PSO for everything. */
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%X, dfactor=0x%X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_d3d, 0, sizeof(s_d3d));
    memset(s_tex_head, 0, sizeof s_tex_head); s_tex_clock = 0;
    memset(s_fp_head, 0, sizeof s_fp_head);
    s_d3d.width = width;
    s_d3d.height = height;
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Debug: dump the first N presented frames to BMP if CELLMARK_DUMP is set
     * (its numeric value when > 1, else 24). */
    {
        const char* dv = getenv("CELLMARK_DUMP");
        int n = dv ? atoi(dv) : 0;
        s_d3d.dump_frames_left = dv ? (n > 1 ? n : 24) : 0;
        /* CELLMARK_DUMP_SKIP=N: ignore the first N presents before dumping.
         * The opening frames are the loading screen; the interesting content
         * only appears once the sim SPUs have advanced the scene. */
        { const char* sv = getenv("CELLMARK_DUMP_SKIP");
          s_d3d.dump_skip_left = sv ? atoi(sv) : 0; }
    }

    /* Create window */
    {
        extern char g_rsx_title_base[128];
        snprintf(g_rsx_title_base, sizeof(g_rsx_title_base), "%s",
                 title ? title : "ps3recomp");
    }
    s_d3d.hwnd = create_window(width, height, title);
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        return -1;
    }

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    rsx_set_backend(&s_d3d12_backend);

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;

    wait_for_gpu();

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    if (s_d3d.hwnd) DestroyWindow(s_d3d.hwnd);

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

int rsx_d3d12_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_d3d.window_closed ? -1 : 0;
}

void rsx_d3d12_backend_present(void)
{
    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u clear_presents=%u\n",
               T_DRAWCNT, s_dbg_clears_since_present, s_clear_presents);
    const u32 clears_this_flip = s_dbg_clears_since_present;
    s_dbg_clears_since_present = 0;

    /* Once frame-boundary presents are active (d3d12_clear presents each
     * completed frame as the drain crosses into the next one), the ticker
     * present would only ever show the partially-accumulated NEXT frame --
     * that partial present right after the FIFO ring recycle was the visible
     * blink. Keep the ticker present solely as the boot-time fallback (before
     * the first framed clear arrives). */
    /* Resistance port: a guest frame with NO clear in it (Fall of Man's movie
     * player draws one full-screen quad straight over the last frame) never
     * reaches the clear-boundary present, so its draws piled up unpresented
     * and the cinematic stayed black. A flip whose frame cleared nothing is
     * presented here; frames that clear keep the boundary present.
     * PRESENT_NOCLEAR=0 restores the old behaviour. */
    if (s_clear_presents > 0) {
        static int nc = -1;
        if (nc < 0) { const char* e = getenv("PRESENT_NOCLEAR"); nc = e ? atoi(e) : 1; }
        int display = 0;
        for (u32 _i = 0; _i < T_DRAWCNT && _i < MAX_DRAWS; _i++)
            if (!T_DRAWS[_i].is_clear && T_DRAWS[_i].rt_off == 0) { display = 1; break; }
        if (!(nc && clears_this_flip == 0 && display))
            return;
        { static int _n = 0;
          if (PS3_LOG_FIRST(_n, 3))
              fprintf(stderr, "[PRESENT] clear-less frame: presenting %u draws on the flip%c",
                      T_DRAWCNT, 10); }
    }

    /* Same display gate as d3d12_present: a batch of offscreen pass work only
     * (render-to-texture) keeps accumulating until its composite arrives.
     * Empty batches present only until the first real frame -- after that an
     * empty present is a flip/drain race and wipes the screen for a frame
     * (wave: black flashes and layout flicker between frames). */
    static int s_seen_content = 0;
    int has_display = (T_DRAWCNT == 0 && !s_seen_content);
    for (u32 _i = 0; _i < T_DRAWCNT && _i < MAX_DRAWS; _i++)
        if (!T_DRAWS[_i].is_clear && T_DRAWS[_i].rt_off == 0) {
            has_display = 1;
            break;
        }
    /* The guest-framebuffer present needs the pass to run at all. */
    { static int gfb = -1;
      if (gfb < 0) { const char* e = getenv("GCM_GUEST_FB"); gfb = e ? 1 : 0; }
      if (gfb) has_display = 1; }
    if (s_composite_src) has_display = 1;
    if (has_display && T_DRAWCNT > 0) s_seen_content = 1;

    /* VP_SUBMIT=<N>: has_display gates render_frame() entirely, so a batch whose
     * records all target an OFFSCREEN rt (rt_off != 0) presents without ever
     * running the draw pass -- the backbuffer then shows only the clear, which
     * reads as "nothing rasterizes" from every downstream check. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
      if (cap && T_DRAWCNT && vps_gate(&n, cap)) {
        u32 onscreen = 0, offscreen = 0, clears = 0;
        for (u32 _i = 0; _i < T_DRAWCNT && _i < MAX_DRAWS; _i++) {
            if (T_DRAWS[_i].is_clear) { clears++; continue; }
            if (T_DRAWS[_i].rt_off) offscreen++; else onscreen++;
        }
        { /* Which surfaces the batch's draws actually target -- "offscreen=57"
           * does not say whether they all go to one RT or scatter. */
          u32 offs[8]; u32 cnt[8]; int nd = 0;
          for (u32 _i = 0; _i < T_DRAWCNT && _i < MAX_DRAWS; _i++) {
              if (T_DRAWS[_i].is_clear) continue;
              u32 o = T_DRAWS[_i].rt_off; int f = -1;
              for (int _j = 0; _j < nd; _j++) if (offs[_j] == o) { f = _j; break; }
              if (f < 0) { if (nd < 8) { offs[nd] = o; cnt[nd] = 1; nd++; } }
              else cnt[f]++;
          }
          { u32 vp = 0, novp = 0;
            for (u32 _i = 0; _i < T_DRAWCNT && _i < MAX_DRAWS; _i++) {
                if (T_DRAWS[_i].is_clear) continue;
                if (T_DRAWS[_i].is_vp) vp++; else novp++;
            }
            /* The execution loop skips anything without is_vp, so a batch full
             * of !is_vp records is counted, gated and then never drawn. */
            fprintf(stderr, "[PRESENTGATE] is_vp=%u not_vp=%u%c", vp, novp, 10); }
          fprintf(stderr, "[PRESENTGATE] targets:");
          for (int _j = 0; _j < nd; _j++)
              fprintf(stderr, " 0x%08X x%u", offs[_j], cnt[_j]);
          fprintf(stderr, "%c", 10); }
        fprintf(stderr, "[PRESENTGATE] records=%u onscreen=%u offscreen=%u clears=%u"
                        " has_display=%d seen_content=%d -> render_frame=%s\n",
                T_DRAWCNT, onscreen, offscreen, clears, has_display,
                s_seen_content, (s_d3d.initialized && has_display) ? "YES" : "SKIPPED"); } }

    /* Execute the batch whenever it has draws. Gating the whole call on
     * has_display meant a render-to-texture pass -- every draw targeting an
     * offscreen surface -- was DISCARDED rather than deferred, so the texture
     * it produces was never written and whatever sampled it later read an
     * empty resource. Only the Present needs onscreen content. */
    if (s_d3d.initialized && (has_display || T_DRAWCNT > 0)) {
        { extern void rsx_reset_upload_claims(void);
          static int remap = -1;
          if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
          if (remap) rsx_reset_upload_claims(); }
        s_present_this_frame = has_display;
        InterlockedIncrement(&s_rfr[RFR_FLIP]);
        render_frame();
        s_present_this_frame = 1;
    }
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}

#endif /* _WIN32 */
