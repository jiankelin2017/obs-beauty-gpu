# =====================================================================
#  build_gpu.ps1 - build obs-beauty.dll (filter only)
#
#  Produces: ..\02_build\obs-beauty.dll   (i.e. the 02_* folder)
#
#  NOTE: this file is deliberately pure ASCII. PowerShell 5.1 decodes a
#  BOM-less .ps1 as ANSI, so a Chinese path literal would be mangled.
#  Every path below is DISCOVERED by enumerating directories and probing
#  for a marker FILE inside them (obs-beauty.cpp, obs-studio-32.1.1\libobs,
#  deps\runtime\include) instead of being written out or matched by name.
#  2026-11-01: the CPU project's tree was re-organized (dependency drop moved
#  to "reference files\03_depend-on-SDK_OBS", verify dir renamed to "05_verify"), which
#  broke the old "07_*" / "05_*" prefix matches.  Content probing replaces
#  them, so future re-numbering cannot break this script again.
#
#  Four things that cost real time the first time round:
#
#   1) /MT (static CRT), not /MD. With /MD the DLL depends on
#      MSVCP140.dll + VCRUNTIME140.dll and shrinks to ~267 KB, while every
#      historical build is ~470 KB and depends on obs.dll + KERNEL32.dll
#      only. Both run, but the delivered artifact should look like the ones
#      already shipped - if the size suddenly drops by 40% something changed.
#
#   2) obs-beauty-ui.cpp must NOT be linked in. It has its own
#      OBS_DECLARE_MODULE() and links into a SEPARATE obs-beauty-ui.dll;
#      including it here yields LNK2005 on obs_module_load / _unload / _ver.
#
#   3) Never assume a shell wrapper: run cl.exe / link.exe directly with
#      arguments in a PowerShell array. In Git Bash, MSYS rewrites
#      "-OUT:D:/x" into "/D:/x/" and the link silently fails with a syntax
#      error unless MSYS2_ARG_CONV_EXCL='*' is exported AND eval is avoided.
#
#   4) The source tree is not UTF-8-BOM everywhere, so /utf-8 is required;
#      MSVC 14.51 also needs /std:c++17 for Qt and C4996 suppression for
#      the many getenv() calls.
#
#  Usage:  powershell -ExecutionPolicy Bypass -File build_gpu.ps1
# =====================================================================
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot        # ...\<project root>
$outDll = Join-Path $PSScriptRoot "obs-beauty.dll"
$objDir = Join-Path $PSScriptRoot "obj"

# 2026-11-01: the CPU project re-organized its tree --
# the dependency drop moved under a new "reference files" parent,
# two levels deep, and the offline-verify dir was re-numbered.
# Matching by name prefix ("07_*", "05_*") no longer
# works, and the old "OBS*" match for the CPU project could also pick this
# very project first (its own name also starts with "OBS").
# Both are now located by CONTENT instead of by name:
#   * CPU project = the sibling dir that actually contains the obs tree
#   * dependency  = the dir that actually contains obs-studio-32.1.1\libobs
#   * onnx include = the dir that actually contains deps\runtime\include
# Re-numbering the tree can therefore never break this script again.

# Locate a directory by probing for a marker FILE inside it.
# Walks up to $maxDepth levels below $parent, so the marker can live at the
# top level, or nested under a "reference files" style parent folder.
function Find-ByContent([string]$parent, [string]$marker, [int]$maxDepth = 4) {
    if (-not $parent) { return $null }
    if (-not (Test-Path -LiteralPath $parent)) { return $null }
    $level1 = @(Get-ChildItem $parent -Directory -ErrorAction SilentlyContinue)
    foreach ($c in $level1) {
        if (Test-Path -LiteralPath (Join-Path $c.FullName $marker)) { return $c.FullName }
    }
    for ($d = 2; $d -le $maxDepth; $d++) {
        $cur = $level1
        for ($i = 1; $i -lt $d; $i++) {
            $next = @()
            foreach ($x in $cur) {
                $next += @(Get-ChildItem $x.FullName -Directory -ErrorAction SilentlyContinue)
            }
            $cur = $next
            if ($cur.Count -eq 0) { break }
        }
        foreach ($x in $cur) {
            if (Test-Path -LiteralPath (Join-Path $x.FullName $marker)) { return $x.FullName }
        }
    }
    return $null
}

# ---- locate everything -------------------------------------------------
# 1) source folder of THIS project
$srcDir = Find-ByContent $root 'obs-beauty.cpp'
if (-not $srcDir) { throw "cannot locate source folder (contains obs-beauty.cpp)" }

$workRoot = Split-Path -Parent $root          # ...\  (sibling of the project)

# 2) the CPU project = the sibling that holds the onnxruntime headers
#    (05_verify\deps\runtime\include\onnxruntime_c_api.h).
#    Probe by CONTENT, not by name: this project's own name also starts with
#    "OBS", and a name-based match could pick this project first.
$obsProj = Find-ByContent $workRoot 'deps\runtime\include\onnxruntime_c_api.h' 4
if (-not $obsProj) { throw "cannot locate OBS source project (contains deps\runtime\include\onnxruntime_c_api.h)" }

# 3) the dependency drop = the dir that actually holds obs-studio-32.1.1\libobs
#    (now nested under the CPU project's reference-files parent, two levels deep)
$depDir = Find-ByContent $workRoot 'obs-studio-32.1.1\libobs' 4
if (-not $depDir) { throw "cannot locate dependency folder (contains obs-studio-32.1.1\libobs)" }

$obsTree = $null
$sdkDir  = $null
foreach ($c in (Get-ChildItem $depDir -Directory -ErrorAction SilentlyContinue)) {
    if (-not $obsTree -and (Test-Path (Join-Path $c.FullName "libobs"))) { $obsTree = $c.FullName }
    if (-not $sdkDir  -and (Test-Path (Join-Path $c.FullName "obs.lib"))) { $sdkDir  = $c.FullName }
}
if (-not $obsTree) { throw "cannot locate obs-studio source tree (has libobs\)" }
if (-not $sdkDir)  { throw "cannot locate obs-sdk (has obs.lib)" }

# The onnxruntime headers: $obsProj was located above BY probing for
# deps\runtime\include\onnxruntime_c_api.h, so it is either the folder that
# holds that include dir, or the include dir's parent.  Accept both.
$onnxInc = $null
if (Test-Path -LiteralPath (Join-Path $obsProj "onnxruntime_c_api.h")) {
    $onnxInc = $obsProj
} elseif (Test-Path -LiteralPath (Join-Path $obsProj "deps\runtime\include\onnxruntime_c_api.h")) {
    $onnxInc = Join-Path $obsProj "deps\runtime\include"
}
if (-not $onnxInc) {
    foreach ($d in (Get-ChildItem $obsProj -Directory -ErrorAction SilentlyContinue)) {
        $cand = Join-Path $d.FullName "deps\runtime\include"
        if (Test-Path -LiteralPath (Join-Path $cand "onnxruntime_c_api.h")) {
            $onnxInc = $cand
            break
        }
    }
}
if (-not $onnxInc) { throw "cannot locate onnxruntime headers (deps\runtime\include\onnxruntime_c_api.h)" }

# ---- toolchain ---------------------------------------------------------
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }
$vsPath = & $vswhere -latest -property installationPath
if (-not $vsPath) { throw "no Visual Studio installation found" }

$msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
$msvc = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending |
         Select-Object -First 1).FullName
$cl   = Join-Path $msvc "bin\Hostx64\x64\cl.exe"
$link = Join-Path $msvc "bin\Hostx64\x64\link.exe"
if (-not (Test-Path $cl)) { throw "cl.exe not found under $msvc" }

$kitsRoot = "C:\Program Files (x86)\Windows Kits\10"
$kitVer = (Get-ChildItem (Join-Path $kitsRoot "Include") -Directory |
           Sort-Object Name -Descending | Select-Object -First 1).Name
$kitInc = Join-Path $kitsRoot "Include\$kitVer"
$kitLib = Join-Path $kitsRoot "Lib\$kitVer"

Write-Output "src      : $srcDir"
Write-Output "obs tree : $obsTree"
Write-Output "obs sdk  : $sdkDir"
Write-Output "onnx inc : $onnxInc"
Write-Output "msvc     : $msvc"
Write-Output "win kit  : $kitVer"

# ---- rebuild -----------------------------------------------------------
$sources = @(
    "obs-beauty.cpp",
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

New-Item -ItemType Directory -Force -Path $objDir | Out-Null
# 3.1.46: piping Get-ChildItem into Remove-Item fails on PowerShell 5.1 when the
# folder is empty or the objects arrive as PSObject (parameter binding error
# "InputObjectNotBound").  Use an explicit path wildcard instead.
Remove-Item (Join-Path $objDir "*.obj") -Force -ErrorAction SilentlyContinue

$inc = @(
    "-I$msvc\include",
    "-I$kitInc\ucrt",
    "-I$kitInc\um",
    "-I$kitInc\shared",
    "-I$kitInc\winrt",
    "-I$obsTree\libobs",
    "-I$onnxInc"
)

$cflags = @(
    "-nologo", "-c", "-EHa", "-O2", "-MT", "-utf-8", "-W3",
    "-DNDEBUG", "-D_CRT_SECURE_NO_WARNINGS",
    "-std:c++17", "-Zc:__cplusplus"
)

$failed = @()
foreach ($f in $sources) {
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
Write-Output "compile  : OK ($($sources.Count) files)"

$objs = @(Get-ChildItem $objDir -Filter *.obj | Select-Object -ExpandProperty FullName)
$lflags = @(
    "-nologo", "-DLL", "-OUT:$outDll",
    "-LIBPATH:$msvc\lib\x64",
    "-LIBPATH:$kitLib\ucrt\x64",
    "-LIBPATH:$kitLib\um\x64",
    "-LIBPATH:$sdkDir"
)
$libs = @(
    "obs.lib", "kernel32.lib", "user32.lib", "gdi32.lib",
    "ole32.lib", "oleaut32.lib", "shell32.lib", "advapi32.lib",
    "winmm.lib", "uuid.lib"
)
$out = & $link @lflags @objs @libs 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Output "---- link ----"
    $out | ForEach-Object { Write-Output $_ }
    throw "link failed"
}

$d = Get-Item $outDll
$h = (Get-FileHash $outDll -Algorithm MD5).Hash
Write-Output ""
Write-Output "dll      : $($d.FullName)"
Write-Output "size     : $($d.Length) B"
Write-Output "md5      : $h"
Write-Output "BUILD OK"
