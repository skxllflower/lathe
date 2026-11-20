# build-release-native.ps1 - one command to produce the native Lathe's Windows release: the zip Vacant Systems
# Manager installs. There is no standalone installer since 2026-11-20: the manager installs, updates and removes it.
#
#   & .\tools\build-release-native.ps1              # full release build
#   & .\tools\build-release-native.ps1 -SkipCpp     # reuse the core + GUI builds
#
# Steps, in order:
#   1. the core CLI (cmake build/ --target lathe)            -> stage\coredist\lathe.exe + its LGPL libav DLLs + notices
#   2. the GUI (native/, its own build tree on the pinned vpf and WAVdesk commits) -> stage\lathe-gui.exe
#   3. Vacant Systems Manager's package                      -> native\build-release\manager\Lathe-<ver>-win-x64.zip
#      (the installed folder, zipped; the program sets itself up: lathe-gui --setup, native/app/setup_cli.h)
#
# The GUI builds in native\build-release with no FETCHCONTENT_SOURCE_DIR_* overrides: a release is the pinned commits
# (native/CMakeLists.txt GIT_TAG), never a local checkout's working tree. A core without libav FAILS the release (its
# decode-server would be a stub: native video in WAVdesk breaks). Quit Lathe first.

param(
    [string]$Configuration = 'Release',
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
$script:LogPath = Join-Path $out 'last-release-build.log'

# the version: tauri.conf.json's, Lathe's one source (cmake/lathe_version.cmake: the CLI's --version, the GUI's
# VERSIONINFO and About; here the manager's zip). Every built piece is checked against it below.
$version = (Get-Content (Join-Path $repo 'gui\src-tauri\tauri.conf.json') -Raw | ConvertFrom-Json).version

# a built piece that says another version fails the release (a bump that missed a place, a stale build tree)
function Assert-Version([string]$Got, [string]$What) {
    if ($Got -ne $version) { throw "version mismatch: $What says '$Got', tauri.conf.json says '$version'" }
    Write-Host "  version ${version}: $What"
}

if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Set-Content -Path $script:LogPath -Value "build-release-native $version $(Get-Date -Format s)"

function Step($n, $msg) { Write-Host "`n[$n/3] $msg" -ForegroundColor Cyan }

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
$cliVersion = ((& (Join-Path $coredist 'lathe.exe') --version | Out-String).Trim() -replace '^lathe\s+', '')
Assert-Version $cliVersion 'lathe.exe --version'

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
Assert-Version ((Get-Item (Join-Path $stage 'lathe-gui.exe')).VersionInfo.ProductVersion -replace '\.0$', '') 'lathe-gui.exe ProductVersion'
& (Join-Path $PSScriptRoot 'bump-version.ps1') -Check | Out-Null
if ($LASTEXITCODE -ne 0) { throw "version mismatch in the Tauri manifests: run tools\bump-version.ps1 -Check" }
Write-Host "  version ${version}: package.json, Cargo.toml, Cargo.lock"

# ---- 3: Vacant Systems Manager's package ----
# vacant-labs apps/manager/docs/app-contract.md: the installed folder (the stage) under one top folder "Lathe", zipped
# by Windows' bsdtar (forward slashes). Reproducible: every file stamped with HEAD's commit time, the entries in the
# folder's (NTFS: sorted) order. The program does its own setup (--setup).
Step 3 "the manager's zip"
$mgr  = Join-Path $out 'manager'
$pack = Join-Path $mgr 'pack'
$item = Join-Path $pack 'Lathe'
if (Test-Path $pack) { Remove-Item -Recurse -Force $pack }
New-Item -ItemType Directory -Force -Path $item | Out-Null
Copy-Item (Join-Path $stage '*') $item -Recurse -Force
$stamp = [datetime]::Parse((& git -C $repo log -1 --format=%cI).Trim()).ToUniversalTime()
Get-ChildItem $item -Recurse -Force | ForEach-Object { $_.LastWriteTimeUtc = $stamp }
(Get-Item $item).LastWriteTimeUtc = $stamp
$zip = Join-Path $mgr "Lathe-$version-win-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# (Windows' own bsdtar by path: from Git Bash a bare tar.exe is GNU tar, which writes no zip)
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
Invoke-Native { & $tar -a -cf $zip -C $pack 'Lathe' } "tar (the manager's zip)"
Remove-Item -Recurse -Force $pack
Write-Host "  -> $zip"

Write-Host "`nRelease ready (the manager's package):" -ForegroundColor Green
Write-Host "  $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
