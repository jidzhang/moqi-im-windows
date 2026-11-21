#Requires -Version 5.1
<#
.SYNOPSIS
  Build all THREE installer tiers locally (2026-11-21, universal x86 engine):
  lite / standard / wanxiang.

  2026-11-21 tier cut (user decision, product-stack d5 evidence): essay tier
  removed (full essay gram 67.43% loses to the 8.6MB distilled standard gram
  67.74% on d5-1888); wiki tier removed (its dist gram behaved bit-identical
  to the standard gram, and the 2026-10-28 wiki gram also lost on re-test).

  The standard tier is produced by _all_in_package.ps1, which also builds the
  shared stage tree and version.txt. The other two tiers reuse that stage
  with -OutputName / -ExcludeBuild / -GramPath, exactly like CI does.

  All three tiers share one stage tree built by _all_in_package.ps1; the only
  differences are the bundled gram (and lite dropping the prebuilt dict
  cache). release.yml builds the same three tiers from the gram-assets release.

  Output files (installer\dist\):
    moqi-im-windows-setup-<ver>.exe           (standard)
    moqi-im-windows-setup-lite-<ver>.exe      (lite: 1.1MB lite gram, no prebuilt dict cache)
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
    [string] $WanxiangGram = ""
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) { $RepoRoot = Join-Path $PSScriptRoot ".." }
# Default $Version from version.txt: the wanxiang-tier file name is built from
# $Version, so an empty value looks for "wanxiang-.exe" downstream.
# Encoding note (2026-11-06): this file now carries Chinese comments and MUST
# stay UTF-8 with BOM -- PowerShell 5.1 reads a BOM-less file as ANSI and the
# comments turn to mojibake.
if (-not $Version) {
    $Version = (Get-Content (Join-Path $RepoRoot "version.txt") -Raw).Trim()
}
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

if (-not $WanxiangGram) {
    $WanxiangGram = Join-Path $RepoRoot "installer\dist\wanxiang-lts-zh-hans.gram"
}
foreach ($gram in @($WanxiangGram)) {
    if (-not (Test-Path -LiteralPath $gram)) {
        throw "Grammar model not found: $gram (put it in installer\dist or pass -WanxiangGram)"
    }
}

$allIn = Join-Path $PSScriptRoot "_all_in_package.ps1"
$buildInstaller = Join-Path $RepoRoot "installer\build-installer.ps1"
$stageDir = Join-Path $RepoRoot "installer\stage"

Write-Host "== Tier 1/3: standard (shared stage + version.txt + 9MB gram) =="
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

Write-Host "== Tier 2/3: lite (compiled-out AI/cloud + 1.1MB gram) =="
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
# Lite 去掉 Notepad2：配置编辑菜单已裁，notepad2.exe 零消费者（2026-11-08）。
$notepad2InLite = Join-Path $stageLite "win32\MoqiIM\Notepad2"
if (Test-Path -LiteralPath $notepad2InLite) { Remove-Item -LiteralPath $notepad2InLite -Recurse -Force }
# Lite 去掉 AI 配置与文件模板：AI 已编译期剔除，ai_config.json 无读者；
# templates\ 只服务置顶短语/超级简拼菜单（Lite 已裁，成对符号设置 222 用
# 内置回退头，不依赖模板）。backends.json/ime.json/icons 是注册链必需，
# 全量与 lite 都必须保留。
$liteRimeRoot = Join-Path $stageLite "win32\MoqiIM\moqi-ime\input_methods\rime"
# cloudclipboard 是云剪贴板数据目录：Lite 编译期剔除云功能后无消费者，空目录
# 也不带（2026-11-14 用户实报）；全量档保留（云剪贴板运行时用）。
foreach ($liteRemove in @("ai_config.json", "templates", "cloudclipboard")) {
    $litePath = Join-Path $liteRimeRoot $liteRemove
    if (Test-Path -LiteralPath $litePath) { Remove-Item -LiteralPath $litePath -Recurse -Force }
}
& $buildInstaller -StageDir $stageLite `
    -OutputName "moqi-im-windows-setup-lite" `
    -ExcludeBuild 'moqi-ime\input_methods\rime\data\build\*' `
    -GramPath $LiteGram

Write-Host "== Tier 3/3: wanxiang (local-only tier) =="
& $buildInstaller -StageDir $stageDir `
    -OutputName "moqi-im-windows-setup-wanxiang" `
    -GramPath $WanxiangGram


Write-Host ""
Write-Host "== All tiers built: =="
Get-ChildItem (Join-Path $RepoRoot "installer\dist") -Filter "moqi-im-windows-setup*.exe" |
    Sort-Object LastWriteTime -Descending |
    Select-Object Name, @{N = "SizeMB"; E = { [math]::Round($_.Length / 1MB, 1) } }, LastWriteTime |
    Format-Table -AutoSize

