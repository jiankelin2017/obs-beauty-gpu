# run_obs_gpu_dims_diag.ps1 - launch OBS with DIAG_DIMS=1 (pure ASCII on purpose)
$ErrorActionPreference = "Stop"
$dbg = "D:\obs_launch_dims_diag.txt"
try {
    "============================================" | Out-File -FilePath $dbg -Encoding ascii
    "OBS Beauty GPU dims-diag launcher @ $(Get-Date)" | Out-File -Append $dbg
    "engine=gpu, GPU smooth ON, DIAG_DIMS=ON" | Out-File -Append $dbg
} catch { }

# 1) kill any running OBS so the env vars apply to a fresh process
$proc = Get-Process obs64 -ErrorAction SilentlyContinue
if ($proc) {
    $proc | Stop-Process -Force
    Start-Sleep -Seconds 2
    "OBS killed" | Out-File -Append $dbg
} else {
    "no OBS running" | Out-File -Append $dbg
}

# 2) set env (inherited by the launched OBS)
$env:BEAUTY_ENGINE_IMPL      = "gpu"
$env:BEAUTY_SHADER_DIR       = "D:/obs_beauty_gpu/shaders"
$env:BEAUTY_PROFILE          = "1"
$env:BEAUTY_GPU_SMOOTH_PROD  = "1"
$env:BEAUTY_GPU_SMOOTH       = "1"
$env:BEAUTY_DIAG_DIMS        = "1"
"env set: BEAUTY_DIAG_DIMS=$($env:BEAUTY_DIAG_DIMS) PROD=$($env:BEAUTY_GPU_SMOOTH_PROD)" | Out-File -Append $dbg

# 3) launch OBS from its bin dir (CWD matters: 'Failed to load locale' otherwise)
$bin = "C:\Program Files\obs-studio\bin\64bit"
Set-Location $bin
if (-not (Test-Path obs64.exe)) { throw "obs64.exe NOT found at $bin" }
Start-Process obs64.exe
"OBS launch requested at $(Get-Date)" | Out-File -Append $dbg

Start-Sleep -Seconds 6
$p = Get-Process obs64 -ErrorAction SilentlyContinue
if ($p) { "OBS is running after 6s" | Out-File -Append $dbg } else { "WARN: OBS NOT running after 6s" | Out-File -Append $dbg }
Write-Output "launched. debug log: $dbg"
