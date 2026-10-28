# Resistance: Fall of Man — native PC port (work in progress)

A static recompilation of the PS3 game *Resistance: Fall of Man* into a native Windows program. No emulator: the game's PowerPC and SPU code is translated to C/C++ and compiled for your PC, with a Direct3D 12 renderer, keyboard and mouse controls, and higher resolutions.

> ⚠️ **Early work in progress.** So far it has been tested on **one PC only** (Ryzen 5 3600, Radeon RX 6700 XT, 32 GB RAM, Windows) and played **only up to Grimsby**. Everything after Grimsby is untested no man's land: levels may crash, softlock or look wrong. Please report what you find.

> **This repository contains no game files and no game code.** You need your own copy of the game. The setup builds everything game-specific on your PC from that copy.

## What you need

- A PC running Windows 10 or 11 (64-bit).
- A CPU with AVX2: Intel 4th generation (Haswell, 2013) or newer, or any AMD Ryzen. Older CPUs without AVX2 won't run it.
- A DirectX 12 graphics card. Only an AMD Radeon has been tested so far; NVIDIA and Intel GPUs should work, but nobody has confirmed it yet.
- Your own dump of **Resistance: Fall of Man, US disc (BCUS98107), version 1.00**. This is the folder that contains `PS3_GAME`, the same kind of folder RPCS3 uses. Other regions and patched versions are not supported yet.
- About 25 GB of free disk space: 16 GB for the game files and the rest for the build tools and the build.
- An internet connection during setup.

## Install

1. Download this repository: on GitHub, use **Code → Download ZIP**, then unzip it somewhere with no spaces in the path, for example `C:\Games\ResistancePort`.
2. Double-click **`Setup.bat`**. It will:
   - install the free build tools if they're missing (Visual Studio 2022 Build Tools, LLVM, CMake, Ninja, Python), using `winget`;
   - ask you to pick your game folder, then copy it into the port folder (or move it, if it's on the same drive);
   - decrypt the game's two program files, using the application keys from RPCS3's public source code;
   - generate the game code from your copy and build the game.

   The first run takes a while: tool installs plus a 20–60 minute build.
3. Play with **`Launcher.exe`**. It has the settings (display mode, resolution, frame rate, anti-aliasing, FOV, mouse sensitivity) and the key bindings.

You only need to run Setup again after downloading a newer version of the port.

**First launch:** the game shows its own one-time "initial setup" screens (about 45–50 seconds, like the PS3's first boot). The first time you load into a level, the screen can stay black for 20 seconds or so while the graphics shaders are prepared; after that, levels load normally.

## Controls

Keyboard and mouse work out of the box: WASD to move, mouse to aim, left click to fire, right click to zoom, Space to jump, Shift or C to crouch, R to reload, F to interact, G for a grenade, E for weapon select, and Esc to pause. In-game button prompts switch between keyboard keys and controller buttons depending on what you last used. Rebind keys in Launcher → Controls. Controllers also work.

## Status

This is a work in progress, tested on a single PC. The campaign has been played from the start through Grimsby; nothing after Grimsby has been tested. Expect bugs:

- Performance is limited by the game's SPU job system and varies by area; a fast CPU helps most.
- There are occasional rare freezes and other bugs. The port watches for some freezes and restarts the game process automatically.

If you hit a problem, the files in `logs\` (especially `run.log`) help with tracking it down. **F12** saves a capture of the current frame to `rsxcap\`.

## How it works

- `ps3recomp/` is a modified copy of [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet (MIT licence). It holds the runtime, the PS3 system libraries (graphics, audio, input, file system, SPU scheduling) and the lifters that translate PowerPC and SPU code into C/C++.
- `project/` is the game-specific part of the port: its hooks, mouse look, the launcher and the debug tools.
- `tools/rfom_regen.py` runs the lifters on your decrypted `EBOOT.elf` and `game.elf`, then re-applies this port's own edits to the generated code (`tools/hooks/`).

## Credits and licences

- ps3recomp, by Ned Heller / sp00nznet: MIT.
- AMD FidelityFX SDK (FSR 3.1), the DLLs in `tools/ffx_bin/`: MIT, © Advanced Micro Devices.
- FFmpeg 7.1.1 (gyan.dev build), downloaded during setup for the game's videos: GPL/LGPL, not part of this repository.
- RPCS3 (`key_vault.cpp`), downloaded during setup and used only to decrypt your own copy.
- The port's own code: MIT, see `LICENSE`.

*Resistance: Fall of Man* is © Sony Interactive Entertainment, developed by Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac. Please own the game you play.
