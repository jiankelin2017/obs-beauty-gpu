# =====================================================================
#  GPU tone / smooth diagnostic - install step  (requires Administrator)
#  1) backup current obs-beauty.dll
#  2) copy newly built obs-beauty.dll into obs-plugins\64bit
#  3) create data\obs-plugins\obs-beauty\shaders\ and copy EVERY *.hlsl
#
#  NOTE: this file is deliberately pure ASCII. PowerShell 5.1 decodes a
#  BOM-less .ps1 as ANSI, so any non-ASCII literal (e.g. a Chinese path)
#  would be mangled. All paths are derived from $PSScriptRoot instead.
# =====================================================================
$ErrorActionPreference = "Stop"

$src     = Split-Path -Parent $PSScriptRoot      # ...\OBS-beauty-GPU root
$logFile = Join-Path $PSScriptRoot "install_result.txt"
Remove-Item $logFile -Force -ErrorAction SilentlyContinue
Start-Transcript -Path $logFile -Force | Out-Null

$dllSrc = $null
$hit = Get-ChildItem $src -Recurse -Filter "obs-beauty.dll" -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -match '\\02_' } | Select-Object -First 1
if ($hit) { $dllSrc = $hit.FullName }

# 3.1.45: copy the whole shader folder instead of a single hard-coded file.
# Phase 3 added smooth.hlsl; hard-coding one name silently shipped a plugin
# whose effect failed to load (the diagnostic only logs a warning, so the
# only symptom is "nothing happened"). Enumerate instead of naming.
#
# The source dir name is Chinese, and this script must stay pure ASCII, so
# it is located via the ASCII-safe '01_*' prefix rather than a literal path.
$shaderSrcDir = $null
$cand = Get-ChildItem $src -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like '01_*' } | Select-Object -First 1
if ($cand) {
    $sd = Join-Path $cand.FullName "shaders"
    if (Test-Path $sd) { $shaderSrcDir = $sd }
}
$hlslFiles = @()
if ($shaderSrcDir) {
    $hlslFiles = @(Get-ChildItem $shaderSrcDir -Filter "*.hlsl" -ErrorAction SilentlyContinue)
}

$obsRoot = "C:\Program Files\obs-studio"
$plugDir = Join-Path $obsRoot "obs-plugins\64bit"
$dataDir = Join-Path $obsRoot "data\obs-plugins\obs-beauty\shaders"
$bakDir  = Join-Path $PSScriptRoot "backup"

Write-Output "src root : $src"
Write-Output "dll src  : $dllSrc"
Write-Output "hlsl dir : $shaderSrcDir"
foreach ($f in $hlslFiles) { Write-Output "  shader : $($f.Name)" }

if (-not (Test-Path $dllSrc))        { throw "MISSING dll source" }
if (-not $shaderSrcDir)              { throw "MISSING shader source dir" }
if ($hlslFiles.Count -eq 0)          { throw "MISSING *.hlsl under shader dir" }
if (-not (Test-Path $plugDir))       { throw "MISSING $plugDir  (wrong OBS install path?)" }

$proc = Get-Process obs64 -ErrorAction SilentlyContinue
if ($proc) { throw "obs64.exe is still running (pid $($proc.Id)). Close OBS first." }

New-Item -ItemType Directory -Force -Path $bakDir  | Out-Null
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null

$ts     = Get-Date -Format "yyyyMMdd_HHmmss"
$dllDst = Join-Path $plugDir "obs-beauty.dll"
if (Test-Path $dllDst) {
    $bak = Join-Path $bakDir "obs-beauty_$ts.dll"
    Copy-Item $dllDst $bak -Force
    Write-Output "backup   : $bak"
}

Copy-Item $dllSrc $dllDst -Force
foreach ($f in $hlslFiles) {
    Copy-Item $f.FullName (Join-Path $dataDir $f.Name) -Force
}

# ---- verify -----------------------------------------------------------
$d1 = Get-Item $dllDst
Write-Output "dll      : $($d1.FullName)  $($d1.Length) B  $($d1.LastWriteTime)"
foreach ($f in $hlslFiles) {
    $d2 = Get-Item (Join-Path $dataDir $f.Name)
    Write-Output "shader   : $($d2.FullName)  $($d2.Length) B  $($d2.LastWriteTime)"
}

$expSize = (Get-Item $dllSrc).Length
if ($d1.Length -ne $expSize) { throw "dll size mismatch: $($d1.Length) != $expSize" }

$h1 = (Get-FileHash $dllSrc  -Algorithm MD5).Hash
$h2 = (Get-FileHash $dllDst  -Algorithm MD5).Hash
if ($h1 -ne $h2) { throw "dll hash mismatch after copy" }

Write-Output "MD5 dll  : $h1"
Write-Output "INSTALL OK"
Stop-Transcript | Out-Null
