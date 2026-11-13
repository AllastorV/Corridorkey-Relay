param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$bundle = Join-Path $root "build\CorridorKey2.ofx.bundle"
if (-not (Test-Path "$bundle\Contents\Win64\CorridorKey2.ofx")) { throw "Run build.bat first." }
New-Item -ItemType Directory -Force (Join-Path $root "build\dist") | Out-Null
$zip = Join-Path $root "build\dist\CorridorKey-Relay-$Version-Windows.zip"
$items = @($bundle, "$PSScriptRoot\install_plugin.bat", "$PSScriptRoot\install_runtime.ps1",
           "$root\README.md", "$root\LICENSE", "$root\THIRD_PARTY.md")
Compress-Archive -Path $items -DestinationPath $zip -Force
Get-FileHash -Algorithm SHA256 $zip | Format-List
Write-Host "Release archive: $zip"
