# =====================================================================
#  build_setup.ps1 - build the standalone installer (Setup.exe)
#
#  Produces: .\Setup.exe  (next to this script)
#
#  WHAT IT PACKAGES:
#    obs-beauty.dll        the filter DLL (copied from the 02_* build folder)
#    shaders\*.hlsl       the 6 GPU shaders (copied from 01_*/shaders)
#    setup.manifest       UAC elevation manifest
#
#  NOTE: this file is deliberately pure ASCII. PowerShell 5.1 decodes a
#  BOM-less .ps1 as ANSI, so a Chinese path literal would be mangled
#  (and a mangled comment can break the parse outright).
#  Every path is DISCOVERED (by wildcard) or DERIVED from $PSScriptRoot
#  instead of being written out. The previous version hardcoded a
#  GBK-mangled literal that pointed at a directory which no longer
#  existed -- the script could not run at all.
#
#  NOTE: setup.rc refers to "obs-beauty.dll" and "shaders\*.hlsl" with
#  RELATIVE paths, so rc.exe must run with the working directory set to
#  this folder. The copies are refreshed from their source folders on
#  every run so the package can never ship a stale artifact.
#
#  Usage:  powershell -ExecutionPolicy Bypass -File build_setup.ps1
# =====================================================================
$ErrorActionPreference = "Stop"

$pkgDir = $PSScriptRoot                              # the 07_* package folder
$root   = Split-Path -Parent $PSScriptRoot          # the project root

function Find-Dir([string]$parent, [string]$pattern) {
    $d = Get-ChildItem $parent -Directory -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -like $pattern } | Select-Object -First 1
    if ($d) { return $d.FullName }
    return $null
}

# ---- locate toolchain ----------------------------------------------------
$msvcRoot = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$kitRoot  = "C:\Program Files (x86)\Windows Kits\10"
$kitVer   = "10.0.26100.0"

$clExe = Join-Path $msvcRoot "bin\Hostx64\x64\cl.exe"
$rcExe = Join-Path $kitRoot "bin\$kitVer\x64\rc.exe"
if (-not (Test-Path $clExe)) { throw "cl.exe not found: $clExe" }
if (-not (Test-Path $rcExe)) { throw "rc.exe not found: $rcExe" }

$env:INCLUDE = "$msvcRoot\include;$kitRoot\Include\$kitVer\um;$kitRoot\Include\$kitVer\shared;$kitRoot\Include\$kitVer\ucrt"
$env:LIB     = "$msvcRoot\lib\x64;$kitRoot\Lib\$kitVer\um\x64;$kitRoot\Lib\$kitVer\ucrt\x64"

# ---- refresh the payload from its sources --------------------------------
$buildDir = Find-Dir $root '02_*'
$srcDir   = Find-Dir $root '01_*'
if (-not $buildDir) { throw "cannot locate build folder (02_*)" }
if (-not $srcDir)   { throw "cannot locate source folder (01_*)" }

$srcDll = Join-Path $buildDir "obs-beauty.dll"
if (-not (Test-Path $srcDll)) { throw "missing build output: $srcDll" }
Copy-Item $srcDll (Join-Path $pkgDir "obs-beauty.dll") -Force

$pkgShaders = Join-Path $pkgDir "shaders"
if (-not (Test-Path $pkgShaders)) { New-Item -ItemType Directory -Force -Path $pkgShaders | Out-Null }
# P2-3: this folder is a BUILD ARTIFACT, never a source and never tracked by git.
# Wipe stale *.hlsl BEFORE copying so a shader that was renamed or deleted in
# 01_*/shaders cannot survive in the package as a ghost copy.
# Same PowerShell 5.1 trap as build_gpu.ps1: piping Get-ChildItem into
# Remove-Item throws InputObjectNotBound when objects arrive as PSObject.
# Use an explicit path wildcard instead.
Remove-Item (Join-Path $pkgShaders "*.hlsl") -Force -ErrorAction SilentlyContinue
$srcShaders = Join-Path $srcDir "shaders"
Get-ChildItem $srcShaders -Filter "*.hlsl" | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $pkgShaders $_.Name) -Force
}
Write-Output ("payload   : " + (Get-Item (Join-Path $pkgDir "obs-beauty.dll")).Length + " B dll, " +
              (Get-ChildItem $pkgShaders -Filter *.hlsl).Count + " shaders")

# ---- compile -------------------------------------------------------------
Push-Location $pkgDir
& $rcExe /nologo /fo setup.res setup.rc
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "rc.exe failed ($LASTEXITCODE)" }
& $clExe /nologo /O2 /EHsc /MT /utf-8 setup.cpp setup.res /link /SUBSYSTEM:WINDOWS user32.lib advapi32.lib kernel32.lib shell32.lib /OUT:Setup.exe
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { throw "link failed ($rc)" }

$f = Get-Item (Join-Path $pkgDir "Setup.exe")
"BUILD OK: {0:N0} B  md5={1}" -f $f.Length, (Get-FileHash $f.FullName -Algorithm MD5).Hash
