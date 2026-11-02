@echo off
rem Double-click this file. It asks for admin rights, then runs 1_install_diag.ps1.
rem Paths are derived from %~dp0 so it works wherever the folder lives.
setlocal
set "PS1=%~dp01_install_diag.ps1"
if not exist "%PS1%" (
  echo MISSING: %PS1%
  pause
  exit /b 1
)
echo.
echo   Requesting administrator rights ...
echo   Click YES on the UAC prompt.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell.exe -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%PS1%'"
echo.
echo   Launcher finished. See install_result.txt for details.
timeout /t 3 >nul
