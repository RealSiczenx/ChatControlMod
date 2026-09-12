# Todo Plugin 1.1 — native OBS checklist

A native C++/Qt dock with manual tasks, 31 automatic conditions, selectable Yes/No
requirements, and customizable appearance. Runs offline inside OBS with no WebSocket.

## Install or update

Download the Windows x64 artifact from this repository's latest successful Actions
run. Close OBS, extract it, and copy `package/stream-checklist` into
`C:\ProgramData\obs-studio\plugins`. Replace the previous plugin files.
Open OBS > Docks > Todo Plugin. Existing native tasks load automatically.

## Add an automatic task

Click + Add Task > Automatic. Choose a check and its target. For boolean checks,
choose Yes / ON or No / OFF. For named matches, Yes requires that name and No
requires a different name. Numeric checks offer at least, at most, or equals
(with a 0.005 tolerance); No inverts the comparison. Add several tasks to require
several conditions. Unknown checks never count as complete.

Supported checks:

- Audio: muted, unmuted, fader percentage, sync offset, monitoring mode, track assignment.
- Scenes: Program scene, preview scene, direct scene-item visibility and lock.
- Sources: active in an output, showing in any view, enabled, existence.
- Filters: enabled state for a named filter on a chosen source.
- Media: playing, paused, ended for controllable media sources.
- Outputs: streaming, recording active, recording running and not paused, recording
  paused, replay buffer, virtual camera.
- Workspace: Studio Mode, main preview enabled, profile, scene collection.
- Video: configured FPS, output width, output height.

Source and scene pickers use the current OBS session. Scene items display a name
and ID to distinguish duplicate sources. Visibility/lock checks target direct items
in the selected scene, not items inside groups. A missing target, missing filter,
unsupported media source, or unavailable preview returns Unknown even for No/OFF.
The explicit Source exists check can pass for an absent source when set to No/OFF.

These checks inspect OBS state, not your stream's actual picture/sound quality.
Audio volume checks read the fader, not a sound meter. Active/shown sources may still
have blank or incorrect content. FPS is configured FPS, not measured performance.
The plugin cannot automatically verify arbitrary real-world tasks or block streaming.

## Customize

Use Customize to select an accent color (or follow OBS), text size (9–18 pt), row
spacing (34–60 px), and refresh interval (1–10 seconds). Select a task and use the
up/down arrow buttons to reorder it. Layout and colors follow the OBS theme by default.

Tasks, order, conditions, and appearance save together in the plugin's `tasks.json`.
New stream clears manual ticks. Export/import backups include appearance. Existing
version-1 task files migrate on save; new version-2 backups require this update.
Keep a backup before rolling back to an older plugin version.

## Build and verification

The GitHub Actions workflow uses a pinned official OBS plugin template and its
OBS 31.1.1 / Qt development dependencies. It compiles the Windows x64 DLL and runs
status/threshold tests plus Qt model tests covering legacy migration, all rule
round trips, OFF conditions, appearance, ordering and invalid-data handling.
Actual UI rendering and live OBS integration still require checking in OBS.

For a local Windows developer build, use Build-Windows.ps1 with matching OBS and Qt
SDK prefixes. The regular OBS installer is not a development SDK. Qt runtime DLLs
must be on PATH for the model tests. Do not copy a different Qt runtime into OBS.

## Sources and license

- OBS frontend API: https://docs.obsproject.com/reference-frontend-api
- OBS source API: https://docs.obsproject.com/reference-sources
- OBS scene API: https://docs.obsproject.com/reference-scenes

GPL-2.0-or-later; see LICENSE.txt.
