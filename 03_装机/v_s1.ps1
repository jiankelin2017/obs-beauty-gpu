# =====================================================================
#  v_s1.ps1 - S1 acceptance launcher (ASCII only)
#
#  Runs the existing 2_run_diag.ps1 against a dedicated all-zero collection
#  (beauty_s1 / beauty_4k) so the S1 passthrough branch actually triggers,
#  then prints the gate table from the newest log.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File v_s1.ps1 -Target s1
#    powershell -ExecutionPolicy Bypass -File v_s1.ps1 -Target s1 -Rollback
#    powershell -ExecutionPolicy Bypass -File v_s1.ps1 -Target 4k
#
#  -Rollback sets BEAUTY_ALLZERO_SKIP=0 (old full-path behaviour, ~2.04 ms).
#  Without it the passthrough branch is active (all-zero params -> ~0 ms).
#
#  NOTE: pure ASCII on purpose - PS 5.1 decodes a BOM-less .ps1 as ANSI.
#  Every Chinese path is derived at runtime from $PSScriptRoot, never
#  written as a literal here.
# =====================================================================
param(
    [ValidateSet("s1", "4k")][string]$Target = "s1",
    [switch]$Rollback
)
$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$diag = Join-Path $here "2_run_diag.ps1"
if (-not (Test-Path $diag)) { throw "MISSING $diag" }

if ($Rollback) {
    $env:BEAUTY_ALLZERO_SKIP = "0"
    Write-Output "MODE       : rollback (BEAUTY_ALLZERO_SKIP=0)  -> expect ~2.04 ms @720p"
} else {
    Remove-Item Env:\BEAUTY_ALLZERO_SKIP -ErrorAction SilentlyContinue
    Write-Output "MODE       : passthrough (BEAUTY_ALLZERO_SKIP unset) -> expect ~0 ms"
}

if ($Target -eq "4k") { $col = "beauty_4k" } else { $col = "beauty_s1" }
Write-Output "COLLECTION : $col"
Write-Output ""

# 2_run_diag.ps1 waits for any running obs64 to exit (up to 60 s), sets
# BEAUTY_PROFILE=1 (plus other diagnostic flags), launches OBS on $col and
# prints the perf / seg / self-check lines after the wait window.
& $diag -Collection $col

# Gate table from the newest log. gate.py with no path auto-scans
# %APPDATA%\obs-studio\logs - avoids quoting a path that contains spaces.
#
# The verify folder name is Chinese; a literal here would be mangled by
# PS 5.1's ANSI decoding of a BOM-less .ps1. Probe it by ASCII prefix "05_"
# instead (same trick build_gpu.ps1 uses), never with a Chinese literal.
$root = Split-Path $here -Parent
$vdir = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like '05_*' } | Select-Object -First 1
if ($vdir) { $gate = Join-Path $vdir.FullName "gate.py" } else { $gate = $null }
if ($gate -and (Test-Path $gate)) {
    Write-Output ""
    Write-Output "================ GATE (auto newest log) ================"
    # Resolve a REAL python: plain "python" can hit the Microsoft Store app
    # execution alias, which only prints "Python was not found" when run
    # non-interactively. Prefer the py launcher, then a real install.
    $pyExe = $null; $pyArgs = @()
    if (Get-Command py -ErrorAction SilentlyContinue) {
        $pyExe = "py"; $pyArgs = @("-3")
    } else {
        $cand = "C:\Users\$env:USERNAME\AppData\Local\Programs\Python\Python312\python.exe"
        if (Test-Path $cand) { $pyExe = $cand } else { $pyExe = "python" }
    }
    & $pyExe @pyArgs $gate
} else {
    Write-Output ""
    Write-Output "gate.py not found: $gate"
}
