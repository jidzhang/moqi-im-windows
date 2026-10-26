@echo off
REM Double-click wrapper: build all 4 installer tiers locally.
REM Prompts for version (default = current version.txt). Log written to
REM installer\dist\package-all.log. Pure ASCII by project rule.
setlocal
cd /d "%~dp0"

set /p VERSIONFILE=<version.txt
echo Current version.txt = %VERSIONFILE%
set /p VER=Version to build (empty = keep %VERSIONFILE%):
if "%VER%"=="" (
    set VER=%VERSIONFILE%
)

echo.
echo Building all 4 tiers for version %VER% ...
echo (Go backend + TSF DLL x64/x86 + dict prebuild + Inno x4, about 15 min)
echo Log: installer\dist\package-all.log
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\_all_local_packages.ps1" -Version "%VER%" > "installer\dist\package-all.log" 2>&1
if errorlevel 1 (
    echo.
    echo BUILD FINISHED WITH ERRORS - check the end of installer\dist\package-all.log
) else (
    echo.
    echo BUILD OK - packages in installer\dist\
)

echo.
dir /b /o-d "installer\dist\moqi-im-windows-setup*.exe" | findstr /r "^moqi-im-windows-setup.*exe$"
echo.
pause
