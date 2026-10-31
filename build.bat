@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist recompiled_game\ppu_recomp.h (echo The game code has not been generated yet - run Setup.bat first. & goto fail)
rem A game window left open locks the .exe and the link fails -- close any that are still running.
taskkill /IM ResistanceFoMGame.exe /F >nul 2>&1 && echo Closed a ResistanceFoMGame.exe that was still running.
taskkill /IM ResistanceFoM.exe /F >nul 2>&1 && echo Closed a ResistanceFoM.exe that was still running.
echo ============================================================
echo  Resistance: Fall of Man - native PC build
echo  Builds two programs: the front end (EBOOT) and the game (game.self).
echo  The first build takes a long time. Leave it running.
echo ============================================================
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (echo Visual Studio C++ build tools not found - run Setup.bat. & goto fail)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo Could not set up the Visual Studio build tools. & goto fail)
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
set "FFOPT=-DPS3RECOMP_FFMPEG_RUNTIME=ON -DPS3RECOMP_FFMPEG_INCLUDE_DIR=%~dp0ffmpeg\include"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not defined RFOM_JOBS set "RFOM_JOBS=4"

(
  echo Configuring front end...
  cmake -S project -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=ResistanceFoM -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled" -DRFOM_SPU_AVX2=ON %FFOPT% > logs\configure.log 2>&1
  if errorlevel 1 (echo Configure FAILED - see logs\configure.log & type logs\configure.log & goto fail)
)
echo [1/2] Building front end (EBOOT)...
"%PS%" -NoProfile -Command "cmake --build build -j %RFOM_JOBS% 2>&1 | Tee-Object -FilePath logs\build.log; $c = $LASTEXITCODE; if (Select-String -Path logs\build.log -Pattern '^FAILED: |^ninja: build stopped' -Quiet) { $c = 1 }; exit $c"
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build.log & goto fail)

(
  echo Configuring game...
  cmake -S project -B build_game -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=ResistanceFoMGame -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled_game" -DRFOM_SPU_AVX2=ON %FFOPT% > logs\configure_game.log 2>&1
  if errorlevel 1 (echo Configure FAILED - see logs\configure_game.log & type logs\configure_game.log & goto fail)
)
echo [2/2] Building game (game.self)...
"%PS%" -NoProfile -Command "cmake --build build_game -j %RFOM_JOBS% 2>&1 | Tee-Object -FilePath logs\build_game.log; $c = $LASTEXITCODE; if (Select-String -Path logs\build_game.log -Pattern '^FAILED: |^ninja: build stopped' -Quiet) { $c = 1 }; exit $c"
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build_game.log & goto fail)

echo Building the launcher...
set "LRES="
rc /nologo /i project /fo build\launcher.res project\launcher.rc > logs\build_launcher_rc.log 2>&1 && set "LRES=build\launcher.res"
clang-cl /nologo /O2 /EHsc /W0 project\launcher.cpp /Fo:build\launcher.obj /Fe:Launcher.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED %LRES% user32.lib gdi32.lib shell32.lib gdiplus.lib dwmapi.lib > logs\build_launcher.log 2>&1
if errorlevel 1 (echo Launcher build failed - see logs\build_launcher.log) else (echo Launcher.exe built)

rem FFmpeg (video decode) is loaded at run time: Setup.bat puts the DLLs in tools\ffmpeg_bin
for %%D in (build build_game) do for %%F in (avcodec-61.dll avutil-59.dll swresample-5.dll) do if exist "tools\ffmpeg_bin\%%F" copy /Y "tools\ffmpeg_bin\%%F" %%D\ >nul
rem AMD FidelityFX (FSR 3.1, the anti-aliasing option): AMD's signed DLLs, MIT licence (tools\ffx_bin\LICENSE.txt)
for %%D in (build build_game) do for %%F in (amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll) do if exist "tools\ffx_bin\%%F" copy /Y "tools\ffx_bin\%%F" %%D\ >nul
echo.
echo BUILD SUCCEEDED: build\ResistanceFoM.exe and build_game\ResistanceFoMGame.exe
echo Start the game with Launcher.exe
if /i not "%~1"=="nopause" pause
exit /b 0
:fail
if /i not "%~1"=="nopause" pause
exit /b 1
