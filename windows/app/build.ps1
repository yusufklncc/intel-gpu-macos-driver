# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
build.ps1: builds IntelXe Manager for Windows. Restores, builds, runs the Core tests, and publishes the app as a
self-contained folder (no .NET or Windows App Runtime to install) plus a zip of it.

.DESCRIPTION
  .\build.ps1                    restore, build, test, publish to out\IntelXeManager-VERSION-win-x64 and zip it
  .\build.ps1 -SkipTests         the same without the tests
  .\build.ps1 -Configuration Debug

Needs the .NET 10 SDK on Windows (the WinUI XAML compiler runs only there). On macOS or Linux only the Core and its
tests build: dotnet test IntelXe.Core.Tests
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$SkipTests,
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
if (Test-Path -LiteralPath $zip) { Remove-Item -Force -LiteralPath $zip }
Compress-Archive -Path $publish -DestinationPath $zip -CompressionLevel Optimal

$size = (Get-ChildItem -LiteralPath $publish -Recurse -File | Measure-Object -Sum Length).Sum
Write-Host ("== done: {0} ({1:N0} MB), {2} ({3:N0} MB)" -f $publish, ($size / 1MB), $zip, ((Get-Item -LiteralPath $zip).Length / 1MB))
