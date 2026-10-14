# Running the Production (Release) Demo

This is the end-to-end recipe for producing and running an **optimized release
build** of `voxel_terrain_demo`. A working release has two independent layers,
and both must be release:

1. **Native GDExtension library** — the voxel engine compiled `/O2` and linked
   against godot-cpp's **release** template.
2. **Exported project** — the `.tscn`/`.gd`/shader/PCK bundled by a release
   export preset, which loads the release library.

Running the project from the editor (F5) is *not* this. That is a debug-context
run; it only loads a release library if the `.gdextension` mapping points the
`debug` feature tag at the release artifact (which this repo does — see
[Which library Godot loads](#which-library-godot-loads)).

---

## Prerequisites

| Requirement | Notes |
| --- | --- |
| Godot **4.7.x** editor | Must match the project's `config/features` (`4.7`) and the export templates. |
| Export templates for that exact version | Editor → **Manage Export Templates… → Download and Install**, or install a `*.tpz` from file. |
| CMake ≥ 3.22 on `PATH` | `cmake --version` should succeed in a fresh shell. |
| MSVC 2022 (VS 17 generator) | Located automatically by CMake. |
| `godot-cpp` checkout matching the engine | `Terrain3D/godot-cpp` in this workspace. |

> **CMake on PATH (Windows).** CMake usually lives at
> `%LOCALAPPDATA%\CMake\bin`. If a terminal reports `cmake is not recognized`,
> the host process (Windows Terminal / VS Code) cached its environment before
> CMake was installed. A new tab that starts PowerShell re-reads the profile,
> which prepends the CMake directory; fully restarting the terminal host also
> works. To fix a single session immediately:
>
> ```powershell
> $env:Path += ';C:\Users\kuroi\AppData\Local\CMake\bin'
> ```

---

## Step 1 — Build the release-slot native library

The artifact filename follows the CMake build configuration, so `--config
Release` refreshes the **release** slot file. Use a dedicated directory that
links godot-cpp's release template:

```powershell
cmake -B build-release `
  -DVOXEL_BUILD_GDEXTENSION=ON `
  -DGODOTCPP_TARGET=template_release `
  -DGODOT_CPP_DIR=C:/Users/kuroi/workspace/Terrain3D/godot-cpp `
  -DVOXEL_BUILD_TESTS=OFF

cmake --build build-release --config Release
```

Output is copied into `bin/`:

```
bin/libvoxel_engine.windows.release.x86_64.dll
```

Sanity check:

```powershell
Get-ChildItem bin -Filter *.dll | Select-Object Name, Length, LastWriteTime
```

The timestamp on the `…release…` file must be from this build. `GODOTCPP_TARGET`
selects the godot-cpp template linked in; `template_release` is what makes this
a genuine release extension rather than a release-compiled debug one.

---

## Step 2 — Install the export templates

There is no CLI to fetch them. Open the project in the 4.7 editor and use
**Editor → Manage Export Templates… → Download and Install**. Verify they
landed:

```
%APPDATA%\Godot\export_templates\4.7.2.stable\windows_release_x86_64.exe
%APPDATA%\Godot\export_templates\4.7.2.stable\windows_debug_x86_64.exe
```

(Only Android templates ship in some setups — if `windows_*` is missing, the
export will fail with *"No export template found…"*.)

---

## Step 3 — Export the project (release)

`export_presets.cfg` already contains a **Windows Desktop** preset.

**Editor:** Project → Export… → **Windows Desktop** → **Export Project** with
**Debug unchecked** (debug symbols are fine to keep checked; the build is still
release).

**Headless CLI:**

```powershell
& "C:\Users\kuroi\bin\godot-4.7.2\Godot_v4.7.2-stable_win64_console.exe" `
  --headless --path "C:\Users\kuroi\workspace\voxel" `
  --export-release "Windows Desktop" `
  "C:\Users\kuroi\workspace\voxel\build\voxel_terrain_demo.exe"
```

Write the executable outside `res://` (here, under the ignored `build/` tree) so
Godot does not re-import it.

---

## Step 4 — Run and verify

Launch `build/voxel_terrain_demo.exe`. At startup the engine must load
`bin/libvoxel_engine.windows.release.x86_64.dll`; a missing/mismatched library
fails the GDExtension init (`voxel_engine_init`).

The exported executable is the only true "release run": the editor's run button
uses the `debug` feature tag. Confirm the correct library by the `release` DLL
timestamp (Step 1) rather than by launching from the editor.

---

## Which library Godot loads

`voxel_engine.gdextension` maps both feature tags to the release DLL:

```ini
windows.debug.x86_64   = "res://bin/libvoxel_engine.windows.release.x86_64.dll"
windows.release.x86_64 = "res://bin/libvoxel_engine.windows.release.x86_64.dll"
```

So the editor/debug slot and the exported release game both use the optimized
binary. If you later want a true debug artifact in the debug slot, build with
`--config Debug` (writes `…debug…dll`) and point `windows.debug.x86_64` back at
it.

---

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `cmake is not recognized` | Host cached env before CMake was added — new shell / restart host, or the `$env:Path += …` line above. |
| `No export template found at …/windows_release_x86_64.exe` | Step 2 not done for this Godot version. |
| `Cannot export project with preset …` | Preset name must match `export_presets.cfg` (`"Windows Desktop"`). |
| GDExtension fails to initialize | Wrong engine version, or the DLL in `bin/` is stale — rerun Step 1 and confirm the timestamp. |
| Editor opens the project with import errors | Use the **4.7.2** editor, not the 4.6.x build in `Downloads`. |
