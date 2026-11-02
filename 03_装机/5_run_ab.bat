@echo off
REM =====================================================================
REM  T2 same-session A/B launcher (needs obs-beauty 3.1.57+).
REM  Normal user, no admin rights needed.
REM
REM  Why this file exists: double-clicking 2_run_diag.ps1 only flashes.
REM  Windows client default ExecutionPolicy is Restricted, so Explorer
REM  refuses the script and the window closes before the error can be
REM  read.  This launcher forces Bypass for the child process only; it
REM  does NOT change the machine-wide policy.
REM
REM  It runs BOTH resolutions, 240 s each:
REM    720p -> collection beauty_test      (camera, native 1280x720)
REM    4K   -> collection phase0_baseline  (3840x2160 canvas)
REM
REM  Why 240 s and not the 22 s default: one A/B arm lasts 180 frames
REM  (6 s at 30 fps, 3 s at 60 fps), so 240 s yields ~40 windows, i.e.
REM  ~20 per arm.  At 22 s you would get ~3 windows per arm and the
REM  medians would be noise.
REM
REM  OBS must be CLOSED between the two runs: the perf log is only final
REM  after obs64 exits, and 2_run_diag.ps1 refuses to launch while an
REM  obs64 process is still alive.
REM
REM  Pure ASCII + CRLF on purpose (see 1_install_diag.ps1 header).  The
REM  parser lives in a folder whose name is Chinese, so it is DISCOVERED
REM  through an ASCII wildcard instead of being written out - same trick
REM  build_gpu.ps1 uses.
REM =====================================================================
setlocal

REM Optional first argument = which switch the A/B flips.
REM   (none)  -> arms chain / 1seg   (T2, the 5a chain hook)
REM   out     -> arms out   / rb     (5b-1, the no-readback switch)
set "ABT=chain / 1seg"
set "ABARGS="
if /i "%~1"=="out" (
    set "ABT=out / rb"
    set "ABARGS=-AbTarget out"
)

set "PS1=%~dp02_run_diag.ps1"
if not exist "%PS1%" (
    echo MISSING 2_run_diag.ps1 next to this file
    pause
    exit /b 1
)

set "PARSER="
for /d %%d in ("%~dp0..\05_*") do (
    if exist "%%d\phase0_parse_log.py" set "PARSER=%%d\phase0_parse_log.py"
)

echo ============================================================
echo  A/B run 1/2 : 720p  (240 s)   arm pair: %ABT%
echo.
echo  Do NOT touch the machine during the run.  Background load is
echo  exactly what this test exists to cancel out, so browsing or
echo  encoding in parallel would bias the result.
echo.
echo  OBS opens by itself.  Leave it running until this window
echo  prints the A/B table, then close OBS.
echo ============================================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Seconds 240 -Ab %ABARGS%

echo.
echo ---- A/B table (720p) ----
call :parse

echo.
echo ============================================================
echo  Run 1 finished.  CLOSE OBS NOW, then press any key.
echo  (The 4K run waits only 60 s for obs64 to disappear.)
echo ============================================================
pause >nul

echo.
echo ============================================================
echo  A/B run 2/2 : 4K  (240 s)   arm pair: %ABT%
echo ============================================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Seconds 240 -Collection phase0_baseline -Ab %ABARGS%

echo.
echo ---- A/B table (4K) ----
call :parse

echo.
echo ============================================================
echo  Both runs finished.  What to look at, per table:
echo    1) the two rows for each arm  (chain/1seg  or  out/rb)
echo    2) the  delta  row: positive = the "on" arm is faster
echo    3) with arm pair chain/1seg only: the arm self-check
echo       line must show "has chain=" on ALL chain windows and
echo       NONE on 1seg windows.  If wrong, the arm never
echo       flipped and the delta is meaningless.
echo  For 5b-1 also search the log for  "5b-1"  to see the
echo  one-shot draw-path pixel check (expect maxdiff=0, PASS).
echo  Close OBS, then press any key to close this window.
echo ============================================================
pause >nul
endlocal
exit /b 0

REM ---------------------------------------------------------------------
:parse
where python >nul 2>&1
if errorlevel 1 (
    echo [skip] python is not on PATH - run phase0_parse_log.py by hand
    goto :eof
)
if not defined PARSER (
    echo [skip] phase0_parse_log.py not found under ..\05_*
    goto :eof
)
python "%PARSER%"
goto :eof
