@echo off
setlocal EnableExtensions
set DBG=D:/obs_launch_cpu.txt
echo ============================================ > "%DBG%"
echo  OBS Beauty CPU baseline run @ %DATE% %TIME% >> "%DBG%"
echo  (engine=core = pure CPU path, for A/B vs GPU mode) >> "%DBG%"
echo ============================================ >> "%DBG%"

echo ============================================
echo  OBS Beauty CPU BASELINE launcher (engine=core)
echo  Use this to compare CPU usage vs the GPU launcher.
echo ============================================
echo.

echo [1/3] Killing any running OBS...
taskkill /IM obs64.exe /F 2>nul
if errorlevel 1 (echo    no OBS running, skip & echo    no OBS running, skip >> "%DBG%") else (echo    OBS killed, wait 2s & echo    OBS killed >> "%DBG%")
timeout /t 2 /nobreak >nul
echo.

echo [2/3] Setting pure-CPU mode (engine=core, no GPU flags)...
set BEAUTY_ENGINE_IMPL=core
set BEAUTY_PROFILE=1
echo   BEAUTY_ENGINE_IMPL      = %BEAUTY_ENGINE_IMPL% >> "%DBG%"
echo   BEAUTY_PROFILE          = %BEAUTY_PROFILE% >> "%DBG%"
echo   (all GPU flags intentionally NOT set) >> "%DBG%"
echo.

echo [3/3] Launching OBS ...
cd /D "C:/Program Files/obs-studio/bin/64bit"
echo   CWD = %CD%
echo   CWD=%CD% >> "%DBG%"
if not exist "obs64.exe" (echo   [ERR] obs64.exe NOT found & echo   [ERR] obs64.exe NOT found >> "%DBG%")
echo   Verify in OBS log: engine=core/1.6.0, region=on, core=N ms (N > 0)
start "" "C:/Program Files/obs-studio/bin/64bit/obs64.exe"
echo   OBS launch requested at %TIME% >> "%DBG%"
timeout /t 6 /nobreak >nul
tasklist | find /I "obs64.exe" >nul && (echo   [OK] OBS is running. & echo   [OK] OBS is running after 6s >> "%DBG%") || (echo   [WARN] OBS exited. & echo   [WARN] OBS NOT running after 6s >> "%DBG%")
echo.
echo   Debug log written to %DBG%
echo   Done. Press any key to close this window...
pause >nul
endlocal
