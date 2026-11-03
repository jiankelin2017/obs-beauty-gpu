# =====================================================================
#  P2-3 - make the solo dev shader dir a LINK to the single source
#
#  Why: before 2.9.0 the project had four shader locations:
#    1) 01_*\shaders                 <- the only hand-written source
#    2) 07_*\shaders                 <- build artifact of build_setup.ps1 (git-ignored)
#    3) %PROGRAMDATA%\...\data\shaders  <- installed runtime copy
#    4) D:\obs_beauty_gpu\shaders    <- hand-copied dev dir for the *.bat launchers
#  Copies 1, 2 and 4 were identical in content but nothing kept them in sync, so a
#  stale copy could be picked up at runtime (the 2.2.0 outage: source said RGBA16F,
#  the packaged copy still said RGBA32F).  This script removes copy 4 by turning it
#  into an NTFS junction onto copy 1 - same bytes, no sync step, and the 11 ASCII
#  launchers that hardcode D:/obs_beauty_gpu/shaders keep working untouched.
#
#  A junction (mklink /J) is used instead of a symlink on purpose: junctions need
#  no SeCreateSymbolicLinkPrivilege, so this works from a normal shell.
#
#  Usage:  powershell -ExecutionPolicy Bypass -File 8_link_dev_shaders.ps1
#          powershell ... -File 8_link_dev_shaders.ps1 -SoloDir "E:\some\dir"
#
#  NOTE: pure ASCII on purpose (PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a
#  Chinese literal would be mangled).  The Chinese source folder is found by the
#  same '01_*' prefix probe the other scripts use.
# =====================================================================
param(
    [string]$SoloDir = "D:\obs_beauty_gpu\shaders"
)
$ErrorActionPreference = "Stop"

$here    = $PSScriptRoot
$srcRoot = Split-Path -Parent $here
$srcDir  = Get-ChildItem $srcRoot -Directory | Where-Object { $_.Name -like '01_*' } |
           Select-Object -First 1
if (-not $srcDir) { throw "cannot locate source folder (01_*)" }
$srcShaders = Join-Path $srcDir.FullName "shaders"
if (-not (Test-Path $srcShaders)) { throw "missing $srcShaders" }

function Get-ShaderSig([string]$dir) {
    $parts = @()
    foreach ($f in (Get-ChildItem $dir -Filter "*.hlsl" | Sort-Object Name)) {
        $parts += ($f.Name + ":" + (Get-FileHash $f.FullName -Algorithm MD5).Hash)
    }
    return ($parts -join "|")
}

$authSig = Get-ShaderSig $srcShaders
if ([string]::IsNullOrWhiteSpace($authSig)) { throw "no *.hlsl under $srcShaders" }
Write-Output "source   : $srcShaders"

$parent = Split-Path -Parent $SoloDir
if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }

if (Test-Path $SoloDir) {
    $item = Get-Item -LiteralPath $SoloDir -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $tgt = @($item.Target)[0]
        if ($tgt -and ((Resolve-Path $tgt).Path -eq (Resolve-Path $srcShaders).Path)) {
            Write-Output "link     : already a junction -> $tgt  (nothing to do)"
            exit 0
        }
        Write-Output "link     : junction points elsewhere ($tgt) - will be rebuilt"
        # Delete the reparse point only; the directory it points at keeps its content.
        # Directory.Delete() sometimes fails on a junction in PS 5.1, so fall back to
        # `cmd /c rmdir` (without /s, rmdir removes the link and leaves the target).
        try { [System.IO.Directory]::Delete($SoloDir, $false) }
        catch { & cmd /c rmdir "`"$SoloDir`"" | Out-Null }
        if (Test-Path $SoloDir) { throw "could not remove old junction: $SoloDir" }
    } else {
        # A real folder. Only replace it when its content is already identical, so
        # nobody loses a modified shader that exists nowhere else.
        if ((Get-ShaderSig $SoloDir) -ne $authSig) {
            Write-Output "REFUSE   : $SoloDir is a real folder AND differs from source."
            Write-Output "           Diff the two by hand first - it may hold edits that were"
            Write-Output "           never merged into 01_*\shaders."
            exit 2
        }
        $bak = $SoloDir + "_bak_" + (Get-Date -Format "yyyyMMdd_HHmmss")
        Rename-Item -LiteralPath $SoloDir -NewName (Split-Path -Leaf $bak)
        Write-Output "backup   : $bak  (identical to source; safe to delete)"
    }
}

New-Item -ItemType Junction -Path $SoloDir -Target $srcShaders | Out-Null
$after = Get-Item -LiteralPath $SoloDir -Force
Write-Output "link     : $SoloDir  ->  $($after.Target)"
if ((Get-ShaderSig $SoloDir) -ne $authSig) { throw "post-check failed: linked dir does not match source" }
Write-Output ("verify   : " + (Get-ChildItem -LiteralPath $SoloDir -Filter *.hlsl).Count +
              " shaders, all identical to source")
