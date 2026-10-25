#Requires -Version 5.1
<#
.SYNOPSIS
  Compile MoqiTsf.iss with Inno Setup 6 (requires ISCC.exe on PATH or default path).

.PARAMETER StageDir
  Root of the staged installer tree. Expected layout:
    win32\MoqiIM\...
    x64\MoqiIM\...

.PARAMETER IssPath
  Optional path to MoqiTsf.iss (default: installer dir next to this script).

.EXAMPLE
  .\installer\build-installer.ps1 -StageDir D:\moqi-im-windows\installer\stage
#>
param(
    [Parameter(Mandatory = $true)]
    [string] $StageDir,
    [string] $IssPath = '',
    [string] $Version = '',
    [string] $OutputName = '',
    [string] $ExcludeBuild = '',
    [string] $GramPath = ''
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $StageDir)) {
    Write-Error "StageDir not found: $StageDir"
}
$StageDir = (Resolve-Path -LiteralPath $StageDir).Path

$win32Root = Join-Path $StageDir 'win32\MoqiIM'
$x64Root = Join-Path $StageDir 'x64\MoqiIM'
if (-not (Test-Path -LiteralPath $win32Root)) {
    Write-Error "Stage win32 payload not found: $win32Root"
}
if (-not (Test-Path -LiteralPath $x64Root)) {
    Write-Error "Stage x64 payload not found: $x64Root"
}

$requiredPaths = @(
    (Join-Path $win32Root 'MoqiLaunch.exe'),
    (Join-Path $win32Root 'SetupHelper.exe'),
    (Join-Path $win32Root 'MoqiTextService.dll'),
    (Join-Path $win32Root 'backends.json'),
    (Join-Path $win32Root 'x64\MoqiTextService.dll'),
    (Join-Path $x64Root 'MoqiTextService.dll')
)
foreach ($path in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Error "Required staged file not found: $path"
    }
}

if ([string]::IsNullOrWhiteSpace($IssPath)) {
    $IssPath = Join-Path $PSScriptRoot 'MoqiTsf.iss'
}
if (-not (Test-Path -LiteralPath $IssPath)) {
    Write-Error "ISS not found: $IssPath"
}

$candidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    'ISCC.exe'
)
$iscc = $null
foreach ($c in $candidates) {
    if ($c -eq 'ISCC.exe') {
        $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($cmd) { $iscc = $cmd.Path; break }
    }
    elseif (Test-Path -LiteralPath $c) {
        $iscc = $c
        break
    }
}
if (-not $iscc) {
    Write-Error @"
Inno Setup 6 compiler (ISCC.exe) not found.
Install: https://jrsoftware.org/isdl.php
Then re-run this script.
"@
}

# Version: an explicit -Version wins; otherwise use version.txt (single source
# of truth for the DLL resources and the installer).
if ([string]::IsNullOrWhiteSpace($Version)) {
    $versionFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'version.txt'
    if (Test-Path -LiteralPath $versionFile) {
        $Version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    }
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = '1.0.0'
}

$argStage = '/DStageDir=' + $StageDir
$argVersion = '/DMyAppVersion=' + $Version
$isccArgs = @("`"$IssPath`"", $argStage, $argVersion)
if (-not [string]::IsNullOrWhiteSpace($OutputName)) {
    $isccArgs += '/DMyOutputBaseFilename=' + $OutputName
}
if (-not [string]::IsNullOrWhiteSpace($ExcludeBuild)) {
    $isccArgs += '/DMyExcludeBuild=' + $ExcludeBuild
}
if (-not [string]::IsNullOrWhiteSpace($GramPath)) {
    if (-not (Test-Path -LiteralPath $GramPath)) {
        Write-Error "Gram file not found: $GramPath"
    }
    $GramPath = (Resolve-Path -LiteralPath $GramPath).Path
    $isccArgs += '/DMyGramPath=' + $GramPath
}
Write-Host "ISCC: $iscc"
Write-Host "Args: `"$IssPath`" $($isccArgs[1..($isccArgs.Count - 1)] -join ' ')"
$p = Start-Process -FilePath $iscc -ArgumentList $isccArgs -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 0) {
    Write-Error "ISCC failed with exit code $($p.ExitCode)"
}

$dist = Join-Path $PSScriptRoot 'dist'
$outputBase = if ([string]::IsNullOrWhiteSpace($OutputName)) { 'moqi-im-windows-setup' } else { $OutputName }
# Append the version to the output file name (2026-10-25, user decision):
# setup-1.5.1.exe / setup-full-1.5.1.exe. Skip when already suffixed.
if ($outputBase -notmatch "-$([regex]::Escape($Version))$") {
    $versionedBase = $outputBase + '-' + $Version
    Rename-Item -LiteralPath (Join-Path $dist ($outputBase + '.exe')) -NewName ($versionedBase + '.exe') -Force
    $outputBase = $versionedBase
}
Write-Host "Output: $(Join-Path $dist ($outputBase + '.exe'))"
exit 0
