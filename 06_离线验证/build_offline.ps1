# =====================================================================
#  build_offline.ps1 - build the OFFLINE (no OBS) verification artifacts
#
#  Produces one thing in this folder:
#
#    core_probe.dll        ctypes-driven CPU engine runner. Feed a raw RGBA8
#                          frame + panel params, get the processed frame back.
#                          Used by offline_regress.py to capture / compare
#                          golden output without ever starting OBS.
#
#  WHY THIS EXISTS: every correctness check used to require installing the DLL
#  and restarting OBS (mask alignment, per-frame timing, shader binding...).
#  The CPU half of the engine has no OBS dependency at all, so it can be
#  exercised headlessly. That turns "write 2 days, gamble on one install" into
#  "diff byte-for-byte on the desk, then install only to verify the GPU part".
#
#  WHAT IT CANNOT DO: anything that needs a graphics context. gs_* / D3D11
#  effects, the GPU smoothing chain, real frame timing - all still need OBS.
#
#  NOTE: deliberately pure ASCII. PowerShell 5.1 decodes a BOM-less .ps1 as
#  ANSI, so a Chinese path literal would be mangled. Every path is DISCOVERED
#  by wildcard enumeration instead (same trick as 02_build/build_gpu.ps1).
#
#  Usage:  powershell -ExecutionPolicy Bypass -File build_offline.ps1
#          (or double-click build_offline.bat next to it)
# =====================================================================
$ErrorActionPreference = "Stop"

$root   = Split-Path -Parent $PSScriptRoot
$outDll = Join-Path $PSScriptRoot "core_probe.dll"
$objA   = Join-Path $PSScriptRoot "obj_probe"

function Find-Dir([string]$parent, [string]$pattern) {
    $d = Get-ChildItem $parent -Directory -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -like $pattern } | Select-Object -First 1
    if ($d) { return $d.FullName }
    return $null
}

# ---- locate sources ----------------------------------------------------
$srcDir = Find-Dir $root '01_*'
if (-not $srcDir) { throw "cannot locate source folder (01_*)" }

# onnxruntime headers live in the sibling source project, not in this branch.
# Probe for the actual header instead of trusting folder order (there are two
# '05_*' folders there and the docs one sorts first).
$workRoot = Split-Path -Parent $root
$obsProj  = Get-ChildItem $workRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like 'OBS*' -and $_.FullName -ne $root } |
            Select-Object -First 1
if (-not $obsProj) { throw "cannot locate OBS source project (OBS*)" }
$obsProj = $obsProj.FullName

$onnxInc = $null
foreach ($d in (Get-ChildItem $obsProj -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like '05_*' })) {
    $cand = Join-Path $d.FullName "deps\runtime\include"
    if (Test-Path (Join-Path $cand "onnxruntime_c_api.h")) { $onnxInc = $cand; break }
}
if (-not $onnxInc) { throw "cannot locate onnxruntime headers (05_*\deps\runtime\include)" }

# ---- toolchain ---------------------------------------------------------
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }
$vsPath = & $vswhere -latest -property installationPath
if (-not $vsPath) { throw "no Visual Studio installation found" }

$msvc = (Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
         Sort-Object Name -Descending | Select-Object -First 1).FullName
$cl   = Join-Path $msvc "bin\Hostx64\x64\cl.exe"
$link = Join-Path $msvc "bin\Hostx64\x64\link.exe"
if (-not (Test-Path $cl)) { throw "cl.exe not found under $msvc" }

$kitsRoot = "C:\Program Files (x86)\Windows Kits\10"
$kitVer = (Get-ChildItem (Join-Path $kitsRoot "Include") -Directory |
           Sort-Object Name -Descending | Select-Object -First 1).Name
$kitInc = Join-Path $kitsRoot "Include\$kitVer"
$kitLib = Join-Path $kitsRoot "Lib\$kitVer"

Write-Output "src      : $srcDir"
Write-Output "onnx inc : $onnxInc"
Write-Output "msvc     : $msvc"
Write-Output "win kit  : $kitVer"

# ---- source lists ------------------------------------------------------
# The engine half only. obs-beauty.cpp / obs-beauty-ui.cpp need libobs and are
# exactly what we are trying to avoid here.
$engine = @(
    "beauty_core.cpp",
    "beauty_face.cpp",
    "beauty_freq.cpp",
    "beauty_guide.cpp",
    "beauty_log.cpp",
    "beauty_onnx.cpp",
    "beauty_pool.cpp",
    "beauty_region.cpp",
    "beauty_smooth.cpp",
    "beauty_warp.cpp"
)

$inc = @(
    "-I$msvc\include",
    "-I$kitInc\ucrt",
    "-I$kitInc\um",
    "-I$kitInc\shared",
    "-I$kitInc\winrt",
    "-I$onnxInc"
)
# /MT (static CRT) for the same reason as build_gpu.ps1: no MSVCP140 /
# VCRUNTIME140 dependency, so the artifact is self-contained.
$cflags = @(
    "-nologo", "-c", "-EHsc", "-O2", "-MT", "-utf-8", "-W3",
    "-DNDEBUG", "-D_CRT_SECURE_NO_WARNINGS",
    "-std:c++17", "-Zc:__cplusplus"
)

function Build-One([string]$entry, [string]$objDir, [string]$outPath, [bool]$asDll) {
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    # Explicit wildcard: piping Get-ChildItem into Remove-Item misbehaves on
    # PowerShell 5.1 (InputObjectNotBound) - same trap as build_gpu.ps1.
    Remove-Item (Join-Path $objDir "*.obj") -Force -ErrorAction SilentlyContinue

    $srcs = @($entry) + $engine
    $failed = @()
    foreach ($f in $srcs) {
        $src = Join-Path $srcDir $f
        if (-not (Test-Path $src)) { throw "missing source: $src" }
        $obj = Join-Path $objDir ($f -replace '\.cpp$', '.obj')
        $out = & $cl @cflags @inc $src "-Fo$obj" 2>&1
        if ($LASTEXITCODE -ne 0) {
            $failed += $f
            Write-Output "---- $f ----"
            $out | Where-Object { $_ -match 'error' } | ForEach-Object { Write-Output $_ }
        }
    }
    if ($failed.Count -gt 0) { throw ("compile failed: " + ($failed -join ", ")) }

    $objs = @(Get-ChildItem $objDir -Filter *.obj | Select-Object -ExpandProperty FullName)
    $lflags = @("-nologo")
    if ($asDll) { $lflags += "-DLL" }
    $lflags += @(
        "-OUT:$outPath",
        "-LIBPATH:$msvc\lib\x64",
        "-LIBPATH:$kitLib\ucrt\x64",
        "-LIBPATH:$kitLib\um\x64"
    )
    $libs = @(
        "kernel32.lib", "user32.lib", "gdi32.lib", "opengl32.lib",
        "ole32.lib", "oleaut32.lib", "shell32.lib", "advapi32.lib",
        "winmm.lib", "uuid.lib"
    )
    $out = & $link @lflags @objs @libs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Output "---- link $outPath ----"
        $out | ForEach-Object { Write-Output $_ }
        throw "link failed: $outPath"
    }
    $i = Get-Item $outPath
    $h = (Get-FileHash $outPath -Algorithm MD5).Hash
    Write-Output ("{0} : {1} B  md5 {2}" -f $i.Name, $i.Length, $h)
}

# The engine resolves ORT through selfDir(), i.e. the folder holding this DLL.
# Copy the runtime next to our output so face detection actually loads; without
# it the engine silently degrades to full-frame smoothing (still a valid
# regression target, just not one that covers the mask branch).
$ortSrc = Join-Path (Split-Path -Parent $onnxInc) "onnxruntime.dll"
if (Test-Path $ortSrc) {
    Copy-Item $ortSrc (Join-Path $PSScriptRoot "onnxruntime.dll") -Force
    Write-Output "ort dll  : copied $(Split-Path -Leaf $ortSrc)"
} else {
    Write-Output "[warn] onnxruntime.dll not found at $ortSrc"
}

# yunet.onnx is the face detector. Without it the engine does NOT fail loudly:
# it reports faces=0 and quietly falls back to full-frame smoothing, so every
# golden captured in that state silently loses mask / region / warp coverage
# while still looking like a clean byte-exact pass.
#
# Target is <this dir>\models\yunet.onnx -- that is the engine's default lookup
# (selfDir() + "\models"), verified: with the file only there and NO
# BEAUTY_FACE_MODEL set, a run reports faces=1. Putting it in the folder root
# does nothing, which is an easy hour to lose.
$yunetDir = Join-Path $PSScriptRoot "models"
$yunetDst = Join-Path $yunetDir "yunet.onnx"
if (Test-Path $yunetDst) {
    Write-Output "yunet    : present (models\)"
} else {
    $yunetHit = $null
    $parent = Split-Path -Parent $srcDir
    foreach ($cand in @(
        (Join-Path $parent "05_*\models\yunet.onnx"),
        (Join-Path $parent "07_*\*\yunet.onnx"),
        (Join-Path $parent "07_*\installer-stage\yunet.onnx"))) {
        $hit = @(Get-ChildItem $cand -ErrorAction SilentlyContinue)
        if ($hit.Count -gt 0) { $yunetHit = $hit[0].FullName; break }
    }
    if ($yunetHit) {
        if (-not (Test-Path $yunetDir)) {
            New-Item -ItemType Directory -Path $yunetDir | Out-Null
        }
        Copy-Item $yunetHit $yunetDst -Force
        Write-Output "yunet    : copied -> models\ from $yunetHit"
    } else {
        Write-Output "[warn] yunet.onnx not found - face detection will fail and"
        Write-Output "       every case will silently degrade to full-frame smoothing."
        Write-Output "       Golden captures made in this state DO NOT cover the mask branch."
    }
}

# ---- build -------------------------------------------------------------
Build-One "core_probe.cpp" $objA $outDll $true

Write-Output ""
Write-Output "OFFLINE BUILD OK"
Write-Output "  next: python offline_regress.py --help"
