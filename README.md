# Todo Plugin — native OBS plugin source

**Status: source implementation, not an installable release. No Windows DLL is included.**
The status decision tests passed with a Linux C++ compiler. The full OBS/Qt module
has not been compiled, loaded into OBS, or visually tested. Windows compilation and
live OBS validation are required before treating this as a working release.

This implements a C++/Qt dock using OBS's own API. It does not use a browser,
WebSocket, server, password, Internet connection, Python, or JavaScript at runtime.

## Features implemented

- Docks > Todo Plugin, with a native dock you can move and resize.
- + Add Task: Manual (type a task and tick it yourself) or Automatic (choose an OBS check).
- Automatic checks: audio muted/unmuted, Program scene, recording running and not
  paused, replay buffer running, virtual camera running, Studio Mode, source active.
- Editable OBS source/scene dropdowns populated from the current OBS session.
- Done / Not done / Unknown status; updates every second.
- Select a row, then Edit or Delete. New stream resets manual ticks.
- Local JSON persistence with atomic saves and backup export/import.
- Unreadable settings are preserved; importing a valid backup first copies the
  original file aside. Saving failures remain visible in the dock.

Automatic tasks must use a supported check; arbitrary text is handled as a manual
task. Mute checks do not verify sound, hardware, or audio routing. Source active
means OBS uses it in an output, not that the picture is correct. The Program scene
check is separate from Studio Mode preview. The checklist does not block streaming.

## Build on Windows (developer setup required)

Target: Windows x64, OBS 30+ dock API, Qt 6, MSVC. For the latest OBS version, use
matching OBS development files and the Qt/MSVC versions used by that OBS build.
Do not mix MinGW Qt with MSVC or copy a different Qt runtime into OBS.

Required:

1. Visual Studio 2022 C++ desktop build tools and Windows SDK.
2. CMake 3.28+ available on PATH.
3. An OBS development SDK/install prefix with headers, import libraries and CMake
   packages exporting `OBS::libobs` and `OBS::obs-frontend-api`. It must include
   `libobsConfig.cmake` and `obs-frontend-apiConfig.cmake` (case may vary).
   The regular OBS installer is not a development SDK. Build/export these from
   matching OBS sources or use an appropriate OBS development package.
4. The matching Qt 6 MSVC x64 development prefix, including Qt Widgets.

Open PowerShell in this folder and run (replace the example paths):

```powershell
.\Build-Windows.ps1 -ObsSdkPrefix 'C:\dev\obs-sdk' -QtPrefix 'C:\dev\qt-msvc-x64'
```

The script configures the project, builds Release, runs the status tests and creates:

```text
package/stream-checklist/bin/64bit/stream-checklist.dll
package/stream-checklist/data/README.md
```

The script does not fetch dependencies or change PowerShell execution policy. If
execution is restricted, use your approved developer environment or run the CMake
commands below directly. If CMake cannot find the packages, correct the SDK prefixes;
pointing it to the ordinary OBS program folder will not fix missing development files.

```powershell
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=C:\dev\obs-sdk;C:\dev\qt-msvc-x64"
cmake --build build-windows --config Release --parallel
ctest --test-dir build-windows -C Release --output-on-failure
cmake --install build-windows --config Release --prefix package
```

## Install after building

Close OBS. Run `Install-Windows.ps1` from an administrator PowerShell, or copy the
built `package/stream-checklist` folder into `%ProgramData%\obs-studio\plugins`.
The installer refuses to proceed without a built DLL or while OBS is running.
Then open OBS > Docks > Todo Plugin.

No installation action has been performed on your computer by creating this archive.
To uninstall, close OBS and remove only the `stream-checklist` plugin folder.
Settings are saved using `obs_module_config_path("tasks.json")`, normally beneath
`%APPDATA%\obs-studio\plugin_config\stream-checklist\tasks.json`; portable OBS uses
its own configuration root. Uninstallation leaves these settings intact.
The task list is global to this OBS configuration, not per scene collection.

## Windows acceptance checks still required

- Build the complete module and confirm OBS loads it without errors.
- Add a manual task, tick it, restart OBS, and verify its saved state.
- Add an automatic audio check; mute/unmute the chosen input and verify updates.
- Change a Program scene and rename/delete a referenced source; verify failed or
  unknown checks never count as done.
- Test recording stopped, running, and paused; replay buffer and virtual camera.
- Export/import; verify New stream only clears manual progress.
- Dock/undock, resize, switch scene collections, close/reopen the dock, exit OBS.

## Status logic tests without OBS/Qt

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/checks.cpp -o checklist-tests
./checklist-tests
```

Or configure with `-DBUILD_PLUGIN=OFF` and use CTest.

## References

- OBS frontend/dock API: https://docs.obsproject.com/reference-frontend-api
- OBS source API: https://docs.obsproject.com/reference-sources
- OBS build instructions: https://github.com/obsproject/obs-studio/wiki/Install-Instructions

Source is provided under GPL-2.0-or-later, without warranty. See LICENSE.txt.
