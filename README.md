# Resistance: Fall of Man: native PC port

A static recompilation of the PS3 game *Resistance: Fall of Man* into a native Windows program. Nothing is emulated: the game's PowerPC and SPU code is translated to C/C++ and compiled for your PC. It renders through its own Direct3D 12 renderer, with NVIDIA DLSS / DLAA and AMD FSR 3.1.

> **This repository contains no game files and no game code.** You need your own copy of the game. Setup builds everything game-specific on your PC from that copy.

## Highlights

- **60 fps and higher** (30 to 165, *Just under refresh* for G-Sync / FreeSync, or unlimited). Game speed is correct at every frame rate.
- **NVIDIA DLSS / DLAA and AMD FSR 3.1**, render resolutions up to 4K, and a *1080p → DLSS 4K* preset for RTX 40 / 50 cards.
- **Native PC controls:** raw mouse look (not stick emulation), rebindable keys, and separate melee and interact keys. Xbox and PlayStation controllers work and can be swapped at any time.
- **Button prompts follow your device:** your keyboard keys, or Xbox buttons when an Xbox controller is in use.
- **In-game PC menu** (F10, or hold Esc / Start): Resume, PC Options, and **Quit to Desktop** with a confirmation. No Alt+F4 needed.
- **Ultrawide (Hor+)**, field of view slider, performance overlay (F3), screenshots (F12), mods folder, and saves in *Saved Games*.
- **Launcher** that detects your hardware and picks recommended settings.
- **Performance work:** the renderer was profiled down to about 5 µs of CPU per draw (see *Performance*).

## What you need

- A PC running Windows 10 or 11 (64-bit).
- A CPU with AVX2 (Intel 4th generation or newer, or any AMD Ryzen).
- A DirectX 12 GPU. It has been tested on AMD (RX 6700 XT) and NVIDIA (RTX 5070) cards. DLSS needs a GeForce RTX card; every other GPU uses FSR 3.1.
- Your own dump of **Resistance: Fall of Man, US disc (BCUS98107), version 1.00**: the folder that contains `PS3_GAME`, the same kind of folder RPCS3 uses. Other regions and patched versions are not supported yet.
- About 25 GB of free disk space: 16 GB for the game files, the rest for the build tools and the build.
- An internet connection for setup.

## Install

1. Download this repository (**Code → Download ZIP**) and unzip it to a path without spaces, for example `C:\Games\ResistancePort`.
2. Double-click **`Setup.bat`**. It:
   - installs the free build tools if they're missing (Visual Studio 2022 Build Tools, LLVM, CMake, Ninja, Python) using `winget`;
   - asks for your game folder, then copies it into the port folder (or moves it, if it's on the same drive);
   - decrypts the game's two program files using the application keys from RPCS3's public source code;
   - downloads the DLSS SDK and FFmpeg;
   - generates the game code from your copy and builds the game.

   The first run takes a while: installing the tools, plus a 20 to 60 minute build.
3. Play with **`Launcher.exe`**. On the first start it detects your hardware and picks recommended settings; everything is adjustable there.

Run Setup again only after downloading a newer version of the port.

**First launch:** the game shows its one-time "initial setup" screens. The first time you load a level, the world can stay black for about 20 seconds while shaders compile. After that, the shader cache is kept, including a backup that survives updates and reinstalls.

## Controls

Keyboard and mouse, Xbox / XInput controllers and PlayStation pads (through XInput) all work, and you can switch between them at any time.

- **Button prompts follow the last device you used.** On keyboard and mouse the game shows your key bindings. On an Xbox / XInput controller it shows Xbox buttons that match the real mapping: Cross = **A**, Circle = **B**, Square = **X**, Triangle = **Y**, L1/R1 = **LB/RB**, L2/R2 = **LT/RT**, L3/R3 = **LS/RS**, Select = **View**, Start = **Menu**. The Xbox glyphs are drawn by the port (coloured circles with letters); no Microsoft artwork is used. If the pad identifies itself as a PlayStation controller, the PlayStation symbols stay. `RFOM_PAD_GLYPHS=auto|xbox|ps` overrides the choice (default `auto`).
- **Controller hot-plug:** controllers are polled every few milliseconds, so plugging one in or pulling it out works at any time without a restart. The log shows `[cellPad] controller N connected` / `disconnected`.
- **Mouse look is native.** Raw mouse movement (Windows raw input) turns the player's camera directly, 1:1 on foot; it is not converted into an analog stick. Scoped weapons scale it by their zoom.
- Melee and interact are split onto separate keys (on the PS3 they shared a button).
- Bindings are edited in the launcher (Controls page) or by hand in `controls.txt` (`KEY = ACTION`, one per line; the header of the file lists every key and action name).

Mouse and controller options in `controls.txt`:

| Option | Default | What it does |
|---|---|---|
| `mouse_look` | `direct` | `direct`: raw mouse deltas turn the camera. `stick`: the old analog-stick emulation. |
| `mouse_sensitivity` | `2.00` | Overall mouse speed (also multiplied by the game's own Look Sensitivity). |
| `mouse_degrees_per_count` | `0.050` | Degrees turned per mouse count in direct mode, before `mouse_sensitivity`. |
| `mouse_invert_y` | `0` | `1` inverts vertical look. |
| `mouse_deadzone`, `mouse_smoothing` | `0.28`, `0.03` | Only for `mouse_look = stick`. |
| `mouse_release_key` | `F8` | Frees / recaptures the mouse cursor. |
| `pad_interact` | `NONE` | A controller button that only uses things (doors, switches, vehicles); the Action button then only melees. `NONE` = as on PS3. |

More input options (launcher → Controls page, written to `settings.bat`; log line at start: `[controls] PC options: vertical mouse x.., stick deadzone .., curve .., zoom hold|toggle (game)`):

| Launcher setting | Choices (default) | Environment variable | What it does |
|---|---|---|---|
| Vertical sensitivity | 50%, 75%, **Same as horizontal**, 125%, 150%, 200% | `PAD_MOUSE_SENS_Y=0.1..4` | Up/down mouse speed relative to left/right (direct and stick mouse look). |
| Zoomed sensitivity | 50%, 75%, **Same**, 125%, 150% | `RFOM_ADS_SENS=0.1..3` | Extra mouse multiplier while zoomed or scoped, on top of the automatic zoom matching (`RFOM_ZOOM_SENS`). |
| Zoom | **Toggle** (PS3), Hold | `PAD_AIM_HOLD=1` | Hold: zoomed only while the zoom key / R3 is held. The game's zoom is a toggle, so the port presses zoom again when you let go (keyboard, mouse and the first controller). |
| Stick deadzone | **Default** (24%, per axis, as on PS3), 5-30% | `PAD_DEADZONE=0..0.9` | A radial deadzone for both controller sticks, rescaled so the full range stays usable. Raise it if the view drifts. |
| Stick response | **Linear**, Fast, Precise, Very precise | `PAD_CURVE=0.5..3` (exponent; 1 = linear) | Above 1: small movements are slower for fine aiming; below 1: quicker. A curve other than 1 also switches the sticks to a radial 24% deadzone when none is chosen. |

"Restore default controls" also resets these.

### Keys that always work

| Key | What it does |
|---|---|
| **Alt + Enter** | Switch between borderless fullscreen and a window. |
| **F8** | Free / capture the mouse cursor (changeable with `mouse_release_key`). |
| **F10**, or hold **Esc** for 1 s, or hold **Start / Menu** for 1.5 s | **PC menu**: Resume / PC Options / Quit to Desktop (see *Window, focus and quitting*). It always opens on Resume (the mouse takes over the highlight only once moved). The game is paused and muted while it is up, and its SPU threads idle instead of spinning. |
| **F12** or **PrtScn** | Screenshot: a PNG in the `screenshots` folder (`RFOM_YYYYMMDD_HHMMSS_mmm.png`); a "Screenshot saved" toast shows briefly. |
| **F3** | Performance overlay (see *Performance overlay*). |
| **Alt + F4** / the window's close button | Also quits cleanly (waits for a save in progress). |

## Launcher settings

`Launcher.exe` writes `settings.ini` (its own choices) and `settings.bat` (the environment variables the game reads); **Play** runs `run.bat`, which loads `settings.bat`. On the first start (no `settings.ini` yet) the launcher detects your hardware and fills in the recommended settings. Every list marks the value recommended for your PC with "★ recommended".

**Display page**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Display mode | **Borderless fullscreen**, Windowed | `RSX_DISPLAY=borderless\|windowed` | Alt+Enter switches in game. |
| Window size | 1280x720 … 3840x2160 (**1920x1080**) | `RSX_WINDOW=WxH` | Windowed mode only. |
| Monitor | **Main display**, Monitor 2-4 | `RSX_MONITOR=0\|2\|3\|4` | Numbered in Windows' order, main display first. The log shows `[display] monitor n of m`. |
| Frame rate limit | 30, 55, **60**, 72, 82, 90, 120, 144, 165, Just under refresh (G-Sync / FreeSync), Unlimited | `CHILD_RFOM_FPS` (→ `RFOM_FPS`, `vrr` = just under refresh) | Game speed is the same at every limit (see *Frame rate* below). |
| Menu frame rate | 30, 60, **90**, Same as the game | `RFOM_MENU_FPS` (0 = same as the game) | Cap while the pause / options menu is up over the game. Above ~90 fps the pause menu's dimming layer can flicker. |
| VSync | **Off**, Every refresh, Every 2nd, Every 3rd | `RSX_VSYNC=0..3` | Off is smoothest with FreeSync / G-Sync. |
| Tearing | **Allowed** (lowest latency, VRR), Never | `RFOM_PRESENT=tearing\|notear` | With VSync off: Allowed presents immediately (with G-Sync / FreeSync the screen follows the frame rate); Never drops late frames instead of tearing (`mailbox` / `fifo` are accepted as synonyms). Log: `[D3D12] present: ...`. |
| Aspect ratio | **Auto** (window shape), 16:9 (PS3), 16:10, 21:9, 32:9 | `RFOM_ASPECT=auto\|off\|16:10\|21:9\|32:9\|<a:b>\|<decimal>` | Ultrawide / Hor+; see *Ultrawide* below. |
| In the background | **Pause and mute**, Mute only, Keep running | `RSX_FOCUS_PAUSE=0\|1`, `RSX_FOCUS_MUTE=0\|1` | What happens on Alt+Tab or a click on another window. Pausing frees the CPU/GPU, releases the mouse and resumes exactly where you were. |
| Quit keys | **On**, Off (Alt+F4 only) | `RFOM_QUIT_PROMPT=1\|0` | F10 / hold Esc / hold Start → the PC menu (Resume / PC Options / Quit to Desktop). |
| Save location | **Saved Games folder**, Game folder (portable) | `RFOM_SAVE_DIR=` (empty) \| `portable` \| a folder path | See *Saves* below. |
| Controller button prompts | **Auto**, Xbox, PlayStation | `RFOM_PAD_GLYPHS=\|xbox\|ps` | Which buttons the game shows with a controller (keyboard and mouse always show your keys). Also in the PC menu. |
| Performance overlay | **Off** (F3 shows it), On at start, Disabled | `RFOM_OVERLAY=\|1\|0`, `RFOM_OVERLAY_POS=tl\|tr\|bl\|br` (default `tl`) | See *Performance overlay*. |
| Mods folder | **Off**, On | `RFOM_MODS=0\|1\|<folder>` | See *Mods*. |

Buttons: **Reset to defaults** (asks first; puts every Display and Graphics setting back to the recommended defaults for this PC) and **Open save folder**.

**Graphics page**

At the top: what was detected on this PC (GPU, VRAM, CPU cores/threads, RAM, screen), the recommended configuration, and **Auto-detect (recommended)**, which applies it.

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Render resolution | 1280x720 (PS3), 1920x1080, **2560x1440**, 3200x1800, 3840x2160 | `RSX_RES_SCALE=1\|1.5\|2\|2.5\|3` (x 720p) | Above your screen's resolution it acts as supersampling. |
| Anti-aliasing | **Off**, AMD FSR 3.1, NVIDIA DLSS / DLAA | `RSX_FSR=0\|1\|2` | DLSS needs a GeForce RTX card; if it cannot start the game uses FSR 3.1, and if that fails too, plain bilinear scaling. |
| Upscaling | **Native**, Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33% | `RSX_UPSCALE=1\|0.667\|0.58\|0.5\|0.333` | With FSR / DLSS on, the 3D scene is drawn at that share of the render resolution and rebuilt by FSR / DLSS. The menus and HUD stay at full resolution. |
| Texture filtering | Trilinear (PS3), 2x, 4x, 8x, **16x** anisotropic | `RSX_ANISO=0\|2\|4\|8\|16` | |
| Scaling | **Smooth**, Sharp pixels | `RSX_FILTER=linear\|point` | How the final image is fitted to the screen. |
| Sharpening | Off, **Low**, Medium, High | `RSX_SHARPEN=0\|0.25\|0.5\|0.8` | |
| Field of view | slider 60-150 (default 82, the game's own) | `RFOM_FOV=<degrees>` | Gameplay camera FOV. |

**Controls page:** key bindings, the mouse options and the input options above. **About page:** the key list, **Open save folder**, **Open screenshots**, the logs folder and the project page.

### Hardware presets (auto-detect)

The launcher's recommendation (also available without the launcher with `RFOM_PRESET`) picks the discrete GPU if there is one and maps it to a tier:

| Tier | Typical GPU | Render (output) | Anti-aliasing / upscaling | Filtering |
|---|---|---|---|---|
| Low | integrated graphics, < 1.5 GB cards | 720p | off | 4x |
| Low | GTX 1050 / RX 560 / Radeon 780M, 1.5-3.5 GB | 1080p | DLSS (RTX) or FSR 3.1, Quality (67%) | 8x |
| Medium | GTX 1060-1660 / RX 580 / RTX 3050, 3.5-7 GB | your screen, up to 1440p | DLSS or FSR 3.1 (Quality at 1440p, native AA below) | 16x |
| High | RTX 2070-4060 / RX 6600-7700, 7 GB+ | your screen | DLSS or FSR 3.1 (Performance on 4K screens, Quality at 1440p, native AA at 1080p) | 16x |
| **Ultra** | **RTX 4070 / 5070 or better, or RTX 3080+, with 10 GB+**; RX 6800 / 7800 or better with 12 GB+ | **3840x2160** | **DLSS Performance on RTX** (scene at 1080p → 4K), FSR 3.1 Performance otherwise | 16x |

The discrete GPU is preferred on laptops. Sharpening is Low (Medium for 720p on a larger screen). Frame rate: 60 fps (30 on CPUs with 4 threads or fewer); VSync off.

`RFOM_PRESET=auto|low|medium|high|ultra` fills in only the graphics variables you have **not** set yourself (running through the launcher sets them all, so the preset is for running `build\ResistanceFoM.exe` directly). The log shows `[preset] ...`.

### 1080p rendered → DLSS 4K output (recommended for RTX 40 / 50 cards)

The scene is drawn at 1920x1080, DLSS (Performance) rebuilds it at 3840x2160, and the 4K image is scaled down to your screen, which is much sharper than native 1080p on a 1080p monitor. The HUD and menus are drawn at 4K.

- **Launcher:** Graphics → Auto-detect (recommended) on an RTX 4070 / 5070-class card, or by hand: Render resolution **3840 x 2160**, Anti-aliasing **NVIDIA DLSS / DLAA**, Upscaling **Performance (50%)**.
- **Environment variables:** `RSX_RES_SCALE=3`, `RSX_FSR=2`, `RSX_UPSCALE=0.5` (or `RFOM_PRESET=ultra`).
- DLSS history is reset only on real scene changes (camera cuts, level loads); if DLSS cannot start, FSR 3.1 is used, then plain scaling.

## Frame rate

The game advances its simulation by the real time each frame took, so the frame rate limit (30 / 60 / 120 / 144 / Unlimited …) does **not** change game speed. Details:

- `RFOM_FPS=<n>` (the launcher writes `CHILD_RFOM_FPS`): target frame rate. `0` = uncapped (240), `orig` = the game's own pacing (as on PS3). Values below 30 are raised to 30.
- Scripted sequences that are timed in frames (a chapter's opening walk, for example) run at the original 30 fps steps automatically, then the full rate returns with control (`RFOM_SCRIPT30=0` disables this).
- `RFOM_DT_MAX=<seconds>` raises the largest time step (e.g. `0.05` keeps real speed down to 20 fps; otherwise the game slows down below 30 fps, as on the PS3).
- `RFOM_FPS=vrr` (launcher: *Just under refresh*): caps just below the refresh rate of the monitor the game window is on (refresh − refresh²/3600: 138 fps on 144 Hz, 157 on 165 Hz, 58 on 60 Hz), so a G-Sync / FreeSync screen stays in its variable-refresh range without VSync's latency. Use it with VSync Off. Log: `[fps] RFOM_FPS=vrr: 144 Hz monitor -> 138 fps cap`.
- `RFOM_MENU_FPS` caps menus over the game (default 90; `0` = off).
- Log: `[fps] target 60 fps: ...`.

## Window, focus and quitting

- **Alt+Enter** toggles borderless fullscreen / windowed. **Alt+Tab** is safe: with "In the background" = Pause, the game stops (no CPU / GPU use), mutes, releases the mouse cursor and continues exactly where it was when you come back. The log shows `[focus] paused (window in the background)` / `[focus] resumed`.
- The mouse is released whenever the window loses focus, and with **F8**. The cursor is only hidden while the game has it captured.
- **PC menu:** F10, holding Esc for 1 s or holding Start / Menu for 1.5 s opens the PC menu over the game (from the main menu, the pause menu or gameplay): **Resume**, **PC Options**, **Quit to Desktop**. Arrows / W S A D or the d-pad / left stick select and change, **Enter / Space / A** = OK, **Esc / Backspace / B** = back, **F10 / Start** = close; the mouse works too (left click = OK / next value, right click = previous value). The game is paused and muted, and does not see the keys, while it is up.
  - **PC Options**: *Frame rate limit* (30 … 165, Unlimited), *Anti-aliasing* (Off / AMD FSR 3.1 / NVIDIA DLSS), *Mouse sensitivity* (0.10–4.00), *Field of view* (82 PS3 / 90 / 100 / 110 / 120), *Button prompts* (Auto / Xbox / PlayStation). Mouse sensitivity, field of view and button prompts apply at once (the FOV as soon as the game next sets the camera's view — at the latest after a zoom, a checkpoint or a level load); frame rate and anti-aliasing are marked `*` and apply the next time the game starts. Changes are saved straight to `settings.ini`, `settings.bat` and `controls.txt`, so the launcher shows them too.
  - A small hint box (*PC menu: F10 or hold Esc / Start*) appears at the bottom while the game's own pause menu is up, and once ~12 s after start-up. `RFOM_MENU_PC_HINT=0` hides it.
  - `RFOM_MENU_PC=0` turns the PC menu off: the keys then open only the "Quit to desktop?" box.
- **Quit to desktop** (PC menu): a small "Quit to desktop?" box (**Enter / A** = yes, **Esc / B** = back to the PC menu). On yes (or Alt+F4 / the close button) the port waits until no save is in progress (up to 30 s), flushes the logs and exits. Log: `[quit] prompt shown`, `[quit] ... waiting for saves`, `[quit] clean exit`. `ESC_CLOSES=1` makes a single Esc quit at once (debug).
- Entries inside the game's own menus are not possible: the menus are scripted scene files of the original game, not code the port can extend; hence the PC menu is an overlay opened with the keys / long presses above, with the hint box shown alongside the pause menu.
- **Screenshots:** F12 or PrtScn saves the presented frame (with HUD) as a PNG in `screenshots\`. Log: `[screenshot] saved ...`. (The old RSXCAP debug capture on F12 now needs `RSX_CAP_F12=1`.)
- **Intro:** the game has no separate logo videos to skip. The opening front-end sequence is drawn by the game itself and can be skipped as usual with Enter / Space / Cross.

## Ultrawide (Hor+)

`RFOM_ASPECT` (launcher: Display → Aspect ratio; default `auto`):

- `auto` follows the shape of the game window (checked twice a second, so resizing a window or Alt+Enter is followed). On a 16:9 window nothing changes.
- `off` / `0` keeps the PS3's 16:9. `21:9`, `32:9`, `16:10`, any `a:b` or a decimal (1.2-4) force an aspect.
- How it works: the game's camera setter is given the wider aspect and a horizontal field of view widened by the same ratio, so the **vertical** view stays as on PS3 and the sides open up (Hor+); the game's culling follows the wider frustum. Narrower screens (16:10) keep the horizontal view and gain height. The level-of-detail fields keep their 16:9 values, so distant models don't switch to far LODs. Your field-of-view setting applies first and Hor+ on top.
- The image the game draws keeps its 16:9 pixel grid and is stretched to the screen's shape at present time, so on a 21:9 screen each pixel is ~1.3x wider. With DLSS / FSR output at 3840 px (the 1080p → 4K preset), a 2560x1080 or 3440x1440 screen still gets more horizontal pixels than it has.
- Only 16:9 cameras are widened. Movies, loading screens and other 2D screens (no 3D camera for 0.5 s) keep their 16:9 picture with black bars.
- **The HUD is stretched with the view** (it is not kept in a centred 16:9 area): the renderer cannot reliably tell the game's HUD draws from its 3D draws, so they are scaled together.
- Log: `[aspect] RFOM_ASPECT: auto (window aspect)`, `[aspect] window aspect 2.370 (2560x1080): Hor+ on 16:9 cameras`, `[aspect] Hor+: 16:9 camera (aspect 1.778) -> 2.370, horizontal FOV tan x1.333`, then `[aspect] Hor+ camera set N times/s`.

## Performance overlay

**F3** toggles a small overlay in a corner of the game window: the game's frame rate and frame time (with the worst frame of the last half second), the renderer's frame rate, draws per frame, and the CPU time per draw in µs (the same measurement as `RSX_DRAWPROF`, refreshed twice a second; measuring costs a little while the overlay is up and nothing while it is hidden). `RFOM_OVERLAY=1` shows it from the start, `RFOM_OVERLAY=0` disables F3; `RFOM_OVERLAY_POS=tl|tr|bl|br` picks the corner. It is a separate always-on-top window, so it does not appear in screenshots.

## Mods

`RFOM_MODS=1` (launcher: Display → Mods folder → On) loads replacement game files from the `mods` folder in the port's folder (`RFOM_MODS=<folder>` uses any other folder):

```
mods  My Texture Mod    USRDIR\<path of the file inside the game's USRDIR>
  Another Mod    USRDIR\...
```

- A file replaces the game file with the same path (case does not matter). Every file access of the game goes through the check (cellFs, sys_fs and the port's own loaders), and only files you add are changed; nothing in `gamefiles` is touched.
- Mods load in alphabetical order; when two mods have the same file, the later one wins (the log says so). A folder starting with `_` or `-` is skipped (an easy way to turn a mod off).
- Log: `[mods] <mod>: N file(s)`, `[mods] override: usrdir/... -> <file>`, `[mods] N mod(s), M file override(s) from <folder>` at start, and `[mods] using <file>` the first time the game opens each replaced file.

## Trophies

Resistance: Fall of Man (2006) has no trophies: the game does not use the trophy library and its disc has no trophy data, so the trophy tracker, toasts and launcher page of the Resistance 2 port do not apply here.

## Saves

- Saves live in **`%USERPROFILE%\Saved Games\Resistance Fall of Man`** by default, so updating or reinstalling the port can't touch them. Your save files are in `...\dev_hdd0\home\00000001\savedata\` (`BCUS98107_*` folders).
- On the first start after this change, existing saves in the port's `gamedata` folder are **copied** there (nothing is overwritten; the old copy stays as a backup). Log: `[saves] folder: ...` and `[saves] copied N save file(s) ...`.
- `RFOM_SAVE_DIR=portable` (launcher: Save location → Game folder) keeps everything in `<port>\gamedata`; `RFOM_SAVE_DIR=<folder>` uses any folder; `PS3_GAMEDATA_ROOT` (advanced) overrides both.
- Launcher → **Open save folder** opens it. The game's own options are stored in the save profile, so they move with it.

## Performance

Measured in York (about 2,600 draws per frame), DLSS rendering at 1080p with 4K output, on an RTX 5070: **a locked 60 fps at about 5 µs of CPU per draw**. Performance fixes have been applied as far as realistically possible, all profile-driven. Low-end PCs benefit most, because the per-draw cost is CPU time.

Performance fixes have been applied as much as possible, profile-driven, and carried over from the Resistance 2 port where they fit R1's own Direct3D 12 renderer:

- **Renderer (per draw):** a redundant-state filter (draws that change nothing skip the state re-apply), fragment-constant patch memoisation, pipeline (PSO) reuse in the pre-pass, cached descriptors, a disk shader / pipeline cache with background compilation and a backup in `%LOCALAPPDATA%\ResistancePort\shadercache`.
- **SPU / PPU runtime:** a fast SPU wait loop (short spin, then sleep), yielding instead of spinning on CPUs with fewer than 8 threads, priority boosts and core pinning only where there are enough cores, a high-resolution sleep timer, and the network HTTP poll (`cellHttpClientPollConnections`) implemented so the HTTP thread no longer busy-spins.
- **Performance overlay:** F3 (see *Performance overlay*).
- **Per-draw profiler:** `RSX_DRAWPROF=1` logs the CPU cost per draw by phase every 5 s:
  `[drawprof] 5.0 s: N draws (... guest draw calls, ... tex binds), D draws/frame, F fps | X us/draw = record (verts, consts, tex binds) + prepass (srv, pso, tex upload) + replay (rt, pso, state, draw)`. The target is under 6 µs per draw.

Performance switches (defaults are the recommended values; set them before `run.bat` or in `settings.bat`):

| Variable | Default | What it does |
|---|---|---|
| `RSX_DRAWPROF` | `0` | `1` = per-draw CPU profile in the log every 5 s. |
| `RSX_DRAW_FILTER` | `1` | Redundant-state filter. Log: `[draw] redundant state filter on`. |
| `RSX_FPK_MEMO` | `1` | Fragment-program constant memoisation. |
| `RSX_SHADERCACHE`, `RSX_PSO_ASYNC` | `1` | Disk shader cache; background pipeline compilation. |
| `SPU_FAST_WAIT` | `1` | Fast SPU wait loop. |
| `SPU_WAIT_YIELD` | auto (on below 8 logical CPUs) | Yield the core on every SPU poll. |
| `PS3_PRIO_BOOST` | auto (on with 8+ logical CPUs) | Raise the priority of the game's busy threads. Log: `[threads] host priority boosts ...`. |
| `RFOM_MAIN_CORE` | auto (on with 12+ logical CPUs) | Pin the main thread (and the RSX FIFO thread, `RFOM_FIFO_CPU`) to their own cores. Log: `[affinity] ...`. |
| `PS3_USLEEP_TIMER_CACHE` | `1` | Reuse a high-resolution timer for short sleeps. |

## Crash and hang reports

- If the game crashes, a minidump and a line in `logs\crash\crashes.txt` are written (`[crash] reporter on` at start). Please send both with `logs\run.log`.
- A hang watchdog writes a dump if no frame is presented for 90 s (not while paused). `RFOM_HANG_S=<seconds>` changes the limit (minimum 10). `RFOM_CRASH_DUMPS=0` turns reports off, `=full` writes full-memory dumps.
- `FREEZE_RELAUNCH=1` restarts the game process after a freeze (pause-aware).

## Other options

| Variable | Default | What it does |
|---|---|---|
| `RSX_LOD_BIAS` | `auto` | Texture mip LOD bias, -2…2. `auto` = log2 of the upscaling ratio when FSR / DLSS upscaling is on (-1 at Performance), so textures keep output-resolution detail; 0 otherwise. |
| `RFOM_FOV_ANYCAM` | off | `1` = zoom-matched mouse speed follows any camera's FOV (old behaviour); by default only the player's view camera counts. |
| `RFOM_ZOOM_SENS` | `1.0` | Zoom-matched mouse speed when aiming / scoped: 1 = a mouse move covers the same part of the screen zoomed or not, 0 = off, up to 2. |
| `RFOM_KEY_PROMPTS` | `1` | `0` = no keyboard / Xbox prompt text (PlayStation prompts always). |
| `RFOM_SPEAKERS` | `6` | Speaker layout: 6 = 5.1, 8 = 7.1, 2 = the game's stereo layout. |
| `RFOM_GORE` | `1` | Full gore (as the US release); `0` = the console setting, `-1` = reduced. |
| `PS3_MUTE`, `PS3_NO_AUDIO` | off | Mute / disable audio. |

Not included, and why: MSAA (the renderer's render targets are shared with PS3-format readbacks and post-process passes that expect single-sample surfaces; FSR / DLSS anti-aliasing covers it); a higher-precision depth buffer (the game's depth is read back in PS3's 24-bit format by its own effects; changing it risks shadow and depth-test artefacts).

## DLSS and upscaling

- **Anti-aliasing → NVIDIA DLSS / DLAA** uses NVIDIA DLSS on GeForce RTX cards and falls back to FSR 3.1 on other GPUs (log `[DLSS] NGX unavailable ... using FSR 3.1`); if FSR cannot start either, the image is scaled normally (`[FSR] ... FSR off`). The DLSS SDK is downloaded by Setup into `tools\dlss` (if that fails, the port still builds without it).
- **Upscaling** (with FSR or DLSS on): renders the 3D scene at 67 / 58 / 50 / 33 % and lets FSR / DLSS rebuild it at the render resolution. The DLSS preset (DLAA, Quality, Balanced, Performance, Ultra Performance) follows the chosen ratio. Menus and HUD stay at full resolution.
- The shader cache is backed up to `%LOCALAPPDATA%\ResistancePort\shadercache` after each session and restored before launch, so reinstalling or updating doesn't bring back first-visit shader stutter.

## Status

- The campaign has been played up to Cheddar Gorge. Later levels haven't been tested yet, so problems are possible there.
- Crashes and freezes are rare. Crash and hang reports are written automatically (see *Crash and hang reports*), and the port can restart the game process after a freeze.
- The game is mostly CPU-bound and uses little GPU. CPU use and temperatures are far lower than when emulating the game.

## Known issues

- **Performance drops in heavy areas.** These come from the game's SPU jobs, which mostly run one after another while the main thread waits on them. A faster CPU helps most.
- **Audio crackle when the frame rate drops** in intensive spots, for example coming out of the crater at the start of York. Audio mixing is one of the SPU jobs.
- **Falling through the floor** has been seen a few times and hasn't been reproduced yet. A safeguard tries to put you back in place before the fall kills you.
- **Shadows can flicker** occasionally.
- **FSR can ghost** on some things, such as the sky, at rare moments. DLSS doesn't show this.
- **Out-of-bounds water** (off the dock in Grimsby, under the bridge in Manchester) shows some stray triangles.
- **Rollers:** on RPCS3 they crashed the game unless OpenGL was used. Their level hasn't been reached on this port yet.
- **Ultrawide HUD:** the HUD is stretched with the view, not kept in a centred 16:9 area.
- **Two program windows:** on the PS3 disc the main menu and the levels are separate programs, so the window reopens when you load into a level from the main menu. A minimised console window holds the log while the game runs.

If you hit a problem, describe how to reproduce it and send `logs\run.log` from that session (plus `logs\crash\` if there was a crash).

## How it works

- `ps3recomp/` is a modified copy of [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet (MIT licence). It holds the runtime, the PS3 system libraries (graphics, audio, input, file system, SPU scheduling) and the lifters that translate PowerPC and SPU code into C/C++.
- `project/` is the game-specific part of the port: its hooks, the D3D12 renderer integration, mouse look, the PC menu, the launcher and the debug tools.
- `tools/rfom_regen.py` runs the lifters on your decrypted `EBOOT.elf` and `game.elf`, then re-applies this port's own edits to the generated code (`tools/hooks/`).

## Credits and licences

- Original Resistance: Fall of Man recomp: [Psyykosis](https://github.com/Psyykosis/Resistance-Fall-of-Man-Recomp). This fork adds DLSS / FSR 3.1, the PC menu, ultrawide, the performance work and the other PC features above.
- ps3recomp, by Ned Heller / sp00nznet: MIT.
- AMD FidelityFX SDK (FSR 3.1), the DLLs in `tools/ffx_bin/`: MIT, © Advanced Micro Devices.
- NVIDIA DLSS SDK v310.9.16, downloaded during setup into `tools/dlss/`: NVIDIA RTX SDK licence (`tools/dlss/LICENSE.txt`), not part of this repository.
- FFmpeg 7.1.1 (gyan.dev build), downloaded during setup for the game's videos: GPL/LGPL, not part of this repository.
- RPCS3 (`key_vault.cpp`), downloaded during setup and used only to decrypt your own copy.
- The port's own code: MIT, see `LICENSE`.

*Resistance: Fall of Man* is © Sony Interactive Entertainment, developed by Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac. Please own the game you play.
