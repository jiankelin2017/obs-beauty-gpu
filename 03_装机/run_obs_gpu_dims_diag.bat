@echo off
setlocal EnableExtensions
set DBG=D:/obs_launch_dims_diag.txt
echo ============================================ > "%DBG%"
echo  OBS Beauty GPU launcher run @ %DATE% %TIME% >> "%DBG%"
echo  (engine=gpu, GPU smooth ON, DIAG_DIMS=ON) >> "%DBG%"
echo ============================================ >> "%DBG%"

echo ============================================
echo  OBS Beauty GPU launcher (v2.0.0, DIAG_DIMS=ON)
echo  Purpose: print final-draw texture dimensions every 180 frames
echo  to confirm whether a half-res RT is drawn at full 1920x1080.
echo ============================================
echo.

echo [1/3] Killing any running OBS (so env vars apply)...
taskkill /IM obs64.exe /F 2>nul
if errorlevel 1 (echo    no OBS running, skip & echo    no OBS running, skip >> "%DBG%") else (echo    OBS killed, wait 2s & echo    OBS killed >> "%DBG%")
timeout /t 2 /nobreak >nul
echo.

echo [2/3] Setting GPU flags (engine=gpu + GPU smooth ON + DIAG_DIMS=ON)...
set BEAUTY_ENGINE_IMPL=gpu
set BEAUTY_SHADER_DIR=D:/obs_beauty_gpu/shaders
set BEAUTY_PROFILE=1
set BEAUTY_GPU_SMOOTH_PROD=1
set BEAUTY_GPU_SMOOTH=1
set BEAUTY_DIAG_DIMS=1
echo   BEAUTY_ENGINE_IMPL     = %BEAUTY_ENGINE_IMPL% >> "%DBG%"
echo   BEAUTY_SHADER_DIR      = %BEAUTY_SHADER_DIR% >> "%DBG%"
echo   BEAUTY_PROFILE         = %BEAUTY_PROFILE% >> "%DBG%"
echo   BEAUTY_GPU_SMOOTH_PROD = 1 >> "%DBG%"
echo   BEAUTY_GPU_SMOOTH      = 1 >> "%DBG%"
echo   BEAUTY_DIAG_DIMS       = 1 (final-draw dims every 180 frames) >> "%DBG%"
echo.

echo [3/3] Launching OBS ...
echo   CWD MUST be OBS bin dir or OBS throws 'Failed to load locale'.
cd /D "C:/Program Files/obs-studio/bin/64bit"
echo   CWD = %CD%
echo   CWD=%CD% >> "%DBG%"
if not exist "obs64.exe" (echo   [ERR] obs64.exe NOT found & echo   [ERR] obs64.exe NOT found >> "%DBG%")
echo   After ~3s, drag 磨皮 slider 0 -> >0 and look for log lines:
echo     [obs-beauty] DIAG_DIMS draw=WxH target=WxH chain_direct=N chain_out_tex=WxH out_tex=WxH
echo   If draw or chain_out_tex = 960x540 while target = 1920x1080 -> half-res RT drawn full -> BUG A.
echo   Launching obs64.exe ...
start "" "C:/Program Files/obs-studio/bin/64bit/obs64.exe"
echo   OBS launch requested at %TIME% >> "%DBG%"
timeout /t 6 /nobreak >nul
tasklist | find /I "obs64.exe" >nul && (echo   [OK] OBS is running. & echo   [OK] OBS is running after 6s >> "%DBG%") || (echo   [WARN] OBS exited immediately. & echo   [WARN] OBS NOT running after 6s >> "%DBG%")
echo.
echo   Debug log written to %DBG%
endlocal
