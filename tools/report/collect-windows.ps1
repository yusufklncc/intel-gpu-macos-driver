# collect-windows.ps1: hardware inventory for the Intel GPU macOS driver project (v2).
#
# Run it on Windows on the same machine (most testers dual-boot). It records the hardware, firmware
# settings that Windows can see (Resizable BAR / Above 4G, Secure Boot), GPU drivers, monitors (EDID)
# and a dxdiag report. Read-only; no admin needed (an admin run adds the TPM and Secure Boot details).
# PowerShell 5.1 (built into Windows 10/11); no modules to install.
#
#   powershell -ExecutionPolicy Bypass -File collect-windows.ps1 [-OutDir <folder>] [-Acpi] [-BootLogs]
#
# Writes xe-report-XR-xxxxxxxx.zip in the current folder (or -OutDir). Serial numbers, UUIDs, MAC
# addresses, user and computer names and Wi-Fi network names are removed. It reads nothing about the
# Windows product key, and it stops without a file if the output contains anything shaped like one.
# ACPI tables only with -Acpi, and then only the DSDT and the first SSDT (all Windows hands out without a
# driver); never MSDM or SLIC (the Windows licence) or WPBT (an OEM program for Windows).
# Boot logs only with -BootLogs: what OpenCore's boot logging left in the root of an EFI volume (its logs, opencore-*.txt,
# with the macOS boot loader's AAPL: lines, and the macOS panics it saved, panic-*.txt), from FAT volumes with a drive
# letter (USB sticks) and, run as administrator, from the EFI partitions, which it mounts with mountvol as
# IntelXe-EFI.ps1 does, only reads, and unmounts again. The serial, MLB, ROM and UUID values of each config.plist there
# are removed from every file too.

param([string]$OutDir = (Get-Location).Path, [switch]$Acpi, [switch]$BootLogs)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$CollectorVersion = 2
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$Budget = 90

function Left { [int]($Budget - $sw.Elapsed.TotalSeconds) }

$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$b = New-Object byte[] 4
$rng.GetBytes($b)
$ID = 'XR-' + (($b | ForEach-Object { $_.ToString('X2') }) -join '')
$Name = "xe-report-$ID"
$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("xe-report-" + [guid]::NewGuid().ToString('N'))
$W = Join-Path $Tmp $Name
foreach ($d in 'system', 'gpu', 'display', 'logs') { New-Item -ItemType Directory -Force -Path (Join-Path $W $d) | Out-Null }
$Log = Join-Path $W 'collector.txt'
$Skipped = New-Object System.Collections.ArrayList
$Literals = New-Object System.Collections.ArrayList   # private values to remove; never written

function Note([string]$m) { Add-Content -Path $Log -Value ("{0} {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) -Encoding UTF8 }
function Skip([string]$m) { [void]$Skipped.Add($m); Note "skipped: $m" }
function Lit([string]$kind, $v) {
    if ($null -eq $v) { return }
    $s = ([string]$v).Trim()
    if ($s.Length -lt 4) { return }
    if ($s -match '^(To be filled by O\.E\.M\.|Default string|System Serial Number|Not Specified|None|0+|Not Applicable|N/A|Unknown|Base Board Serial Number|Chassis Serial Number)$') { return }
    [void]$Literals.Add([pscustomobject]@{ Kind = $kind; Value = $s })
}
# Section 'file' { block }: run the block, write its output as text; errors go into the file
function Section([string]$file, [scriptblock]$block) {
    $path = Join-Path $W $file
    if ((Left) -lt 3) { Skip "$file : time budget used up"; return }
    try { $out = & $block 2>&1 | Out-String -Width 250 }
    catch { $out = "error: $($_.Exception.Message)" }
    Add-Content -Path $path -Value $out -Encoding UTF8
}

$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "Intel GPU: collecting the Windows hardware inventory ($ID), about a minute..."
Set-Content -Path $Log -Encoding UTF8 -Value @(
    "collector: collect-windows.ps1 v$CollectorVersion",
    "report id: $ID",
    "started: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
    "powershell: $($PSVersionTable.PSVersion)  admin: $IsAdmin",
    "")
if (-not $IsAdmin) { Skip "admin-only details (TPM version via Get-Tpm, Confirm-SecureBootUEFI): not run as administrator; Secure Boot and TPM presence come from the registry and device list instead" }

# ------------------------------------------------------------ private values (never written)
Lit user $env:USERNAME
Lit host $env:COMPUTERNAME
Lit host $env:USERDOMAIN
$un = $env:USERNAME -replace "'", "''"
try { Get-CimInstance Win32_UserAccount -Filter "LocalAccount=True AND Name='$un'" -ErrorAction Stop | ForEach-Object { Lit user $_.FullName } } catch {}
try {
    $cs = Get-CimInstance Win32_ComputerSystem
    Lit host $cs.Name; Lit host $cs.DNSHostName; Lit user (($cs.UserName -split '\\')[-1])
    if ($cs.Domain -ne 'WORKGROUP') { Lit host $cs.Domain }
} catch {}
try { Get-CimInstance Win32_BIOS | ForEach-Object { Lit serial $_.SerialNumber } } catch {}
try { Get-CimInstance Win32_BaseBoard | ForEach-Object { Lit serial $_.SerialNumber } } catch {}
try { Get-CimInstance Win32_SystemEnclosure | ForEach-Object { Lit serial $_.SerialNumber; Lit serial $_.SMBIOSAssetTag } } catch {}
try { Get-CimInstance Win32_ComputerSystemProduct | ForEach-Object { Lit serial $_.IdentifyingNumber; Lit uuid $_.UUID } } catch {}
try { Get-CimInstance Win32_DiskDrive | ForEach-Object { Lit serial $_.SerialNumber } } catch {}
try { Get-CimInstance Win32_PhysicalMedia | ForEach-Object { Lit serial $_.SerialNumber } } catch {}
try { Lit uuid (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Cryptography' -Name MachineGuid -ErrorAction Stop).MachineGuid } catch {}
try {
    Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.MACAddress } | ForEach-Object {
        $m = $_.MACAddress
        Lit mac $m; Lit mac ($m -replace ':', '-'); if (($m -replace ':', '') -match '[A-Fa-f]') { Lit mac ($m -replace ':', '') }
    }
} catch {}
try {
    $wl = & netsh.exe wlan show profiles 2>$null
    # "    All User Profile     : name" (the label is translated; the layout is not)
    foreach ($l in $wl) {
        if ($l -match '^\s{4,}\S[^:]*\s:\s(.+)$') {
            $s = $Matches[1].Trim()
            if ($s.Length -ge 3 -and -not $s.StartsWith('<')) { [void]$Literals.Add([pscustomobject]@{ Kind = 'ssid'; Value = $s }) }
        }
    }
} catch {}

# ------------------------------------------------------------ system
Note 'system'
Section 'system\os.txt' {
    Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture, InstallDate, LastBootUpTime
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
    "DisplayVersion: $($cv.DisplayVersion)  UBR: $($cv.UBR)  EditionID: $($cv.EditionID)"
}
Section 'system\hardware.txt' {
    '== System'; Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer, Model, SystemFamily, SystemSKUNumber, TotalPhysicalMemory, PCSystemType
    '== Board'; Get-CimInstance Win32_BaseBoard | Select-Object Manufacturer, Product, Version
    '== BIOS'; Get-CimInstance Win32_BIOS | Select-Object Manufacturer, SMBIOSBIOSVersion, BIOSVersion, ReleaseDate, SMBIOSMajorVersion, SMBIOSMinorVersion
    '== CPU'; Get-CimInstance Win32_Processor | Select-Object Name, Manufacturer, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed
    '== Memory'; Get-CimInstance Win32_PhysicalMemory | Select-Object Manufacturer, PartNumber, Capacity, ConfiguredClockSpeed
}
Section 'system\security.txt' {
    $sb = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -ErrorAction SilentlyContinue
    "Secure Boot (registry UEFISecureBootEnabled): $(if ($null -eq $sb) { 'unknown (legacy BIOS?)' } else { $sb.UEFISecureBootEnabled })"
    if ($IsAdmin) {
        try { "Confirm-SecureBootUEFI: $(Confirm-SecureBootUEFI -ErrorAction Stop)" } catch { "Confirm-SecureBootUEFI: $($_.Exception.Message)" }
        try { Get-Tpm -ErrorAction Stop | Select-Object TpmPresent, TpmReady, TpmEnabled, ManufacturerIdTxt, ManufacturerVersion } catch { "Get-Tpm: $($_.Exception.Message)" }
    }
    '== TPM devices'
    Get-PnpDevice -Class SecurityDevices -PresentOnly -ErrorAction SilentlyContinue | Select-Object FriendlyName, Status
    "Firmware type: $(if (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State') { 'UEFI' } else { 'unknown' })"
}
Section 'system\storage-network.txt' {
    '== Disks (model only)'
    Get-CimInstance Win32_DiskDrive | Select-Object Model, InterfaceType, MediaType, @{ n = 'SizeGB'; e = { [math]::Round($_.Size / 1GB) } }
    '== Network adapters (model only)'
    Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.PhysicalAdapter } | Select-Object Name, Manufacturer, AdapterType
}

# ------------------------------------------------------------ GPUs
Note 'gpu'
$gpus = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)
Section 'gpu\video-controllers.txt' {
    $gpus | Select-Object Name, PNPDeviceID, DriverVersion, DriverDate, InstalledDisplayDrivers, VideoProcessor, AdapterCompatibility,
        AdapterRAM, CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate, Status | Format-List
}
$gpuSummary = New-Object System.Collections.ArrayList
# The ranges Windows assigned (Enum\<device>\Control AllocConfig, a REG_RESOURCE_LIST): WMI's Win32_DeviceMemoryAddress
# lists only ranges below 4 GB, so it never showed a resized VRAM BAR (a B580's 16 GB at 0x4000000000 was missing and
# the summary said "no Resizable BAR"). Descriptors are 20 bytes on x64; type 7 is a large memory range, its length
# shifted by 8, 16 or 32 bits (flags 0x200/0x400/0x800). Read with reg.exe (Get-ItemProperty does not return this type).
function Get-AllocRanges([string]$instanceId) {
    $q = & reg.exe query "HKLM\SYSTEM\CurrentControlSet\Enum\$instanceId\Control" /v AllocConfig 2>$null
    $line = $q | Where-Object { $_ -match 'REG_RESOURCE_LIST\s+([0-9A-Fa-f]+)' } | Select-Object -First 1
    if (-not $line -or $line -notmatch 'REG_RESOURCE_LIST\s+([0-9A-Fa-f]+)') { return $null }
    $hex = $Matches[1]; $v = New-Object byte[] ($hex.Length / 2)
    for ($i = 0; $i -lt $v.Length; $i++) { $v[$i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
    $out = @(); $o = 4
    $nfull = [BitConverter]::ToUInt32($v, 0)
    for ($f = 0; $f -lt $nfull -and $o + 16 -le $v.Length; $f++) {
        $o += 8; $cnt = [BitConverter]::ToUInt32($v, $o + 4); $o += 8
        for ($i = 0; $i -lt $cnt -and $o + 20 -le $v.Length; $i++) {
            $t = $v[$o]; $fl = [BitConverter]::ToUInt16($v, $o + 2)
            if ($t -eq 3 -or $t -eq 7) {
                $st = [BitConverter]::ToUInt64($v, $o + 4); $ln = [uint64][BitConverter]::ToUInt32($v, $o + 12)
                if ($t -eq 7) { if ($fl -band 0x200) { $ln = $ln * 256 } elseif ($fl -band 0x400) { $ln = $ln * 65536 } elseif ($fl -band 0x800) { $ln = $ln * 4294967296 } }
                $out += [pscustomobject]@{ Start = $st; Size = $ln }
            }
            $o += 20
        }
    }
    return ,$out
}
Section 'gpu\pnp-display.txt' {
    $devs = @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue)
    foreach ($dev in $devs) {
        "== $($dev.FriendlyName)  [$($dev.Status), present: $($dev.Present)]"
        "InstanceId: $($dev.InstanceId)"
        $keys = 'DEVPKEY_Device_HardwareIds', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_DriverDate', 'DEVPKEY_Device_DriverProvider',
            'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_LocationInfo', 'DEVPKEY_Device_LocationPaths', 'DEVPKEY_Device_ProblemCode',
            'DEVPKEY_PciDevice_CurrentLinkSpeed', 'DEVPKEY_PciDevice_CurrentLinkWidth', 'DEVPKEY_PciDevice_MaxLinkSpeed', 'DEVPKEY_PciDevice_MaxLinkWidth'
        foreach ($k in $keys) {
            $p = Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName $k -ErrorAction SilentlyContinue
            if ($p -and $null -ne $p.Data) { "{0}: {1}" -f ($k -replace '^DEVPKEY_', ''), (($p.Data | ForEach-Object { "$_" }) -join '; ') }
        }
        # memory ranges = BARs; one larger than 256 MB, or one above 4 GB, means Resizable BAR / Above 4G decoding is on
        $bars = @(); $bars4g = @()
        $alloc = Get-AllocRanges $dev.InstanceId
        if ($alloc) {
            foreach ($m in $alloc) {
                "memory range: 0x{0:X} - 0x{1:X} ({2} MB, assigned)" -f $m.Start, ($m.Start + $m.Size - 1), [math]::Round($m.Size / 1MB)
                $bars += $m.Size; $bars4g += ($m.Start -ge 4GB)
            }
        } else { try {
            $ent = Get-CimInstance Win32_PnPEntity -Filter ("DeviceID='" + ($dev.InstanceId -replace '\\', '\\') + "'") -ErrorAction Stop
            $mem = @(Get-CimAssociatedInstance -InputObject $ent -ResultClassName Win32_DeviceMemoryAddress -ErrorAction Stop)
            foreach ($m in $mem) {
                $size = [uint64]$m.EndingAddress - [uint64]$m.StartingAddress + 1
                "memory range: 0x{0:X} - 0x{1:X} ({2} MB)" -f [uint64]$m.StartingAddress, [uint64]$m.EndingAddress, [math]::Round($size / 1MB)
                $bars += $size
                $bars4g += ([uint64]$m.StartingAddress -ge 4GB)
            }
        } catch { "memory ranges: $($_.Exception.Message)" } }
        $hw = ''
        if ($dev.InstanceId -match 'VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4})&SUBSYS_([0-9A-F]{8})&REV_([0-9A-F]{2})') {
            $hw = "{0}:{1} sub {2}:{3} rev {4}" -f $Matches[1], $Matches[2], $Matches[3].Substring(4, 4), $Matches[3].Substring(0, 4), $Matches[4]
        }
        $big = ($bars | Measure-Object -Maximum).Maximum
        $hi = @($bars4g | Where-Object { $_ }).Count
        $rebar = if ($null -eq $big) { 'BARs unknown (no memory ranges reported)' } elseif ($big -gt 256MB) { "largest BAR $([math]::Round($big / 1MB)) MB (Resizable BAR active)" } else { "largest BAR $([math]::Round($big / 1MB)) MB (no Resizable BAR)" }
        if ($hi) { $rebar += ", $hi range(s) above 4 GB (Above 4G decoding on)" }
        $ver = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
        $dat = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_DriverDate -ErrorAction SilentlyContinue).Data
        if ($dev.Present) { [void]$gpuSummary.Add(("{0}  {1}  driver {2} ({3})  {4}" -f $dev.FriendlyName, $hw, $ver, $(if ($dat) { $dat.ToString('yyyy-MM-dd') } else { '?' }), $rebar)) }
        ''
    }
}
Section 'gpu\signed-drivers.txt' {
    Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceClass='DISPLAY' OR DeviceClass='MEDIA' OR DeviceClass='MONITOR'" -ErrorAction SilentlyContinue |
        Select-Object DeviceClass, DeviceName, Manufacturer, DriverProviderName, DriverVersion, DriverDate, InfName, HardWareID | Format-List
}
Section 'gpu\all-pci.txt' {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\*' } | Sort-Object InstanceId |
        Select-Object Class, FriendlyName, @{ n = 'HardwareId'; e = { ($_.InstanceId -split '\\')[1] } } | Format-Table -AutoSize
}

# ------------------------------------------------------------ monitors (EDID)
Note 'display'
function Get-EdidByte([byte[]]$e, [int]$i) { if ($i -lt $e.Length) { return [int]$e[$i] } else { return 0 } }
function Clean-Edid([byte[]]$e) {
    $c = [byte[]]$e.Clone()
    if ($c.Length -lt 128) { return $c }
    for ($i = 12; $i -le 15; $i++) { $c[$i] = 0 }                       # serial number
    foreach ($d in 54, 72, 90, 108) {                                     # serial-number text descriptor
        if ($c[$d] -eq 0 -and $c[$d + 1] -eq 0 -and $c[$d + 3] -eq 0xFF) { $c[$d + 5] = 10; for ($j = $d + 6; $j -le $d + 17; $j++) { $c[$j] = 32 } }
    }
    $sum = 0; for ($i = 0; $i -lt 127; $i++) { $sum += $c[$i] }
    $c[127] = [byte]((256 - ($sum % 256)) % 256)
    return $c
}
function Describe-Edid([byte[]]$e) {
    if ($e.Length -lt 128) { return "short EDID ($($e.Length) bytes)" }
    $m = $e[8] * 256 + $e[9]
    $mfg = [string][char](64 + (($m -shr 10) -band 31)) + [char](64 + (($m -shr 5) -band 31)) + [char](64 + ($m -band 31))
    $prod = '{0:x4}' -f ($e[11] * 256 + $e[10])
    $name = ''
    foreach ($d in 54, 72, 90, 108) {
        if ($e[$d] -eq 0 -and $e[$d + 1] -eq 0 -and $e[$d + 3] -eq 0xFC) {
            for ($j = $d + 5; $j -le $d + 17; $j++) { if ($e[$j] -eq 10) { break }; $name += [char]$e[$j] }
        }
    }
    $pc = ($e[54] + $e[55] * 256) * 10000
    $w = $e[56] + ($e[58] -shr 4) * 256; $ht = $w + $e[57] + ($e[58] -band 15) * 256
    $h = $e[59] + ($e[61] -shr 4) * 256; $vt = $h + $e[60] + ($e[61] -band 15) * 256
    $hz = if ($ht * $vt -gt 0) { $pc / ($ht * $vt) } else { 0 }
    return ('{0} {1} "{2}" preferred {3}x{4}@{5:N2}, EDID {6}.{7}, {8} extension block(s)' -f $mfg, $prod, $name.Trim(), $w, $h, $hz, $e[18], $e[19], $e[126])
}
$present = @{}
Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object { $present[$_.InstanceId.ToUpper()] = $true }
$monSummary = New-Object System.Collections.ArrayList
$n = 0
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY' -ErrorAction SilentlyContinue | ForEach-Object {
    $model = $_.PSChildName
    Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue | ForEach-Object {
        $inst = "DISPLAY\$model\$($_.PSChildName)"
        $edid = (Get-ItemProperty (Join-Path $_.PSPath 'Device Parameters') -Name EDID -ErrorAction SilentlyContinue).EDID
        if ($edid) {
            $n++
            $clean = Clean-Edid ([byte[]]$edid)
            $hex = ($clean | ForEach-Object { $_.ToString('x2') }) -join ''
            $isNow = $present.ContainsKey($inst.ToUpper())
            $desc = Describe-Edid $clean
            Set-Content -Path (Join-Path $W "display\edid-$n-$model.hex") -Value $hex -Encoding ASCII
            Add-Content -Path (Join-Path $W 'display\monitors.txt') -Value ("monitor {0}: {1} ({2}) {3}" -f $n, $model, $(if ($isNow) { 'connected now' } else { 'seen before' }), $desc) -Encoding UTF8
            if ($isNow) { [void]$monSummary.Add("$model  $desc") }
        }
    }
}
Section 'display\connections.txt' {
    Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorConnectionParams -ErrorAction SilentlyContinue | Select-Object InstanceName, VideoOutputTechnology, Active
    '(VideoOutputTechnology: 10 = DisplayPort, 5 = HDMI, 11 = embedded DisplayPort (laptop panel), 0x80000000 = internal)'
}

# ------------------------------------------------------------ dxdiag
Note 'dxdiag'
$dx = Join-Path $Tmp 'dxdiag.txt'
$wait = [math]::Max(5, [math]::Min(45, (Left) - 5))
try {
    $p = Start-Process -FilePath "$env:SystemRoot\System32\dxdiag.exe" -ArgumentList '/whql:off', '/t', "`"$dx`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit($wait * 1000)) { try { $p.Kill() } catch {}; Skip "dxdiag /t: did not finish in $wait s" }
    $t0 = Get-Date
    while (-not (Test-Path $dx) -and ((Get-Date) - $t0).TotalSeconds -lt 3) { Start-Sleep -Milliseconds 250 }
    if (Test-Path $dx) {
        Get-Content $dx | Where-Object { $_ -notmatch '^\s*(Machine name|Machine Id|User Name)\s*:' } |
            Set-Content (Join-Path $W 'gpu\dxdiag.txt') -Encoding UTF8
    } elseif ($p.HasExited) { Skip 'dxdiag /t: wrote no file' }
} catch { Skip "dxdiag /t: $($_.Exception.Message)" }

# ------------------------------------------------------------ ACPI (-Acpi)
# GetSystemFirmwareTable: no admin, no driver. It returns the first table of a signature, so of several SSDTs only
# the first; the full list of signatures is recorded. Binary (.bin): redaction leaves it alone, the final check reads it.
$acpiNote = 'not asked for (-Acpi)'
if ($Acpi) {
    Note 'acpi'
    try {
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class XeFw {
  [DllImport("kernel32.dll")] public static extern uint EnumSystemFirmwareTables(uint p, byte[] b, uint n);
  [DllImport("kernel32.dll")] public static extern uint GetSystemFirmwareTable(uint p, uint id, byte[] b, uint n);
}
"@
        $prov = 0x41435049   # 'ACPI'
        $n = [XeFw]::EnumSystemFirmwareTables($prov, $null, 0); $buf = New-Object byte[] $n; [void][XeFw]::EnumSystemFirmwareTables($prov, $buf, $n)
        $sigs = for ($i = 0; $i + 4 -le $n; $i += 4) { [Text.Encoding]::ASCII.GetString($buf, $i, 4) }
        $ad = Join-Path $W 'acpi'; New-Item -ItemType Directory -Force -Path $ad | Out-Null
        Set-Content -Path (Join-Path $ad 'table-names.txt') -Value (($sigs | Where-Object { $_ -notin 'MSDM', 'SLIC', 'WPBT' }) -join "`r`n") -Encoding ASCII
        $got = @()
        foreach ($t in 'DSDT', 'SSDT') {   # the DSDT is not in the list (the FADT points to it), but Windows returns it
            if ($t -eq 'SSDT' -and $sigs -notcontains $t) { continue }
            $tid = [BitConverter]::ToUInt32([Text.Encoding]::ASCII.GetBytes($t), 0)
            $sz = [XeFw]::GetSystemFirmwareTable($prov, $tid, $null, 0)
            if ($sz -gt 0) { $tb = New-Object byte[] $sz; [void][XeFw]::GetSystemFirmwareTable($prov, $tid, $tb, $sz); [IO.File]::WriteAllBytes((Join-Path $ad "$t.bin"), $tb); $got += "$t $sz bytes" }
        }
        $nssdt = @($sigs | Where-Object { $_ -eq 'SSDT' }).Count
        $acpiNote = "$($got -join ', ') (Windows hands out only the first of $nssdt SSDTs; Linux --acpi has all)"
    } catch { Skip "ACPI tables: $($_.Exception.Message)"; $acpiNote = 'failed' }
}

# ------------------------------------------------------------ boot logs (-BootLogs)
# OpenCore's boot logging (IntelXe-EFI.ps1 -Plan bootlog: Misc > Debug > Target 0x41, AppleDebug, ApplePanic) writes
# opencore-YYYY-MM-DD-HHMMSS.txt and, at the boot after a macOS panic, panic-YYYY-MM-DD-HHMMSS.txt into the root of the
# EFI volume it started from. Searched: FAT volumes with a drive letter (USB sticks; no administrator needed) and, as
# administrator, the EFI partitions without one: mounted with mountvol at a free letter as IntelXe-EFI.ps1's Mount-Esp
# does, only read, then unmounted (mountvol /D) after checking that the letter still holds an EFI partition. Of each
# volume: the newest 5 OpenCore logs and 10 panics (up to 1.5 MB each, without the zero bytes OpenCore pads its log
# with) and the AppleDebug lines (AAPL:, EBPF:) of those logs on their own. config.plist is read only for its private
# values (SystemSerialNumber, MLB, ROM, SystemUUID, ...), which OpenCore and boot.efi can print: they join the values
# removed from every file. Nothing is written to any volume.
$EspType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$bootNote = 'not asked for (-BootLogs)'
function Add-PlistLiterals([string]$path) {
    try {
        $xs = New-Object System.Xml.XmlReaderSettings
        $xs.DtdProcessing = [System.Xml.DtdProcessing]::Ignore; $xs.XmlResolver = $null
        $r = [System.Xml.XmlReader]::Create($path, $xs)
        try { $doc = New-Object System.Xml.XmlDocument; $doc.Load($r) } finally { $r.Close() }
        foreach ($k in @($doc.SelectNodes('//key'))) {
            if ($k.InnerText -notmatch '^(SystemSerialNumber|MLB|SystemUUID|ROM|BoardSerialNumber|ChassisSerialNumber|ApECID)$') { continue }
            $n = $k.NextSibling
            while ($null -ne $n -and $n.NodeType -ne [System.Xml.XmlNodeType]::Element) { $n = $n.NextSibling }
            if ($null -eq $n) { continue }
            $v = ($n.InnerText -replace '\s', '')
            Lit oc $v
            if ($n.Name -eq 'data' -and $v -ne '') {
                try {
                    $bytes = [Convert]::FromBase64String($v)
                    $hex = ($bytes | ForEach-Object { $_.ToString('x2') }) -join ''
                    if ($hex -match '[1-9a-f]') { Lit oc $hex; Lit oc (($bytes | ForEach-Object { $_.ToString('x2') }) -join ':') }
                } catch {}
            }
        }
    } catch { Note "boot logs: could not read $path for its private values: $($_.Exception.Message)" }
}
# The text of a log: up to 1.5 MB, cut at the first zero byte (OpenCore writes its whole 256 KB buffer each time)
function Get-LogBytes([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $end = [Array]::IndexOf($b, [byte]0)
    if ($end -lt 0) { $end = $b.Length }
    $end = [math]::Min($end, 1500000)
    $out = New-Object byte[] $end
    [Array]::Copy($b, $out, $end)
    return ,$out
}
function Read-BootLogs([string]$root, [string]$what, [string]$dir) {
    $cfg = Join-Path $root 'EFI\OC\config.plist'
    if (Test-Path -LiteralPath $cfg) { Add-PlistLiterals $cfg }
    $all = @(Get-ChildItem -LiteralPath $root -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(opencore|panic)-[0-9]{4}-[0-9-]+\.txt$' })
    $oc = @($all | Where-Object { $_.Name -like 'opencore-*' } | Sort-Object Name -Descending)
    $pn = @($all | Where-Object { $_.Name -like 'panic-*' } | Sort-Object Name -Descending)
    $line = "${what}: $($oc.Count) OpenCore log(s), $($pn.Count) panic(s)$(if (Test-Path -LiteralPath $cfg) { ', EFI\OC\config.plist' } else { '' })"
    Add-Content -Path (Join-Path $script:bd 'where.txt') -Value $line -Encoding UTF8
    if ($all.Count -eq 0) { return 0 }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Set-Content -Path (Join-Path $dir 'volume.txt') -Encoding UTF8 -Value (@($line, '', 'files in the root (name, bytes, last written):') +
        @($all | Sort-Object Name | ForEach-Object { '{0}  {1}  {2}' -f $_.Name, $_.Length, $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') }))
    $apple = New-Object System.Collections.ArrayList
    foreach ($f in @($oc | Select-Object -First 5) + @($pn | Select-Object -First 10)) {
        try {
            $bytes = Get-LogBytes $f.FullName
            [IO.File]::WriteAllBytes((Join-Path $dir $f.Name), $bytes)
            if ($f.Name -like 'opencore-*') {
                $lines = @([Text.Encoding]::UTF8.GetString($bytes) -split "`r?`n" | Where-Object { $_ -match ' (AAPL|EBPF): ' })
                if ($lines.Count) { [void]$apple.Add("== $($f.Name)"); foreach ($l in $lines) { [void]$apple.Add($l) }; [void]$apple.Add('') }
            }
        } catch { Skip "boot logs: $($f.Name) on ${what}: $($_.Exception.Message)" }
    }
    if ($apple.Count) { Set-Content -Path (Join-Path $dir 'apple-debug.txt') -Value $apple -Encoding UTF8 }
    return $all.Count
}
if ($BootLogs) {
    Note 'boot logs'
    $script:bd = Join-Path $W 'bootlogs'
    New-Item -ItemType Directory -Force -Path $bd | Out-Null
    $nvol = 0; $nfiles = 0; $seen = @{}
    try {
        foreach ($v in @(Get-Volume -ErrorAction Stop | Where-Object { $null -ne $_.DriveLetter -and [int][char]$_.DriveLetter -ne 0 -and "$($_.FileSystem)" -match '^(FAT|FAT12|FAT16|FAT32|exFAT)$' })) {
            $l = "$($v.DriveLetter)".ToUpperInvariant()
            $seen[$l] = $true; $nvol++
            $nfiles += Read-BootLogs "$($l):\" "drive $($l): ($($v.DriveType), $($v.FileSystem))" (Join-Path $bd "drive-$l")
        }
    } catch { Skip "boot logs: the list of volumes: $($_.Exception.Message)" }
    if ($IsAdmin) {
        $mountvol = Join-Path $env:SystemRoot 'System32\mountvol.exe'
        try {
            foreach ($p in @(Get-Partition -ErrorAction Stop | Where-Object { "$($_.GptType)" -eq $EspType })) {
                if ((Left) -lt 10) { Skip 'boot logs: time budget used up before every EFI partition was read'; break }
                $disk = 'unknown disk'
                try { $disk = "$((Get-Disk -Number $p.DiskNumber -ErrorAction Stop).FriendlyName)" } catch {}
                $what = "EFI partition, $disk, disk $($p.DiskNumber) partition $($p.PartitionNumber)"
                $has = $null
                if ($null -ne $p.DriveLetter -and [int][char]$p.DriveLetter -ne 0) { $has = "$($p.DriveLetter)".ToUpperInvariant() }
                if ($has -and $seen.ContainsKey($has)) { Add-Content -Path (Join-Path $bd 'where.txt') -Value "$($what): read as drive $($has):" -Encoding UTF8; continue }
                $nvol++
                if ($has) { $nfiles += Read-BootLogs "$($has):\" $what (Join-Path $bd "esp-$($p.DiskNumber)-$($p.PartitionNumber)"); continue }
                $vol = @($p.AccessPaths | Where-Object { "$_" -like '\\?\Volume{*' }) | Select-Object -First 1
                if (-not $vol) { Skip "boot logs: $what has no volume path to mount"; continue }
                $used = @([IO.Directory]::GetLogicalDrives() | ForEach-Object { $_.Substring(0, 1).ToUpperInvariant() })
                $letter = $null
                foreach ($c in 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'R', 'Q', 'P', 'O', 'N', 'M') { if ($used -notcontains $c) { $letter = "$($c):"; break } }
                if (-not $letter) { Skip 'boot logs: no free drive letter to mount an EFI partition at'; break }
                $out = & $mountvol $letter "$vol" 2>&1
                if ($LASTEXITCODE -ne 0) { Skip "boot logs: mountvol could not mount ${what}: $out"; continue }
                try { $nfiles += Read-BootLogs "$letter\" $what (Join-Path $bd "esp-$($p.DiskNumber)-$($p.PartitionNumber)") }
                finally {
                    $q = $null
                    try { $q = Get-Partition -DriveLetter $letter.Substring(0, 1) -ErrorAction Stop } catch {}
                    if ($null -ne $q -and "$($q.GptType)" -eq $EspType) { $null = & $mountvol $letter /D 2>&1 }
                    else { Skip "boot logs: $letter no longer holds an EFI partition, so it was not unmounted" }
                }
            }
        } catch { Skip "boot logs: the EFI partitions: $($_.Exception.Message)" }
    } else {
        Skip 'boot logs on the EFI partitions: Windows lets only an administrator mount them (run as administrator); FAT volumes with a drive letter were searched'
    }
    $bootNote = "$nfiles file(s) on $nvol volume(s) searched (bootlogs\where.txt)"
    if (-not $IsAdmin) { $bootNote += '; EFI partitions not read (not administrator)' }
}

# ------------------------------------------------------------ summary
Note 'summary'
$os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
$csys = Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue
$bb = Get-CimInstance Win32_BaseBoard -ErrorAction SilentlyContinue | Select-Object -First 1
$bios = Get-CimInstance Win32_BIOS -ErrorAction SilentlyContinue | Select-Object -First 1
$cpu = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
$sbv = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -ErrorAction SilentlyContinue).UEFISecureBootEnabled
$tpm = @(Get-PnpDevice -Class SecurityDevices -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match 'Trusted Platform' })
$S = New-Object System.Collections.ArrayList
[void]$S.Add("Intel GPU: Windows hardware inventory $ID")
[void]$S.Add("Created $(Get-Date -Format 'yyyy-MM-dd HH:mm zzz') by collect-windows.ps1 v$CollectorVersion")
[void]$S.Add('')
[void]$S.Add("Windows      $($os.Caption) $($cv.DisplayVersion) build $($os.BuildNumber).$($cv.UBR) $($os.OSArchitecture)")
[void]$S.Add("Machine      $($csys.Manufacturer) $($csys.Model) $($csys.SystemFamily)")
[void]$S.Add("Board        $($bb.Manufacturer) $($bb.Product) $($bb.Version)")
[void]$S.Add("BIOS         $($bios.Manufacturer) $($bios.SMBIOSBIOSVersion) ($(if ($bios.ReleaseDate) { $bios.ReleaseDate.ToString('yyyy-MM-dd') }))")
[void]$S.Add("CPU          $($cpu.Name), $($cpu.NumberOfCores) cores / $($cpu.NumberOfLogicalProcessors) threads, $([math]::Round($csys.TotalPhysicalMemory / 1GB)) GB RAM")
[void]$S.Add("Secure Boot  $(switch ($sbv) { 1 { 'on' } 0 { 'off' } default { 'unknown' } })   TPM: $(if ($tpm.Count) { $tpm[0].FriendlyName } else { 'not found' })")
[void]$S.Add('')
[void]$S.Add('GPUs')
if ($gpuSummary.Count) { foreach ($g in $gpuSummary) { [void]$S.Add("  $g") } } else { [void]$S.Add('  none found') }
[void]$S.Add('')
[void]$S.Add('Monitors connected now')
if ($monSummary.Count) { foreach ($m in $monSummary) { [void]$S.Add("  $m") } } else { [void]$S.Add('  no EDID found in the registry') }
[void]$S.Add('')
[void]$S.Add("ACPI tables  $acpiNote")
[void]$S.Add("Boot logs    $bootNote")
[void]$S.Add('')
[void]$S.Add('Note: Windows cannot show the "Above 4G decoding" BIOS switch directly. A GPU memory range above')
[void]$S.Add('4 GB or larger than 256 MB (see gpu\pnp-display.txt) means it is on and Resizable BAR is active.')
if ($Skipped.Count) {
    [void]$S.Add('')
    [void]$S.Add('Not collected')
    foreach ($sk in $Skipped) { [void]$S.Add("  - $sk") }
}
[void]$S.Add('')
[void]$S.Add('Removed before packing: serial numbers, UUIDs, machine GUID, MAC addresses, user and computer names,')
[void]$S.Add('Wi-Fi network names, monitor serial numbers (EDID). Product keys are never read; ACPI tables only with -Acpi')
[void]$S.Add('(DSDT and the first SSDT; never MSDM, SLIC or WPBT). With -BootLogs, also the serial, MLB, ROM and UUID values')
[void]$S.Add('of the OpenCore config.plist found beside the logs.')
Set-Content -Path (Join-Path $W 'summary.txt') -Value $S -Encoding UTF8

# ------------------------------------------------------------ redaction and checks
Note 'redaction'
$lits = $Literals | Sort-Object { $_.Value.Length } -Descending
$files = Get-ChildItem -Path $W -Recurse -File | Where-Object { $_.Extension -ne '.bin' }   # binary tables stay intact
foreach ($f in $files) {
    $t = [System.IO.File]::ReadAllText($f.FullName)
    foreach ($l in $lits) { $t = $t -replace [regex]::Escape($l.Value), "<$($l.Kind)-redacted>" }
    $t = $t -replace '(?i)([A-Z]:\\Users\\)[^\\\s"'']+', '$1USER'
    $t = $t -replace '(?i)\b([0-9A-F]{2}[:-]){5}[0-9A-F]{2}\b', 'xx-xx-xx-xx-xx-xx'
    $t = $t -replace '(?im)^(\s*(Serial ?Number|SerialNumber|UUID|IdentifyingNumber|MACAddress|Machine (name|Id))\s*:\s*).+$', '$1<redacted>'
    # OpenCore's own lines that print an identifier (OcSmbiosLib, OpenCorePlatform.c), for a log whose config.plist was not found
    $t = $t -replace '((SSN|MLB|UUID) from NVRAM took precedence: )\S+', '$1<redacted>'
    $t = $t -replace '(Grabbed SB uuid )\S+', '$1<redacted>'
    $t = $t -replace '(ApECID )[0-9]+', '$1<redacted>'
    [System.IO.File]::WriteAllText($f.FullName, $t, (New-Object System.Text.UTF8Encoding($false)))
}
$fail = $false
foreach ($f in (Get-ChildItem -Path $W -Recurse -File)) {
    # binary files as Latin-1, byte for byte, so a value or key in them is still found
    $t = if ($f.Extension -eq '.bin') { [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($f.FullName)) } else { [System.IO.File]::ReadAllText($f.FullName) }
    foreach ($l in $lits) {
        if ($t.IndexOf($l.Value, [StringComparison]::OrdinalIgnoreCase) -ge 0) { Write-Warning "privacy check: a $($l.Kind) value is still present in $($f.Name)"; $fail = $true }
    }
    if ($t -cmatch '\b[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}\b') {
        Write-Warning "privacy check: $($f.Name) contains something shaped like a Windows product key"; $fail = $true
    }
}
if ($fail) {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
    Write-Host 'No report was written. Please open an issue and say that the privacy check failed (no file needed).'
    exit 3
}

# ------------------------------------------------------------ pack
Add-Content -Path $Log -Value @('', "finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz') ($([int]$sw.Elapsed.TotalSeconds) s)", 'privacy check: passed') -Encoding UTF8
$man = Get-ChildItem -Path $W -Recurse -File | Sort-Object FullName | ForEach-Object {
    $h = Get-FileHash -Algorithm SHA256 -Path $_.FullName
    '{0}  ./{1}' -f $h.Hash.ToLower(), ($_.FullName.Substring($W.Length + 1) -replace '\\', '/')
}
Set-Content -Path (Join-Path $W 'manifest.txt') -Value $man -Encoding ASCII
$zip = Join-Path $OutDir "$Name.zip"
try { Compress-Archive -Path $W -DestinationPath $zip -Force -ErrorAction Stop }
catch { Write-Host "could not write $zip : $($_.Exception.Message)"; Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue; exit 1 }
Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
Write-Host ''
Write-Host "Report ID: $ID"
Write-Host "File:      $zip"
Write-Host "Took $([int]$sw.Elapsed.TotalSeconds) s. Nothing was uploaded. Attach the file to your GitHub issue and put the report ID in the form."
