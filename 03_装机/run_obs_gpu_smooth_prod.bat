@echo off
setlocal EnableExtensions
echo ============================================
echo  run_obs_gpu_smooth_prod.bat is RETIRED.
echo  Phase 3.5 GPU smooth is ON by default since v2.0.0
echo  (no env var needed; BEAUTY_GPU_SMOOTH_PROD=0 to disable).
echo  This now just launches the verified test launcher.
echo ============================================
echo.
call "%~dp0run_obs_gpu_test.bat"
endlocal
