/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */
#include "ps3emu/log_first.h"

/* PAD_CLOCK=flips: PAD_SCRIPT and PAD_STICK times count guest flips instead of
 * wall-clock seconds. A loaded machine runs a title at a third of its usual
 * frame rate, and a wall-clock schedule then presses START on the logos; a
 * flip schedule replays the same screens however slow the run is. */
extern u32 cellGcm_flip_request_count(void);
static double pad_clock(unsigned long long t0_ms)
{
    static int flips = -1;
    if (flips < 0) { const char* e = getenv("PAD_CLOCK"); flips = e && !strcmp(e, "flips"); }
    return flips ? (double)cellGcm_flip_request_count()
                 : (double)(GetTickCount64() - t0_ms) / 1000.0;
}

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;
volatile unsigned long long g_pad_inj_until = 0;
volatile u32 g_pad_inj_btn = 0, g_pad_inj_sticks = 0x80808080u;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * Rumble (cellPadSetActDirect)
 *
 * A DualShock 3 has two motors: the small one (motor[0]) is on or off, the
 * large one (motor[1]) takes 0-255. A title sets them and they stay that way
 * until it sets them again; some titles only call when the value changes.
 * SDL's rumble runs for a duration instead, so the last request per port is
 * kept and renewed from the pad poll while it is non-zero. If the title stops
 * polling (a hang, a closed window), the motors stop within
 * PAD_RUMBLE_HOLD_MS. Large -> the host's low-frequency motor, small -> the
 * high-frequency one, which is how DualShock 4, DualSense and Xbox pads are
 * laid out too.
 * -------------------------------------------------------------------------*/

#define PAD_RUMBLE_HOLD_MS     1000   /* one SDL request lasts this long ... */
#define PAD_RUMBLE_RENEW_MS     400   /* ... and is renewed this often */

typedef struct {
    u16 lo, hi;                 /* what the host motors were last asked for */
    unsigned long long sent_ms; /* when (pad_now_ms), 0 = nothing to renew */
} PadRumble;
static PadRumble s_rumble[PAD_MAX_HOST_PORTS];

static unsigned long long pad_now_ms(void);

/* The two host motor levels (0-65535) for a PS3 actuator request. */
static void pad_rumble_levels(u8 small_on, u8 large, u16* lo, u16* hi)
{
    *lo = (u16)(large * 257u);
    *hi = small_on ? 0xFFFFu : 0u;
}

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Resistance port: PC shooter controls for port 0 (when no XInput pad).
 * Keyboard keys and mouse buttons map to PS3 buttons/sticks through
 * controls.txt (created with defaults on first run, next to run.bat); mouse
 * movement drives the right stick from raw input deltas collected by the
 * D3D12 window (g_kbm_mouse_dx/dy, g_kbm_wheel). While the game window has
 * focus the cursor is hidden and held in the window; F8 (configurable)
 * releases/recaptures it. PAD_NO_KEYBOARD=1 disables all of it. */
volatile long g_kbm_mouse_dx = 0, g_kbm_mouse_dy = 0, g_kbm_wheel = 0;
volatile int  g_kbm_capture = 1;          /* read by the window's WM_SETCURSOR */
void*         g_kbm_hwnd = NULL;          /* set by the window owner */

enum { KA_NONE = 0, KA_BTN, KA_LS_UP, KA_LS_DOWN, KA_LS_LEFT, KA_LS_RIGHT,
       KA_RS_UP, KA_RS_DOWN, KA_RS_LEFT, KA_RS_RIGHT, KA_INTERACT };
typedef struct { int vk; int act; u16 btn; } KbmBind;
static KbmBind s_kbm[96]; static int s_kbm_n = 0;
static double s_kbm_sens = 1.0, s_kbm_dz = 0.28, s_kbm_smooth = 0.03;
static int s_kbm_invert = 0, s_kbm_toggle_vk = VK_F8;
#define KBM_WHEEL_UP   0x1001
#define KBM_WHEEL_DOWN 0x1002

static int kbm_key(const char* k)
{
    static const struct { const char* n; int vk; } t[] = {
        {"SPACE",VK_SPACE},{"SHIFT",VK_SHIFT},{"LSHIFT",VK_LSHIFT},{"CTRL",VK_CONTROL},{"LCTRL",VK_LCONTROL},
        {"ALT",VK_MENU},{"TAB",VK_TAB},{"ENTER",VK_RETURN},{"ESC",VK_ESCAPE},{"BACKSPACE",VK_BACK},
        {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
        {"MOUSE1",VK_LBUTTON},{"MOUSE2",VK_RBUTTON},{"MOUSE3",VK_MBUTTON},{"MOUSE4",VK_XBUTTON1},{"MOUSE5",VK_XBUTTON2},
        {"WHEELUP",KBM_WHEEL_UP},{"WHEELDOWN",KBM_WHEEL_DOWN},
        {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},{"F7",VK_F7},
        {"F8",VK_F8},{"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
        /* v151: the rest of what the launcher's Controls window can bind */
        {"RSHIFT",VK_RSHIFT},{"RCTRL",VK_RCONTROL},{"CAPSLOCK",VK_CAPITAL},{"INSERT",VK_INSERT},{"DELETE",VK_DELETE},
        {"HOME",VK_HOME},{"END",VK_END},{"PGUP",VK_PRIOR},{"PGDN",VK_NEXT},
        {"NUM0",VK_NUMPAD0},{"NUM1",VK_NUMPAD1},{"NUM2",VK_NUMPAD2},{"NUM3",VK_NUMPAD3},{"NUM4",VK_NUMPAD4},
        {"NUM5",VK_NUMPAD5},{"NUM6",VK_NUMPAD6},{"NUM7",VK_NUMPAD7},{"NUM8",VK_NUMPAD8},{"NUM9",VK_NUMPAD9},
        {"GRAVE",VK_OEM_3},{"MINUS",VK_OEM_MINUS},{"EQUALS",VK_OEM_PLUS},{"LBRACKET",VK_OEM_4},{"RBRACKET",VK_OEM_6},
        {"BACKSLASH",VK_OEM_5},{"SEMICOLON",VK_OEM_1},{"APOSTROPHE",VK_OEM_7},{"COMMA",VK_OEM_COMMA},
        {"PERIOD",VK_OEM_PERIOD},{"SLASH",VK_OEM_2},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!_stricmp(k, t[i].n)) return t[i].vk;
    if (strlen(k) == 1) { char c = k[0]; if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; }
    return 0;
}
static int kbm_action(const char* a, u16* btn)
{
    static const struct { const char* n; int act; u16 btn; } t[] = {
        {"LS_UP",KA_LS_UP,0},{"LS_DOWN",KA_LS_DOWN,0},{"LS_LEFT",KA_LS_LEFT,0},{"LS_RIGHT",KA_LS_RIGHT,0},
        {"RS_UP",KA_RS_UP,0},{"RS_DOWN",KA_RS_DOWN,0},{"RS_LEFT",KA_RS_LEFT,0},{"RS_RIGHT",KA_RS_RIGHT,0},
        {"CROSS",KA_BTN,CELL_PAD_CTRL_CROSS},{"CIRCLE",KA_BTN,CELL_PAD_CTRL_CIRCLE},
        {"SQUARE",KA_BTN,CELL_PAD_CTRL_SQUARE},{"TRIANGLE",KA_BTN,CELL_PAD_CTRL_TRIANGLE},
        {"L1",KA_BTN,CELL_PAD_CTRL_L1},{"R1",KA_BTN,CELL_PAD_CTRL_R1},{"L2",KA_BTN,CELL_PAD_CTRL_L2},
        {"R2",KA_BTN,CELL_PAD_CTRL_R2},{"L3",KA_BTN,CELL_PAD_CTRL_L3},{"R3",KA_BTN,CELL_PAD_CTRL_R3},
        {"START",KA_BTN,CELL_PAD_CTRL_START},{"SELECT",KA_BTN,CELL_PAD_CTRL_SELECT},
        {"DPAD_UP",KA_BTN,CELL_PAD_CTRL_UP},{"DPAD_DOWN",KA_BTN,CELL_PAD_CTRL_DOWN},
        {"DPAD_LEFT",KA_BTN,CELL_PAD_CTRL_LEFT},{"DPAD_RIGHT",KA_BTN,CELL_PAD_CTRL_RIGHT},
        {"INTERACT",KA_INTERACT,0},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!_stricmp(a, t[i].n)) { *btn = t[i].btn; return t[i].act; }
    return KA_NONE;
}

static const char* s_kbm_default =
    "# Resistance PC port -- keyboard & mouse controls.\n"
    "# Format:  KEY = ACTION   (one per line, # starts a comment)\n"
    "# KEYS:    A-Z 0-9 SPACE SHIFT CTRL ALT TAB ENTER ESC BACKSPACE UP DOWN LEFT RIGHT\n"
    "#          MOUSE1 (left) MOUSE2 (right) MOUSE3 (middle) MOUSE4 MOUSE5 WHEELUP WHEELDOWN F1-F12\n"
    "# ACTIONS: LS_UP LS_DOWN LS_LEFT LS_RIGHT (move)  RS_UP RS_DOWN RS_LEFT RS_RIGHT (look)\n"
    "#          CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT\n"
    "#          DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT\n"
    "# A key may be listed more than once to press several things.\n"
    "\n"
    "# Movement\n"
    "W = LS_UP\nS = LS_DOWN\nA = LS_LEFT\nD = LS_RIGHT\n"
    "\n"
    "# Shooting\n"
    "MOUSE1 = R1        # fire\n"
    "MOUSE3 = L1        # alternate fire\n"
    "MOUSE2 = R3        # zoom\n"
    "G = CIRCLE         # grenade\n"
    "\n"
    "# Actions\n"
    "SPACE = CROSS      # jump\n"
    "C = L2             # crouch\n"
    "SHIFT = L2\n"
    "R = SQUARE         # reload\n"
    "V = TRIANGLE       # melee\n"
    "F = INTERACT       # use switches, doors, vehicles...\n"
    "Q = R2             # weapon select\n"
    "\n"
    "# Weapon zoom / d-pad\n"
    "WHEELUP = DPAD_UP\nWHEELDOWN = DPAD_DOWN\n"
    "1 = DPAD_UP\n2 = DPAD_RIGHT\n3 = DPAD_DOWN\n4 = DPAD_LEFT\n"
    "UP = DPAD_UP\nDOWN = DPAD_DOWN\nLEFT = DPAD_LEFT\nRIGHT = DPAD_RIGHT\n"
    "\n"
    "# Menus\n"
    "ESC = START        # pause\n"
    "ENTER = START\n"
    "TAB = SELECT\n"
    "\n"
    "# Look with the arrow-free keys too (optional)\n"
    "I = RS_UP\nK = RS_DOWN\nJ = RS_LEFT\nL = RS_RIGHT\n"
    "\n"
    "# Mouse settings\n"
    "mouse_sensitivity = 1.0\n"
    "mouse_invert_y = 0\n"
    "mouse_deadzone = 0.28      # the game's own stick deadzone (raise if small moves do nothing)\n"
    "mouse_smoothing = 0.03     # seconds\n"
    "mouse_release_key = F8     # free / recapture the mouse cursor\n"
    "mouse_look = direct        # direct: the mouse turns the camera 1:1 on foot; stick: old stick emulation\n"
    "mouse_degrees_per_count = 0.05   # direct look speed (also x mouse_sensitivity)\n";

/* Resistance port (v148/v149): direct mouse look. The game's look code
 * (game.elf func_002A4D14, project/stubs.cpp rfom_mouselook) adds mouse counts
 * straight to the player's yaw/pitch and cancels the turning the right stick
 * would have done. The stick is STILL fed from the mouse as before (v149):
 * the game reads it for more than the camera -- shaking off a grabbing
 * Menial is "waggle both sticks" -- so only its effect on the view goes.
 * Everything that does not run that look code (menus, vehicles, cutscenes)
 * keeps the stick emulation as it was. controls.txt:
 *   mouse_look = direct | stick
 *   mouse_degrees_per_count = 0.05   (x mouse_sensitivity) */
volatile long g_ml_mode = 1;                 /* 1 = direct */
volatile double g_ml_deg_per_count = 0.05;   /* already x mouse_sensitivity */
volatile int g_ml_invert_y = 0;
volatile int g_ml_fed_rx = 0, g_ml_fed_ry = 0;   /* last stick deflection fed (-127..127) */
volatile long g_kbm_ml_dx = 0, g_kbm_ml_dy = 0;  /* mouse counts for the direct look */
/* v151: when the keyboard+mouse pad last fed port 0 (a connected controller
 * takes the port and this stops), and whether the look keys (RS_*) drove the
 * stick instead of the mouse: the look code only cancels the stick's turning
 * when the stick IS the mouse, so a controller and the look keys turn as on PS3. */
volatile ULONGLONG g_ml_kbm_ms = 0;
volatile int g_ml_rs_keys = 0;
static double s_ml_deg = 0.05;
static int s_menu_mouse = 1; static long s_menu_step = 150;   /* v160 */

/* v153: melee and interact on separate inputs. On PS3 one button (Triangle)
 * does both: it uses a switch/door/vehicle when one is in reach and melees
 * otherwise. Keys bound to TRIANGLE now only melee; keys bound to INTERACT
 * only use things: project/stubs.cpp (rfom_use_begin) hands the game's
 * "use" checks (func_00296EBC, func_00295FC8) the INTERACT key instead of
 * Triangle. With a controller, pad_interact = <button> does the same for that
 * button (NONE = as on PS3). */
volatile unsigned long long g_kbm_mouse_abs = 0;     /* total mouse travel, counts */
volatile unsigned long long g_rfom_play_ms = 0;   /* v162: last tick the in-play look code ran (stubs.cpp) */
volatile int g_rfom_pause_ui = 0;   /* v162c: the pause menu loaded its text (stubs.cpp); cleared when play resumes */
volatile int g_rfom_space_start = 0;   /* v160v: set by stubs.cpp while a level intro card waits for Start */
volatile int g_rfom_use_kbm = 0;              /* an INTERACT key is held */
volatile int g_rfom_kbm_interact_bound = 0;   /* controls.txt binds INTERACT */
volatile int g_rfom_input_kbm = 0;            /* port 0 is the keyboard (no controller) */
volatile u16 g_rfom_pad_btns0 = 0;            /* controller port 0 buttons (CELL_PAD_CTRL_*) */
volatile u16 g_rfom_pad_interact = 0;         /* controller interact button, 0 = as on PS3 */
static void ctl_extra_load(void)
{
    /* One-time migration of an older controls.txt: what was "Action / melee"
     * (TRIANGLE) becomes interact, and V becomes melee if it is free. */
    FILE* f = fopen("controls.txt", "r");
    if (!f) return;
    static char lines[200][256]; int n = 0, has_inter = 0, has_tri = 0, v_used = 0;
    while (n < 200 && fgets(lines[n], sizeof lines[n], f)) {
        char tmp[256]; memcpy(tmp, lines[n], sizeof tmp);
        char* h = strchr(tmp, '#'); if (h) *h = 0;
        char* eq = strchr(tmp, '=');
        if (eq) { *eq = 0; char k[64], v[64];
            if (sscanf(tmp, " %63s", k) == 1 && sscanf(eq + 1, " %63s", v) == 1) {
                if (!_stricmp(v, "INTERACT")) has_inter = 1;
                if (!_stricmp(v, "TRIANGLE")) has_tri = 1;
                if (!_stricmp(k, "V")) v_used = 1;
                if (!_stricmp(k, "pad_interact")) {
                    u16 b = 0; g_rfom_pad_interact = (kbm_action(v, &b) == KA_BTN) ? b : 0; }
            } }
        n++;
    }
    fclose(f);
    if (has_inter || !has_tri) { g_rfom_kbm_interact_bound = has_inter; return; }
    FILE* w = fopen("controls.txt", "w");
    if (!w) return;
    for (int i = 0; i < n; i++) {
        char* L = lines[i];
        char* eq = strchr(L, '='); char* h = strchr(L, '#');
        if (eq && (!h || h > eq)) {
            char k[64], v[64];
            if (sscanf(L, " %63[^= \t]", k) == 1 && sscanf(eq + 1, " %63s", v) == 1 && !_stricmp(v, "TRIANGLE")) {
                fprintf(w, "%-10s = %-10s # Interact / use\n", k, "INTERACT");
                continue;
            }
        }
        fputs(L, w);
        if (!strchr(L, '\n')) fputc('\n', w);
    }
    if (!v_used) fprintf(w, "%-10s = %-10s # Melee\n", "V", "TRIANGLE");
    fclose(w);
    g_rfom_kbm_interact_bound = 1;
    printf("[controls] controls.txt updated: melee and interact are separate now (V = melee)\n");
}
static void kbm_load(void)
{
    FILE* f = fopen("controls.txt", "r");
    if (!f) {
        FILE* w = fopen("controls.txt", "w");
        if (w) { fputs(s_kbm_default, w); fclose(w); }
        f = fopen("controls.txt", "r");
    }
    char line[256];
    s_kbm_n = 0;
    while (f && fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#'); if (h) *h = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char k[64], v[64];
        if (sscanf(line, " %63s", k) != 1 || sscanf(eq + 1, " %63s", v) != 1) continue;
        if (!_stricmp(k, "mouse_sensitivity")) { s_kbm_sens = atof(v); continue; }
        if (!_stricmp(k, "mouse_invert_y"))    { s_kbm_invert = atoi(v); continue; }
        if (!_stricmp(k, "mouse_deadzone"))    { s_kbm_dz = atof(v); continue; }
        if (!_stricmp(k, "mouse_smoothing"))   { s_kbm_smooth = atof(v); continue; }
        if (!_stricmp(k, "mouse_release_key")) { int vk = kbm_key(v); if (vk) s_kbm_toggle_vk = vk; continue; }
        if (!_stricmp(k, "mouse_look"))        { g_ml_mode = _stricmp(v, "stick") ? 1 : 0; continue; }
        if (!_stricmp(k, "mouse_degrees_per_count")) { s_ml_deg = atof(v); continue; }
        if (!_stricmp(k, "pad_interact")) continue;               /* ctl_extra_load */
        if (!_stricmp(k, "menu_mouse"))       { s_menu_mouse = atoi(v); continue; }
        if (!_stricmp(k, "menu_mouse_step"))  { s_menu_step = atol(v); continue; }
        int vk = kbm_key(k); u16 btn = 0; int act = kbm_action(v, &btn);
        if (!vk || !act) { printf("[controls] ignored line: %s = %s\n", k, v); continue; }
        if (act == KA_INTERACT) g_rfom_kbm_interact_bound = 1;
        if (s_kbm_n < (int)(sizeof s_kbm / sizeof s_kbm[0])) { s_kbm[s_kbm_n].vk = vk; s_kbm[s_kbm_n].act = act; s_kbm[s_kbm_n].btn = btn; s_kbm_n++; }
    }
    if (f) fclose(f);
    g_ml_deg_per_count = s_ml_deg * s_kbm_sens;
    g_ml_invert_y = s_kbm_invert;
    printf("[controls] %d bindings loaded from controls.txt (sens %.2f, deadzone %.2f, mouse look %s, %.3f deg/count)\n",
           s_kbm_n, s_kbm_sens, s_kbm_dz, g_ml_mode ? "direct" : "stick", g_ml_deg_per_count);
}

static u8 kbm_mouse_axis(double v)
{
    /* v: deflection fraction, signed. Past the deadzone, linear to full. */
    double a = fabs(v);
    if (a < 0.002) return 128;
    if (a > 1.0) a = 1.0;
    double o = s_kbm_dz + (1.0 - s_kbm_dz) * a;
    if (o > 1.0) o = 1.0;
    int iv = 128 + (int)((v < 0 ? -o : o) * 127.0);
    if (iv < 0) iv = 0; if (iv > 255) iv = 255;
    return (u8)iv;
}

/* v152: the game's controller screen lets the player swap which button does
 * what. It keeps the layout as 16 action -> button masks at the start of its
 * options block (found by project/stubs.cpp; game word = digital2 | digital1<<8).
 * Keys are bound to ACTIONS in the launcher, so a key's button is translated
 * through the default -> current table: a swap made on the controller screen
 * changes the controller, never what a key does. */
volatile u32 g_rfom_opts_ea = 0;
static const u16 s_def_layout[16] = { 0x0004, 0x0080, 0x0040, 0x0800, 0x1000, 0x4000, 0x0020, 0x0010,
                                      0x8000, 0x0008, 0x0001, 0x1000, 0x2000, 0x4000, 0x0002, 0x0400 };   /* as shipped (v152 log) */
static u32 kbm_unswap(u32 hsb)
{
    extern uint8_t* vm_base;
    const u32 o = g_rfom_opts_ea;
    if (!o || !vm_base || !hsb) return hsb;
    u16 cur[16];
    for (int i = 0; i < 16; i++) { const uint8_t* p = vm_base + o + 4 * i; cur[i] = (u16)((p[2] << 8) | p[3]); }
    /* game word: digital1 in the high byte, digital2 in the low byte with its
     * nibbles swapped (L2/R2/L1/R1 = 0x10/0x20/0x40/0x80, Tri/Circle/Cross/Square
     * = 0x01..0x08) -- a crouch (L2) <-> d-pad up swap moved 0x10 <-> 0x1000. */
#define NSW(x) ((u16)((((x) >> 4) & 0x0F) | (((x) & 0x0F) << 4)))
    const u16 g = (u16)(((hsb & 0xFF) << 8) | NSW((hsb >> 8) & 0xFF));
    u16 out = 0;
    for (int b = 0; b < 16; b++) {
        const u16 bit = (u16)(1u << b);
        if (!(g & bit)) continue;
        /* the slots the player moved define the swap (some buttons appear in
         * two slots, e.g. d-pad up for two actions; an unmoved twin must not hide it) */
        u16 to = bit;
        for (int i = 0; i < 16; i++)
            if (s_def_layout[i] == bit && cur[i] != bit && cur[i] && !(cur[i] & (cur[i] - 1))) { to = cur[i]; break; }
        out |= to;
    }
    return (u32)(((out >> 8) & 0xFF) | ((u32)NSW(out & 0xFF) << 8));
#undef NSW
}
static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;
    static int loaded = 0;
    if (!loaded) { loaded = 1; kbm_load(); }

    PadHostState* hs = &s_host_state[0];
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever; and give the mouse back. */
        hs->buttons = 0;
        g_rfom_use_kbm = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->trigger_l2 = hs->trigger_r2 = 0;
        hs->connected = 1;
        ClipCursor(NULL);
        InterlockedExchange(&g_kbm_mouse_dx, 0); InterlockedExchange(&g_kbm_mouse_dy, 0);
        InterlockedExchange(&g_kbm_wheel, 0);
        return;
    }

    /* Mouse capture toggle (edge-triggered). */
    { static int was = 0; int down = (GetAsyncKeyState(s_kbm_toggle_vk) & 0x8000) != 0;
      if (down && !was) { g_kbm_capture = !g_kbm_capture;
          printf("[controls] mouse %s\n", g_kbm_capture ? "captured" : "released"); }
      was = down; }
    if (g_kbm_capture && g_kbm_hwnd) {
        RECT rc; POINT c;
        if (GetClientRect((HWND)g_kbm_hwnd, &rc)) {
            c.x = (rc.left + rc.right) / 2; c.y = (rc.top + rc.bottom) / 2;
            ClientToScreen((HWND)g_kbm_hwnd, &c);
            RECT one = { c.x, c.y, c.x + 1, c.y + 1 };
            ClipCursor(&one);
        }
    } else ClipCursor(NULL);

    /* Wheel: each notch is a short press. */
    static int wheel_up_frames = 0, wheel_dn_frames = 0;
    { long w = InterlockedExchange(&g_kbm_wheel, 0);
      if (w > 0) wheel_up_frames = 4; else if (w < 0) wheel_dn_frames = 4; }

    u16 btns = 0; int lx = 0, ly = 0, krx = 0, kry = 0, interact = 0;
    for (int i = 0; i < s_kbm_n; i++) {
        int vk = s_kbm[i].vk, down;
        if (vk == KBM_WHEEL_UP) down = wheel_up_frames > 0;
        else if (vk == KBM_WHEEL_DOWN) down = wheel_dn_frames > 0;
        else if ((vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) && !g_kbm_capture) down = 0;
        else down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!down) continue;
        switch (s_kbm[i].act) {
        case KA_BTN: btns |= s_kbm[i].btn; break;
        case KA_LS_UP: ly -= 1; break;    case KA_LS_DOWN: ly += 1; break;
        case KA_LS_LEFT: lx -= 1; break;  case KA_LS_RIGHT: lx += 1; break;
        case KA_RS_UP: kry -= 1; break;   case KA_RS_DOWN: kry += 1; break;
        case KA_RS_LEFT: krx -= 1; break; case KA_RS_RIGHT: krx += 1; break;
        case KA_INTERACT: interact = 1; break;
        }
    }
    if (wheel_up_frames > 0) wheel_up_frames--;
    if (wheel_dn_frames > 0) wheel_dn_frames--;

    /* Mouse -> right stick: counts/second, smoothed, scaled so that
     * ~2500 counts/s (a brisk flick at 800 dpi) is full deflection. */
    static ULONGLONG last = 0; static double vx = 0, vy = 0;
    ULONGLONG now = GetTickCount64();
    double dt = last ? (double)(now - last) / 1000.0 : 0.016;
    if (dt < 0.001) dt = 0.001; if (dt > 0.1) dt = 0.1;
    last = now;
    long mdx = InterlockedExchange(&g_kbm_mouse_dx, 0), mdy = InterlockedExchange(&g_kbm_mouse_dy, 0);
    if (!g_kbm_capture) mdx = mdy = 0;
    g_kbm_mouse_abs += (unsigned long long)(labs(mdx) + labs(mdy));   /* v159: grab escape meter */
    /* v160: menus (front end, ResistanceFoM.exe) on keyboard & mouse. The
     * front end only knows the d-pad and Cross/Circle, so: moving the mouse
     * steps the highlight (one d-pad press per menu_mouse_step counts), the
     * wheel and arrow keys/WASD step too, left click / Enter / Space = select
     * (Cross; Enter also Start), right click / Esc / Backspace = back (Circle).
     * The in-game bindings are not used here. menu_mouse = 0 in controls.txt
     * turns it off. */
    { static int fe = -1;
      if (fe < 0) { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
                    const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
                    fe = (!_stricmp(b, "ResistanceFoM.exe") && s_menu_mouse) ? 1 : 0;
                    if (fe) printf("[controls] menu mode: WASD / arrows move, Space / Enter select, Esc / Backspace back\n"); }
      if (fe) {
        static long acc_x = 0, acc_y = 0; static int pend_x = 0, pend_y = 0;
        static ULONGLONG ph_t = 0; static int ph = 0; static u16 ph_btn = 0;   /* pulse: 0 idle, 1 down, 2 gap */
        if (s_menu_mouse >= 2) { acc_x += mdx; acc_y += mdy; }   /* v160b: stepping by mouse motion off (menu_mouse = 2 turns it on) */
        const long st = s_menu_step > 10 ? s_menu_step : 150, stx = st * 2;
        while (acc_y >=  st) { acc_y -= st; if (pend_y < 3) pend_y++; }
        while (acc_y <= -st) { acc_y += st; if (pend_y > -3) pend_y--; }
        while (acc_x >=  stx) { acc_x -= stx; if (pend_x < 3) pend_x++; }
        while (acc_x <= -stx) { acc_x += stx; if (pend_x > -3) pend_x--; }
        if (ph == 0 && (pend_y || pend_x)) {
            if (pend_y) { ph_btn = pend_y > 0 ? CELL_PAD_CTRL_DOWN : CELL_PAD_CTRL_UP; pend_y += pend_y > 0 ? -1 : 1; acc_x = 0; }
            else { ph_btn = pend_x > 0 ? CELL_PAD_CTRL_RIGHT : CELL_PAD_CTRL_LEFT; pend_x += pend_x > 0 ? -1 : 1; }
            ph = 1; ph_t = now;
        }
        if (ph == 1 && now - ph_t >= 45) { ph = 2; ph_t = now; }
        if (ph == 2 && now - ph_t >= 45) ph = 0;
        u16 m = 0;
        #define KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
        if (KD(VK_RETURN)) m |= CELL_PAD_CTRL_CROSS | CELL_PAD_CTRL_START;
        if (KD(VK_SPACE)) m |= CELL_PAD_CTRL_CROSS;
        if (KD(VK_ESCAPE) || KD(VK_BACK)) m |= CELL_PAD_CTRL_CIRCLE;
        if (KD(VK_UP) || KD('W')) m |= CELL_PAD_CTRL_UP;            /* v160i: keyboard only (user) */
        if (KD(VK_DOWN) || KD('S')) m |= CELL_PAD_CTRL_DOWN;
        if (KD(VK_LEFT) || KD('A')) m |= CELL_PAD_CTRL_LEFT;
        if (KD(VK_RIGHT) || KD('D')) m |= CELL_PAD_CTRL_RIGHT;
        #undef KD
        if (ph == 1) m |= ph_btn;
        btns = m; lx = ly = krx = kry = 0; interact = 0;
        mdx = mdy = 0;                       /* the mouse is the highlight here, not a stick */
      } }
    double ix = (double)mdx / dt, iy = (double)mdy / dt;
    double al = s_kbm_smooth > 0 ? 1.0 - exp(-dt / s_kbm_smooth) : 1.0;
    vx += (ix - vx) * al; vy += (iy - vy) * al;
    double fx = vx * s_kbm_sens / 2500.0, fy = vy * s_kbm_sens / 2500.0;
    if (s_kbm_invert) fy = -fy;

    /* v162: in-game menus (pause etc.) only read the d-pad and Cross/Circle, but WASD are
     * bound to the left stick. While the in-play look code isn't running (paused, menus),
     * WASD / arrows also press the d-pad, Enter = Cross, Backspace = Circle. Esc stays Start. */
    int menu_raw = 0;
    { static int fe2 = -1;
      if (fe2 < 0) { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
                     const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
                     fe2 = _stricmp(b, "ResistanceFoM.exe") ? 1 : 0; }
      const int menu = fe2 && g_rfom_play_ms && now - g_rfom_play_ms > 200;
      { static int was = -1;
        if (menu != was) { was = menu; static int n = 0;
          if (n++ < 200) { FILE* f = fopen("logs\\menumode.txt", n == 1 ? "w" : "a");
                           if (f) { fprintf(f, "t=%llu menu=%d space_start=%d pause_ui=%d\n", now, menu, (int)g_rfom_space_start, (int)g_rfom_pause_ui); fclose(f); } } } }
      static int s_esc_armed = 0;
      if (!menu) s_esc_armed = 0;
      if (menu) {
        #define KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
        if (KD(VK_UP) || KD('W')) btns |= CELL_PAD_CTRL_UP;
        if (KD(VK_DOWN) || KD('S')) btns |= CELL_PAD_CTRL_DOWN;
        if (KD(VK_LEFT) || KD('A')) btns |= CELL_PAD_CTRL_LEFT;
        if (KD(VK_RIGHT) || KD('D')) btns |= CELL_PAD_CTRL_RIGHT;
        if (KD(VK_RETURN)) btns |= CELL_PAD_CTRL_CROSS;
        if (KD(VK_BACK)) btns |= CELL_PAD_CTRL_CIRCLE;
        /* v162c: in the pause menu Esc = back (O), like G; on the top page O also resumes.
         * The Esc press that opened the pause must be let go first. */
        if (!KD(VK_ESCAPE)) s_esc_armed = 1;
        if (g_rfom_pause_ui) {
            btns &= (u16)~CELL_PAD_CTRL_START;
            if (s_esc_armed && KD(VK_ESCAPE)) btns |= CELL_PAD_CTRL_CIRCLE;
        }
        #undef KD
        lx = ly = 0;   /* the stick would fight the d-pad on some screens */
        menu_raw = 1;  /* v162b: menus read raw buttons -- the controller-screen swap (e.g. crouch <-> d-pad up) must not apply */
      } }
    if (g_rfom_space_start && (GetAsyncKeyState(VK_SPACE) & 0x8000)) btns |= CELL_PAD_CTRL_START;   /* v160v */
    /* v186: the front end's first-run screens (initial setup, notices) say
     * "Press the SPACE bar" -- the relabelled Start prompt -- but only Enter
     * worked there (new-player test). In the front-end exe Space also presses Start. */
    { static int fe3 = -1;
      if (fe3 < 0) { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
                     const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
                     fe3 = _stricmp(b, "ResistanceFoM.exe") ? 0 : 1; }
      if (fe3 && (GetAsyncKeyState(VK_SPACE) & 0x8000)) btns |= CELL_PAD_CTRL_START; }
    hs->buttons   = menu_raw ? btns : kbm_unswap(btns);   /* v152: undo the in-game button swaps for the keyboard */
    g_rfom_use_kbm = interact;
    hs->connected = 1;
    hs->analog_lx = (u8)(lx < 0 ? 0 : lx > 0 ? 255 : 128);
    hs->analog_ly = (u8)(ly < 0 ? 0 : ly > 0 ? 255 : 128);
    if (krx || kry) {
        hs->analog_rx = (u8)(krx < 0 ? 0 : krx > 0 ? 255 : 128);
        hs->analog_ry = (u8)(kry < 0 ? 0 : kry > 0 ? 255 : 128);
    } else {
        hs->analog_rx = kbm_mouse_axis(fx);
        hs->analog_ry = kbm_mouse_axis(fy);
    }
    g_ml_fed_rx = (int)hs->analog_rx - 128; g_ml_fed_ry = (int)hs->analog_ry - 128;
    g_ml_rs_keys = (krx || kry); g_ml_kbm_ms = GetTickCount64();
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);
    hs->press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
    hs->press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    hs->press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    hs->press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    hs->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    hs->press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    hs->press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    hs->press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    hs->press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
    hs->press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard+mouse active on port 0 (no XInput device)\n");
          fflush(stdout); } }
}
#endif

static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;
        if (i == 0) g_rfom_pad_btns0 = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Send port i's rumble state to its controller, with a fresh duration. */
static void pad_sdl_rumble_send(int i)
{
    SDL_GameController* gc = s_sdl_controllers[i];
    if (!gc) return;
    PadRumble* r = &s_rumble[i];
    const int on = r->lo || r->hi;
    SDL_GameControllerRumble(gc, r->lo, r->hi, on ? PAD_RUMBLE_HOLD_MS : 0);
    r->sent_ms = on ? pad_now_ms() : 0;
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            memset(&s_rumble[i], 0, sizeof s_rumble[i]);
            continue;
        }

        s_host_state[i].connected = 1;

        /* Keep a rumble the title set going (see PAD_RUMBLE_HOLD_MS). */
        if (s_rumble[i].sent_ms && pad_now_ms() - s_rumble[i].sent_ms >= PAD_RUMBLE_RENEW_MS)
            pad_sdl_rumble_send(i);

        u16 btns = 0;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerRumble(s_sdl_controllers[i], 0, 0, 0);
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
    memset(s_rumble, 0, sizeof s_rumble);
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
#ifdef _WIN32
    { static int once = 0; if (!once) { once = 1; ctl_extra_load(); } }
    if (!s_host_state[0].connected) g_rfom_pad_btns0 = 0;
    g_rfom_input_kbm = !s_host_state[0].connected;
    if (!s_host_state[0].connected) pad_poll_keyboard();
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

volatile u32 g_pad_data_ea0 = 0;   /* v147: guest address of port 0's last GetData buffer (pad watch) */
s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    if (port_no == 0 && data_guest) g_pad_data_ea0 = (u32)(uintptr_t)data_guest;
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];
    /* Resistance port: remote control from the debug console ("pad ..."):
     * buttons and both sticks held until a deadline, on port 0. */
    PadHostState inj;
    { extern volatile unsigned long long g_pad_inj_until; extern volatile u32 g_pad_inj_btn, g_pad_inj_sticks;
      extern unsigned long long ps3_ms_now(void);
      if (port_no == 0 && g_pad_inj_until && ps3_ms_now() < g_pad_inj_until) {
          inj = *hs; const u32 st = g_pad_inj_sticks;
          inj.buttons |= (u16)g_pad_inj_btn; inj.connected = 1;
          inj.analog_lx = (u8)(st >> 24); inj.analog_ly = (u8)(st >> 16);
          inj.analog_rx = (u8)(st >> 8);  inj.analog_ry = (u8)st;
          hs = &inj;
      } }

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second (or flip, under PAD_CLOCK=flips), each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[256]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 256) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)%s\n", n_ev,
                     *p ? " -- TRUNCATED at 256, the rest are ignored" : "");
          }
      }
      if (s_sc && port_no == 0) {
          double now = pad_clock(t0);
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[256], held[256];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[256];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[64]; buf[0] = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, 0, 0);
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && pad_clock(s_t0) >= s_from && pad_clock(s_t0) < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 2))
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

    if (port_no >= PAD_MAX_HOST_PORTS)
        return CELL_OK;

    u16 lo, hi;
    pad_rumble_levels(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL],
                      param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE], &lo, &hi);
    s_rumble[port_no].lo = lo;
    s_rumble[port_no].hi = hi;

#if PAD_BACKEND_XINPUT
    /* XInput keeps the motors at what they were last set to: no renewal. */
    if (s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)lo;
        vib.wRightMotorSpeed = (WORD)hi;
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    pad_sdl_rumble_send((int)port_no);
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
