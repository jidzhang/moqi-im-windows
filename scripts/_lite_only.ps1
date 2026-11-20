$ErrorActionPreference = "Stop"
$RepoRoot = "Z:\dev\IME\MoqiIME\moqi-im-windows"
$Generator = "Visual Studio 17 2022"
$stageDir = Join-Path $RepoRoot "installer\stage"
$stageLite = Join-Path $RepoRoot "installer\stage-lite"
$daemonDir = Join-Path $RepoRoot "moqi-daemon"
$daemonLiteBuild = Join-Path $daemonDir "build-x86-lite"
$LiteGram = Join-Path $RepoRoot "installer\dist\zh-moqi-lite.gram"
$buildInstaller = Join-Path $RepoRoot "installer\build-installer.ps1"
& cmake -S $daemonDir -B $daemonLiteBuild -G $Generator -A Win32 -DMOQI_LITE=ON
if ($LASTEXITCODE -ne 0) { throw "lite configure failed" }
& cmake --build $daemonLiteBuild --config Release --target MoqiDaemon
if ($LASTEXITCODE -ne 0) { throw "lite build failed" }
if (Test-Path -LiteralPath $stageLite) { Remove-Item -LiteralPath $stageLite -Recurse -Force }
Copy-Item -LiteralPath $stageDir -Destination $stageLite -Recurse
Copy-Item -LiteralPath (Join-Path $daemonLiteBuild "Release\MoqiDaemon.exe") -Destination (Join-Path $stageLite "win32\MoqiIM\MoqiDaemon.exe") -Force
$notepad2InLite = Join-Path $stageLite "win32\MoqiIM\Notepad2"
if (Test-Path -LiteralPath $notepad2InLite) { Remove-Item -LiteralPath $notepad2InLite -Recurse -Force }
$liteRimeRoot = Join-Path $stageLite "win32\MoqiIM\moqi-ime\input_methods\rime"
foreach ($liteRemove in @("ai_config.json", "templates", "cloudclipboard")) {
    $litePath = Join-Path $liteRimeRoot $liteRemove
    if (Test-Path -LiteralPath $litePath) { Remove-Item -LiteralPath $litePath -Recurse -Force }
}
& $buildInstaller -StageDir $stageLite -OutputName "moqi-im-windows-setup-lite" -ExcludeBuild 'moqi-ime\input_methods\rime\data\build\*' -GramPath $LiteGram
Write-Host "LITE DONE"
