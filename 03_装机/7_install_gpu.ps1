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

# --- P2-1 / P2-3: shader copies must not drift -------------------------
# The 2.2.0 outage was caused by a stale smooth.hlsl being picked up from a
# second copy (source said RGBA16F, the packaged copy still said RGBA32F).
# There is exactly ONE hand-written source (01_*\shaders); everything else is
# generated from it:
#   * 07_*\shaders                -> build artifact of build_setup.ps1 (WARN if stale)
#   * D:\obs_beauty_gpu\shaders   -> NTFS junction into 01_*\shaders (see 8_link_dev_shaders.ps1)
#   * %PROGRAMDATA%\...\data\shaders -> written by THIS script
# Any *other* shaders folder inside the project is treated as a hand-made copy
# and drift there is fatal, because it could be picked up as an install source.
# Multiple identical copies are tolerated; only DRIFT in a hand-made copy is fatal.
function Get-ShaderSig([string]$dir) {
    $parts = @()
    foreach ($f in (Get-ChildItem $dir -Filter "*.hlsl" | Sort-Object Name)) {
        $parts += ($f.Name + ":" + (Get-FileHash $f.FullName -Algorithm MD5).Hash)
    }
    return ($parts -join "|")
}
$authSig = Get-ShaderSig $shaderSrc
$shaderCopies = @($shaderSrc)
$warnCopies = @()
foreach ($d in (Get-ChildItem $srcRoot -Directory -ErrorAction SilentlyContinue)) {
    $sd2 = Join-Path $d.FullName "shaders"
    if ((-not (Test-Path $sd2)) -or ($sd2 -eq $shaderSrc)) { continue }
    # P2-3: the installer payload (07_*) carries its own shaders\ folder, but that is a
    # BUILD ARTIFACT produced by 07_*/build_setup.ps1 and git-ignored - it is never an
    # install source. A stale copy there only means "the package has not been rebuilt",
    # so warn instead of failing: the authoritative install source is always 01_*/shaders.
    if ($d.Name -like '07_*') { $warnCopies += $sd2 } else { $shaderCopies += $sd2 }
}
# Solo dev copy: BEAUTY_SOLO_SHADER_DIR takes precedence; the hardcoded path is
# only the default.  The env var must win when both exist - otherwise a machine
# that happens to have D:\obs_beauty_gpu would ignore the override completely and
# the switch would be dead.  An env var pointing at a missing dir is skipped with
# a note rather than treated as fatal (it is a dev dir, not a deploy target), but
# the note always names the path actually used, so a typo shows up in the log
# instead of silently disabling the drift check.
$soloSd = $env:BEAUTY_SOLO_SHADER_DIR
if ([string]::IsNullOrWhiteSpace($soloSd)) {
    $soloSd = "D:\obs_beauty_gpu\shaders"
    Write-Output "solo dir : default $soloSd"
} else {
    Write-Output "solo dir : BEAUTY_SOLO_SHADER_DIR=$soloSd"
}
if (Test-Path $soloSd) {
    # P2-3: the solo dev dir is now a NTFS junction into 01_*/shaders, so the "second
    # copy" is literally the same bytes. Report it as a link instead of pretending it
    # is an independent copy (and skip the redundant hash pass).
    $soloItem = Get-Item -LiteralPath $soloSd -Force
    if ($soloItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        Write-Output "solo dir : junction -> $($soloItem.Target) (same files, not a copy)"
    } else {
        Write-Output "solo dir : real folder (copy - will verify it matches source)"
        $shaderCopies += $soloSd
    }
} else {
    Write-Output "solo dir : skipped (not found: $soloSd)"
}
$drift = @()
foreach ($sd in $shaderCopies) {
    if ((Get-ShaderSig $sd) -ne $authSig) { $drift += $sd }
}
if ($drift.Count -gt 0) {
    Write-Output "FATAL: shader copies drifted from source ($shaderSrc):"
    foreach ($sd in $drift) { Write-Output "  $sd" }
    throw "shader copies not identical to source - sync or delete the drifted copies before installing"
}
Write-Output "shader copies: $($shaderCopies.Count) (all identical to source)"
foreach ($sd in $warnCopies) {
    if ((Get-ShaderSig $sd) -ne $authSig) {
        Write-Output "WARN     : package payload is stale: $sd"
        Write-Output "           (re-run 07_*/build_setup.ps1 before shipping - install itself is unaffected)"
    }
}

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

# --- 3.2.4: the top-level "Beauty" menu panel ---------------------------
# obs-beauty-ui.dll is a SECOND, independent obs module (own
# OBS_DECLARE_MODULE) that registers the top-level menu. It is not part of
# this dll and cannot be -- see 02_*/build_ui.ps1. Optional here: a tree
# where only build_gpu.ps1 has run simply has no panel yet, and that must not
# block a dev install (the filter works fine without it).
$uiSrc = $null
$uiHit = Get-ChildItem $srcRoot -Recurse -Filter "obs-beauty-ui.dll" -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -match '\\02_' } | Select-Object -First 1
if ($uiHit) { $uiSrc = $uiHit.FullName }
if ($uiSrc) {
    $uiRoot = "C:\ProgramData\obs-studio\plugins\obs-beauty-ui"
    $uiDst  = Join-Path $uiRoot "bin\64bit\obs-beauty-ui.dll"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $uiDst) | Out-Null
    if (Test-Path $uiDst) {
        $ubak = Join-Path $bakDir "obs-beauty-ui_$ts.dll"
        Copy-Item $uiDst $ubak -Force
        Write-Output "ui backup: $ubak"
    }
    Copy-Item $uiSrc $uiDst -Force
    Write-Output "ui dll   : $uiDst"
    Write-Output "ui md5   : $((Get-FileHash $uiDst -Algorithm MD5).Hash)  (build $((Get-FileHash $uiSrc -Algorithm MD5).Hash))"
} else {
    Write-Output "ui dll   : skipped (no obs-beauty-ui.dll under 02_* - run 02_*/build_ui.ps1 for the top menu)"
}

# --- install shaders (the half that used to be forgotten) ----------------
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
foreach ($f in $hlsl) { Copy-Item $f.FullName (Join-Path $dataDir $f.Name) -Force }

# 2.15.1: legacy data dirs. Older installs left shader copies under
#   <plug>\bin\64bit\data\shaders, <plug>\bin\data\shaders,
#   <plug>\data\obs-plugins\<module>\shaders
# and obs_module_file() may resolve to any of them depending on the layout.
# A stale copy there is invisible until the DLL needs a NEW shader uniform: then
# the effect fails to load ("缺少参数 faceRot"), the GPU mask chain silently
# disappears for the whole session.  Sync every existing copy so "which one OBS
# picked" stops mattering.
$legacyData = @(
    (Join-Path $plugRoot "bin\64bit\data\shaders"),
    (Join-Path $plugRoot "bin\data\shaders"),
    (Join-Path $plugRoot "data\obs-plugins\obs-beauty\shaders")
)
foreach ($ld in $legacyData) {
    if (-not (Test-Path $ld)) { continue }
    foreach ($f in $hlsl) { Copy-Item $f.FullName (Join-Path $ld $f.Name) -Force }
    Write-Output "shaders  : synced legacy copy $ld"
}

# --- verify ---------------------------------------------------------------
$d1 = Get-FileHash $dllSrc -Algorithm MD5
$d2 = Get-FileHash $dllDst -Algorithm MD5
Write-Output "dll      : $dllDst"
Write-Output "dll md5  : $($d2.Hash)  (build $($d1.Hash))  match=$($d1.Hash -eq $d2.Hash)"
Write-Output "shaders  : $dataDir"
$badShaders = @()
foreach ($f in $hlsl) {
    $p = Join-Path $dataDir $f.Name
    $ok = (Test-Path $p) -and $((Get-FileHash $p -Algorithm MD5).Hash -eq (Get-FileHash $f.FullName -Algorithm MD5).Hash)
    if (-not $ok) { $badShaders += $f.Name }
    Write-Output ("  {0,-12} {1}" -f $f.Name, $(if ($ok) { "OK" } else { "MISMATCH" }))
}
if ($d1.Hash -ne $d2.Hash) { throw "dll md5 mismatch after copy" }
# A per-file MISMATCH used to be printed and then ignored - the install still
# said "INSTALL OK" and OBS silently fell back to CPU for that segment.  Every
# printed line is now also a hard failure, reported after the whole list so the
# log still shows the status of all six files.
if ($badShaders.Count -gt 0) {
    throw ("shader md5 mismatch after copy: " + ($badShaders -join ", "))
}
Write-Output "INSTALL OK"
