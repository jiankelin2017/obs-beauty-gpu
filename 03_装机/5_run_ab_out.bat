@echo off
REM =====================================================================
REM  5b-1 A/B launcher (needs obs-beauty 3.1.58+).
REM
REM  Thin wrapper: identical to 5_run_ab.bat except that the flipped
REM  switch is BEAUTY_CHAIN_OUT instead of BEAUTY_CHAIN_PROD, so the two
REM  arms are:
REM     out -> chain tail draws straight from the hook's texture
REM     rb  -> chain tail reads the texture back, re-uploads, then draws
REM  i.e. it measures exactly what removing the readback is worth.
REM
REM  Why a wrapper instead of a .ps1: double-clicking a .ps1 only flashes
REM  (default ExecutionPolicy is Restricted).  See 5_run_ab.bat header.
REM
REM  Pure ASCII + CRLF on purpose.  Keep it that way.
REM =====================================================================
call "%~dp05_run_ab.bat" out
