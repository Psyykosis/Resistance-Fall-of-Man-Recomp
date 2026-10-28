@echo off
rem Resistance: Fall of Man - native PC port: one-time setup (see setup.ps1).
cd /d "%~dp0"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
if errorlevel 1 (echo. & echo Setup did not finish - see logs\setup.log & pause)
