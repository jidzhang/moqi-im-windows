#Requires -Version 5.1
<#
.SYNOPSIS
  Build all FOUR installer tiers locally (2026-10-25, user decision):
  lite / standard / full(-essay) / wanxiang.

  The standard tier is produced by _all_in_package.ps1, which also builds the
  shared stage tree and version.txt. The other three tiers reuse that stage
  with -OutputName / -ExcludeBuild / -GramPath, exactly like CI does.

  The wanxiang tier is LOCAL ONLY: full payload with the wanxiang grammar
  model replacing essay (380MB; too large for a CI tier per user decision).
  CI keeps building the three official tiers.

  Output files (installer\dist\):
    moqi-im-windows-setup-<ver>.exe           (standard)
    moqi-im-windows-setup-lite-<ver>.exe      (lite: no gram, no prebuilt data)
    moqi-im-windows-setup-full-<ver>.exe      (essay gram bundled)
    moqi-im-windows-setup-wanxiang-<ver>.exe  (wanxiang gram bundled, local only)

.EXAMPLE
  powershell -File scripts\_all_local_packages.ps1 -Version 1.6.0
#>
param(
    [string] $RepoRoot = "",
    [string] $MoqiImeRoot = "",
    [string] $Configuration = "Release",
    [string] $Generator = "Visual Studio 17 2022",
    [string] $ProtobufRoot = "",
    [string] $ProtobufSourceDir = "",
    [string] $Version = "",
    [string] $EssayGram = "",
    [string] $WanxiangGram = ""
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) { $RepoRoot = Join-Path $PSScriptRoot ".." }
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

if (-not $EssayGram) {
    $EssayGram = Join-Path $RepoRoot "installer\dist\zh-hans-t-essay-bgw.gram"
}
if (-not $WanxiangGram) {
    $WanxiangGram = Join-Path $RepoRoot "installer\dist\wanxiang-lts-zh-hans.gram"
}
foreach ($gram in @($EssayGram, $WanxiangGram)) {
    if (-not (Test-Path -LiteralPath $gram)) {
        throw "Grammar model not found: $gram (put it in installer\dist or pass -EssayGram/-WanxiangGram)"
    }
}

$allIn = Join-Path $PSScriptRoot "_all_in_package.ps1"
$buildInstaller = Join-Path $RepoRoot "installer\build-installer.ps1"
$stageDir = Join-Path $RepoRoot "installer\stage"

Write-Host "== Tier 1/4: standard (shared stage + version.txt) =="
$allArgs = @{
    RepoRoot = $RepoRoot
    Configuration = $Configuration
    Generator = $Generator
}
if ($MoqiImeRoot) { $allArgs.MoqiImeRoot = $MoqiImeRoot }
if ($ProtobufRoot) { $allArgs.ProtobufRoot = $ProtobufRoot }
if ($ProtobufSourceDir) { $allArgs.ProtobufSourceDir = $ProtobufSourceDir }
if ($Version) { $allArgs.Version = $Version }
& $allIn @allArgs

Write-Host "== Tier 2/4: lite =="
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup-lite" `
    -ExcludeBuild 'moqi-ime\input_methods\rime\data\build\*,moqi-ime\input_methods\rime\data\zh-moqi.gram'

Write-Host "== Tier 3/4: full (essay) =="
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup-full" `
    -GramPath $EssayGram

Write-Host "== Tier 4/4: wanxiang (local-only tier) =="
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup-wanxiang" `
    -GramPath $WanxiangGram

Write-Host ""
Write-Host "== All tiers built: =="
Get-ChildItem (Join-Path $RepoRoot "installer\dist") -Filter "moqi-im-windows-setup*.exe" |
    Sort-Object LastWriteTime -Descending |
    Select-Object Name, @{N = "SizeMB"; E = { [math]::Round($_.Length / 1MB, 1) } }, LastWriteTime |
    Format-Table -AutoSize
