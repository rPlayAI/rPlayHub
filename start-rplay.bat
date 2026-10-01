@echo off
cd /d "%~dp0"

set RPLAY_USERSPACE_NET=1
if not defined RPLAY_DDI (
    if exist "%~dp0deps\iOS_DDI" (
        set "RPLAY_DDI=%~dp0deps\iOS_DDI"
    )
)

:: Start cdhost daemon if not already running
tasklist /FI "IMAGENAME eq cdhost.exe" 2>NUL | find /I /N "cdhost.exe">NUL
if "%ERRORLEVEL%"=="1" (
    echo Starting rPlayHub host daemon...
    start "rPlayHub Host Daemon" /D "%~dp0host-c" "%~dp0host-c\cdhost.exe"
    timeout /t 2 /nobreak >nul
) else (
    echo rPlayHub host daemon is already running.
)

:: Start rPlayHub GUI
echo Launching rPlayHub GUI...
start "" "%~dp0rplay-gui.exe"
