# Resistance: Fall of Man — native PC port/recomp (95% finished)

A static recompilation of the PS3 game *Resistance: Fall of Man* into a native Windows program. No emulation: the game's PowerPC and SPU code is translated to C/C++ and compiled for your PC, with a Direct3D 12 renderer, keyboard and mouse controls, and higher resolutions.

> ⚠️ **Not Perfect.** I myself have only tested it **on my pc** (Ryzen 5 3600, Radeon RX 6700 XT, 32 GB RAM, Windows 11) haven't beat the game, but gotten up to Cheddar Gorge. To my knowledge no one has gotten past there yet, so things could go wrong after. Some crashes may happen but should be few and far between.

> **This repository contains no game files and no game code.** You need your own copy of the game. The setup builds everything game-specific on your PC from that copy.

## What you need

- A PC running Windows 10 or 11 (64-bit).
- A CPU with AVX2: so basically Intel 4th gen or newer, or any AMD Ryzen. Older CPUs without AVX2 won't run it. If your CPU is from the last 10-15 years it'll work.
- A DirectX 12 GPU. Only been tested on my AMD GPU so far; I don't see why NVIDIA or Intel GPUs wouldn't work, but nobody has confirmed it yet.
- Your own dump of **Resistance: Fall of Man, US disc (BCUS98107), version 1.00**. This is the folder that contains `PS3_GAME`, the same kind of folder RPCS3 uses. Other regions and patched versions are not supported yet.
- About 25 GB of free disk space: 16 GB for the game files and the rest for the build tools and the build.
- An internet connection for the setup.

## Install

1. Download this repository: Click on Code at the top, then Download ZIP, then unzip it somewhere with no spaces in the path or folder name, for example `C:\Games\ResistancePort`.
2. Double-click **`Setup.bat`**. It will:
   - install the free build tools if they're missing (it needs Visual Studio 2022 Build Tools, LLVM, CMake, Ninja, and Python), using `winget`;
   - ask you to pick your game folder, then copy it into the port folder (or move it, if it's on the same drive);
   - decrypt the game's two program files, using the application keys from RPCS3's public source code;
   - generate the game code from your copy and build the game.

   The first run takes a while: tool installs plus a 20–60 minute build.
3. Play with **`Launcher.exe`**. It has the settings (display mode, resolution, frame rate, anti-aliasing, FOV, mouse sensitivity) and the key bindings.

You only need to run Setup again after downloading a newer version of the port, for example when I update it, other than that you can just use the launcher.

**First launch:** the game shows its own one-time "initial setup" screens. The first time you load into a level for example York, the world will render black for maybe 20 seconds or so while the graphics shaders are loading; after that first time, levels should load normally.

## Controls

This does have keyboard and mouse support. It should be hotswappable but I haven't tested it with controller myself as I don't use controller. Button prompts should show keys for keyboard and should switch back for controller, and certain things I didn't like like how the melee and interact button were the same button on PS3 have been separated into separate keys. You can configure keyboard and mouse controls in the launcher.

## Status

This is a work in progress, tested on a single PC. The campaign has been played from the start through Grimsby; I haven't gone any further than Grimsby on this version so it's no man's land as I said:

- It is mostly CPU bound, uses very little GPU. Even with how CPU bound it is, it likely won't skyrocket your usage the way that emulating would. Your temp may go up but still far less than emulating does and all while giving better performance.
- Performance is currently still limited by the game's SPU job system and varies by area; so having a decent CPU helps most.
- There are occasional rare freezes and other bugs. The port watches for some freezes and may restart the game process automatically.

If you hit a problem, let me know how to reproduce it and what it was, and if you can, send the `logs\run.log` from that session too (it's in the logs folder), that helps a ton, and I'll see if I can fix it.

## How it works

- `ps3recomp/` is a modified copy of [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet (MIT licence). It holds the runtime, the PS3 system libraries (graphics, audio, input, file system, SPU scheduling) and the lifters that translate PowerPC and SPU code into C/C++.
- `project/` is the game-specific part of the port: its hooks, mouse look, the launcher and the debug tools.
- `tools/rfom_regen.py` runs the lifters on your decrypted `EBOOT.elf` and `game.elf`, then re-applies this port's own edits to the generated code (`tools/hooks/`).

## Known Bugs

- General performance drops. That's mostly the game's SPU jobs (the stuff the PS3's SPUs used to handle), right now they kinda run one after another and the main thread just sits there waiting on them. It is basically the known limitation, I haven't found a way around that yet.
- Sometimes you may randomly fall through the floor, it happened about 4 times across 3 days of testing so it's very rare and unknown what causes it yet, but a crutch has been added so that when it does happen it will hopefully put you back in place before you fall to your death, but I haven't been able to reproduce it to test the fix.
- Shadows may flicker on and off at some times, but again it's rare and I am not sure what triggers it or how to reproduce it so I just have to play until it happens.
- In intensive areas, sound may or may not become slightly staticky when FPS/performance drops (most noticeable right when coming out of the crater in York at the very beginning). Another SPU job issue, the sound mixing is one of those jobs so it gets bogged down right along with everything else.
- FSR causing ghosting with some things like the sky at some rare moments.
- One unknown, Rollers. On RPCS3, Rollers were the biggest problem, they would always crash your game when they come on screen unless you used OpenGL, Rollers would crash the game when on screen. This may be fixed or may not be, considering this uses DirectX 12 and not Vulkan. I have yet to get to where they spawn, I believe it is in Bracknell.
- Out of bounds water like off the dock in Grimsby or under the bridge in Manchester has some triangles on it.

## Some Plans

- Revamping how the game runs in terms of windows, so that it doesn't open the CMD log window and doesn't reopen when loading into missions. The CMD is just for debugging and the reopen happens because on the PS3 disc the game is separated into two different programs, the main menu is one program and the level is another program, and on the PS3 when you load into a level from the main menu it opens that next program, the window is just the separate program. I plan on making them both load in one window and removing the CMD log.
- Adding DLSS 5 into the launcher as an optional graphics setting. It may be possible to do yourself with Reshade, but Reshade itself is untested (will be tested eventually) and it may not be able to read the depth buffer properly, so feel free to try Reshade yourself and let me know how it turns out. It should be directed to build_game\ResistanceFoMGame.exe. DLSS 5 would be much more straightforward done through the launcher because it would work through the same pipeline as FSR 3.1, just swapping it out. I cannot test it myself because I do not have an Nvidia GPU at the moment.

## Credits and licences

- ps3recomp, by Ned Heller / sp00nznet: MIT.
- AMD FidelityFX SDK (FSR 3.1), the DLLs in `tools/ffx_bin/`: MIT, © Advanced Micro Devices.
- FFmpeg 7.1.1 (gyan.dev build), downloaded during setup for the game's videos: GPL/LGPL, not part of this repository.
- RPCS3 (`key_vault.cpp`), downloaded during setup and used only to decrypt your own copy.
- The port's own code: MIT, see `LICENSE`.

*Resistance: Fall of Man* is © Sony Interactive Entertainment, developed by Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac. Please own the game you play.
