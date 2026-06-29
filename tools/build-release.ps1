# build-release.ps1 - one command to produce a shippable Lathe installer.
#
# Lathe's Tauri GUI spawns its C++ core (lathe.exe) as a subprocess but does NOT
# declare it as a sidecar. So this stages the freshly-built core plus the LGPL
# ffmpeg DLLs it links into gui/src-tauri/coredist/, which tauri.conf bundles as
# resources -> they install into <install>/coredist/ next to the GUI exe, where
# tools.rs resolves them (and the app self-registers the path for WAVdesk).
#
#   pwsh tools/build-release.ps1                 # full release build
#   pwsh tools/build-release.ps1 -Clean          # wipe build/ first
#   pwsh tools/build-release.ps1 -SkipCpp        # reuse existing C++ build
#   pwsh tools/build-release.ps1 -SkipBundle     # stop after staging the core

param(
    [string]$Configuration = 'Release',
    [switch]$Clean,
    [switch]$SkipCpp,
    [switch]$SkipBundle
)

$ErrorActionPreference = 'Stop'

$repo     = Split-Path -Parent $PSScriptRoot
$build    = Join-Path $repo 'build'
$gui      = Join-Path $repo 'gui'
$tauriDir = Join-Path $gui 'src-tauri'
$coredist = Join-Path $tauriDir 'coredist'
$coreExe  = 'lathe.exe'

function Step($n, $msg) { Write-Host "`n[$n/3] $msg" -ForegroundColor Cyan }

$script:LogPath = Join-Path $PSScriptRoot 'last-release-build.log'
Set-Content -Path $script:LogPath -Value "Lathe release build $(Get-Date -Format s)`n"

# Native tools write progress/warnings to stderr; under EAP=Stop a single stderr
# line becomes a terminating NativeCommandError. Relax EAP for the call and gate
# on the exit code instead.
function Invoke-Native([scriptblock]$Block, [string]$What) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Block 2>&1 | Tee-Object -FilePath $script:LogPath -Append }
    finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

# Make the installer's directory page show the true Program Files\Vacant Systems
# path. Tauri exposes no install-dir config and our installerHooks can't reach
# the directory page, so the page would show Program Files\<App> while the
# PREINSTALL hook redirects to the vendor subfolder. Patch the generated
# installer.nsi's .onInit default and recompile with Tauri's own makensis, then
# swap the result into the bundle. The PREINSTALL redirect stays as a fallback.
function Repair-VendorInstallDir {
    $nsi  = Join-Path $tauriDir 'target\release\nsis\x64\installer.nsi'
    $dest = Get-ChildItem (Join-Path $tauriDir 'target\release\bundle\nsis\*-setup.exe') -EA SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not (Test-Path $nsi) -or -not $dest) {
        Write-Host "  vendor-path display fix skipped (no installer.nsi / bundle)" -ForegroundColor Yellow
        return
    }
    $mk = Get-ChildItem "$env:LOCALAPPDATA\tauri" -Recurse -Filter 'makensis.exe' -EA SilentlyContinue | Select-Object -First 1
    if (-not $mk) {
        Write-Host "  vendor-path display fix skipped (makensis not found)" -ForegroundColor Yellow
        return
    }
    $t = [System.IO.File]::ReadAllText($nsi)
    $t = $t.Replace('StrCpy $INSTDIR "$PROGRAMFILES64\${PRODUCTNAME}"', 'StrCpy $INSTDIR "$PROGRAMFILES64\Vacant Systems\${PRODUCTNAME}"')
    $t = $t.Replace('StrCpy $INSTDIR "$PROGRAMFILES\${PRODUCTNAME}"',   'StrCpy $INSTDIR "$PROGRAMFILES\Vacant Systems\${PRODUCTNAME}"')
    [System.IO.File]::WriteAllText($nsi, $t, (New-Object System.Text.UTF8Encoding($false)))
    Invoke-Native { & $mk.FullName $nsi } "makensis (vendor-path recompile)"
    $built = Join-Path (Split-Path $nsi -Parent) 'nsis-output.exe'
    if (Test-Path $built) {
        Move-Item $built $dest.FullName -Force
        Write-Host "  vendor-path display fix applied -> $($dest.Name)"
    } else {
        Write-Host "  vendor-path recompile produced no output; kept Tauri's installer" -ForegroundColor Yellow
    }
}

# ---- Step 1: build the C++ core ------------------------------------------
Step 1 "C++ core ($Configuration)"
if ($SkipCpp) {
    Write-Host "  -SkipCpp: reusing existing build/" -ForegroundColor Yellow
} else {
    if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
    Invoke-Native { & cmake -B $build -S $repo } "cmake configure"
    Invoke-Native { & cmake --build $build --config $Configuration } "cmake build"
}

# ---- Step 2: stage the core + runtime files into coredist ----------------
Step 2 "Stage core -> coredist"
$relDir  = Join-Path $build $Configuration
$coreSrc = Join-Path $relDir $coreExe
if (-not (Test-Path $coreSrc)) { throw "core binary not found: $coreSrc (run without -SkipCpp?)" }
if (Test-Path $coredist) {
    Get-ChildItem $coredist -Exclude '.gitkeep' | Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Force -Path $coredist | Out-Null
}
Copy-Item -Path $coreSrc -Destination $coredist -Force
Get-ChildItem -Path $relDir -Filter '*.dll' | Copy-Item -Destination $coredist -Force
$notice = Join-Path $relDir 'THIRD_PARTY_NOTICES.txt'
if (Test-Path $notice) { Copy-Item -Path $notice -Destination $coredist -Force }
$dllCount = (Get-ChildItem $coredist -Filter '*.dll' -ErrorAction SilentlyContinue).Count
Write-Host "  staged $coreExe + $dllCount DLL(s) -> $coredist"

# ---- Step 3: bundle the installer ----------------------------------------
if ($SkipBundle) {
    Write-Host "`n-SkipBundle: stopping after staging." -ForegroundColor Yellow
    return
}
Step 3 "pnpm tauri build (NSIS)"
if (-not (Get-Command pnpm -ErrorAction SilentlyContinue)) {
    throw "pnpm not found on PATH. Install it or run 'corepack enable', then retry."
}
Push-Location $gui
try { Invoke-Native { & pnpm tauri build } "pnpm tauri build" }
finally { Pop-Location }

Repair-VendorInstallDir

$nsisDir = Join-Path $tauriDir 'target\release\bundle\nsis'
$installer = Get-ChildItem -Path $nsisDir -Filter '*-setup.exe' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host ""
if ($installer) {
    Write-Host "Installer ready:" -ForegroundColor Green
    Write-Host "  $($installer.FullName)"
} else {
    Write-Host "Build finished, but no *-setup.exe found under $nsisDir" -ForegroundColor Yellow
}
