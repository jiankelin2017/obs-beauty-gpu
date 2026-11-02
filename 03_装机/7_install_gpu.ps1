# =====================================================================
#  GPU branch - full install step (requires Administrator)
#
#  Deploys BOTH halves of the GPU build to the runtime location:
#    1) obs-beauty.dll  -> %PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\
#    2) shaders\*.hlsl  -> %PROGRAMDATA%\obs-studio\plugins\obs-beauty\data\shaders\
#
#  WHY THE SHADERS MATTER (2.2.0 regression note):
#  The GPU engine loads every effect through obs_module_file(), which for a
#  user plugin resolves to <plugin root>\data\ (verified against the
#  obs-studio/plugins/%module% + /bin/64bit + /data path strings inside
#  obs64.exe).  If shaders/*.hlsl are missing there, EVERY GPU segment
#  (smooth/tone/detail/warp/mask/region) silently falls back to CPU:
#  engine time jumps from ~1 ms to ~12 ms per frame @720p, the Phase 5a
#  chain hook claims nothing, and A/B runs become meaningless (both arms
#  measure the CPU path).  The build script only produces the dll, so the
#  shaders must be installed by THIS script - never ship the dll alone.
#
#  BEAUTY_SHADER_DIR (dev override) still works and takes precedence; it
#  is only a development convenience and must not be the only copy.
#
#  NOTE: pure ASCII on purpose.  PowerShell 5.1 decodes a BOM-less .ps1
#  as ANSI, so Chinese literals would be mangled.  Source folders whose
#  names are Chinese are located through ASCII prefixes / wildcards.
# =====================================================================
$ErrorActionPreference = "Stop"

$here    = $PSScriptRoot
$srcRoot = Split-Path -Parent $here          # ...\OBS-beauty-GPU root

# --- locate the freshly built dll (content probe, ASCII-safe) ----------
$dllSrc = $null
$hit = Get-ChildItem $srcRoot -Recurse -Filter "obs-beauty.dll" -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -match '\\02_' } | Select-Object -First 1
if ($hit) { $dllSrc = $hit.FullName }

# --- locate the shader sources (01_* prefix probe) ----------------------
$shaderSrc = $null
$cand = Get-ChildItem $srcRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like '01_*' } | Select-Object -First 1
if ($cand) {
    $sd = Join-Path $cand.FullName "shaders"
    if (Test-Path $sd) { $shaderSrc = $sd }
}
$hlsl = @()
if ($shaderSrc) { $hlsl = @(Get-ChildItem $shaderSrc -Filter "*.hlsl" -ErrorAction SilentlyContinue) }

# --- runtime locations ---------------------------------------------------
$plugRoot = "C:\ProgramData\obs-studio\plugins\obs-beauty"
$dllDst   = Join-Path $plugRoot "bin\64bit\obs-beauty.dll"
$dataDir  = Join-Path $plugRoot "data\shaders"
$bakDir   = Join-Path $here "backup"

if (-not (Test-Path $dllSrc)) { throw "MISSING dll source under 02_*" }
if (-not $shaderSrc)          { throw "MISSING shader source dir (01_*\shaders)" }
if ($hlsl.Count -eq 0)        { throw "MISSING *.hlsl under $shaderSrc" }

$proc = Get-Process obs64 -ErrorAction SilentlyContinue
if ($proc) { throw "obs64.exe is still running (pid $($proc.Id)). Close OBS first." }

# --- backup + install dll ------------------------------------------------
New-Item -ItemType Directory -Force -Path $bakDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dllDst) | Out-Null
$ts = Get-Date -Format "yyyyMMdd_HHmmss"
if (Test-Path $dllDst) {
    $bak = Join-Path $bakDir "obs-beauty_$ts.dll"
    Copy-Item $dllDst $bak -Force
    Write-Output "backup   : $bak"
}
Copy-Item $dllSrc $dllDst -Force

# --- install shaders (the half that used to be forgotten) ----------------
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
foreach ($f in $hlsl) { Copy-Item $f.FullName (Join-Path $dataDir $f.Name) -Force }

# --- verify ---------------------------------------------------------------
$d1 = Get-FileHash $dllSrc -Algorithm MD5
$d2 = Get-FileHash $dllDst -Algorithm MD5
Write-Output "dll      : $dllDst"
Write-Output "dll md5  : $($d2.Hash)  (build $($d1.Hash))  match=$($d1.Hash -eq $d2.Hash)"
Write-Output "shaders  : $dataDir"
foreach ($f in $hlsl) {
    $p = Join-Path $dataDir $f.Name
    $ok = (Test-Path $p) -and $((Get-FileHash $p -Algorithm MD5).Hash -eq (Get-FileHash $f.FullName -Algorithm MD5).Hash)
    Write-Output ("  {0,-12} {1}" -f $f.Name, $(if ($ok) { "OK" } else { "MISMATCH" }))
}
if ($d1.Hash -ne $d2.Hash) { throw "dll md5 mismatch after copy" }
Write-Output "INSTALL OK"
