@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist build\ResistanceFoM.exe (echo The game is not built yet - run Setup.bat first. & pause & exit /b 1)
rem ---- settings the port needs (Launcher.exe's choices come after, from settings.bat) ----
set "SCREEN_AS_TEX=0"
set "CHILD_RSX_ACCUM_FRAME=1"
set "CHILD_RFOM_FPS=60"
set "CHILD_SPURS_PM_YIELD=0"
set "CHILD_FP_SAVE=1"
set "CHILD_VS_SAVE=1"
set "CHILD_FREEZE_RELAUNCH=900"
set "CHILD_SPU_JOB_INTERP=0"
set "CHILD_RSX_MIDFLUSH=1"
rem F8 / F11: frame trace and profile, written to logs\ (for bug reports)
set "CHILD_PS3_FTRACE=99999:6:%~dp0logs\ftrace.txt"
set "CHILD_PS3_FTRACE_CAP=1500000"
set "PS3_DEBUG=%~dp0logs\dbg.txt"
rem your copy of the game (Setup.bat put it here)
set "PS3_VFS_ROOT=%~dp0gamefiles"
rem Launcher.exe writes settings.bat (display mode, frame limit, vsync, filtering, keys)
if exist settings.bat call settings.bat
echo Running... (log: %~dp0logs\run.log). Close the game window to stop.
build\ResistanceFoM.exe "%~dp0game\EBOOT.elf" 2>&1 | "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "$input | Tee-Object -FilePath logs\run.log"
