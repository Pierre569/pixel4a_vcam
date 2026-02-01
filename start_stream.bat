@echo off
setlocal
echo Starting VCam Stream for Pixel 4a...

REM 1. ADB Forwarding
echo Setting up ADB forwarding (TCP 5555)...
adb forward tcp:5555 tcp:5555
if %errorlevel% neq 0 (
    echo Error: ADB Forwarding failed. Is the device connected?
    pause
    exit /b 1
)

REM 2. Check for FFmpeg
where ffmpeg >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: FFmpeg not found in PATH. Please install FFmpeg.
    pause
    exit /b 1
)

REM 3. Start Streaming (OBS Virtual Camera)
echo Starting FFmpeg Stream from 'OBS Virtual Camera'...
echo Press 'q' in this window to stop.

ffmpeg -f dshow -i video="OBS Virtual Camera" -c:v libx264 -preset ultrafast -tune zerolatency -f rawvideo tcp://127.0.0.1:5555

echo Stream stopped.
pause
