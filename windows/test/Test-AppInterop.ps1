# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Test-AppInterop.ps1 -App PATH\IntelXeManager.exe -Root DIR [-Package DIR]: backups and undo across
windows\IntelXe-EFI.ps1 and IntelXe Manager for Windows. A change made by one is undone by the other, both ways, on
EFI folders made under DIR from windows\test\stage1\config.plist (efi\sample-z790 as stage 1 leaves it). Never an EFI
partition: the app is driven through its helper (IntelXeManager.exe --efi-helper ... --folder), the same code path the
EFI page takes for a partition once it is mounted, without mounting anything and without administrator rights.

.DESCRIPTION
  powershell -ExecutionPolicy Bypass -File windows\test\Test-AppInterop.ps1 -App out\...\IntelXeManager.exe -Root C:\work\fake-efi

-Package: an unpacked -efi.zip, for stage 4 (IntelXe.kext, HybridPM.kext, SSDT-ARCOFF.aml) in both directions too.
Everything is made in DIR (created; its fake-efi* folders are replaced) and left there for a look.
#>
param(
    [Parameter(Mandatory = $true)][string]$App,
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Package = ''
)

$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$Tool = Join-Path (Split-Path -Parent $Here) 'IntelXe-EFI.ps1'
$Stage1 = Join-Path $Here 'stage1\config.plist'
$Cpu = '13th Gen Intel(R) Core(TM) i5-13400F'   # a hybrid CPU: HybridPM.kext is planned when the EFI loads Lilu
$App = (Resolve-Path -LiteralPath $App).Path
$null = New-Item -ItemType Directory -Force -Path $Root
$Root = (Resolve-Path -LiteralPath $Root).Path

$script:fails = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:fails++ }
}
function Sha([string]$path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }
# every file under the EFI folder with its SHA-256 (two EFI folders hold the same when these are the same)
function Get-Tree([string]$dir) {
    $base = (Resolve-Path -LiteralPath $dir).Path
    return ((Get-ChildItem -LiteralPath $base -Recurse -File | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($base.Length) + ' ' + (Sha $_.FullName) }) -join "`n")
}

# an OpenCore EFI folder as stage 1 leaves it, with Lilu.kext loaded (so HybridPM.kext has its place)
function New-FakeEfi([string]$name) {
    $d = Join-Path $Root $name
    if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    foreach ($s in 'EFI\BOOT', 'EFI\OC\ACPI', 'EFI\OC\Kexts\Lilu.kext\Contents\MacOS') { $null = New-Item -ItemType Directory -Force -Path (Join-Path $d $s) }
    [IO.File]::WriteAllBytes((Join-Path $d 'EFI\OC\OpenCore.efi'), [byte[]]@())
    [IO.File]::WriteAllText((Join-Path $d 'EFI\OC\Kexts\Lilu.kext\Contents\MacOS\Lilu'), "stand-in Lilu`n")
    Copy-Item -LiteralPath $Stage1 -Destination (Join-Path $d 'EFI\OC\config.plist')
    return (Join-Path $d 'EFI')
}

# IntelXe-EFI.ps1 in Windows PowerShell, its output lines
function Invoke-Script([string[]]$words) {
    $env:INTELXE_CPU_BRAND = $Cpu
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Tool @words 2>&1
    $script:ScriptExit = $LASTEXITCODE
    return @($out | ForEach-Object { "$_" })
}
function Get-ScriptHash($lines) {
    foreach ($l in $lines) { if ($l -match '^== plan SHA-256: ([0-9a-f]{64})$') { return $Matches[1] } }
    return ''
}

# the app's helper on a folder: its JSON answer
function Quote([string]$a) {
    if ($a -ne '' -and $a -notmatch '[\s"]') { return $a }
    $a = $a -replace '(\\*)"', '$1$1\"'
    $a = $a -replace '(\\+)$', '$1$1'
    return '"' + $a + '"'
}
function Invoke-Helper([string[]]$words) {
    $out = Join-Path $Root ('answer-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.json')
    $all = @('--efi-helper') + $words + @('--cpu', $Cpu, '--out', $out)
    $p = Start-Process -FilePath $App -ArgumentList (($all | ForEach-Object { Quote $_ }) -join ' ') -Wait -PassThru -WindowStyle Hidden
    if (-not (Test-Path -LiteralPath $out)) { throw "the helper wrote no answer (exit code $($p.ExitCode))" }
    $a = [IO.File]::ReadAllText($out) | ConvertFrom-Json
    Remove-Item -LiteralPath $out
    return $a
}

$pkgWords = @()
if ($Package -ne '') { $pkgWords = @('--package', (Resolve-Path -LiteralPath $Package).Path) }
$scriptPkg = @()
if ($Package -ne '') { $scriptPkg = @('-Package', (Resolve-Path -LiteralPath $Package).Path) }

# ---- 1. the script changes, the app undoes ----------------------------------------------------------------------
$efi = New-FakeEfi 'fake-efi-1'
$cfg = Join-Path $efi 'OC\config.plist'
$before = Sha $cfg
$plan = Invoke-Script @('-Plan', 'settings', '-Efi', $efi)
$h = Get-ScriptHash $plan
$null = Invoke-Script @('-Apply', 'settings', '-Efi', $efi, '-Expect', $h)
Check ($ScriptExit -eq 0 -and (Sha $cfg) -ne $before) 'the script made stage 2'
$status = Invoke-Helper @('status', '--folder', $efi)
$scriptStatus = Invoke-Script @('-Status', '-Efi', $efi)
$scriptRoot = ($scriptStatus | Where-Object { $_ -like 'backup_root=*' } | Select-Object -First 1).Substring(12)
Check ($status.ok -and $status.status.backup_root -eq $scriptRoot) "the app looks for backups where the script puts them ($scriptRoot)"
Check ($status.status.backup_kind -eq 'settings') "the app sees the script's backup (kind $($status.status.backup_kind))"
$u = Invoke-Helper @('plan', '--change', 'undo', '--folder', $efi)
Check ($u.ok -and $u.plan.has_changes -and ($u.plan.lines -contains 'restore  EFI/OC/config.plist')) 'the app plans to undo the script''s change'
$d = Invoke-Helper @('apply', '--change', 'undo', '--folder', $efi, '--expect', $u.plan.hash)
Check ($d.ok -and (Sha $cfg) -eq $before) "the app undid it: config.plist as before$(if (-not $d.ok) { ': ' + $d.error })"
$again = Invoke-Script @('-Plan', 'undo', '-Efi', $efi)
Check (($again -join "`n") -match 'no backup to put back') 'the script sees the backup as put back (restored)'

# ---- 2. the app changes, the script undoes ----------------------------------------------------------------------
$efi = New-FakeEfi 'fake-efi-2'
$cfg = Join-Path $efi 'OC\config.plist'
$before = Sha $cfg
$p = Invoke-Helper @('plan', '--change', 'settings-safari', '--folder', $efi)
$a = Invoke-Helper @('apply', '--change', 'settings-safari', '--folder', $efi, '--expect', $p.plan.hash)
Check ($a.ok -and (Sha $cfg) -ne $before) "the app made stage 2 and the Safari setting$(if (-not $a.ok) { ': ' + $a.error })"
$bad = Invoke-Helper @('apply', '--change', 'settings', '--folder', $efi, '--expect', $p.plan.hash)
Check (-not $bad.ok -and $bad.error -like '*changed since the plan was shown*') 'the app refuses a plan hash that is no longer the EFI''s'
$plan = Invoke-Script @('-Plan', 'undo', '-Efi', $efi)
$kind = ([IO.File]::ReadAllText((Join-Path (Split-Path -Parent (Split-Path -Parent $Here)) 'release\templates\efi-rules.json')) | ConvertFrom-Json).backup_kinds.'settings-safari'
$found = ($plan -join "`n").Contains("($kind, ")
Check $found "the script finds the app's backup (kind '$kind')"
if (-not $found) { Write-Host ($plan -join "`n") }
$null = Invoke-Script @('-Undo', '-Efi', $efi, '-Expect', (Get-ScriptHash $plan))
Check ($ScriptExit -eq 0 -and (Sha $cfg) -eq $before) 'the script undid it: config.plist as before'
$b = Get-ChildItem -LiteralPath (Join-Path (Split-Path -Parent $efi) 'IntelXe-EFI-backups') -Recurse -Directory | Where-Object { $_.Name -match '^\d{3}-' } | Select-Object -First 1
$names = @(Get-ChildItem -LiteralPath $b.FullName -File | ForEach-Object { $_.Name } | Sort-Object)
Check ((@('changed', 'config-after.sha256', 'date', 'kind', 'restored', 'where') -join ',') -eq ($names -join ',')) "the app's backup holds the script's files ($($names -join ', '))"

# ---- 3. stage 4 both ways, with a package -----------------------------------------------------------------------
if ($Package -ne '') {
    $efi = New-FakeEfi 'fake-efi-3'
    $tree = Get-Tree $efi
    $p = Invoke-Helper (@('plan', '--change', 'driver', '--folder', $efi, '--card', '\_SB.PC00.PEG1.PEGP') + $pkgWords)
    Check ($p.ok -and ($p.plan.lines -join "`n") -match 'copy     EFI/OC/Kexts/IntelXe.kext' -and ($p.plan.lines -join "`n") -match 'SSDT-ARCOFF.aml' -and ($p.plan.lines -join "`n") -match 'HybridPM.kext') "the app plans stage 4 with the package$(if (-not $p.ok) { ': ' + $p.error })"
    $a = Invoke-Helper (@('apply', '--change', 'driver', '--folder', $efi, '--card', '\_SB.PC00.PEG1.PEGP', '--expect', $p.plan.hash) + $pkgWords)
    Check ($a.ok -and (Test-Path -LiteralPath (Join-Path $efi 'OC\Kexts\IntelXe.kext\Contents\MacOS\IntelXe'))) "the app made stage 4$(if (-not $a.ok) { ': ' + $a.error })"
    $plan = Invoke-Script @('-Plan', 'undo', '-Efi', $efi)
    $null = Invoke-Script @('-Undo', '-Efi', $efi, '-Expect', (Get-ScriptHash $plan))
    Check ($ScriptExit -eq 0 -and (Get-Tree $efi) -eq $tree) 'the script undid the app''s stage 4: every file as before'

    $efi = New-FakeEfi 'fake-efi-4'
    $tree = Get-Tree $efi
    $plan = Invoke-Script (@('-Plan', 'driver', '-Efi', $efi, '-CardAcpiPath', '\_SB.PC00.PEG1.PEGP') + $scriptPkg)
    $mine = Invoke-Helper (@('plan', '--change', 'driver', '--folder', $efi, '--card', '\_SB.PC00.PEG1.PEGP') + $pkgWords)
    $same = $mine.ok -and $mine.plan.hash -eq (Get-ScriptHash $plan)
    Check $same 'the script and the app give stage 4 the same plan hash'
    if (-not $same) { Write-Host "      the script:`n$($plan -join "`n")`n      the app:`n$($mine.plan.text)$($mine.error)" }
    $null = Invoke-Script (@('-Apply', 'driver', '-Efi', $efi, '-CardAcpiPath', '\_SB.PC00.PEG1.PEGP', '-Expect', (Get-ScriptHash $plan)) + $scriptPkg)
    Check ($ScriptExit -eq 0 -and (Get-Tree $efi) -ne $tree) 'the script made stage 4'
    $u = Invoke-Helper @('plan', '--change', 'undo', '--folder', $efi)
    $d = Invoke-Helper @('apply', '--change', 'undo', '--folder', $efi, '--expect', $u.plan.hash)
    Check ($d.ok -and (Get-Tree $efi) -eq $tree) 'the app undid the script''s stage 4: every file as before'
}

Write-Host "== $($script:fails) failure(s); the folders stay in $Root"
exit $script:fails
