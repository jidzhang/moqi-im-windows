#Requires -Version 5.1
<#
.SYNOPSIS
  Build all FIVE installer tiers locally (2026-11-04, universal x86 engine):
  lite / standard / essay / wiki / wanxiang.

  The standard tier is produced by _all_in_package.ps1, which also builds the
  shared stage tree and version.txt. The other four tiers reuse that stage
  with -OutputName / -ExcludeBuild / -GramPath, exactly like CI does.

  All five tiers share one stage tree built by _all_in_package.ps1; the only
  differences are the bundled gram (and lite dropping the prebuilt dict
  cache). release.yml builds the same five tiers from the gram-assets release.

  Output files (installer\dist\):
    moqi-im-windows-setup-<ver>.exe           (standard)
    moqi-im-windows-setup-lite-<ver>.exe      (lite: 1.1MB lite gram, no prebuilt dict cache)
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
# Default $Version from version.txt: the wiki-tier existence check builds the
# file name from $Version, so an empty value looks for "wiki-.exe" and reports
# "wiki tier failed" even though the installer was produced.
# Encoding note (2026-11-06): this file now carries Chinese comments and MUST
# stay UTF-8 with BOM -- PowerShell 5.1 reads a BOM-less file as ANSI and the
# comments turn to mojibake.
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
# 标准档的 gram 由 _all_in_package.ps1 统一带进安装包（2026-11-06：
# 直接跑 _all_in_package.ps1 曾产出漏 gram 的 62.7MB 包），这里显式传入
# 并做存在性检查，不再二次跑 ISCC。
$StdGram = Join-Path $RepoRoot "installer\dist\zh-moqi.gram"
if (-not (Test-Path -LiteralPath $StdGram)) { throw "standard gram not found: $StdGram" }
$allArgs = @{
    RepoRoot = $RepoRoot
    Configuration = $Configuration
    Generator = $Generator
    GramPath = $StdGram
}
if ($MoqiImeRoot) { $allArgs.MoqiImeRoot = $MoqiImeRoot }
if ($ProtobufRoot) { $allArgs.ProtobufRoot = $ProtobufRoot }
if ($ProtobufSourceDir) { $allArgs.ProtobufSourceDir = $ProtobufSourceDir }
if ($Version) { $allArgs.Version = $Version }
& $allIn @allArgs

Write-Host "== Tier 2/5: lite (compiled-out AI/cloud + 1.1MB gram) =="
$LiteGram = Join-Path $RepoRoot "installer\dist\zh-moqi-lite.gram"
if (-not (Test-Path -LiteralPath $LiteGram)) { throw "lite gram not found: $LiteGram" }
# Lite = 全量 stage 换 lite daemon（MOQI_LITE 编译期剔除 AI/云剪贴板/WebDAV/
# 剪贴板监视；TSF DLL 无需 lite 变体——执行与菜单全在 daemon 侧）。
$daemonDir = Join-Path $RepoRoot "moqi-daemon"
$daemonLiteBuild = Join-Path $daemonDir "build-x86-lite"
& cmake -S $daemonDir -B $daemonLiteBuild -G $Generator -A Win32 -DMOQI_LITE=ON
if ($LASTEXITCODE -ne 0) { throw "lite daemon configure failed" }
& cmake --build $daemonLiteBuild --config Release --target MoqiDaemon
if ($LASTEXITCODE -ne 0) { throw "lite daemon build failed" }
$stageLite = Join-Path $RepoRoot "installer\stage-lite"
if (Test-Path -LiteralPath $stageLite) { Remove-Item -LiteralPath $stageLite -Recurse -Force }
Copy-Item -LiteralPath $stageDir -Destination $stageLite -Recurse
Copy-Item -LiteralPath (Join-Path $daemonLiteBuild "Release\MoqiDaemon.exe") `
    -Destination (Join-Path $stageLite "win32\MoqiIM\MoqiDaemon.exe") -Force
& $buildInstaller -StageDir $stageLite `
    -OutputName "moqi-im-windows-setup-lite" `
    -ExcludeBuild 'moqi-ime\input_methods\rime\data\build\*' `
    -GramPath $LiteGram

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

