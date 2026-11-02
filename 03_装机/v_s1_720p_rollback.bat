@echo off
REM S1 acceptance - 720p rollback (BEAUTY_ALLZERO_SKIP=0, expect ~2.04 ms)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0v_s1.ps1" -Target s1 -Rollback
echo.
echo ---- done. press any key to close ----
pause >nul
