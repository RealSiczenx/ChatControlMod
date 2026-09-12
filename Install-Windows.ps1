# Run only after a successful Windows build. Existing plugin is backed up.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before installing.' }
$source = Join-Path $PSScriptRoot 'package\stream-checklist'
if (-not (Test-Path -LiteralPath (Join-Path $source 'bin\64bit\stream-checklist.dll'))) {
    throw 'No compiled DLL found. This source package must be built on Windows first. Read README.md.'
}
$dest = Join-Path $env:ProgramData 'obs-studio\plugins\stream-checklist'
if (Test-Path -LiteralPath $dest) {
    $backup = Join-Path $env:TEMP ('stream-checklist-backup-' + [Guid]::NewGuid().ToString('N'))
    Copy-Item -LiteralPath $dest -Destination $backup -Recurse
    Write-Host "Previous plugin backed up to $backup"
}
New-Item -ItemType Directory -Path $dest -Force | Out-Null
Copy-Item -Path (Join-Path $source '*') -Destination $dest -Recurse -Force
Write-Host 'Installed. Start OBS and open Docks > Todo Plugin.'
