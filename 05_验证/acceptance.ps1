# =====================================================================
#  obs-beauty 工程验收 —— 一条命令，PASS/FAIL 表（只读，不需管理员）
#  用法： powershell -ExecutionPolicy Bypass -File 05_验证\acceptance.ps1
# =====================================================================
$ErrorActionPreference = "Continue"
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
# 本机控制台代码页是 gb2312，而 Python 按 UTF-8 输出。不改这里的话，
# 捕获到的外部命令输出里中文全是乱码，正则统计恒为 0（看起来像「一条都没跑」）。
$prevEncoding = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$py   = "C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
$rows = @()
function Row($name, $ok, $detail) {
    $script:rows += [pscustomobject]@{ Test = $name; Result = $(if ($ok) { "PASS" } else { "FAIL" }); Detail = $detail }
}

$gates = "verify_detail_compiles","verify_mask_compiles","verify_region_compiles",
         "verify_smooth_compiles","verify_tone_compiles","verify_warp_compiles"
foreach ($g in $gates) {
    $out = & $py "$root\05_验证\$g.py" 2>&1
    $ok  = ($LASTEXITCODE -eq 0)
    Row "gate:$g" $ok $(if ($ok) { "ok" } else { ($out | Select-Object -Last 1) })
}

Push-Location "$root\06_离线验证"
$o  = & $py offline_regress.py check 2>&1
$ok = ($LASTEXITCODE -eq 0)
$n  = ([regex]::Matches(($o -join "`n"), "判定：逐字节相同")).Count
Row "golden:offline_regress" $ok "$n/4 byte-identical"
Pop-Location

$plug = "C:\Program Files\obs-studio\obs-plugins\64bit\obs-beauty.dll"
$okp  = Test-Path $plug
Row "install:dll" $okp $(if ($okp) { (Get-FileHash $plug -Algorithm MD5).Hash.Substring(0, 8) } else { "missing" })
$sh = @(Get-ChildItem "C:\Program Files\obs-studio\data\obs-plugins\obs-beauty\shaders\*.hlsl" -ErrorAction SilentlyContinue).Count
Row "install:shaders" ($sh -eq 6) "$sh/6"
Row "install:uninstall-entry" (Test-Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\obs-beauty") "registry"
$sf = "$root\07_安装包\Setup.exe"
Row "package:Setup.exe" (Test-Path $sf) $(if (Test-Path $sf) { "{0:N0} B" -f (Get-Item $sf).Length } else { "missing" })

$rows | Format-Table -AutoSize
$fail = ($rows | Where-Object { $_.Result -ne "PASS" }).Count
Write-Host ""
Write-Host ("ACCEPTANCE: " + $(if ($fail -eq 0) { "ALL PASS (" + $rows.Count + " checks)" } else { "$fail FAILED" }))
[Console]::OutputEncoding = $prevEncoding
exit $(if ($fail -eq 0) { 0 } else { 1 })
