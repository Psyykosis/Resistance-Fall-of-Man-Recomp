// rfom_dlss.dll: the only part of the Resistance port that uses the NVIDIA DLSS (NGX) SDK.
// The game loads it at run time (RSX_FSR=2); without it, or on a non-NVIDIA GPU, it falls back to FSR.
// Same split as sspc_dlss.dll in the inFAMOUS: Second Son port, but for D3D12.
// Built by build.bat: clang-cl /MT /LD rfom_dlss.cpp tools\dlss\lib\nvsdk_ngx_s.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include <string>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

typedef void (*RfomDlssLog)(const char* msg);

namespace {
constexpr char ProjectId[] = "6b3f9c2e-5a41-4d8e-9b7a-2f0e1c7d4a93";
constexpr char EngineVersion[] = "ResistanceFoM-ps3recomp";

RfomDlssLog g_log;
ID3D12Device* g_dev;
bool g_init;
NVSDK_NGX_Parameter* g_caps;
NVSDK_NGX_Parameter* g_params;
NVSDK_NGX_Handle* g_feature;
unsigned g_w, g_h;
std::wstring g_dir;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    g_log(buf);
}
bool Ok(const char* what, NVSDK_NGX_Result r) {
    if (NVSDK_NGX_FAILED(r)) { Log("%s failed: 0x%08x", what, (unsigned)r); return false; }
    return true;
}
} // namespace

extern "C" {

// dir: folder holding nvngx_dlss.dll (and where NGX may write its logs). 1 = DLSS usable.
__declspec(dllexport) int rfom_dlss_init(ID3D12Device* dev, const wchar_t* dir, RfomDlssLog log)
{
    g_log = log;
    if (g_init) return 1;
    if (!dev) return 0;
    g_dir = dir ? dir : L".";
    static const wchar_t* paths[1];
    paths[0] = g_dir.c_str();
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    if (!Ok("NGX init", NVSDK_NGX_D3D12_Init_with_ProjectID(ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion,
                                                            g_dir.c_str(), dev, &common, NVSDK_NGX_Version_API)))
        return 0;
    g_init = true; g_dev = dev;
    if (!Ok("NGX capability query", NVSDK_NGX_D3D12_GetCapabilityParameters(&g_caps))) return 0;
    int avail = 0, drv = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &drv);
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (drv) { Log("DLSS needs a newer NVIDIA driver"); return 0; }
    if (!avail) { Log("DLSS is not available on this GPU"); return 0; }
    return 1;
}

__declspec(dllexport) void rfom_dlss_release(void)
{
    if (g_feature) { NVSDK_NGX_D3D12_ReleaseFeature(g_feature); g_feature = nullptr; }
    if (g_params) { NVSDK_NGX_D3D12_DestroyParameters(g_params); g_params = nullptr; }
}

// Records feature creation into cl. flags: 1 depth inverted, 2 HDR (+auto exposure).
// preset: NVSDK_NGX_DLSS_Hint_Render_Preset (0 = NVIDIA's default; K, transformer, for DLAA).
__declspec(dllexport) int rfom_dlss_create(ID3D12GraphicsCommandList* cl, unsigned w, unsigned h,
                                           unsigned ow, unsigned oh, int flags, unsigned preset)
{
    if (!g_init || !g_caps) return 0;
    rfom_dlss_release();
    if (!Ok("NGX parameter allocation", NVSDK_NGX_D3D12_AllocateParameters(&g_params))) return 0;
    const char* keys[] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality };
    for (const char* k : keys) NVSDK_NGX_Parameter_SetUI(g_params, k, preset);
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = w; cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
    const float r = ow ? (float)w / (float)ow : 1.0f;
    const NVSDK_NGX_PerfQuality_Value q = r > 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA
                                        : r > 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                        : r > 0.54f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                        : r > 0.42f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                    : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    static const char* qn[] = { "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA" };
    cp.Feature.InPerfQualityValue = q;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              ((flags & 1) ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
                              ((flags & 2) ? (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) : 0);
    if (!Ok("DLSS feature creation", NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &g_feature, g_params, &cp)) || !g_feature) {
        g_feature = nullptr; return 0;
    }
    g_w = w; g_h = h;
    Log("DLSS feature %ux%u -> %ux%u (%s), preset %u", w, h, ow, oh, (unsigned)q < 6 ? qn[q] : "?", preset);
    return 1;
}

// Inputs in NON_PIXEL_SHADER_RESOURCE (or a superset), output in UNORDERED_ACCESS.
// jitter: render pixels; mv * mv_scale = render pixels, current -> previous.
__declspec(dllexport) int rfom_dlss_eval(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* depth,
                                         ID3D12Resource* mv, ID3D12Resource* out, float jx, float jy,
                                         float mvsx, float mvsy, int reset, float frame_ms)
{
    if (!g_feature || !g_params) return 0;
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = out;
    ep.pInDepth = depth;
    ep.pInMotionVectors = mv;
    ep.InJitterOffsetX = jx; ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = g_w; ep.InRenderSubrectDimensions.Height = g_h;
    ep.InReset = reset;
    ep.InMVScaleX = mvsx; ep.InMVScaleY = mvsy;
    ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
    ep.InFrameTimeDeltaInMsec = frame_ms;
    return Ok("DLSS evaluation", NGX_D3D12_EVALUATE_DLSS_EXT(cl, g_feature, g_params, &ep)) ? 1 : 0;
}

__declspec(dllexport) void rfom_dlss_shutdown(void)
{
    rfom_dlss_release();
    if (g_caps) { NVSDK_NGX_D3D12_DestroyParameters(g_caps); g_caps = nullptr; }
    if (g_init) { NVSDK_NGX_D3D12_Shutdown1(g_dev); g_init = false; g_dev = nullptr; }
}

} // extern "C"
