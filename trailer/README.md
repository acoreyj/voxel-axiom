# Voxel Engine v4.0 Trailer Pipeline

This directory contains the deterministic in-engine capture rig for the 60s and
75s tech-demo cuts.

## Requirements

- Godot 4.7.2 with the built GDExtension in `bin/`
- Blender 5.2 LTS at `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe`
- PowerShell 5.1
- A release `VoxelTerrain3D` library that includes `save_directory_path`,
  `undo()`, `redo()`, `undo_all()`, and `clear_undo_history()`

## Capture

```powershell
# Quick low-resolution proof of every shot
.\tools\previs_trailer.ps1 -All

# Capture specific shots at 4K
.\tools\capture_trailer.ps1 -Shots play_soil,play_hardrock,play_statue_recon

# Capture every shot at 4K
.\tools\capture_trailer.ps1 -All
```

The 4K profile is applied by `project.godot` through the `movie` feature tag.
The previs wrapper temporarily copies `trailer/previs_override.cfg` to
`override.cfg` and restores any existing override afterwards.

Every take:

1. Calls `undo_all()` and clears undo history.
2. Resets the demo statues.
3. Builds only the set it needs.
4. Hides the HUD, reticle, cursor, and all 3D statue labels.
5. Waits for streaming, commit, and optional navigation sync.
6. Writes `marker.txt` and flashes a 16×16 magenta capture marker.
7. Plays the timed shot action list and quits.

The marker frame is copied into each `captures/raw/<shot>/marker.txt`; the edit
script uses it to skip setup frames and the marker flash.

## Sets

- `material_bay.gd` builds the five play-mode material pillars from real
  `add_terrain_with_material()` fills.
- `cave_set.gd` builds a deterministic ridge and carves a real CSG tunnel for
  the cave and 3D-navigation shots. Copy for those shots says
  `CAVES · REAL-TIME CSG`; replace the set with a natural procedural cave if a
  stronger location is scouted.

## Edit

```powershell
# 1080p H.264 review cut
.\tools\render_deliverables.ps1 -Timeline trailer/edit_60.json -Preview

# 4K ProRes master
.\tools\render_deliverables.ps1 -Timeline trailer/edit_60.json -Master

# Vertical preview
.\tools\render_deliverables.ps1 -Timeline trailer/edit_60.json -Vertical

# All timelines: preview, master, and vertical
.\tools\render_deliverables.ps1 -All
```

Outputs are written to `captures/edit/`.

The Blender editor uses:

- captured PNG sequences as image strips
- `trailer/assets/benchmark_chart.png` for the benchmark beat
- native Blender text strips for lower-thirds and the outro
- Standard color management to avoid double tone-mapping the game footage
- no audio track in this pass

## Benchmark Card

```powershell
.\tools\make_benchmark_chart.ps1 -RequireComplete
```

The chart generator refuses an aborted or incomplete run. The current
`voxel_benchmark_report.md` may be incomplete; rerun the full five-preset suite
before generating the final chart.

## Shot Notes

- `play_*` shots are first-person pickaxe shots. The hard-rock and boulder
  sequences rely on persistent 3-hit / 5-hit damage. Every play shot shows the
  trailer target dot.
- `react_lava_water` is the focused material-change beat: the LAVA tool is
  selected, lava is added, the tool switches to WATER, and the result is
  labelled OBSIDIAN. The in-game tool panel and target dot are captured, not
  added in post.
- `play_statue_swap` is captured at 120 fps and edited at 0.5× speed.
- `play_statue_recon` captures the 15s quiet-period reconstitution and is
  time-compressed to 4s in the cut.
- `fly_persist_a` and `fly_persist_b` share `trailer_saves/persist_pair`.
  The capture wrapper clears it before A and preserves it for B.
- `fly_undo_redo` carves a crater, calls native `undo()`, then `redo()`.
- `debug_xray` adds `--debug-collisions --debug-navigation` for one technical
  insert.

## Current Status`n`nThe 120s cut is the slower, clearer version: fewer cuts, longer holds, centred`ncallouts, and a dedicated underground-cave tunnel shot. The 60s and 75s cuts use`nthe same captures with tighter pacing.

- Previs captures exist under `captures/previs/`.
- `captures/edit/edit_60_preview.mp4`, `captures/edit/edit_75_preview.mp4`, and `captures/edit/edit_120_preview.mp4`
  are rendered 1080p review cuts.
- `trailer/assets/benchmark_chart.png` is generated from a completed
  five-preset run.
- The 4K master has not been rendered yet. Run the 4K capture pass first:

```powershell
.\tools\capture_trailer.ps1 -All -GodotExe "<godot console exe>"
.\tools\render_deliverables.ps1 -Timeline trailer/edit_60.json -Captures captures/raw -Master
.\tools\render_deliverables.ps1 -Timeline trailer/edit_75.json -Captures captures/raw -Master
```

The capture wrapper trims every frame before the magenta marker, so each
`captures/raw/<shot>/` folder contains only the shot itself.

## Cave Scout

`tools/scout_caves.gd` scans the pristine world for deep surface depressions.
Run it with an isolated save directory:

```powershell
godot --headless --path . --script res://tools/scout_caves.gd -- --trailer-save-dir="<abs>/trailer_saves/scout"
```

The current trailer set uses a carved slot canyon (`cave_set.gd`) rather than a
natural cave because the first scout pass found mostly water pools. Replace the
canyon set with a natural location if a better cave is scouted.

## Intentional Omissions

- No generated video or synthetic gameplay.
- No music, SFX, voiceover, or CTA in this pass.
- No Resolve dependency; Blender VSE is the editor of record.
