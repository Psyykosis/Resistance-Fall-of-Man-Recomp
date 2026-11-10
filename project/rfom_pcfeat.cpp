// Resistance: Fall of Man PC port: performance overlay (F3, RFOM_OVERLAY) and toast
// notifications (screenshot saved). Both are small topmost layered Win32 windows drawn with GDI
// on their own thread, placed over the game window's client area and hidden while the game is
// not the foreground window. They never take focus or input (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT).
//
// RFOM_OVERLAY   unset: off, F3 toggles | 1: on at start, F3 toggles | 0: off and F3 does nothing
// RFOM_OVERLAY_POS  tl (default) | tr | bl | br  corner of the game window
//
// Lines: game fps and frame time (avg / worst over the last 0.5 s, from the game's frame
// limiter, stubs.cpp rfom_frame_wait), RSX frames per second, draws per frame and the average
// CPU time per draw (the D3D12 backend's RSX_DRAWPROF counters, switched on only while the
// overlay is shown; with RSX_DRAWPROF=1 the window is the log's 5 s one).
//
// Exports: rfpf_overlay_get / rfpf_overlay_set, rfpf_toast(title, text, badge).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>

extern "C" void* g_kbm_hwnd;                                                        // cellPad.c
extern "C" void rsx_prof_enable(int on);                                            // rsx_d3d12_backend.c
extern "C" int rsx_prof_sample(double* fps, double* draws_per_frame, double* us_per_draw);

namespace {

std::atomic<int> s_overlay{0};
std::atomic<int> s_overlay_allowed{1};
std::atomic<int> s_thread_started{0};

// Game frame times (QPC ticks between rfom_frame_wait calls)
std::mutex s_ft_mtx;
double s_ft_sum = 0, s_ft_max = 0; int s_ft_n = 0;
LARGE_INTEGER s_qpf;

struct Toast { std::wstring title, text; char badge; };
std::mutex s_toast_mtx;
std::deque<Toast> s_toasts;

std::wstring widen(const char* s)
{
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

HWND game_hwnd() { return (HWND)g_kbm_hwnd; }

bool game_visible(RECT& client_screen)
{
    HWND g = game_hwnd();
    if (!g || !IsWindow(g) || IsIconic(g) || GetForegroundWindow() != g) return false;
    RECT rc;
    if (!GetClientRect(g, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
    ClientToScreen(g, &a); ClientToScreen(g, &b);
    client_screen = { a.x, a.y, b.x, b.y };
    return true;
}

// ---- overlay ----
HWND s_ov = nullptr, s_tw = nullptr;
HFONT s_font = nullptr, s_font_b = nullptr, s_font_badge = nullptr;
wchar_t s_ov_text[512] = L"";
int s_ov_w = 0, s_ov_h = 0;

void ov_measure()
{
    HDC dc = GetDC(s_ov);
    HGDIOBJ of = SelectObject(dc, s_font);
    RECT r{ 0, 0, 1000, 1000 };
    DrawTextW(dc, s_ov_text, -1, &r, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(s_ov, dc);
    s_ov_w = r.right + 16; s_ov_h = r.bottom + 10;
}

LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(150, 255, 150));
        HGDIOBJ of = SelectObject(dc, s_font);
        RECT t{ 8, 5, rc.right - 4, rc.bottom - 2 };
        DrawTextW(dc, s_ov_text, -1, &t, DT_NOPREFIX);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

// ---- toast ----
Toast s_cur; bool s_cur_on = false; ULONGLONG s_cur_t0 = 0, s_prev_now = 0;
const int TW = 360, TH = 72;

COLORREF badge_color(char b)
{
    switch (b) {
    case 'P': return RGB(170, 200, 235);
    case 'G': return RGB(230, 185, 60);
    case 'S': return RGB(190, 195, 205);
    case 'B': return RGB(200, 125, 70);
    default:  return RGB(90, 160, 230);
    }
}

LRESULT CALLBACK toast_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        const COLORREF bc = badge_color(s_cur.badge);
        HBRUSH bar = CreateSolidBrush(bc);
        RECT strip{ 0, 0, 4, rc.bottom }; FillRect(dc, &strip, bar);
        // badge: a filled circle with the grade letter
        HGDIOBJ ob = SelectObject(dc, bar);
        HPEN pen = CreatePen(PS_SOLID, 1, bc); HGDIOBJ op = SelectObject(dc, pen);
        const int cy = rc.bottom / 2;
        Ellipse(dc, 16, cy - 20, 56, cy + 20);
        SelectObject(dc, op); DeleteObject(pen);
        SelectObject(dc, ob); DeleteObject(bar);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, s_font_badge);
        SetTextColor(dc, RGB(20, 22, 28));
        wchar_t letter[2] = { (wchar_t)(s_cur.badge ? s_cur.badge : L'i'), 0 };
        RECT br{ 16, cy - 20, 56, cy + 20 };
        DrawTextW(dc, letter, 1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, s_font_b);
        SetTextColor(dc, RGB(235, 235, 240));
        RECT t1{ 68, 10, rc.right - 10, 32 };
        DrawTextW(dc, s_cur.title.c_str(), -1, &t1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, s_font);
        SetTextColor(dc, RGB(175, 180, 190));
        RECT t2{ 68, 34, rc.right - 10, rc.bottom - 6 };
        DrawTextW(dc, s_cur.text.c_str(), -1, &t2, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

HWND make_layer(const wchar_t* cls, WNDPROC proc)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                             cls, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    if (h) SetLayeredWindowAttributes(h, 0, 220, LWA_ALPHA);
    return h;
}

void place(HWND h, int x, int y, int w, int hh)
{
    SetWindowPos(h, HWND_TOPMOST, x, y, w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void hide(HWND h) { if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE); }

void update_overlay_text(bool fresh)
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!fresh && now - last < 500) return;
    const double dt = last ? (now - last) / 1000.0 : 0.5;
    last = now;
    double sum, mx; int n;
    {
        std::lock_guard<std::mutex> lk(s_ft_mtx);
        sum = s_ft_sum; mx = s_ft_max; n = s_ft_n;
        s_ft_sum = s_ft_max = 0; s_ft_n = 0;
    }
    std::wstring t;
    wchar_t b[160];
    if (n > 0) {
        const double avg = sum / n;
        swprintf(b, 160, L"Game  %5.1f fps   %5.2f ms (max %5.2f)", avg > 0 ? 1000.0 / avg : 0.0, avg, mx);
    } else swprintf(b, 160, L"Game  --");
    t += b;
    double fr = 0, dpf = 0, upd = 0;
    (void)dt;
    if (rsx_prof_sample(&fr, &dpf, &upd)) {
        swprintf(b, 160, L"\nRSX   %5.1f fps   %4.0f draws/frame   %5.2f us/draw", fr, dpf, upd);
        t += b;
    }
    wcsncpy(s_ov_text, t.c_str(), 511);
    s_ov_text[511] = 0;
}

void ui_thread()
{
    QueryPerformanceFrequency(&s_qpf);
    while (!g_kbm_hwnd) Sleep(250);
    s_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    s_font_b = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_font_badge = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_ov = make_layer(L"RFOMPortPerfOverlay", ov_proc);
    s_tw = make_layer(L"RFOMPortToast", toast_proc);
    const char* pos = getenv("RFOM_OVERLAY_POS");
    const bool right = pos && (pos[1] == 'r' || pos[1] == 'R'), bottom = pos && (pos[0] == 'b' || pos[0] == 'B');
    int was_f3 = 0, prof_on = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        RECT cs{};
        const bool vis = game_visible(cs);
        // F3 toggles (only while the game has focus)
        const int f3 = vis && s_overlay_allowed && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !was_f3) s_overlay = !s_overlay;
        was_f3 = f3;
        if (s_overlay != prof_on) {
            prof_on = s_overlay;
            rsx_prof_enable(prof_on);
            if (prof_on) update_overlay_text(true);
        }
        if (vis && s_overlay && s_ov) {
            const ULONGLONG before = GetTickCount64();
            static ULONGLONG painted = 0;
            if (before - painted >= 500) { update_overlay_text(true); ov_measure(); InvalidateRect(s_ov, nullptr, FALSE); painted = before; }
            const int x = right ? cs.right - s_ov_w - 8 : cs.left + 8, y = bottom ? cs.bottom - s_ov_h - 8 : cs.top + 8;
            place(s_ov, x, y, s_ov_w, s_ov_h);
        } else if (s_ov) hide(s_ov);

        // toasts: 5 s each, the last 0.6 s fading out; queued while the game is in the background
        if (s_tw) {
            const ULONGLONG now = GetTickCount64();
            if (s_cur_on && now - s_cur_t0 > 5000) { s_cur_on = false; hide(s_tw); }
            if (!s_cur_on && vis) {
                std::lock_guard<std::mutex> lk(s_toast_mtx);
                if (!s_toasts.empty()) { s_cur = s_toasts.front(); s_toasts.pop_front(); s_cur_on = true; s_cur_t0 = now; InvalidateRect(s_tw, nullptr, TRUE); }
            }
            if (s_cur_on && vis) {
                const ULONGLONG age = now - s_cur_t0;
                const int a = age > 4400 ? (int)(230 * (5000 - std::min<ULONGLONG>(age, 5000)) / 600) : age < 200 ? (int)(230 * age / 200) : 230;
                SetLayeredWindowAttributes(s_tw, 0, (BYTE)std::max(a, 0), LWA_ALPHA);
                const int x = right ? cs.left + 16 : cs.right - TW - 16;   // opposite corner to the overlay
                place(s_tw, x, cs.top + 16, TW, TH);
            } else if (!vis && s_cur_on) { s_cur_t0 += now - s_prev_now; hide(s_tw); }   // paused in the background
            s_prev_now = now;
        }
        Sleep(s_overlay || s_cur_on ? 33 : 100);
    }
}

void start_thread()
{
    if (s_thread_started.exchange(1)) return;
    std::thread(ui_thread).detach();
}

struct PcFeatInit {
    PcFeatInit()
    {
        const char* e = getenv("RFOM_OVERLAY");
        if (e && *e == '0') s_overlay_allowed = 0;
        else if (e && *e == '1') s_overlay = 1;
        start_thread();
    }
} s_init;

} // namespace

// Called once per game frame (stubs.cpp rfom_frame_wait).
extern "C" void rfpf_frame_tick(void)
{
    static LARGE_INTEGER last{};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart && s_qpf.QuadPart && s_overlay) {
        const double ms = (now.QuadPart - last.QuadPart) * 1000.0 / s_qpf.QuadPart;
        if (ms < 1000.0) {
            std::lock_guard<std::mutex> lk(s_ft_mtx);
            s_ft_sum += ms; s_ft_n++; if (ms > s_ft_max) s_ft_max = ms;
        }
    }
    last = now;
}

extern "C" int rfpf_overlay_get(void) { return s_overlay; }
extern "C" void rfpf_overlay_set(int on) { if (s_overlay_allowed) s_overlay = on ? 1 : 0; }

// A toast at the game window's top-right corner for 5 s. title/text are UTF-8; badge is a
// letter in a coloured circle ('P' 'G' 'S' 'B' have their own colours), 0 = a plain 'i' notice.
extern "C" void rfpf_toast(const char* title, const char* text, char badge)
{
    std::lock_guard<std::mutex> lk(s_toast_mtx);
    if (s_toasts.size() < 32) s_toasts.push_back({ widen(title), widen(text), badge });
}
