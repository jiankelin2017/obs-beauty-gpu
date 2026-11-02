@echo off
setlocal EnableExtensions
set DBG=D:/obs_launch_safe.txt
echo ============================================ > "%DBG%"
echo  OBS Beauty SAFE launcher run @ %DATE% %TIME% >> "%DBG%"
echo  (only engine=gpu, NO shader prod / no chain-out) >> "%DBG%"
echo ============================================ >> "%DBG%"

echo ============================================
echo  OBS Beauty SAFE launcher (engine=gpu only, no shader flags)
echo ============================================
echo.

echo [1/3] Killing any running OBS...
taskkill /IM obs64.exe /F 2>nul
if errorlevel 1 (echo    no OBS running, skip & echo    no OBS running, skip >> "%DBG%") else (echo    OBS killed, wait 2s & echo    OBS killed >> "%DBG%")
timeout /t 2 /nobreak >nul
echo.

echo [2/3] Setting ONLY engine=gpu (passthrough, no GPU shader work)...
set BEAUTY_ENGINE_IMPL=gpu
set BEAUTY_SHADER_DIR=D:/obs_beauty_gpu/shaders
echo   BEAUTY_ENGINE_IMPL=%BEAUTY_ENGINE_IMPL% >> "%DBG%"
echo   BEAUTY_GPU_SMOOTH_PROD=NOT SET >> "%DBG%"
echo   BEAUTY_CHAIN_OUT=NOT SET >> "%DBG%"
echo.

echo [3/3] Launching OBS ...
cd /D "C:/Program Files/obs-studio/bin/64bit"
echo   CWD=%CD% >> "%DBG%"
echo   CWD = %CD%
if not exist "obs64.exe" (echo   [ERR] obs64.exe NOT found & echo   [ERR] obs64.exe NOT found >> "%DBG%")
start "" "C:/Program Files/obs-studio/bin/64bit/obs64.exe"
echo   obs64.exe launch requested at %TIME% >> "%DBG%"
timeout /t 6 /nobreak >nul
tasklist | find /I "obs64.exe" >nul && (echo   [OK] OBS is running. & echo   [OK] OBS running after 6s >> "%DBG%") || (echo   [WARN] OBS exited immediately. & echo   [WARN] OBS NOT running after 6s >> "%DBG%")
echo.
echo   Done. Press any key to close this window...
pause >nul
endlocal
