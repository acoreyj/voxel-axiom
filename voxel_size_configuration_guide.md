# Voxel Size (`voxel_scale`) Usage Guide

`voxel_scale` sets the physical size of one voxel sample, in **meters per voxel**.
Chunks always store `32 × 32 × 32` samples; changing the scale changes how large
that grid is in the world, not how much memory it uses.

| Value | Meaning |
| :--- | :--- |
| `1.0` (default) | 1 voxel = 1 m; one chunk = 32 m |
| `0.5` | 2× finer; 1 voxel = 0.5 m; one chunk = 16 m |
| `0.25` | 4× finer; 1 voxel = 0.25 m; one chunk = 8 m |
| `2.0` | 2× coarser; 1 voxel = 2 m; one chunk = 64 m |

Supported range: **`0.125` to `4.0`**. Values outside it are clamped.

---

## Quick Start (Godot)

Set `voxel_scale` on the `VoxelTerrain3D` node **before it enters the scene tree**.

### From the Inspector

1. Select the `VoxelTerrain3D` node.
2. Set **Voxel Scale** (`voxel_scale`) to `0.5` (or another value).
3. Adjust `world_height_chunks`, `view_distance_chunks`, and
   `resident_memory_budget_mb` as described below.
4. Point `save_directory_path` at a **new, scale-specific folder**.
5. Save the scene and run.

### From GDScript

For a node created at runtime:

```gdscript
var terrain := VoxelTerrain3D.new()
terrain.voxel_scale = 0.5
terrain.world_height_chunks = 8
terrain.view_distance_chunks = 8
terrain.resident_memory_budget_mb = 1536
terrain.save_directory_path = "/tmp/voxel_world_saves_s050"
add_child(terrain)
```

For a scene that already contains a `VoxelTerrain3D`, set the property in the
Inspector before running. Setting it after the node is already in the tree logs
a warning and does nothing, because resident chunks and nav groups are already
built at the old resolution. Reload the scene to change scale.

---

## What to Adjust When You Change Scale

The chunk is still `32³` samples, so its world size is `32 × voxel_scale` meters.
Everything measured in chunks changes meaning in meters.

### 1. World height

`world_height_chunks` sets the number of vertical chunks. Total world height is:

```
world_height_meters = world_height_chunks × 32 × voxel_scale
```

The procedural surface sits around **68 m** (roughly 54–82 m with amplitude and
overhangs).

| `voxel_scale` | Chunk height | Chunks for ~96 m | Suggested `world_height_chunks` |
| :--- | :--- | :--- | :--- |
| `1.0` | 32 m | 3 | 4 (demo uses 6) |
| `0.5` | 16 m | 6 | 6–8 |
| `0.25` | 8 m | 12 | 12 (max) |
| `2.0` | 64 m | 2 | 2–3 |

### 2. View distance

`view_distance_chunks` is a radius in **chunks**. Physical coverage across is:

```
view_width_meters = (2 × view_distance_chunks + 1) × 32 × voxel_scale
```

| `voxel_scale` | Chunk width | `view_distance_chunks = 4` | `= 6` | `= 8` |
| :--- | :--- | :--- | :--- | :--- |
| `1.0` | 32 m | 288 m | 416 m | 544 m |
| `0.5` | 16 m | 144 m | 208 m | 272 m |
| `0.25` | 8 m | 72 m | 104 m | 136 m |

Keeping the same physical view distance at a finer scale requires more chunks in
every direction: halving `voxel_scale` quadruples the horizontal chunk count and
doubles the vertical chunk count, so a chunk budget of roughly **8×** the volume.
`prefetch_distance_chunks` and `collision_distance_chunks` are also chunk
radii; scale them alongside `view_distance_chunks` if you rely on them.

### 3. Memory budget

Per-chunk memory is unchanged (`32³` samples), but finer voxels need far more
chunks for the same physical volume. `resident_memory_budget_mb` is a hard cap;
when it is reached, the effective view distance is reduced automatically.

| `voxel_scale` | Suggested starting budget |
| :--- | :--- |
| `2.0` | 256–512 MB |
| `1.0` | 512 MB |
| `0.5` | 1024–2048 MB |
| `0.25` | 2048–4096 MB |

### 4. Save directory

Chunk indices are canonical `(cx, cy, cz)` coordinates, so worlds at different
scales must not share saves. Use a scale-tagged folder:

```gdscript
terrain.save_directory_path = "/tmp/voxel_world_saves_s050" # 0.5 m
```

or delete the old save directory before switching scale. The default is
`/tmp/voxel_world_saves`.

---

## Editing, Raycasts, and Navigation Use World Meters

You do **not** convert anything manually when calling the runtime API. All
public positions and radii are in world meters; the engine converts them to
voxel samples internally.

```gdscript
# All of these are world-space meters.
terrain.trigger_explosion(Vector3(16, 68, 16), 8.5)
terrain.trigger_elemental_sphere(Vector3(16, 68, 16), 8.5, 2) # Frost
terrain.add_terrain(Vector3(16, 68, 16), 6.0)
terrain.place_boulder(Vector3(16, 68, 16), 4.0)

var hit := terrain.raycast_terrain(from, to)          # world-space hit / INF
var material := terrain.raycast_terrain_material(from, to)
var ground_y := terrain.get_surface_height(x, z)      # world-space Y
terrain.set_stream_focus(camera_position, velocity)   # world-space
```

Signals `explosion_requested`, `explosion_completed`, `terrain_add_requested`,
and `terrain_add_completed` also report world-space positions and radii.

At `voxel_scale = 0.5`, an explosion of radius `6.0` m becomes a 12-sample
radius in grid space and carves the same physical crater at twice the
resolution. The TSDF narrow band stays 2 samples wide (1 m in world space at
0.5 m voxels).

---

## Baking and Stamping Assets

Offline `.vreg` assets are baked at a specific voxel size. **Re-bake assets for
the scale you will stamp them into.** An asset baked at `1.0 m` will appear
half-size and half-thickness in a `0.5 m` world.

### Bake with `voxel_baker`

```text
voxel_baker <model.obj> <output.vreg> --voxel-scale 0.5 [options]
```

Example:

```text
voxel_baker statue.obj statue_0p5.vreg --voxel-scale 0.5 --watertight
```

Notes:

- `--voxel-scale` is **meters per voxel**, separate from `--scale`, which is a
  uniform multiplier applied to the source model's vertices.
- The narrow-band truncation delta is set automatically to
  `2.0 × voxel_scale` world meters (2 voxel widths). Only pass `--delta` if you
  intentionally want a different band.
- `--center-x`, `--center-y`, `--center-z` are in **region sample
  coordinates**, not world meters. World center = value × `voxel_scale`.
  The default is `(128, 128, 128)`.
- Changing `--voxel-scale` does not change the region's sample layout: an
  `8 × 8 × 8` region is always `256 × 256 × 256` samples and covers
  `256 × voxel_scale` meters per side.

### Compute the stamp origin chunk

`stamp_asset(path, origin_chunk, recreate_delay_seconds)` takes an origin chunk
in canonical chunk coordinates. To place the baked region's center at a desired
world position:

```
origin_chunk = round((world_center / voxel_scale - region_center_samples) / 32)
```

where `region_center_samples` is the bake's `--center-x/y/z` value (default
`128,128,128`).

Example with the default bake center and `voxel_scale = 0.5`:

```gdscript
var s := 0.5
var world_center := Vector3(16.0, 64.0, 0.0)
var region_center := Vector3(128.0, 128.0, 128.0) # baker default
var origin := ((world_center / s - region_center) / 32.0).round()
var id := terrain.stamp_asset("res://statue_0p5.vreg", origin, 15.0)
```

Stamping snaps to whole chunks, so the actual world center can differ by up to
`16 × voxel_scale` meters per axis (8 m at `0.5`, 4 m at `0.25`). To place an
asset exactly, bake it with a center that matches your target world position, or
account for the snap when choosing the origin.

The demo's Moai/Atlas assets are baked at `0.5 m` with region center
`(160, 160, 128)`; use that value in the formula when stamping those `.vreg`
files.

---

## Choosing a Scale

| Scale | Use it for | Trade-offs |
| :--- | :--- | :--- |
| `2.0` | Large, low-detail worlds; long draw distances | Chunkier silhouettes; blasts/damage resolve in 2 m steps |
| `1.0` | Default gameplay; balanced detail and memory | Baseline |
| `0.5` | Sculpted statues, fine craters, small props | ~8× chunk count for the same volume; re-bake assets |
| `0.25` | Small objects, close-up detail, dense caves | Very high memory and streaming cost; use a smaller view radius |

### Recommended starting values

| Setting | `1.0` | `0.5` | `0.25` |
| :--- | :--- | :--- | :--- |
| `world_height_chunks` | 4–6 | 6–8 | 12 |
| `view_distance_chunks` | 4–6 | 6–8 | 8–16 |
| `resident_memory_budget_mb` | 512 | 1024–2048 | 2048–4096 |
| `save_directory_path` | `.../saves_s100` | `.../saves_s050` | `.../saves_s025` |
| Asset `.vreg` files | baked at 1.0 | re-baked at 0.5 | re-baked at 0.25 |

The demo scene uses `voxel_scale = 0.5`, `view_distance_chunks = 6`, and
`world_height_chunks = 8`, covering about 208 m across and 128 m vertically.
The extra height keeps the 48 m stamped statues (tops at 120 m) inside the
world ceiling.

---

## Troubleshooting

| Symptom | Fix |
| :--- | :--- |
| `voxel_scale` change has no effect | Set it before the node enters the tree; reload the scene after changing it |
| Terrain is cut off above or below | Increase `world_height_chunks`; total height is `chunks × 32 × voxel_scale` |
| Terrain ends too close to the camera | Increase `view_distance_chunks`; coverage is `(2r+1) × 32 × voxel_scale` |
| Effective view distance drops or chunks fail to load | Raise `resident_memory_budget_mb` or reduce view/height chunks |
| Saved world looks wrong after changing scale | Use a separate `save_directory_path` per scale |
| Stamped asset is too small or thin | Re-bake it with the matching `--voxel-scale` |
| Stamped asset is offset from where you aimed | Account for chunk snapping; see the stamp-origin formula above |
| Craters look blockier/chunkier than expected | That is the new voxel resolution; use a smaller `voxel_scale` for finer geometry |
