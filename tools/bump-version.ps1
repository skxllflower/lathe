# bump-version.ps1 - Lathe's version, changed in one go.
#
#   & .\tools\bump-version.ps1 0.1.9          # write it
#   & .\tools\bump-version.ps1 -Check         # only report: every place agrees with tauri.conf.json (exit 1 if not)
#
# The source is gui/src-tauri/tauri.conf.json's "version": the core CLI (lathe --version) and the native GUI (its
# VERSIONINFO, About) read it at configure time (cmake/lathe_version.cmake), the release scripts pass it to the NSIS
# installer and name the manager's zip with it, and Vacant Systems Manager's pack checks the zip against it. The Tauri
# GUI's own manifests (gui/package.json, gui/src-tauri/Cargo.toml and Cargo.lock's lathe-gui entry) are kept in step
# here. build-release-native.ps1 runs the same check on what it built and fails on any disagreement.

param(
    [Parameter(Position = 0)] [string]$Version = '',
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$conf = Join-Path $repo 'gui\src-tauri\tauri.conf.json'
$pkg = Join-Path $repo 'gui\package.json'
$cargo = Join-Path $repo 'gui\src-tauri\Cargo.toml'
$lock = Join-Path $repo 'gui\src-tauri\Cargo.lock'

# (each file: a regex whose group 2 is the version, the first match only; the file rewritten byte for byte otherwise)
$places = @(
    @{ File = $conf;  Re = '("version"\s*:\s*")([0-9]+\.[0-9]+\.[0-9]+)(")' },
    @{ File = $pkg;   Re = '("version"\s*:\s*")([0-9]+\.[0-9]+\.[0-9]+)(")' },
    @{ File = $cargo; Re = '(?m)(^\[package\][^\[]*?^version\s*=\s*")([0-9]+\.[0-9]+\.[0-9]+)(")' },
    @{ File = $lock;  Re = '(name = "lathe-gui"\r?\nversion = ")([0-9]+\.[0-9]+\.[0-9]+)(")' }
)

function Read-Text($p) { [System.IO.File]::ReadAllText($p) }
function Found($place) {
    $m = [regex]::Match((Read-Text $place.File), $place.Re)
    if (-not $m.Success) { throw "no version found in $($place.File)" }
    return $m.Groups[2].Value
}

if ($Version) {
    if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "not x.y.z: $Version" }
    foreach ($p in $places) {
        $text = Read-Text $p.File
        $re = [regex]::new($p.Re)
        if (-not $re.IsMatch($text)) { throw "no version found in $($p.File)" }
        $new = $re.Replace($text, { param($m) $m.Groups[1].Value + $Version + $m.Groups[3].Value }, 1)
        [System.IO.File]::WriteAllText($p.File, $new, (New-Object System.Text.UTF8Encoding $false))
        Write-Host "  $($p.File): $Version"
    }
    Write-Host "Lathe $Version (rebuild: cmake reconfigures from tauri.conf.json)"
    exit 0
}

$want = Found $places[0]
$bad = @()
foreach ($p in $places) {
    $v = Found $p
    Write-Host ("  {0,-8} {1}" -f $v, $p.File)
    if ($v -ne $want) { $bad += $p.File }
}
if ($bad.Count) { Write-Host "version mismatch: tauri.conf.json says $want" -ForegroundColor Red; exit 1 }
Write-Host "Lathe $want everywhere" -ForegroundColor Green
if (-not $Check) { exit 0 }
