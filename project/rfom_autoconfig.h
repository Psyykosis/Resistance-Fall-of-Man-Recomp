// rfom_autoconfig.h -- hardware detection and the recommended settings for Resistance: Fall of Man's
// D3D12 renderer.
//
// Shared by Launcher.exe (project/launcher.cpp: first-run defaults, "Auto-detect (recommended)", the
// "recommended" markers) and the game (RFOM_PRESET=auto|low|medium|high|ultra, project/rfom_pcport.inc),
// so both pick the same settings from the same rules. Header-only, Windows-only; dxgi.dll is loaded at
// run time. Adapted from the Resistance 2 port's r2_autoconfig.h.
//
//   Hardware hw; detect(hw);          // GPU (the one the renderer uses), CPU, RAM, screen
//   Settings s = recommend(hw);       // or recommend(hw, parse_preset("high"))
//   apply_preset_env("auto");         // the game: export the settings as unset env vars
//
// Values are the ones settings.bat uses. RSX_RES_SCALE is relative to the PS3's 1280 x 720 and is the
// OUTPUT resolution of DLSS / FSR; RSX_UPSCALE is the share of it the 3D scene is drawn at. So
// RSX_RES_SCALE=3 + RSX_UPSCALE=0.5 + RSX_FSR=2 = the scene at 1920x1080, DLSS Performance to 3840x2160,
// shown on the screen (scaled down on a 1080p / 1440p monitor = supersampling).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

namespace rfac
{
enum Tier { T_AUTO = -1, T_LOW = 0, T_MEDIUM, T_HIGH, T_ULTRA, T_COUNT };
inline const char* tier_name(int t)
{
    static const char* n[T_COUNT] = { "Low", "Medium", "High", "Ultra" };
    return t >= 0 && t < T_COUNT ? n[t] : "Auto";
}
// "auto" (or empty / unknown) -> T_AUTO
inline int parse_preset(const char* p)
{
    if (!p || !*p) return T_AUTO;
    if (!_stricmp(p, "low") || !_stricmp(p, "min") || !_stricmp(p, "minimum")) return T_LOW;
    if (!_stricmp(p, "medium") || !_stricmp(p, "mid")) return T_MEDIUM;
    if (!_stricmp(p, "high")) return T_HIGH;
    if (!_stricmp(p, "ultra") || !_stricmp(p, "max")) return T_ULTRA;
    return T_AUTO;
}

struct Hardware
{
    char gpu[128] = "unknown GPU";
    unsigned vendor = 0;          // PCI vendor id: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    double vram_gb = 0;           // dedicated video memory
    int cores = 0, threads = 0;   // physical cores / logical CPUs
    int pcores = 0, ecores = 0;   // hybrid CPUs: performance / efficiency cores
    double ram_gb = 0;
    int screen_w = 0, screen_h = 0, refresh = 60;   // primary display, physical pixels
    bool integrated = false;
    bool rtx = false;             // GeForce / Quadro RTX: DLSS
    bool strong = false;          // RTX 4070 / 5070 class or better (or the AMD equivalent)
    int gpu_class = 0;            // 0 very weak / iGPU, 1 weak (2-4 GB), 2 mid, 3 high, 4 strong
    double eff_cores = 0;         // cores the game can use well (hybrid: P + E/2)
};

struct Settings
{
    int tier = T_MEDIUM;
    const char* fps = "60";       // CHILD_RFOM_FPS
    const char* vsync = "0";      // RSX_VSYNC
    const char* res = "1.5";      // RSX_RES_SCALE (output resolution, x 1280x720)
    const char* fsr = "1";        // RSX_FSR: 0 off, 1 FSR 3.1, 2 DLSS (falls back to FSR 3.1, then plain scaling)
    const char* upscale = "1";    // RSX_UPSCALE (with RSX_FSR > 0)
    const char* aniso = "16";     // RSX_ANISO
    const char* filter = "linear";// RSX_FILTER
    const char* sharpen = "0.25"; // RSX_SHARPEN
};
// ---- helpers ---------------------------------------------------------------
inline bool has(const char* s, const char* sub)
{
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}
// The model number after a marker ("RTX 4070 Ti" -> 4070; "RX 9070 XT" -> 9070), 0 if none.
inline int model_after(const char* s, const char* marker)
{
    const size_t ml = strlen(marker);
    for (; *s; s++) {
        if (_strnicmp(s, marker, ml)) continue;
        const char* p = s + ml;
        while (*p == ' ') p++;
        if (isdigit((unsigned char)*p)) return atoi(p);
    }
    return 0;
}

// Integrated GPUs: DXGI has no flag for it, so name and memory (iGPUs report a small carve-out).
inline bool is_integrated(unsigned vendor, const char* name, double vram_gb)
{
    if (vendor == 0x10DE) return false;
    if (vram_gb < 0.5) return true;
    if (vendor == 0x8086)   // Arc A/B desktop and laptop cards are discrete; "Arc(TM) Graphics" (Meteor Lake) is not
        return !(model_after(name, "Arc(TM) A") || model_after(name, "Arc(TM) B") || model_after(name, "Arc A") ||
                 model_after(name, "Arc B") || has(name, "Iris Xe MAX"));
    if (vendor == 0x1002)   // "AMD Radeon(TM) Graphics", "Radeon 780M Graphics", "Vega 8 Graphics"
        return has(name, "Graphics") && !has(name, "RX") && !has(name, "Pro ");
    return vendor != 0x1414 ? vram_gb < 1.0 : true;
}

// Fills the derived fields from the raw ones.
inline void classify(Hardware& h)
{
    const char* n = h.gpu;
    const double gb = h.vram_gb;
    h.integrated = is_integrated(h.vendor, n, gb);
    h.rtx = h.vendor == 0x10DE && has(n, "RTX");
    if (h.cores <= 0) h.cores = h.threads > 0 ? h.threads : 4;
    if (h.threads < h.cores) h.threads = h.cores;
    h.eff_cores = (h.pcores && h.ecores) ? h.pcores + h.ecores / 2.0 : (double)h.cores;

    int cls;
    if (h.integrated) {
        // recent iGPUs are about a GTX 1050-1650: AMD 680M/780M/880M/890M/8060S, Intel Arc (Meteor/Lunar Lake)
        const bool fast_igpu = has(n, "680M") || has(n, "760M") || has(n, "780M") || has(n, "880M") ||
                               has(n, "890M") || has(n, "8050S") || has(n, "8060S") || has(n, "Arc");
        cls = fast_igpu ? 1 : 0;
    } else if (gb < 1.5) cls = 0;
    else if (gb < 3.5) cls = 1;
    else if (gb < 7.0) cls = 2;
    else cls = 3;

    if (!h.integrated) {
        // name-based caps for old cards with a lot of memory, and the strong class
        if (h.vendor == 0x10DE) {
            const int m = h.rtx ? model_after(n, "RTX") : model_after(n, "GTX");
            const int gen = m / 100, tier = m % 100;     // 4070 -> 40, 70; 1060 -> 10, 60; 970 -> 9, 70
            if (!h.rtx && m) {
                if (gen < 10) cls = cls < 1 ? cls : 1;                  // GTX 9xx / 7xx
                else if (!(gen == 10 && tier >= 80)) cls = cls < 2 ? cls : 2;   // GTX 1050-1070, 16xx
            }
            if (h.rtx && m && tier <= 50) cls = cls < 2 ? cls : 2;      // RTX 2050 / 3050
            h.strong = h.rtx && gb >= 10.0 &&
                       (m ? ((gen >= 40 && tier >= 70) || (gen == 30 && tier >= 80)) : gb >= 16.0);  // RTX A5000 etc.
        } else if (h.vendor == 0x1002) {
            const int m = model_after(n, "RX");
            if (m >= 400 && m < 600) cls = cls < 2 ? cls : 2;           // Polaris RX 4xx / 5xx
            const int gen = m / 1000, tier = m % 1000;
            h.strong = gb >= 12.0 && (((gen == 6 || gen == 7) && tier >= 800) || (gen >= 9 && tier >= 70));
        } else if (h.vendor == 0x8086) {
            if (model_after(n, "Arc(TM) A") && model_after(n, "Arc(TM) A") < 700) cls = cls < 2 ? cls : 2;
        } else {
            h.strong = gb >= 16.0;
        }
        if (h.strong) cls = 4;
    }
    h.gpu_class = cls;
}

inline void detect_gpu(Hardware& h)
{
    HMODULE dx = LoadLibraryA("dxgi.dll");
    if (!dx) return;
    typedef HRESULT (WINAPI *CF)(REFIID, void**);
    CF cf = (CF)(void*)GetProcAddress(dx, "CreateDXGIFactory1");
    IDXGIFactory1* f = nullptr;
    if (!cf || FAILED(cf(__uuidof(IDXGIFactory1), (void**)&f)) || !f) { FreeLibrary(dx); return; }
    IDXGIFactory6* f6 = nullptr;
    if (FAILED(f->QueryInterface(__uuidof(IDXGIFactory6), (void**)&f6))) f6 = nullptr;

    // High-performance order first (on laptops: the discrete GPU before the iGPU). The D3D12 renderer takes the
    // first hardware adapter in high-performance order; the first discrete adapter, else the first hardware one.
    bool have = false, have_discrete = false;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a = nullptr;
        HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)&a)
                        : f->EnumAdapters1(i, &a);
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr) || !a) break;
        DXGI_ADAPTER_DESC1 d;
        const bool ok = SUCCEEDED(a->GetDesc1(&d));
        a->Release();
        if (!ok || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
        const double gb = d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
        const bool discrete = !is_integrated(d.VendorId, name, gb);
        // without IDXGIFactory6 the order is arbitrary: then the discrete adapter with the most memory
        const bool take = !have || (discrete && !have_discrete) || (discrete && !f6 && gb > h.vram_gb);
        if (!take) continue;
        strncpy_s(h.gpu, name, _TRUNCATE);
        h.vendor = d.VendorId; h.vram_gb = gb;
        have = true;
        have_discrete = have_discrete || discrete;
    }
    if (f6) f6->Release();
    f->Release();
    FreeLibrary(dx);
}

inline void detect_cpu(Hardware& h)
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (len) {
        BYTE* buf = (BYTE*)malloc(len);
        if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
            int cls_n[256] = {}; BYTE maxe = 0, mine = 255;
            for (DWORD off = 0; off < len;) {
                auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
                if (e->Relationship == RelationProcessorCore) {
                    h.cores++;
                    for (WORD g = 0; g < e->Processor.GroupCount; g++) {
                        KAFFINITY m = e->Processor.GroupMask[g].Mask;
                        while (m) { h.threads += (int)(m & 1); m >>= 1; }
                    }
                    const BYTE ec = e->Processor.EfficiencyClass;
                    cls_n[ec]++;
                    if (ec > maxe) maxe = ec;
                    if (ec < mine) mine = ec;
                }
                off += e->Size;
            }
            if (maxe != mine) { h.pcores = cls_n[maxe]; h.ecores = h.cores - h.pcores; }
        }
        free(buf);
    }
    if (!h.cores) { SYSTEM_INFO si; GetSystemInfo(&si); h.cores = h.threads = (int)si.dwNumberOfProcessors; }
    MEMORYSTATUSEX ms = { sizeof ms };
    if (GlobalMemoryStatusEx(&ms)) h.ram_gb = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
}

inline void detect_screen(Hardware& h)
{
    DEVMODEA dm = {}; dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        h.screen_w = (int)dm.dmPelsWidth; h.screen_h = (int)dm.dmPelsHeight;
        if (dm.dmDisplayFrequency > 1) h.refresh = (int)dm.dmDisplayFrequency;
    }
    if (h.screen_w <= 0 || h.screen_h <= 0) { h.screen_w = GetSystemMetrics(SM_CXSCREEN); h.screen_h = GetSystemMetrics(SM_CYSCREEN); }
}

inline void detect(Hardware& h)
{
    detect_gpu(h);
    detect_cpu(h);
    detect_screen(h);
    classify(h);
}

// RSX_RES_SCALE for an output height (the 720p steps the launcher offers)
inline const char* res_for_height(int sh)
{
    return sh >= 2160 ? "3" : sh >= 1800 ? "2.5" : sh >= 1440 ? "2" : sh >= 1080 ? "1.5" : "1";
}
inline int height_of_res(const char* r) { return (int)(atof(r) * 720 + 0.5); }

// The mapping. tier = T_AUTO: from the hardware; otherwise that preset on this hardware (DLSS still
// only on RTX; the screen still decides the resolution of the lower tiers).
inline Settings recommend(const Hardware& h, int tier = T_AUTO)
{
    Settings s;
    int cls = h.gpu_class;
    if (tier != T_AUTO) cls = tier == T_LOW ? 0 : tier == T_MEDIUM ? 2 : tier == T_HIGH ? 3 : 4;
    const int sh = h.screen_h > 0 ? h.screen_h : 1080;
    const bool weak_cpu = h.eff_cores <= 4.0;
    const bool very_weak_cpu = weak_cpu && h.threads <= 4;

    s.tier = tier != T_AUTO ? tier : cls <= 1 ? T_LOW : cls == 2 ? T_MEDIUM : cls == 3 ? T_HIGH : T_ULTRA;
    const char* aa = h.rtx ? "2" : "1";
    const char* screen = res_for_height(sh);
    s.vsync = "0"; s.filter = "linear"; s.sharpen = "0.25";

    switch (cls) {
    case 0:   // iGPU / < 1.5 GB: PS3 resolution, no temporal AA
        s.res = "1"; s.fsr = "0"; s.upscale = "1"; s.aniso = "4";
        break;
    case 1:   // GTX 1050 / RX 560 / 780M: 1080p output from ~720p
        s.res = sh >= 1080 ? "1.5" : "1"; s.fsr = aa; s.upscale = sh >= 1080 ? "0.667" : "1"; s.aniso = "8";
        break;
    case 2:   // GTX 1060-1660 / RX 580 / RTX 3050: the screen up to 1440p
        s.res = sh >= 1440 ? "2" : screen; s.fsr = aa; s.upscale = sh >= 1440 ? "0.667" : "1"; s.aniso = "16";
        break;
    case 3:   // RTX 2070-4060 / RX 6600-7700: the screen, upscaled above 1080p
        s.res = screen; s.fsr = aa; s.aniso = "16";
        s.upscale = sh >= 1800 ? "0.5" : sh >= 1440 ? "0.667" : "1";
        break;
    default:  // strong (RTX 4070 / 5070 and up): the scene at 1080p, DLSS Performance to 4K -- on a 1080p or
              // 1440p screen that 4K image is scaled down (supersampling); on a 4K screen it is native
        s.res = "3"; s.fsr = aa; s.aniso = "16"; s.upscale = "0.5";
        break;
    }
    if (cls == 0 && sh > 720) s.sharpen = "0.5";
    // frame rate: 60 is the tested target (game speed is the same at any limit); 30 on 4-thread CPUs
    s.fps = very_weak_cpu ? "30" : "60";
    return s;
}

// ---- text ------------------------------------------------------------------
inline void describe_hw(const Hardware& h, char* out, size_t n)
{
    char cpu[64];
    if (h.pcores && h.ecores) snprintf(cpu, sizeof cpu, "%dP+%dE cores / %d threads", h.pcores, h.ecores, h.threads);
    else snprintf(cpu, sizeof cpu, "%d cores / %d threads", h.cores, h.threads);
    char gpu[48];
    if (h.integrated) snprintf(gpu, sizeof gpu, "integrated");
    else snprintf(gpu, sizeof gpu, "%.0f GB%s", h.vram_gb, h.rtx ? ", RTX" : "");
    snprintf(out, n, "%s (%s), %s, %.0f GB RAM, %dx%d @ %d Hz", h.gpu, gpu, cpu, h.ram_gb, h.screen_w, h.screen_h, h.refresh);
}
inline const char* aa_text(const Settings& s)
{
    const double u = atof(s.upscale);
    const bool native = u >= 0.99;
    if (!strcmp(s.fsr, "2")) return native ? "DLAA" : u > 0.6 ? "DLSS Quality" : u > 0.55 ? "DLSS Balanced" : u > 0.4 ? "DLSS Performance" : "DLSS Ultra Performance";
    if (!strcmp(s.fsr, "1")) return native ? "FSR 3.1 Native AA" : u > 0.6 ? "FSR 3.1 Quality" : u > 0.55 ? "FSR 3.1 Balanced" : u > 0.4 ? "FSR 3.1 Performance" : "FSR 3.1 Ultra Performance";
    return "no AA";
}
inline void describe(const Settings& s, char* out, size_t n)
{
    const int h = height_of_res(s.res);
    const int ih = !strcmp(s.fsr, "0") ? h : (int)(h * atof(s.upscale) + 0.5);
    char af[24];
    if (!strcmp(s.aniso, "0")) snprintf(af, sizeof af, "trilinear");
    else snprintf(af, sizeof af, "%sx anisotropic", s.aniso);
    if (ih != h) snprintf(out, n, "%s: %dp -> %dp %s, %s fps, %s", tier_name(s.tier), ih, h, aa_text(s), s.fps, af);
    else snprintf(out, n, "%s: %dp, %s, %s fps, %s", tier_name(s.tier), h, aa_text(s), s.fps, af);
}

// ---- the game: RFOM_PRESET ---------------------------------------------------
// Exports the preset's settings as environment variables, only the ones that are not set already
// (explicit settings -- settings.bat, your own set lines -- always win). Must run before the renderer
// starts (main() does it first thing). Returns the number of variables set.
inline int apply_preset_env(const char* preset, char* summary = nullptr, size_t n = 0)
{
    Hardware h; detect(h);
    const int t = parse_preset(preset);
    const Settings s = recommend(h, t);
    const char* vars[][2] = {
        { "RSX_RES_SCALE", s.res }, { "RSX_FILTER", s.filter }, { "RSX_SHARPEN", s.sharpen }, { "RSX_ANISO", s.aniso },
        { "RSX_FSR", s.fsr }, { "RSX_UPSCALE", !strcmp(s.fsr, "0") ? "1" : s.upscale },
        { "RFOM_FPS", s.fps }, { "RSX_VSYNC", s.vsync },
    };
    int set = 0;
    for (auto& v : vars) {
        const char* cur = getenv(v[0]);
        if (cur && *cur) continue;
        if (!strcmp(v[0], "RFOM_FPS")) { const char* c = getenv("CHILD_RFOM_FPS"); if (c && *c) continue; }
        if (_putenv_s(v[0], v[1]) == 0) set++;
    }
    if (summary && n) {
        char hw[256], st[192];
        describe_hw(h, hw, sizeof hw);
        describe(s, st, sizeof st);
        snprintf(summary, n, "RFOM_PRESET=%s -> %s | %s | %d unset variable(s) filled", preset, st, hw, set);
    }
    return set;
}
} // namespace rfac