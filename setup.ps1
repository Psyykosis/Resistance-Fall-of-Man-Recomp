# Resistance: Fall of Man - native PC port: one-time setup.
#
# 1. installs the free build tools (Visual Studio Build Tools, LLVM, CMake,
#    Ninja, Python) with winget, if they are missing
# 2. asks for YOUR copy of the game (a PS3 disc dump folder) and copies it
#    into this folder (gamefiles\)
# 3. decrypts the game's two program files and regenerates the port's game
#    code from them (tools\rfom_regen.py)
# 4. builds the game (build.bat)
#
# Nothing from the game is downloaded: everything game-specific comes from the
# copy you point it at. Run it again any time; finished steps are skipped.

# 'Continue': tools like python/winget print warnings on stderr, which
# PowerShell 5 would otherwise turn into a fatal error. Every step checks
# its own result instead.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$Port = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Port
$env:PYTHONUTF8 = '1'
New-Item -ItemType Directory -Force -Path (Join-Path $Port 'logs') | Out-Null
try { Start-Transcript -Path (Join-Path $Port 'logs\setup.log') -Force | Out-Null } catch {}
trap { Write-Host ""; Write-Host "SETUP STOPPED with an unexpected error: $_" -ForegroundColor Red; Write-Host "The details are in logs\setup.log."; try { Stop-Transcript | Out-Null } catch {}; Read-Host "Press Enter to close"; exit 1 }

# The supported version: Resistance: Fall of Man, BCUS98107 (US disc), v1.00.
$TitleId = 'BCUS98107'
$Sha1 = @{ 'EBOOT.elf' = 'a33494999e0bc60296bb7ab159d4da420f4e25e5'; 'game.elf' = '45cca7e7ec8e3cec0b9bccc1d204f3cf7159b468' }

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Info($t) { Write-Host "   $t" }
function Fail($t) {
    Write-Host ""; Write-Host "SETUP STOPPED: $t" -ForegroundColor Red
    Write-Host "Nothing is lost - fix the problem above and run Setup.bat again. (Full log: logs\setup.log)"
    try { Stop-Transcript | Out-Null } catch {}
    Read-Host "Press Enter to close"; exit 1
}
function Refresh-Path {
    $m = [Environment]::GetEnvironmentVariable('Path', 'Machine'); $u = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$m;$u;$env:LOCALAPPDATA\Microsoft\WinGet\Links;C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin"
}
function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }
function Winget-Install($id, $label, $extra) {
    if (-not (Have 'winget')) { Fail "winget is not available. Install 'App Installer' from the Microsoft Store, then run Setup.bat again." }
    Info "Installing $label (this can take several minutes)..."
    $a = @('install', '--id', $id, '-e', '--accept-package-agreements', '--accept-source-agreements', '--silent')
    if ($extra) { $a += $extra }
    & winget @a | Out-Host
    Refresh-Path
}
function Sha1Of($p) { (Get-FileHash -Algorithm SHA1 -LiteralPath $p).Hash.ToLower() }

Write-Host "Resistance: Fall of Man - PC port setup" -ForegroundColor Yellow
Write-Host "Folder: $Port"
if ($Port -match ' ') { Info "Note: this folder's path has spaces in it. That works, but a path without spaces (like C:\Games\ResistancePort) is safer." }

# ---------------------------------------------------------------- 1. tools
Step "1/5  Build tools"
Refresh-Path
$py = $null
foreach ($c in @('py', 'python')) {
    if (Have $c) {
        $v = & $c -c "import sys; print(sys.version_info[0]*100+sys.version_info[1])" 2>$null
        if ($LASTEXITCODE -eq 0 -and ("$v".Trim() -match '^\d+$') -and [int]"$v".Trim() -ge 310) { $py = $c; break }
    }
}
if (-not $py) { Winget-Install 'Python.Python.3.12' 'Python 3.12'; if (Have 'py') { $py = 'py' } elseif (Have 'python') { $py = 'python' } else { Fail "Python did not install." } }
Info "Python: OK"
& $py -c "import Crypto" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Info "Installing pycryptodome (for decrypting the game files)..."
    & $py -m pip install --user --quiet --disable-pip-version-check pycryptodome 2>&1 | Out-Host
    & $py -c "import Crypto" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Could not install the Python package pycryptodome." }
}
if (-not (Have 'cmake')) { Winget-Install 'Kitware.CMake' 'CMake' }
if (-not (Have 'ninja')) { Winget-Install 'Ninja-build.Ninja' 'Ninja' }
if (-not (Have 'clang-cl')) { Winget-Install 'LLVM.LLVM' 'LLVM (clang)' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) {
    Winget-Install 'Microsoft.VisualStudio.2022.BuildTools' 'Visual Studio 2022 Build Tools (C++)' @('--override', '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) { Fail "Visual Studio Build Tools did not install (C++ workload)." }
}
foreach ($c in @('cmake', 'ninja', 'clang-cl')) { if (-not (Have $c)) { Fail "$c is still missing after installing it. Restart the PC and run Setup.bat again." } }
Info "Visual Studio C++ tools, LLVM, CMake, Ninja: OK"

# ---------------------------------------------------------------- 2. game copy
Step "2/5  Your copy of the game"
$gf = Join-Path $Port 'gamefiles'
$usr = Join-Path $gf 'PS3_GAME\USRDIR'
if (-not (Test-Path (Join-Path $usr 'EBOOT.BIN'))) {
    Info "Pick the folder of your Resistance: Fall of Man (BCUS98107) disc dump -"
    Info "the folder that contains PS3_GAME (and usually PS3_DISC.SFB)."
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Select your Resistance: Fall of Man disc dump (the folder containing PS3_GAME)'
    if ($dlg.ShowDialog() -ne 'OK') { Fail "No game folder picked." }
    $src = $dlg.SelectedPath
    if (-not (Test-Path (Join-Path $src 'PS3_GAME')) -and (Split-Path -Leaf $src) -eq 'PS3_GAME') { $src = Split-Path -Parent $src }
    $sfo = Join-Path $src 'PS3_GAME\PARAM.SFO'
    if (-not (Test-Path (Join-Path $src 'PS3_GAME\USRDIR\game.self'))) { Fail "That folder has no PS3_GAME\USRDIR\game.self. Pick the disc dump folder (the one containing PS3_GAME)." }
    $sfoText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($sfo))
    if ($sfoText -notmatch $TitleId) { Fail "This is not $TitleId (the US disc). Only the US disc version is supported for now." }
    $sameDrive = ([IO.Path]::GetPathRoot($src) -eq [IO.Path]::GetPathRoot($Port))
    $move = $false
    if ($sameDrive) {
        $ans = Read-Host "   Move the game folder into the port folder (instant, but RPCS3 will no longer find it there) or Copy it (keeps the original, needs ~16 GB free)? [M/C]"
        $move = ($ans -match '^[Mm]')
    }
    if ($move) {
        Info "Moving..."
        if (Test-Path $gf) { Remove-Item $gf -Recurse -Force }
        Move-Item -LiteralPath $src -Destination $gf
    } else {
        Info "Copying the game files (about 16 GB, this takes a while)..."
        & robocopy $src $gf /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Fail "Copying the game files failed (robocopy code $LASTEXITCODE). Is there enough free space?" }
    }
}
# the PS3's hard-drive cache and game-data folders the game expects to exist
New-Item -ItemType Directory -Force -Path (Join-Path $gf "cache\$TitleId"), (Join-Path $gf "game\$TitleId\USRDIR") | Out-Null
Info "Game files: $gf"

# ---------------------------------------------------------------- 3. decrypt
Step "3/5  Decrypting the game program files"
New-Item -ItemType Directory -Force -Path (Join-Path $Port 'game') | Out-Null
$need = $false
foreach ($n in $Sha1.Keys) { $p = Join-Path $Port "game\$n"; if (-not (Test-Path $p) -or (Sha1Of $p) -ne $Sha1[$n]) { $need = $true } }
if ($need) {
    $kv = Join-Path $Port 'tools\key_vault.cpp'
    if (-not (Test-Path $kv)) {
        Info "Fetching the PS3 application keys from RPCS3's public source code..."
        try { Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Crypto/key_vault.cpp' -OutFile $kv }
        catch { Fail "Could not download key_vault.cpp from github.com (no internet?)." }
    }
    & $py tools\unself.py --keys $kv --out game "$usr\EBOOT.BIN" "$usr\game.self" | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "Decrypting EBOOT.BIN / game.self failed." }
    foreach ($n in $Sha1.Keys) {
        if ((Sha1Of (Join-Path $Port "game\$n")) -ne $Sha1[$n]) {
            Fail "game\$n is not the expected version. The port supports the unpatched US disc (BCUS98107 v1.00); a game update or another release won't match."
        }
    }
}
Info "EBOOT.elf and game.elf: OK (BCUS98107 v1.00)"

# ---------------------------------------------------------------- 4. game code
Step "4/5  Generating the game code from your copy"
if (-not (Test-Path (Join-Path $Port 'recompiled_game\ppu_recomp.h')) -or -not (Test-Path (Join-Path $Port 'project\spu\pm_wwsjob\spu_recomp.c')) -or $env:RFOM_REGEN) {
    & $py tools\rfom_regen.py --eboot game\EBOOT.elf --game game\game.elf | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "Generating the game code failed (see the messages above)." }
} else { Info "Already generated (set RFOM_REGEN=1 to force)." }

# ---------------------------------------------------------------- 5. build
Step "5/5  Building (the first build takes 20-60 minutes)"
$ffd = Join-Path $Port 'tools\ffmpeg_bin'
if (-not (Test-Path (Join-Path $ffd 'avcodec-61.dll'))) {
    Info "Fetching FFmpeg 7.1.1 (for the game's videos)..."
    $zip = Join-Path $env:TEMP 'rfom_ffmpeg.zip'
    try { Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/GyanD/codexffmpeg/releases/download/7.1.1/ffmpeg-7.1.1-full_build-shared.zip' -OutFile $zip }
    catch { Fail "Could not download FFmpeg from github.com." }
    $tmp = Join-Path $env:TEMP 'rfom_ffmpeg'; if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $tmp
    New-Item -ItemType Directory -Force -Path $ffd | Out-Null
    Get-ChildItem $tmp -Recurse -Include 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll' | Copy-Item -Destination $ffd
    if (-not (Test-Path (Join-Path $ffd 'avcodec-61.dll'))) { Fail "The FFmpeg download did not contain avcodec-61.dll." }
}
& cmd /c "`"$Port\build.bat`" nopause"
if ($LASTEXITCODE -ne 0) { Fail "The build failed - see logs\build.log and logs\build_game.log." }

Write-Host ""
Write-Host "SETUP COMPLETE. Start the game with Launcher.exe." -ForegroundColor Green
try { Stop-Transcript | Out-Null } catch {}
Start-Process (Join-Path $Port 'Launcher.exe')
Read-Host "Press Enter to close"
