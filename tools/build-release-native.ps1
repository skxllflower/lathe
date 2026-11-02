# build-release-native.ps1 - one command to produce the native Lathe's Windows installer (the vpf GUI in native/,
# replacing the Tauri one that tools/build-release.ps1 bundles).
#
#   & .\tools\build-release-native.ps1              # full release build
#   & .\tools\build-release-native.ps1 -SkipCpp     # reuse the core + GUI builds
#
# Steps, in order:
#   1. the core CLI (cmake build/ --target lathe)            -> stage\coredist\lathe.exe + its LGPL libav DLLs + notices
#   2. the GUI (native/, its own build tree on the pinned vpf and WAVdesk commits) -> stage\lathe-gui.exe
#   3. makensis native/installer/lathe.nsi                   -> native\build-release\Lathe_<ver>_x64-setup.exe
#   4. a copy into gui\src-tauri\target\release\bundle\nsis\, where WAVdesk's build-release-native.ps1 picks up Lathe's
#      installer for its companion offer (the newest Lathe*-setup.exe there)
#
# The GUI builds in native\build-release with no FETCHCONTENT_SOURCE_DIR_* overrides: a release is the pinned commits
# (native/CMakeLists.txt GIT_TAG), never a local checkout's working tree. A core without libav FAILS the release (its
# decode-server would be a stub: native video in WAVdesk breaks). Quit Lathe first.

param(
    [string]$Configuration = 'Release',
    [string]$Makensis      = '',
    [switch]$Clean,
    [switch]$SkipCpp
)

$ErrorActionPreference = 'Stop'

$repo     = Split-Path -Parent $PSScriptRoot
$build    = Join-Path $repo 'build'
$native   = Join-Path $repo 'native'
$out      = Join-Path $native 'build-release'
$guiBuild = Join-Path $out 'build'
$stage    = Join-Path $out 'stage'
$coredist = Join-Path $stage 'coredist'
$icon     = Join-Path $repo 'gui\src-tauri\icons\icon.ico'
$script:LogPath = Join-Path $out 'last-release-build.log'

# the version: tauri.conf.json's, the one source native/CMakeLists.txt also reads (the exe's VERSIONINFO, About)
$version = (Get-Content (Join-Path $repo 'gui\src-tauri\tauri.conf.json') -Raw | ConvertFrom-Json).version

if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Set-Content -Path $script:LogPath -Value "build-release-native $version $(Get-Date -Format s)"

function Step($n, $msg) { Write-Host "`n[$n/4] $msg" -ForegroundColor Cyan }

# cmake and friends write warnings to stderr; under EAP Stop one stderr line would abort a good build, so the exit code
# is the only failure signal
function Invoke-Native([scriptblock]$Block, [string]$What) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Block 2>&1 | Tee-Object -FilePath $script:LogPath -Append }
    finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

# ---- 1: the core ----
Step 1 "the core CLI ($Configuration)"
if ($SkipCpp) {
    Write-Host "  -SkipCpp: reusing build/" -ForegroundColor Yellow
} else {
    Invoke-Native { & cmake -B $build -S $repo } "cmake configure (core)"
    Invoke-Native { & cmake --build $build --config $Configuration --target lathe } "cmake build (core)"
}
$relDir  = Join-Path $build $Configuration
$coreSrc = Join-Path $relDir 'lathe.exe'
if (-not (Test-Path $coreSrc)) { throw "core binary not found: $coreSrc (run without -SkipCpp?)" }
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $coredist | Out-Null
Copy-Item $coreSrc $coredist -Force
Get-ChildItem -Path $relDir -Filter '*.dll' | Copy-Item -Destination $coredist -Force
$notice = Join-Path $relDir 'THIRD_PARTY_NOTICES.txt'
if (-not (Test-Path $notice)) { $notice = Join-Path $repo 'THIRD_PARTY_NOTICES.txt' }
Copy-Item $notice $coredist -Force
$dllCount = (Get-ChildItem $coredist -Filter '*.dll').Count
$libavInfo = (& (Join-Path $coredist 'lathe.exe') libav-version | Out-String).Trim()
if ($dllCount -eq 0 -or $libavInfo -match 'not built in') {
    throw "lathe.exe was built WITHOUT libav ($dllCount DLL(s); libav-version: '$libavInfo'). Configure with the LGPL ffmpeg dev package (third_party/ffmpeg or -DLATHE_FETCH_FFMPEG=ON) and rebuild without -SkipCpp."
}
Write-Host "  staged lathe.exe + $dllCount DLL(s) -> $coredist"
Write-Host "  libav OK: $((($libavInfo -split "`r?`n")[0..1]) -join ' | ')"

# ---- 2: the GUI ----
Step 2 "the native GUI ($Configuration, pinned vpf + WAVdesk)"
if (-not $SkipCpp -or -not (Test-Path (Join-Path $guiBuild "bin\$Configuration\lathe-gui.exe"))) {
    Invoke-Native { & cmake -S $native -B $guiBuild -A x64 -U 'FETCHCONTENT_SOURCE_DIR_*' } "cmake configure (GUI)"
    Invoke-Native { & cmake --build $guiBuild --config $Configuration --target lathe_gui } "cmake build (GUI)"
}
$guiExe = Join-Path $guiBuild "bin\$Configuration\lathe-gui.exe"
if (-not (Test-Path $guiExe)) { throw "GUI not found: $guiExe" }
Copy-Item $guiExe $stage -Force
Write-Host "  -> $(Join-Path $stage 'lathe-gui.exe')"

# ---- 3: the installer ----
Step 3 "makensis"
if (-not $Makensis) {
    $cands = @((Get-Command makensis -EA SilentlyContinue).Source,
               "${env:ProgramFiles(x86)}\NSIS\makensis.exe",
               "$env:ProgramFiles\NSIS\makensis.exe",
               "$env:LOCALAPPDATA\tauri\NSIS\makensis.exe") | Where-Object { $_ -and (Test-Path $_) }
    $Makensis = $cands | Select-Object -First 1
}
if (-not $Makensis) { throw "makensis not found: install NSIS 3 (nsis.sourceforge.io) or pass -Makensis <path>." }
$setup = Join-Path $out "Lathe_${version}_x64-setup.exe"
Invoke-Native { & $Makensis /V2 "/DVERSION=$version" "/DSTAGE=$stage" "/DOUTFILE=$setup" "/DICON=$icon" (Join-Path $native 'installer\lathe.nsi') } "makensis"

# ---- 4: where WAVdesk's release looks for it ----
Step 4 "copy for WAVdesk's companion offer"
$compat = Join-Path $repo 'gui\src-tauri\target\release\bundle\nsis'
New-Item -ItemType Directory -Force -Path $compat | Out-Null
Copy-Item $setup $compat -Force
(Get-Item (Join-Path $compat (Split-Path -Leaf $setup))).LastWriteTime = Get-Date   # (WAVdesk takes the newest)
Write-Host "  -> $compat"

Write-Host "`nInstaller ready:" -ForegroundColor Green
Write-Host "  $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
