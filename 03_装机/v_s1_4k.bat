@echo off
REM S1 acceptance - 4K passthrough (scene-mode canvas 3840x2160, expect ~0 ms)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0v_s1.ps1" -Target 4k
echo.
echo ---- done. press any key to close ----
pause >nul
