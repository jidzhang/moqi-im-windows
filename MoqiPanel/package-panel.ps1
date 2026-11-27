#Requires -Version 5.1
<#
.SYNOPSIS
  Assemble the portable MoqiPanel folder: one self-contained directory that
  runs without installation (no registry, no files written outside the folder).

.DESCRIPTION
  Build steps (unless -SkipBuild):
    1. build MoqiDaemon.exe (x86) -- it carries the --portable switch
    2. build MoqiPanel.exe and run its unit tests

  Then assemble <OutDir> (default MoqiPanel\dist\MoqiPanel):
    MoqiPanel.exe            the panel app
    MoqiDaemon.exe           input engine (started/stopped by the panel)
    rime.dll                 x86 engine library (daemon loads it from exe dir)
    moqi-ime\input_methods\rime\{data,icons,ime.json}
    fonts\                   bundled Noto Sans SC + its OFL license
    README.txt               user guide
    moqi-ime\...\rime\data\default.custom.yaml
                             panel-only data patch: punctuation keys output ASCII

  The engine data comes from the standard installer stage tree
  (installer\stage\win32\MoqiIM, produced by scripts\_all_in_package.ps1 or
  package-all.bat). Pass -StageDir to use another product tree instead.

  This script is pure ASCII and CRLF by project rule (PowerShell 5.1 reads a
  BOM-less file as ANSI). The Chinese user guide ships as README.txt (UTF-8
  with BOM, so Notepad on an English Windows detects the encoding).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File MoqiPanel\package-panel.ps1

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File MoqiPanel\package-panel.ps1 -SkipBuild
#>
param(
  [string] $RepoRoot = "",
  [string] $StageDir = "",
  [string] $OutDir = "",
  [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
if (-not $StageDir) { $StageDir = Join-Path $RepoRoot "installer\stage\win32\MoqiIM" }
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot "dist\MoqiPanel" }
$StageDir = [System.IO.Path]::GetFullPath($StageDir)
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

$daemonDir = Join-Path $RepoRoot "moqi-daemon"
$daemonBuild = Join-Path $daemonDir "build-x86"
$panelBuild = Join-Path $PSScriptRoot "build-x86"

if (-not $SkipBuild) {
  Write-Host "== Step 1/2: Build MoqiDaemon (x86, --portable capable) =="
  & cmake -S $daemonDir -B $daemonBuild -G "Visual Studio 17 2022" -A Win32
  if ($LASTEXITCODE -ne 0) { throw "MoqiDaemon configure failed" }
  & cmake --build $daemonBuild --config Release --target MoqiDaemon
  if ($LASTEXITCODE -ne 0) { throw "MoqiDaemon build failed" }

  Write-Host "== Step 2/2: Build MoqiPanel + unit tests =="
  & cmake -S $PSScriptRoot -B $panelBuild -G "Visual Studio 17 2022" -A Win32
  if ($LASTEXITCODE -ne 0) { throw "MoqiPanel configure failed" }
  & cmake --build $panelBuild --config Release
  if ($LASTEXITCODE -ne 0) { throw "MoqiPanel build failed" }
  & ctest --test-dir $panelBuild -C Release --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "MoqiPanel unit tests failed" }
}

# ---- inputs must exist before anything is deleted ----
$sources = @(
  (Join-Path $panelBuild "Release\MoqiPanel.exe"),
  (Join-Path $daemonBuild "Release\MoqiDaemon.exe"),
  (Join-Path $StageDir "rime.dll"),
  (Join-Path $StageDir "moqi-ime\input_methods\rime\data"),
  (Join-Path $StageDir "moqi-ime\input_methods\rime\icons"),
  (Join-Path $StageDir "moqi-ime\input_methods\rime\ime.json"),
  (Join-Path $PSScriptRoot "vendor\fonts\NotoSansSC-Regular.otf"),
  (Join-Path $PSScriptRoot "vendor\fonts\OFL-NotoSansSC.txt"),
  (Join-Path $PSScriptRoot "README.txt"),
  (Join-Path $PSScriptRoot "rime\default.custom.yaml")
)
foreach ($path in $sources) {
  if (-not (Test-Path -LiteralPath $path)) {
    throw "Required input not found: $path (run package-all.bat to build the stage tree, or pass -StageDir)"
  }
}

Write-Host "== Assemble: $OutDir =="
if (Test-Path -LiteralPath $OutDir) {
  # The folder may hold a running panel/daemon (exe and user\userdb locked).
  try {
    Remove-Item -LiteralPath $OutDir -Recurse -Force
  } catch {
    # An EMPTY folder is fine (Explorer sitting in it also holds the handle
    # on HGFS): assemble into it. Only a non-empty leftover is a real error.
    $remaining = @(Get-ChildItem -LiteralPath $OutDir -Recurse -Force)
    if ($remaining.Count -gt 0) {
      throw "Cannot replace $OutDir -- close MoqiPanel.exe and MoqiDaemon.exe first. ($($_.Exception.Message))"
    }
  }
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

Copy-Item -LiteralPath $sources[0] -Destination $OutDir
Copy-Item -LiteralPath $sources[1] -Destination $OutDir
Copy-Item -LiteralPath $sources[2] -Destination $OutDir
$rimeOut = Join-Path $OutDir "moqi-ime\input_methods\rime"
New-Item -ItemType Directory -Path $rimeOut -Force | Out-Null
Copy-Item -LiteralPath $sources[3] -Destination $rimeOut -Recurse
Copy-Item -LiteralPath $sources[4] -Destination $rimeOut -Recurse
Copy-Item -LiteralPath $sources[5] -Destination $rimeOut
$fontsOut = Join-Path $OutDir "fonts"
New-Item -ItemType Directory -Path $fontsOut -Force | Out-Null
Copy-Item -LiteralPath $sources[6] -Destination $fontsOut
Copy-Item -LiteralPath $sources[7] -Destination $fontsOut
Copy-Item -LiteralPath $sources[8] -Destination $OutDir
# Panel-only engine config: every punctuation key outputs half-width ASCII.
# Pure data inside this folder; the product's own switching logic is untouched.
Copy-Item -LiteralPath $sources[9] -Destination (Join-Path $rimeOut "data")

$count = (Get-ChildItem -LiteralPath $OutDir -Recurse -File).Count
$sizeMb = [math]::Round(((Get-ChildItem -LiteralPath $OutDir -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host ""
Write-Host "OK: $OutDir ($count files, $sizeMb MB)"
Write-Host "Next: run MoqiPanel\tests\portable_daemon_check.py to verify portable mode."
