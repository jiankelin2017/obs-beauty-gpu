@echo off
REM =====================================================================
REM  4K diagnostic launcher (normal user, no admin needed)
REM
REM  Same as 2_run_diag.bat but loads the phase0_baseline collection, whose
REM  canvas is 3840x2160 with the beauty filter on the SCENE source.  The
REM  camera stays at its native 1280x720 and is shown centred, so mask
REM  coverage is low (~2%) and this run measures PIXEL SCALE only - that is
REM  exactly what we need to tell whether the GPU smoothing segment scales
REM  linearly with resolution.
REM
REM  Generate the collection first (OBS closed is not required, but the new
REM  file only shows up in the collection list after the next start):
REM      python 05_??\phase0_prep.py --mode scene --canvas 3840x2160
REM
REM  Pure ASCII + CRLF on purpose (see 1_install_diag.ps1 header).
REM =====================================================================
setlocal

if not exist "%~dp02_run_diag.ps1" (
    echo MISSING 2_run_diag.ps1 next to this file
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp02_run_diag.ps1" -Collection phase0_baseline

echo.
echo ---- done. press any key to close ----
pause >nul
endlocal
