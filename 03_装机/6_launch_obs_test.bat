@echo off
REM =====================================================================
REM  Launch OBS with the two opt-in GPU features ENABLED (for testing).
REM  Pure ASCII + CRLF on purpose (project convention for .bat / .ps1).
REM
REM  What this turns on:
REM    BEAUTY_ENGINE_IMPL=gpu    GPU engine (MANDATORY - see 2.1.1 note)
REM    BEAUTY_SHADER_DIR=...     shader weight-free install dir
REM    BEAUTY_GPU_REGION=1      GPU five-region segment (mouth/law-line/spot)
REM    BEAUTY_GPU_FRAME_RING=1  VRAM frame ring (skips the aligned upload)
REM    BEAUTY_REGION_CMP=1      one-shot GPU-vs-CPU region diff line in the log
REM    BEAUTY_PROFILE=1         the 'perf' / 'seg' lines every 6 s
REM
REM  2.1.1 TRAP FIX: this launcher used to set ONLY the feature flags and
REM  forgot BEAUTY_ENGINE_IMPL=gpu. The engine is chosen ONLY by env at
REM  module load (createEngine() defaults to CoreEngine when unset - see
REM  beauty_core.cpp), so running this file silently produced a CPU-engine
REM  session. That is exactly how the 09:37 run ended up engine=core and
REM  confused the GPU/CPU question. Every GPU launcher must set the engine.
REM  A debug file is written too, so "which launcher ran?" is always
REM  answerable from D:\obs_launch_*.txt.
REM
REM  If OBS crashes: a new file appears in
REM    %%APPDATA%%\obs-studio\crashes\
REM  Then just close this window; the plugin defaults (both features OFF)
REM  are safe again on the next normal OBS start.
REM =====================================================================
set DBG=D:/obs_launch_feat.txt
echo ============================================================ >> "%DBG%"
echo  OBS Beauty GPU feature-test launcher run @ %DATE% %TIME% >> "%DBG%"
echo  (engine=gpu + REGION + FRAME_RING + CMP + PROFILE) >> "%DBG%"
echo ============================================================ >> "%DBG%"
set BEAUTY_ENGINE_IMPL=gpu
set BEAUTY_SHADER_DIR=D:/obs_beauty_gpu/shaders
set BEAUTY_GPU_REGION=1
set BEAUTY_GPU_FRAME_RING=1
set BEAUTY_REGION_CMP=1
set BEAUTY_PROFILE=1
echo   BEAUTY_ENGINE_IMPL = %BEAUTY_ENGINE_IMPL% >> "%DBG%"
echo   BEAUTY_SHADER_DIR  = %BEAUTY_SHADER_DIR% >> "%DBG%"
echo   BEAUTY_GPU_REGION  = %BEAUTY_GPU_REGION% >> "%DBG%"
echo   BEAUTY_GPU_FRAME_RING = %BEAUTY_GPU_FRAME_RING% >> "%DBG%"
echo   BEAUTY_REGION_CMP  = %BEAUTY_REGION_CMP% >> "%DBG%"
echo   BEAUTY_PROFILE     = %BEAUTY_PROFILE% >> "%DBG%"
cd /d "C:\Program Files\obs-studio\bin\64bit"
start "" obs64.exe
echo   OBS launch requested at %TIME% >> "%DBG%"