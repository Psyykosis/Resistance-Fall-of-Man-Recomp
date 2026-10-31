/* ===========================================================================
 * Resistance port (v165): AMD FSR 3.1 temporal upscaler, NATIVE AA mode.
 *
 * RSX_FSR=1 turns it on (default 0). Needs amd_fidelityfx_loader_dx12.dll and
 * amd_fidelityfx_upscaler_dx12.dll beside the exe (AMD FidelityFX SDK, MIT /
 * AMD redistributable licence); without them it logs once and stays off.
 *
 * What FSR needs and where it comes from here:
 *   colour   the game's scene surface (the display-sized offscreen colour
 *            target most draws go to -- 0x740000 in Fall of Man), just before
 *            the first draw that samples it (the post / HUD composite).
 *   jitter   a sub-pixel Halton offset added to every scene draw's viewport
 *            offset (vp_posoffset.xy), i.e. to clip space, at record time.
 *   depth +  a separate pass that re-runs this frame's opaque scene draws
 *   motion   with a vertex-shader variant that evaluates the guest transform
 *            twice -- once with this draw's constants, once with the SAME
 *            draw's constants from the previous frame (matched by program,
 *            mesh, texture and occurrence order) -- and writes the screen-space
 *            difference (R16G16F) plus depth (R32F), depth-tested LESS_EQUAL
 *            against the scene's own depth so only visible surfaces write.
 *            Skinned characters and moving props get real motion this way:
 *            their bone / world matrices are vertex constants on RSX.
 * The result is copied back over the scene surface, so everything after it
 * (bloom, colour grade, HUD) is untouched and un-jittered.
 * RSX_FSR_SHARP=<0..1> (default 0.25): FSR's RCAS sharpening.
 * RSX_FSR_DEBUG=1: FSR API validation messages to the log.
 * =========================================================================*/
#include "ffx/ffx_api.h"
#include "ffx/ffx_api_types.h"
#include "ffx/ffx_upscale.h"
/* From ffx/dx12/ffx_api_dx12.h (that header is C++ only). */
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12, 0x02)
struct ffxCreateBackendDX12Desc { ffxCreateContextDescHeader header; ID3D12Device* device; };

#define FSR_RING 8u   /* v184: 4 -> 8. The record thread ran far enough ahead of the GPU to
                       * rewrite a history slot the GPU had not read yet: a still
                       * camera got 40% of the screen with garbage motion (floor
                       * pixels all "coming from" one spot, the gun moving 1400 px)
                       * = the FSR flicker. Plus a lead cap, see FSR_LEAD. */
#define FSR_LEAD 4u   /* record may be at most this many frames ahead of the last FSR dispatch */
/* v189: 3072 -> up to 8192. Fields of grass are 5000+ scene draws; every draw past
 * the limit got no motion, so whole patches of grass and trees smeared whenever
 * enough of them were on screen ("only at some angles"). The buffer is
 * FSR_RING x slots x 8.4 KB (550 MB of upload memory at 8192); if that can't be
 * had it steps down. RSX_FSR_SLOTS caps it. */
#define FSR_HIST_SLOTS  8192u
static u32 s_fsr_slots = FSR_HIST_SLOTS;   /* what was actually allocated */
#define FSR_HIST_STRIDE 8448u            /* vp_c[512] + posscale/posoffset/branch = 8240 B, 256-aligned */
#define FSR_MAP_N       32768u           /* power of two (v189: 8192 -> 32768 with the slots) */

int fsr_mode(void)
{
    static int m = -1;
    if (m < 0) { const char* e = getenv("RSX_FSR"); m = e ? atoi(e) : 0; if (m < 0) m = 0;
                 if (m) fprintf(stderr, "[FSR] requested (mode %d: native AA)%c", m, 10); }
    return m;
}
static volatile LONG g_fsr_live;          /* render thread has a context: record side jitters */
static volatile u32  g_fsr_scene_off;     /* render thread's pick of the scene colour surface */
static u32 s_fsr_rec_frame = 1;           /* FIFO thread: presented frames */

/* ---- jitter: Halton(2,3), 8 phases at 1x (FSR's own sequence length) ---- */
static float fsr_halton(u32 i, u32 b) { float f = 1.0f, r = 0.0f; while (i) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; }
/* v184 diag: Home cycles 0 normal / 1 no auto-exposure / 2 no jitter /
 * 3 FSR bypassed (no jitter, no FSR) -- live A/B for the flicker. */
static volatile int s_fsr_ab;
static int s_fsr_ab_ctx;   /* auto-exposure state the context was made with */
static void fsr_jitter(u32 frame, float* jx, float* jy)
{
    static int jon = -1; if (jon < 0) { const char* e = getenv("RSX_FSR_JITTER"); jon = e ? atoi(e) : 1; }
    if (!jon || s_fsr_ab >= 2) { *jx = *jy = 0.0f; return; }   /* 4 too */   /* v184 diag: RSX_FSR_JITTER=0 / Home modes 2,3 */
    const u32 i = (frame % 8u) + 1u;
    *jx = fsr_halton(i, 2u) - 0.5f;
    *jy = fsr_halton(i, 3u) - 0.5f;
}

/* ---- previous-frame constant history (record side writes, GPU reads) ---- */
static ID3D12Resource* s_fsr_hist; static u8* s_fsr_hist_mapped; static int s_fsr_hist_fail;
static int fsr_hist_init(void)
{
    if (s_fsr_hist_mapped) return 1;
    if (s_fsr_hist_fail || !s_d3d.device) return 0;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    { const char* e = getenv("RSX_FSR_SLOTS"); if (e && atoi(e) >= 256 && (u32)atoi(e) < s_fsr_slots) s_fsr_slots = (u32)atoi(e); }
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (;;) {
        rd.Width = (u64)FSR_RING * s_fsr_slots * FSR_HIST_STRIDE;
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_fsr_hist))) break;
        s_fsr_hist = NULL;
        if (s_fsr_slots <= 2048u) { s_fsr_hist_fail = 1; fprintf(stderr, "[FSR] history buffer allocation failed%c", 10); return 0; }
        s_fsr_slots = s_fsr_slots > 4096u ? 4096u : 2048u;
    }
    fprintf(stderr, "[FSR] motion history: %u draws a frame%c", s_fsr_slots, 10);
    D3D12_RANGE nr = {0, 0}; void* m = NULL;
    s_fsr_hist->lpVtbl->Map(s_fsr_hist, 0, &nr, &m);
    if (!m) { s_fsr_hist_fail = 1; return 0; }
    s_fsr_hist_mapped = (u8*)m;
    return 1;
}

typedef struct { u64 key; u32 val; u32 gen; u32 n; } FsrMapE;   /* n: v189 draws of this key so far (ordinal) */
static FsrMapE s_fsr_map[2][FSR_MAP_N];  static u32 s_fsr_map_gen[2], s_fsr_map_frame[2];
static FsrMapE s_fsr_occ[FSR_MAP_N];     static u32 s_fsr_occ_gen;
/* v189: exact-match lookups by (key, hfull) and (key, hvar), same generations as
 * s_fsr_map. The match used to walk the key's whole instance chain (up to 1024)
 * per draw: a grass field is thousands of draws of one key, so that walk was
 * quadratic -- 18% of the FIFO thread in Nottingham. */
static FsrMapE s_fsr_exf[2][FSR_MAP_N], s_fsr_exv[2][FSR_MAP_N];
/* v189: (key, ordinal) -> slot: the n-th draw of a key last frame. Swaying grass
 * never matches exactly, and searching 64 instances' signatures per clump was
 * the FIFO thread's top cost; the same-ordinal instance is tried first and kept
 * when it is close (draw order is stable while the view is). */
static FsrMapE s_fsr_ord[2][FSR_MAP_N];
static int s_fsr_map_cur; static u32 s_fsr_tab_frame, s_fsr_hist_n;
#define FSR_SIG_ROWS 16u   /* v184j: 32 -> 16 (v184e raised it; exact hashes now do the heavy lifting) */
static float s_fsr_sig[FSR_RING][FSR_HIST_SLOTS][FSR_SIG_ROWS * 4];   /* v177: per slot constant signature */
static u32   s_fsr_next[FSR_RING][FSR_HIST_SLOTS];                    /* same-key chain (0xFFFFFFFF ends) */


static u64 fsr_mix(u64 h, u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0xFF51AFD7ED558CCDull; }
static FsrMapE* fsr_find(FsrMapE* t, u32 gen, u64 key, int insert)
{
    u32 i = (u32)(key ^ (key >> 29)) & (FSR_MAP_N - 1u);
    for (u32 n = 0; n < FSR_MAP_N; n++, i = (i + 1u) & (FSR_MAP_N - 1u)) {
        if (t[i].gen != gen) { if (!insert) return NULL; t[i].gen = gen; t[i].key = key; t[i].val = 0; t[i].n = 0; return &t[i]; }
        if (t[i].key == key) return &t[i];
    }
    return NULL;
}
/* v190b: start the cache miss early. The tables are 768 KB each (eight of
 * them), so every lookup missed the cache; fsr_find was ~6% of the FIFO thread
 * in a Titan fight. The keys are known well before the lookups. */
static void fsr_pf(const FsrMapE* t, u64 key)
{
    _mm_prefetch((const char*)&t[(u32)(key ^ (key >> 29)) & (FSR_MAP_N - 1u)], _MM_HINT_T0);
}

/* Called from vp_record_cb with the draw's finished constant slot. */
#define FSR_VARY_VS 1024
static u32 s_fsr_lastf[FSR_VARY_VS];
/* frame+1 of that last draw ^ */
static u64 s_fsr_hfull[FSR_RING][FSR_HIST_SLOTS], s_fsr_hvar[FSR_RING][FSR_HIST_SLOTS];   /* v184h */
static u8 s_fsr_vary[FSR_VARY_VS][512];          /* v184g: rows seen to differ between draws of a program */
static float s_fsr_lastc[FSR_VARY_VS][512 * 4];  /* last draw's constants per program (16 MB) */
static volatile u32 s_fsr_st_rec, s_fsr_st_match, s_fsr_st_over, s_fsr_st_wait, s_fsr_st_exact;   /* v184 stats */
static volatile u32 s_fsr_disp_frame; static volatile ULONGLONG s_fsr_disp_ms;   /* v184: last FSR dispatch (render thread) */
static void fsr_record(D3D12DrawRecord* dr, int vs_idx, char* dst, const char* csrc, const float* epi)
{
    if (!dr) return;
    dr->fsr = 0;
    if (!g_fsr_live || !dr->rt_off || dr->rt_off != g_fsr_scene_off) return;
    if (!fsr_hist_init()) return;
    s_fsr_st_rec++;
    const u32 fr = s_fsr_rec_frame;
    if (fr != s_fsr_tab_frame) {
        /* v184: don't overwrite history the GPU may still read. The previous
         * frame's batch is already with the render thread here, so this only
         * waits for work that is in flight (30 ms cap; off when FSR is idle). */
        if (s_fsr_disp_frame && GetTickCount64() - s_fsr_disp_ms < 250) {
            const ULONGLONG t0 = GetTickCount64();
            while (fr - s_fsr_disp_frame > FSR_LEAD && GetTickCount64() - t0 < 30) { s_fsr_st_wait++; SwitchToThread(); }
        }
        s_fsr_tab_frame = fr; s_fsr_hist_n = 0;
        s_fsr_map_cur ^= 1; s_fsr_map_gen[s_fsr_map_cur]++; s_fsr_map_frame[s_fsr_map_cur] = fr;
        s_fsr_occ_gen++;
    }
    if (s_fsr_hist_n >= s_fsr_slots) {
        /* v184: out of history slots (York: 4700 scene draws > 3072). Still
         * jitter it -- an un-jittered draw among jittered ones z-fights its
         * own light passes and shakes against the rest. No motion for it. */
        s_fsr_st_over++;
        float jx, jy; fsr_jitter(fr, &jx, &jy);
        const float nx = 2.0f * jx / (float)SC(s_d3d.width), ny = -2.0f * jy / (float)SC(s_d3d.height);
        float* vpx = (float*)(dst + 8192);
        vpx[4] = epi[4] + nx; vpx[5] = epi[5] + ny;
        return;
    }
    /* which draw is this: program, mesh size, material, nth such draw this frame */
    u64 h = 0x84222325CBF29CE4ull;
    h = fsr_mix(h, (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) ? s_d3d.vp_vs[vs_idx].hash : 0xFFFFFFFFu);
    h = fsr_mix(h, dr->fp_addr);
    h = fsr_mix(h, dr->ib_count ? dr->ib_count : dr->vertex_count);
    h = fsr_mix(h, dr->tex[0].raw);
    h = fsr_mix(h, ((u64)dr->gf << 32) | dr->tex[1].raw);
    fsr_pf(s_fsr_map[s_fsr_map_cur], h); fsr_pf(s_fsr_map[s_fsr_map_cur ^ 1], h);

    const u32 slot = s_fsr_hist_n++;
    const u32 ring = fr % FSR_RING;
    const u64 cur_off = ((u64)ring * s_fsr_slots + slot) * FSR_HIST_STRIDE;
    u8* hp = s_fsr_hist_mapped + cur_off;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && !s_d3d.vp_vs[vs_idx].c_all) {
        const VPVSEntry* ve = &s_d3d.vp_vs[vs_idx];
        for (u32 r = 0; r < ve->crun_n; r++)
            memcpy(hp + (size_t)ve->crun[r][0] * 16, csrc + (size_t)ve->crun[r][0] * 16, (size_t)ve->crun[r][1] * 16);
    } else memcpy(hp, csrc, 512u * 16u);
    /* v165e: the constants come from the CPU-side bank, never read back from
     * dst -- that is an upload heap (write-combined), and reading it back cost
     * the FIFO thread ~60% of its time. Only 48 bytes of epilogue come from dst. */
    memcpy(hp + 8192, epi, 48);                 /* posscale, posoffset (un-jittered), branch -- v181: from the local copy */

    /* v177: match last frame's draw by CONSTANTS among draws with the same
     * program/mesh/material. York draws thousands of repeated meshes (grass,
     * debris, props: 4000 of 4700 scene draws share their key with others) and
     * the game re-orders them as the view moves, so "the nth such draw" paired
     * different instances -> wrong motion -> distant things shimmered under FSR.
     * Signature: 6 of the program's constant rows, evenly spread (world matrix
     * rows differ between instances; the camera's are shared). */
    float* sig = s_fsr_sig[ring][slot];
    { u32 rows[512]; u32 nr = 0;
      if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && s_d3d.vp_vs[vs_idx].crun_n) {   /* v184f: c_all programs too */
          const VPVSEntry* ve = &s_d3d.vp_vs[vs_idx];
          for (u32 r = 0; r < ve->crun_n && nr < 512; r++)
              for (u32 k = 0; k < ve->crun[r][1] && nr < 512; k++) rows[nr++] = ve->crun[r][0] + k;
      } else for (u32 k = 0; k < 16; k++) rows[nr++] = k;
      /* v184g: learn, per program, which rows differ from one draw to the
       * next (per-instance world transform, colours) and put those first in
       * the signature; rows shared by every instance (the camera) only fill
       * what is left. Sampling missed a world row (e.g. the one holding x
       * translation) and instances of a piece paired with each other. */
      /* v184h: exact identity first. hfull = every row the program reads
       * (a still camera: a static instance matches itself exactly). hvar =
       * only the rows seen to differ between draws of this program within a
       * frame (per-instance world transform, colour): unchanged for a static
       * instance even while the camera moves. Sampled rows (sig) are only the
       * fallback for things that really move. */
      u64 hf = 0xCBF29CE484222325ull, hv = 0xCBF29CE484222325ull; int nv = 0;
      const u8* vary = NULL;
      if (vs_idx >= 0 && vs_idx < FSR_VARY_VS) {
          u8* vy = s_fsr_vary[vs_idx]; float* last = s_fsr_lastc[vs_idx];
          const int same = s_fsr_lastf[vs_idx] == fr + 1u; s_fsr_lastf[vs_idx] = fr + 1u;
          for (u32 i = 0; i < nr; i++) {
              const u32 r = rows[i];
              if (memcmp(last + r * 4, csrc + (size_t)r * 16, 16)) { if (same) vy[r] = 1; memcpy(last + r * 4, csrc + (size_t)r * 16, 16); }
          }
          vary = vy;
      }
      for (u32 i = 0; i < nr; i++) {
          const u32 r = rows[i]; const u64* q = (const u64*)(csrc + (size_t)r * 16);
          hf = (hf ^ q[0]) * 0x100000001B3ull; hf = (hf ^ q[1]) * 0x100000001B3ull;
          if (vary && vary[r]) { hv = (hv ^ q[0]) * 0x100000001B3ull; hv = (hv ^ q[1]) * 0x100000001B3ull; hv ^= r; nv++; }
      }
      s_fsr_hfull[ring][slot] = hf; s_fsr_hvar[ring][slot] = nv ? hv : 0;
      { const int pm2 = s_fsr_map_cur ^ 1;
        fsr_pf(s_fsr_exf[pm2], fsr_mix(h, hf)); fsr_pf(s_fsr_exf[s_fsr_map_cur], fsr_mix(h, hf));
        if (nv) { fsr_pf(s_fsr_exv[pm2], fsr_mix(h, hv) ^ 1u); fsr_pf(s_fsr_exv[s_fsr_map_cur], fsr_mix(h, hv) ^ 1u); } }
      for (u32 q = 0; q < FSR_SIG_ROWS; q++) {
          const u32 row = nr ? rows[(u32)(((u64)q * nr) / FSR_SIG_ROWS)] : 0u;
          memcpy(sig + q * 4, csrc + (size_t)row * 16, 16);
      }
    }
    u64 prev_off = cur_off;                     /* no match: "did not move" */
    dr->fsr_mt = 0; dr->fsr_chain = 0; dr->fsr_bd = -1.0f; dr->fsr_slot = slot; dr->fsr_pslot = 0xFFFFFFFFu;
    const int pm = s_fsr_map_cur ^ 1;
    FsrMapE* ce = fsr_find(s_fsr_map[s_fsr_map_cur], s_fsr_map_gen[s_fsr_map_cur], h, 1);
    const u32 ord = ce ? ce->n++ : 0xFFFFFFFFu;
    if (s_fsr_map_frame[pm] == fr - 1u) {
        FsrMapE* e = fsr_find(s_fsr_map[pm], s_fsr_map_gen[pm], h, 0);
        if (e) {
            const u32 pr = (fr - 1u) % FSR_RING;
            u32 best = 0xFFFFFFFFu; float bd = 3.0e38f; int n = 0;
            { const u64 hf = s_fsr_hfull[ring][slot], hv = s_fsr_hvar[ring][slot];
              u32 bv = 0xFFFFFFFFu; int nn = 0;
              { const FsrMapE* xf = fsr_find(s_fsr_exf[pm], s_fsr_map_gen[pm], fsr_mix(h, hf), 0);
                if (xf && xf->val) best = xf->val - 1u;
                else if (hv) { const FsrMapE* xv = fsr_find(s_fsr_exv[pm], s_fsr_map_gen[pm], fsr_mix(h, hv) ^ 1u, 0);
                               if (xv && xv->val) bv = xv->val - 1u; } }
              dr->fsr_chain = (u16)nn; dr->fsr_mt = best != 0xFFFFFFFFu ? 1 : 0;
              if (best == 0xFFFFFFFFu) { best = bv; if (bv != 0xFFFFFFFFu) dr->fsr_mt = 2; }
              if (best != 0xFFFFFFFFu) { bd = 0.0f; n = 1 << 20; s_fsr_st_exact++; } }
            if (best == 0xFFFFFFFFu && ord != 0xFFFFFFFFu) {   /* v189: same-ordinal instance first */
                const FsrMapE* xo = fsr_find(s_fsr_ord[pm], s_fsr_map_gen[pm], fsr_mix(h, ord) ^ 2u, 0);
                if (xo && xo->val) {
                    const u32 c = xo->val - 1u; const float* ps = s_fsr_sig[pr][c];
                    float dsum = 0.0f, mag = 0.0f;
                    for (u32 k = 0; k < FSR_SIG_ROWS * 4; k++) {
                        const float df = ps[k] - sig[k]; dsum += df < 0.0f ? -df : df;
                        mag += sig[k] < 0.0f ? -sig[k] : sig[k];
                    }
                    if (dsum == dsum) { best = c; bd = dsum; if (dsum <= 0.02f * mag) n = 1 << 20; else n = 32; }
                }
            }
            for (u32 c = e->val ? e->val - 1u : 0xFFFFFFFFu; c != 0xFFFFFFFFu && n < 64; c = s_fsr_next[pr][c], n++) {   /* v184j: 256 -> 64 */
                const float* ps = s_fsr_sig[pr][c]; float dsum = 0.0f;
                for (u32 k = 0; k < FSR_SIG_ROWS * 4; k += 4) {
                    for (u32 j = 0; j < 4; j++) { const float df = ps[k + j] - sig[k + j]; dsum += df < 0.0f ? -df : df; }
                    if (dsum >= bd) break;   /* v184e: can't win */
                }
                if (!(dsum == dsum)) continue;
                if (dsum < bd) { bd = dsum; best = c; if (dsum == 0.0f) break; }
            }
            if (best != 0xFFFFFFFFu) { prev_off = ((u64)pr * s_fsr_slots + best) * FSR_HIST_STRIDE; s_fsr_st_match++;
                                       if (!dr->fsr_mt) dr->fsr_mt = 3; dr->fsr_bd = bd; dr->fsr_pslot = best; }
        }
    }
    { const u64 hf = s_fsr_hfull[ring][slot], hv = s_fsr_hvar[ring][slot];   /* v189 exact tables */
      FsrMapE* xf = fsr_find(s_fsr_exf[s_fsr_map_cur], s_fsr_map_gen[s_fsr_map_cur], fsr_mix(h, hf), 1);
      if (xf) xf->val = slot + 1u;
      if (hv) { FsrMapE* xv = fsr_find(s_fsr_exv[s_fsr_map_cur], s_fsr_map_gen[s_fsr_map_cur], fsr_mix(h, hv) ^ 1u, 1);
                if (xv) xv->val = slot + 1u; } }
    if (ord != 0xFFFFFFFFu) { FsrMapE* xo = fsr_find(s_fsr_ord[s_fsr_map_cur], s_fsr_map_gen[s_fsr_map_cur], fsr_mix(h, ord) ^ 2u, 1);
                              if (xo) xo->val = slot + 1u; }
    if (ce) {   /* push this slot onto the key's chain (val = head + 1, 0 = empty) */
        s_fsr_next[ring][slot] = ce->val ? ce->val - 1u : 0xFFFFFFFFu;
        ce->val = slot + 1u;
    }

    float jx, jy; fsr_jitter(fr, &jx, &jy);
    const float nx = 2.0f * jx / (float)SC(s_d3d.width), ny = -2.0f * jy / (float)SC(s_d3d.height);
    float* vpx = (float*)(dst + 8192);
    vpx[4] = epi[4] + nx; vpx[5] = epi[5] + ny;   /* v181: write only (dst is write-combined) */
    dr->fsr = 1; dr->fsr_frame = fr; dr->fsr_prev = (u32)prev_off; dr->fsr_jnx = nx; dr->fsr_jny = ny;
}
/* FIFO thread, once per presented frame. */
static void fsr_record_frame_end(void) { s_fsr_rec_frame++; }

/* ======================= render-thread side ============================= */
static HMODULE s_ffx_dll; static int s_ffx_dll_fail;
static PfnFfxCreateContext  p_ffxCreateContext;
static PfnFfxDestroyContext p_ffxDestroyContext;
static PfnFfxDispatch       p_ffxDispatch;
static PfnFfxQuery          p_ffxQuery;
static ffxContext s_fsr_ctx; static u32 s_fsr_cw, s_fsr_ch; static int s_fsr_ctx_fail;
static ID3D12Resource *s_fsr_mv, *s_fsr_z, *s_fsr_out;
static D3D12_RESOURCE_STATES s_fsr_mv_st, s_fsr_z_st, s_fsr_out_st;
/* v189: reactive mask (R8): blended draws (leaves, smoke, sparks) mark where FSR
 * should lean on the current frame instead of history. They write no motion or
 * depth, so with no mask a tree's leaves reprojected with the sky behind them and
 * smeared up close. RSX_FSR_REACTIVE=<0..1> value written (default 0.6, 0 = off). */
static ID3D12Resource *s_fsr_rx; static D3D12_RESOURCE_STATES s_fsr_rx_st;
static float fsr_rx_val(void) { static float v = -1.0f; if (v < 0.0f) { const char* e = getenv("RSX_FSR_REACTIVE"); v = e ? (float)atof(e) : 0.6f; if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; } return v; }
static ID3D12DescriptorHeap* s_fsr_rtv_heap;
static ID3D12RootSignature* s_mv_rs; static ID3DBlob* s_mv_ps; static ID3DBlob* s_rx_ps; static int s_mv_init_fail;
static int s_fsr_slot = -1;          /* off_rt slot of the frame's jittered scene */
static u32 s_fsr_frame_r;            /* record frame number of the scene being built */
static int s_fsr_mv_started, s_fsr_done;
static u32 s_fsr_last_frame;         /* last frame dispatched (reset on discontinuity) */
static u32 s_fsr_cnt[MAX_OFF_RTS];   /* scene pick: display-sized draws per slot this frame */
static LARGE_INTEGER s_fsr_t_last;

static void fsr_msg(uint32_t type, const wchar_t* msg)
{
    static int n = 0; if (n++ > 60) return;
    char buf[512]; int k = 0;
    for (; msg && msg[k] && k < 510; k++) buf[k] = (char)(msg[k] < 128 ? msg[k] : '?');
    buf[k] = 0;
    fprintf(stderr, "[FSR] %s: %s%c", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf, 10);
}

static ID3D12Resource* fsr_tex(u32 w, u32 h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st, const float* clear)
{
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = f; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; td.Flags = fl;
    D3D12_CLEAR_VALUE cv; memset(&cv, 0, sizeof cv); cv.Format = f;
    if (clear) memcpy(cv.Color, clear, 16);
    ID3D12Resource* r = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td, st,
            (fl & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &cv : NULL, &IID_ID3D12Resource, (void**)&r))) return NULL;
    return r;
}
static void fsr_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES* cur, D3D12_RESOURCE_STATES to)
{
    if (!r || *cur == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.StateBefore = *cur; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    *cur = to;
}
static D3D12_CPU_DESCRIPTOR_HANDLE fsr_rtv(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h; s_fsr_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fsr_rtv_heap, &h);
    h.ptr += (u64)i * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return h;
}
static void fsr_release(void)
{
    if (s_fsr_ctx && p_ffxDestroyContext) { wait_for_gpu(); p_ffxDestroyContext(&s_fsr_ctx, NULL); }
    s_fsr_ctx = NULL;
    if (s_fsr_mv)  { grave_defer(s_fsr_mv);  s_fsr_mv = NULL; }
    if (s_fsr_z)   { grave_defer(s_fsr_z);   s_fsr_z = NULL; }
    if (s_fsr_rx)  { grave_defer(s_fsr_rx);  s_fsr_rx = NULL; }
    if (s_fsr_out) { grave_defer(s_fsr_out); s_fsr_out = NULL; }
    s_fsr_cw = s_fsr_ch = 0;
}
/* Context + targets for a w x h scene (render = display: native AA). */
static int fsr_ensure(u32 w, u32 h, DXGI_FORMAT colour_fmt)
{
    if (!fsr_mode() || s_ffx_dll_fail || s_fsr_ctx_fail) return 0;
    if (s_fsr_ctx && s_fsr_cw == w && s_fsr_ch == h) return 1;
    if (!s_ffx_dll) {
        s_ffx_dll = LoadLibraryA("amd_fidelityfx_loader_dx12.dll");
        if (s_ffx_dll) {
            p_ffxCreateContext  = (PfnFfxCreateContext)(void*)GetProcAddress(s_ffx_dll, "ffxCreateContext");
            p_ffxDestroyContext = (PfnFfxDestroyContext)(void*)GetProcAddress(s_ffx_dll, "ffxDestroyContext");
            p_ffxDispatch       = (PfnFfxDispatch)(void*)GetProcAddress(s_ffx_dll, "ffxDispatch");
            p_ffxQuery          = (PfnFfxQuery)(void*)GetProcAddress(s_ffx_dll, "ffxQuery");
        }
        if (!s_ffx_dll || !p_ffxCreateContext || !p_ffxDispatch || !p_ffxDestroyContext) {
            s_ffx_dll_fail = 1;
            fprintf(stderr, "[FSR] amd_fidelityfx_loader_dx12.dll not found next to the exe (or incomplete) -- FSR off%c", 10);
            return 0;
        }
    }
    fsr_release();
    struct ffxCreateBackendDX12Desc be; memset(&be, 0, sizeof be);
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; be.device = s_d3d.device;
    struct ffxCreateContextDescUpscale cd; memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    cd.maxRenderSize.width = w; cd.maxRenderSize.height = h;
    cd.maxUpscaleSize.width = w; cd.maxUpscaleSize.height = h;
    /* v165c: auto exposure (AMD's recommendation; without an exposure input the
     * first build came out black). Debug checking on unless RSX_FSR_DEBUG=0. */
    cd.flags = s_fsr_ab == 1 ? 0u : FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    s_fsr_ab_ctx = s_fsr_ab == 1;
    { const char* e = getenv("RSX_FSR_LINEAR"); if (!(e && atoi(e))) cd.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE; }
    { const char* e = getenv("RSX_FSR_DEBUG"); if (!e || atoi(e)) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING; }
    cd.fpMessage = fsr_msg;
    ffxReturnCode_t rc = p_ffxCreateContext(&s_fsr_ctx, &cd.header, NULL);
    if (rc != FFX_API_RETURN_OK || !s_fsr_ctx) {
        s_fsr_ctx = NULL; s_fsr_ctx_fail = 1;
        fprintf(stderr, "[FSR] context creation failed (%u) -- FSR off%c", (unsigned)rc, 10);
        return 0;
    }
    if (p_ffxQuery) {
        struct ffxQueryGetProviderVersion pv; memset(&pv, 0, sizeof pv);
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (p_ffxQuery(&s_fsr_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
            fprintf(stderr, "[FSR] upscaler provider: %s%c", pv.versionName, 10);
    }
    if (!s_fsr_rtv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.NumDescriptors = 3; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_fsr_rtv_heap);
    }
    static const float zero4[4] = {0, 0, 0, 0}, one4[4] = {1, 1, 1, 1};
    s_fsr_mv  = fsr_tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, zero4);
    s_fsr_z   = fsr_tex(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, one4);
    s_fsr_rx  = fsr_tex(w, h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, zero4);
    s_fsr_rx_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_fsr_out = fsr_tex(w, h, colour_fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL);
    s_fsr_mv_st = s_fsr_z_st = D3D12_RESOURCE_STATE_RENDER_TARGET; s_fsr_out_st = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!s_fsr_rtv_heap || !s_fsr_mv || !s_fsr_z || !s_fsr_rx || !s_fsr_out) {
        fprintf(stderr, "[FSR] target allocation failed -- FSR off%c", 10);
        fsr_release(); s_fsr_ctx_fail = 1; return 0;
    }
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_fsr_mv, NULL, fsr_rtv(0));
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_fsr_z, NULL, fsr_rtv(1));
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_fsr_rx, NULL, fsr_rtv(2));
    s_fsr_cw = w; s_fsr_ch = h;
    s_fsr_last_frame = 0;
    fprintf(stderr, "[FSR] native AA at %ux%u%c", w, h, 10);
    return 1;
}

/* ---- motion-vector pass: shaders + pipelines ---- */
static const char s_mv_ps_hlsl[] =
"struct MVOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prv:TEXCOORD1; };\n"
"struct MVT { float2 mv:SV_Target0; float z:SV_Target1; float rx:SV_Target2; };\n"
"#ifndef RXV\n#define RXV 0.0\n#endif\n"
"MVT main(MVOut i) {\n"
"    MVT o; o.rx = RXV; o.z = i.prv.z > 1e29 ? i.cur.z : i.pos.z;\n"
"    if (abs(i.prv.w) < 1e-6 || abs(i.cur.w) < 1e-6) { o.mv = float2(0, 0); return o; }\n"
"    float2 c = i.cur.xy / i.cur.w, p = i.prv.xy / i.prv.w;\n"
"    o.mv = (p - c) * float2(0.5, -0.5);\n"   /* UV units, previous minus current */
"    return o;\n"
"}\n";

static int mv_init(void)
{
    if (s_mv_rs && s_mv_ps) return 1;
    if (s_mv_init_fail) return 0;
    D3D12_ROOT_PARAMETER rp[4]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[1].Descriptor.ShaderRegister = 3; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[2].Descriptor.ShaderRegister = 30; rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[3].Constants.ShaderRegister = 4;
    rp[3].Constants.Num32BitValues = 4; rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 4; rd.pParameters = rp;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *sig = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) err->lpVtbl->Release(err); s_mv_init_fail = 1; return 0; }
    HRESULT hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, sig->lpVtbl->GetBufferPointer(sig),
        sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_mv_rs);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) { s_mv_init_fail = 1; return 0; }
    if (FAILED(shc_compile(s_mv_ps_hlsl, sizeof s_mv_ps_hlsl - 1, "fsr_mv_ps", "main", "ps_5_0", &s_mv_ps, &err)) || !s_mv_ps) {
        fprintf(stderr, "[FSR] MV pixel shader: %s%c", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?", 10);
        if (err) err->lpVtbl->Release(err);
        s_mv_init_fail = 1; return 0;
    }
    if (err) err->lpVtbl->Release(err); err = NULL;
    { char tx[sizeof s_mv_ps_hlsl + 64];   /* v189: same shader, reactive value baked in */
      const int k = snprintf(tx, sizeof tx, "#define RXV %.4f\n%s", fsr_rx_val(), s_mv_ps_hlsl);
      if (FAILED(shc_compile(tx, (size_t)k, "fsr_rx_ps", "main", "ps_5_0", &s_rx_ps, &err))) s_rx_ps = NULL;
      if (err) err->lpVtbl->Release(err); }
    return 1;
}

/* Text substitution into a new buffer. */
static char* mv_subst(const char* s, size_t n, const char* const* from, const char* const* to, int k)
{
    size_t cap = n * 2 + 1024, o = 0;
    char* out = (char*)malloc(cap); if (!out) return NULL;
    for (size_t i = 0; i < n; ) {
        int hit = -1;
        for (int q = 0; q < k; q++) { size_t L = strlen(from[q]); if (i + L <= n && !memcmp(s + i, from[q], L)) { hit = q; break; } }
        if (o + 64 + (hit >= 0 ? strlen(to[hit]) : 1) >= cap) { cap *= 2; char* t = (char*)realloc(out, cap); if (!t) { free(out); return NULL; } out = t; }
        if (hit >= 0) { size_t L = strlen(to[hit]); memcpy(out + o, to[hit], L); o += L; i += strlen(from[hit]); }
        else out[o++] = s[i++];
    }
    out[o] = 0;
    return out;
}
/* The decompiled program (or its GPU-fetch form) -> motion-vector VS. */
static char* mv_make_vs_text(const char* base)
{
    static const char sigA[] = "VSOutput main(VSInput input) {";
    static const char sigB[] = "VSOutput main(uint _vid : SV_VertexID) {";
    const char* m = strstr(base, "VSOutput main(");
    if (!m || strstr(base, "rsx_vtex")) return NULL;     /* vertex texture fetch: not handled */
    const int gfv = !strncmp(m, sigB, sizeof sigB - 1);
    if (!gfv && strncmp(m, sigA, sizeof sigA - 1)) return NULL;
    const size_t pre = (size_t)(m - base), fn = strlen(m);
    static const char* f1[] = { "VSOutput main(", "uint _vid : SV_VertexID" };
    static const char* t1[] = { "VSOutput mv_cur(", "uint _vid" };
    static const char* f2[] = { "VSOutput main(", "uint _vid : SV_VertexID", "vp_c[", "vp_posscale", "vp_posoffset" };
    static const char* t2[] = { "VSOutput mv_prv(", "uint _vid", "vp_cp[", "vp_pps", "vp_ppo" };
    char* c1 = mv_subst(m, fn, f1, t1, 2);
    char* c2 = mv_subst(m, fn, f2, t2, 5);
    if (!c1 || !c2) { free(c1); free(c2); return NULL; }
    static const char extra[] =
        "cbuffer VPPrev : register(b3) { float4 vp_cp[512]; float4 vp_pps; float4 vp_ppo; uint4 vp_pbr; };\n"
        "cbuffer MVK : register(b4) { float4 mv_k; };\n"
        "struct MVOut { float4 pos:SV_Position; float4 cur:TEXCOORD0; float4 prv:TEXCOORD1; };\n";
    const char* tail = gfv
        ? "MVOut main(uint _vid : SV_VertexID) { VSOutput a = mv_cur(_vid); VSOutput b = mv_prv(_vid);\n"
        : "MVOut main(VSInput input) { VSOutput a = mv_cur(input); VSOutput b = mv_prv(input);\n";
    static const char tail2[] =
        "    MVOut O; O.pos = a.pos; O.cur = float4(a.pos.xy - mv_k.xy * a.pos.w, a.pos.zw); O.prv = b.pos;\n"
    "    if (mv_k.w > 0.5) { O.cur.z = mv_k.z; O.prv.z = 1e30; } else O.prv.z = 0.0; return O; }\n";
    const size_t L = pre + strlen(c1) + strlen(c2) + sizeof extra + strlen(tail) + sizeof tail2 + 16;
    char* out = (char*)malloc(L);
    if (out) {
        size_t o = 0;
        memcpy(out, base, pre); o = pre;
        o += (size_t)sprintf(out + o, "%s%s\n%s\n%s%s", extra, c1, c2, tail, tail2);
    }
    free(c1); free(c2);
    return out;
}

typedef struct { u32 hash; u8 gf, cull, used; ID3D12PipelineState* pso; } MvPso;
#define MV_PSO_N 4096u
static MvPso s_mv_pso[MV_PSO_N];
typedef struct { u32 hash; u8 gf, used; ID3DBlob* vs; volatile LONG state; char* text; } MvVs;   /* state 1 queued, 2 done */
#define MV_VS_N 2048u
static MvVs s_mv_vs[MV_VS_N];
static char* gf_make_text(const char* hlsl, size_t* olen);   /* fwd */

/* v165f: motion-vector VS variants compile on a worker thread -- compiling one
 * on the render thread took up to 0.6 s (the hitch before a fall through the
 * floor). Until it is ready, draws of that program write no motion. */
static SRWLOCK s_mvq_lock = SRWLOCK_INIT;
static u32 s_mvq[MV_VS_N], s_mvq_head, s_mvq_tail;
static HANDLE s_mvq_ev;
static DWORD WINAPI mv_worker(LPVOID p)
{
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WaitForSingleObject(s_mvq_ev, INFINITE);
        for (;;) {
            u32 i = 0xFFFFFFFFu;
            AcquireSRWLockExclusive(&s_mvq_lock);
            if (s_mvq_head != s_mvq_tail) { i = s_mvq[s_mvq_head % MV_VS_N]; s_mvq_head++; }
            ReleaseSRWLockExclusive(&s_mvq_lock);
            if (i == 0xFFFFFFFFu) break;
            MvVs* m = &s_mv_vs[i];
            ID3DBlob *vb = NULL, *e = NULL;
            HRESULT hr = shc_compile(m->text, strlen(m->text), "fsr_mv_vs", "main", "vs_5_0", &vb, &e);
            if (FAILED(hr) || !vb) {
                static int n = 0; if (n < 4 && ++n) {
                    fprintf(stderr, "[FSR] MV VS compile failed (vs 0x%08X gf %d): %s%c", m->hash, m->gf,
                            e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?", 10);
                    FILE* f = fopen("logs/fsr_mv_fail.hlsl", "w"); if (f) { fputs(m->text, f); fclose(f); } }
                vb = NULL;
            }
            if (e) e->lpVtbl->Release(e);
            free(m->text); m->text = NULL;
            m->vs = vb;
            MemoryBarrier();
            InterlockedExchange(&m->state, 2);
        }
    }
    return 0;
}
static ID3DBlob* mv_get_vs(int vreal, int gf)
{
    const VPVSEntry* ve = &s_d3d.vp_vs[vreal];
    u32 i = (ve->hash * 2654435761u ^ (u32)gf) & (MV_VS_N - 1u);
    for (u32 n = 0; n < MV_VS_N; n++, i = (i + 1u) & (MV_VS_N - 1u)) {
        if (!s_mv_vs[i].used) break;
        if (s_mv_vs[i].hash == ve->hash && s_mv_vs[i].gf == (u8)gf)
            return s_mv_vs[i].state == 2 ? s_mv_vs[i].vs : NULL;
    }
    if (s_mv_vs[i].used) return NULL;            /* table full */
    MvVs* m = &s_mv_vs[i];
    m->used = 1; m->hash = ve->hash; m->gf = (u8)gf; m->vs = NULL; m->state = 2; m->text = NULL;
    if (!ve->src) return NULL;
    char* base = NULL; size_t bl = 0;
    if (gf) { base = gf_make_text(ve->src, &bl); if (!base) return NULL; }
    char* t = mv_make_vs_text(gf ? base : ve->src);
    free(base);
    if (!t) return NULL;
    if (!s_mvq_ev) {
        s_mvq_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 1u << 20, mv_worker, NULL, 0, NULL);
    }
    m->text = t; m->state = 1;
    AcquireSRWLockExclusive(&s_mvq_lock);
    s_mvq[s_mvq_tail % MV_VS_N] = i; s_mvq_tail++;
    ReleaseSRWLockExclusive(&s_mvq_lock);
    SetEvent(s_mvq_ev);
    return NULL;
}
static ID3D12PipelineState* mv_get_pso(const D3D12DrawRecord* dr, int rx)
{
    if (rx && !s_rx_ps) return NULL;
    const int gf = dr->gf ? 1 : 0;
    const int vreal = dr->vs_idx;
    if (vreal < 0 || vreal >= s_d3d.vp_vs_n || s_d3d.vp_vs[vreal].pending) return NULL;
    const u32 hash = s_d3d.vp_vs[vreal].hash;
    const u8 cull = (u8)((dr->cull & 7u) | (rx ? 0x80u : 0u));
    u32 i = (hash * 2654435761u ^ ((u32)gf << 3 | cull) * 40503u) & (MV_PSO_N - 1u);
    for (u32 n = 0; n < MV_PSO_N; n++, i = (i + 1u) & (MV_PSO_N - 1u)) {
        if (!s_mv_pso[i].used) break;
        if (s_mv_pso[i].hash == hash && s_mv_pso[i].gf == (u8)gf && s_mv_pso[i].cull == cull) return s_mv_pso[i].pso;
    }
    if (s_mv_pso[i].used) return NULL;
    ID3DBlob* vs = mv_get_vs(vreal, gf);
    if (!vs) return NULL;                         /* compiling (or failed): retry later, cache nothing */
    s_mv_pso[i].used = 1; s_mv_pso[i].hash = hash; s_mv_pso[i].gf = (u8)gf; s_mv_pso[i].cull = cull; s_mv_pso[i].pso = NULL;
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int k = 0; k < 16; k++) {
        il[k].SemanticName = "ATTR"; il[k].SemanticIndex = k; il[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[k].InputSlot = 0; il[k].AlignedByteOffset = (UINT)(k * 16);
        il[k].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA; il[k].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_mv_rs;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    { ID3DBlob* ps = rx ? s_rx_ps : s_mv_ps;
      pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps); }
    pd.InputLayout.pInputElementDescs = gf ? NULL : il; pd.InputLayout.NumElements = gf ? 0 : 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = (dr->cull & 1u) ? ((dr->cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK) : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = (dr->cull & 4u) ? TRUE : FALSE;
    /* v189: the motion pass tests against the scene's depth (LEQUAL) with its own
     * compile of the game's VS. Trees sway with sin() wind math, and the separate
     * compile rounds a hair differently -- the depth came out 1-2 units behind the
     * scene's in patches, those leaf pixels failed the test and kept "no motion"
     * (patchy ghosting). Pull the motion pass slightly toward the camera.
     * RSX_FSR_MV_BIAS=<D24 units> (default 16, 0 = off). */
    { static int bz = -1; if (bz < 0) { const char* e = getenv("RSX_FSR_MV_BIAS"); bz = e ? atoi(e) : 16; }
      if (bz > 0) { pd.RasterizerState.DepthBias = -bz; pd.RasterizerState.SlopeScaledDepthBias = -2.0f; } }
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = rx ? 0 : D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.RenderTarget[1].RenderTargetWriteMask = rx ? 0 : D3D12_COLOR_WRITE_ENABLE_ALL;
    if (rx) {   /* v189: reactive = max over the blended layers */
        D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[2];
        b->BlendEnable = TRUE; b->SrcBlend = b->DestBlend = D3D12_BLEND_ONE; b->BlendOp = D3D12_BLEND_OP_MAX;
        b->SrcBlendAlpha = b->DestBlendAlpha = D3D12_BLEND_ONE; b->BlendOpAlpha = D3D12_BLEND_OP_MAX;
        b->LogicOp = D3D12_LOGIC_OP_NOOP;
        b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    }
    pd.BlendState.IndependentBlendEnable = TRUE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 3; pd.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT; pd.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT; pd.RTVFormats[2] = DXGI_FORMAT_R8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso))) {
        static int n = 0; if (n < 4 && ++n) fprintf(stderr, "[FSR] MV pipeline failed (vs 0x%08X)%c", hash, 10);
        pso = NULL;
    }
    s_mv_pso[i].pso = pso;
    return pso;
}

/* Re-run draws [d0, d1) of the current batch that belong to the jittered
 * scene, into the motion / depth targets, against the scene's depth. The
 * caller's OM bindings are clobbered; the graphics root signature and the
 * GPU-fetch SRV are restored. */
static void fsr_mv_replay(u32 d0, u32 d1, D3D12_CPU_DESCRIPTOR_HANDLE dsv, int rtslot)
{
    if (rtslot < 0 || !s_fsr_ctx || !mv_init()) return;
    const OffRT* rt = &s_d3d.off_rt[rtslot];
    if (SC(rt->w) != s_fsr_cw || SC(rt->h) != s_fsr_ch) return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    fsr_barrier(s_fsr_mv, &s_fsr_mv_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    fsr_barrier(s_fsr_z, &s_fsr_z_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    fsr_barrier(s_fsr_rx, &s_fsr_rx_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh[3] = { fsr_rtv(0), fsr_rtv(1), fsr_rtv(2) };
    if (!s_fsr_mv_started) {
        static const float z4[4] = {0, 0, 0, 0}, o4[4] = {1, 1, 1, 1};
        cl->lpVtbl->ClearRenderTargetView(cl, rh[0], z4, 0, NULL);
        cl->lpVtbl->ClearRenderTargetView(cl, rh[1], o4, 0, NULL);
        cl->lpVtbl->ClearRenderTargetView(cl, rh[2], z4, 0, NULL);
        s_fsr_mv_started = 1;
    }
    if (s_fsr_ab == 4) return;   /* v184 diag: no motion / depth data (cleared: still, far) */
    cl->lpVtbl->OMSetRenderTargets(cl, 3, rh, FALSE, &dsv);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_mv_rs);
    if (s_gf_buf)
        cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 2, s_gf_buf->lpVtbl->GetGPUVirtualAddress(s_gf_buf) + (u64)T_PARITY * GF_CAP);
    const D3D12_GPU_VIRTUAL_ADDRESS cbva = s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb);
    const D3D12_GPU_VIRTUAL_ADDRESS hva = s_fsr_hist->lpVtbl->GetGPUVirtualAddress(s_fsr_hist);
    const float tw = (float)rt->w, th = (float)rt->h;
    ID3D12PipelineState* last = NULL;
    u32 n = 0, ncut = 0;
    for (u32 d = d0; d < d1 && d < MAX_DRAWS; d++) {
        const D3D12DrawRecord* dr = &T_DRAWS[d];
        if (!dr->is_vp || dr->is_clear || dr->fsr != 1 || !dr->fp_addr) continue;
        if (dr->topology != RSX_TOPOLOGY_TRIANGLES) continue;
        /* blended: no motion -- except blending set to ONE,ZERO (= replace), which is
         * opaque: v188, outdoor terrain is drawn that way and had no motion/depth
         * (ground smeared under FSR). Blend key: bit0 on, src bits 1-5, dst 6-10. */
        const int rx = (dr->blend_key & 1u) && !(((dr->blend_key >> 1) & 31u) == 2u && ((dr->blend_key >> 6) & 31u) == 1u);
        if (rx && fsr_rx_val() <= 0.0f) continue;   /* v189: blended -> reactive mask only */
        /* v184i: colour writes off = occlusion-query boxes / depth-only passes
         * (185 per frame in the Grimsby factory). Not part of the picture; each
         * box stamped its own (mismatched) motion over what was behind it --
         * over half of the bad motion pixels. */
        /* v189: ...but a colour-off draw that WRITES depth is a depth pre-pass: the
         * trash heaps (and other soft-edged props) lay their depth down colour-off,
         * then draw their colour blended with depth writes off. Skipping both left
         * no motion there -- and their depth hid the floor behind them too, so the
         * whole heap kept "still, far" and smeared. Occlusion boxes don't write
         * depth. RSX_FSR_MV_ZPRE=0 = skip every colour-off draw (v188). */
        { static int zp = -1; if (zp < 0) { const char* e = getenv("RSX_FSR_MV_ZPRE"); zp = e ? atoi(e) : 1; }
          if ((dr->cmask & 0xFu) == 0u && !(zp && (dr->cull & 0x400u) && (dr->cull & 0x200u))) continue; }
        if ((dr->cull & 0x100u) && !(dr->cull & 0x200u)) continue;         /* depth test off */
        /* v184: alpha-cutout draws (alpha test or KIL in the program). The
         * motion PS has no cutout, so a grass card / fence / branch quad wrote
         * its motion and depth over the background showing through its holes:
         * ghost trails around grass and crawling far edges. Skipped, the holes
         * keep the motion of what is behind them. RSX_FSR_MV_CUTOUT=1 = old. */
        { static int co = -1; if (co < 0) { const char* e = getenv("RSX_FSR_MV_CUTOUT"); co = e ? atoi(e) : 1; }   /* v188: on (cut-outs had no motion -> foliage smeared) */
          if (!co && (((dr->alpha_ctl >> 16) & 1u) || fp_kil_has(dr->fp_addr))) { ncut++; continue; } }
        if (dr->vp_w && dr->vp_w < rt->w) continue;                        /* sub-viewport pass */
        if (fpkill_match(dr->fp_addr)) continue;
        if (!dr->ib_count && !dr->vertex_count) continue;
        ID3D12PipelineState* pso = mv_get_pso(dr, rx);
        if (!pso) continue;
        if (pso != last) { cl->lpVtbl->SetPipelineState(cl, pso); last = pso; }
        D3D12_VIEWPORT vp = {0, 0, tw, th, 0.0f, 1.0f};
        if (dr->vp_w >= 2 && dr->vp_h >= 2 && (float)(dr->vp_x + dr->vp_w) <= tw + 0.5f && (float)(dr->vp_y + dr->vp_h) <= th + 0.5f) {
            vp.TopLeftX = (float)dr->vp_x; vp.TopLeftY = (float)dr->vp_y; vp.Width = (float)dr->vp_w; vp.Height = (float)dr->vp_h; }
        vp.TopLeftX = SCF(vp.TopLeftX); vp.TopLeftY = SCF(vp.TopLeftY); vp.Width = SCF(vp.Width); vp.Height = SCF(vp.Height);
        D3D12_RECT sc = {(LONG)(vp.TopLeftX + 0.5f), (LONG)(vp.TopLeftY + 0.5f), (LONG)(vp.TopLeftX + vp.Width + 0.5f), (LONG)(vp.TopLeftY + vp.Height + 0.5f)};
        cl->lpVtbl->RSSetViewports(cl, 1, &vp);
        cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
        cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, 0, cbva + ((u64)T_PARITY * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
        cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, 1, hva + dr->fsr_prev);
        { float k[4] = { dr->fsr_jnx, dr->fsr_jny, (float)d, s_fsr_ab == 5 ? 1.0f : 0.0f };
          cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 3, 4, k, 0); }
        if (dr->ib_count && s_vp_ib)
            cl->lpVtbl->DrawIndexedInstanced(cl, dr->ib_count, 1, dr->ib_first, dr->gf ? 0 : (INT)(dr->vb_byte_offset / 256), 0);
        else
            cl->lpVtbl->DrawInstanced(cl, dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
        n++;
    }
    { static int lg = 0; if (lg < 6 && n) { lg++; fprintf(stderr, "[FSR] motion pass: %u draws (%u cutout skipped)%c", n, ncut, 10); } }
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.vp_root_sig);
    if (s_gf_buf)
        cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 3, s_gf_buf->lpVtbl->GetGPUVirtualAddress(s_gf_buf) + (u64)T_PARITY * GF_CAP);
}

static uint32_t fsr_ffx_fmt(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT:    return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R16G16_FLOAT:       return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_FLOAT:          return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default:                             return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}
static struct FfxApiResource fsr_res(ID3D12Resource* r, DXGI_FORMAT f, u32 w, u32 h, uint32_t usage, uint32_t state)
{
    struct FfxApiResource x; memset(&x, 0, sizeof x);
    x.resource = r; x.state = state;
    x.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    x.description.format = fsr_ffx_fmt(f);
    x.description.width = w; x.description.height = h; x.description.depth = 1;
    x.description.mipCount = 1; x.description.flags = FFX_API_RESOURCE_FLAGS_NONE; x.description.usage = usage;
    return x;
}

/* v165d: copy FSR's colour back over the scene WITHOUT its alpha: the game
 * keeps a mask in the scene surface's alpha that its composite uses, and FSR
 * writes its own alpha (the first builds showed only the HUD on black). */
static ID3D12DescriptorHeap* s_mg_heap; static ID3D12RootSignature* s_mg_rs;
static ID3D12PipelineState* s_mg_pso; static u32 s_mg_fmt; static int s_mg_fail;
static const char s_mg_hlsl[] =
"Texture2D src : register(t0);\n"
"float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
"float4 ps(float4 p : SV_Position) : SV_Target { return src.Load(int3(p.xy, 0)); }\n";
static int fsr_merge(int slot)
{
    OffRT* rt = &s_d3d.off_rt[slot];
    ID3D12Device* dv = s_d3d.device;
    if (s_mg_fail) return 0;
    if (!s_mg_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_DESCRIPTOR_RANGE rg = {0}; rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp = {0}; rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = 1; rp.DescriptorTable.pDescriptorRanges = &rg;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd = {0}; rd.NumParameters = 1; rd.pParameters = &rp;
        ID3DBlob *sig = NULL, *err = NULL;
        if (FAILED(dv->lpVtbl->CreateDescriptorHeap(dv, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_mg_heap)) ||
            FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(dv->lpVtbl->CreateRootSignature(dv, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                                   &IID_ID3D12RootSignature, (void**)&s_mg_rs))) {
            fprintf(stderr, "[FSR] merge pass setup failed%c", 10); s_mg_fail = 1; return 0; }
        sig->lpVtbl->Release(sig);
    }
    if (!s_mg_pso || s_mg_fmt != rt->dxgi) {
        ID3DBlob *vb = NULL, *pb = NULL, *e = NULL;
        if (FAILED(D3DCompile(s_mg_hlsl, sizeof s_mg_hlsl - 1, "fsr_merge", NULL, NULL, "vs", "vs_5_0", 0, 0, &vb, &e)) ||
            FAILED(D3DCompile(s_mg_hlsl, sizeof s_mg_hlsl - 1, "fsr_merge", NULL, NULL, "ps", "ps_5_0", 0, 0, &pb, &e))) {
            fprintf(stderr, "[FSR] merge shader: %s%c", e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?", 10);
            s_mg_fail = 1; return 0; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
        pd.pRootSignature = s_mg_rs;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = (DXGI_FORMAT)rt->dxgi; pd.SampleDesc.Count = 1;
        if (s_mg_pso) grave_defer((ID3D12Resource*)s_mg_pso);
        s_mg_pso = NULL;
        if (FAILED(dv->lpVtbl->CreateGraphicsPipelineState(dv, &pd, &IID_ID3D12PipelineState, (void**)&s_mg_pso))) {
            fprintf(stderr, "[FSR] merge pipeline failed%c", 10); s_mg_fail = 1; return 0; }
        s_mg_fmt = rt->dxgi;
        vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE ch; s_mg_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_mg_heap, &ch);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = (DXGI_FORMAT)rt->dxgi; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    dv->lpVtbl->CreateShaderResourceView(dv, s_fsr_out, &sv, ch);
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    fsr_barrier(s_fsr_out, &s_fsr_out_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(slot, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = off_rt_rtv(slot);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rh, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)s_fsr_cw, (float)s_fsr_ch, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)s_fsr_cw, (LONG)s_fsr_ch};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp); cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    ID3D12DescriptorHeap* hh[] = { s_mg_heap };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, hh);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_mg_rs);
    cl->lpVtbl->SetPipelineState(cl, s_mg_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE gh; s_mg_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_mg_heap, &gh);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, gh);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    { ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap }; cl->lpVtbl->SetDescriptorHeaps(cl, 1, heaps); }
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.vp_root_sig);
    if (s_gf_buf)
        cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 3, s_gf_buf->lpVtbl->GetGPUVirtualAddress(s_gf_buf) + (u64)T_PARITY * GF_CAP);
    off_rt_transition(slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return 1;
}
/* v184: F12 capture -> fsr_mv.raw (R16G16F) + fsr_z.raw (R32F). */
void fsr_cap_dump(const char* dir, u32 fi, u32 nd)
{
    char path[512];
    if (s_fsr_mv) { snprintf(path, sizeof path, "%s/fsr_mv.raw", dir);
        cap_readback_write(s_fsr_mv, s_fsr_cw, s_fsr_ch, DXGI_FORMAT_R16G16_FLOAT, s_fsr_mv_st, fi, path); }
    if (s_fsr_z) { snprintf(path, sizeof path, "%s/fsr_z.raw", dir);
        cap_readback_write(s_fsr_z, s_fsr_cw, s_fsr_ch, DXGI_FORMAT_R32_FLOAT, s_fsr_z_st, fi, path); }
    if (s_fsr_rx) { snprintf(path, sizeof path, "%s/fsr_rx.raw", dir);
        cap_readback_write(s_fsr_rx, s_fsr_cw, s_fsr_ch, DXGI_FORMAT_R8_UNORM, s_fsr_rx_st, fi, path); }
    snprintf(path, sizeof path, "%s/fsr_draws.txt", dir);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "# d mt(0 none,1 exact,2 exact-instance,3 nearest) chain bd slot pslot fp vs tex0 tex1 count\n");
        for (u32 d = 0; d < nd && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* dr = &T_DRAWS[d];
            if (dr->fsr != 1) continue;
            fprintf(f, "%u %u %u %g %u %d %08X %d %08X %08X %u\n", d, dr->fsr_mt, dr->fsr_chain, dr->fsr_bd, dr->fsr_slot,
                    (int)dr->fsr_pslot, dr->fp_addr, dr->vs_idx, (u32)dr->tex[0].raw, (u32)dr->tex[1].raw, dr->ib_count ? dr->ib_count : dr->vertex_count);
        }
        fclose(f);
    }
    fprintf(stderr, "[FSR] capture: motion/depth dumped (Home mode %d)%c", s_fsr_ab, 10);
}
/* Run FSR on the finished scene surface and copy the result back over it. */
static void fsr_dispatch(void)
{
    { static int hw = 0; const int hd = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
      if (hd && !hw) {
          s_fsr_ab = (s_fsr_ab + 1) % 6;
          static const char* nm[6] = { "normal", "no auto-exposure", "no jitter", "FSR off (bypass)", "no jitter + no motion data", "draw-id debug (F12)" };
          fprintf(stderr, "[FSR] Home: mode %d = %s%c", s_fsr_ab, nm[s_fsr_ab], 10);
      }
      hw = hd;
      if (s_fsr_ctx && (s_fsr_ab == 1) != s_fsr_ab_ctx) { fsr_release(); s_fsr_done = 1; return; }   /* recreated next frame */
    }
    const int slot = s_fsr_slot;
    s_fsr_done = 1;
    if (slot < 0 || !s_fsr_ctx || !s_fsr_mv_started) return;
    OffRT* rt = &s_d3d.off_rt[slot];
    const u32 w = SC(rt->w), h = SC(rt->h);
    if (w != s_fsr_cw || h != s_fsr_ch || !rt->res) return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    off_rt_transition(slot, rd);
    fsr_barrier(s_fsr_mv, &s_fsr_mv_st, rd);
    fsr_barrier(s_fsr_z, &s_fsr_z_st, rd);
    fsr_barrier(s_fsr_rx, &s_fsr_rx_st, rd);
    fsr_barrier(s_fsr_out, &s_fsr_out_st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    struct ffxDispatchDescUpscale dd; memset(&dd, 0, sizeof dd);
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = cl;
    dd.color  = fsr_res(rt->res, (DXGI_FORMAT)rt->dxgi, w, h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dd.depth  = fsr_res(s_fsr_z, DXGI_FORMAT_R32_FLOAT, w, h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dd.motionVectors = fsr_res(s_fsr_mv, DXGI_FORMAT_R16G16_FLOAT, w, h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    if (fsr_rx_val() > 0.0f)
        dd.reactive = fsr_res(s_fsr_rx, DXGI_FORMAT_R8_UNORM, w, h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dd.output = fsr_res(s_fsr_out, (DXGI_FORMAT)rt->dxgi, w, h, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    float jx, jy; fsr_jitter(s_fsr_frame_r, &jx, &jy);
    dd.jitterOffset.x = jx; dd.jitterOffset.y = jy;
    dd.motionVectorScale.x = (float)w; dd.motionVectorScale.y = (float)h;
    dd.renderSize.width = w; dd.renderSize.height = h;
    dd.upscaleSize.width = w; dd.upscaleSize.height = h;
    { static float sh = -1.0f; if (sh < 0.0f) { const char* e = getenv("RSX_FSR_SHARP"); sh = e ? (float)atof(e) : 0.25f;
                                                if (sh < 0.0f) sh = 0.0f; if (sh > 1.0f) sh = 1.0f; }
      dd.enableSharpening = sh > 0.0f; dd.sharpness = sh; }
    LARGE_INTEGER now, qf; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&qf);
    float dt = s_fsr_t_last.QuadPart ? (float)((double)(now.QuadPart - s_fsr_t_last.QuadPart) * 1000.0 / (double)qf.QuadPart) : 16.7f;
    s_fsr_t_last = now;
    if (dt < 1.0f) dt = 1.0f; if (dt > 100.0f) dt = 100.0f;
    dd.frameTimeDelta = dt;
    dd.preExposure = 1.0f;
    dd.reset = (s_fsr_last_frame == 0 || s_fsr_frame_r != s_fsr_last_frame + 1u);
    dd.cameraNear = 0.1f; dd.cameraFar = 1000.0f;
    dd.cameraFovAngleVertical = 1.0f;
    dd.viewSpaceToMetersFactor = 1.0f;
    { const char* e = getenv("RSX_FSR_LINEAR"); dd.flags = (e && atoi(e)) ? 0u : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB; }
    /* RSX_FSR_TEST=1: no FSR, scene -> out -> scene through the same path (plumbing check). */
    static int tst = -1; if (tst < 0) { const char* e = getenv("RSX_FSR_TEST"); tst = e ? atoi(e) : 0; }
    ffxReturnCode_t rc = FFX_API_RETURN_OK;
    if (tst || s_fsr_ab == 3) {
        off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
        fsr_barrier(s_fsr_out, &s_fsr_out_st, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->lpVtbl->CopyResource(cl, s_fsr_out, rt->res);
    } else rc = p_ffxDispatch(&s_fsr_ctx, &dd.header);
    /* FFX bound its own descriptor heap / compute state. */
    { ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
      cl->lpVtbl->SetDescriptorHeaps(cl, 1, heaps); }
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.vp_root_sig);
    if (s_gf_buf)
        cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 3, s_gf_buf->lpVtbl->GetGPUVirtualAddress(s_gf_buf) + (u64)T_PARITY * GF_CAP);
    if (rc != FFX_API_RETURN_OK) {
        static int n = 0; if (n < 5 && ++n) fprintf(stderr, "[FSR] dispatch failed (%u)%c", (unsigned)rc, 10);
        return;
    }
    { static u32 nd = 0, nr = 0, gap = 0; nd++; if (dd.reset) { nr++; if (s_fsr_last_frame) gap += s_fsr_frame_r - s_fsr_last_frame; }
      if (nd % 600u == 0u) {
          const u32 rec = s_fsr_st_rec, mt = s_fsr_st_match, ov = s_fsr_st_over;
          fprintf(stderr, "[FSR] stats/600: history resets %u (frame gaps %u), scene draws %u, matched to last frame %.1f%%, over slot limit %u, lead waits %u, exact %.1f%%%c",
                  nr, gap, rec, rec ? 100.0 * mt / rec : 0.0, ov, s_fsr_st_wait, rec ? 100.0 * s_fsr_st_exact / rec : 0.0, 10);
          s_fsr_st_rec = s_fsr_st_match = s_fsr_st_over = s_fsr_st_wait = s_fsr_st_exact = 0; nr = gap = 0;
      } }
    s_fsr_last_frame = s_fsr_frame_r;
    s_fsr_disp_frame = s_fsr_frame_r; s_fsr_disp_ms = GetTickCount64();
    if (!fsr_merge(slot)) {
        fsr_barrier(s_fsr_out, &s_fsr_out_st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->lpVtbl->CopyResource(cl, rt->res, s_fsr_out);
        off_rt_transition(slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    { static int lg = 0; if (lg < 3 || (s_fsr_frame_r % 600u) == 0) { lg++; fprintf(stderr, "[FSR] frame %u upscaled (jitter %.3f,%.3f)%c", s_fsr_frame_r, jx, jy, 10); } }
}

/* Render thread, per non-clear draw: a jittered scene draw starts / continues
 * this frame's scene. */
static void fsr_note_scene_draw(const D3D12DrawRecord* dr, int slot)
{
    if (dr->fsr_frame != s_fsr_frame_r) {
        s_fsr_frame_r = dr->fsr_frame;
        s_fsr_mv_started = 0; s_fsr_done = 0;
    }
    s_fsr_slot = slot;
}
/* Render thread, end of a presented frame: pick next frame's scene surface. */
static void fsr_frame_end(void)
{
    if (!fsr_mode()) return;
    int best = -1; u32 bn = 0;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        if (s_fsr_cnt[i] > bn) { bn = s_fsr_cnt[i]; best = i; }
        s_fsr_cnt[i] = 0;
    }
    if (best >= 0 && bn >= 30 && s_d3d.off_rt[best].res) {
        const OffRT* r = &s_d3d.off_rt[best];
        if (fsr_ensure(SC(r->w), SC(r->h), (DXGI_FORMAT)r->dxgi)) {
            if (g_fsr_scene_off != r->off) {
                fprintf(stderr, "[FSR] scene surface 0x%08X (%ux%u, %u draws)%c", r->off, r->w, r->h, bn, 10);
                g_fsr_scene_off = r->off;
            }
            InterlockedExchange(&g_fsr_live, 1);
            return;
        }
    }
    g_fsr_scene_off = 0;
}
