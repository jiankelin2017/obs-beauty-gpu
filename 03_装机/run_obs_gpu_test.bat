@echo off
setlocal EnableExtensions
set DBG=D:/obs_launch_debug.txt
echo ============================================ > "%DBG%"
echo  OBS Beauty GPU launcher run @ %DATE% %TIME% >> "%DBG%"
echo  (engine=gpu, Phase 5a/5b GPU path, NO Phase 3.5 prod) >> "%DBG%"
echo ============================================ >> "%DBG%"

echo ============================================
echo  OBS Beauty GPU launcher (v3.1.76, verified working)
echo ============================================
echo.

echo [1/3] Killing any running OBS (so env vars apply)...
taskkill /IM obs64.exe /F 2>nul
if errorlevel 1 (echo    no OBS running, skip & echo    no OBS running, skip >> "%DBG%") else (echo    OBS killed, wait 2s & echo    OBS killed >> "%DBG%")
timeout /t 2 /nobreak >nul
echo.

echo [2/3] Setting GPU flags (engine=gpu + Phase 3.5 prod ON for this test)...
set BEAUTY_ENGINE_IMPL=gpu
set BEAUTY_SHADER_DIR=D:/obs_beauty_gpu/shaders
set BEAUTY_PROFILE=1
set BEAUTY_GPU_SMOOTH_PROD=1
echo   BEAUTY_ENGINE_IMPL      = %BEAUTY_ENGINE_IMPL% >> "%DBG%"
echo   BEAUTY_SHADER_DIR       = %BEAUTY_SHADER_DIR% >> "%DBG%"
echo   BEAUTY_PROFILE          = %BEAUTY_PROFILE% >> "%DBG%"
echo   BEAUTY_GPU_SMOOTH_PROD  = 1 (Phase 3.5 prod ON; 16F fix + SEH fallback) >> "%DBG%"
echo   BEAUTY_CHAIN_OUT        = NOT SET (defaults ON = 5b-1 no-readback) >> "%DBG%"
echo.
echo   Note: Phase 5a segment hook + 5b-1 chain-out are enabled by default
echo   when engine=gpu. They do the GPU smoothing. core=0.0ms is expected.
echo.

echo [3/3] Launching OBS ...
echo   CWD MUST be OBS bin dir or OBS throws 'Failed to load locale'.
cd /D "C:/Program Files/obs-studio/bin/64bit"
echo   CWD = %CD%
echo   CWD=%CD% >> "%DBG%"
if not exist "obs64.exe" (echo   [ERR] obs64.exe NOT found & echo   [ERR] obs64.exe NOT found >> "%DBG%")
echo   Verify in OBS log: engine=gpu/2.0.0, region=on, shaders loaded, core~0.0ms
echo   Launching obs64.exe ...
start "" "C:/Program Files/obs-studio/bin/64bit/obs64.exe"
echo   OBS launch requested at %TIME% >> "%DBG%"
timeout /t 6 /nobreak >nul
tasklist | find /I "obs64.exe" >nul && (echo   [OK] OBS is running. & echo   [OK] OBS is running after 6s >> "%DBG%") || (echo   [WARN] OBS exited immediately. & echo   [WARN] OBS NOT running after 6s >> "%DBG%")
echo.
echo   Debug log written to %DBG%
echo   Done. Press any key to close this window...
pause >nul
endlocal
