/*
 * ps3recomp - RSX Command Buffer Processor
 *
 * Parses NV47xx GPU methods from the command buffer and updates RSX state.
 * Dispatches to the registered graphics backend for actual rendering.
 *
 * Command buffer format (NV47xx FIFO):
 *   Each command is a 32-bit header followed by N data words.
 *   Header format: [31:29] type | [28:18] count | [17:13] subchannel | [12:2] method | [1:0] flags
 *
 *   Type 0 (increasing): method, method+4, method+8, ... for each data word
 *   Type 2 (non-increasing): same method repeated for each data word
 *   Type 1 (jump): jump to address in data
 *   Type 3 (call/return): call/return from subroutine
 */

#include "rsx_commands.h"
#include "ps3emu/milestone.h"   /* ps3_ms -- boot milestone log */
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include "../../runtime/memory/vm.h"    /* VM_HLE_INJECT_BASE */
#include "ps3emu/log_first.h"
/* Resistance port: getenv() is called on hot paths (per draw / per method);
 * the UCRT version takes a lock and scans the environment every time, which
 * profiled at ~20% of the render thread. Cached by call-site string pointer. */
char* rsx_getenv(const char* name);
#define getenv(n) rsx_getenv(n)

/* ---------------------------------------------------------------------------
 * Global backend
 * -----------------------------------------------------------------------*/

static rsx_backend* s_backend = NULL;

/* Last NV406E_SET_REFERENCE value seen in the FIFO. The title writes a
 * reference then spins on the GCM control register's `ref` field until the RSX
 * reports it as reached; cellGcmSys mirrors this into the guest control
 * register after each drain so those waits (cellGcmFinish / wait-label) unblock. */
u32 g_rsx_last_reference = 0;

void rsx_set_backend(rsx_backend* backend)
{
    s_backend = backend;
}

rsx_backend* rsx_get_backend(void)
{
    return s_backend;
}

/* ---------------------------------------------------------------------------
 * State initialization
 * -----------------------------------------------------------------------*/

void rsx_state_init(rsx_state* state)
{
    memset(state, 0, sizeof(rsx_state));
    /* RSX reset value for the constant vertex attributes is (0,0,0,1); a zeroed
     * struct would leave w at 0. */
    for (int _i = 0; _i < RSX_MAX_VERTEX_ATTRIBS; _i++)
        state->vertex_data4f[_i][3] = 1.0f;

    /* Default viewport */
    state->viewport_w = 1280;
    state->viewport_h = 720;
    state->clip_min = 0.0f;
    state->clip_max = 1.0f;

    /* Default scissor */
    state->scissor_w = 4096;
    state->scissor_h = 4096;

    /* Default depth */
    state->depth_func = 1; /* LESS */
    state->depth_mask = 1;

    /* Default cull */
    state->cull_face = 1; /* BACK */
    state->front_face = 0; /* CW */

    /* Default color mask: all channels writable (A|R|G|B) */
    state->color_mask = 0x01010101;

    /* Default stencil: sensible initial values */
    state->stencil_func = 0x0207; /* ALWAYS */
    state->stencil_ref = 0;
    state->stencil_mask = 0xFF;
    state->stencil_op_fail = 0x1E00;  /* KEEP */
    state->stencil_op_zfail = 0x1E00; /* KEEP */
    state->stencil_op_zpass = 0x1E00; /* KEEP */
    state->stencil_write_mask = 0xFF;   /* v178 */
    state->two_sided_stencil = 0;
    state->back_stencil_func = 0x0207; state->back_stencil_ref = 0; state->back_stencil_mask = 0xFF;
    state->back_stencil_write_mask = 0xFF;
    state->back_stencil_op_fail = state->back_stencil_op_zfail = state->back_stencil_op_zpass = 0x1E00;

    /* Default alpha test */
    state->alpha_func = 0x0207; /* ALWAYS */
    state->alpha_ref = 0;

    /* Default shader control: 32-bit colour exports (r0..) -- matches every
     * title that never programs the register. */
    state->shader_control = CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;

    /* Mark everything dirty */
    state->surface_dirty = 1;
    state->viewport_dirty = 1;
    state->blend_dirty = 1;
    state->depth_dirty = 1;
    state->stencil_dirty = 1;
    state->texture_dirty = 1;
    state->vertex_dirty = 1;
    state->color_mask_dirty = 1;
    state->alpha_dirty = 1;
    state->shader_dirty = 1;
}

/* ---------------------------------------------------------------------------
 * Method processing
 * -----------------------------------------------------------------------*/

static int process_surface_method(rsx_state* state, u32 method, u32 data)
{
    switch (method) {
    case NV4097_SET_SURFACE_FORMAT:
        state->surface_format = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_HORIZONTAL:
        state->surface_clip_x = data & 0xFFFF;
        state->surface_clip_w = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_VERTICAL:
        state->surface_clip_y = data & 0xFFFF;
        state->surface_clip_h = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_AOFFSET:
        state->surface_color_offset[0] = data;
        { static int _s = -1; if (_s < 0) _s = getenv("SURFDBG") ? 1 : 0;
          static unsigned _seen[32]; static int _n = 0;
          if (_s) { int f = 0; for (int k = 0; k < _n; k++) if (_seen[k] == data) f = 1;
              if (!f && _n < 32) { _seen[_n++] = data;
                  fprintf(stderr, "[SURF] color offset -> 0x%08X%c", data, 10); } } }
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_BOFFSET:
        state->surface_color_offset[1] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_COFFSET:
        state->surface_color_offset[2] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_DOFFSET:
        state->surface_color_offset[3] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_ZETA_OFFSET:
        state->surface_zeta_offset = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_TARGET:
        { static int _ct=0; if (PS3_LOG_FIRST(_ct, 24) && getenv("RTT_DUMP"))
            fprintf(stderr, "[RSXCT] color_target=0x%X offA=0x%X offB=0x%X offC=0x%X offD=0x%X%s",
                    data, state->surface_color_offset[0], state->surface_color_offset[1],
                    state->surface_color_offset[2], state->surface_color_offset[3], "\n"); }
        state->color_target = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_PITCH_A:
        state->surface_color_pitch[0] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_B:
        state->surface_color_pitch[1] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_C:
        state->surface_color_pitch[2] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_D:
        state->surface_color_pitch[3] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_Z:
        state->surface_zeta_pitch = data;
        return 0;
    default:
        return -1;
    }
}

/* ---------------------------------------------------------------------------
 * Texture method processing
 *
 * Texture registers are laid out in 16 units, each spanning 0x20 bytes:
 *   Unit 0: 0x1A00..0x1A1C
 *   Unit 1: 0x1A20..0x1A3C
 *   ...
 *   Unit N: 0x1A00 + N*0x20 .. 0x1A1C + N*0x20
 * -----------------------------------------------------------------------*/

static int process_texture_method(rsx_state* state, u32 method, u32 data)
{
    /* Compute texture unit index and register offset within the unit */
    u32 base = method - NV4097_SET_TEXTURE_OFFSET; /* 0x1A00 */
    u32 unit = base / 0x20;
    u32 reg  = base % 0x20;

    if (unit >= RSX_MAX_TEXTURES)
        return -1;

    rsx_texture_state* tex = &state->textures[unit];

    switch (reg) {
    case 0x00: /* TEXTURE_OFFSET */
        tex->offset = data & 0x7FFFFFFFu;   /* 31-bit offset; bit 31 is not an address bit */
        break;
    case 0x04: /* TEXTURE_FORMAT */
        tex->format = data;
        break;
    case 0x08: /* TEXTURE_ADDRESS (wrap S/T/R) */
        tex->address = data;
        break;
    case 0x0C: /* TEXTURE_CONTROL0 (enable, min/max LOD, max aniso) */
        tex->control0 = data;
        break;
    case 0x10: /* TEXTURE_CONTROL1 (remap) */
        tex->control1 = data;
        break;
    case 0x14: /* TEXTURE_FILTER (bias, min/mag filter) */
        tex->filter = data;
        break;
    case 0x18: /* TEXTURE_IMAGE_RECT (width << 16 | height) */
        tex->image_rect = data;
        break;
    case 0x1C: /* TEXTURE_BORDER_COLOR */
        tex->border_color = data;
        break;
    default:
        return -1;
    }

    tex->dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

/* Vertex texture unit block: 0x0900 + unit*0x20, eight words. Same order as a
 * fragment unit's except at +0x10, which is CONTROL3 (the row pitch) rather
 * than CONTROL1 (the component crossbar) -- a vertex unit has no crossbar. */
static int process_vertex_texture_method(rsx_state* state, u32 method, u32 data)
{
    u32 base = method - NV4097_SET_VERTEX_TEXTURE_OFFSET;
    u32 unit = base / 0x20;
    u32 reg  = base % 0x20;

    if (unit >= RSX_MAX_VERTEX_TEXTURES)
        return -1;

    rsx_texture_state* tex = &state->vertex_textures[unit];

    switch (reg) {
    case 0x00: tex->offset       = data & 0x7FFFFFFFu; break;   /* 31-bit offset */
    case 0x04: tex->format       = data; break;
    case 0x08: tex->address      = data; break;
    case 0x0C: tex->control0     = data; break;
    case 0x10: tex->control3     = data; break;   /* CONTROL1's slot */
    case 0x14: tex->filter       = data; break;
    case 0x18: tex->image_rect   = data; break;
    case 0x1C: tex->border_color = data; break;
    default:
        return -1;
    }

    tex->dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

/* SET_TEXTURE_CONTROL3: 0x1840 + unit*4, one word per unit rather than a slot
 * in the 0x20-byte unit block. */
static int process_texture_control3(rsx_state* state, u32 method, u32 data)
{
    u32 unit = (method - NV4097_SET_TEXTURE_CONTROL3) / 4;
    if (unit >= RSX_MAX_TEXTURES)
        return -1;
    state->textures[unit].control3 = data;
    state->textures[unit].dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Vertex attribute method processing
 *
 * FORMAT registers: 0x1740 + attrib*4  (16 attributes)
 * OFFSET registers: 0x1680 + attrib*4  (16 attributes)
 * -----------------------------------------------------------------------*/

static int process_vertex_attrib_method(rsx_state* state, u32 method, u32 data)
{
    if (method >= NV4097_SET_VERTEX_DATA_ARRAY_FORMAT &&
        method < NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + RSX_MAX_VERTEX_ATTRIBS * 4) {
        u32 index = (method - NV4097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4;
        rsx_vertex_attrib* attr = &state->vertex_attribs[index];

        /*
         * Format register layout:
         *   [3:0]   type   (float, half, ubyte, short, etc.)
         *   [7:4]   size   (number of components: 1-4)
         *   [15:8]  stride (bytes between consecutive elements)
         *   [16]    enable
         */
        attr->type      = data & 0xF;
        attr->size      = (data >> 4) & 0xF;
        attr->stride    = (data >> 8) & 0xFF;
        attr->frequency = (data >> 16) & 0xFFFF;   /* instancing divisor */
        /* SIZE (the component count) is what enables the attribute: NV4097 uses
         * size == 0 to mean "this array is off". Keying off `type` instead marked
         * every unused slot enabled-with-zero-components, because PSGL writes a
         * bare type (e.g. 0x00000002 = float32, size 0, stride 0) into the slots
         * it is NOT using -- so the input layout was built from 16 attributes of
         * which most were degenerate, and the draws rasterized nothing. */
        attr->enabled   = (attr->size != 0);
        attr->format    = data;
        state->vertex_dirty = 1;
        return 0;
    }

    if (method >= NV4097_SET_VERTEX_DATA_ARRAY_OFFSET &&
        method < NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + RSX_MAX_VERTEX_ATTRIBS * 4) {
        u32 index = (method - NV4097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4;
        state->vertex_attribs[index].offset = data;
        state->vertex_dirty = 1;
        return 0;
    }

    return -1;
}

/* ---------------------------------------------------------------------------
 * Main method dispatch
 * -----------------------------------------------------------------------*/

/* Resistance port: RSX immediate mode. Between SET_BEGIN_END(prim) and
 * SET_BEGIN_END(0), writing the last component of attribute 0 through any
 * SET_VERTEX_DATA*_M register emits one vertex carrying the current value of
 * every attribute register (glVertex semantics). Fall of Man draws its menu
 * backgrounds and full-screen passes this way (2F_M position + texcoord);
 * those vertices were dropped, so the passes never drew. At BEGIN_END(0) the
 * collected vertices are drawn through the backend's normal draw_arrays path,
 * with rsx_fetch_attrib reading them from this buffer. */
#define RSX_IMM_MAX 8192
static float s_imm_verts[RSX_IMM_MAX][RSX_MAX_VERTEX_ATTRIBS][4];
static u32   s_imm_count;
const float (*g_rsx_imm_verts)[RSX_MAX_VERTEX_ATTRIBS][4] = 0;
u32 g_rsx_imm_count = 0;

static void rsx_imm_emit(rsx_state* state)
{
    if (!state->in_begin_end || s_imm_count >= RSX_IMM_MAX) return;
    memcpy(s_imm_verts[s_imm_count++], state->vertex_data4f, sizeof state->vertex_data4f);
}

static void rsx_imm_flush(rsx_state* state, u32 prim)
{
    u32 n = s_imm_count;
    s_imm_count = 0;
    if (!n || !s_backend || !s_backend->draw_arrays) return;
    { static int _d = -1, _k = 0; if (_d < 0) _d = getenv("IMM_DBG") ? 1 : 0;
      if (_d && _k < 60) { _k++;
        fprintf(stderr, "[IMM] prim=%u verts=%u v0.a0=(%g %g) v0.a1=(%g %g)%c", prim, n,
                s_imm_verts[0][0][0], s_imm_verts[0][0][1],
                s_imm_verts[0][1][0], s_imm_verts[0][1][1], 10); } }
    rsx_vertex_attrib saved[RSX_MAX_VERTEX_ATTRIBS];
    memcpy(saved, state->vertex_attribs, sizeof saved);
    for (int i = 0; i < RSX_MAX_VERTEX_ATTRIBS; i++) {
        state->vertex_attribs[i].enabled = 1;
        state->vertex_attribs[i].type = 2;
        state->vertex_attribs[i].size = 4;
        state->vertex_attribs[i].stride = 16;
        state->vertex_attribs[i].frequency = 0;
    }
    g_rsx_imm_verts = (const float (*)[RSX_MAX_VERTEX_ATTRIBS][4])s_imm_verts;
    g_rsx_imm_count = n;
    s_backend->draw_arrays(s_backend->userdata, prim, 0, n);
    g_rsx_imm_verts = 0;
    g_rsx_imm_count = 0;
    memcpy(state->vertex_attribs, saved, sizeof saved);
}

int rsx_tconst_fast_ok(void)
{
    static int ok = -1;
    if (ok < 0) ok = !getenv("TCONST_DBG") && !getenv("SEQ_DBG") && !getenv("METHOD_SEEN") &&
                     !getenv("RSX_TRACE") && !getenv("GCM_TCONST_SLOW");
    return ok;
}
void rsx_tconst_block(rsx_state* state, u32 method, const u8* be, u32 count)
{
    u32 reg = (method - NV4097_SET_TRANSFORM_CONSTANT) / 4;
    const u32 load = state->transform_constant_load;
    u32 lo = state->vertex_constants_lo, hi = state->vertex_constants_hi;
    int dirty = state->vertex_constants_dirty;
    for (u32 i = 0; i < count; i++, reg++, be += 4) {
        const u32 slot = load + (reg >> 2);
        if (slot >= RSX_MAX_VERTEX_CONSTANTS) continue;
        const u32 v = ((u32)be[0] << 24) | ((u32)be[1] << 16) | ((u32)be[2] << 8) | be[3];
        memcpy(&state->vertex_constants[slot][reg & 3], &v, 4);
        if (!dirty) { lo = hi = slot; dirty = 1; }
        else { if (slot < lo) lo = slot; if (slot > hi) hi = slot; }
    }
    state->vertex_constants_lo = lo; state->vertex_constants_hi = hi;
    state->vertex_constants_dirty = dirty;
}

/* v140: NV4097_SET_TRANSFORM_PROGRAM runs. Same effect as feeding each word
 * to rsx_process_method (little-endian store at the cursor, cursor advances,
 * byte high-water mark, dirty, one generation step per word), without the
 * per-word dispatch. */
void rsx_tprog_block(rsx_state* state, const u8* be, u32 count)
{
    u32 w = state->vp_ucode_write, n = 0;
    for (; n < count && w + 4 <= sizeof(state->vp_ucode); n++, w += 4, be += 4) {
        state->vp_ucode[w + 0] = be[3];
        state->vp_ucode[w + 1] = be[2];
        state->vp_ucode[w + 2] = be[1];
        state->vp_ucode[w + 3] = be[0];
    }
    if (!n) return;
    state->vp_ucode_write = w;
    if (w > state->vp_ucode_bytes) state->vp_ucode_bytes = w;
    state->vp_dirty = 1;
    state->vp_ucode_gen += n;
}

/* v140: method -> where rsx_process_method's compare chain handles it.
 * The chain below tests ~100 conditions in a fixed order, so a draw call
 * (BEGIN_END, DRAW_INDEX_ARRAY near the end) paid for all of them, and so did
 * every method nothing handles. Each entry names a section of the chain to
 * enter at; everything between that entry and the method's own test is
 * other methods' equality/range tests, so entering early is always
 * equivalent. 0 = enter at the top (the semaphore/notify methods, which have
 * side effects without returning, and anything not listed). */
enum {
    MC_TOP = 0, MC_UNKNOWN, MC_SURF, MC_TEX, MC_TEXC3, MC_VTEX, MC_VATTR,
    MC_VPORT, MC_VIEW, MC_CMASK, MC_CLEAR, MC_BLEND, MC_DEPTH, MC_STENCIL,
    MC_CULL, MC_SHADER, MC_TPROG, MC_VAOM, MC_VDATA, MC_TCLOAD, MC_TCONST,
    MC_DRAW, MC_SCISSOR, MC_FREQ, MC_VDBOFF, MC_VDBIDX
};
static u8 s_mcls[0x800];
static void mcls_set(u32 m, u8 c) { if (m < 0x2000u) s_mcls[m >> 2] = c; }
static void mcls_range(u32 lo, u32 hi, u8 c) { for (u32 m = lo; m < hi; m += 4) mcls_set(m, c); }
static int rsx_mcls_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("RSX_METHOD_CHAIN");      /* =1: the old top-down chain only */
        on = !(e && atoi(e));
        if (!on) return 0;
        mcls_range(0x200, 0x240, MC_SURF);
        mcls_range(0x280, 0x290, MC_SURF);
        mcls_range(0x1A00, 0x1A00 + RSX_MAX_TEXTURES * 0x20, MC_TEX);
        mcls_range(NV4097_SET_TEXTURE_CONTROL3, NV4097_SET_TEXTURE_CONTROL3 + RSX_MAX_TEXTURES * 4, MC_TEXC3);
        mcls_range(NV4097_SET_VERTEX_TEXTURE_OFFSET,
                   NV4097_SET_VERTEX_TEXTURE_OFFSET + RSX_MAX_VERTEX_TEXTURES * 0x20, MC_VTEX);
        mcls_range(0x1740, 0x1740 + RSX_MAX_VERTEX_ATTRIBS * 4, MC_VATTR);
        mcls_range(0x1680, 0x1680 + RSX_MAX_VERTEX_ATTRIBS * 4, MC_VATTR);
        mcls_range(0x0A20, 0x0A40, MC_VPORT);
        mcls_set(0x1D88, MC_VIEW);
        mcls_set(NV4097_SET_VIEWPORT_HORIZONTAL, MC_VIEW);
        mcls_set(NV4097_SET_VIEWPORT_VERTICAL, MC_VIEW);
        mcls_set(NV4097_SET_COLOR_MASK, MC_CMASK);
        mcls_set(NV4097_SET_ALPHA_TEST_ENABLE, MC_CMASK);
        mcls_set(NV4097_SET_ALPHA_FUNC, MC_CMASK);
        mcls_set(NV4097_SET_ALPHA_REF, MC_CMASK);
        mcls_set(NV4097_SET_COLOR_CLEAR_VALUE, MC_CLEAR);
        mcls_set(NV4097_SET_ZSTENCIL_CLEAR_VALUE, MC_CLEAR);
        mcls_set(NV4097_CLEAR_SURFACE, MC_CLEAR);
        mcls_set(NV4097_SET_BLEND_ENABLE, MC_BLEND);
        mcls_set(NV4097_SET_BLEND_FUNC_SFACTOR, MC_BLEND);
        mcls_set(NV4097_SET_BLEND_FUNC_DFACTOR, MC_BLEND);
        mcls_set(NV4097_SET_BLEND_EQUATION, MC_BLEND);
        mcls_set(NV4097_SET_DEPTH_TEST_ENABLE, MC_DEPTH);
        mcls_set(NV4097_SET_DEPTH_FUNC, MC_DEPTH);
        mcls_set(NV4097_SET_DEPTH_MASK, MC_DEPTH);
        mcls_set(0x0A68, MC_DEPTH); mcls_set(0x0A78, MC_DEPTH); mcls_set(0x0A7C, MC_DEPTH);
        mcls_set(NV4097_SET_STENCIL_TEST_ENABLE, MC_STENCIL);
        mcls_set(NV4097_SET_STENCIL_FUNC, MC_STENCIL);
        mcls_set(NV4097_SET_STENCIL_FUNC_REF, MC_STENCIL);
        mcls_set(NV4097_SET_STENCIL_FUNC_MASK, MC_STENCIL);
        mcls_set(NV4097_SET_STENCIL_OP_FAIL, MC_STENCIL);
        mcls_set(NV4097_SET_STENCIL_OP_ZFAIL, MC_STENCIL);
        mcls_set(NV4097_SET_STENCIL_OP_ZPASS, MC_STENCIL);
        mcls_set(NV4097_SET_STENCIL_MASK, MC_STENCIL);   /* v178 */
        mcls_range(NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE, NV4097_SET_BACK_STENCIL_OP_ZPASS + 4, MC_STENCIL);
        mcls_set(NV4097_SET_CULL_FACE_ENABLE, MC_CULL);
        mcls_set(NV4097_SET_CULL_FACE, MC_CULL);
        mcls_set(NV4097_SET_FRONT_FACE, MC_CULL);
        mcls_set(NV4097_SET_SHADER_PROGRAM, MC_SHADER);
        mcls_set(NV4097_SET_TRANSFORM_PROGRAM_LOAD, MC_SHADER);
        mcls_set(NV4097_SET_TRANSFORM_PROGRAM_START, MC_SHADER);
        mcls_range(NV4097_SET_TRANSFORM_PROGRAM, NV4097_SET_TRANSFORM_PROGRAM + 32 * 4, MC_TPROG);
        mcls_set(NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK, MC_VAOM);
        mcls_range(0x1C00, 0x1C00 + RSX_MAX_VERTEX_ATTRIBS * 16, MC_VDATA);   /* 4F_M */
        mcls_range(0x1940, 0x1940 + RSX_MAX_VERTEX_ATTRIBS * 4, MC_VDATA);    /* 4UB_M */
        mcls_range(0x1E40, 0x1E40 + RSX_MAX_VERTEX_ATTRIBS * 4, MC_VDATA);    /* 1F_M */
        mcls_range(0x1880, 0x1880 + RSX_MAX_VERTEX_ATTRIBS * 8, MC_VDATA);    /* 2F_M */
        mcls_range(0x1500, 0x1500 + RSX_MAX_VERTEX_ATTRIBS * 16, MC_VDATA);   /* 3F_M */
        mcls_range(0x1900, 0x1900 + RSX_MAX_VERTEX_ATTRIBS * 4, MC_VDATA);    /* 2S_M */
        mcls_range(0x1980, 0x1980 + RSX_MAX_VERTEX_ATTRIBS * 8, MC_VDATA);    /* 4S_M */
        mcls_range(0x0A80, 0x0A80 + RSX_MAX_VERTEX_ATTRIBS * 8, MC_VDATA);    /* SCALED4S_M */
        mcls_set(NV4097_SET_TRANSFORM_CONSTANT_LOAD, MC_TCLOAD);
        mcls_set(NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, MC_TCLOAD);
        mcls_set(NV4097_SET_TRANSFORM_BRANCH_BITS, MC_TCLOAD);
        mcls_set(NV4097_INVALIDATE_L2, MC_TCLOAD);
        mcls_range(NV4097_SET_TRANSFORM_CONSTANT,
                   NV4097_SET_TRANSFORM_CONSTANT + NV4097_SET_TRANSFORM_CONSTANT_DWORDS * 4, MC_TCONST);
        mcls_set(NV4097_SET_BEGIN_END, MC_DRAW);
        mcls_set(NV4097_DRAW_ARRAYS, MC_DRAW);
        mcls_set(NV4097_SET_SHADER_CONTROL, MC_DRAW);
        mcls_set(NV4097_SET_INDEX_ARRAY_ADDRESS, MC_DRAW);
        mcls_set(0x1DAC, MC_DRAW); mcls_set(0x1DB0, MC_DRAW);
        mcls_set(NV4097_SET_INDEX_ARRAY_DMA, MC_DRAW);
        mcls_set(NV4097_DRAW_INDEX_ARRAY, MC_DRAW);
        mcls_set(NV4097_SET_SCISSOR_HORIZONTAL, MC_SCISSOR);
        mcls_set(NV4097_SET_SCISSOR_VERTICAL, MC_SCISSOR);
        mcls_set(0x0050, MC_SCISSOR);
        mcls_set(0x1FC0, MC_FREQ); mcls_set(0x1738, MC_VDBOFF); mcls_set(0x173C, MC_VDBIDX);
        /* The ones with side effects that do not return stay at the top. */
        mcls_set(0x0104, MC_TOP); mcls_set(0x0180, MC_TOP); mcls_set(0x01A4, MC_TOP);
        mcls_set(0x1D6C, MC_TOP); mcls_set(0x1D70, MC_TOP); mcls_set(0x1D74, MC_TOP);
    }
    return on;
}

int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    static int s_mdbg = -1;
    if (s_mdbg < 0) s_mdbg = (getenv("METHOD_SEEN") || getenv("RSX_TRACE")) ? 1 : 0;
    if (s_mdbg) {
    /* Resistance port diagnostic: METHOD_SEEN=1 prints every distinct NV4097
     * method the first time it arrives (with its data and begin/end state). */
    { static int _ms = -1; static u8 _seen[0x4000];
      if (_ms < 0) _ms = getenv("METHOD_SEEN") ? 1 : 0;
      if (_ms && method < 0x10000u && !_seen[method >> 2]) { _seen[method >> 2] = 1;
        fprintf(stderr, "[MSEEN] 0x%04X data=0x%08X inbe=%d%c", method, data, state->in_begin_end, 10); } }
    /* RSX_TRACE=<N>: trace the first N methods (bare "1" keeps the old 250).
     * The fixed 250 was spent entirely on boot-time setup, so the methods around
     * the first real draw -- exactly the ones worth seeing -- were never traced. */
    { static int _rt=-1; if(_rt<0){ const char* e=getenv("RSX_TRACE");
        _rt = e ? (atoi(e) > 1 ? atoi(e) : 250) : 0; }
      if(_rt){ static int _m=0; if(PS3_LOG_FIRST(_m, _rt)) fprintf(stderr,"[rsxm] method=0x%04X data=0x%08X\n", method, data); } }
    }
    /* v140: jump to the method's section of the chain (s_mcls above). */
    { static int s_mc = -1;
      if (s_mc < 0) s_mc = rsx_mcls_enabled();
      if (s_mc && method < 0x2000u) {
        switch (s_mcls[method >> 2]) {
        case MC_UNKNOWN: return -1;
        case MC_SURF:    return process_surface_method(state, method, data);
        case MC_TEX:     return process_texture_method(state, method, data);
        case MC_TEXC3:   return process_texture_control3(state, method, data);
        case MC_VTEX:    return process_vertex_texture_method(state, method, data);
        case MC_VATTR:   return process_vertex_attrib_method(state, method, data);
        case MC_FREQ:    state->frequency_divider_op = data; return 0;
        case MC_VDBOFF:  state->vertex_data_base_offset = data; return 0;
        case MC_VDBIDX:  state->vertex_data_base_index  = data; return 0;
        case MC_VPORT:   goto L_vport;
        case MC_VIEW:    goto L_view;
        case MC_CMASK:   goto L_cmask;
        case MC_CLEAR:   goto L_clear;
        case MC_BLEND:   goto L_blend;
        case MC_DEPTH:   goto L_depth;
        case MC_STENCIL: goto L_stencil;
        case MC_CULL:    goto L_cull;
        case MC_SHADER:  goto L_shader;
        case MC_TPROG:   goto L_tprog;
        case MC_VAOM:    goto L_vaom;
        case MC_VDATA:   goto L_vdata;
        case MC_TCLOAD:  goto L_tcload;
        case MC_TCONST:  goto L_tconst;
        case MC_DRAW:    goto L_draw;
        case MC_SCISSOR: goto L_scissor;
        default: break;
        }
      } }
    /* Back-end write label / semaphore (cellGcmSetWriteBackEndLabel): the RSX
     * writes a value to a report/label the CPU polls for CPU<->RSX sync (double
     * buffering). Real hardware DOES this; without it the game's frame-fence
     * loop (func_0006E6E0 polling label 0x41 @ 0x03000410) spins forever and
     * never reaches render. NV4097_SET_SEMAPHORE_OFFSET(0x1D6C)=offset (index*0x10
     * into the GCM label window @0x03000000); NV4097_BACK_END_WRITE_SEMAPHORE_
     * RELEASE(0x1D70)=value. Also NV406E semaphore release (0x0010 offset/0x0014
     * value) for the sub-channel path. */
    /* NV4097_SET_FREQUENCY_DIVIDER_OPERATION: per-attribute modulo/divide mask
     * for the frequency divisor (instancing). Captured for read_vp_vertex. */
    if (method == 0x00001FC0) { state->frequency_divider_op = data; return 0; }
    /* Resistance port: vertex data base offset / base index. Ignored, every
     * mesh that relies on them (Fall of Man shares one vertex pool across a
     * level and selects each mesh by base offset) fetched from the start of
     * the pool: the world drew as giant stretched triangles. */
    if (method == 0x00001738) { state->vertex_data_base_offset = data; return 0; }
    if (method == 0x0000173C) { state->vertex_data_base_index  = data; return 0; }

    /* NV4097_SET_CONTEXT_DMA_SEMAPHORE (0x01A4) picks where the semaphore
     * offset points. CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER (0xFEED0001) is
     * main memory by IO offset: Edge-style SPU geometry jobs release ring
     * segments that way, and their producer polls that very word -- written
     * into the label window instead, the read index never moved and every
     * job spun until the DMA repeat limiter killed it. */
    /* NV4097_SET_CONTEXT_DMA_NOTIFIES (0x0180) / NV4097_NOTIFY (0x0104): write
     * {timestamp, 0} to notify slot (ctx & 7) ^ 7 in IO page 0xF1, for the
     * CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_n objects (cellGcmGetNotifyDataAddress). */
    { static u32 s_notify_ctx = 0;
      if (method == 0x0180) s_notify_ctx = data;
      if (method == 0x0104) {
          if ((s_notify_ctx & ~7u) == (0x6660420Fu & ~7u)) {
              extern void vm_write32(uint32_t a, uint32_t v);
              extern u32 gcm_io_offset_to_ea(u32 io);
              extern unsigned long long ps3_ms_now(void);
              u32 ea = gcm_io_offset_to_ea(0x0F100000u + ((s_notify_ctx & 7u) ^ 7u) * 0x40u);
              { static int n = 0; if (n++ < 4)   /* TEMP */
                  fprintf(stderr, "[RSX] NOTIFY slot %u -> ea 0x%08X\n", (s_notify_ctx & 7u) ^ 7u, ea); }
              if (ea) {
                  unsigned long long ts = ps3_ms_now() * 1000000ull;
                  vm_write32(ea, (u32)(ts >> 32)); vm_write32(ea + 4, (u32)ts);
                  vm_write32(ea + 8, 0); vm_write32(ea + 12, 0);
              }
          }
          return 0;
      } }
    { static u32 s_sem_off = 0, s_sem_ctx = 0;
      if (method == 0x01A4) {
          if (data != s_sem_ctx) { static int _l = 0; if (_l++ < 8)
              fprintf(stderr, "[RSX] semaphore context dma = 0x%08X\n", data); }
          s_sem_ctx = data;
      }
      if (method == 0x1D6C) { s_sem_off = data; return 0; }
      if (method == 0x1D70 || method == 0x1D74) {   /* v180: reads land before the label */
          extern void rsx_backend_flush_reads(void); rsx_backend_flush_reads(); }
      if ((method == 0x1D70 || method == 0x1D74) && s_sem_ctx == 0xFEED0001u) {
        extern void vm_write32(uint32_t a, uint32_t v);
        extern u32 gcm_io_offset_to_ea(u32 io);
        u32 ea = gcm_io_offset_to_ea(s_sem_off);
        u32 val = method == 0x1D74 ? data
                : (data & 0xff00ff00u) | ((data >> 16) & 0xffu) | ((data & 0xffu) << 16);
        if (ea) vm_write32(ea, val);
        return 0;
      }
      if (method == 0x1D70) {
        extern void vm_write32(uint32_t a, uint32_t v);
        /* The RSX back-end semaphore write swaps bytes 0<->2 of the value (hw
         * quirk); libgcm's cellGcmSetWriteBackEndLabel PRE-swaps to compensate
         * (SDK gcm_implementation_sub.h: "// swap byte 0 and 2"). Writing the
         * FIFO value verbatim left the label byte-swapped, so gcmutil's
         * cellGcmUtilFinish poll (label 255 == sLabelVal, starts at 1) compared
         * 0x00010000 != 1 and spun forever (gcm/cube sample hang after init).
         * Apply the same swap the hardware does so the pre-swap cancels out. */
        u32 val = (data & 0xff00ff00u) | ((data >> 16) & 0xffu) | ((data & 0xffu) << 16);
        vm_write32(VM_HLE_INJECT_BASE + (s_sem_off & 0x00FFFFFFu), val);
        { static u32 seen[64]; static int ns = 0; int k = 0;   /* TEMP: distinct release offsets */
          for (; k < ns; k++) if (seen[k] == s_sem_off) break;
          if (k == ns && ns < 64) { seen[ns++] = s_sem_off;
              fprintf(stderr, "[RSX] first back-end release @off 0x%X val 0x%08X\n", s_sem_off, val); } }
        { static int _l=0; if(PS3_LOG_FIRST(_l, 8)) fprintf(stderr,"[RSX] label write @0x%08X = 0x%08X (sync fence, raw 0x%08X)\n", VM_HLE_INJECT_BASE+(s_sem_off&0xFFFFFF), val, data); }
        return 0;
      }
      if (method == 0x1D74) {
        extern void vm_write32(uint32_t a, uint32_t v);
        vm_write32(VM_HLE_INJECT_BASE + (s_sem_off & 0x00FFFFFFu), data);
        return 0;
      } }
    if ((method >= 0x200 && method <= 0x23C) ||
        (method >= 0x280 && method <= 0x28C))
        return process_surface_method(state, method, data);

    /* Texture methods: 0x1A00..0x1A00 + 16*0x20 - 1 */
    if (method >= 0x1A00 && method < 0x1A00 + RSX_MAX_TEXTURES * 0x20)
        return process_texture_method(state, method, data);

    /* Texture CONTROL3: 0x1840..0x187C */
    if (method >= NV4097_SET_TEXTURE_CONTROL3 &&
        method < NV4097_SET_TEXTURE_CONTROL3 + RSX_MAX_TEXTURES * 4)
        return process_texture_control3(state, method, data);

    /* Vertex texture units: 0x0900..0x097C */
    if (method >= NV4097_SET_VERTEX_TEXTURE_OFFSET &&
        method < NV4097_SET_VERTEX_TEXTURE_OFFSET + RSX_MAX_VERTEX_TEXTURES * 0x20)
        return process_vertex_texture_method(state, method, data);

    /* Vertex attribute FORMAT: 0x1740..0x177C */
    if (method >= 0x1740 && method < 0x1740 + RSX_MAX_VERTEX_ATTRIBS * 4)
        return process_vertex_attrib_method(state, method, data);

    /* Vertex attribute OFFSET: 0x1680..0x16BC */
    if (method >= 0x1680 && method < 0x1680 + RSX_MAX_VERTEX_ATTRIBS * 4)
        return process_vertex_attrib_method(state, method, data);

    /* Viewport */
    /* Viewport transform: window = ndc*scale + offset. The z lane is the
     * GL->[0,1] depth remap (offset.z=0.5/scale.z=0.5); without honoring it,
     * GL-convention projections (SDK gcm samples) get their near-camera
     * geometry clipped by D3D's 0<=z<=w rule (gcm/cube: missing polygons). */
L_vport:
    if (method >= 0x0A20 && method < 0x0A30) {
        u32 f = data; float v; memcpy(&v, &f, 4);
        state->viewport_offset[(method - 0x0A20) >> 2] = v;
        return 0;
    }
    if (method >= 0x0A30 && method < 0x0A40) {
        u32 f = data; float v; memcpy(&v, &f, 4);
        state->viewport_scale[(method - 0x0A30) >> 2] = v;
        return 0;
    }

L_view:
    if (method == 0x1D88) {   /* NV4097_SET_SHADER_WINDOW (v125) */
        state->shader_window = data;
        return 0;
    }
    if (method == NV4097_SET_VIEWPORT_HORIZONTAL) {
        state->viewport_x = data & 0xFFFF;
        state->viewport_w = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_VIEWPORT_VERTICAL) {
        state->viewport_y = data & 0xFFFF;
        state->viewport_h = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }

    /* Color mask */
L_cmask:
    if (method == NV4097_SET_COLOR_MASK) {
        state->color_mask = data;
        state->color_mask_dirty = 1;
        return 0;
    }

    /* Alpha test */
    if (method == NV4097_SET_ALPHA_TEST_ENABLE) {
        state->alpha_test_enable = data ? 1 : 0;
        state->alpha_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_ALPHA_FUNC) {
        state->alpha_func = data;
        state->alpha_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_ALPHA_REF) {
        state->alpha_ref = data;
        state->alpha_dirty = 1;
        return 0;
    }

    /* Clear */
L_clear:
    if (method == NV4097_SET_COLOR_CLEAR_VALUE) {
        state->color_clear_value = data;
        return 0;
    }
    if (method == NV4097_SET_ZSTENCIL_CLEAR_VALUE) {
        state->zstencil_clear_value = data;
        return 0;
    }
    if (method == NV4097_CLEAR_SURFACE) {
        { static int _c=0; int _first = PS3_LOG_FIRST(_c, 12); if (_first) fprintf(stderr, "[RSX] CLEAR_SURFACE mask=0x%X color=0x%08X\n", data, state->color_clear_value);
          /* Each distinct mask once more, however late: a depth-only clear
           * first shows up long after the boot screens used up the 12. */
          static u32 seen[16]; static int ns = 0; int k = 0;
          while (k < ns && seen[k] != data) k++;
          if (k == ns && ns < 16) { seen[ns++] = data;
              if (!_first) fprintf(stderr, "[RSX] CLEAR_SURFACE new mask=0x%X color=0x%08X\n",
                                   data, state->color_clear_value); } }
        if (s_backend && s_backend->clear) {
            float depth = (float)(state->zstencil_clear_value >> 8) / (float)0xFFFFFF;
            u8 stencil = state->zstencil_clear_value & 0xFF;
            s_backend->clear(s_backend->userdata, data,
                           state->color_clear_value, depth, stencil);
        }
        return 0;
    }

    /* Blend */
L_blend:
    if (method == NV4097_SET_BLEND_ENABLE) {
        state->blend_enable = data ? 1 : 0;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_SFACTOR) {
        state->blend_sfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_DFACTOR) {
        state->blend_dfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_EQUATION) {
        state->blend_equation = data;
        state->blend_dirty = 1;
        return 0;
    }

    /* Depth */
L_depth:
    if (method == NV4097_SET_DEPTH_TEST_ENABLE) {
        state->depth_test_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_FUNC) {
        state->depth_func = data;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_MASK) {
        state->depth_mask = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    /* Resistance port: polygon offset -- decals and blob shadows sit on the
     * ground with a depth bias; without it they z-fight (flickering black
     * patches that shift with the camera). */
    if (method == 0x0A68u) { state->poly_offset_fill = data ? 1 : 0; state->depth_dirty = 1; return 0; }
    if (method == 0x0A78u) { memcpy(&state->poly_offset_scale, &data, 4); state->depth_dirty = 1; return 0; }
    if (method == 0x0A7Cu) { memcpy(&state->poly_offset_bias, &data, 4); state->depth_dirty = 1; return 0; }

    /* Stencil */
L_stencil:
    if (method == NV4097_SET_STENCIL_TEST_ENABLE) {
        state->stencil_test_enable = data ? 1 : 0;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC) {
        state->stencil_func = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC_REF) {
        state->stencil_ref = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC_MASK) {
        state->stencil_mask = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_FAIL) {
        state->stencil_op_fail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_ZFAIL) {
        state->stencil_op_zfail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_ZPASS) {
        state->stencil_op_zpass = data;
        state->stencil_dirty = 1;
        return 0;
    }
    /* v178: write mask and the back-face (two-sided) set */
    switch (method) {
    case NV4097_SET_STENCIL_MASK:                  state->stencil_write_mask = data;      state->stencil_dirty = 1; return 0;
    case NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE: state->two_sided_stencil = data ? 1 : 0; state->stencil_dirty = 1; return 0;
    case NV4097_SET_BACK_STENCIL_MASK:             state->back_stencil_write_mask = data; state->stencil_dirty = 1; return 0;
    case NV4097_SET_BACK_STENCIL_FUNC:             state->back_stencil_func = data;       state->stencil_dirty = 1; return 0;
    case NV4097_SET_BACK_STENCIL_FUNC_REF:         state->back_stencil_ref = data;        state->stencil_dirty = 1; return 0;
    case NV4097_SET_BACK_STENCIL_FUNC_MASK:        state->back_stencil_mask = data;       state->stencil_dirty = 1; return 0;
    case NV4097_SET_BACK_STENCIL_OP_FAIL:          state->back_stencil_op_fail = data;    state->stencil_dirty = 1; return 0;
    case NV4097_SET_BACK_STENCIL_OP_ZFAIL:         state->back_stencil_op_zfail = data;   state->stencil_dirty = 1; return 0;
    case NV4097_SET_BACK_STENCIL_OP_ZPASS:         state->back_stencil_op_zpass = data;   state->stencil_dirty = 1; return 0;
    default: break;
    }

    /* Culling */
L_cull:
    if (method == NV4097_SET_CULL_FACE_ENABLE) {
        state->cull_face_enable = data ? 1 : 0;
        return 0;
    }
    if (method == NV4097_SET_CULL_FACE) {
        state->cull_face = data;
        return 0;
    }
    if (method == NV4097_SET_FRONT_FACE) {
        state->front_face = data;
        return 0;
    }

    /* Shader programs */
L_shader:
    if (method == NV4097_SET_SHADER_PROGRAM) {
        /*
         * Fragment program address register:
         *   [1:0]  location (0 = local, 1 = main)
         *   [31:2] offset (4-byte aligned)
         */
        state->shader_program = data;
        state->fragment_program_addr = data & ~0x3u;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_TRANSFORM_PROGRAM_LOAD) {
        /* Vertex program load slot — instruction index (each = 16 bytes).
         * Following NV4097_SET_TRANSFORM_PROGRAM words fill from here. */
        state->transform_program_load = data;
        state->vp_ucode_write = data * 16;
        state->shader_dirty = 1;
        return 0;
    }

    if (method == NV4097_SET_TRANSFORM_PROGRAM_START) {
        if (state->transform_program_start != data) state->vp_dirty = 1;
        state->transform_program_start = data;
        state->shader_dirty = 1;
        return 0;
    }

    /* NV4097_SET_TRANSFORM_PROGRAM[0..31] — a run of 32-bit vertex-program
     * microcode words appended at the current write cursor. Capture them so the
     * backend can decompile the real VP (4 words = one NV40 instruction). */
L_tprog:
    if (method >= NV4097_SET_TRANSFORM_PROGRAM &&
        method <  NV4097_SET_TRANSFORM_PROGRAM + 32 * 4) {
        u32 w = state->vp_ucode_write;
        if (w + 4 <= sizeof(state->vp_ucode)) {
            /* data is host-endian already; store as little-endian bytes. */
            state->vp_ucode[w+0] = (u8)(data);
            state->vp_ucode[w+1] = (u8)(data >> 8);
            state->vp_ucode[w+2] = (u8)(data >> 16);
            state->vp_ucode[w+3] = (u8)(data >> 24);
            state->vp_ucode_write = w + 4;
            if (w + 4 > state->vp_ucode_bytes) state->vp_ucode_bytes = w + 4;
            state->vp_dirty = 1;
            state->vp_ucode_gen++;
        }
        return 0;
    }
L_vaom:
    if (method == NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK) {
        state->vertex_attrib_output_mask = data;
        return 0;
    }
    /* NV4097_SET_VERTEX_DATA4F_M (0x1C00): the constant/"current" value for each
     * vertex attribute, 16 attributes x 4 floats. The hardware feeds this to
     * every vertex when the attribute's ARRAY is disabled -- it is not zero.
     *
     * Rubber Ducky leaves attribute 3 (diffuse colour) disabled and sets it here
     * instead; its duck shader computes (lighting * col0) * texture + spec, so a
     * zero col0 multiplies the texture away and the ducks rendered as a dim grey
     * specular term only -- present in the framebuffer, invisible on screen.
     * SET_VERTEX_DATA2F/4UB/2S/4S are the other encodings of the same register
     * file; add them if a title needs them. */
L_vdata:
    if (method >= 0x1C00u && method < 0x1C00u + RSX_MAX_VERTEX_ATTRIBS * 16u) {
        u32 idx  = (method - 0x1C00u) >> 4;         /* attribute */
        u32 lane = ((method - 0x1C00u) >> 2) & 3;   /* x/y/z/w   */
        float f; memcpy(&f, &data, 4);
        state->vertex_data4f[idx][lane] = f;
        { static int _wo = -1, _won = 0;
          if (_wo < 0) _wo = getenv("VDATA_OUTSIDE") ? 1 : 0;
          if (_wo && !state->in_begin_end && _won < 400) { _won++;
            fprintf(stderr, "[VOUT] m=0x%04X 4F a%u lane%u = %.3f%c", method, idx, lane, f, 10); } }
        { static int _wa = -2, _wn = 0;
          if (_wa == -2) { const char* e = getenv("VDATA_ATTR"); _wa = e ? atoi(e) : -1; }
          if (_wa == (int)idx && _wn < 300) { _wn++;
            fprintf(stderr, "[VATTR] m=0x%04X 4F lane%u = %.3f%c", method, lane, f, 10); } }
        if (idx == 0 && lane == 3) rsx_imm_emit(state);
        { static int _d = -1; if (_d < 0) _d = getenv("VDATA_DBG") ? 1 : 0;
          static int _n = 0;
          if (_d && _n < 400) { _n++;
            fprintf(stderr, "[VDATA4F] attr=%u lane=%u = %.4f%c", idx, lane, f, 10); } }
        return 0;
    }

    /* Resistance port: the other encodings of the constant vertex-attribute
     * register file (see the 4F_M comment above). Fall of Man feeds its UI
     * vertex colour (attribute 1) through one of these with the colour array
     * disabled; without them col0 read as (0,0,0,1) and every menu draw came
     * out black. Semantics follow RPCS3 NV47/HW/nv4097.h set_vertex_data_impl:
     * components not supplied default to (0,0,0,1). */
    {
        int idx = -1, n = 0, sub = 0; float v[4] = {0, 0, 0, 1}; int partial = 0;
        if (method >= 0x1940u && method < 0x1940u + RSX_MAX_VERTEX_ATTRIBS * 4u) {        /* 4UB_M */
            idx = (method - 0x1940u) >> 2; n = 4;
            /* Byte order measured on Fall of Man's UI colours (checked
             * against the real menu art): the word is 0xAABBGGRR -- R in the
             * low byte, alpha in the high byte. */
            v[0] = (float)( data        & 0xFFu) / 255.0f;   /* R */
            v[1] = (float)((data >>  8) & 0xFFu) / 255.0f;   /* G */
            v[2] = (float)((data >> 16) & 0xFFu) / 255.0f;   /* B */
            v[3] = (float)((data >> 24) & 0xFFu) / 255.0f;   /* A */
        } else if (method >= 0x1E40u && method < 0x1E40u + RSX_MAX_VERTEX_ATTRIBS * 4u) { /* 1F_M */
            idx = (method - 0x1E40u) >> 2; n = 1; memcpy(&v[0], &data, 4);
        } else if (method >= 0x1880u && method < 0x1880u + RSX_MAX_VERTEX_ATTRIBS * 8u) { /* 2F_M */
            idx = (method - 0x1880u) >> 3; sub = ((method - 0x1880u) >> 2) & 1; n = 2; partial = 1;
            memcpy(&v[0], &data, 4);
        } else if (method >= 0x1500u && method < 0x1500u + RSX_MAX_VERTEX_ATTRIBS * 16u) { /* 3F_M */
            idx = (method - 0x1500u) >> 4; sub = ((method - 0x1500u) >> 2) & 3; n = 3; partial = 1;
            if (sub > 2) return 0;
            memcpy(&v[0], &data, 4);
        } else if (method >= 0x1900u && method < 0x1900u + RSX_MAX_VERTEX_ATTRIBS * 4u) { /* 2S_M */
            idx = (method - 0x1900u) >> 2; n = 2;
            v[0] = (float)(s16)(data & 0xFFFFu); v[1] = (float)(s16)(data >> 16);
        } else if (method >= 0x1980u && method < 0x1980u + RSX_MAX_VERTEX_ATTRIBS * 8u) { /* 4S_M */
            idx = (method - 0x1980u) >> 3; sub = ((method - 0x1980u) >> 2) & 1; n = 4; partial = 2;
            v[0] = (float)(s16)(data & 0xFFFFu); v[1] = (float)(s16)(data >> 16);
        } else if (method >= 0x0A80u && method < 0x0A80u + RSX_MAX_VERTEX_ATTRIBS * 8u) { /* SCALED4S_M */
            idx = (method - 0x0A80u) >> 3; sub = ((method - 0x0A80u) >> 2) & 1; n = 4; partial = 2;
            float a = (float)(s16)(data & 0xFFFFu), b = (float)(s16)(data >> 16);
            v[0] = a < 0 ? a / 32768.0f : a / 32767.0f; v[1] = b < 0 ? b / 32768.0f : b / 32767.0f;
        }
        if (idx >= 0) {
            float* r = state->vertex_data4f[idx];
            if (partial == 1) {            /* one float lane per register */
                if (sub == 0) { for (int k = n; k < 4; k++) r[k] = (k == 3) ? 1.0f : 0.0f; }
                r[sub] = v[0];
            } else if (partial == 2) {     /* two 16-bit lanes per register */
                r[sub * 2] = v[0]; r[sub * 2 + 1] = v[1];
            } else {
                for (int k = 0; k < 4; k++) r[k] = v[k];
            }
            { static int _wo = -1, _won = 0;
              if (_wo < 0) _wo = getenv("VDATA_OUTSIDE") ? 1 : 0;
              if (_wo && !state->in_begin_end && _won < 400) { _won++;
                fprintf(stderr, "[VOUT] m=0x%04X data=0x%08X -> a%d=(%.3f %.3f %.3f %.3f)%c",
                        method, data, idx, r[0], r[1], r[2], r[3], 10); } }
            { static int _wa = -2, _wn = 0;
              if (_wa == -2) { const char* e = getenv("VDATA_ATTR"); _wa = e ? atoi(e) : -1; }
              if (_wa == idx && _wn < 300) { _wn++;
                fprintf(stderr, "[VATTR] m=0x%04X data=0x%08X -> a%d=(%.3f %.3f %.3f %.3f)%c",
                        method, data, idx, r[0], r[1], r[2], r[3], 10); } }
            if (idx == 0) {
                int last = (partial == 1) ? (sub == n - 1) : (partial == 2) ? (sub == 1) : 1;
                if (last) rsx_imm_emit(state);
            }
            { static int _d = -1; if (_d < 0) _d = getenv("VDATA_DBG") ? 1 : 0;
              static int _n = 0;
              if (_d && _n < 400) { _n++;
                fprintf(stderr, "[VDATA] m=0x%04X attr=%d -> (%.3f %.3f %.3f %.3f)%c",
                        method, idx, r[0], r[1], r[2], r[3], 10); } }
            return 0;
        }
    }

L_tcload:
    if (method == NV4097_SET_TRANSFORM_CONSTANT_LOAD) {
        { static int _n=0, _e=-1; if (_e < 0) _e = getenv("LOAD_DBG") ? 1 : 0; if (_e && PS3_LOG_FIRST(_n, 200))
            fprintf(stderr, "[LOAD] transform_constant_load = %u\n", data); }
        state->transform_constant_load = data;
        return 0;
    }

    /* Registers that sit right after the 32-dword constant window. Taken as
     * constants c(load+8..15) they overwrite whatever was uploaded there. */
    if (method == NV4097_SET_VERTEX_ATTRIB_INPUT_MASK) {
        state->vertex_attrib_input_mask = data;
        return 0;
    }
    if (method == NV4097_SET_TRANSFORM_BRANCH_BITS) {
        state->transform_branch_bits = data;
        return 0;
    }
    if (method == NV4097_INVALIDATE_L2) {
        /* Post-transform/L2 cache invalidate: the host keeps no such cache,
         * nothing to drop. */
        return 0;
    }

    /* NV4097_SET_TRANSFORM_CONSTANT[0..31] — up to 32 dwords (8 vec4s) per
     * command, 0x1F00..0x1F7C. Each register slot writes to a lane of one
     * vertex constant vec4: vec_index = LOAD + (reg_offset/4), lane = reg_offset%4.
     * The data arrives as a host-endian u32; reinterpret the bits as float
     * because the game's intent is "these 32 bits are a float". The hardware
     * does NOT auto-advance LOAD between commands — games re-issue
     * SET_TRANSFORM_CONSTANT_LOAD before each block. */
L_tconst:
    if (method >= NV4097_SET_TRANSFORM_CONSTANT &&
        method <  NV4097_SET_TRANSFORM_CONSTANT + NV4097_SET_TRANSFORM_CONSTANT_DWORDS * 4) {
        u32 reg_offset = (method - NV4097_SET_TRANSFORM_CONSTANT) / 4;
        u32 slot = state->transform_constant_load + (reg_offset >> 2);
        u32 lane = reg_offset & 3;
        if (slot < RSX_MAX_VERTEX_CONSTANTS) {
            float f;
            memcpy(&f, &data, 4);
            { static int _en=-1; if(_en<0){const char*e=getenv("TCONST_DBG");_en=e?1:0;}
              static int _n=0;
              static int _max=-1; if(_max<0){const char*m=getenv("TCONST_MAX");_max=m?atoi(m):64;}
              int _hit = _en && (getenv("TCONST_ALL") ? (_n<_max) : (slot>=12 && slot<=30 && _n<400));
              if(_hit){ _n++; fprintf(stderr,"[TCONST] load=%u slot=%u lane=%u = %.4f\n", state->transform_constant_load, slot, lane, f); } }
            state->vertex_constants[slot][lane] = f;
            { static int _sq=0, _e=-1; if (_e < 0) _e = getenv("SEQ_DBG") ? 1 : 0; if (_e && slot==256 && lane==0 && PS3_LOG_FIRST(_sq, 500))
                fprintf(stderr, "[SEQ] upload c256.x=%.4f (load=%u)\n", f, state->transform_constant_load); }
            if (!state->vertex_constants_dirty) {
                state->vertex_constants_lo = slot;
                state->vertex_constants_hi = slot;
                state->vertex_constants_dirty = 1;
            } else {
                if (slot < state->vertex_constants_lo) state->vertex_constants_lo = slot;
                if (slot > state->vertex_constants_hi) state->vertex_constants_hi = slot;
            }
        }
        return 0;
    }

    /* Draw */
L_draw:
    if (method == NV4097_SET_BEGIN_END) {
        if (data != 0) {
            state->primitive_type = data;
            state->in_begin_end = 1;
            state->begin_epoch++;
            s_imm_count = 0;

            /* Flush dirty state to backend before drawing */
            if (s_backend) {
                if (state->surface_dirty && s_backend->set_render_target)
                    s_backend->set_render_target(s_backend->userdata, state);
                if (state->viewport_dirty && s_backend->set_viewport)
                    s_backend->set_viewport(s_backend->userdata, state);
                if (state->blend_dirty && s_backend->set_blend)
                    s_backend->set_blend(s_backend->userdata, state);
                if ((state->depth_dirty || state->stencil_dirty) && s_backend->set_depth_stencil)
                    s_backend->set_depth_stencil(s_backend->userdata, state);
                if (state->color_mask_dirty && s_backend->set_color_mask)
                    s_backend->set_color_mask(s_backend->userdata, state);
                if (state->alpha_dirty && s_backend->set_alpha_test)
                    s_backend->set_alpha_test(s_backend->userdata, state);
                if (state->shader_dirty && s_backend->set_shader)
                    s_backend->set_shader(s_backend->userdata, state);
                if (state->vertex_dirty && s_backend->set_vertex_attribs)
                    s_backend->set_vertex_attribs(s_backend->userdata, state);

                /* Bind dirty textures */
                if (state->texture_dirty && s_backend->bind_texture) {
                    for (u32 i = 0; i < RSX_MAX_TEXTURES; i++) {
                        if (state->textures[i].dirty) {
                            s_backend->bind_texture(s_backend->userdata, i, &state->textures[i]);
                            state->textures[i].dirty = 0;
                        }
                    }
                }

                state->surface_dirty = 0;
                state->viewport_dirty = 0;
                state->blend_dirty = 0;
                state->depth_dirty = 0;
                state->stencil_dirty = 0;
                state->color_mask_dirty = 0;
                state->alpha_dirty = 0;
                state->shader_dirty = 0;
                state->vertex_dirty = 0;
                state->texture_dirty = 0;
            }
        } else {
            u32 prim = state->primitive_type;
            if (s_imm_count) rsx_imm_flush(state, prim);
            state->in_begin_end = 0;
        }
        return 0;
    }

    if (method == NV4097_DRAW_ARRAYS) {
        u32 first = data & 0xFFFFFF;
        u32 count = ((data >> 24) & 0xFF) + 1;
        { static int _d=0; if (PS3_LOG_FIRST(_d, 32)) fprintf(stderr, "[RSX] DRAW_ARRAYS prim=%u first=%u count=%u\n", state->primitive_type, first, count); }
        { static int _sq=0, _e=-1; if (_e < 0) _e = getenv("SEQ_DBG") ? 1 : 0; if (_e && PS3_LOG_FIRST(_sq, 500))
            fprintf(stderr, "[SEQ] DRAW surf0=0x%X c256.x=%.4f c257.y=%.4f\n",
                    state->surface_color_offset[0], state->vertex_constants[256][0],
                    state->vertex_constants[257][1]); }
        /* MVPDBG: dump every non-zero vertex-constant slot the FIRST time a
         * G-buffer draw (surf0=0xCC0000) is dispatched -- the true MVP the GPU
         * will use, read live (no snapshot/parity indirection). */
        static int s_mvp = -1; if (s_mvp < 0) s_mvp = getenv("MVPDBG") ? 1 : 0;
        if (s_mvp && state->surface_color_offset[0] == 0xCC0000) {
            static int _once = 0;
            if (!_once) { _once = 1;
                for (int s = 0; s < RSX_MAX_VERTEX_CONSTANTS; s++) {
                    float* v = state->vertex_constants[s];
                    if (v[0]||v[1]||v[2]||v[3])
                        fprintf(stderr, "[MVP] c[%3d] = %10.4f %10.4f %10.4f %10.4f\n",
                                s, v[0], v[1], v[2], v[3]);
                }
            }
        }
        ps3_ms("rsx:draw_arrays");
        if (s_backend && s_backend->draw_arrays)
            s_backend->draw_arrays(s_backend->userdata, state->primitive_type, first, count);
        return 0;
    }

    if (method == NV4097_SET_SHADER_CONTROL) {
        { static int _sc=0; if (PS3_LOG_FIRST(_sc, 12) && getenv("RTT_DUMP"))
            fprintf(stderr, "[RSXSC] shader_control=0x%X\n", data); }
        state->shader_control = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_INDEX_ARRAY_ADDRESS) {
        state->index_array_offset = data;
        return 0;
    }
    /* Resistance port: primitive restart. Fall of Man draws its ground as
     * indexed triangle strips cut by 0xFFFF; ignoring the cut fetched vertex
     * 65535 (spikes) and flipped every following strip's winding (culled). */
    if (method == 0x1DAC) { state->restart_index_enable = data & 1u; return 0; }
    if (method == 0x1DB0) { state->restart_index = data; return 0; }
    if (method == NV4097_SET_INDEX_ARRAY_DMA) {
        state->index_array_dma = data;
        return 0;
    }
    if (method == NV4097_DRAW_INDEX_ARRAY) {
        /* [23:0] first index, [31:24] count-1 (same packing as DRAW_ARRAYS). */
        u32 first = data & 0xFFFFFF;
        u32 count = ((data >> 24) & 0xFF) + 1;
        { static int _d=0; if (PS3_LOG_FIRST(_d, 8)) fprintf(stderr, "[RSX] DRAW_INDEX_ARRAY prim=%u first=%u count=%u idxoff=0x%X dma=0x%X\n", state->primitive_type, first, count, state->index_array_offset, state->index_array_dma); }
        ps3_ms("rsx:draw_indexed");
        if (s_backend && s_backend->draw_indexed)
            s_backend->draw_indexed(s_backend->userdata, state->primitive_type,
                                    first, count);
        return 0;
    }

    /* Scissor */
L_scissor:
    if (method == NV4097_SET_SCISSOR_HORIZONTAL) {
        state->scissor_x = data & 0xFFFF;
        state->scissor_w = (data >> 16) & 0xFFFF;
        return 0;
    }
    if (method == NV4097_SET_SCISSOR_VERTICAL) {
        state->scissor_y = data & 0xFFFF;
        state->scissor_h = (data >> 16) & 0xFFFF;
        return 0;
    }

    /* NV406E_SET_REFERENCE (0x0050): the title writes a reference value it then
     * spins on (cellGcmFinish / cellGcmSetWaitLabel). Record the latest so the
     * GCM control register can report completion back to the guest and unblock
     * the spin. Class 0 (software) method, no subchannel state needed here. */
    if (method == 0x0050) {
        g_rsx_last_reference = data;
        return 0;
    }

    /* GCM_FLIP_HEAD (0xE920/0xE924): immediate display flip from the FIFO.
     * The draw engine handles the actual present; suppress the unknown log. */
    if (method == 0xE920 || method == 0xE924) return 0;

    /* v140: nothing handles it (and the top of the chain has had its say):
     * next time return straight away. */
    if (method < 0x2000u && method != 0x0180u && method != 0x01A4u &&
        s_mcls[method >> 2] == MC_TOP && rsx_mcls_enabled())
        s_mcls[method >> 2] = MC_UNKNOWN;

    /* Unrecognized method -- log in debug builds */
#ifndef NDEBUG
    static int s_unknown_count = 0;
    if (s_unknown_count < 50) {
        printf("[RSX] unknown method 0x%04X = 0x%08X\n", method, data);
        s_unknown_count++;
    }
#endif

    return -1;
}

/* ---------------------------------------------------------------------------
 * Command buffer parsing
 * -----------------------------------------------------------------------*/

int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size)
{
    int methods_processed = 0;
    u32 pos = 0;
    u32 count = size / 4; /* size in dwords */

    { static int _c=0; if (count && PS3_LOG_FIRST(_c, 16)) fprintf(stderr, "[RSX] process_cmd_buffer words=%u first_hdr=0x%08X\n", count, buf[0]); }
    while (pos < count) {
        u32 header = buf[pos++];
        u32 type = (header >> 29) & 0x7;

        if (type == 0 || type == 2) {
            /* Increasing or non-increasing method */
            u32 method = (header >> 2) & 0x7FF;
            method <<= 2; /* method addresses are dword-aligned */
            u32 num_data = (header >> 18) & 0x7FF;
            int increasing = (type == 0);

            for (u32 i = 0; i < num_data && pos < count; i++) {
                u32 data = buf[pos++];
                u32 m = increasing ? (method + i * 4) : method;
                rsx_process_method(state, m, data);
                methods_processed++;
            }
        } else if (type == 1) {
            /* Jump — change command buffer read position */
            /* In recomp context, this is handled by the caller */
            break;
        } else {
            /* Unknown type, skip */
            break;
        }
    }

    return methods_processed;
}
