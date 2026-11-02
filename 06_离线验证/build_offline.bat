@echo off
rem Double-click launcher for build_offline.ps1.
rem
rem Why a .bat wrapper: this machine has ExecutionPolicy = Undefined in every
rem scope, so double-clicking a .ps1 is refused with "running scripts is
rem disabled". -ExecutionPolicy Bypass on the command line is allowed.
rem
rem Pure ASCII + CRLF on purpose (same reason as 03_*/2_run_diag.bat).
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_offline.ps1"
echo.
echo ---- done. press any key to close ----
pause >nul
