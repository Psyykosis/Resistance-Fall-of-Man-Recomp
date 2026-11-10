/* ---------------------------------------------------------------------------
 * PC menu (included by rsx_d3d12_backend.c, runs on pc_quit_thread)
 *
 * F10 / Esc held 1 s / Start held 1.5 s open a small menu over the game:
 *   Resume / PC Options / Quit to Desktop
 * PC Options: frame rate, anti-aliasing (FSR / DLSS), mouse sensitivity,
 * field of view, button prompts. Every change is written straight to the
 * launcher's files (settings.ini, settings.bat, controls.txt). Mouse
 * sensitivity, field of view and button prompts apply at once; frame rate and
 * anti-aliasing the next time the game starts. Quit to Desktop opens the
 * usual "Quit to desktop?" prompt (pc_prompt_run).
 *
 * A small hint box ("PC menu: F10") is shown while the game's own pause menu
 * is up (a pause press during play, then the look code stops) and once
 * ~12 s after start-up.
 *
 *   RFOM_MENU_PC=0       F10 & co. open only the quit prompt (old behaviour)
 *   RFOM_MENU_PC_HINT=0  no hint box
 * -------------------------------------------------------------------------*/
extern double cellpad_mouse_sens(void);               /* cellPad.c */
extern void cellpad_set_mouse_sens(double s);
extern void cellpad_set_glyph_pref(const char* v);
extern void rfom_fov_set_live(double deg);             /* project/stubs.cpp */
extern volatile unsigned long long g_rfom_play_ms;     /* project/stubs.cpp: look code ran */

static int pm_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RFOM_MENU_PC"); v = (e && *e) ? atoi(e) != 0 : 1; }
    return v;
}

/* ---- the launcher's files ---------------------------------------------- */
#define PM_MAXL 400
#define PM_LINE 512
static int pm_read_lines(const char* path, char (*out)[PM_LINE], int max)
{
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int n = 0;
    while (n < max && fgets(out[n], PM_LINE, f)) { out[n][strcspn(out[n], "\r\n")] = 0; n++; }
    fclose(f);
    return n;
}
/* Replace the first line starting with `prefix` (case-insensitive, leading
 * blanks ignored) by `line`, drop repeats, append when missing; "" removes it.
 * Written to a temporary file first. */
static int pm_file_set(const char* path, const char* prefix, const char* line, const char* header)
{
    static char in[PM_MAXL][PM_LINE];
    const int n = pm_read_lines(path, in, PM_MAXL);
    if (n < 0 && !*line) return 1;
    char tmp[MAX_PATH]; snprintf(tmp, sizeof tmp, "%s.pcmenu.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return 0;
    int done = 0;
    for (int i = 0; i < n; i++) {
        const char* l = in[i]; while (*l == ' ' || *l == '\t') l++;
        if (!_strnicmp(l, prefix, strlen(prefix))) { if (!done && *line) fprintf(f, "%s\n", line); done = 1; continue; }
        fprintf(f, "%s\n", in[i]);
    }
    if (!done && *line) { if (n < 0 && header) fprintf(f, "%s\n", header); fprintf(f, "%s\n", line); }
    const int ok = fclose(f) == 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return 0; }
    return 1;
}
static int pm_ini_get(const char* key, char* out, int cap)
{
    static char in[PM_MAXL][PM_LINE];
    const int n = pm_read_lines("settings.ini", in, PM_MAXL);
    const size_t kl = strlen(key);
    for (int i = 0; i < n; i++)
        if (!strncmp(in[i], key, kl) && in[i][kl] == '=') { snprintf(out, (size_t)cap, "%s", in[i] + kl + 1); return 1; }
    return 0;
}
static void pm_ini_set(const char* key, const char* v)
{
    char p[96], l[256]; snprintf(p, sizeof p, "%s=", key); snprintf(l, sizeof l, "%s=%s", key, v);
    pm_file_set("settings.ini", p, l, NULL);
}
static void pm_bat_set(const char* env, const char* v)
{
    char p[96], l[256]; snprintf(p, sizeof p, "set \"%s=", env);
    if (*v) snprintf(l, sizeof l, "set \"%s=%s\"", env, v); else l[0] = 0;
    pm_file_set("settings.bat", p, l, "@echo off");
}

/* ---- the options --------------------------------------------------------- */
enum { PM_FPS, PM_FSR, PM_SENS, PM_FOV, PM_PROMPTS, PM_COUNT };
typedef struct { char label[48]; char value[24]; } PmChoice;
typedef struct { const char* title; const char* help; const char* ini; const char* env; int restart; PmChoice ch[14]; int n, sel, run; } PmOpt;
static PmOpt s_pm[PM_COUNT];
static double s_pm_sens = 1.0;
static int s_pm_init = 0;

static void pm_add(int i, const char* title, const char* help, const char* ini, const char* env, int restart,
                   const char* const* pairs, const char* running)
{
    PmOpt* o = &s_pm[i];
    o->title = title; o->help = help; o->ini = ini; o->env = env; o->restart = restart; o->n = 0;
    for (; pairs && pairs[0] && o->n < 13; pairs += 2, o->n++) {
        snprintf(o->ch[o->n].label, sizeof o->ch[o->n].label, "%s", pairs[0]);
        snprintf(o->ch[o->n].value, sizeof o->ch[o->n].value, "%s", pairs[1]);
    }
    o->run = -1;
    for (int k = 0; k < o->n; k++) if (!_stricmp(o->ch[k].value, running)) { o->run = k; break; }
    if (o->run < 0 && o->n) {                 /* a value set by hand: listed as is */
        snprintf(o->ch[o->n].label, sizeof o->ch[o->n].label, "%s", running);
        snprintf(o->ch[o->n].value, sizeof o->ch[o->n].value, "%s", running);
        o->run = o->n++;
    }
    o->sel = o->run;
    char saved[64];
    if (ini && pm_ini_get(ini, saved, sizeof saved))
        for (int k = 0; k < o->n; k++) if (!_stricmp(o->ch[k].value, saved)) { o->sel = k; break; }
}
static const char* pm_env(const char* n, const char* def) { const char* e = getenv(n); return (e && *e) ? e : def; }
static void pm_opts_init(void)
{
    if (s_pm_init) return;
    s_pm_init = 1;
    static const char* const fps[] = { "30  (PS3 original)", "30", "55", "55", "60", "60", "72", "72", "82", "82", "90", "90",
                                       "120", "120", "144", "144", "165", "165", "Unlimited", "0", NULL };
    const char* rf = getenv("CHILD_RFOM_FPS"); if (!rf || !*rf) rf = pm_env("RFOM_FPS", "60");
    pm_add(PM_FPS, "Frame rate limit", "The game runs at the same speed at any limit. 60 is the most tested.",
           "fps", "CHILD_RFOM_FPS", 1, fps, rf);
    static const char* const fsr[] = { "Off  (PS3 original)", "0", "AMD FSR 3.1", "1", "NVIDIA DLSS / DLAA", "2", NULL };
    pm_add(PM_FSR, "Anti-aliasing", "DLSS needs a GeForce RTX (falls back to FSR). Upscaling preset: launcher.",
           "fsr", "RSX_FSR", 1, fsr, pm_env("RSX_FSR", "0"));
    s_pm_sens = cellpad_mouse_sens();
    {   /* controls.txt is the truth (the pad code may not have loaded it yet) */
        static char cl[PM_MAXL][PM_LINE];
        const int n = pm_read_lines("controls.txt", cl, PM_MAXL);
        for (int i = 0; i < n; i++) {
            const char* l = cl[i]; while (*l == ' ' || *l == '\t') l++;
            if (_strnicmp(l, "mouse_sensitivity", 17)) continue;
            const char* eq = strchr(l, '=');
            const double s = eq ? atof(eq + 1) : 0.0;
            if (s >= 0.1 && s <= 4.0) s_pm_sens = s;
            break;
        }
    }
    pm_add(PM_SENS, "Mouse sensitivity", "Also multiplied by the game's own Look Sensitivity option.", NULL, NULL, 0, NULL, "");
    static const char* const fov[] = { "82  (PS3 original)", "82", "90", "90", "100", "100", "110", "110", "120", "120", NULL };
    char fv[16]; snprintf(fv, sizeof fv, "%d", atoi(pm_env("RFOM_FOV", "82")) ? atoi(pm_env("RFOM_FOV", "82")) : 82);
    pm_add(PM_FOV, "Field of view", "Horizontal degrees. Zoomed views keep their zoom.", "fov", "RFOM_FOV", 0, fov, fv);
    static const char* const gl[] = { "Auto", "", "Xbox", "xbox", "PlayStation", "ps", NULL };
    pm_add(PM_PROMPTS, "Button prompts", "Which buttons the game shows with a controller (keyboard shows keys).",
           "padglyphs", "RFOM_PAD_GLYPHS", 0, gl, pm_env("RFOM_PAD_GLYPHS", ""));
}
static void pm_apply(int i)
{
    if (i == PM_SENS) {
        cellpad_set_mouse_sens(s_pm_sens);
        char l[64]; snprintf(l, sizeof l, "mouse_sensitivity = %.2f", s_pm_sens);
        pm_file_set("controls.txt", "mouse_sensitivity", l, NULL);
        fprintf(stderr, "[pcmenu] mouse sensitivity %.2f%c", s_pm_sens, 10);
        return;
    }
    const PmOpt* o = &s_pm[i];
    const char* v = o->ch[o->sel].value;
    if (o->ini) pm_ini_set(o->ini, v);
    if (o->env) pm_bat_set(o->env, v);
    if (i == PM_FOV) rfom_fov_set_live(atof(v));
    if (i == PM_PROMPTS) cellpad_set_glyph_pref(v);
    fprintf(stderr, "[pcmenu] %s = %s%s%c", o->title, o->ch[o->sel].label, o->restart ? " (next start)" : "", 10);
}
static void pm_step(int i, int d)
{
    if (i == PM_SENS) {
        double s = floor(s_pm_sens * 20.0 + 0.5) / 20.0 + d * 0.05;
        if (s < 0.1) s = 0.1;
        if (s > 4.0) s = 4.0;
        if (fabs(s - s_pm_sens) < 1e-9) return;
        s_pm_sens = s;
    } else {
        PmOpt* o = &s_pm[i];
        o->sel = (o->sel + d + o->n) % o->n;
    }
    pm_apply(i);
}

/* ---- the window ---------------------------------------------------------- */
static int s_pm_page = 0, s_pm_sel = 0;
static volatile int s_pm_click = 0;                  /* from the window: +1 left, -1 right */
static int s_pm_row_top = 0, s_pm_row_h = 1;
static POINT s_pm_mouse0; static volatile int s_pm_mouse_armed = 0;   /* cursor at open; moved since */
static void pm_mouse_reset(void) { GetCursorPos(&s_pm_mouse0); s_pm_mouse_armed = 0; }
static int pm_rows(void) { return s_pm_page == 0 ? 3 : PM_COUNT + 1; }
static const char* pm_label(int r)
{
    static const char* main_rows[] = { "Resume", "PC Options", "Quit to Desktop" };
    return s_pm_page == 0 ? main_rows[r] : r < PM_COUNT ? s_pm[r].title : "Back";
}
static void pm_value(int r, char* b, int cap)
{
    b[0] = 0;
    if (s_pm_page == 0 || r >= PM_COUNT) return;
    if (r == PM_SENS) { snprintf(b, (size_t)cap, "<   %.2fx   >", s_pm_sens); return; }
    const PmOpt* o = &s_pm[r];
    snprintf(b, (size_t)cap, "<   %s%s   >", o->ch[o->sel].label, o->restart && o->sel != o->run ? " *" : "");
}
static void pm_help(char* b, int cap)
{
    static const char* h[] = { "Back to the game.", "Frame rate, anti-aliasing, mouse, field of view, button prompts.",
                               "Waits for any save in progress, then closes the game." };
    if (s_pm_page == 0) { snprintf(b, (size_t)cap, "%s", h[s_pm_sel]); return; }
    if (s_pm_sel >= PM_COUNT) { snprintf(b, (size_t)cap, "Back to the PC menu."); return; }
    const PmOpt* o = &s_pm[s_pm_sel];
    snprintf(b, (size_t)cap, "%s%s", o->help, !o->restart ? "" : o->sel != o->run ? "  * Applies the next time the game starts." :
                                                               "  (Applies the next time the game starts.)");
}
static void pm_paint(HDC dc, RECT rc)
{
    const int H = rc.bottom - rc.top, W = rc.right - rc.left, n = pm_rows();
    const int rh = s_pm_row_h = H / (n + 4);
    const int top = s_pm_row_top = rc.top + rh * 3 / 2;
    HFONT ft = CreateFontA(-(rh * 62 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift SemiBold");
    HFONT fr = CreateFontA(-(rh * 48 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift");
    HFONT fs = CreateFontA(-(rh * 34 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift");
    HGDIOBJ of = SelectObject(dc, ft);
    SetTextColor(dc, RGB(240, 240, 240));
    RECT r = { rc.left + rh / 2, rc.top + rh / 3, rc.right - rh / 2, top };
    DrawTextA(dc, s_pm_page == 0 ? "PC MENU" : "PC OPTIONS", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, fr);
    const int vx = rc.left + W * 46 / 100;
    for (int i = 0; i < n; i++) {
        RECT rr = { rc.left + 6, top + i * rh, rc.right - 6, top + (i + 1) * rh };
        if (i == s_pm_sel) { HBRUSH b = CreateSolidBrush(RGB(150, 32, 24)); FillRect(dc, &rr, b); DeleteObject(b); }
        SetTextColor(dc, i == s_pm_sel ? RGB(255, 255, 255) : RGB(200, 200, 200));
        RECT rl = { rr.left + rh / 2, rr.top, vx, rr.bottom };
        DrawTextA(dc, pm_label(i), -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        char v[96]; pm_value(i, v, sizeof v);
        if (v[0]) { RECT rv = { vx, rr.top, rr.right - rh / 2, rr.bottom }; DrawTextA(dc, v, -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); }
    }
    SelectObject(dc, fs);
    char h[256]; pm_help(h, sizeof h);
    SetTextColor(dc, RGB(185, 185, 185));
    RECT r1 = { rc.left + rh / 2, top + n * rh + rh / 6, rc.right - rh / 2, top + n * rh + rh * 7 / 6 };
    DrawTextA(dc, h, -1, &r1, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    SetTextColor(dc, RGB(140, 140, 140));
    RECT r2 = { rc.left + rh / 2, top + n * rh + rh * 7 / 6, rc.right - rh / 2, rc.bottom - rh / 6 };
    DrawTextA(dc, "Up/Down select    Left/Right change    Enter / A  OK    Esc / B  back    F10  close", -1, &r2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of); DeleteObject(ft); DeleteObject(fr); DeleteObject(fs);
}
static LRESULT CALLBACK pm_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        const int y = (short)HIWORD(lp);
        const int row = y >= s_pm_row_top ? (y - s_pm_row_top) / s_pm_row_h : -1;
        /* The menu opens centred, right under the mouse-look cursor (the
         * window centre is on "Quit to Desktop"): the mouse only takes over
         * once it has really moved, and an unmoved click only selects. */
        const int was_armed = s_pm_mouse_armed;
        if (!was_armed) {
            POINT p; GetCursorPos(&p);
            if (msg != WM_MOUSEMOVE || abs(p.x - s_pm_mouse0.x) + abs(p.y - s_pm_mouse0.y) >= 8) s_pm_mouse_armed = 1;
            if (!s_pm_mouse_armed) return 0;
        }
        if (row >= 0 && row < pm_rows()) {
            if (row != s_pm_sel) { s_pm_sel = row; InvalidateRect(hwnd, NULL, FALSE); }
            if (!was_armed) return 0;
            if (msg == WM_LBUTTONDOWN) s_pm_click = 1;
            if (msg == WM_RBUTTONDOWN) s_pm_click = -1;
        }
        return 0; }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        HGDIOBJ obm = SelectObject(mdc, bm);
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 20)); FillRect(mdc, &rc, bg); DeleteObject(bg);
        const int hint = GetWindowLongPtrA(hwnd, GWLP_USERDATA) != 0;
        HBRUSH fr = CreateSolidBrush(hint ? RGB(90, 90, 100) : RGB(170, 40, 30));
        FrameRect(mdc, &rc, fr);
        RECT r2 = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 }; FrameRect(mdc, &r2, fr); DeleteObject(fr);
        SetBkMode(mdc, TRANSPARENT);
        if (!hint) pm_paint(mdc, rc);
        else {
            const int H = rc.bottom - rc.top;
            HFONT f1 = CreateFontA(-(H * 30 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift SemiBold");
            HFONT f2 = CreateFontA(-(H * 20 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0, ANTIALIASED_QUALITY, 0, "Bahnschrift");
            HGDIOBJ of = SelectObject(mdc, f1);
            SetTextColor(mdc, RGB(240, 240, 240));
            RECT t1 = { rc.left, rc.top + H * 10 / 100, rc.right, rc.top + H * 55 / 100 };
            DrawTextA(mdc, "PC menu:  F10   or hold Esc / Start", -1, &t1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mdc, f2);
            SetTextColor(mdc, RGB(190, 190, 190));
            RECT t2 = { rc.left, rc.top + H * 55 / 100, rc.right, rc.top + H * 90 / 100 };
            DrawTextA(mdc, "PC Options  -  Quit to Desktop", -1, &t2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mdc, of); DeleteObject(f1); DeleteObject(f2);
        }
        BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, obm); DeleteObject(bm); DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0; }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
static HWND pm_window(int hint)
{
    static int reg = 0;
    if (!reg) { reg = 1; WNDCLASSEXA wc = {0}; wc.cbSize = sizeof wc; wc.lpfnWndProc = pm_wndproc;
                wc.hInstance = GetModuleHandle(NULL); wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "rfom_pc_menu";
                RegisterClassExA(&wc); }
    HWND w = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "rfom_pc_menu", "PC menu", WS_POPUP,
                             0, 0, 10, 10, s_d3d.hwnd, NULL, GetModuleHandle(NULL), NULL);
    if (w) SetWindowLongPtrA(w, GWLP_USERDATA, hint);
    return w;
}
static void pm_place(HWND w, int hint)
{
    RECT gr; if (!GetWindowRect(s_d3d.hwnd, &gr)) return;
    const int gw = gr.right - gr.left, gh = gr.bottom - gr.top;
    int ww, hh, x, y;
    if (hint) {
        ww = gw * 30 / 100; if (ww < 380) ww = 380;
        hh = gh * 7 / 100; if (hh < 54) hh = 54;
        x = gr.left + (gw - ww) / 2; y = gr.top + gh - hh - gh * 4 / 100;
    } else {
        int rh = gh * 55 / 1000; if (rh < 34) rh = 34;
        ww = gw * 44 / 100; if (ww < 560) ww = 560; if (ww > gw) ww = gw;
        hh = rh * (pm_rows() + 4); if (hh > gh) hh = gh;
        x = gr.left + (gw - ww) / 2; y = gr.top + (gh - hh) / 2;
    }
    SetWindowPos(w, HWND_TOPMOST, x, y, ww, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(w, NULL, FALSE); UpdateWindow(w);
}

/* ---- input --------------------------------------------------------------- */
enum { PM_YES = 1, PM_NO = 2, PM_CLOSE = 4, PM_UP = 8, PM_DOWN = 16, PM_LEFT = 32, PM_RIGHT = 64 };
#define PM_KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
static int pm_input(void)
{
    HWND fgw = GetForegroundWindow();
    DWORD pid = 0; if (fgw) GetWindowThreadProcessId(fgw, &pid);
    const int fg = pid == GetCurrentProcessId();
    int r = 0;
    if (fg) {
        if (PM_KD(VK_RETURN) || PM_KD(VK_SPACE)) r |= PM_YES;
        if (PM_KD(VK_ESCAPE) || PM_KD(VK_BACK)) r |= PM_NO;
        if (PM_KD(VK_F10)) r |= PM_CLOSE;
        if (PM_KD(VK_UP) || PM_KD('W')) r |= PM_UP;
        if (PM_KD(VK_DOWN) || PM_KD('S')) r |= PM_DOWN;
        if (PM_KD(VK_LEFT) || PM_KD('A')) r |= PM_LEFT;
        if (PM_KD(VK_RIGHT) || PM_KD('D')) r |= PM_RIGHT;
    }
    XINPUT_STATE st;
    for (DWORD i = 0; i < 4; i++) {
        if (XInputGetState(i, &st) != ERROR_SUCCESS) break;
        const WORD b = st.Gamepad.wButtons;
        if (b & XINPUT_GAMEPAD_A) r |= PM_YES;
        if (b & XINPUT_GAMEPAD_B) r |= PM_NO;
        if (b & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) r |= PM_CLOSE;
        if ((b & XINPUT_GAMEPAD_DPAD_UP) || st.Gamepad.sThumbLY > 20000) r |= PM_UP;
        if ((b & XINPUT_GAMEPAD_DPAD_DOWN) || st.Gamepad.sThumbLY < -20000) r |= PM_DOWN;
        if ((b & XINPUT_GAMEPAD_DPAD_LEFT) || st.Gamepad.sThumbLX < -20000) r |= PM_LEFT;
        if ((b & XINPUT_GAMEPAD_DPAD_RIGHT) || st.Gamepad.sThumbLX > 20000) r |= PM_RIGHT;
    }
    return r;
}
static int pm_edges(int in, int* prev)
{
    static ULONGLONG rep_at = 0;
    const ULONGLONG now = GetTickCount64();
    int e = in & ~*prev;
    const int dirs = in & (PM_UP | PM_DOWN | PM_LEFT | PM_RIGHT);
    if (e & dirs) rep_at = now + 400;
    else if (dirs && rep_at && now >= rep_at) { e |= dirs; rep_at = now + 110; }
    if (!dirs) rep_at = 0;
    *prev = in;
    return e;
}
static void pm_pump(void) { MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); } }
/* let go of every menu key before the game sees input again (3 s at most) */
static void pm_wait_release(void)
{
    const ULONGLONG t0 = GetTickCount64();
    while (pm_input() && GetTickCount64() - t0 < 3000) { pm_pump(); Sleep(15); }
}

/* the menu; 1 = quit to desktop */
static int pm_run(void)
{
    pm_opts_init();
    HWND w = pm_window(0);
    if (!w) return pc_prompt_run();
    s_pm_page = 0; s_pm_sel = 0; s_pm_click = 0;
    pm_mouse_reset();
    pm_place(w, 0);
    ClipCursor(NULL);
    fprintf(stderr, "[pcmenu] shown%c", 10);
    int armed = 0, prev = 0, result = -1;
    while (result < 0) {
        pm_pump();
        Sleep(15);
        if (s_quit_req == 2) { result = 1; break; }
        const int in = pm_input();
        if (!armed) { if (!(in & (PM_YES | PM_NO | PM_CLOSE))) { armed = 1; prev = in; } continue; }   /* what opened it is let go first */
        const int e = pm_edges(in, &prev);
        int act = 0;                                 /* 1 = OK / next, -1 = previous */
        const int click = s_pm_click; s_pm_click = 0;
        const int n = pm_rows();
        if (click) act = click;
        else if (e & PM_UP) { s_pm_sel = (s_pm_sel + n - 1) % n; InvalidateRect(w, NULL, FALSE); }
        else if (e & PM_DOWN) { s_pm_sel = (s_pm_sel + 1) % n; InvalidateRect(w, NULL, FALSE); }
        else if (e & (PM_LEFT | PM_RIGHT)) { if (s_pm_page == 1 && s_pm_sel < PM_COUNT) act = (e & PM_LEFT) ? -1 : 1; }
        else if (e & PM_YES) act = 1;
        else if (e & PM_CLOSE) result = 0;
        else if (e & PM_NO) { if (s_pm_page == 1) { s_pm_page = 0; s_pm_sel = 1; pm_place(w, 0); } else result = 0; }
        if (!act) continue;
        if (s_pm_page == 0) {
            if (act < 0) continue;
            if (s_pm_sel == 0) result = 0;
            else if (s_pm_sel == 1) { s_pm_page = 1; s_pm_sel = 0; pm_place(w, 0); }
            else {
                ShowWindow(w, SW_HIDE);
                if (pc_prompt_run()) result = 1;
                else { pm_mouse_reset(); pm_place(w, 0); armed = 0; }  /* No: back to the menu */
            }
        } else if (s_pm_sel >= PM_COUNT) {
            if (act > 0) { s_pm_page = 0; s_pm_sel = 1; pm_place(w, 0); }
        } else { pm_step(s_pm_sel, act); InvalidateRect(w, NULL, FALSE); }
    }
    DestroyWindow(w);
    if (!result) { pm_wait_release(); fprintf(stderr, "[pcmenu] closed%c", 10); }
    return result;
}

/* the hint box; called from pc_quit_thread every ~30 ms while no prompt is up */
static HWND s_pm_hint = NULL;
static int s_pm_hint_shown = 0;
static void pm_hint_hide(void) { if (s_pm_hint_shown) { ShowWindow(s_pm_hint, SW_HIDE); s_pm_hint_shown = 0; } }
static void pm_hint_tick(int fg)
{
    static int on = -1;
    static ULONGLONG t_first = 0, pause_t = 0;
    static int was_d = 0;
    if (on < 0) {
        const char* e = getenv("RFOM_MENU_PC_HINT");
        on = pm_enabled() && (e && *e ? atoi(e) != 0 : 1);
    }
    if (!on || !s_d3d.hwnd) return;
    const ULONGLONG now = GetTickCount64();
    if (!t_first) t_first = now;
    const ULONGLONG look = g_rfom_play_ms;
    const int playing = look && now - look < 200;
    const int d = (fg && ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000))) ||
                  (pc_xinput_buttons() & XINPUT_GAMEPAD_START);
    if (d && !was_d && playing) pause_t = now;
    was_d = d;
    if (pause_t && ((playing && now - pause_t > 600) || now - pause_t > 120000)) pause_t = 0;
    const int want = fg && !s_quit_prompt && !rsx_focus_paused &&
                     ((pause_t && !playing && now - pause_t > 250) || (!look && now - t_first > 12000 && now - t_first < 22000));
    if (want && !s_pm_hint_shown) {
        if (!s_pm_hint) s_pm_hint = pm_window(1);
        if (s_pm_hint) { pm_place(s_pm_hint, 1); s_pm_hint_shown = 1; }
    } else if (!want) pm_hint_hide();
    if (s_pm_hint) pm_pump();
}
#undef PM_KD
