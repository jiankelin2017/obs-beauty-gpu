# =====================================================================
#  build_ui.ps1 - build obs-beauty-ui.dll (top-level "Beauty" menu panel)
#
#  Produces: .\obs-beauty-ui.dll   (i.e. this 02_* folder)
#
#  WHY THIS EXISTS (3.2.4): the top-level menu entry in OBS is registered by
#  a SEPARATE module, obs-beauty-ui.dll, not by the filter dll.  Until 3.2.3
#  that dll was deployed by hand, so the installer knew nothing about it --
#  and when 3.2.3 started purging it on install while shipping no copy of its
#  own, every install ended up with no "Beauty" menu at all.  From 3.2.4 the
#  panel is a first-class installer payload, which means the GPU tree has to
#  be able to build it.  Hence this script.
#
#  NOTE: pure ASCII on purpose.  PowerShell 5.1 decodes a BOM-less .ps1 as
#  ANSI, so a Chinese path literal would be mangled.  Every folder below is
#  located by CONTENT PROBING (a marker FILE inside it), never by name, so
#  re-numbering the tree cannot break this script.
#
#  Three things that cost real time on the CPU branch:
#
#   1) This is the GPU flavour: do NOT define BEAUTY_UI_CPU_EDITION.  The
#      macro changes the menu title ("Beauty.CPU(&A)" vs "Beauty(&A)"), the
#      window title, the corner version label and the About box.  A CPU-flavour
#      panel next to the GPU filter gives two entries with different wording.
#
#   2) Qt is built with /MD, so this dll must NOT be /MT (unlike the filter
#      dll, which is /MT on purpose).  Default is /MD -- just do not add /MT.
#
#   3) No moc step: obs-beauty-ui.cpp has no Q_OBJECT, no signals/slots
#      macros.  If a Q_OBJECT ever appears, this script will link-fail with
#      unresolved vtable symbols and moc has to be wired in.
#
#  Usage:  powershell -ExecutionPolicy Bypass -File build_ui.ps1
# =====================================================================
$ErrorActionPreference = "Stop"

$root   = Split-Path -Parent $PSScriptRoot        # ...\<project root>
$outDll = Join-Path $PSScriptRoot "obs-beauty-ui.dll"
$objDir = Join-Path $PSScriptRoot "obj_ui"

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
$srcDir = Find-ByContent $root 'obs-beauty-ui.cpp'
if (-not $srcDir) { throw "cannot locate source folder (contains obs-beauty-ui.cpp)" }

$workRoot = Split-Path -Parent $root          # ...\  (sibling of the project)

# dependency drop = the dir that actually holds obs-studio-32.1.1\libobs
$depDir = Find-ByContent $workRoot 'obs-studio-32.1.1\libobs' 4
if (-not $depDir) { throw "cannot locate dependency folder (contains obs-studio-32.1.1\libobs)" }

$obsTree = $null
$sdkDir  = $null
foreach ($c in (Get-ChildItem $depDir -Directory -ErrorAction SilentlyContinue)) {
    if (-not $obsTree -and (Test-Path (Join-Path $c.FullName "libobs"))) { $obsTree = $c.FullName }
    # the panel needs obs-frontend-api.lib as well as obs.lib -- probe for both
    if (-not $sdkDir -and (Test-Path (Join-Path $c.FullName "obs.lib")) -and
        (Test-Path (Join-Path $c.FullName "obs-frontend-api.lib"))) { $sdkDir = $c.FullName }
}
if (-not $obsTree) { throw "cannot locate obs-studio source tree (has libobs\)" }
if (-not $sdkDir)  { throw "cannot locate obs-sdk (has obs.lib + obs-frontend-api.lib)" }

# Qt: probe a few well-known roots by CONTENT (lib\Qt6Widgets.lib).
$qtDir = $null
foreach ($q in @("C:\Qt", "D:\Qt", "C:\Qt\6.8.3\msvc2022_64")) {
    if (Test-Path -LiteralPath (Join-Path $q "lib\Qt6Widgets.lib")) { $qtDir = $q; break }
}
if (-not $qtDir) { throw "cannot locate Qt (lib\Qt6Widgets.lib under C:\Qt or D:\Qt)" }

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
Write-Output "qt       : $qtDir"
Write-Output "msvc     : $msvc"
Write-Output "win kit  : $kitVer"

# ---- rebuild -----------------------------------------------------------
New-Item -ItemType Directory -Force -Path $objDir | Out-Null
Remove-Item (Join-Path $objDir "*.obj") -Force -ErrorAction SilentlyContinue

$inc = @(
    "-I$msvc\include",
    "-I$kitInc\ucrt",
    "-I$kitInc\um",
    "-I$kitInc\shared",
    "-I$kitInc\winrt",
    "-I$obsTree\libobs",
    "-I$qtDir\include",
    "-I$qtDir\include\QtCore",
    "-I$qtDir\include\QtGui",
    "-I$qtDir\include\QtWidgets"
)

# /MD is the default and is REQUIRED (Qt is /MD).  Never add /MT here.
# /permissive- : Qt6 headers reject the loose MSVC mode.
$cflags = @(
    "-nologo", "-c", "-EHsc", "-O2", "-utf-8", "-W3",
    "-DNDEBUG", "-D_CRT_SECURE_NO_WARNINGS", "-DUNICODE", "-D_UNICODE",
    "-std:c++17", "-Zc:__cplusplus", "-permissive-"
)

$src = Join-Path $srcDir "obs-beauty-ui.cpp"
$obj = Join-Path $objDir "obs-beauty-ui.obj"
$out = & $cl @cflags @inc $src "-Fo$obj" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Output "---- compile ----"
    $out | Where-Object { $_ -match 'error|warning C4' } | ForEach-Object { Write-Output $_ }
    throw "compile failed"
}
Write-Output "compile  : OK"

$lflags = @(
    "-nologo", "-DLL", "-OUT:$outDll",
    "-LIBPATH:$msvc\lib\x64",
    "-LIBPATH:$kitLib\ucrt\x64",
    "-LIBPATH:$kitLib\um\x64",
    "-LIBPATH:$sdkDir",
    "-LIBPATH:$qtDir\lib"
)
$libs = @(
    "obs.lib", "obs-frontend-api.lib",
    "Qt6Widgets.lib", "Qt6Gui.lib", "Qt6Core.lib",
    "kernel32.lib", "user32.lib", "gdi32.lib", "shell32.lib",
    "ole32.lib", "oleaut32.lib", "advapi32.lib", "uuid.lib"
)
$out = & $link @lflags $obj @libs 2>&1
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
