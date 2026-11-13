# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
build.ps1: builds IntelXe Manager for Windows. Restores, builds, runs the Core tests, and publishes the app as a
self-contained folder (no .NET or Windows App Runtime to install) plus a zip of it.

.DESCRIPTION
  .\build.ps1                    restore, build, test, publish to out\IntelXeManager-VERSION-win-x64, zip it and
                                 write the zip's SHA-256 beside it (.zip.sha256, for release/package.sh)
  .\build.ps1 -SkipTests         the same without the tests
  .\build.ps1 -SkipEngine        without the EFI builder beside the app (engine\: the Build the EFI page then says so)
  .\build.ps1 -Configuration Debug

The EFI builder (engine\ in the published folder, docs/design/DESIGN-efi-build.md section 5): python.org's embeddable
Python and the wheels pinned in engine.lock.json (SHA-256), windows/efibuild, and OpCore Simplify and Hardware Sniffer
at the commits of windows/efibuild/upstream/PINNED.json, every file checked against their manifests. The downloads are
kept in OUT\downloads\. Nothing downloaded is run here; engine\MANIFEST.sha256 lists every file for the app's own check.

Needs the .NET 10 SDK on Windows (the WinUI XAML compiler runs only there). On macOS or Linux only the Core and its
tests build: dotnet test IntelXe.Core.Tests
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$SkipTests,
    [switch]$SkipEngine,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$version = (Get-Content -Raw (Join-Path $repo 'VERSION')).Trim()
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
$name = "IntelXeManager-$version-win-x64"
$publish = Join-Path $OutDir $name
$zip = Join-Path $OutDir "$name.zip"

function Invoke-Step([string]$title, [scriptblock]$block) {
    Write-Host "== $title"
    & $block
    if ($LASTEXITCODE -ne 0) { throw "$title failed (exit code $LASTEXITCODE)" }
}

$app = Join-Path $here 'IntelXe.App\IntelXe.App.csproj'
$tests = Join-Path $here 'IntelXe.Core.Tests\IntelXe.Core.Tests.csproj'

Invoke-Step 'restore' { dotnet restore $app -r win-x64 -p:Platform=x64; if ($LASTEXITCODE -eq 0) { dotnet restore $tests } }
Invoke-Step 'build' { dotnet build $app -c $Configuration -r win-x64 -p:Platform=x64 --no-restore }
if (-not $SkipTests) {
    Invoke-Step 'test' { dotnet test $tests -c $Configuration --no-restore }
}
if (Test-Path -LiteralPath $publish) { Remove-Item -Recurse -Force -LiteralPath $publish }
Invoke-Step 'publish' { dotnet publish $app -c $Configuration -r win-x64 -p:Platform=x64 --self-contained --no-restore -o $publish }

# the PDBs stay in the build folder; the folder a user unpacks holds what runs
Get-ChildItem -LiteralPath $publish -Recurse -Filter '*.pdb' | Remove-Item -Force

# ---- the EFI builder beside the app ------------------------------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-Sha256([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }

# A pinned download, kept in OUT\downloads under its name; used only when its bytes have the pinned SHA-256.
function Get-Pinned([string]$url, [string]$sha256, [string]$name) {
    $dir = Join-Path $OutDir 'downloads'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $file = Join-Path $dir $name
    # an upstream source archive has no pinned bytes (its contents are checked): a kept one is used as it is
    if ((Test-Path -LiteralPath $file) -and (-not $sha256 -or (Get-Sha256 $file) -eq $sha256)) { return $file }
    if (-not $url.StartsWith('https://')) { throw "only https downloads: $url" }
    Write-Host "   download $url"
    $tmp = "$file.part"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.ServicePointManager]::SecurityProtocol
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $tmp
    $got = Get-Sha256 $tmp
    if ($sha256 -and $got -ne $sha256) { Remove-Item -Force -LiteralPath $tmp; throw "${url}: SHA-256 $got is not the pinned $sha256" }
    Move-Item -Force -LiteralPath $tmp -Destination $file
    return $file
}

# "sha256  relative/path" per file, sorted by path (ordinal), LF: the format of windows/efibuild/upstream/*.sha256
# (upstream.py manifest_text, which skips __pycache__ folders) and of engine\MANIFEST.sha256 (EngineLayout).
function Get-ManifestText([string]$root, [string]$exclude = '') {
    $root = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\')
    $rels = New-Object System.Collections.Generic.List[string]
    $lines = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
    foreach ($f in Get-ChildItem -LiteralPath $root -Recurse -File -Force) {
        $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
        if ($rel -eq $exclude -or ('/' + $rel) -match '/__pycache__/') { continue }
        $rels.Add($rel)
        $lines[$rel] = '{0}  {1}' -f (Get-Sha256 $f.FullName), $rel
    }
    $sorted = $rels.ToArray()
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    return (($sorted | ForEach-Object { $lines[$_] + "`n" }) -join '')
}

function Expand-Zip([string]$zip, [string]$dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $dest)
}

function Add-Engine([string]$engine) {
    $efibuild = Join-Path $repo 'windows\efibuild'
    $lock = Get-Content -Raw -LiteralPath (Join-Path $here 'engine.lock.json') | ConvertFrom-Json
    if ($lock.format -ne 'intelxe-winapp-engine' -or $lock.format_version -ne 1) { throw 'engine.lock.json: unknown format' }
    if (Test-Path -LiteralPath $engine) { Remove-Item -Recurse -Force -LiteralPath $engine }

    # python.org's embeddable Python; its ._pth names the search path (and turns site on for the wheels' .pth files)
    $py = Join-Path $engine 'python'
    Expand-Zip (Get-Pinned $lock.python.url $lock.python.sha256 ([IO.Path]::GetFileName($lock.python.url))) $py
    $pth = @(Get-ChildItem -LiteralPath $py -Filter 'python3*._pth')
    if ($pth.Count -ne 1) { throw 'the embeddable Python has no single python3*._pth' }
    [IO.File]::WriteAllText($pth[0].FullName, (($lock.python.path -join "`r`n") + "`r`n"))

    # the wheels, unpacked as pip would into Lib\site-packages (their .data folders and pywin32's IDE and help left out)
    $site = Join-Path $py 'Lib\site-packages'
    foreach ($w in $lock.wheels) {
        Expand-Zip (Get-Pinned $w.url $w.sha256 ([IO.Path]::GetFileName($w.url))) $site
    }
    foreach ($r in $lock.remove) {
        $p = Join-Path $py $r
        if (Test-Path -LiteralPath $p) { Remove-Item -Recurse -Force -LiteralPath $p }
    }

    # windows/efibuild itself (not its tests)
    $eb = Join-Path $engine 'efibuild'
    foreach ($pattern in $lock.efibuild) {
        foreach ($f in @(Get-ChildItem -Path (Join-Path $efibuild $pattern) -File)) {
            $rel = $f.FullName.Substring($efibuild.Length + 1)
            $dst = Join-Path $eb $rel
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
            Copy-Item -LiteralPath $f.FullName -Destination $dst
        }
    }
    # the rules the engine reads (rules.py looks beside the package first): the published app has no release\ folder
    Copy-Item -LiteralPath (Join-Path $repo 'release\templates\efi-rules.json') -Destination (Join-Path $eb 'efi-rules.json')

    # OpCore Simplify and Hardware Sniffer at their pinned commits: the source archive's files must be exactly the
    # manifest's (GitHub does not promise stable archive bytes, so the contents are what is pinned)
    $pinned = Get-Content -Raw -LiteralPath (Join-Path $efibuild 'upstream\PINNED.json') | ConvertFrom-Json
    foreach ($prop in $pinned.projects.PSObject.Properties) {
        $name = $prop.Name; $proj = $prop.Value
        $manifest = Join-Path $efibuild ('upstream\' + $proj.manifest)
        if ((Get-Sha256 $manifest) -ne $proj.manifest_sha256) { throw "$($proj.manifest) does not match manifest_sha256 in PINNED.json" }
        $archive = Get-Pinned $proj.archive '' "$name-$($proj.commit).tar.gz"
        $tmp = Join-Path $OutDir "upstream-$name"
        if (Test-Path -LiteralPath $tmp) { Remove-Item -Recurse -Force -LiteralPath $tmp }
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        & (Join-Path $env:SystemRoot 'System32\tar.exe') -xzf $archive -C $tmp
        if ($LASTEXITCODE -ne 0) { throw "could not unpack $archive" }
        $top = @(Get-ChildItem -LiteralPath $tmp -Directory)
        if ($top.Count -ne 1) { throw "$archive does not hold one folder" }
        $want = [IO.File]::ReadAllText($manifest)
        if ((Get-ManifestText $top[0].FullName) -ne $want) {
            Remove-Item -Force -LiteralPath $archive
            throw "$name at $($proj.commit) differs from its pinned manifest (the archive was deleted; run again to download it again)"
        }
        Move-Item -LiteralPath $top[0].FullName -Destination (Join-Path $eb "upstream\$name-$($proj.commit)")
        Remove-Item -Recurse -Force -LiteralPath $tmp
    }

    Copy-Item -LiteralPath (Join-Path $here 'engine.lock.json') -Destination (Join-Path $engine 'engine.lock.json')
    # every file, for the app's check before each scan and build (Core/EfiBuild/EngineLayout.cs)
    [IO.File]::WriteAllText((Join-Path $engine 'MANIFEST.sha256'), (Get-ManifestText $engine 'MANIFEST.sha256'), (New-Object Text.UTF8Encoding($false)))
    $engineSize = (Get-ChildItem -LiteralPath $engine -Recurse -File | Measure-Object -Sum Length).Sum
    Write-Host ("== engine: {0} ({1:N0} MB, {2} files)" -f $engine, ($engineSize / 1MB), @(Get-ChildItem -LiteralPath $engine -Recurse -File).Count)
}

if (-not $SkipEngine) { Invoke-Step 'engine' { Add-Engine (Join-Path $publish 'engine'); $global:LASTEXITCODE = 0 } }
if (Test-Path -LiteralPath $zip) { Remove-Item -Force -LiteralPath $zip }
Compress-Archive -Path $publish -DestinationPath $zip -CompressionLevel Optimal
# its SHA-256 beside it, as shasum writes it: release/package.sh (WINAPP_ZIP) checks the zip it is given against it
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n")

$size = (Get-ChildItem -LiteralPath $publish -Recurse -File | Measure-Object -Sum Length).Sum
Write-Host ("== done: {0} ({1:N0} MB), {2} ({3:N0} MB), SHA-256 {4}" -f $publish, ($size / 1MB), $zip, ((Get-Item -LiteralPath $zip).Length / 1MB), $hash)
