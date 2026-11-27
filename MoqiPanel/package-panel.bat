@echo off
REM Double-click wrapper: build and assemble the portable MoqiPanel folder.
REM Pure ASCII by project rule. Log written to MoqiPanel\package-panel.log.
setlocal
cd /d "%~dp0"

echo Building portable MoqiPanel (daemon x86 + panel + assemble) ...
echo Log: package-panel.log
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "package-panel.ps1" > "package-panel.log" 2>&1
if errorlevel 1 (
    echo.
    echo BUILD FINISHED WITH ERRORS - check the end of package-panel.log
) else (
    echo.
    echo BUILD OK - portable folder: dist\MoqiPanel
)

echo.
dir /b dist\MoqiPanel 2>nul
echo.
pause
