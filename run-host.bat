@echo off
title rPlayHub Host Daemon
cd /d "%~dp0"

echo ===================================================
echo   rPlayHub iOS Host Daemon
echo ===================================================

set RPLAY_USERSPACE_NET=1
if not defined RPLAY_KEYFRAME_EVERY_S set RPLAY_KEYFRAME_EVERY_S=0
if not defined RPLAY_FPS set RPLAY_FPS=60
if not defined RPLAY_MAX_BITRATE set RPLAY_MAX_BITRATE=15000000
if not defined RPLAY_DDI (
    if exist "%~dp0deps\iOS_DDI" (
        set "RPLAY_DDI=%~dp0deps\iOS_DDI"
    )
)

if exist "%~dp0host-c\cdhost.exe" (
    cd /d "%~dp0host-c"
    cdhost.exe %*
) else (
    echo Error: host-c\cdhost.exe not found!
    pause
)
