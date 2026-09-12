$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$staging = Join-Path $root '_obs-template'
$release = Join-Path $root 'release'
if (Test-Path $staging) { throw 'Build staging folder already exists; start from a clean checkout.' }
& git clone https://github.com/obsproject/obs-plugintemplate.git $staging
if ($LASTEXITCODE -ne 0) { throw 'Could not fetch official OBS template.' }
& git -C $staging checkout 3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc
if ($LASTEXITCODE -ne 0) { throw 'Could not check out the pinned template revision.' }
$specPath = Join-Path $staging 'buildspec.json'
$spec = Get-Content -Raw $specPath | ConvertFrom-Json
$spec.name = 'stream-checklist'
$spec.displayName = 'Todo Plugin'
$spec.version = '1.1.0'
$spec.author = 'Todo Plugin contributors'
$spec.website = 'https://obsproject.com'
$spec.email = ''
$spec | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 $specPath
Copy-Item -Path (Join-Path $root 'src/*') -Destination (Join-Path $staging 'src') -Force
Copy-Item -LiteralPath (Join-Path $root 'tests') -Destination $staging -Recurse
$cmake = @'
cmake_minimum_required(VERSION 3.28...3.30)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version} LANGUAGES C CXX)
option(ENABLE_FRONTEND_API "Use OBS frontend API" ON)
option(ENABLE_QT "Use Qt" ON)
include(compilerconfig)
include(defaults)
include(helpers)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
add_library(${CMAKE_PROJECT_NAME} MODULE src/plugin.cpp)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
include(CTest)
add_executable(checklist-tests tests/checks.cpp)
target_include_directories(checklist-tests PRIVATE src)
target_compile_features(checklist-tests PRIVATE cxx_std_17)
add_test(NAME checklist-status COMMAND checklist-tests)
add_executable(checklist-model-tests tests/model.cpp)
target_include_directories(checklist-model-tests PRIVATE src)
target_compile_features(checklist-model-tests PRIVATE cxx_std_17)
target_link_libraries(checklist-model-tests PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME checklist-model COMMAND checklist-model-tests)
'@
Set-Content -Encoding utf8 -LiteralPath (Join-Path $staging 'CMakeLists.txt') -Value $cmake
Push-Location $staging
try {
    & cmake --preset windows-x64 -DENABLE_FRONTEND_API=ON -DENABLE_QT=ON
    if ($LASTEXITCODE -ne 0) { throw 'OBS/Qt configuration failed.' }
    & cmake --build --preset windows-x64 --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Plugin compilation failed.' }
    $qtDll = Get-ChildItem -Path (Join-Path $staging '.deps') -Filter Qt6Core.dll -Recurse | Select-Object -First 1
    if (-not $qtDll) { throw 'Qt runtime for model tests was not found.' }
    $env:PATH = $qtDll.DirectoryName + ';' + $env:PATH
    & ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Status tests failed.' }
    & cmake --install build_x64 --config RelWithDebInfo --prefix (Join-Path $release 'package')
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
} finally { Pop-Location }
$dll = Join-Path $release 'package/stream-checklist/bin/64bit/stream-checklist.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw 'Compiled DLL is missing.' }
Copy-Item -LiteralPath (Join-Path $root 'Install-Windows.ps1') -Destination $release
Copy-Item -LiteralPath (Join-Path $root 'LICENSE.txt') -Destination $release
@'
TODO PLUGIN - native OBS plugin for Windows x64

1. Extract this entire download.
2. Close OBS.
3. Open C:\ProgramData\obs-studio\plugins (create the plugins folder if needed).
4. Copy the stream-checklist folder from inside package into that plugins folder.
5. Start OBS. Open Docks > Todo Plugin.
6. Click + Add Task and select Manual or Automatic.

No WebSocket or Internet connection is needed to use the plugin.
This package was compiled in CI; live OBS operation still needs verification.
Uses the OBS 31.1.1 development baseline from the pinned official plugin template.
Use Windows x64 OBS, and report the OBS version and log if loading fails.

Version 1.1 adds 31 automatic checks, Yes/No conditions, numeric comparisons,
Customize (accent color, text size, row spacing, refresh interval) and Up/Down task
ordering. Existing tasks load automatically; new backups include your appearance.

Automatic checks reflect current OBS state. Audio mute checks do not prove sound
is present. New stream clears manual ticks. The plugin does not block streaming.
To uninstall: close OBS and remove only its stream-checklist plugin folder.
'@ | Set-Content -Encoding utf8 (Join-Path $release 'INSTALL.txt')
Get-FileHash -Algorithm SHA256 $dll | Format-List | Out-String | Set-Content (Join-Path $release 'DLL-SHA256.txt')
Write-Host "Windows DLL built: $dll"
