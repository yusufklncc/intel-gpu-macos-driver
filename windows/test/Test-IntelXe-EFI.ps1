# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Test-IntelXe-EFI.ps1 [-RealPackage DIR]: runs windows\IntelXe-EFI.ps1 on Windows against a throwaway EFI folder made
from efi\sample-z790\config.plist as stage 1 leaves it (windows\test\stage1\config.plist), the same steps as
release/test-efi-flow.sh runs with efi.sh on macOS. No administrator rights, no EFI partition: everything happens in
a folder under %TEMP%, removed afterwards.

.DESCRIPTION
Each config.plist the tool writes is compared twice:
 - with expected\efi-sh\: what efi.sh writes on macOS for the same step (the same keys and values);
 - with expected\engine\: what the tool's engine writes on macOS (make-expected.sh), byte for byte apart from line
   ends: the layout of the file is kept, only the changed values and the added entries differ.
Plans are compared with efi.sh's line by line (notes aside). Last, Test-EfiVectors.ps1 runs the shared test vectors
of the EFI rules (release\efi-rules-vectors.json) against the script's rules loader and engine. Without -RealPackage the package's IntelXe.kext,
HybridPM.kext and SSDT-ARCOFF.aml are stand-ins. HybridPM.kext is tested with a hybrid CPU's name in
INTELXE_CPU_BRAND (the rest with a CPU without efficiency cores), and the table adapted to other ports is compared
byte for byte with what efi.sh writes from the 0.1.3 SSDT-ARCOFF.aml (stage1\SSDT-ARCOFF-0.1.3.aml).

  powershell -ExecutionPolicy Bypass -File windows\test\Test-IntelXe-EFI.ps1      (Windows PowerShell 5.1)
  pwsh -File windows\test\Test-IntelXe-EFI.ps1                                    (PowerShell 7)

Off Windows (PowerShell 7 on macOS, e.g. installed with dotnet tool install PowerShell) the same steps run, but not
what needs Windows: administrator rights, the card's Location path (Get-PnpDevice) and the CPU's name (CIM) are
skipped and say so.
#>
param([string]$RealPackage = '')

$Here = $PSScriptRoot
$ToolPath = Join-Path (Split-Path -Parent $Here) 'IntelXe-EFI.ps1'
$Stage1 = Join-Path $Here 'stage1\config.plist'
$NoEntries = Join-Path $Here 'stage1\config-noentries.plist'
$NoHpm = Join-Path $Here 'stage1\config-nohybridpm.plist'
$Aml013 = Join-Path $Here 'stage1\SSDT-ARCOFF-0.1.3.aml'
$NoHybrid = 'Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz'
$Hybrid = '13th Gen Intel(R) Core(TM) i5-13400F'
$CpuBefore = [Environment]::GetEnvironmentVariable('INTELXE_CPU_BRAND')
$Xp = Join-Path $Here 'expected'
$Exe = (Get-Process -Id $PID).Path
$Apple = '7C436110-AB2A-4BBB-A880-FE41995C9F82'   # Apple's NVRAM GUID, for an edit below
$ExeArgs = @()
# PowerShell installed as a dotnet tool runs as "dotnet pwsh.dll": the tool's processes need that dll too
if ([IO.Path]::GetFileNameWithoutExtension($Exe) -eq 'dotnet') { $ExeArgs = @([Environment]::GetCommandLineArgs()[0]) }
$OnWindows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT

# Dot-sourcing defines the tool's parameters here too ($Plan, $Efi, $Package, ...: typed and validated), so this
# script uses none of those names.
. $ToolPath   # the engine's types and the tool's functions; dot-sourced, its command line does not run
$ErrorActionPreference = 'Stop'

$script:fails = 0
$script:code = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:fails++ }
}

# a work folder with an ASCII path: the tool's output is read back through the console's code page
$Tmp = [IO.Path]::GetTempPath()
if ($Tmp -match '[^\x20-\x7e]') { $Tmp = "$env:SystemDrive\" }
$Root = Join-Path $Tmp ('ixeefi-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$F = Join-Path $Root 'efi'
$Pk = Join-Path $Root 'pkg'
$Bk = Join-Path $Root 'backups'
$Cfg = Join-Path $F 'EFI\OC\config.plist'
function Oc([string]$rel) { return Join-Path (Join-Path $F 'EFI\OC') $rel }

# the tool in its own process, as a user runs it: its lines, and its exit code in $script:code
function Invoke-Tool([string[]]$words) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = @(& $Exe @ExeArgs -NoProfile -ExecutionPolicy Bypass -File $ToolPath @words 2>&1 | ForEach-Object { "$_" }) }
    finally { $ErrorActionPreference = $old }
    $script:code = $LASTEXITCODE
    return , $lines
}
function Get-Status([string[]]$more) {
    $s = @{}
    foreach ($l in (Invoke-Tool (@('-Status', '-Efi', $F, '-Package', $Pk, '-BackupDir', $Bk) + $more))) {
        $i = $l.IndexOf('=')
        if ($i -gt 0) { $s[$l.Substring(0, $i)] = $l.Substring($i + 1) }
    }
    return $s
}
function Expect-Status([string]$what, [string]$key, [string]$value) {
    $got = (Get-Status @())[$key]
    Check ($got -eq $value) "$($what): $key=$got$(if ($got -ne $value) { ", expected $value" })"
}
# -Plan KIND: the lines, and the hash in $script:hash
function Get-Plan([string]$kind, [string[]]$more) {
    $lines = Invoke-Tool (@('-Plan', $kind, '-Efi', $F, '-Package', $Pk, '-BackupDir', $Bk) + $more)
    $script:hash = ''
    foreach ($l in $lines) { if ($l -like '== plan SHA-256: *') { $script:hash = $l.Substring(17).Trim() } }
    return , $lines
}
function Invoke-Apply([string]$kind, [string[]]$more) {
    $null = Get-Plan $kind $more
    if ($kind -eq 'undo') { $words = @('-Undo') } else { $words = @('-Apply', $kind) }
    $out = Invoke-Tool ($words + @('-Efi', $F, '-Package', $Pk, '-BackupDir', $Bk, '-Expect', $script:hash) + $more)
    if ($script:code -ne 0) { Write-Host ('      ' + ($out -join "`n      ")) }
    return , $out
}
function Get-Actions([string[]]$lines, [bool]$fromEfiSh) {
    $list = @()
    foreach ($l in $lines) {
        if ($l -eq '' -or $l -like 'EFI: *' -or $l -like 'note *' -or $l -like '== *' -or $l -like 'to make these changes:*' -or $l -like 'to put it back:*' -or $l -like 'warning:*') { continue }
        if ($fromEfiSh) {
            $l = $l.Replace('for this script)', 'for efi.sh on macOS)')
            if ($script:version -ne '') { $l = $l.Replace('(version 9.9.9)', "(version $script:version)") }
            if ($script:hpmVersion -ne '') { $l = $l.Replace('(version 9.9.8;', "(version $script:hpmVersion;") }
        }
        $list += $l
    }
    return , $list
}
function Compare-Plan([string[]]$lines, [string]$name) {
    $want = Get-Actions ([IO.File]::ReadAllText((Join-Path $Xp "efi-sh\$name.plan")).Replace("`r`n", "`n").Split("`n")) $true
    $got = Get-Actions $lines $false
    $same = ($want -join "`n") -ceq ($got -join "`n")
    Check $same "plan $($name): the same $($want.Count) lines as efi.sh's"
    if (-not $same) { Write-Host "      efi.sh:`n        $($want -join "`n        ")`n      this tool:`n        $($got -join "`n        ")" }
}
function Compare-Config([string]$name, [string]$source) {
    $got = [IntelXeEfi.Plist]::Load($Cfg)
    $want = [IntelXeEfi.Plist]::Load((Join-Path $Xp "efi-sh\$name.plist"))
    $d = [IntelXeEfi.Plist]::Difference($want.Root, $got.Root, 'config.plist')
    Check ($null -eq $d) "$($name): the keys and values of efi.sh's$(if ($null -ne $d) { ": $d" })"
    $text = [IO.File]::ReadAllText($Cfg).Replace("`r`n", "`n")
    $engine = [IO.File]::ReadAllText((Join-Path $Xp "engine\$name.plist")).Replace("`r`n", "`n")
    Check ($text -ceq $engine) "$($name): the same text as the engine's output on macOS (layout kept)"
    $before = [IntelXeEfi.Plist]::KeyPaths([IntelXeEfi.Plist]::Load($source).Root, '', $null)
    $after = [IntelXeEfi.Plist]::KeyPaths($got.Root, '', $null)
    Check ([IntelXeEfi.Plist]::IsSubsequence($before, $after)) "$($name): the keys it had, in their order"
    Write-Host "      $([IntelXeEfi.Util]::ChangedLines([IO.File]::ReadAllText($source), [IO.File]::ReadAllText($Cfg))) lines differ from the input"
}
function Get-Bytes([string]$path) { return , [IO.File]::ReadAllBytes($path) }
function Test-SameBytes([byte[]]$a, [byte[]]$b) {
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
}
function Test-Has([string[]]$lines, [string]$text) { return @($lines | Where-Object { $_.Contains($text) }).Count -gt 0 }
function Edit-Config([scriptblock]$change) {
    $pl = [IntelXeEfi.Plist]::Load($Cfg)
    & $change $pl
    [IO.File]::WriteAllBytes($Cfg, $pl.ToBytes())
}
# A new EFI folder and backup folder for each test, never a deleted one again: Windows finishes a recursive delete late
# (an open handle from Defender or the indexer), so a folder made again at once vanished under the test (5 Nov, 5.1).
$script:efiRun = 0
function New-Efi([string]$config) {
    $script:efiRun++
    $script:F = Join-Path $Root ('efi' + $script:efiRun)
    $script:Bk = Join-Path $Root ('backups' + $script:efiRun)
    $script:Cfg = Join-Path $script:F 'EFI\OC\config.plist'
    foreach ($d in 'EFI\OC\Kexts', 'EFI\OC\ACPI', 'EFI\BOOT') { $null = [IO.Directory]::CreateDirectory((Join-Path $script:F $d)) }
    [IO.File]::WriteAllBytes((Oc 'OpenCore.efi'), [byte[]]@())
    [IO.File]::Copy($config, $script:Cfg, $true)
}
function Write-Sums([string]$dir) {
    $lines = @()
    foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $dir 'EFI') -Recurse -File -Force | Sort-Object FullName)) {
        $rel = $f.FullName.Substring($dir.Length + 1).Replace('\', '/')
        $lines += (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLowerInvariant() + '  ' + $rel
    }
    [IO.File]::WriteAllText((Join-Path $dir 'SHA256SUMS'), ($lines -join "`n") + "`n")
}

try {
    $null = New-Item -ItemType Directory -Path $Root -Force
    Write-Host "== $($PSVersionTable.PSEdition) PowerShell $($PSVersionTable.PSVersion), $Exe; work folder $Root"

    # ---- the engine in this PowerShell ----
    foreach ($f in $Stage1, $NoEntries) {
        Check (Test-SameBytes ([IntelXeEfi.Plist]::Load($f).ToBytes()) (Get-Bytes $f)) "$([IO.Path]::GetFileName($f)) read and written back: the same bytes"
    }
    $rules = Get-EfiRules
    Check ($rules.Strings('cards.device_ids').Count -ge 1) "the rules: $($rules.Source), cards $($rules.Strings('cards.device_ids') -join ' ')"
    $levels = $rules.Int('cards.port_levels_above_gpu')
    Check ([IntelXeEfi.EfiFolder]::ConvertLocationPath('ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)', $levels) -eq '\_SB.PC00.PEG1.PEGP') 'Location path: the shipped table''s port'
    Check ([IntelXeEfi.EfiFolder]::ConvertLocationPath('ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#PCI(0000)#PCI(0100)#PCI(0000)', $levels) -eq 'none') 'Location path with three PCI parts: none'
    Check ($rules.IsCardInstance('PCI\VEN_8086&DEV_E20B&SUBSYS_00000000&REV_00\4&1&0&0008') -and -not $rules.IsCardInstance('PCI\VEN_8086&DEV_E20D&SUBSYS_00000000&REV_00\4&1&0&0008')) 'the cards by their instance id: 8086:E20B yes, 8086:E20D no'
    if ($OnWindows) {
        Check ((Test-Admin) -is [bool]) 'Test-Admin runs'
        $loc = Get-CardLocation
        Write-Host "      this PC's B580 Location path: $(if ($loc -eq '') { 'none found' } else { $loc })"
        [Environment]::SetEnvironmentVariable('INTELXE_CPU_BRAND', $null)
        $realCpu = Get-CpuName
        Check ($realCpu -ne '') "Get-CpuName: Win32_Processor.Name is '$realCpu' (hybrid: $(if ([IntelXeEfi.EfiFolder]::HybridCpu($rules, $realCpu)) { 'yes' } else { 'no' }))"
    } else { Write-Host 'skip  not Windows: Test-Admin, Get-CardLocation (Get-PnpDevice), Get-CpuName (CIM)' }
    # the tool's processes inherit it: a CPU without efficiency cores, but in the HybridPM steps
    [Environment]::SetEnvironmentVariable('INTELXE_CPU_BRAND', $NoHybrid)

    # ---- the package ----
    $script:version = ''
    $script:hpmVersion = ''
    if ($RealPackage -ne '') {
        Copy-Item -LiteralPath (Join-Path $RealPackage 'EFI') -Destination (Join-Path $Pk 'EFI') -Recurse
        Copy-Item -LiteralPath (Join-Path $RealPackage 'SHA256SUMS') -Destination (Join-Path $Pk 'SHA256SUMS')
        $script:version = [IntelXeEfi.EfiFolder]::KextVersion((Join-Path $Pk 'EFI\OC\Kexts\IntelXe.kext'))
        $script:hpmVersion = [IntelXeEfi.EfiFolder]::KextVersion((Join-Path $Pk 'EFI\OC\Kexts\HybridPM.kext'))
    } else {
        $k = Join-Path $Pk 'EFI\OC\Kexts\IntelXe.kext\Contents'
        $null = New-Item -ItemType Directory -Path (Join-Path $k 'MacOS') -Force
        $null = New-Item -ItemType Directory -Path (Join-Path $Pk 'EFI\OC\ACPI') -Force
        [IO.File]::WriteAllText((Join-Path $k 'MacOS\IntelXe'), "stand-in IntelXe executable`n")
        [IO.File]::WriteAllText((Join-Path $k 'Info.plist'), "<?xml version=`"1.0`" encoding=`"UTF-8`"?>`n<plist version=`"1.0`">`n<dict>`n`t<key>CFBundleVersion</key>`n`t<string>9.9.9</string>`n</dict>`n</plist>`n")
        $aml = [Text.Encoding]::ASCII.GetBytes("SSDT") + [byte[]](0x2f, 0x04) + [Text.Encoding]::ASCII.GetBytes('_SB_PC00PEG1PEGP stand-in')
        [IO.File]::WriteAllBytes((Join-Path $Pk 'EFI\OC\ACPI\SSDT-ARCOFF.aml'), [byte[]]$aml)
        $hk = Join-Path $Pk 'EFI\OC\Kexts\HybridPM.kext\Contents'
        $null = New-Item -ItemType Directory -Path (Join-Path $hk 'MacOS') -Force
        [IO.File]::WriteAllText((Join-Path $hk 'MacOS\HybridPM'), "stand-in HybridPM executable`n")
        [IO.File]::WriteAllText((Join-Path $hk 'Info.plist'), "<?xml version=`"1.0`" encoding=`"UTF-8`"?>`n<plist version=`"1.0`">`n<dict>`n`t<key>CFBundleIdentifier</key>`n`t<string>io.github.yusufklncc.HybridPM</string>`n`t<key>CFBundleVersion</key>`n`t<string>9.9.8</string>`n</dict>`n</plist>`n")
        Write-Sums $Pk
    }
    $stage1Bytes = Get-Bytes $Stage1

    # ---- stage 1 -> settings -> driver, undo ----
    New-Efi $Stage1
    $st = Get-Status @()
    foreach ($kv in @(@('opencore', 'yes'), @('kext_file', 'absent'), @('kext_entry', 'disabled'), @('ssdt_file', 'absent'), @('ssdt_entry', 'disabled'),
            @('package_checksums', 'ok'), @('settings_changes', '6'), @('plan_changes', '3'), @('backup', 'none'))) {
        Check ($st[$kv[0]] -eq $kv[1]) "stage 1: $($kv[0])=$($st[$kv[0]])$(if ($st[$kv[0]] -ne $kv[1]) { ", expected $($kv[1])" })"
    }
    $pp = Get-Plan 'settings' @()
    Compare-Plan $pp 'settings'
    Check ($script:hash -match '^[0-9a-f]{64}$') 'the plan ends with its SHA-256'
    $out = Invoke-Tool @('-Apply', 'settings', '-Efi', $F, '-Package', $Pk, '-BackupDir', $Bk, '-Expect', ('0' * 64))
    Check ($script:code -ne 0 -and (Test-Has $out 'changed since the plan was shown')) 'apply with another plan''s hash refuses'
    $out = Invoke-Tool @('-Apply', 'settings', '-Efi', $F, '-Package', $Pk, '-BackupDir', $Bk)
    Check ($script:code -ne 0 -and (Test-Has $out 'give the SHA-256')) 'apply without -Expect refuses'
    Check ((Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) -and -not (Test-Path -LiteralPath $Bk)) '... and nothing changed, no backup made'
    $out = Invoke-Apply 'settings' @()
    Check ($script:code -eq 0 -and (Test-Has $out '== backup of the whole EFI folder: ')) 'apply settings: backed up, done'
    Compare-Config 'after-settings' $Stage1
    Expect-Status 'after settings' 'settings_changes' '0'
    Expect-Status 'after settings' 'backup_kind' 'settings'
    $afterSettings = Get-Bytes $Cfg
    $b1 = (Get-Status @())['backup']
    Check ((Test-SameBytes (Get-Bytes (Join-Path $b1 'EFI\OC\config.plist')) $stage1Bytes) -and (Test-Path -LiteralPath (Join-Path $b1 'EFI\OC\OpenCore.efi'))) 'the backup holds the whole EFI folder as it was'

    $pp = Get-Plan 'driver' @()
    Compare-Plan $pp 'driver-nossdt'
    Check (Test-Has $pp 'note     not added: SSDT-ARCOFF.aml. Add it only after macOS is installed') 'without -CardAcpiPath SSDT-ARCOFF is left out, and the note says why'
    $null = Invoke-Apply 'driver' @()
    Compare-Config 'after-settings-driver-nossdt' $Stage1
    Check ([IntelXeEfi.Util]::SameFile((Oc 'Kexts\IntelXe.kext\Contents\MacOS\IntelXe'), (Join-Path $Pk 'EFI\OC\Kexts\IntelXe.kext\Contents\MacOS\IntelXe')) -and -not (Test-Path -LiteralPath (Oc 'ACPI\SSDT-ARCOFF.aml'))) 'IntelXe.kext copied, no SSDT-ARCOFF.aml'
    Expect-Status 'after driver' 'plan_changes' '0'
    Expect-Status 'after driver' 'kext_file' 'same'

    $pp = Get-Plan 'undo' @()
    Check ((Test-Has $pp 'restore  EFI/OC/config.plist') -and (Test-Has $pp 'remove   EFI/OC/Kexts/IntelXe.kext (it was not there before)')) 'undo plan: config.plist back, the kext removed'
    $null = Invoke-Apply 'undo' @()
    Check ((Test-SameBytes (Get-Bytes $Cfg) $afterSettings) -and -not (Test-Path -LiteralPath (Oc 'Kexts\IntelXe.kext'))) 'undo: as after settings'
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) 'second undo: config.plist byte for byte as in stage 1'
    $out = Invoke-Apply 'undo' @()
    Check ($script:code -ne 0 -and (Test-Has $out 'no backup to put back')) 'nothing left to undo'

    # ---- with the card's path confirmed ----
    $null = Invoke-Apply 'settings' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) $afterSettings) 'settings again: the same bytes'
    $pp = Get-Plan 'driver' @('-CardAcpiPath', '\_SB.PC00.PEG1.PEGP')
    Compare-Plan $pp 'driver-ssdt'
    Check (Test-Has $pp 'warning: SSDT-ARCOFF.aml hides the B580') 'the plan warns about SSDT-ARCOFF'
    $h1 = $script:hash
    $null = Get-Plan 'driver' @('-CardAcpiPath', 'ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)')
    Check ($script:hash -eq $h1) 'a Location paths line gives the same plan'
    $null = Invoke-Apply 'driver' @('-CardAcpiPath', '\_SB.PC00.PEG1.PEGP')
    Compare-Config 'after-settings-driver-ssdt' $Stage1
    Check ([IntelXeEfi.Util]::SameFile((Oc 'ACPI\SSDT-ARCOFF.aml'), (Join-Path $Pk 'EFI\OC\ACPI\SSDT-ARCOFF.aml'))) 'SSDT-ARCOFF.aml copied'
    Edit-Config { param($pl) $pl.Set([string[]]@('Misc', 'Boot', 'Timeout'), [IntelXeEfi.PValue]::Int(9)) }
    $pp = Get-Plan 'undo' @()
    Check (Test-Has $pp 'note     config.plist was changed after that change') 'undo plan warns about a later edit'
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) $afterSettings) 'undo: as after settings (the later edit is gone, as warned)'
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) 'undo: stage 1'

    # ---- the package ----
    $amlPath = Join-Path $Pk 'EFI\OC\ACPI\SSDT-ARCOFF.aml'
    $amlBytes = Get-Bytes $amlPath
    [IO.File]::WriteAllBytes($amlPath, [byte[]]($amlBytes + [byte]0x78))
    $out = Invoke-Apply 'driver' @('-CardAcpiPath', '\_SB.PC00.PEG1.PEGP')
    Check ($script:code -ne 0 -and (Test-Has $out 'differ from its SHA256SUMS')) 'a damaged package is refused'
    Check ((Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) -and -not (Test-Path -LiteralPath (Oc 'Kexts\IntelXe.kext'))) '... and nothing changed'
    [IO.File]::WriteAllBytes($amlPath, $amlBytes)

    # ---- driver before settings; a different kext ----
    $pp = Get-Plan 'driver' @()
    Compare-Plan $pp 'driver-stage1'
    $null = Invoke-Apply 'driver' @()
    Compare-Config 'after-driver-nossdt' $Stage1
    [IO.File]::AppendAllText((Oc 'Kexts\IntelXe.kext\Contents\MacOS\IntelXe'), "older build`n")
    Expect-Status 'an older kext in the EFI' 'kext_file' 'different'
    Check (Test-Has (Get-Plan 'driver' @()) 'replace  EFI/OC/Kexts/IntelXe.kext') 'plan replaces it'
    $null = Invoke-Apply 'driver' @()
    Expect-Status 'after apply' 'kext_file' 'same'
    $null = Invoke-Apply 'undo' @()
    Expect-Status 'undo puts the older one back' 'kext_file' 'different'
    $null = Invoke-Apply 'undo' @()
    Check ((Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) -and -not (Test-Path -LiteralPath (Oc 'Kexts\IntelXe.kext'))) 'undo: stage 1'

    # ---- SSDT-ARCOFF: the port ----
    $pp = Get-Plan 'driver' @('-CardAcpiPath', '\_SB.PC00.PEG0.PEGP')
    Check (-not (Test-Has $pp 'EFI/OC/ACPI/SSDT-ARCOFF.aml (names') -and (Test-Has $pp 'the card''s port here is \_SB.PC00.PEG0.PEGP')) 'another port: the table is not copied, and the note says why'
    Check (Test-Has (Get-Plan 'driver' @('-CardAcpiPath', 'ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#PCI(0000)#PCI(0100)#PCI(0000)')) 'has no ACPI name') 'a port without an ACPI name: says so'
    $out = Get-Plan 'driver' @('-CardAcpiPath', 'PEG1 slot')
    Check ($script:code -ne 0 -and (Test-Has $out 'is not an ACPI path')) 'a wrong path is refused'
    [IO.File]::WriteAllBytes((Oc 'ACPI\SSDT-ARCOFF.aml'), [byte[]]([Text.Encoding]::ASCII.GetBytes('SSDT') + [byte[]](0x2f, 0x04) + [Text.Encoding]::ASCII.GetBytes('_SB_PC00PEG0PEGP my own build')))
    $pp = Get-Plan 'driver' @('-CardAcpiPath', '\_SB.PC00.PEG0.PEGP')
    Check ((Test-Has $pp 'kept: EFI/OC/ACPI/SSDT-ARCOFF.aml differs') -and (Test-Has $pp 'ACPI > Add, item 5 (SSDT-ARCOFF.aml): Enabled false -> true')) 'your own table for that port is kept, its entry enabled'
    Remove-Item -LiteralPath (Oc 'ACPI\SSDT-ARCOFF.aml')

    # ---- an earlier B580.kext entry; a wider SIP value ----
    Edit-Config { param($pl) $pl.Append([string[]]@('Kernel', 'Add'), [IntelXeEfi.PValue]::Dict('BundlePath', 'B580.kext', 'Enabled', $true, 'ExecutablePath', 'Contents/MacOS/B580', 'PlistPath', 'Contents/Info.plist')) }
    Check (Test-Has (Get-Plan 'driver' @()) 'item 16 (B580.kext, the earlier name of IntelXe.kext): Enabled true -> false') 'an earlier B580.kext entry is turned off'
    [IO.File]::WriteAllBytes($Cfg, $stage1Bytes)
    $null = Invoke-Apply 'settings' @()
    Edit-Config { param($pl) $pl.Set([string[]]@('NVRAM', 'Add', $Apple, 'csr-active-config'), [IntelXeEfi.PValue]::Data([IntelXeEfi.EfiFolder]::CsrBytes(0xfef))) }
    Check (Test-Has (Get-Plan 'settings' @()) 'nothing to change') 'a wider SIP value (0xfef) is kept'
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) 'undo: stage 1'

    # ---- adding the entries ----
    New-Efi $NoEntries
    Compare-Plan (Get-Plan 'driver' @('-CardAcpiPath', '\_SB.PC00.PEG1.PEGP')) 'noentries-driver-ssdt'
    $null = Invoke-Apply 'driver' @('-CardAcpiPath', '\_SB.PC00.PEG1.PEGP')
    Compare-Config 'after-noentries-driver-ssdt' $NoEntries
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) (Get-Bytes $NoEntries)) 'undo: as before'

    # ---- the Safari setting (SecureBootModel): apart from stage 2, and with it as one change ----
    New-Efi $Stage1
    Edit-Config { param($pl) $pl.Set([string[]]@('Misc', 'Security', 'SecureBootModel'), [IntelXeEfi.PValue]::Str('Default')) }
    $sbm = Get-Bytes $Cfg
    $pp = Get-Plan 'safari' @()
    Check ((Test-Has $pp 'config   Misc > Security > SecureBootModel: Default -> Disabled') -and $script:hash -match '^[0-9a-f]{64}$') 'safari plan: SecureBootModel Default -> Disabled'
    Check (-not (Test-Has (Get-Plan 'settings' @()) 'SecureBootModel')) 'stage 2 leaves SecureBootModel alone'
    $null = Invoke-Apply 'safari' @()
    Expect-Status 'after -Apply safari' 'secure_boot_model' 'Disabled'
    Expect-Status 'after -Apply safari' 'backup_kind' 'Safari setting'
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) $sbm) 'undo of the Safari setting: config.plist back'
    $pp = Get-Plan 'settings-safari' @()
    Check ((Test-Has $pp 'csr-active-config: 0x0 -> 0x803') -and (Test-Has $pp 'SecureBootModel: Default -> Disabled')) 'settings-safari: both in one plan'
    $null = Invoke-Apply 'settings-safari' @()
    Expect-Status 'after -Apply settings-safari' 'settings_changes' '0'
    Expect-Status 'after -Apply settings-safari' 'backup_kind' 'settings and Safari setting'
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) $sbm) 'undo of settings-safari: config.plist back in one step'

    # ---- HybridPM.kext: by the CPU's name, below Lilu.kext, only when Lilu loads ----
    $cpus = 0
    $cpuFails = 0
    foreach ($l in [IO.File]::ReadAllLines((Join-Path $Xp 'efi-sh\hybrid-cpus.txt'))) {
        $bar = $l.IndexOf('|')
        if ($bar -lt 0) { continue }
        $cpus++
        if ([IntelXeEfi.EfiFolder]::HybridCpu($rules, $l.Substring($bar + 1)) -ne ($l.Substring(0, $bar) -eq 'yes')) {
            $cpuFails++
            Write-Host "      $($l.Substring($bar + 1)): efi.sh says $($l.Substring(0, $bar))"
        }
    }
    Check ($cpus -ge 10 -and $cpuFails -eq 0) "hybrid CPU names: the same answer as efi.sh for all $cpus"
    New-Efi $Stage1
    $null = [IO.Directory]::CreateDirectory((Oc 'Kexts\Lilu.kext\Contents\MacOS'))
    Check (-not (Test-Has (Get-Plan 'driver' @()) 'HybridPM')) 'a CPU without efficiency cores: no HybridPM in the plan'
    [Environment]::SetEnvironmentVariable('INTELXE_CPU_BRAND', $Hybrid)
    $st = Get-Status @()
    foreach ($kv in @(@('hybridpm_needed', 'yes'), @('hybridpm_cpu', $Hybrid), @('hybridpm_package', 'yes'), @('hybridpm_file', 'absent'), @('hybridpm_entry', 'disabled'), @('lilu_entry', 'enabled'))) {
        Check ($st[$kv[0]] -eq $kv[1]) "hybrid CPU: $($kv[0])=$($st[$kv[0]])$(if ($st[$kv[0]] -ne $kv[1]) { ", expected $($kv[1])" })"
    }
    Compare-Plan (Get-Plan 'driver' @()) 'hybrid-driver'
    $null = Invoke-Apply 'driver' @()
    Compare-Config 'after-hybrid-driver' $Stage1
    Check ([IntelXeEfi.Util]::SameFile((Oc 'Kexts\HybridPM.kext\Contents\MacOS\HybridPM'), (Join-Path $Pk 'EFI\OC\Kexts\HybridPM.kext\Contents\MacOS\HybridPM'))) 'HybridPM.kext copied'
    $st = Get-Status @()
    Check ($st['hybridpm_file'] -eq 'same' -and $st['hybridpm_entry'] -eq 'enabled' -and $st['plan_changes'] -eq '0') "hybrid, after apply: hybridpm_file=$($st['hybridpm_file']), hybridpm_entry=$($st['hybridpm_entry']), plan_changes=$($st['plan_changes'])"
    $null = Invoke-Apply 'undo' @()
    Check ((Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) -and -not (Test-Path -LiteralPath (Oc 'Kexts\HybridPM.kext'))) 'undo: stage 1, HybridPM.kext removed'

    New-Efi $NoHpm
    $null = [IO.Directory]::CreateDirectory((Oc 'Kexts\Lilu.kext\Contents\MacOS'))
    Compare-Plan (Get-Plan 'driver' @()) 'nohybridpm-driver'
    $null = Invoke-Apply 'driver' @()
    Compare-Config 'after-nohybridpm-driver' $NoHpm
    $pl = [IntelXeEfi.Plist]::Load($Cfg)
    Check ([IntelXeEfi.Plist]::Raw($pl.Get([string[]]@('Kernel', 'Add', '0', 'BundlePath'))) -eq 'Lilu.kext' -and [IntelXeEfi.Plist]::Raw($pl.Get([string[]]@('Kernel', 'Add', '1', 'BundlePath'))) -eq 'HybridPM.kext') 'HybridPM.kext is item 1, right below Lilu.kext'
    $null = Invoke-Apply 'undo' @()
    Check (Test-SameBytes (Get-Bytes $Cfg) (Get-Bytes $NoHpm)) 'undo: as before'

    New-Efi $Stage1
    $pp = Get-Plan 'driver' @()
    Check ((Test-Has $pp 'this EFI does not load Lilu.kext') -and -not (Test-Has $pp 'EFI/OC/Kexts/HybridPM.kext')) 'no Lilu.kext file: a note, HybridPM left alone'
    [Environment]::SetEnvironmentVariable('INTELXE_CPU_BRAND', $NoHybrid)

    # ---- SSDT-ARCOFF adapted to the card's port (the 0.1.3 table) ----
    $PkStandIn = $Pk
    $Pk = Join-Path $Root 'pkg-aml'   # Get-Plan, Get-Status and Invoke-Apply use $Pk
    [IntelXeEfi.Util]::CopyTree((Join-Path $PkStandIn 'EFI'), (Join-Path $Pk 'EFI'))
    [IO.File]::Copy($Aml013, (Join-Path $Pk 'EFI\OC\ACPI\SSDT-ARCOFF.aml'), $true)
    Write-Sums $Pk
    New-Efi $Stage1
    $st = Get-Status @('-CardAcpiPath', '\_SB.PC00.PEG1.PEGP')
    Check ($st['ssdt_package_names_card'] -eq 'yes') "the 0.1.3 table, the shipped port: ssdt_package_names_card=$($st['ssdt_package_names_card'])"
    foreach ($port in @(@('rp05', '\_SB.PC00.RP05.PXSX'), @('gpp8', '\_SB.PCI0.GPP8'), @('deep', '\_SB.PCI0.GPP8.UP00.DP40.UP00'))) {
        New-Efi $Stage1
        $more = @('-CardAcpiPath', $port[1])
        $st = Get-Status $more
        Check ($st['ssdt_package_names_card'] -eq 'adaptable') "the 0.1.3 table, $($port[1]): ssdt_package_names_card=$($st['ssdt_package_names_card'])"
        Compare-Plan (Get-Plan 'driver' $more) "adapt-$($port[0])"
        $null = Invoke-Apply 'driver' $more
        Check (Test-SameBytes (Get-Bytes (Oc 'ACPI\SSDT-ARCOFF.aml')) (Get-Bytes (Join-Path $Xp "efi-sh\adapt-$($port[0]).aml"))) "adapt-$($port[0]): byte for byte the table efi.sh writes"
        if ($port[0] -eq 'rp05') { Compare-Config 'after-adapt-rp05' $Stage1 }
        $st = Get-Status $more
        Check ($st['ssdt_file'] -eq 'same' -and $st['plan_changes'] -eq '0') "adapted, after apply: ssdt_file=$($st['ssdt_file']), plan_changes=$($st['plan_changes'])"
        $null = Invoke-Apply 'undo' @()
        Check ((Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes) -and -not (Test-Path -LiteralPath (Oc 'ACPI\SSDT-ARCOFF.aml'))) 'undo: stage 1, the adapted table removed'
    }
    $Pk = $PkStandIn

    # ---- the folder, the default backup place ----
    New-Efi $Stage1
    $a = Invoke-Tool @('-Status', '-Efi', $F, '-Package', $Pk, '-BackupDir', $Bk)
    $b = Invoke-Tool @('-Status', '-Efi', (Join-Path $F 'EFI'), '-Package', $Pk, '-BackupDir', $Bk)
    Check ((Test-Has $a "efi_folder=$(Join-Path $F 'EFI')") -and ($a -join "`n") -eq ($b -join "`n")) 'the EFI folder or the folder holding it: the same'
    $out = Invoke-Tool @('-Plan', 'settings', '-Efi', $Root)
    Check ($script:code -ne 0 -and (Test-Has $out 'no OC folder')) 'a folder without OC is refused'
    $out = Invoke-Tool @('-Plan', 'settings', '-Efi', $F, '-BackupDir', (Join-Path $F 'EFI\backups'))
    Check ($script:code -ne 0 -and (Test-Has $out 'inside the EFI folder')) 'backups inside the EFI folder are refused'
    $out = Invoke-Tool @('-Plan', 'settings', '-Efi', $F)
    Check ($script:code -eq 0 -and (Test-Has $out 'Booter > Quirks > ResizeAppleGpuBars')) 'stage 2 needs no -Package'
    $st = @{}
    foreach ($l in (Invoke-Tool @('-Status', '-Efi', $F, '-Package', $Pk))) { $i = $l.IndexOf('='); if ($i -gt 0) { $st[$l.Substring(0, $i)] = $l.Substring($i + 1) } }
    Check ($st['backup_root'] -like "$Root*IntelXe-EFI-backups*" -or $st['backup_root'] -like "$env:LOCALAPPDATA*") "default backup place: $($st['backup_root'])"
    if ($st['backup_root'] -like "$Root*") {
        $lines = Invoke-Tool @('-Plan', 'settings', '-Efi', $F, '-Package', $Pk)
        $h = ''
        foreach ($l in $lines) { if ($l -like '== plan SHA-256: *') { $h = $l.Substring(17).Trim() } }
        $out = Invoke-Tool @('-Apply', 'settings', '-Efi', $F, '-Package', $Pk, '-Expect', $h)
        Check ($script:code -eq 0 -and (Test-Path -LiteralPath (Join-Path $F 'IntelXe-EFI-backups'))) 'without -BackupDir: the backup next to the EFI folder'
        $lines = Invoke-Tool @('-Plan', 'undo', '-Efi', $F)
        foreach ($l in $lines) { if ($l -like '== plan SHA-256: *') { $h = $l.Substring(17).Trim() } }
        $out = Invoke-Tool @('-Undo', '-Efi', $F, '-Expect', $h)
        Check ($script:code -eq 0 -and (Test-SameBytes (Get-Bytes $Cfg) $stage1Bytes)) '... and undo finds it there'
    }
    $out = Invoke-Tool @('-Status')
    Check ($script:code -eq 2 -and (Test-Has $out 'give the EFI folder')) '-Status without -Efi says what is missing'
    $null = Invoke-Tool @('-Help')
    Check ($script:code -eq 0) '-Help'
    Remove-Item -LiteralPath (Oc 'OpenCore.efi')
    $out = Invoke-Tool @('-Plan', 'settings', '-Efi', $F)
    Check ($script:code -ne 0 -and (Test-Has $out 'not an OpenCore EFI')) 'without OpenCore.efi: not an OpenCore EFI'

    # ---- the shared test vectors of the EFI rules (release\efi-rules-vectors.json), which efi.sh runs on macOS ----
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $out = @(& $Exe @ExeArgs -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Here 'Test-EfiVectors.ps1') 2>&1 | ForEach-Object { "$_" }) }
    finally { $ErrorActionPreference = $old }
    $vcode = $LASTEXITCODE
    Check ($vcode -eq 0 -and $out.Count -gt 0) "Test-EfiVectors.ps1: $(if ($out.Count -gt 0) { $out[-1] })"
    if ($vcode -ne 0) { foreach ($l in $out) { if ($l -like 'FAIL*' -or $l -like '      *') { Write-Host "      $l" } } }
} catch {
    Write-Host "FAIL  the test stopped: $(Get-ErrorText $_)"
    Write-Host $_.ScriptStackTrace
    $script:fails++
} finally {
    [Environment]::SetEnvironmentVariable('INTELXE_CPU_BRAND', $CpuBefore)
    if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
}
Write-Host "== $($script:fails) failure(s)"
exit $script:fails
