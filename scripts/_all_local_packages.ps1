#Requires -Version 5.1
<#
.SYNOPSIS
  Build all FOUR installer tiers locally (2026-10-25, user decision):
  lite / standard / essay / wiki / wanxiang.

  The standard tier is produced by _all_in_package.ps1, which also builds the
  shared stage tree and version.txt. The other three tiers reuse that stage
  with -OutputName / -ExcludeBuild / -GramPath, exactly like CI does.

  The wanxiang tier is LOCAL ONLY: full payload with the wanxiang grammar
  model replacing essay (380MB; too large for a CI tier per user decision).
  CI keeps building the three official tiers.

  Output files (installer\dist\):
    moqi-im-windows-setup-<ver>.exe           (standard)
    moqi-im-windows-setup-lite-<ver>.exe      (lite: no gram, no prebuilt data)
    moqi-im-windows-setup-essay-<ver>.exe     (essay gram bundled)
    moqi-im-windows-setup-wiki-<ver>.exe      (self-trained wiki gram bundled)
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
# 默认版本号取 version.txt：wiki 档的产物存在性检查用 $Version 拼文件名，
# 空值会去找 "wiki-.exe" 而误报 wiki tier failed（2026-10-29 实证，v10 同样中招）。
if (-not $Version) {
    $Version = (Get-Content (Join-Path $RepoRoot "version.txt") -Raw).Trim()
}
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

if (-not $EssayGram) {
    $EssayGram = Join-Path $RepoRoot "installer\dist\zh-hans-t-essay-bgw.gram"
}
if (-not $WanxiangGram) {
    $WanxiangGram = Join-Path $RepoRoot "installer\dist\wanxiang-lts-zh-hans.gram"
}
if (-not $WikiGram) {
    $WikiGram = Join-Path $RepoRoot "installer\dist\zh-moqi-wiki.gram"
}
foreach ($gram in @($EssayGram, $WanxiangGram, $WikiGram)) {
    if (-not (Test-Path -LiteralPath $gram)) {
        throw "Grammar model not found: $gram (put it in installer\dist or pass -EssayGram/-WanxiangGram)"
    }
}

$allIn = Join-Path $PSScriptRoot "_all_in_package.ps1"
$buildInstaller = Join-Path $RepoRoot "installer\build-installer.ps1"
$stageDir = Join-Path $RepoRoot "installer\stage"

Write-Host "== Tier 1/5: standard (shared stage + version.txt + 9MB gram) =="
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
# 2026-10-29: standard tier now ships the 9MB zh-moqi.gram.
# Re-run ISCC on the shared stage with -GramPath instead of plumbing the
# parameter through two layers of scripts.
$StdGram = Join-Path $RepoRoot "installer\dist\zh-moqi.gram"
if (-not (Test-Path -LiteralPath $StdGram)) { throw "standard gram not found: $StdGram" }
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup" `
    -GramPath $StdGram

Write-Host "== Tier 2/5: lite (1KB dummy gram: BeamSearch switch, no content) =="
$DummyGram = Join-Path $RepoRoot "installer\dist\zh-moqi-dummy.gram"
if (-not (Test-Path -LiteralPath $DummyGram)) { throw "dummy gram not found: $DummyGram" }
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup-lite" `
    -ExcludeBuild 'moqi-ime\input_methods\rime\data\build\*' `
    -GramPath $DummyGram

Write-Host "== Tier 3/5: essay =="
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup-essay" `
    -GramPath $EssayGram

Write-Host "== Tier 4/5: wanxiang (local-only tier) =="
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup-wanxiang" `
    -GramPath $WanxiangGram
Write-Host "== Tier 5/5: wiki (self-trained gram) =="
& $buildInstaller -StageDir $stageDir `
    -Version $Version `
    -OutputName "moqi-im-windows-setup-wiki" `
    -GramPath $WikiGram
if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $RepoRoot ("installer\dist\moqi-im-windows-setup-wiki-" + $Version + ".exe")))) { throw "wiki tier failed" }


Write-Host ""
Write-Host "== All tiers built: =="
Get-ChildItem (Join-Path $RepoRoot "installer\dist") -Filter "moqi-im-windows-setup*.exe" |
    Sort-Object LastWriteTime -Descending |
    Select-Object Name, @{N = "SizeMB"; E = { [math]::Round($_.Length / 1MB, 1) } }, LastWriteTime |
    Format-Table -AutoSize

