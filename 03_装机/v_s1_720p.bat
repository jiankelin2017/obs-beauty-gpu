@echo off
REM S1 acceptance - 720p passthrough (all-zero params, expect ~0 ms)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0v_s1.ps1" -Target s1
echo.
echo ---- done. press any key to close ----
pause >nul
