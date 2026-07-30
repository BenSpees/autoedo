@echo off
setlocal
rem AutoEDO one-stop launcher for Windows 10: rebuild if a make is on PATH,
rem restart the service, health-check, open the web UI.
rem
rem   autoedo.bat            build (if possible) + relaunch + open browser
rem   autoedo.bat --no-ui    same, but never touch the browser (for use by
rem                          an external controller app)
rem   autoedo.bat --stop     stop the instance on this port
rem   set AUTOEDO_PORT=9000 first to use another port; set AUTOEDO_CONFIG to
rem   give the instance its own settings file (multi-instance rigs need both)
rem
rem Building needs the MSYS2 mingw-w64 toolchain (pacman -S make
rem mingw-w64-x86_64-gcc) — run from an "MSYS2 MinGW x64" shell with `make`,
rem or put mingw32-make/make on PATH and double-click this.

set PORT=%AUTOEDO_PORT%
if "%PORT%"=="" set PORT=8017
set CFGARG=
if not "%AUTOEDO_CONFIG%"=="" set CFGARG=--config "%AUTOEDO_CONFIG%"
cd /d "%~dp0.."

if "%~1"=="--stop" (
    call :stopport
    echo stopped.
    exit /b 0
)
set OPEN_UI=1
if "%~1"=="--no-ui" set OPEN_UI=

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

rem ── 2. Stop what's running ON THIS PORT, relaunch detached ──────────────
rem A rig can run several instances (voice on 8017, guitar on 8018, ...);
rem killing by image name would take the other channel's engine down too,
rem so the stop is scoped to whatever is listening on our port.
call :stopport
start "" /B build\autoedo.exe --port %PORT% %CFGARG%

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
if defined OPEN_UI start "" http://localhost:%PORT%/
echo Stop it later with: tools\autoedo.bat --stop
exit /b 0

:buildfail
echo ERROR: build failed - nothing was relaunched.
exit /b 1

:stopport
rem Kill only the process listening on %PORT% (per-instance stop). The
rem state word is localized ("LISTENING" is "ABHOEREN" on a German
rem system), so match the listener's SHAPE instead: a TCP listen row is
rem the only one whose foreign address port is 0 (0.0.0.0:0 or [::]:0).
rem An established row to/from the port never carries ":0 ", so clients
rem of the engine (health checks, controllers) are never touched.
for /f "tokens=5" %%p in ('netstat -ano -p TCP ^| findstr /C:":%PORT% " ^| findstr /C:":0 "') do (
    taskkill /F /PID %%p >nul 2>&1
)
goto :eof
