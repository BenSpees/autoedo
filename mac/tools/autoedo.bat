@echo off
setlocal
rem AutoEDO one-stop launcher for Windows 10: rebuild if a make is on PATH,
rem restart the service, health-check, open the web UI.
rem
rem   autoedo.bat            build (if possible) + relaunch + open browser
rem   autoedo.bat --stop     stop a running instance
rem   set AUTOEDO_PORT=9000 first to use another port
rem
rem Building needs the MSYS2 mingw-w64 toolchain (pacman -S make
rem mingw-w64-x86_64-gcc) — run from an "MSYS2 MinGW x64" shell with `make`,
rem or put mingw32-make/make on PATH and double-click this.

set PORT=%AUTOEDO_PORT%
if "%PORT%"=="" set PORT=8017
cd /d "%~dp0.."

if "%~1"=="--stop" (
    taskkill /F /IM autoedo.exe >nul 2>&1
    echo stopped.
    exit /b 0
)

rem ── 1. Build only if a make is available ────────────────────────────────
where mingw32-make >nul 2>&1
if %errorlevel%==0 (
    mingw32-make
    if errorlevel 1 goto :buildfail
    goto :run
)
where make >nul 2>&1
if %errorlevel%==0 (
    make
    if errorlevel 1 goto :buildfail
    goto :run
)
echo [no make on PATH - skipping build, using the existing exe]

:run
if not exist build\autoedo.exe (
    echo ERROR: no build\autoedo.exe. Build from an "MSYS2 MinGW x64" shell:
    echo    pacman -S make mingw-w64-x86_64-gcc ^&^& make
    exit /b 1
)

rem ── 2. Stop what's running, relaunch detached ───────────────────────────
taskkill /F /IM autoedo.exe >nul 2>&1
start "" /B build\autoedo.exe --port %PORT%

rem ── 3. Wait for the web UI (curl ships with Windows 10 1803+) ───────────
set UP=
for /L %%i in (1,1,30) do (
    curl -sf -o NUL --max-time 1 http://127.0.0.1:%PORT%/api/status >nul 2>&1
    if not errorlevel 1 (
        set UP=1
        goto :up
    )
    timeout /t 1 /nobreak >nul
)
:up
if not defined UP (
    echo ERROR: AutoEDO did not answer on port %PORT%.
    exit /b 1
)

echo AutoEDO Live is up: http://localhost:%PORT%/
start "" http://localhost:%PORT%/
echo Stop it later with: tools\autoedo.bat --stop
exit /b 0

:buildfail
echo ERROR: build failed - nothing was relaunched.
exit /b 1
