@echo off
REM =====================================================================
REM  GPU smooth diagnostic - launcher (normal user, no admin needed)
REM
REM  Why this file exists: a bare .ps1 cannot be started by double-click.
REM  Windows client default ExecutionPolicy is Restricted (all scopes are
REM  Undefined on this machine), so Explorer refuses the script and the
REM  window closes before the error can be read. This launcher forces
REM  Bypass for this one child process only; it does NOT change the
REM  machine-wide policy and does NOT need administrator rights.
REM
REM  Pure ASCII + CRLF on purpose (see 1_install_diag.ps1 header).
REM =====================================================================
setlocal

if not exist "%~dp02_run_diag.ps1" (
    echo MISSING 2_run_diag.ps1 next to this file
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp02_run_diag.ps1"

echo.
echo ---- done. press any key to close ----
pause >nul
endlocal
