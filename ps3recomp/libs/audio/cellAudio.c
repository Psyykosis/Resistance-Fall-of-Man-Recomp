/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ps3emu/log_first.h"

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  ts_us;          /* v124: guest system time when read_index last advanced */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Request event-driven mode with a ~20ms buffer */
    REFERENCE_TIME buf_duration = 200000; /* 20ms in 100ns units */
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* Shared mode only takes the device's own mix format unless the stream
     * asks the audio engine to convert: on a device not running 48 kHz stereo
     * float (a 44.1 kHz or 5.1 output) Initialize failed with
     * AUDCLNT_E_UNSUPPORTED_FORMAT and every title ran silent. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            convert, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        /* PS3_MUTE=1: silence at the device, nothing else changes -- the
         * title still gets a working cellAudio and the device still paces
         * it, unlike PS3_NO_AUDIO, which sends the title down its no-audio
         * path. For unattended runs. */
        static int mute = -1;
        if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                               mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static unsigned g_audio_limited = 0;   /* blocks the limiter turned down (5 s report) */
static int g_audio_held_tick = 0;      /* a port waited this tick: no guest notify */
/* Mix one block from all active ports into s_mix_buffer (stereo float) */
/* Resistance port (v138): which speaker layout the game is mixing for. Every
 * 10 s of port-0 audio: each channel's share of the energy, and "surround"
 * when the centre/LFE/surround channels carry sound, "STEREO" when only L/R
 * do (the broken-positions state, which a launch picks at random). */
extern void rfom_snd_mode(int surround, double c_pct, double nan_pct);   /* project/stubs.cpp (v144) */
static void audio_mode_monitor(const float* src, u32 nch)
{
    static double e[8]; static u32 blocks; static int last = -1;
    static unsigned long long nans, samples;
    if (nch != 8) return;
    for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s += 4)
        for (u32 c = 0; c < 8; c++) {
            u32 v; memcpy(&v, &src[s * 8 + c], 4);
            v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
            if (v == 0x80000000u) continue;
            float f; memcpy(&f, &v, 4);
            samples++;
            if (f == f) e[c] += (double)f * f; else nans++;
        }
    if (++blocks < 1875u) return;   /* 10 s */
    double t = 0; for (int c = 0; c < 8; c++) t += e[c];
    if (t > 1e-6) {
        const double sur = e[2] + e[3] + e[4] + e[5] + e[6] + e[7];
        const int mode = sur > t * 0.01 ? 1 : 0;
        fprintf(stderr, "[audio-mode] %s | L %.0f%% R %.0f%% C %.0f%% LFE %.0f%% Ls %.0f%% Rs %.0f%% Lb %.0f%% Rb %.0f%%%s%c",
                mode ? "surround" : "STEREO",
                100*e[0]/t, 100*e[1]/t, 100*e[2]/t, 100*e[3]/t, 100*e[4]/t, 100*e[5]/t, 100*e[6]/t, 100*e[7]/t,
                (last >= 0 && last != mode) ? "  <-- changed" : "", 10);
        last = mode;
#ifdef _WIN32
        { FILE* mf = fopen("logs/sndmode.txt", "a");
          if (mf) { fprintf(mf, "pid=%lu t=%llu %s C=%.0f%% LFE=%.0f%% NaN=%llu\n", (unsigned long)GetCurrentProcessId(),
                            (unsigned long long)GetTickCount64(),
                            mode ? "surround" : "STEREO", 100*e[2]/t, 100*e[3]/t, nans); fclose(mf); } }
#endif
        rfom_snd_mode(mode, 100*e[2]/t, samples ? 100.0 * (double)nans / (double)samples : 0.0);
    }
    for (int c = 0; c < 8; c++) e[c] = 0;
    blocks = 0; nans = 0; samples = 0;
}
/* Resistance port (v132): F9 audio recording (see the mix loop). */
volatile long g_audio_rec_blocks = 0;   /* blocks still to record; set by the F9 key */
volatile long g_audio_rec_seq = 0;      /* bumped per request: a new file */
static void audio_rec_block(const float* src, u32 nch)
{
    static FILE* f = NULL; static long seq = 0; static u32 frames = 0, chs = 0;
    if (g_audio_rec_blocks <= 0) {
        if (f) goto finish;
        return;
    }
    if (!f || seq != g_audio_rec_seq) {
        if (f) { /* a new request while one runs: just keep going in this file */ }
        else {
            char fn[64]; seq = g_audio_rec_seq;
            snprintf(fn, sizeof fn, "logs/audiorec_%ld.wav", seq);
            f = fopen(fn, "wb");
            if (!f) { g_audio_rec_blocks = 0; return; }
            chs = nch; frames = 0;
            unsigned char h[44] = {0};
            fwrite(h, 1, 44, f);             /* header patched at the end */
            fprintf(stderr, "[audiorec] recording %s (%u channels)%c", fn, nch, 10);
        }
    }
    if (nch == chs) {
        const uint32_t* w = (const uint32_t*)src;
        uint32_t tmp[CELL_AUDIO_BLOCK_SAMPLES * 8];
        const u32 n = CELL_AUDIO_BLOCK_SAMPLES * nch;
        for (u32 k = 0; k < n && k < CELL_AUDIO_BLOCK_SAMPLES * 8; k++) {
            uint32_t v = w[k];
            v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
            if (v == 0x80000000u) v = 0;     /* the consumed-block sentinel (-0.0) */
            tmp[k] = v;
        }
        fwrite(tmp, 4, n, f);
        frames += CELL_AUDIO_BLOCK_SAMPLES;
    }
    if (InterlockedDecrement(&g_audio_rec_blocks) > 0) return;
finish:
    { const uint32_t data = frames * chs * 4, rate = 48000, br = rate * chs * 4;
      unsigned char h[44];
      memcpy(h, "RIFF", 4); uint32_t v = 36 + data; memcpy(h + 4, &v, 4);
      memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
      uint16_t s16 = 3; memcpy(h + 20, &s16, 2);            /* IEEE float */
      s16 = (uint16_t)chs; memcpy(h + 22, &s16, 2);
      memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4);
      s16 = (uint16_t)(chs * 4); memcpy(h + 32, &s16, 2);
      s16 = 32; memcpy(h + 34, &s16, 2);
      memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
      fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fclose(f); f = NULL;
      fprintf(stderr, "[audiorec] done: %u frames (%.1f s)%c", frames, frames / 48000.0, 10); }
}

static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        /* AUDIO_RINGLOG=1 (with AUDIO_SENTINEL): for 300 ticks out of every
         * 3000, port 0's whole ring at read time -- '#' written since we last
         * consumed it, '.' still the sentinel, '>' marks the block read now --
         * to see where and when the producer writes. */
#ifdef _WIN32
        { static int rl = -1;
          if (rl < 0) { const char* e = getenv("AUDIO_RINGLOG"); rl = e ? atoi(e) : 0; }
          static unsigned tick;
          if (rl && p == 0 && (tick++ % 3000u) < 300u) {
              char map[40]; u32 b;
              for (b = 0; b < nblock && b < 32; b++) {
                  const uint32_t* w = (const uint32_t*)(port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch);
                  u32 nn = 0;
                  for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i += 61) if (w[i] != 0x00000080u) { nn = 1; break; }
                  map[b] = (b == block_idx) ? (nn ? 'R' : 'r') : (nn ? '#' : '.');
              }
              map[b] = 0;
              LARGE_INTEGER qc, qf2; QueryPerformanceCounter(&qc); QueryPerformanceFrequency(&qf2);
              fprintf(stderr, "[ring] t=%.2fms ridx=%llu [%s]%c",
                      (double)qc.QuadPart * 1000.0 / (double)qf2.QuadPart,
                      (unsigned long long)port->read_index, map, 10);
          } }
#endif
        /* AUDIO_PORTWAV=<file>: port 0's raw block as the game wrote it (nch
         * channels, big-endian float), to tell a broken producer from a broken
         * fold. ffmpeg -f f32be -ar 48000 -ac 8 -i <file> */
        { static FILE* _pf = (FILE*)-1;
          if (_pf == (FILE*)-1) { const char* e = getenv("AUDIO_PORTWAV"); _pf = e ? fopen(e, "wb") : NULL; }
          if (_pf && p == 0) fwrite(src, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * nch, _pf); }
        /* Resistance port (v132): F9 in game = record the next 20 s of port 0
         * (the game's 8-channel mix, before the stereo fold) to
         * logs/audiorec_N.wav, 48 kHz 32-bit float, 8 channels
         * (L R C LFE Ls Rs Lb Rb). For diagnosing wrong 3D sound positions:
         * which output channels a sound lands in. */
        if (p == 0) { audio_rec_block(src, nch); audio_mode_monitor(src, nch); }
        /* v123: AUDIO_PORTWAV1=<file> -- the same for port 1 (libmixer: movies). */
        { static FILE* _pf1 = (FILE*)-1;
          if (_pf1 == (FILE*)-1) { const char* e = getenv("AUDIO_PORTWAV1"); _pf1 = e ? fopen(e, "wb") : NULL; }
          if (_pf1 && p == 1) fwrite(src, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * nch, _pf1); }
        /* Resistance port: underrun hold. A block that is still all zero right
         * after a block with sound is almost always one the game's producer
         * has not written yet (it runs late in a heavy frame) -- consuming it
         * played a 5 ms hole, the crackle. Instead, replay the last block
         * softer and DON'T advance the read index: the producer gets one more
         * block of lead, and the underrun turns into a 5 ms time stretch. At
         * most 2 holds in a row (genuine silence passes after 10 ms).
         * AUDIO_HOLD=0 = old behaviour. Gaps/holds logged every 5 s. */
        { static int hold_on = -1;
          if (hold_on < 0) { const char* e = getenv("AUDIO_HOLD"); hold_on = e ? atoi(e) : 0; }   /* v115: off -- it made the crackle worse and turned movie audio robotic */
          static unsigned char hist[CELL_AUDIO_PORT_MAX], run[CELL_AUDIO_PORT_MAX];
          static float last[CELL_AUDIO_PORT_MAX][CELL_AUDIO_BLOCK_SAMPLES * 8];
          static unsigned gaps, holds, blocks;
          /* raw big-endian words: +0.0 is 0, the -0.0 sentinel reads 0x00000080
           * on this little-endian host (as a host float it is a denormal, not 0) */
          int nz = 0;
          { const uint32_t* w = (const uint32_t*)src;
            for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 3)
                if (w[s2] != 0u && w[s2] != 0x00000080u) { nz = 1; break; } }
          blocks++;
          /* v120 wait-hold: with AUDIO_SENTINEL (consumed blocks are -0.0), a
           * block still all sentinel was NEVER WRITTEN -- the producer is late,
           * not silent (libmixer writes its blocks in bursts that land 1-2
           * blocks behind the read position in heavy scenes; consumed, those
           * late writes replayed 43 ms stale = the crackle). Wait for it: play
           * the last block softly, do not consume, do not notify the guest
           * (it writes one block per notify). At most 3 ticks in a row.
           * AUDIO_WAITHOLD=0 off. */
          static int wait_on = -1;
          if (wait_on < 0) { const char* e = getenv("AUDIO_WAITHOLD"); wait_on = e ? atoi(e) : 0; }   /* v121: off -- heavy scenes only get 60-80% of the audio, holding made it unrecognisable */
          int unwritten = 0;
          if (wait_on && !nz) {
              const uint32_t* w = (const uint32_t*)src; unwritten = 1;
              for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i += 13) if (w[i] != 0x00000080u) { unwritten = 0; break; }
          }
          const int hold = ((hold_on && !nz && (hist[p] & 1) && run[p] < 2) ||
                            (unwritten && run[p] < 3)) && nch <= 8;
          if (hold) g_audio_held_tick = 1;
          if (hold) {
              holds++; run[p]++;
              const float* lp = last[p];
              for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
                  float l = 0.0f, r = 0.0f;
                  if (nch >= 2) { l = ld_be_f32(&lp[s * nch]); r = ld_be_f32(&lp[s * nch + 1]); }
                  else l = r = ld_be_f32(&lp[s]);
                  if (nch == 8) {
                      const float c = ld_be_f32(&lp[s * 8 + 2]) * 0.707f + ld_be_f32(&lp[s * 8 + 3]) * 0.5f;
                      l += c + ld_be_f32(&lp[s * 8 + 4]) * 0.5f + ld_be_f32(&lp[s * 8 + 6]) * 0.3f;
                      r += c + ld_be_f32(&lp[s * 8 + 5]) * 0.5f + ld_be_f32(&lp[s * 8 + 7]) * 0.3f;
                  }
                  s_mix_buffer[s * 2 + 0] += l * level * 0.6f;
                  s_mix_buffer[s * 2 + 1] += r * level * 0.6f;
              }
          } else {
              if (nz && (hist[p] & 1) == 0 && (hist[p] & 0x6)) gaps++;
              run[p] = 0;
              hist[p] = (unsigned char)((hist[p] << 1) | (nz ? 1 : 0));
              if (nz && nch <= 8) memcpy(last[p], src, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));
          }
#ifdef _WIN32
          { static ULONGLONG t0; ULONGLONG now = GetTickCount64(); if (!t0) t0 = now;
            if (now - t0 >= 5000) {
                if (gaps || holds || g_audio_limited)
                    fprintf(stderr, "[audio] 5 s: %u underrun gap(s), %u hold(s), %u limited, %u blocks%c",
                            gaps, holds, g_audio_limited, blocks, 10);
                gaps = holds = blocks = 0; g_audio_limited = 0; t0 = now;
            } }
#endif
          if (hold) continue;   /* block not consumed: no clear, no read-index advance */
        }

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        /* Resistance port (v124): the movie port's LFE is left out of the stereo
         * fold. v123 dumps: libmixer's port-1 output is the decoded AC3 x 0.42,
         * sample-exact on L/R/C/Ls/Rs, but its LFE channel carries the LFE at
         * 12x for the first 43 samples of EVERY 256-sample block -- a
         * 187.5 Hz rasp under every bass line (the movie "static"). The bug is
         * in the game's lifted channel-strip code; dropping LFE is also what a
         * standard Dolby stereo downmix does. AUDIO_P1_LFE=<gain> (0.5 = old). */
        float lfe_gain = 0.5f;
        if (p == 1) { static float g1 = -1.0f;
                      if (g1 < 0.0f) { const char* e = getenv("AUDIO_P1_LFE"); g1 = e ? (float)atof(e) : 0.0f; }
                      lfe_gain = g1; }
        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&src[s * nch + 0]) * level;
                right = ld_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&src[s]) * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * lfe_gain;
                float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        /* AUDIO_SENTINEL=1 (diagnostic): clear consumed blocks to -0.0 instead
         * of +0.0 (equal as floats, inaudible), then classify every block at
         * read time: still all -0.0 = the producer never wrote it (late);
         * all +0.0 = the producer wrote silence. Reported every 5 s. */
#ifdef _WIN32
        { static int sen = -1;
          if (sen < 0) { const char* e = getenv("AUDIO_SENTINEL"); sen = e ? atoi(e) : 1; }   /* v120: on (wait-hold needs it) */
          if (sen) {
              static unsigned unwritten, silent, total; static ULONGLONG t0;
              const uint32_t* w = (const uint32_t*)src;
              const u32 nw = CELL_AUDIO_BLOCK_SAMPLES * nch;
              u32 neg = 0, pos = 0;
              for (u32 i = 0; i < nw; i++) { if (w[i] == 0x00000080u) neg++; else if (w[i] == 0) pos++; }
              if (neg == nw) unwritten++; else if (pos == nw) silent++;
              total++;
              ULONGLONG now = GetTickCount64(); if (!t0) t0 = now;
              if (now - t0 >= 5000) {
                  fprintf(stderr, "[audio-sentinel] 5 s: %u blocks never written, %u written as silence, %u total%c",
                          unwritten, silent, total, 10);
                  unwritten = silent = total = 0; t0 = now;
              }
              uint32_t* ww = (uint32_t*)src;
              for (u32 i = 0; i < nw; i++) ww[i] = 0x00000080u;   /* -0.0f, big-endian */
          } else
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float)); }
#else
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));
#endif

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        { extern uint64_t ps3_guest_system_time_us(void); port->ts_us = ps3_guest_system_time_us(); }
        /* Resistance port: AUDIO_LEAD (default 3 blocks, 16 ms): the index the
         * guest sees runs that many blocks AHEAD of the block this mixer reads.
         * Fall of Man's libmixer writes the block AT the published index -- the
         * one consumed 5.3 ms later -- so any wake-up later than one block left
         * that block unwritten (AUDIO_SENTINEL: up to 27% of blocks "never
         * written" in heavy scenes). The lead gives it 3 blocks of slack, the
         * cost is 16 ms more latency. 0 = old behaviour. */
        if (port->read_idx_addr) {
            static int lead = -1;
            if (lead < 0) { const char* e = getenv("AUDIO_LEAD"); lead = e ? atoi(e) : 0;   /* v119b: 3 made it worse */
                            if (lead < 0) lead = 0; }
            u32 l = (u32)lead; if (l > nblock - 2) l = nblock > 2 ? nblock - 2 : 0;
            vm_write64((u32)port->read_idx_addr, (port->read_index + l) % nblock);
        }
    }

    mutex_unlock(&s_audio_mutex);

    /* Resistance port: limiter instead of a bare clip. The 7.1 -> stereo fold
     * above adds centre, LFE and four surrounds into each side (gains summing
     * to ~3), so loud passages -- explosions, a movie's bass line -- went far
     * past full scale and the hard clip below turned them into crackle. A
     * block whose peak would clip has its gain pulled down to fit (ramped over
     * the first 64 samples, the block is known in full before output), then
     * recovers over ~0.3 s. AUDIO_LIMIT=0 = hard clip only. Peak reductions
     * are counted in the 5 s [audio] line. */
    { static int lim = -1; static float g = 1.0f;
      if (lim < 0) { const char* e = getenv("AUDIO_LIMIT"); lim = e ? atoi(e) : 1; }
      if (lim) {
          float pk = 0.0f;
          for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
              const float a2 = s_mix_buffer[i] < 0 ? -s_mix_buffer[i] : s_mix_buffer[i];
              if (a2 > pk) pk = a2;
          }
          float target = (pk * g > 0.97f) ? 0.97f / pk : g;
          if (target >= g) target = g + (1.0f - g) * 0.02f;     /* release */
          else g_audio_limited++;
          if (target > 1.0f) target = 1.0f;
          for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES; i++) {
              const float t = i < 64 ? (float)i / 64.0f : 1.0f;
              const float gg = target < g ? target : g + (target - g) * t;   /* down: at once */
              s_mix_buffer[i * 2]     *= gg;
              s_mix_buffer[i * 2 + 1] *= gg;
          }
          g = target;
      } }
    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid)
            sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
    }
}

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    /* Resistance port (v115): mix on a steady 5.33 ms clock, like the PS3,
     * and feed the device from a small FIFO. Pacing on device room mixed in
     * bursts -- the WASAPI event fires every ~10 ms, and each wake took two
     * or more blocks back to back -- so the guest's read index jumped ahead
     * of a producer that keeps only a block or two of lead: "underrun gaps"
     * even at full frame rate, more under load. AUDIO_STEADY=0 = old pacing. */
    static int steady = -1;
    if (steady < 0) { const char* e = getenv("AUDIO_STEADY"); steady = e ? atoi(e) : 1; }
    enum { FIFO_N = 24 };
    static float fifo[FIFO_N][CELL_AUDIO_BLOCK_SAMPLES * 2];
    unsigned fhead = 0, fcount = 0;
    HANDLE tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!tmr) tmr = CreateWaitableTimerW(NULL, FALSE, NULL);
    while (s_mix_thread_running) {
        if (steady) {
            /* 1. drain the FIFO into the device as far as it has room */
            while (fcount) {
                const int room = audio_backend_room();
                if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) break;
                const unsigned tail = (fhead + FIFO_N - fcount) % FIFO_N;
                if (room >= 0) audio_backend_submit(fifo[tail], CELL_AUDIO_BLOCK_SAMPLES);
                fcount--;
            }
            /* 2. next block due on the guest clock? */
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) {
                long long wait100ns = (due - now.QuadPart) * 10000000LL / qf.QuadPart;
                if (wait100ns > 20000) wait100ns = 20000;          /* <= 2 ms, then re-check the device */
                if (tmr) { LARGE_INTEGER d; d.QuadPart = -wait100ns;
                           SetWaitableTimer(tmr, &d, 0, NULL, NULL, FALSE);
                           WaitForSingleObject(tmr, 5); }
                else Sleep(1);
                continue;
            }
            if (now.QuadPart - due > qf.QuadPart / 20)          /* >50 ms behind: resync */
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            blocks++;
            audio_mix_one_block();
            { static FILE* _wf = (FILE*)-1;   /* AUDIO_WAV=<file>: raw f32le stereo 48 kHz of the final mix */
              if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
              if (_wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); }
            if (fcount == FIFO_N) fcount--;                       /* device stalled: drop the oldest */
            memcpy(fifo[fhead], s_mix_buffer, sizeof(fifo[0]));
            fhead = (fhead + 1) % FIFO_N; fcount++;
            if (!g_audio_held_tick) audio_notify_event_queues();
            g_audio_held_tick = 0;
            continue;
        }
        const int room = audio_backend_room();
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
        if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
        blocks++;
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        /* AUDIO_WAV=<file>: append the final mix as raw f32le stereo 48 kHz
         * (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). */
        { static FILE* _wf = (FILE*)-1;
          if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
          if (_wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); }
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, device buf %u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, 10);
#else
                        0u, 10);
#endif
                n = z = 0; d0 = g_audio_dropped; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
    /* Resistance port: this thread is the audio clock. It sleeps on the device
     * event between blocks, so a top priority costs nothing -- and at normal
     * priority a busy frame (9 polling SPU lanes, a spinning FIFO thread)
     * woke it late, and every late block is a crackle. AUDIO_MIX_PRIO=0 off. */
    { const char* e = getenv("AUDIO_MIX_PRIO");
      if (!e || atoi(e)) SetThreadPriority(s_mix_thread, THREAD_PRIORITY_TIME_CRITICAL); }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->write_index = 0;

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Each port reuses its own two pages in the runtime-owned audio window. */
    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    u32 guest_buf = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, VM_AUDIO_PORT_SIZE) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    u64 ts = s_ports[portNum].ts_us;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    /* Resistance port (v124): as RPCS3 -- the time the port's newest block
     * was consumed, minus one block period per block back to the tag, in the
     * guest's own clock (sys_time_get_system_time). It was the HOST's absolute
     * QPC time at cellAudioInit plus tag * period: hours ahead of the guest
     * clock, and blind to when this port actually started (movies open port 1
     * long after init). AUDIO_OLD_STAMP=1 = old value. */
    static int old = -1;
    if (old < 0) { const char* e = getenv("AUDIO_OLD_STAMP"); old = e ? atoi(e) : 0; }
    u64 t;
    if (old || !ts) t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    else {
        const u64 back = (global_counter - tag) * 256000000ULL / 48000ULL;
        t = ts > back ? ts - back : 0;
    }
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 24))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
