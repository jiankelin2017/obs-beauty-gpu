# =====================================================================
#  Phase 0 baseline runner  (normal user, no admin needed)
#
#  Launches OBS with BEAUTY_PROFILE=1 on a given scene collection, lets it
#  preview for N seconds, closes it gracefully, then prints the profiler
#  profiler lines (download / engine / upload, in ms per frame) from the
#  fresh log.
#
#  The CPU path is what we want here, so BEAUTY_GPU / BEAUTY_GPU_TONE /
#  BEAUTY_ENGINE_IMPL are explicitly CLEARED. If they leak in from the
#  parent shell the engine switches to GpuEngine and the numbers change
#  meaning.
#
#  NOTE: pure ASCII on purpose (see 1_install_diag.ps1 header).
# =====================================================================
param(
    [string]$Collection = "phase0_baseline",
    [int]$Seconds = 45
)

$ErrorActionPreference = "Stop"

$obsRoot = "C:\Program Files\obs-studio"
$exe     = Join-Path $obsRoot "bin\64bit\obs64.exe"
$bin     = Join-Path $obsRoot "bin\64bit"
$logDir  = Join-Path $env:APPDATA "obs-studio\logs"

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "obs64.exe already running. Close it first, then run this again."
}
if (-not (Test-Path $exe)) { throw "MISSING $exe" }

# profiler on, GPU switches off (Phase 0 measures the CPU baseline)
$env:BEAUTY_PROFILE = "1"
Remove-Item Env:\BEAUTY_GPU         -ErrorAction SilentlyContinue
Remove-Item Env:\BEAUTY_GPU_TONE    -ErrorAction SilentlyContinue
Remove-Item Env:\BEAUTY_ENGINE_IMPL -ErrorAction SilentlyContinue
Remove-Item Env:\BEAUTY_SHADER_DIR  -ErrorAction SilentlyContinue

$before = @(Get-ChildItem $logDir -Filter *.txt -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Name)

Write-Output "launching OBS: collection=$Collection seconds=$Seconds profile=1 (CPU baseline)"
Start-Process -FilePath $exe -ArgumentList "--collection", $Collection -WorkingDirectory $bin

Start-Sleep -Seconds $Seconds

# graceful close so the log is flushed and no config is lost
$p = Get-Process obs64 -ErrorAction SilentlyContinue
if ($p) {
    $p | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-Process obs64 -ErrorAction SilentlyContinue)) { break }
    }
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
        Write-Output "WARN: OBS did not close in 10s, stopping it"
        Get-Process obs64 | Stop-Process -Force
        Start-Sleep -Seconds 2
    }
}
Write-Output "OBS closed"

$log = Get-ChildItem $logDir -Filter *.txt |
       Where-Object { $before -notcontains $_.Name } |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1
if (-not $log) { throw "no new log file appeared in $logDir" }

Write-Output ""
Write-Output "log: $($log.FullName)"
Write-Output ""
Write-Output "---- engine / resolution actually used ----"
Select-String -Path $log.FullName -Pattern 'init OK|render#0|DShow Device|resolution:' |
    ForEach-Object { $_.Line } | Select-Object -First 12
Write-Output ""
Write-Output "---- profiler lines ----"
$hit = Select-String -Path $log.FullName -Pattern 'ms/'
if ($hit) { $hit | ForEach-Object { $_.Line } }
else { Write-Output "(NOT FOUND - filter not rendering, or params all zero -> engine skipped)" }
