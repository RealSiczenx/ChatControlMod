# Build from a Windows OBS/Qt development environment; does not download dependencies.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ObsSdkPrefix,
    [Parameter(Mandatory=$true)][string]$QtPrefix
)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'Install CMake 3.28+ and add it to PATH.' }
if (-not (Test-Path -LiteralPath $ObsSdkPrefix)) { throw 'OBS SDK prefix does not exist.' }
if (-not (Test-Path -LiteralPath $QtPrefix)) { throw 'Qt prefix does not exist.' }
$build = Join-Path $PSScriptRoot 'build-windows'
$package = Join-Path $PSScriptRoot 'package'
& cmake -S $PSScriptRoot -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$ObsSdkPrefix;$QtPrefix" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed. See README for required SDK exports.' }
& cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
& cmake --install $build --config Release --prefix $package
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
$dll = Join-Path $package 'stream-checklist\bin\64bit\stream-checklist.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw 'Expected DLL missing.' }
Write-Host "Build complete: $dll"
Write-Host 'Close OBS, then run Install-Windows.ps1 as administrator.'
