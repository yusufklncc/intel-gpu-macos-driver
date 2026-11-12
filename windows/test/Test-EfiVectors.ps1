# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Test-EfiVectors.ps1 [-Vectors FILE]: the shared test vectors of the EFI rules (release\efi-rules-vectors.json; the
format is in docs\design\DESIGN-efi-rules.md section 5) against windows\IntelXe-EFI.ps1: its rules loader
(ConvertFrom-Json, Get-EfiRules) and its engine, in this PowerShell, in a throwaway folder.

.DESCRIPTION
For each vector: the package and the EFI folder it describes, config.plist as stage 1 leaves it with the vector's
edits, the facts as Windows gives them (the card's Location path, which the user confirms, when its instance id is one
of the rules' cards; the CPU's name; the boot path is never recorded on Windows, but a vector may say it is), then the
plan's steps and notes, the change made with the plan's hash, the values, files and list of changes it must leave, and
undo back to the state before. Then each CPU name (HybridCpu) and each PCI device (IsCardInstance).
release/test-efi-vectors.sh runs the same vectors against efi.sh on macOS. Test-IntelXe-EFI.ps1 runs this script.
Needs nothing that only Windows has: it also runs in PowerShell 7 on macOS.

  powershell -ExecutionPolicy Bypass -File windows\test\Test-EfiVectors.ps1      (Windows PowerShell 5.1)
  pwsh -File windows\test\Test-EfiVectors.ps1                                    (PowerShell 7)
#>
param([string]$Vectors = '')

$Here = $PSScriptRoot
$Repo = Split-Path -Parent (Split-Path -Parent $Here)
$ToolPath = Join-Path (Split-Path -Parent $Here) 'IntelXe-EFI.ps1'
if ($Vectors -eq '') { $Vectors = Join-Path (Join-Path $Repo 'release') 'efi-rules-vectors.json' }
$VectorsPath = $Vectors

# Dot-sourcing defines the tool's parameters here too ($Plan, $Efi, $Package, ...): this script uses none of those names.
. $ToolPath   # the engine, Get-EfiRules, ConvertTo-RulesTree, New-EfiFolder; dot-sourced, its command line does not run
$ErrorActionPreference = 'Stop'

$script:fails = 0
$script:count = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:fails++ }
}
function Key($d, [string]$k) { if ($null -ne $d -and $d.ContainsKey($k)) { return , $d[$k] } return $null }   # the value itself
function Items($d, [string]$k) { if ($null -ne $d -and $d.ContainsKey($k)) { return $d[$k] } }               # a list's items, one by one
function Count($l) { if ($null -eq $l) { return 0 } return $l.Count }
function Native([string]$rel) { return [IntelXeEfi.Util]::Native($rel) }
function Oc([string]$dir, [string]$rel) { return Join-Path (Join-Path (Join-Path $dir 'EFI') 'OC') (Native $rel) }
function Get-HexBytes([string]$hex) {
    $b = New-Object byte[] ($hex.Length / 2)
    for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
    return , $b
}

# A value of the vectors' JSON as a config.plist value of the type it names.
function Get-PValue([string]$type, $value) {
    switch ($type) {
        'integer' { return [IntelXeEfi.PValue]::Int([long]$value) }
        'bool' { return [IntelXeEfi.PValue]::Bool([bool]$value) }
        'string' { return [IntelXeEfi.PValue]::Str([string]$value) }
        'data' { return [IntelXeEfi.PValue]::Data([Convert]::FromBase64String([string]$value)) }
        'dict' { return [IntelXeEfi.Rules]::Value($value) }
    }
    throw "vectors: no value type $type"
}
# one edit (a step, or remove) on a loaded config.plist
function Invoke-Edit($pl, $e) {
    [string[]]$path = ([string]$e['path']).Split('.')
    switch ($e['op']) {
        'set' { $pl.Set($path, (Get-PValue $e['type'] $e['value'])) }
        'remove' { $pl.Remove($path) }
        'append' { $pl.Append($path, (Get-PValue $e['type'] $e['value'])) }
        'insert' { $pl.Insert($path, [int]$e['index'], (Get-PValue $e['type'] $e['value'])) }
        default { throw "vectors: no edit $($e['op'])" }
    }
}

# A fixture: the file it describes at DIR\EFI\OC\<its file> (a stand-in kext, a table, a folder, a copy of the package's)
function New-Fixture([string]$dir, $fx, [string]$pkg) {
    $rel = [string]$fx['file']
    if ($rel -eq 'SHA256SUMS') { return }   # the package's sums: Write-Sums, last
    $d = Oc $dir $rel
    [IntelXeEfi.Util]::Delete($d)
    $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $d))
    switch ($fx['type']) {
        'kext' {
            $exe = Join-Path $d (Native $fx['executable'])
            $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $exe))
            [IO.File]::WriteAllText($exe, [string]$fx['text'] + "`n")
            $keys = ''
            if ($fx['bundle_id'] -ne '') { $keys += "`t<key>CFBundleIdentifier</key>`n`t<string>$($fx['bundle_id'])</string>`n" }
            if ($fx['version'] -ne '') { $keys += "`t<key>CFBundleVersion</key>`n`t<string>$($fx['version'])</string>`n" }
            [IO.File]::WriteAllText((Join-Path (Join-Path $d 'Contents') 'Info.plist'), "<?xml version=`"1.0`" encoding=`"UTF-8`"?>`n<plist version=`"1.0`">`n<dict>`n$keys</dict>`n</plist>`n")
        }
        'table' { [IO.File]::WriteAllBytes($d, (Get-HexBytes $script:tables[$fx['table']])) }
        'dir' { $null = [IO.Directory]::CreateDirectory((Join-Path (Join-Path $d 'Contents') 'MacOS')) }
        'package' {
            [IntelXeEfi.Util]::CopyTree((Oc $pkg $rel), $d)
            $more = Key $fx 'append'
            if ($null -ne $more) { foreach ($x in [IO.Directory]::GetFiles((Join-Path (Join-Path $d 'Contents') 'MacOS'))) { [IO.File]::AppendAllText($x, [string]$more + "`n") } }
        }
        'none' { }
        default { throw "vectors: no fixture type $($fx['type'])" }
    }
}
function Write-Sums([string]$dir) {
    $lines = @()
    $efi = Join-Path $dir 'EFI'
    $files = @([IO.Directory]::GetFiles($efi, '*', [IO.SearchOption]::AllDirectories) | ForEach-Object { $_.Substring($dir.Length + 1).Replace('\', '/') } | Sort-Object -CaseSensitive)
    foreach ($rel in $files) { $lines += [IntelXeEfi.Util]::Sha256File((Join-Path $dir (Native $rel))) + '  ' + $rel }
    [IO.File]::WriteAllText((Join-Path $dir 'SHA256SUMS'), ($lines -join "`n") + "`n")
}
# Every file under DIR but config.plist, with its SHA-256: two trees hold the same when these are the same.
function Get-Tree([string]$dir) {
    $out = @()
    foreach ($f in @([IO.Directory]::GetFiles($dir, '*', [IO.SearchOption]::AllDirectories))) {
        if ([IO.Path]::GetFileName($f) -eq 'config.plist') { continue }
        $out += $f.Substring($dir.Length).Replace('\', '/') + ' ' + [IntelXeEfi.Util]::Sha256File($f)
    }
    return ((@($out | Sort-Object -CaseSensitive)) -join "`n")
}

# A plan's step in the vectors' form, and any tree as one comparable string (keys sorted).
function Get-ValueTree($v) {
    switch ($v.Type) {
        'dict' {
            $d = New-Object 'System.Collections.Generic.Dictionary[string,object]'
            foreach ($kv in $v.Entries) { $d.Add($kv.Key, (Get-ValueTree $kv.Value)) }
            return , $d
        }
        'bool' { return ($v.Text -eq 'true') }
        'integer' { return [long]$v.Text }
        default { return $v.Text }
    }
}
function ConvertFrom-Op($op) {
    $d = New-Object 'System.Collections.Generic.Dictionary[string,object]'
    $d.Add('op', $op.Kind)
    switch ($op.Kind) {
        'copy' { $d.Add('to', $op.Rel); $d.Add('from', $(if ($null -ne $op.Bytes) { 'adapted' } else { 'package' })) }
        { $_ -eq 'set' -or $_ -eq 'append' -or $_ -eq 'insert' } {
            $d.Add('path', ($op.Path -join '.'))
            if ($op.Kind -eq 'insert') { $d.Add('index', [long]$op.Index) }
            $d.Add('type', $op.Value.Type)
            $d.Add('value', (Get-ValueTree $op.Value))
        }
        { $_ -eq 'mkdict' -or $_ -eq 'mkarray' } { $d.Add('path', ($op.Path -join '.')) }
    }
    return , $d
}
function Get-Canon($v) {
    if ($null -eq $v) { return 'null' }
    if ($v -is [string]) { return 's"' + $v.Replace('\', '\\').Replace('"', '\"') + '"' }
    if ($v -is [bool]) { return 'b' + $v }
    if ($v -is [long] -or $v -is [int]) { return 'i' + $v }
    if ($v -is [System.Collections.IDictionary]) {
        $keys = New-Object 'System.Collections.Generic.List[string]'
        foreach ($k in $v.Keys) { $keys.Add([string]$k) }
        $keys.Sort([StringComparer]::Ordinal)
        $parts = @()
        foreach ($k in $keys) { $parts += (Get-Canon $k) + '=' + (Get-Canon $v[$k]) }
        return '{' + ($parts -join ',') + '}'
    }
    if ($v -is [System.Collections.IList]) {
        $parts = @()
        foreach ($x in $v) { $parts += Get-Canon $x }
        return '[' + ($parts -join ',') + ']'
    }
    throw "vectors: cannot compare a $($v.GetType().Name)"
}

# The facts as Windows gives them: a PCI device's instance id, and the card's Location path in Device Manager (the
# port's ACPI names, then one PCI part per level down to the GPU; "none": the port is a PCI part too)
function Get-InstanceId([string]$pci) {
    $p = $pci.Split(':')
    return "PCI\VEN_$($p[0].ToUpperInvariant())&DEV_$($p[1].ToUpperInvariant())&SUBSYS_00000000&REV_00\4&0&0&0008"
}
function Get-LocationPath([string]$port, [long]$levels) {
    $parts = @()
    $pci = $levels
    if ($port -eq 'none') { $parts = @('ACPI(_SB_)', 'ACPI(PC00)'); $pci = $levels + 1 }
    else { foreach ($s in $port.TrimStart('\').Split('.')) { $parts += "ACPI($($s.PadRight(4, '_')))" } }
    for ($i = 0; $i -lt $pci; $i++) { $parts += 'PCI(0000)' }
    return $parts -join '#'
}

$Tmp = [IO.Path]::GetTempPath()
$Root = Join-Path $Tmp ('ixevec-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$quiet = [Action[string]] { param($line) }
try {
    $null = [IO.Directory]::CreateDirectory($Root)
    $rules = Get-EfiRules
    $levels = $rules.Int('cards.port_levels_above_gpu')
    $vt = (ConvertTo-RulesTree ([IO.File]::ReadAllText($VectorsPath, [Text.Encoding]::UTF8) | ConvertFrom-Json) 'vectors').PSObject.BaseObject
    if ($vt['format'] -ne 'intelxe-efi-rules-vectors' -or $vt['format_version'] -ne 1) { throw "$VectorsPath is not intelxe-efi-rules-vectors 1" }
    Write-Host "== $($PSVersionTable.PSEdition) PowerShell $($PSVersionTable.PSVersion); $VectorsPath; the rules $($rules.Source)"
    $script:tables = $vt['tables']

    # the package every vector starts from, and config.plist as stage 1 leaves it
    $P0 = Join-Path $Root 'pkg0'
    foreach ($fx in $vt['package']) { New-Fixture $P0 $fx '' }
    $stage1 = [IntelXeEfi.Plist]::Load((Join-Path $Repo (Native $vt['config'])))
    foreach ($e in $vt['stage1']) { Invoke-Edit $stage1 $e }
    $stage1Bytes = $stage1.ToBytes()

    $n = 0
    foreach ($V in $vt['vectors']) {
        $n++
        $name = $V['name']
        $W = Join-Path $Root ('v' + $n)
        $P = Join-Path $W 'pkg'
        $F = Join-Path $W 'efi'
        $Bk = Join-Path $W 'backups'
        $Cfg = Oc $F 'config.plist'
        foreach ($d in 'Kexts', 'ACPI') { $null = [IO.Directory]::CreateDirectory((Oc $F $d)) }
        $null = [IO.Directory]::CreateDirectory((Join-Path (Join-Path $F 'EFI') 'BOOT'))
        [IO.File]::WriteAllBytes((Oc $F 'OpenCore.efi'), [byte[]]@())
        [IntelXeEfi.Util]::CopyTree($P0, $P)
        $sums = $true
        foreach ($fx in (Items $V 'package')) {
            if ($fx['file'] -eq 'SHA256SUMS' -and $fx['type'] -eq 'none') { $sums = $false }
            New-Fixture $P $fx ''
        }
        if ($sums) { Write-Sums $P }
        $pl = [IntelXeEfi.Plist]::Parse($stage1Bytes, 'stage 1')
        foreach ($e in (Items $V 'edits')) { Invoke-Edit $pl $e }
        [IO.File]::WriteAllBytes($Cfg, $pl.ToBytes())
        foreach ($fx in (Items $V 'efi')) { New-Fixture $F $fx $P }
        $before = [IntelXeEfi.Plist]::Canonical([IntelXeEfi.Plist]::Load($Cfg).Root)
        $beforeTree = Get-Tree $F

        # the facts
        $facts = $V['facts']
        $card = Key $facts 'card'
        $loc = ''
        $detected = ''
        if ($null -ne $card -and $rules.IsCardInstance((Get-InstanceId $card['pci']))) {
            $loc = Get-LocationPath $card['port'] $levels                         # what Device Manager shows
            $detected = [IntelXeEfi.EfiFolder]::ConvertLocationPath($loc, $levels)   # "Read from Windows"
        }
        $kind = $V['plan']
        function Open-Efi { $f = New-EfiFolder $F $P $Bk $loc $detected ([string]$facts['cpu']); $f.BootPathRecorded = [bool]$facts['boot_path_recorded']; return $f }
        $script:count++
        $x = $V['expect']

        # the plan: its steps and notes
        $made = (Open-Efi).MakePlan($kind)
        $got = New-Object 'System.Collections.Generic.List[object]'
        foreach ($op in $made.Ops) { $got.Add((ConvertFrom-Op $op)) }
        $same = (Get-Canon $got) -ceq (Get-Canon $x['ops'])
        Check $same "$($name): the plan's $(Count $x['ops']) steps"
        if (-not $same) { Write-Host "      the script: $(Get-Canon $got)"; Write-Host "      the vector: $(Get-Canon $x['ops'])"; Write-Host "      $($made.Text.Replace("`n", "`n      "))" }
        $notes = @($made.Notes)
        $want = @($x['notes'])
        $good = $notes.Count -eq (Count $x['notes'])
        for ($i = 0; $good -and $i -lt $notes.Count; $i++) { if (-not $notes[$i].Contains([string]$want[$i])) { $good = $false } }
        Check $good "$($name): the $(Count $x['notes']) note(s)"
        if (-not $good) { foreach ($l in $notes) { Write-Host "        $l" } }

        # make it, and what it must leave
        if ((Count $x['ops']) -eq 0) { continue }
        try { (Open-Efi).Run($kind, $made.Hash, $quiet) } catch { Check $false "$($name): the change failed: $(Get-ErrorText $_)"; continue }
        $why = ''
        $after = Key $x 'after'
        $done = [IntelXeEfi.Plist]::Load($Cfg)
        foreach ($a in (Items $after 'config')) {
            [string[]]$path = ([string]$a['path']).Split('.')
            $e = $done.Get($path)
            if ($null -ne (Key $a 'absent')) { if ($null -ne $e) { $why += " $($a['path']) is there;" } }
            else {
                $raw = [IntelXeEfi.Plist]::Raw($e)
                $wantRaw = (Get-PValue $a['type'] $a['value']).Raw
                if ($raw -cne $wantRaw) { $why += " $($a['path'])=$raw;" }
            }
        }
        foreach ($a in (Items $after 'files')) {
            $file = Oc $F $a['file']
            $state = Key $a 'state'
            if ($state -eq 'package') {
                $src = Oc $P $a['file']
                $same = if ([IO.Directory]::Exists($src)) { [IO.Directory]::Exists($file) -and (Get-Tree $src) -ceq (Get-Tree $file) } else { [IntelXeEfi.Util]::SameFile($src, $file) }
                if (-not $same) { $why += " $($a['file']) is not the package's;" }
            }
            elseif ($state -eq 'absent') { if ([IntelXeEfi.Util]::Exists($file)) { $why += " $($a['file']) is there;" } }
            elseif ($null -ne (Key $a 'sha256')) { if (-not [IO.File]::Exists($file) -or [IntelXeEfi.Util]::Sha256File($file) -ne $a['sha256']) { $why += " $($a['file'])'s SHA-256;" } }
            elseif ($null -ne (Key $a 'table')) { if (-not [IntelXeEfi.Util]::SameBytes([IO.File]::ReadAllBytes($file), (Get-HexBytes $script:tables[$a['table']]))) { $why += " $($a['file']) is not $($a['table']);" } }
        }
        if ((Count (Key $after 'changed')) -gt 0) {
            $b = (Open-Efi).LastBackup()
            $list = ''
            if ($null -ne $b) { $list = [IO.File]::ReadAllText((Join-Path $b 'changed')).TrimEnd("`n").Replace("`n", ' ') }
            if ($list -cne (@($after['changed']) -join ' ')) { $why += " the backup lists $list;" }
        }
        Check ($why -eq '') "$($name): applied, the values, files and list of changes$(if ($why -ne '') { ':' + $why })"

        # undo: back to the state before
        if (-not $x['undo']) { continue }
        $u = (Open-Efi).MakePlan('undo')
        (Open-Efi).Run('undo', $u.Hash, $quiet)
        $now = [IntelXeEfi.Plist]::Canonical([IntelXeEfi.Plist]::Load($Cfg).Root)
        Check ($now -ceq $before -and (Get-Tree $F) -ceq $beforeTree) "$($name): undo, config.plist and the files as before"
    }

    # which CPUs need HybridPM.kext, which PCI devices are cards
    $bad = 0
    foreach ($c in $vt['cpus']) {
        if ([IntelXeEfi.EfiFolder]::HybridCpu($rules, $c['cpu']) -ne [bool]$c['hybridpm']) { $bad++; Write-Host "      '$($c['cpu'])': the vectors say $($c['hybridpm'])" }
    }
    Check ($bad -eq 0) "the $(Count $vt['cpus']) CPU names: HybridPM.kext for the ones the vectors say"
    $bad = 0
    foreach ($c in $vt['cards']) {
        if ($rules.IsCardInstance((Get-InstanceId $c['pci'])) -ne [bool]$c['card']) { $bad++; Write-Host "      PCI $($c['pci']): the vectors say card=$($c['card'])" }
    }
    Check ($bad -eq 0) "the $(Count $vt['cards']) PCI devices: the cards are the ones the vectors say"
} catch {
    Write-Host "FAIL  the test stopped: $(Get-ErrorText $_)"
    Write-Host $_.ScriptStackTrace
    $script:fails++
} finally {
    if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
}
Write-Host "== $($script:count) vectors, $($script:fails) failure(s)"
exit $script:fails
