/*
 * ps3recomp - cellAdec HLE implementation
 *
 * Stub audio decoder. Accepts AU data and delivers AUDONE callbacks.
 * Actual decoding (AAC, ATRAC3+, etc.) requires integration with
 * an audio codec library (e.g., FFmpeg).
 */

#include "cellAdec.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/ppu/ppu_context.h" /* g_active_ctx -> the guest lr */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cbFunc is a GUEST OPD */
#include "../../runtime/memory/vm.h"     /* VM_HLE_INJECT_BASE */
#include "ps3emu/log_first.h"
#include "ffmpeg_runtime.h"

/* Resistance port: real AC3 decoding (Fall of Man's cutscene audio) through
 * the run-time FFmpeg, in the PCM format the title asked for at StartSeq
 * (CellAdecParamAc3: word size, output mode, LFE, channel map). The stub
 * delivered silence. ADEC_AC3=0 = silence again. */
#define ADEC_AC3_TYPE      2u          /* CELL_ADEC_TYPE_AC3 */
#define ADEC_BIG_EA(h)     (VM_HLE_INJECT_BASE + 0x400000u + (u32)(h) * 0x10000u)
#define ADEC_BIG_BYTES     0x10000u    /* 1536 samples x 6 ch x 4 bytes = 36 KB */

/* CellAdecPcmItem as the guest reads it, from RPCS3 Modules/cellAdec.h:339.
 * ps1_netemu copies the whole thing out with six 8-byte loads from offsets
 * 0x00..0x28 (at 0x000ED964), and that copy is what confirms these offsets
 * rather than a guess about padding: auInfo lands at 0x18, not 0x14, because it
 * contains a u64. The block must be 0x30 readable GUEST bytes. */
#define PCMITEM_PCM_HANDLE   0x00u
#define PCMITEM_STATUS       0x04u
#define PCMITEM_START_ADDR   0x08u
#define PCMITEM_SIZE         0x0Cu
#define PCMITEM_BSI_INFO     0x10u
#define PCMITEM_AU_START     0x18u
#define PCMITEM_AU_SIZE      0x1Cu
#define PCMITEM_AU_PTS_HI    0x20u
#define PCMITEM_AU_PTS_LO    0x24u
#define PCMITEM_AU_USERDATA  0x28u
#define PCMITEM_BYTES        0x30u

/* ponytail: one fixed-size silence buffer per handle, 1024 stereo float samples
 * -- the ATRAC3 frame this title (codecType 5) decodes. The ceiling is that it
 * is SILENCE and always this size; wire a real decoder in here when the audio
 * itself matters, without changing the guest-visible contract. */
#define ADEC_PCM_SAMPLES  1024u
#define ADEC_PCM_BYTES    (ADEC_PCM_SAMPLES * 2u * 4u)

/* Guest-visible scratch, in the HLE inject window rather than from the guest's
 * own heap. Taking 8 KB out of that heap in cellAdecOpen stalled the title
 * before it even reached cellAdecStartSeq -- ps1_netemu allocates its own
 * buffers from the same bump allocator, and perturbing it is not worth it for
 * two fixed-size blocks whose lifetime is the whole process.
 *
 * +0x40000 is clear of everything else in the window: labels +0x0000, control
 * +0x2000, callback +0x2F00, the offset tables +0x3000/+0x5000, and sys_rsx's
 * device/driver-info/reports pages at +0x30000/+0x31000/+0x38000. */
#define ADEC_SCRATCH_BASE   (VM_HLE_INJECT_BASE + 0x40000u)
#define ADEC_SCRATCH_STRIDE 0x4000u
#define ADEC_ITEM_EA(h)     (ADEC_SCRATCH_BASE + (u32)(h) * ADEC_SCRATCH_STRIDE)
#define ADEC_PCM_EA(h)      (ADEC_ITEM_EA(h) + 0x100u)
/* The codec info block the PcmItem's bsiInfo points to, in the gap between
 * the 0x30-byte item and the PCM buffer. */
#define ADEC_BSI_EA(h)      (ADEC_ITEM_EA(h) + 0x40u)
#define ADEC_BSI_BYTES      0x40u

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_ADEC 4

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;         /* guest EA of the callback's OPD */
    u32 cbArg;          /* guest EA handed back to it     */
    int seqStarted;
    u32 itemEa;         /* guest EA of the 0x30-byte CellAdecPcmItem */
    u32 pcmEa;          /* guest EA of the PCM buffer it points at    */
    int hasPcm;
    u32 auCount;        /* total AUs decoded */
    /* AC3 (Resistance port) */
    int  ac3;           /* decoding for real */
    u8   wordSize;      /* 0 int16, 1 float */
    u8   outMode;       /* CELL_ADEC_AC3_OUTPUT_MODE_* */
    u8   lfe;
    s32  chptr[6];      /* output slot -> input channel (0 L,1 C,2 R,3 l,4 r,5 s) */
    float pcmScale;     /* CellAdecParamAc3.pcmScaleFactor (1 = as decoded) */
    u32  pcmBytes;      /* size of the last decoded block */
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    AVCodecContext* ctx;
    AVPacket* pkt;
    AVFrame* frame;
#endif
} AdecSlot;

static AdecSlot s_adec[MAX_ADEC];

/* Fire one guest callback.
 *
 * The guest's cbFunc is a guest EA naming an OPD, so it goes through the
 * recompiled code's dispatcher. It used to be cast to a host function pointer
 * and called directly, which jumped to the guest address as host code:
 *
 *     [CRASH] code=0xC0000005 rip=00000000001B5F00
 *     [CRASH] last HLE NID (cellAdecDecodeAu)
 *
 * 0x1B5F00 is the guest OPD. That crash is what this function exists to stop.
 *
 * ponytail: dispatched SYNCHRONOUSLY on the caller's thread rather than from a
 * decoder thread as lv2 does. The ceiling is a guest callback that blocks
 * waiting on the thread that called DecodeAu -- give this its own thread if a
 * title ever deadlocks here. */
static void adec_notify(CellAdecHandle handle, u32 msg_type, s32 msg_data)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) return;
    const AdecSlot* a = &s_adec[handle];
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          printf("[cellAdec] notify h=%u msg=%u data=0x%X cb=0x%08X arg=0x%08X caller=%d\n",
                 handle, msg_type, (unsigned)msg_data, a->cbFunc, a->cbArg,
                 g_ps3_guest_caller ? 1 : 0); }
    if (!a->cbFunc || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(a->cbFunc, (u64)handle, (u64)msg_type,
                       (u64)(s64)msg_data, (u64)a->cbArg, 0, 0, 0, 0);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* cellAdecQueryAttr: how much work memory the decoder wants, before Open.
 * CellAdecAttr (guest, big-endian): +0 u32 workMemSize, +4 u32 adecVerUpper,
 * +8 u32 adecVerLower. Decoding is host-side and never touches the buffer;
 * see cellVdecQueryAttr. */
s32 cellAdecQueryAttr(const CellAdecType* type, void* attr)
{
    u32 ea = (u32)(uintptr_t)attr;
    if (!type || !ea)
        return (s32)CELL_ADEC_ERROR_ARG;
    vm_write32(ea + 0x0, 0x40000);    /* workMemSize: 256 KB */
    vm_write32(ea + 0x4, 0x00010000); /* adecVerUpper */
    vm_write32(ea + 0x8, 0);          /* adecVerLower */
    return CELL_OK;
}

/* cellAdecOpen(type, res, cb, handle) -- FOUR arguments.
 *
 * This took five, splitting the guest's CellAdecCb struct into cbFunc + cbArg,
 * which pushed `handle` off r6 onto r7. r7 held whatever happened to be there, so
 * the null check failed and every Open returned CELL_ADEC_ERROR_ARG. ps1_netemu
 * opens a decoder for CD-DA/XA audio inside its CD-ROM constructor and gives up on
 * the error -- so the disc was never mounted and the emulator sat in its run loop
 * with nothing to run. Signature confirmed against RPCS3 (Modules/cellAdec.cpp).
 *
 * cb is a guest pointer to { u32 cbFunc; u32 cbArg; }. */
s32 cellAdecOpen(const CellAdecType* type, const CellAdecResource* res,
                 const CellAdecCb* cb, CellAdecHandle* handle)
{
    (void)res;

    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;   /* audioCodecType */
    u32 cb_ea      = (u32)(uintptr_t)cb;
    u32 handle_ea  = (u32)(uintptr_t)handle;
    printf("[cellAdec] Open(codecType=%u, cb=0x%08X, handle=0x%08X)\n",
           codec_type, cb_ea, handle_ea);

    if (!type || !handle_ea)
        return (s32)CELL_ADEC_ERROR_ARG;

    for (int i = 0; i < MAX_ADEC; i++) {
        if (!s_adec[i].in_use) {
            memset(&s_adec[i], 0, sizeof(AdecSlot));
            s_adec[i].in_use    = 1;
            s_adec[i].codecType = codec_type;
            if (cb_ea) {
                s_adec[i].cbFunc = vm_read32(cb_ea + 0);
                s_adec[i].cbArg  = vm_read32(cb_ea + 4);
            }
            /* The PcmItem is handed to the guest BY POINTER, so it cannot live
             * in host memory -- returning &host_struct is what crashed here
             * once already. */
            s_adec[i].itemEa = ADEC_ITEM_EA(i);
            s_adec[i].pcmEa  = ADEC_PCM_EA(i);
            vm_write32(handle_ea, (u32)i);
            printf("[cellAdec] Open -> handle=%u item=0x%08X pcm=0x%08X\n",
                   i, s_adec[i].itemEa, s_adec[i].pcmEa);
            return CELL_OK;
        }
    }
    return (s32)CELL_ADEC_ERROR_BUSY;
}

s32 cellAdecClose(CellAdecHandle handle)
{
    printf("[cellAdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

#ifdef PS3RECOMP_FFMPEG_RUNTIME
    { AdecSlot* a = &s_adec[handle]; const PS3FFmpeg* ff = a->ac3 ? ps3_ffmpeg_get() : NULL;
      if (ff) { if (a->ctx) ff->avcodec_free_context(&a->ctx);
                if (a->pkt) { a->pkt->data = NULL; a->pkt->size = 0; ff->av_packet_free(&a->pkt); }
                if (a->frame) ff->av_frame_free(&a->frame); }
      a->ac3 = 0; }
#endif
    s_adec[handle].in_use = 0;
    return CELL_OK;
}

s32 cellAdecStartSeq(CellAdecHandle handle, void* param)
{
    printf("[cellAdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];
    const u32 pe = GUEST_EA(param);
    if (a->codecType == ADEC_AC3_TYPE && pe) {
        /* CellAdecParamAc3 (RPCS3 cellAdec.h): u8 wordSize, outputMode, outLfeOn,
         * pad, f32 drcCut @4, drcBoost @8, u8 compression @C, numberOfChannels
         * @D, stereo @E, dualmono @F, karaoke @10, f32 pcmScale @14,
         * s32 channelPointer0..5 @18. */
        a->wordSize = vm_read8(pe + 0x0);
        a->outMode  = vm_read8(pe + 0x1);
        a->lfe      = vm_read8(pe + 0x2);
        for (int k = 0; k < 6; k++) a->chptr[k] = (s32)vm_read32(pe + 0x18 + 4u * (u32)k);
        { u32 b; float drc_cut, drc_boost, scale;
          b = vm_read32(pe + 0x04); memcpy(&drc_cut, &b, 4);
          b = vm_read32(pe + 0x08); memcpy(&drc_boost, &b, 4);
          b = vm_read32(pe + 0x14); memcpy(&scale, &b, 4);
          /* v123: the title's PCM scale is applied (a real decoder does); a
           * value outside (0, 1] is reported and ignored */
          a->pcmScale = (scale > 0.0f && scale <= 1.0f) ? scale : 1.0f;
          printf("[cellAdec] AC3 param: wordSize=%u outputMode=%u lfe=%u ch=%d,%d,%d,%d,%d,%d"
                 " drcCut=%g drcBoost=%g compression=%u nch=%u stereo=%u pcmScale=%g\n",
                 a->wordSize, a->outMode, a->lfe, a->chptr[0], a->chptr[1], a->chptr[2],
                 a->chptr[3], a->chptr[4], a->chptr[5], (double)drc_cut, (double)drc_boost,
                 vm_read8(pe + 0x0C), vm_read8(pe + 0x0D), vm_read8(pe + 0x0E), (double)scale); }
#ifdef PS3RECOMP_FFMPEG_RUNTIME
        const char* e = getenv("ADEC_AC3");
        const PS3FFmpeg* ff = (e && atoi(e) == 0) ? NULL : ps3_ffmpeg_get();
        if (ff && !a->ctx) {
            const AVCodec* c = ff->avcodec_find_decoder(AV_CODEC_ID_AC3);
            if (c) {
                a->ctx = ff->avcodec_alloc_context3(c);
                a->pkt = ff->av_packet_alloc();
                a->frame = ff->av_frame_alloc();
                if (a->ctx) a->ctx->thread_count = 1;
                if (a->ctx && a->pkt && a->frame && ff->avcodec_open2(a->ctx, c, NULL) >= 0) a->ac3 = 1;
            }
            printf("[cellAdec] AC3 decoder %s\n", a->ac3 ? "open (libavcodec)" : "unavailable -- silence");
        }
#endif
    }

    s_adec[handle].seqStarted = 1;
    return CELL_OK;
}

s32 cellAdecEndSeq(CellAdecHandle handle)
{
    /* ADEC_WHO=1: name the guest function driving this. The title calls EndSeq
     * hundreds of times against a single StartSeq, and the image has eight
     * EndSeq call sites -- the backtrace says which one. */
    { static int _n = -1;
      if (_n < 0) _n = getenv("ADEC_WHO") ? 0 : -2;
      if (_n >= 0 && _n < 8) { _n++;
          /* The guest lr, not a host backtrace. ppu_guest_caller maps host
           * frames to the nearest lifted function, and here it landed on
           * func_00013040 -- a single `blr`, so plainly a mis-attribution.
           * lr is written by the `bl` to the import stub, so it is exactly
           * (call site + 4) for a call like this one. */
          extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
          printf("[cellAdec] EndSeq from guest lr=0x%08X tid=%llu\n",
                 g_active_ctx ? (u32)g_active_ctx->lr : 0u,
                 g_active_ctx ? (unsigned long long)g_active_ctx->thread_id : 0ull); } }
    printf("[cellAdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    s_adec[handle].seqStarted = 0;

    adec_notify(handle, CELL_ADEC_MSG_TYPE_SEQDONE, CELL_OK);

    return CELL_OK;
}

/* Decode one AC3 AU into dst (guest EA) in the title's format; returns bytes.
 * On any failure: 1536 frames of silence in that format. */
static u32 adec_ac3_decode(AdecSlot* a, CellAdecHandle h, u32 src, u32 size, u32 dst)
{
    (void)h;
    static const u8 mode_ch[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    const u32 nout = (u32)mode_ch[a->outMode & 7] + (a->lfe ? 1u : 0u);
    const u32 wb = a->wordSize ? 4u : 2u;
    u32 frames = 1536;
    float out[1536 * 6];
    memset(out, 0, sizeof out);
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    const PS3FFmpeg* ff = a->ac3 ? ps3_ffmpeg_get() : NULL;
    if (ff && size && size <= 4096) {
        static u8 buf[4096 + 64];
        for (u32 i = 0; i < size; i++) buf[i] = vm_read8(src + i);
        memset(buf + size, 0, 64);
        a->pkt->data = buf; a->pkt->size = (int)size;
        if (ff->avcodec_send_packet(a->ctx, a->pkt) >= 0 &&
            ff->avcodec_receive_frame(a->ctx, a->frame) >= 0) {
            AVFrame* f = a->frame;
            const int nch = f->ch_layout.nb_channels;
            const int ns = f->nb_samples < 1536 ? f->nb_samples : 1536;
            frames = (u32)ns;
            /* input planes by AC3 name: 0 L, 1 C, 2 R, 3 l (Ls), 4 r (Rs), 5 s (LFE).
             * FFmpeg native order = ascending AV_CH_* bits. */
            const float* in[6] = {0, 0, 0, 0, 0, 0};
            if (f->format == AV_SAMPLE_FMT_FLTP) {
                const uint64_t m = (f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) ? f->ch_layout.u.mask : 0;
                int idx = 0;
                for (int b = 0; b < 64 && idx < nch; b++) {
                    if (m && !(m & (1ull << b))) continue;
                    const float* pl = (const float*)f->extended_data[idx++];
                    const uint64_t bit = m ? (1ull << b) : 0;
                    if (!m) { if (idx == 1) in[0] = pl; else if (idx == 2) in[2] = pl; continue; }
                    if (bit == AV_CH_FRONT_LEFT) in[0] = pl;
                    else if (bit == AV_CH_FRONT_CENTER) in[1] = pl;
                    else if (bit == AV_CH_FRONT_RIGHT) in[2] = pl;
                    else if (bit == AV_CH_SIDE_LEFT || bit == AV_CH_BACK_LEFT) in[3] = pl;
                    else if (bit == AV_CH_SIDE_RIGHT || bit == AV_CH_BACK_RIGHT) in[4] = pl;
                    else if (bit == AV_CH_LOW_FREQUENCY) in[5] = pl;
                    else if (bit == AV_CH_BACK_CENTER && !in[3]) in[3] = in[4] = pl;
                }
                if (!in[0] && in[1]) in[0] = in[2] = in[1];   /* mono source */
                for (u32 k = 0; k < nout && k < 6; k++) {
                    const s32 c = a->chptr[k];
                    for (int i = 0; i < ns; i++) {
                        float v = 0.0f;
                        if (nout <= 2 && (c == 0 || c == 2)) {
                            /* stereo / mono out: fold centre and surround in */
                            const float* fr = in[c];
                            const float* sr = in[c == 0 ? 3 : 4];
                            v = (fr ? fr[i] : 0.0f) + (in[1] ? 0.7071f * in[1][i] : 0.0f) +
                                (sr ? 0.7071f * sr[i] : 0.0f);
                            v *= 0.7f;
                        } else if (c >= 0 && c < 6 && in[c]) v = in[c][i];
                        out[(u32)i * nout + k] = v;
                    }
                }
                { static int _n = 0; if (PS3_LOG_FIRST(_n, 2))
                    printf("[cellAdec] AC3 frame: %d ch (mask 0x%llX) %d samples -> %u ch %s%c", nch,
                           (unsigned long long)m, ns, nout, a->wordSize ? "float" : "int16", 10); }
            }
            ff->av_frame_unref(f);
        } else {
            static int _n = 0; if (PS3_LOG_FIRST(_n, 4)) printf("[cellAdec] AC3 decode failed (size %u)%c", size, 10);
        }
    }
#endif
    const u32 n = frames * nout;
    /* v123 (diagnostic): ADEC_DUMP=<file> -- every decoded AC3 frame exactly as
     * handed to the title (nout channels interleaved, f32 little-endian, 48 kHz),
     * to tell a bad decode from a bad mix downstream. Clipped samples counted. */
    { const float sc = a->pcmScale > 0.0f ? a->pcmScale : 1.0f;
      if (sc != 1.0f) for (u32 i = 0; i < n; i++) out[i] *= sc; }
    { static FILE* df = (FILE*)-1;
      if (df == (FILE*)-1) { const char* e = getenv("ADEC_DUMP"); df = e ? fopen(e, "wb") : NULL;
                             if (e) printf("[cellAdec] dumping AC3 PCM to %s (%u ch f32le)\n", e, nout); }
      if (df) fwrite(out, sizeof(float), n, df);
      static unsigned clipped, fr; fr++;
      for (u32 i = 0; i < n; i++) if (out[i] > 1.0f || out[i] < -1.0f) clipped++;
      if (fr % 160 == 0) { if (clipped) printf("[cellAdec] AC3: %u samples over full scale in the last 160 frames\n", clipped);
                           clipped = 0; } }
    for (u32 i = 0; i < n; i++) {
        float v = out[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        if (wb == 4) { u32 bits; memcpy(&bits, &v, 4); vm_write32(dst + i * 4u, bits); }
        else vm_write16(dst + i * 2u, (u16)(s16)(v * 32767.0f));
    }
    return n * wb;
}

s32 cellAdecDecodeAu(CellAdecHandle handle, const CellAdecAuInfo* auInfo)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];

    printf("[cellAdec] DecodeAu(handle=%u, addr=0x%X, size=%u)\n",
           handle, vm_read32(GUEST_EA(auInfo) + (u32)offsetof(CellAdecAuInfo, startAddr)),
           vm_read32(GUEST_EA(auInfo) + (u32)offsetof(CellAdecAuInfo, size)));

    /* Step 1: report the AU consumed. msgData is the AU INFO ADDRESS, not a
     * status -- see RPCS3 Modules/cellAdec.cpp:1345. A decoder that tracks its
     * outstanding AUs by address gets a null back if this is CELL_OK. */
    adec_notify(handle, CELL_ADEC_MSG_TYPE_AUDONE, (s32)GUEST_EA(auInfo));

    /* Step 2: Generate PCMOUT callback with dummy PCM info.
     * Without FFmpeg, we produce silence. But games that check for
     * decode completion via callbacks will proceed correctly. */
    a->auCount++;
    {
        const u32 au = GUEST_EA(auInfo);
        const u32 it = a->itemEa;
        u32 pcm_ea = a->pcmEa, pcm_bytes = ADEC_PCM_BYTES;
        if (a->codecType == ADEC_AC3_TYPE) {
            pcm_ea = ADEC_BIG_EA(handle);
            pcm_bytes = adec_ac3_decode(a, handle,
                                        vm_read32(au + (u32)offsetof(CellAdecAuInfo, startAddr)),
                                        vm_read32(au + (u32)offsetof(CellAdecAuInfo, size)), pcm_ea);
            a->pcmBytes = pcm_bytes;
        } else
        for (u32 o = 0; o < ADEC_PCM_BYTES; o += 4) vm_write32(a->pcmEa + o, 0);

        vm_write32(it + PCMITEM_PCM_HANDLE, (u32)handle);
        vm_write32(it + PCMITEM_STATUS,     0);              /* CELL_OK */
        vm_write32(it + PCMITEM_START_ADDR, pcm_ea);
        vm_write32(it + PCMITEM_SIZE,       pcm_bytes);
        /* bsiInfo must point at a codec info block, not be null. Resistance:
         * Fall of Man sizes each AC3 block from bsiInfo+0x14; with a null
         * pointer that read address 0x14 and came back 0, so its ring buffer
         * never filled, it never started its audio port, and its movie player
         * waited forever. ponytail: only that output-size word is filled; the
         * rest of the block is zero until a title reads another field. */
        const u32 bsi = ADEC_BSI_EA(handle);
        for (u32 o = 0; o < ADEC_BSI_BYTES; o += 4) vm_write32(bsi + o, 0);
        vm_write32(bsi + 0x00, a->codecType);
        vm_write32(bsi + 0x14, pcm_bytes);   /* CellAdecBsiAc3.pcmSize */
        vm_write32(it + PCMITEM_BSI_INFO,   bsi);
        /* Echo the AU back, which is what a real decoder does -- the guest
         * matches returned PCM against the AU it submitted. */
        vm_write32(it + PCMITEM_AU_START,
                   vm_read32(au + (u32)offsetof(CellAdecAuInfo, startAddr)));
        vm_write32(it + PCMITEM_AU_SIZE,
                   vm_read32(au + (u32)offsetof(CellAdecAuInfo, size)));
        vm_write32(it + PCMITEM_AU_PTS_HI,       vm_read32(au + 0x08));
        vm_write32(it + PCMITEM_AU_PTS_LO,       vm_read32(au + 0x0C));
        vm_write32(it + PCMITEM_AU_USERDATA,     vm_read32(au + 0x10));
        vm_write32(it + PCMITEM_AU_USERDATA + 4, vm_read32(au + 0x14));
    }
    a->hasPcm = 1;

    adec_notify(handle, CELL_ADEC_MSG_TYPE_PCMOUT, CELL_OK);

    return CELL_OK;
}

s32 cellAdecGetPcm(CellAdecHandle handle, void* outBuffer)
{
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 6))
        printf("[cellAdec] GetPcm(handle=%u out=0x%08X)\n",
               handle, GUEST_EA(outBuffer)); }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    /* outBuffer is the GUEST's buffer. Leaving it untouched hands the title
     * whatever happened to be there, which it then plays. */
    { const u32 out = GUEST_EA(outBuffer);
      const AdecSlot* a = &s_adec[handle];
      if (out && a->codecType == ADEC_AC3_TYPE) {
          const u32 src = ADEC_BIG_EA(handle);
          for (u32 o = 0; o < a->pcmBytes; o += 4) vm_write32(out + o, vm_read32(src + o));
      } else if (out) for (u32 o = 0; o < ADEC_PCM_BYTES; o += 4) vm_write32(out + o, 0); }

    s_adec[handle].hasPcm = 0;
    return CELL_OK;
}

s32 cellAdecGetPcmItem(CellAdecHandle handle, const CellAdecPcmItem** pcmItem)
{
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 6))
        printf("[cellAdec] GetPcmItem(handle=%u out=0x%08X hasPcm=%d)\n",
               handle, GUEST_EA(pcmItem),
               handle < MAX_ADEC ? s_adec[handle].hasPcm : -1); }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    /* pcmItem is a GUEST CellAdecPcmItem**. Writing &host_struct here put a
     * HOST address into guest memory; the guest dereferenced it and died on a
     * wild address (rip 0x870D0E, fault 0xCFFDFFF0). Write the guest EA. */
    if (pcmItem)
        vm_write32(GUEST_EA(pcmItem), s_adec[handle].itemEa);

    return CELL_OK;
}
