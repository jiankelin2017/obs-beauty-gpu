@echo off
setlocal EnableExtensions
set DBG=D:/obs_launch_debug_solo.txt
echo ============================================ > "%DBG%"
echo  OBS Beauty GPU launcher run @ %DATE% %TIME% >> "%DBG%"
echo  (engine=gpu, Phase 3.5 prod ON, SMOOTH selfcheck ONLY, NO TONE) >> "%DBG%"
echo ============================================ >> "%DBG%"

echo [1/3] Killing any running OBS (so env vars apply)...
taskkill /IM obs64.exe /F 2>nul
if errorlevel 1 (echo    no OBS running, skip >> "%DBG%") else (echo    OBS killed >> "%DBG%")
timeout /t 2 /nobreak >nul
echo.

echo [2/3] Setting GPU flags (SMOOTH selfcheck ONLY - proving no TONE dependency)...
set BEAUTY_ENGINE_IMPL=gpu
set BEAUTY_SHADER_DIR=D:/obs_beauty_gpu/shaders
set BEAUTY_PROFILE=1
set BEAUTY_GPU_SMOOTH=1
echo   BEAUTY_ENGINE_IMPL = %BEAUTY_ENGINE_IMPL% >> "%DBG%"
echo   BEAUTY_SHADER_DIR  = %BEAUTY_SHADER_DIR% >> "%DBG%"
echo   BEAUTY_PROFILE     = %BEAUTY_PROFILE% >> "%DBG%"
echo   BEAUTY_GPU_SMOOTH  = 1 (SMOOTH selfcheck ONLY, no TONE) >> "%DBG%"
echo.

echo [3/3] Launching OBS ...
cd /D "C:/Program Files/obs-studio/bin/64bit"
echo   CWD = %CD% >> "%DBG%"
if not exist "obs64.exe" (echo [ERR] obs64.exe NOT found & echo [ERR] obs64.exe NOT found >> "%DBG%")
start "" "C:/Program Files/obs-studio/bin/64bit/obs64.exe"
echo   OBS launch requested at %TIME% >> "%DBG%"
timeout /t 6 /nobreak >nul
tasklist | find /I "obs64.exe" >nul && (echo [OK] OBS is running. & echo [OK] OBS running after 6s >> "%DBG%") || (echo [WARN] OBS exited immediately. & echo [WARN] OBS NOT running >> "%DBG%")
echo.
echo   Debug log written to %DBG%
endlocal
