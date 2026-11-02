@echo off
setlocal EnableExtensions
set DBG=D:/obs_launch_features.txt
echo ============================================ > "%DBG%"
echo  OBS Beauty GPU feature-test launcher @ %DATE% %TIME% >> "%DBG%"
echo  (engine=gpu + REGION + FRAME_RING opt-in, stability test) >> "%DBG%"
echo ============================================ >> "%DBG%"

echo ============================================
echo  OBS Beauty GPU feature test (v3.1.76)
echo ============================================
echo.

echo [1/3] Killing any running OBS (so env vars apply)...
taskkill /IM obs64.exe /F 2>nul
if errorlevel 1 (echo    no OBS running, skip) else (echo    OBS killed, wait 2s)
timeout /t 2 /nobreak >nul
echo.

echo [2/3] Setting GPU flags + opt-in features under test...
set BEAUTY_ENGINE_IMPL=gpu
set BEAUTY_SHADER_DIR=D:/obs_beauty_gpu/shaders
set BEAUTY_PROFILE=1
set BEAUTY_GPU_REGION=1
set BEAUTY_GPU_FRAME_RING=1
echo   BEAUTY_ENGINE_IMPL     = %BEAUTY_ENGINE_IMPL% >> "%DBG%"
echo   BEAUTY_SHADER_DIR      = %BEAUTY_SHADER_DIR% >> "%DBG%"
echo   BEAUTY_GPU_REGION      = %BEAUTY_GPU_REGION% >> "%DBG%"
echo   BEAUTY_GPU_FRAME_RING  = %BEAUTY_GPU_FRAME_RING% >> "%DBG%"
echo.
echo   NOTE: REGION (4d) + FRAME_RING (5b-2) are opt-in. They crashed on
echo   4K / RTX 5060 before, so this launcher is for a stability check only.
echo   If OBS crashes, run run_obs_gpu_test.bat (which does NOT enable these)
echo   to get back to the known-good state.
echo.

echo [3/3] Launching OBS ...
cd /D "C:/Program Files/obs-studio/bin/64bit"
if not exist "obs64.exe" (echo   [ERR] obs64.exe NOT found)
start "" "C:/Program Files/obs-studio/bin/64bit/obs64.exe"
echo   OBS launch requested at %TIME% >> "%DBG%"
timeout /t 6 /nobreak >nul
tasklist | find /I "obs64.exe" >nul && (echo   [OK] OBS is running.) || (echo   [WARN] OBS exited immediately - feature may be unstable on this GPU.)
echo.
echo   Debug log written to %DBG%
echo   Done.
endlocal
