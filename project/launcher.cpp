// Resistance: Fall of Man -- PC port launcher.
//
// A settings window in front of run.bat. It reads/writes settings.ini next to
// itself and turns the choices into settings.bat (plain `set` lines), which
// run.bat calls after its own defaults -- so run.bat stays the one place the
// game is started from, with or without the launcher.
//
// Build (build.bat does this):
//   clang-cl /nologo /O2 /EHsc project\launcher.cpp /Fo:build\ /Fe:Launcher.exe
//            /link /MANIFEST:EMBED user32.lib gdi32.lib shell32.lib comctl32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <ctype.h>
#include <stdlib.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

struct Choice { const char* label; const char* value; };
struct Setting {
    const char* key;      // settings.ini key
    const char* title;    // label in the window
    const char* hint;     // one line under the box
    std::vector<Choice> choices;
    int def;              // default choice index
    HWND combo;
    int sel;
};

static std::vector<Setting> g_set = {
    { "display", "Display mode",
      "Alt+Enter switches in game too.",
      { {"Windowed", "windowed"}, {"Borderless fullscreen", "borderless"} }, 1 },
    { "res", "Render resolution",
      "What the game actually renders at. Higher is sharper (and sharper shadows) but costs GPU time.",
      { {"1280 x 720 (PS3 original)", "1"}, {"1920 x 1080", "1.5"}, {"2560 x 1440", "2"},
        {"3200 x 1800", "2.5"}, {"3840 x 2160", "3"} }, 2 },
    { "window", "Window size (windowed mode)",
      "Only used in windowed mode. The rendered picture is scaled to fit the window.",
      { {"1280 x 720", "1280x720"}, {"1600 x 900", "1600x900"}, {"1920 x 1080", "1920x1080"},
        {"2560 x 1440", "2560x1440"}, {"3840 x 2160", "3840x2160"} }, 2 },
    { "fps", "Frame rate limit",
      "Above 60 is experimental: game logic runs faster than it was ever tested at.",
      { {"30 (original PS3 rate)", "30"}, {"55 (even pacing on 165 Hz)", "55"}, {"60", "60"},
        {"72", "72"}, {"82 (even pacing on 165 Hz)", "82"}, {"90", "90"}, {"120", "120"},
        {"144", "144"}, {"165", "165"}, {"Unlimited", "0"} }, 2 },
    { "vsync", "VSync",
      "Off + FreeSync/G-Sync is smoothest if your monitor has it. Every 3rd refresh = 55 fps on 165 Hz.",
      { {"Off", "0"}, {"Every refresh", "1"}, {"Every 2nd refresh", "2"}, {"Every 3rd refresh", "3"} }, 0 },
    { "aniso", "Texture filtering",
      "Anisotropic filtering keeps floors and walls sharp at an angle. Very cheap on modern GPUs.",
      { {"Trilinear (original)", "0"}, {"Anisotropic 2x", "2"}, {"Anisotropic 4x", "4"},
        {"Anisotropic 8x", "8"}, {"Anisotropic 16x", "16"} }, 4 },
    { "filter", "Upscaling",
      "How the rendered picture is scaled to your screen when the sizes differ.",
      { {"Smooth (bilinear)", "linear"}, {"Sharp pixels (nearest)", "point"} }, 0 },
    { "sharpen", "Sharpening",
      "Counteracts the softness of upscaling. Leave Off when render resolution matches your screen.",
      { {"Off", "0"}, {"Low", "0.25"}, {"Medium", "0.5"}, {"High", "0.8"} }, 1 },
    { "fsr", "Anti-aliasing",
      "AMD FSR 3.1 smooths jagged edges and shimmer (works on any GPU). Experimental.",
      { {"Off (original)", "0"}, {"AMD FSR 3.1 (native AA)", "1"} }, 0 },
};

static char g_dir[MAX_PATH];
static const int FOV_MIN = 60, FOV_MAX = 150, FOV_DEF = 82;   // 82 = the game's own view
static int  g_fov = FOV_DEF;
static HWND g_fov_tb, g_fov_lbl;
static HFONT g_font, g_bold, g_small;
// v153: dark theme
static const COLORREF C_BG = RGB(32, 32, 32), C_CTL = RGB(48, 48, 48), C_TEXT = RGB(235, 235, 235), C_DIM = RGB(160, 160, 160);
static HBRUSH g_bg_brush, g_ctl_brush;
static void dark_titlebar(HWND hw)
{
    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(hw, 20, &on, sizeof on)))   // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(hw, 19, &on, sizeof on);            // older Windows 10 builds
}
static void dark_app_mode()
{
    // uxtheme's (unnamed) SetPreferredAppMode: dark scrollbars and drop-down lists
    if (HMODULE ux = LoadLibraryExA("uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        typedef int (WINAPI *SetMode)(int);
        if (SetMode f = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135))) f(2);   // ForceDark
    }
}
static LRESULT dark_ctlcolor(UINT msg, WPARAM wp, LPARAM lp)
{
    HDC dc = (HDC)wp;
    if (msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT) {
        SetTextColor(dc, C_TEXT); SetBkColor(dc, C_CTL); return (LRESULT)g_ctl_brush; }
    const bool dim = (HFONT)SendMessageA((HWND)lp, WM_GETFONT, 0, 0) == g_small;
    SetTextColor(dc, dim ? C_DIM : C_TEXT);
    SetBkColor(dc, C_BG);
    SetBkMode(dc, TRANSPARENT);
    return (LRESULT)g_bg_brush;
}

static std::string path_of(const char* name) { return std::string(g_dir) + "\\" + name; }

static void load_settings()
{
    for (auto& s : g_set) s.sel = s.def;
    g_fov = FOV_DEF;
    FILE* f = fopen(path_of("settings.ini").c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "fov")) { int f = atoi(v); if (f >= FOV_MIN && f <= FOV_MAX) g_fov = f; }
        for (auto& s : g_set)
            if (!strcmp(s.key, line))
                for (size_t i = 0; i < s.choices.size(); i++)
                    if (!strcmp(s.choices[i].value, v)) s.sel = (int)i;
    }
    fclose(f);
}

static const char* val(const char* key)
{
    for (auto& s : g_set) if (!strcmp(s.key, key)) return s.choices[s.sel].value;
    return "";
}

static bool save_settings()
{
    for (auto& s : g_set) {
        int i = (int)SendMessageA(s.combo, CB_GETCURSEL, 0, 0);
        if (i >= 0) s.sel = i;
    }
    g_fov = (int)SendMessageA(g_fov_tb, TBM_GETPOS, 0, 0);
    FILE* f = fopen(path_of("settings.ini").c_str(), "w");
    if (!f) return false;
    for (auto& s : g_set) fprintf(f, "%s=%s\n", s.key, s.choices[s.sel].value);
    fprintf(f, "fov=%d\n", g_fov);
    fclose(f);

    f = fopen(path_of("settings.bat").c_str(), "w");
    if (!f) return false;
    fprintf(f, "@echo off\nrem Written by Launcher.exe -- change these in the launcher.\n");
    fprintf(f, "set \"RSX_DISPLAY=%s\"\n", val("display"));
    fprintf(f, "set \"RSX_WINDOW=%s\"\n", val("window"));
    fprintf(f, "set \"RSX_RES_SCALE=%s\"\n", val("res"));
    fprintf(f, "set \"CHILD_RFOM_FPS=%s\"\n", val("fps"));
    fprintf(f, "set \"RSX_VSYNC=%s\"\n", val("vsync"));
    fprintf(f, "set \"RSX_ANISO=%s\"\n", val("aniso"));
    fprintf(f, "set \"RSX_FILTER=%s\"\n", val("filter"));
    fprintf(f, "set \"RSX_SHARPEN=%s\"\n", val("sharpen"));
    fprintf(f, "set \"RSX_FSR=%s\"\n", val("fsr"));
    fprintf(f, "set \"RFOM_FOV=%d\"\n", g_fov);
    fclose(f);
    return true;
}


// ---------------------------------------------------------------------------
// Keyboard & mouse controls (v151). Edits controls.txt, which the game reads at
// start (libs/input/cellPad.c). Actions are listed by what they do in the game,
// with the PS3 button each one presses; the game's own Controls screen keeps
// showing the controller and is only meant for controller players.
// ---------------------------------------------------------------------------
struct Act { const char* name; const char* pad; const char* label; };
static const Act g_acts[] = {
    { "Move forward",           "LS_UP",      "" },
    { "Move back",              "LS_DOWN",    "" },
    { "Move left",              "LS_LEFT",    "" },
    { "Move right",             "LS_RIGHT",   "" },
    { "Primary fire",           "R1",         "R1" },
    { "Alt. fire",              "L1",         "L1" },
    { "Toggle zoom",            "R3",         "R3" },
    { "Jump",                   "CROSS",      "Cross" },
    { "Crouch",                 "L2",         "L2" },
    { "Reload",                 "SQUARE",     "Square" },
    { "Melee",                  "TRIANGLE",   "Triangle" },
    { "Interact / use",         "INTERACT",   "Triangle (use)" },
    { "Weapon select",          "R2",         "R2" },
    { "Use grenade",            "CIRCLE",     "Circle" },
    { "Next grenade",           "DPAD_UP",    "D-pad up" },
    { "Previous grenade",       "DPAD_DOWN",  "D-pad down" },
    { "Find partner",           "DPAD_LEFT",  "D-pad left" },
    { "Spotlight",              "DPAD_RIGHT", "D-pad right" },
    { "Pause",                  "START",      "Start" },
    { "Journal",                "SELECT",     "Select" },
    { "Voice chat",             "L3",         "L3" },
    { "Look up (keys)",         "RS_UP",      "" },
    { "Look down (keys)",       "RS_DOWN",    "" },
    { "Look left (keys)",       "RS_LEFT",    "" },
    { "Look right (keys)",      "RS_RIGHT",   "" },
};
static const int NACT = sizeof g_acts / sizeof g_acts[0];
static const int NSLOT = 3;
static std::string g_bind[NACT][NSLOT];          // key names, "" = empty
static double g_msens = 1.0; static int g_minv = 0, g_mdirect = 1;
static std::string g_mrelease = "F8";
static double g_mdeg = 0.05, g_mdz = 0.28, g_msmooth = 0.03;
// v153: controller button that interacts (melee stays on the game's Action button)
static const char* g_padint_opts[][2] = {
    {"NONE","Same as melee (as on PS3)"},{"SQUARE","Square"},{"CIRCLE","Circle"},{"CROSS","Cross"},
    {"TRIANGLE","Triangle"},{"L1","L1"},{"R1","R1"},{"L2","L2"},{"R2","R2"},{"L3","L3 (left stick click)"},
    {"R3","R3 (right stick click)"},{"DPAD_UP","D-pad up"},{"DPAD_DOWN","D-pad down"},{"DPAD_LEFT","D-pad left"},
    {"DPAD_RIGHT","D-pad right"},
};
static int g_padint = 0;

static const char* g_default_binds[][2] = {
    {"W","LS_UP"},{"S","LS_DOWN"},{"A","LS_LEFT"},{"D","LS_RIGHT"},
    {"MOUSE1","R1"},{"MOUSE3","L1"},{"MOUSE2","R3"},{"SPACE","CROSS"},{"CTRL","L2"},{"C","L2"},
    {"R","SQUARE"},{"V","TRIANGLE"},{"F","INTERACT"},{"E","INTERACT"},{"Q","R2"},{"G","CIRCLE"},
    {"WHEELUP","DPAD_UP"},{"WHEELDOWN","DPAD_DOWN"},{"1","DPAD_LEFT"},{"T","DPAD_RIGHT"},
    {"ESC","START"},{"TAB","SELECT"},{"UP","RS_UP"},{"DOWN","RS_DOWN"},{"LEFT","RS_LEFT"},{"RIGHT","RS_RIGHT"},
};

static int act_of(const char* pad)
{
    for (int i = 0; i < NACT; i++) if (!_stricmp(g_acts[i].pad, pad)) return i;
    return -1;
}
static void bind_add(int a, const char* key)
{
    for (int k = 0; k < NSLOT; k++) if (!_stricmp(g_bind[a][k].c_str(), key)) return;
    for (int k = 0; k < NSLOT; k++) if (g_bind[a][k].empty()) { g_bind[a][k] = key; return; }
}
static void ctl_defaults()
{
    for (auto& r : g_bind) for (auto& k : r) k.clear();
    for (auto& d : g_default_binds) { int a = act_of(d[1]); if (a >= 0) bind_add(a, d[0]); }
    g_msens = 1.0; g_minv = 0; g_mdirect = 1; g_mrelease = "F8"; g_mdeg = 0.05; g_mdz = 0.28; g_msmooth = 0.03;
    g_padint = 0;
}
static void ctl_load()
{
    FILE* f = fopen(path_of("controls.txt").c_str(), "r");
    if (!f) { ctl_defaults(); return; }
    for (auto& r : g_bind) for (auto& k : r) k.clear();
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (char* h = strchr(line, '#')) *h = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char k[64] = "", v[64] = "";
        if (sscanf(line, " %63s", k) != 1 || sscanf(eq + 1, " %63s", v) != 1) continue;
        if (!_stricmp(k, "mouse_sensitivity")) { g_msens = atof(v); continue; }
        if (!_stricmp(k, "mouse_invert_y")) { g_minv = atoi(v); continue; }
        if (!_stricmp(k, "mouse_look")) { g_mdirect = _stricmp(v, "stick") ? 1 : 0; continue; }
        if (!_stricmp(k, "mouse_release_key")) { g_mrelease = v; continue; }
        if (!_stricmp(k, "mouse_degrees_per_count")) { g_mdeg = atof(v); continue; }
        if (!_stricmp(k, "mouse_deadzone")) { g_mdz = atof(v); continue; }
        if (!_stricmp(k, "mouse_smoothing")) { g_msmooth = atof(v); continue; }
        if (!_stricmp(k, "pad_interact")) {
            g_padint = 0;
            for (int i = 0; i < (int)(sizeof g_padint_opts / sizeof g_padint_opts[0]); i++)
                if (!_stricmp(v, g_padint_opts[i][0])) g_padint = i;
            continue; }
        const int a = act_of(v);
        if (a >= 0) { for (char* p = k; *p; p++) *p = (char)toupper((unsigned char)*p); bind_add(a, k); }
    }
    fclose(f);
    // v153: older files had one "Action / melee" (TRIANGLE): those keys interact now, V melees
    const int am = act_of("TRIANGLE"), ai = act_of("INTERACT");
    bool any_i = false, any_m = false, v_used = false;
    for (int k = 0; k < NSLOT; k++) { any_i |= !g_bind[ai][k].empty(); any_m |= !g_bind[am][k].empty(); }
    for (int a = 0; a < NACT; a++) for (int k = 0; k < NSLOT; k++) v_used |= !_stricmp(g_bind[a][k].c_str(), "V");
    if (!any_i && any_m) {
        for (int k = 0; k < NSLOT; k++) { g_bind[ai][k] = g_bind[am][k]; g_bind[am][k].clear(); }
        if (!v_used) bind_add(am, "V");
    }
}
static bool ctl_save()
{
    FILE* f = fopen(path_of("controls.txt").c_str(), "w");
    if (!f) return false;
    fprintf(f, "# Resistance PC port -- keyboard & mouse controls.\n"
               "# Edit these in Launcher.exe -> Controls, or by hand: KEY = ACTION, one per line.\n"
               "# KEYS:    A-Z 0-9 F1-F12 SPACE SHIFT CTRL ALT TAB ENTER ESC BACKSPACE UP DOWN LEFT RIGHT\n"
               "#          CAPSLOCK INSERT DELETE HOME END PGUP PGDN NUM0-NUM9\n"
               "#          GRAVE MINUS EQUALS LBRACKET RBRACKET BACKSLASH SEMICOLON APOSTROPHE COMMA PERIOD SLASH\n"
               "#          MOUSE1 (left) MOUSE2 (right) MOUSE3 (middle) MOUSE4 MOUSE5 WHEELUP WHEELDOWN\n"
               "# ACTIONS: LS_UP LS_DOWN LS_LEFT LS_RIGHT (move)  RS_UP RS_DOWN RS_LEFT RS_RIGHT (look)\n"
               "#          CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT\n"
               "#          DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT\n"
               "#          INTERACT (use switches/doors/vehicles; TRIANGLE then only melees)\n"
               "# A key may be listed more than once to press several things.\n\n");
    for (int a = 0; a < NACT; a++)
        for (int k = 0; k < NSLOT; k++)
            if (!g_bind[a][k].empty())
                fprintf(f, "%-10s = %-10s # %s\n", g_bind[a][k].c_str(), g_acts[a].pad, g_acts[a].name);
    fprintf(f, "\n# Mouse\n");
    fprintf(f, "mouse_sensitivity = %.2f\n", g_msens);
    fprintf(f, "mouse_invert_y = %d\n", g_minv);
    fprintf(f, "mouse_look = %s        # direct: the mouse turns the camera 1:1 on foot; stick: old stick emulation\n", g_mdirect ? "direct" : "stick");
    fprintf(f, "mouse_degrees_per_count = %.3f   # direct look speed (x mouse_sensitivity, x the game's Look Sensitivity)\n", g_mdeg);
    fprintf(f, "mouse_deadzone = %.2f      # stick emulation only\n", g_mdz);
    fprintf(f, "mouse_smoothing = %.2f     # stick emulation only, seconds\n", g_msmooth);
    fprintf(f, "mouse_release_key = %s     # free / recapture the mouse cursor\n", g_mrelease.c_str());
    fprintf(f, "\n# Controller\n");
    fprintf(f, "pad_interact = %s     # button that uses things (the game's Action button then only melees); NONE = as on PS3\n",
            g_padint_opts[g_padint][0]);
    fclose(f);
    return true;
}

// key name for a captured input (names the game's controls.txt reader knows)
static const char* vk_name(int vk)
{
    static char one[2];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { one[0] = (char)vk; one[1] = 0; return one; }
    if (vk >= VK_F1 && vk <= VK_F12) { static char fn[4]; snprintf(fn, sizeof fn, "F%d", vk - VK_F1 + 1); return fn; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { static char nn[5]; snprintf(nn, sizeof nn, "NUM%d", vk - VK_NUMPAD0); return nn; }
    switch (vk) {
    case VK_SPACE: return "SPACE"; case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "SHIFT";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "CTRL"; case VK_MENU: case VK_LMENU: case VK_RMENU: return "ALT";
    case VK_TAB: return "TAB"; case VK_RETURN: return "ENTER"; case VK_ESCAPE: return "ESC"; case VK_BACK: return "BACKSPACE";
    case VK_UP: return "UP"; case VK_DOWN: return "DOWN"; case VK_LEFT: return "LEFT"; case VK_RIGHT: return "RIGHT";
    case VK_CAPITAL: return "CAPSLOCK"; case VK_INSERT: return "INSERT"; case VK_DELETE: return "DELETE";
    case VK_HOME: return "HOME"; case VK_END: return "END"; case VK_PRIOR: return "PGUP"; case VK_NEXT: return "PGDN";
    case VK_OEM_3: return "GRAVE"; case VK_OEM_MINUS: return "MINUS"; case VK_OEM_PLUS: return "EQUALS";
    case VK_OEM_4: return "LBRACKET"; case VK_OEM_6: return "RBRACKET"; case VK_OEM_5: return "BACKSLASH";
    case VK_OEM_1: return "SEMICOLON"; case VK_OEM_7: return "APOSTROPHE"; case VK_OEM_COMMA: return "COMMA";
    case VK_OEM_PERIOD: return "PERIOD"; case VK_OEM_2: return "SLASH";
    }
    return NULL;
}
static std::string pretty(const std::string& k)
{
    static const struct { const char* n; const char* p; } t[] = {
        {"MOUSE1","Left mouse"},{"MOUSE2","Right mouse"},{"MOUSE3","Middle mouse"},{"MOUSE4","Mouse 4"},{"MOUSE5","Mouse 5"},
        {"WHEELUP","Wheel up"},{"WHEELDOWN","Wheel down"},{"SPACE","Space"},{"SHIFT","Shift"},{"CTRL","Ctrl"},{"ALT","Alt"},
        {"TAB","Tab"},{"ENTER","Enter"},{"ESC","Esc"},{"BACKSPACE","Backspace"},{"UP","Up arrow"},{"DOWN","Down arrow"},
        {"LEFT","Left arrow"},{"RIGHT","Right arrow"},{"CAPSLOCK","Caps Lock"},{"GRAVE","`"},{"MINUS","-"},{"EQUALS","="},
        {"LBRACKET","["},{"RBRACKET","]"},{"BACKSLASH","\\"},{"SEMICOLON",";"},{"APOSTROPHE","'"},{"COMMA",","},{"PERIOD","."},{"SLASH","/"},
    };
    for (auto& e : t) if (!_stricmp(k.c_str(), e.n)) return e.p;
    return k;
}

enum { IDC_BIND0 = 1000, IDC_CTL_SAVE = 1900, IDC_CTL_CANCEL, IDC_CTL_RESET, IDC_MSENS, IDC_MINV, IDC_MDIRECT, IDC_PADINT };
static HWND g_ctl, g_bindbtn[NACT][NSLOT], g_msens_tb, g_msens_lbl, g_minv_cb, g_mdirect_cb, g_ctl_status, g_padint_cb;
static int g_cap_a = -1, g_cap_k = -1;           // slot being captured
static void ctl_refresh()
{
    for (int a = 0; a < NACT; a++)
        for (int k = 0; k < NSLOT; k++) {
            std::string t = (a == g_cap_a && k == g_cap_k) ? std::string("press a key...") :
                            g_bind[a][k].empty() ? std::string("-") : pretty(g_bind[a][k]);
            SetWindowTextA(g_bindbtn[a][k], t.c_str());
        }
    char s[64]; snprintf(s, sizeof s, "%.2fx", g_msens); SetWindowTextA(g_msens_lbl, s);
}
static void ctl_capture_done(const char* key)
{
    if (g_cap_a < 0) return;
    if (key) {
        // one key, one action: a key taken from elsewhere moves here
        for (int a = 0; a < NACT; a++) for (int k = 0; k < NSLOT; k++)
            if (!_stricmp(g_bind[a][k].c_str(), key)) g_bind[a][k].clear();
        g_bind[g_cap_a][g_cap_k] = key;
        if (!_stricmp(key, g_mrelease.c_str()))
            SetWindowTextA(g_ctl_status, ("Note: " + pretty(key) + " is also the key that frees the mouse cursor.").c_str());
        else SetWindowTextA(g_ctl_status, "");
    }
    g_cap_a = g_cap_k = -1;
    ctl_refresh();
}
// called from the message loop: returns true when it consumed the message
static bool ctl_capture_msg(MSG* m)
{
    if (g_cap_a < 0 || !g_ctl) return false;
    switch (m->message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const int vk = (int)m->wParam;
        const char* n = vk_name(vk);
        ctl_capture_done(n);              // an unsupported key cancels
        return true; }
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: return true;
    case WM_LBUTTONDOWN: ctl_capture_done("MOUSE1"); return true;
    case WM_RBUTTONDOWN: ctl_capture_done("MOUSE2"); return true;
    case WM_MBUTTONDOWN: ctl_capture_done("MOUSE3"); return true;
    case WM_XBUTTONDOWN: ctl_capture_done(HIWORD(m->wParam) == XBUTTON1 ? "MOUSE4" : "MOUSE5"); return true;
    case WM_MOUSEWHEEL: ctl_capture_done(GET_WHEEL_DELTA_WPARAM(m->wParam) > 0 ? "WHEELUP" : "WHEELDOWN"); return true;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: return true;
    }
    return false;
}
static LRESULT CALLBACK ctlproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT:
        return dark_ctlcolor(msg, wp, lp);
    case WM_HSCROLL:
        if ((HWND)lp == g_msens_tb) { g_msens = SendMessageA(g_msens_tb, TBM_GETPOS, 0, 0) / 20.0; ctl_refresh(); }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_BIND0 && id < IDC_BIND0 + NACT * NSLOT) {
            if (g_cap_a >= 0) return 0;
            g_cap_a = (id - IDC_BIND0) / NSLOT; g_cap_k = (id - IDC_BIND0) % NSLOT;
            ctl_refresh();
            return 0;
        }
        if (id == IDC_MINV) { g_minv = SendMessageA(g_minv_cb, BM_GETCHECK, 0, 0) == BST_CHECKED; return 0; }
        if (id == IDC_MDIRECT) { g_mdirect = SendMessageA(g_mdirect_cb, BM_GETCHECK, 0, 0) == BST_CHECKED; return 0; }
        if (id == IDC_PADINT && HIWORD(wp) == CBN_SELCHANGE) {
            const int i = (int)SendMessageA(g_padint_cb, CB_GETCURSEL, 0, 0); if (i >= 0) g_padint = i; return 0; }
        if (id == IDC_CTL_RESET) { ctl_defaults(); SendMessageA(g_msens_tb, TBM_SETPOS, TRUE, (LPARAM)(g_msens * 20));
            SendMessageA(g_minv_cb, BM_SETCHECK, g_minv ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageA(g_mdirect_cb, BM_SETCHECK, g_mdirect ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageA(g_padint_cb, CB_SETCURSEL, g_padint, 0);
            ctl_refresh(); return 0; }
        if (id == IDC_CTL_SAVE) {
            if (!ctl_save()) { MessageBoxA(hw, "Could not write controls.txt.", "Controls", MB_ICONERROR); return 0; }
            DestroyWindow(hw); return 0; }
        if (id == IDC_CTL_CANCEL) { DestroyWindow(hw); return 0; }
        break; }
    case WM_CONTEXTMENU: {                       // right-click a key box: clear it
        for (int a = 0; a < NACT; a++) for (int k = 0; k < NSLOT; k++)
            if ((HWND)wp == g_bindbtn[a][k] && g_cap_a < 0) { g_bind[a][k].clear(); ctl_refresh(); return 0; }
        break; }
    case WM_CLOSE: DestroyWindow(hw); return 0;
    case WM_DESTROY: g_ctl = NULL; g_cap_a = g_cap_k = -1; return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}
static HWND add(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h,
                int id = 0, HFONT font = NULL);
static void open_controls(HWND owner)
{
    if (g_ctl) { SetForegroundWindow(g_ctl); return; }
    ctl_load();
    static bool reg = false;
    if (!reg) { reg = true;
        WNDCLASSEXA wc = { sizeof wc }; wc.lpfnWndProc = ctlproc; wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = g_bg_brush;
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION); wc.lpszClassName = "RfomControls"; RegisterClassExA(&wc); }
    const int L = 20, RH = 25, KW = 120;
    const int ch = 16 + 26 + 40 + 24 + NACT * RH + 16 + 30 + 30 + 30 + 30 + 34 + 28 + 40 + 26 + 44 + 16;
    RECT r = { 0, 0, 640, ch };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT orc; GetWindowRect(owner, &orc);
    g_ctl = CreateWindowExA(0, "RfomControls", "Controls - keyboard & mouse", style, orc.right + 8, orc.top,
                            r.right - r.left, r.bottom - r.top, owner, NULL, GetModuleHandleA(NULL), NULL);
    HWND hw = g_ctl;
    dark_titlebar(hw);
    int y = 16;
    add(hw, "STATIC", "Keyboard && mouse controls", 0, L, y, 600, 26, 0, g_bold); y += 28;
    add(hw, "STATIC", "Click a box, then press a key or mouse button. Right-click a box to clear it. "
        "The game's own Controls screen shows the PS3 controller; it applies when a controller is plugged in.",
        0, L, y, 600, 34, 0, g_small); y += 40;
    add(hw, "STATIC", "Action", 0, L, y, 200, 20, 0, g_small);
    add(hw, "STATIC", "PS3 button", 0, L + 190, y, 80, 20, 0, g_small);
    add(hw, "STATIC", "Keys", 0, L + 280, y, 200, 20, 0, g_small); y += 22;
    for (int a = 0; a < NACT; a++) {
        add(hw, "STATIC", g_acts[a].name, 0, L, y + 4, 190, 20);
        add(hw, "STATIC", g_acts[a].label, 0, L + 190, y + 5, 90, 20, 0, g_small);
        for (int k = 0; k < NSLOT; k++)
            g_bindbtn[a][k] = add(hw, "BUTTON", "", WS_TABSTOP, L + 280 + k * (KW + 6), y, KW, RH - 3,
                                  IDC_BIND0 + a * NSLOT + k, g_small);
        y += RH;
    }
    y += 16;
    add(hw, "STATIC", "Mouse sensitivity", 0, L, y + 4, 190, 20);
    g_msens_tb = add(hw, TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, L + 184, y, 300, 28, IDC_MSENS);
    SendMessageA(g_msens_tb, TBM_SETRANGE, TRUE, MAKELPARAM(2, 80));     // 0.10x .. 4.00x
    SendMessageA(g_msens_tb, TBM_SETPOS, TRUE, (LPARAM)(g_msens * 20 + 0.5));
    g_msens_lbl = add(hw, "STATIC", "", 0, L + 490, y + 4, 80, 20);
    y += 30;
    add(hw, "STATIC", "Also scaled by the game's Look Sensitivity option (0-9, middle = 1x).", 0, L + 190, y, 420, 20, 0, g_small);
    y += 24;
    g_minv_cb = add(hw, "BUTTON", "", BS_AUTOCHECKBOX | WS_TABSTOP, L, y, 18, 24, IDC_MINV);
    add(hw, "STATIC", "Invert mouse up/down (on top of the game's Invert-Y option)", 0, L + 24, y + 3, 540, 20);
    SendMessageA(g_minv_cb, BM_SETCHECK, g_minv ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 30;
    g_mdirect_cb = add(hw, "BUTTON", "", BS_AUTOCHECKBOX | WS_TABSTOP, L, y, 18, 24, IDC_MDIRECT);
    add(hw, "STATIC", "Direct mouse look (off = the old stick-style mouse)", 0, L + 24, y + 3, 540, 20);
    SendMessageA(g_mdirect_cb, BM_SETCHECK, g_mdirect ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 34;
    add(hw, "STATIC", "Controller interact button", 0, L, y + 4, 190, 20);
    g_padint_cb = add(hw, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, L + 190, y, 260, 300, IDC_PADINT);
    for (auto& o : g_padint_opts) SendMessageA(g_padint_cb, CB_ADDSTRING, 0, (LPARAM)o[1]);
    SendMessageA(g_padint_cb, CB_SETCURSEL, g_padint, 0);
    y += 28;
    add(hw, "STATIC", "With a controller: this button uses switches, doors and vehicles, and the game's "
        "Action button (set on the game's Controls screen) only melees. The button keeps its own job too.",
        0, L + 190, y, 420, 34, 0, g_small);
    y += 40;
    g_ctl_status = add(hw, "STATIC", "", 0, L, y, 600, 20, 0, g_small);
    y += 26;
    add(hw, "BUTTON", "Save", BS_DEFPUSHBUTTON | WS_TABSTOP, L, y, 120, 34, IDC_CTL_SAVE);
    add(hw, "BUTTON", "Cancel", WS_TABSTOP, L + 130, y, 100, 34, IDC_CTL_CANCEL);
    add(hw, "BUTTON", "Reset to defaults", WS_TABSTOP, L + 440, y, 160, 34, IDC_CTL_RESET);
    ctl_refresh();
    ShowWindow(hw, SW_SHOW);
}

enum { ID_PLAY = 100, ID_SAVE, ID_QUIT, ID_FOLDER, ID_CONTROLS };

static HWND add(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h,
                int id, HFONT font)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    if (!_stricmp(cls, "COMBOBOX")) SetWindowTheme(c, L"DarkMode_CFD", NULL);
    else if (_stricmp(cls, "STATIC")) SetWindowTheme(c, L"DarkMode_Explorer", NULL);
    return c;
}

static void fov_label()
{
    char t[64];
    const int f = (int)SendMessageA(g_fov_tb, TBM_GETPOS, 0, 0);
    snprintf(t, sizeof t, f == FOV_DEF ? "%d\xB0 (original)" : "%d\xB0", f);
    SetWindowTextA(g_fov_lbl, t);
}

static void build_ui(HWND hw)
{
    const int L = 20, W = 460;
    int y = 16;
    add(hw, "STATIC", "Resistance: Fall of Man", 0, L, y, W, 26, 0, g_bold);
    y += 28;
    add(hw, "STATIC", "PC port settings. Changes apply the next time the game starts.", 0, L, y, W, 18, 0, g_small);
    y += 30;
    for (auto& s : g_set) {
        add(hw, "STATIC", s.title, 0, L, y + 4, 190, 20);
        s.combo = add(hw, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, L + 200, y, W - 200, 300);
        for (auto& c : s.choices) SendMessageA(s.combo, CB_ADDSTRING, 0, (LPARAM)c.label);
        SendMessageA(s.combo, CB_SETCURSEL, s.sel, 0);
        y += 28;
        add(hw, "STATIC", s.hint, 0, L + 200, y, W - 200, 32, 0, g_small);
        y += 38;
    }
    add(hw, "STATIC", "Field of view", 0, L, y + 4, 190, 20);
    g_fov_tb = add(hw, TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, L + 194, y, W - 300, 28);
    SendMessageA(g_fov_tb, TBM_SETRANGE, TRUE, MAKELPARAM(FOV_MIN, FOV_MAX));
    SendMessageA(g_fov_tb, TBM_SETPAGESIZE, 0, 5);
    SendMessageA(g_fov_tb, TBM_SETPOS, TRUE, g_fov);
    g_fov_lbl = add(hw, "STATIC", "", 0, L + W - 100, y + 4, 100, 20);
    fov_label();
    y += 28;
    add(hw, "STATIC", "Horizontal, in degrees. 82 is the game's own view; 90-110 is typical on PC. "
        "Zoom and scopes keep their zoom.", 0, L + 200, y, W - 200, 32, 0, g_small);
    y += 38;
    y += 6;
    add(hw, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, L, y, 110, 34, ID_PLAY);
    add(hw, "BUTTON", "Controls...", WS_TABSTOP, L + 118, y, 100, 34, ID_CONTROLS);
    add(hw, "BUTTON", "Save", WS_TABSTOP, L + 226, y, 70, 34, ID_SAVE);
    add(hw, "BUTTON", "Open folder", WS_TABSTOP, L + 304, y, 90, 34, ID_FOLDER);
    add(hw, "BUTTON", "Quit", WS_TABSTOP, L + 402, y, 58, 34, ID_QUIT);
}

static LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: build_ui(hw); return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_fov_tb) fov_label();
        return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT:
        return dark_ctlcolor(msg, wp, lp);
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_PLAY:
            if (!save_settings()) { MessageBoxA(hw, "Could not write settings.bat.", "Launcher", MB_ICONERROR); return 0; }
            if ((INT_PTR)ShellExecuteA(hw, "open", path_of("run.bat").c_str(), NULL, g_dir, SW_SHOWMINNOACTIVE) <= 32) {
                MessageBoxA(hw, "Could not start run.bat.", "Launcher", MB_ICONERROR); return 0; }
            DestroyWindow(hw);
            return 0;
        case ID_SAVE:
            if (save_settings()) MessageBoxA(hw, "Saved.", "Launcher", MB_ICONINFORMATION);
            else MessageBoxA(hw, "Could not write settings.", "Launcher", MB_ICONERROR);
            return 0;
        case ID_CONTROLS:
            open_controls(hw);
            return 0;
        case ID_FOLDER:
            ShellExecuteA(hw, "open", g_dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case ID_QUIT: DestroyWindow(hw); return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show)
{
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    if (char* sl = strrchr(g_dir, '\\')) *sl = 0;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    load_settings();

    g_bg_brush = CreateSolidBrush(C_BG); g_ctl_brush = CreateSolidBrush(C_CTL);
    dark_app_mode();
    g_font  = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_small = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_bold  = CreateFontA(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    WNDCLASSEXA wc = { sizeof wc };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "RfomLauncher";
    RegisterClassExA(&wc);

    const int ch = 16 + 28 + 30 + ((int)g_set.size() + 1) * 66 + 6 + 34 + 20;
    RECT r = { 0, 0, 500, ch };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND hw = CreateWindowExA(0, wc.lpszClassName, "Resistance: Fall of Man - Launcher", style,
                              (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                              ww, wh, NULL, NULL, inst, NULL);
    dark_titlebar(hw);
    ShowWindow(hw, show);
    UpdateWindow(hw);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (ctl_capture_msg(&m)) continue;
        if (g_ctl && IsDialogMessageA(g_ctl, &m)) continue;
        if (IsDialogMessageA(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
