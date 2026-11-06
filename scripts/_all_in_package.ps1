#Requires -Version 5.1
<#
.SYNOPSIS
  One-click build for moqi-ime backend, moqi-im-windows binaries, and installer package.

.PARAMETER RepoRoot
  Root of moqi-im-windows (defaults to the parent directory of this script).

.PARAMETER MoqiImeRoot
  Root of sibling moqi-ime repository (defaults to RepoRoot\..\moqi-ime).

.PARAMETER Configuration
  Build configuration for moqi-im-windows (default: Release).

.PARAMETER Generator
  CMake generator for moqi-im-windows (default: Visual Studio 17 2022).

.PARAMETER ProtobufRoot
  Optional local protobuf/protoc install root forwarded to scripts\build.ps1.

.PARAMETER ProtobufSourceDir
  Optional local protobuf source tree forwarded to scripts\build.ps1.

.PARAMETER GramPath
  Standard-tier grammar model (default: installer\dist\zh-moqi.gram).
  Missing file is an error: the standard package must ship the gram.

.PARAMETER WithoutGram
  Explicitly build a base package without the standard gram (lint/lite/dev only).
#>
param(
    [string] $RepoRoot = "",
    [string] $MoqiImeRoot = "",
    [string] $Configuration = "Release",
    [string] $Generator = "Visual Studio 17 2022",
    [string] $ProtobufRoot = "",
    [string] $ProtobufSourceDir = "",
    [string] $Version = "",
    # 标准档必须带 9MB zh-moqi.gram。默认取 installer\dist\zh-moqi.gram，
    # 缺失直接报错；只有显式 -WithoutGram 才允许产出无 gram 的基础包。
    [string] $GramPath = "",
    [switch] $WithoutGram
)

$ErrorActionPreference = "Stop"

function Invoke-Step {
    param(
        [string] $FilePath,
        [string[]] $ArgumentList,
        [string] $WorkingDirectory
    )

    Write-Host ">> $FilePath $($ArgumentList -join ' ')"
    if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        & $FilePath @ArgumentList
    } else {
        Push-Location $WorkingDirectory
        try {
            & $FilePath @ArgumentList
        }
        finally {
            Pop-Location
        }
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

$scriptRepoRoot = Join-Path $PSScriptRoot ".."
if (-not $RepoRoot) { $RepoRoot = $scriptRepoRoot }
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

# Optional release version (e.g. "v1.2.3" from a git tag). It is written to
# version.txt, which CMake embeds into the binaries and build-installer.ps1
# passes to Inno Setup.
if (-not [string]::IsNullOrWhiteSpace($Version)) {
    $normalizedVersion = $Version.Trim()
    if ($normalizedVersion.StartsWith('v') -or $normalizedVersion.StartsWith('V')) {
        $normalizedVersion = $normalizedVersion.Substring(1)
    }
    if ($normalizedVersion -notmatch '^\d+\.\d+\.\d+([-+][0-9A-Za-z.\-]+)?$') {
        throw "Invalid version '$Version'; expected vX.Y.Z or X.Y.Z."
    }
    Set-Content -LiteralPath (Join-Path $RepoRoot "version.txt") `
        -Value "$normalizedVersion`n" -NoNewline -Encoding ascii
    Write-Host "[INFO] version.txt <- $normalizedVersion"
}

# 标准档的 9MB gram 必须随包；之前直接跑本脚本产出的 62.7MB 包漏了它，
# 覆盖安装会把用户的 gram 洗掉。默认强制包含，只有 -WithoutGram 可跳过；
# -WithoutGram 显式要求基础包，优先于误传的 -GramPath。
if ($WithoutGram) {
    $GramPath = ""
} else {
    if ([string]::IsNullOrWhiteSpace($GramPath)) {
        $GramPath = Join-Path $RepoRoot "installer\dist\zh-moqi.gram"
    }
    if (-not (Test-Path -LiteralPath $GramPath)) {
        throw "Standard tier requires the grammar model, but it was not found: $GramPath. Pass -GramPath or use -WithoutGram to build a base package explicitly."
    }
    $GramPath = (Resolve-Path -LiteralPath $GramPath).Path
    Write-Host "[INFO] standard gram: $GramPath"
}

if (-not $MoqiImeRoot) { $MoqiImeRoot = Join-Path $RepoRoot "..\moqi-ime" }
$MoqiImeRoot = [System.IO.Path]::GetFullPath($MoqiImeRoot)

$moqiImeBuildScript = Join-Path $MoqiImeRoot "scripts\build.ps1"
$windowsBuildScript = Join-Path $RepoRoot "scripts\build.ps1"
$windowsInstallScript = Join-Path $RepoRoot "scripts\install.ps1"
$moqiImeRuntimeDir = Join-Path $MoqiImeRoot "scripts\build\moqi-ime"

if (-not $ProtobufRoot) {
    $candidatePaths = @()
    $defaultRoot = "D:\a_dev\protoc-33.5-win64"
    if (Test-Path -LiteralPath $defaultRoot) {
        $candidatePaths += $defaultRoot
    }
    if (-not [string]::IsNullOrWhiteSpace($env:MOQI_PROTOBUF_ROOT)) {
        $candidatePaths += $env:MOQI_PROTOBUF_ROOT
    }

    foreach ($candidate in $candidatePaths) {
        if (Test-Path -LiteralPath (Join-Path $candidate "bin\protoc.exe")) {
            $ProtobufRoot = [System.IO.Path]::GetFullPath($candidate)
            break
        }
    }
}

if (-not $ProtobufSourceDir) {
    $candidatePaths = @()
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $cacheRoot = Join-Path $env:USERPROFILE ".cache\moqi-protobuf"
        $candidatePaths += @(
            (Join-Path $cacheRoot "protobuf-33.5"),
            (Join-Path $cacheRoot "protobuf-34.1"),
            (Join-Path $cacheRoot "protobuf-29.5")
        )
    }

    foreach ($candidate in $candidatePaths) {
        if (Test-Path -LiteralPath (Join-Path $candidate "CMakeLists.txt")) {
            $ProtobufSourceDir = [System.IO.Path]::GetFullPath($candidate)
            break
        }
    }
}

if ($ProtobufSourceDir) {
    Write-Host "[INFO] Using local protobuf source: $ProtobufSourceDir"
}
if ($ProtobufRoot) {
    Write-Host "[INFO] Using local protobuf root: $ProtobufRoot"
}

foreach ($path in @($moqiImeBuildScript, $windowsBuildScript, $windowsInstallScript)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required script not found: $path"
    }
}

Write-Host "== Step 1/3: Build moqi-ime runtime =="
# Argument elements are passed through & as-is: paths with spaces get quoted
# by PowerShell itself. Hand-embedded quotes break under a pwsh 7 host
# (powershell.exe then receives -File '"<path>"' -> "Illegal characters in
# path"), so keep the elements bare (2026-11-06).
Invoke-Step -FilePath "powershell.exe" -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $moqiImeBuildScript,
    "-RepoRoot", $MoqiImeRoot
) -WorkingDirectory $MoqiImeRoot

# Go 后端已退休：运行时树的必需产物是 rime.dll 与打包数据，不再有 MoqiIME.exe。
if (-not (Test-Path -LiteralPath (Join-Path $moqiImeRuntimeDir "input_methods\rime\rime.dll"))) {
    throw "moqi-ime runtime was not produced: $moqiImeRuntimeDir"
}

Write-Host "== Step 2/3: Build moqi-im-windows binaries =="
$windowsBuildArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $windowsBuildScript,
    "-RepoRoot", $RepoRoot,
    "-Configuration", $Configuration,
    "-Generator", $Generator
)
if ($ProtobufSourceDir) {
    $windowsBuildArgs += @("-ProtobufSourceDir", $ProtobufSourceDir)
}
if ($ProtobufRoot) {
    $windowsBuildArgs += @("-ProtobufRoot", $ProtobufRoot)
}
Invoke-Step -FilePath "powershell.exe" -ArgumentList $windowsBuildArgs -WorkingDirectory $RepoRoot

Write-Host "== Step 3/3: Build installer package =="
$installArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $windowsInstallScript,
    "-RepoRoot", $RepoRoot,
    "-MoqiImeSource", $moqiImeRuntimeDir
)
if (-not [string]::IsNullOrWhiteSpace($GramPath)) {
    $installArgs += @("-GramPath", $GramPath)
}
Invoke-Step -FilePath "powershell.exe" -ArgumentList $installArgs -WorkingDirectory $RepoRoot

# Product name now carries the version suffix (2026-10-25): accept the versioned
# file produced by build-installer.ps1, falling back to the legacy plain name.
$versionFile = Join-Path $RepoRoot "version.txt"
$version = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -Raw).Trim() } else { "" }
$installerCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($version)) {
    $installerCandidates += (Join-Path $RepoRoot "installer\dist\moqi-im-windows-setup-$version.exe")
}
$installerCandidates += (Join-Path $RepoRoot "installer\dist\moqi-im-windows-setup.exe")
$installerPath = $installerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($installerPath) {
    Write-Host "OK: $installerPath"
} else {
    throw "Installer was not produced: $($installerCandidates -join ' or ')"
}
