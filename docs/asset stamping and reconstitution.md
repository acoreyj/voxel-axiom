# Asset Stamping & Reconstitution (`voxel_baker` → `.vreg` → live voxels)

Hand-crafted 3D meshes (`.obj`) can be voxelized **offline** into native $8 \times 8 \times 8$ chunk `.vreg` region files, stamped into the procedural world, destroyed with identical voxel physics, and either reconstituted after a quiet period or kept permanently destroyed. Meshes with UVs can also carry their **source albedo** into the voxels (RGB5551 per voxel), so sculpted props keep their texture through carving and reconstitution. This mirrors Nintendo's *Maya → Houdini → Voxel* asset workflow on top of the engine's existing TSDF/region format.

```
 [Artist Sculpt: Maya / Blender / Houdini]
                   │
                   ▼ (.obj)
 ┌──────────────────────────────────────────────────┐
 │           voxel_baker CLI Executable             │
 │  1. ObjMeshLoader: Triangulation & Material Tags │
 │  2. MeshSpatialGrid: O(1) Accelerated Distance   │
 │  3. Narrow-Band TSDF: Shell / Watertight         │
 │  4. Byte-Plane RLE Codec: 512-TOC & FNV-1a64     │
 └──────────────────────────────────────────────────┘
                   │
                   ▼ (.vreg)
 ┌──────────────────────────────────────────────────┐
 │         AssetStampManager & VoxelTerrain3D       │
 │  • Stamping: Union / Overwrite into ChunkStore   │
 │  • Physics Destruction: Multi-Hit Rock & CSG     │
 │  • Damage Tracking: notify_chunk_damaged(ck)     │
 │  • Reconstitution: Timer → baseline TSDF restore │
 │    (stays destroyed forever if delay <= 0)       │
 └──────────────────────────────────────────────────┘
```

Relevant files:

| File | Role |
| :--- | :--- |
| `include/voxel_mesh_baker.hpp` | OBJ + UV loader, PPM albedo sampling, spatial-grid distance queries, TSDF + RGB5551 color voxelizer, `.vreg` writer |
| `src/voxel_baker_main.cpp` | Standalone `voxel_baker` CLI entry point |
| `include/asset_stamp_manager.hpp` | `AssetStampManager`: stamping, baselines, damage tracking, reconstitution |
| `src/voxel_terrain_3d.cpp/.h` | `stamp_asset()` GDScript binding, `structure_damaged`/`structure_recreated` signals |
| `tests/test_offline_asset_baker.cpp` | End-to-end suite: loader math, baking, destruction, reconstitution, permanent mode |

---

## 1. Bake an `.obj` into a `.vreg`

Build the CLI (CMake target `voxel_baker`; output lands in `build/bin/` — on multi-config generators like Visual Studio, `build/bin/Release/voxel_baker.exe`):

```bash
cmake --build build --config Release --target voxel_baker
```

Run it:

```bash
voxel_baker <input.obj> <output.vreg> [options]
```

| Option | Default | Meaning |
| :--- | :--- | :--- |
| `--scale <float>` | `1.0` | Uniform multiplier applied to mesh vertices before voxelization. |
| `--voxel-scale <float>` | `1.0` | Meters per voxel sample. Also sets `--delta` to `2 × voxel_scale` unless `--delta` is passed. |
| `--thickness <float>` | `1.6` | Shell wall thickness $T$ in meters (shell mode only). |
| `--material <name>` | `hard_rock` | Default material for geometry with no/unknown `usemtl`. |
| `--watertight` | off | Watertight solid mode: closed interiors become solid (surface sits exactly on the source mesh). |
| `--texture <file.ppm>` | none | Binary PPM (P6/P3) albedo sampled through the OBJ's UVs into per-voxel RGB5551 colors. |
| `--color-depth <float>` | `1.8` | How deep from the source surface color is baked; deeper interior voxels stay unpainted raw rock. |
| `--no-color` | off | Disable color baking even when a texture is supplied. |
| `--center-x/y/z <float>` | `128` | Target region-sample center for auto-centering (lets you offset content inside the region). |
| `--no-center` | off | Do not auto-center the mesh inside the region (keep raw vertex coordinates). |
| `--rx/--ry/--rz <int>` | `0` | Region coordinates written into the file header. |
| `--delta <float>` | `2 × voxel_scale` | Narrow-band truncation delta in meters (TSDF encode range); `2.0` at `voxel_scale = 1.0`. |
| `--help, -h` | | Print usage. |

Example:

```bash
voxel_baker ancient_ruin.obj ancient_ruin.vreg --thickness 2.0 --material hard_rock
```

### What the baker does

1. **`ObjMeshLoader`** parses the Wavefront file, fan-triangulates every polygon face (`f` with any vertex count), and maps `usemtl` names to engine material IDs:

   | Name contains | Material (ID) |
   | :--- | :--- |
   | `lava`, `magma` | Lava (3, 1-hit) |
   | `obsidian` | Obsidian (5, 3-hit) |
   | `ice`, `frost` | Ice (6, 1-hit) |
   | `water` | Water (4, 1-hit) |
   | `soil`, `dirt`, `wood`, `grass` | Soil (1, 1-hit) |
   | anything else / unspecified | Hard Rock (2, 3-hit) |

2. **`MeshSpatialGrid`** buckets triangles into an 8 m acceleration grid so point-to-triangle distance queries only visit nearby cells instead of brute-forcing the whole model across the $256^3$ region volume. A parallel grid accelerates the watertight inside/outside parity rays.

3. **Voxelization modes**
   - **Shell Thickness** (default): signed distance $S = d - T/2$ against the nearest surface. Works on arbitrary *open* meshes — ruins, arches, and walls do not need to be manifold; they get a real $T$ metre wall thickness. Note the isosurface sits $T/2$ outside the source surface, so a closed sculpt is inflated by $T/2$ in every direction.
   - **Watertight Solid** (`--watertight`): closes the interior with an XY ray-parity test ($S = \pm d$). The visible surface stays exactly on the source mesh, and blasting exposes the solid interior instead of a hollow cavity — ideal for sculpted props.

4. **Per-voxel baked colors**: when `--texture` is supplied, the closest-point query also interpolates the triangle's barycentric UVs, samples the albedo bilinearly, and writes an RGB555 + painted-bit value into every solid voxel within `--color-depth` of the source surface. Voxels deeper than that (the raw interior a crater exposes) stay unpainted. In shell mode only the outer half of the shell is painted (surface-normal side), so crater bowls read as raw rock even when the shell is thinner than `--color-depth`.

5. **`.vreg` serialization**: the region is 512 chunks ($8 \times 8 \times 8$ of $32^3$). Chunks fully outside the narrow band are uniform `Empty` (0 payload bytes); fully solid chunks are uniform `Solid`; boundary chunks are encoded as 16-bit TSDF samples ($\delta$ = `--delta`), split into low/high byte planes, byte-plane-RLE compressed, and written with a 512-entry TOC and an FNV-1a64 payload checksum. Painted chunks additionally carry a compressed RGB5551 color plane. This is the engine's canonical persistence format (`persistence_and_streaming.hpp`), so a baked file can also double as a save region (name it `region_<rx>_<ry>_<rz>.vreg` so the file name matches the header coordinates).

Layout on disk (`#pragma pack`):

```
RegionFileHeader     magic "VREG", version 4, rx/ry/rz (int32), payload_checksum (u64)   = 28 B
RegionTocEntry[512]  (form u8, base_material_id u8, has_material_plane u8,               = 6144 B
                      has_color_plane u8, payload_offset u32, payload_size u32) each
payload              per-Dense-chunk, unpainted: [low_sz][high_sz][mat_sz] (12 B) + RLE planes
                     per-Dense-chunk, painted:  [low_sz][high_sz][mat_sz][col_sz] (20 B) + RLE planes
```

Two useful properties: a baked asset is *centered* at region-local sample `(128, 128, 128)` by default (override with `--center-x/y/z`) unless `--no-center` is passed, and the region-local chunk coordinates run `(0..7, 0..7, 0..7)` from the region origin.

---

## 2. Stamp the asset into the world (GDScript)

After changing C++ code, rebuild and redeploy the GDExtension before opening the editor (Godot locks the `.dll`/`.so` it has loaded):

```bash
cmake --build build --config Release --target voxel_engine
```

Place the `.vreg` where the process can open it and call:

```gdscript
var structure_id := $VoxelTerrain3D.stamp_asset("res://ancient_ruin.vreg", Vector3(4, 0, 4), 15.0)
```

**`stamp_asset(vreg_path, chunk_coords, recreate_delay_seconds) -> int`**

- `chunk_coords` is the **origin chunk** of the region — the world position of region-local chunk `(0, 0, 0)`. The asset therefore occupies the 8×8×8 chunk block starting there, and (with default centering) the mesh itself sits around `origin + (4, 4, 4)` chunks.
- `recreate_delay_seconds > 0` arms the reconstitution timer; `<= 0` makes the structure permanent (destroyed stays destroyed).
- Returns the structure ID (`> 0`), or `0` on failure (engine not ready, file missing, bad magic).
- Color data baked into the `.vreg` is stamped into the chunks in the same transaction; `remove_structure(id)` restores every affected chunk to its exact pre-stamp state (TSDF, materials, colors) and returns `true`.

> The core opens the path with `fopen`, so prefer an absolute path or a project-relative path (the demo uses `models/easter-island/easter_island_statue.vreg`). A `res://` path works in the editor when the file exists on disk.

Placement example — to straddle the crosshair hit point with the mesh center, subtract the baked center offset (this is what the demo does):

```gdscript
var hit := $VoxelTerrain3D.raycast_terrain(camera.global_position, camera.global_position - dir * 250.0)
var chunk := Vector3i(int(floor(hit.x / 32.0)), int(floor(hit.y / 32.0)), int(floor(hit.z / 32.0)))
var origin := chunk - Vector3i(4, 4, 4)      # region center lands at the hit chunk
var id := $VoxelTerrain3D.stamp_asset("res://ancient_ruin.vreg", Vector3(origin), 15.0)
```

Stamp chunks land in the vertical range `0 … world_height_chunks - 1` (the streamable band), so pick `origin.y` so the whole structure fits: an 8-chunk-tall region needs `world_height_chunks >= 8` to fit fully, or bury the lower empty chunks below layer 0 (far-field `Empty` chunks are skipped, so they cost nothing).

### Baked albedo in the mesh (`COLOR.a` / `UV` / `UV2`)

Painted voxels become a per-vertex albedo stream on the remeshed chunks:

| Attribute | Carries |
| :--- | :--- |
| `COLOR.rgb` | damage ratio, hard-rock flag, material code (unchanged) |
| `COLOR.a` | baked-albedo weight (0 = unpainted interior, 1 = painted surface) |
| `UV.xy` | baked albedo red/green |
| `UV2.x` | baked albedo blue |

The terrain shader converts the sRGB-encoded baked color to linear and blends it over the procedural albedo, so painted statues keep their texture through carving and reconstitution while newly exposed deep voxels fall back to raw procedural rock. Unpainted chunks allocate **0 B** and upload no UV arrays.

### Blend modes

The GDScript binding uses `StampBlendMode::Union`. The C++ API also exposes `Overwrite`:

- **Union** (`q_new = min(q_world, q_asset)` per sample): the asset merges into existing terrain; a uniform-`Solid` asset chunk forces the whole world chunk solid.
- **Overwrite**: every non-empty asset chunk replaces the world chunk wholesale (including the asset's air). Empty asset chunks are always skipped in both modes.

---

## 3. Damage tracking & reconstitution

When a stamped structure is created, `AssetStampManager` snapshots each stamped chunk's post-stamp state (storage form, base material, TSDF samples, material plane, baked colors) as its **reconstitution baseline**, and its pre-stamp state as the **removal baseline**.

- Every completed **non-fill** edit transaction (explosions and elemental spheres — not `add_terrain`/`place_boulder` fills) reports its dirtied chunks to `notify_chunk_damaged()`. Any structure whose stamped chunks intersect is marked damaged and its timer restarts; the scene emits `structure_damaged`.
- Carving/filling/elemental edits clear the baked color of voxels they convert to air, so crater interiors expose raw rock instead of stretching the painted skin.
- Each frame, `process_reconstitution(delta, ...)` advances damaged structures' timers:
  - **`delay > 0`**: after a *quiet period* (no new hits) equal to the delay, the exact baseline TSDF, materials, colors, and damage buffers are restored. The terrain revision is bumped, the chunks are published to the edit-dirty lane for remeshing, and the affected navigation groups are queued for Recast rebake. The scene emits `structure_recreated`.
  - **`delay <= 0`**: the structure is skipped forever — it stays permanently destroyed.
- `remove_structure(id)` removes a structure immediately: every affected chunk is restored to its pre-stamp baseline (evicting chunks that did not exist before the stamp), remeshed, and re-baked for navigation. The scene emits `structure_removed`.

Signals on `VoxelTerrain3D`:

| Signal | Payload | Fires when |
| :--- | :--- | :--- |
| `structure_damaged` | `structure_id`, `name` | A stamped structure's chunks were carved; the reconstitution timer (re)starts. |
| `structure_recreated` | `structure_id`, `name` | The quiet period elapsed and the exact pre-damage voxels were restored. |
| `structure_removed` | `structure_id`, `name` | `remove_structure()` restored the pre-stamp chunks. |

---

## 4. Using it in the demo

Three two-option statue pairs are shown side by side in `voxel_terrain_demo.tscn` (`terrain_demo.gd`): the 16x Easter Island pair at `z = 0`, the 5.33x Atlas pair at `z = -64`, and the 5.33x Ape pair at `z = -128`.

| Option | What it demonstrates |
| :--- | :--- |
| **Option 1 — Pristine Mesh Swap** (`-16, 0` / `-16, -64` / `-32, -128`) | A textured pristine mesh + StaticBody3D is shown until the first explosion/elemental hit reaches it. The hit hides the mesh and stamps the same statue into the voxel terrain, so the crater is carved into voxels from the first frame. Key **R** restores the pre-stamp chunks and the pristine mesh. |
| **Option 2 — Baked Voxel Colors** (`+16, 0` / `+16, -64` / `+32, -128`) | The statue is stamped directly at startup from its `.vreg` with per-voxel RGB5551 albedo, 3-hit crack damage, raw-rock crater interiors, and a 15 s reconstitution timer. Key **R** removes and re-stamps it. |

| Input | Action |
| :--- | :--- |
| **Key R** | Reset all six statues (Option 1: remove structure + restore pristine mesh; Option 2: remove + re-stamp). |
| **Key T** | Toggle the add/paint material (soil / 3-hit rock). |
| **Key 5** | Select the **Small hit** chip blast (`small_hit_radius`, default 1.5 m) for sculpting fine craters into a statue. |
| **Key B** | Stamp `stamp_asset_path` at the crosshair with `stamp_recreate_delay_seconds` (default `15 s`). |
| **Shift + B** | Stamp `stamp_asset_path` permanently (no reconstitution timer). |

Demo exports (Inspector): `stamp_asset_path`, `stamp_recreate_delay_seconds`,
`easter_island_mesh_path` (`res://models/easter-island/easter_island.obj`),
`easter_island_albedo_path` (`res://models/easter-island/easter_island_albedo.png`),
`easter_island_vreg_path` (`models/easter-island/easter_island_statue.vreg`). Bake the statue from the scaled OBJ + PPM albedo:

```bash
voxel_baker models/easter-island/easter_island.obj \
    models/easter-island/easter_island_statue.vreg \
    --voxel-scale 0.5 --watertight --color-depth 2.8 \
    --texture models/easter-island/easter_island_albedo.ppm \
    --center-x 160 --center-y 160 --center-z 128
```

`--center-y 160` seats the 48 m model (base at local sample 112) on the terrain; the demo resolves the stamp origin from `get_surface_height()` at runtime. All three statues are baked with `--watertight`; the `2.8 m` color shell keeps texture on the surface while craters expose raw rock. A minimal open ruin example requires shell mode:


```obj
# ancient ruin shrine
usemtl ancient_stone
v -10.0 0.0 -10.0
v  10.0 0.0 -10.0
v  10.0 0.0  10.0
v -10.0 0.0  10.0
v -10.0 12.0 -10.0
v  10.0 12.0 -10.0
v  10.0 12.0  10.0
v -10.0 12.0  10.0
f 1 2 3 4
f 1 5 6 2
f 2 6 7 3
f 3 7 8 4
f 5 6 7 8
```

```bash
voxel_baker ancient_ruin.obj ancient_ruin.vreg --thickness 2.0
```

---

## 5. C++ core API (no Godot required)

`AssetStampManager` is pure C++ (`include/asset_stamp_manager.hpp`), exercised by `tests/test_offline_asset_baker.cpp`:

```cpp
voxel::MemoryLedger ledger;
voxel::ChunkStore store(ledger, /*world_height_chunks=*/8);
voxel::DualDirtyScheduler dirty_scheduler;
voxel::VoxelNavigationManager nav_mgr;
nav_mgr.initialize(voxel::ServerRid{3});
voxel::AssetStampManager stamp_mgr;

const uint32_t id = stamp_mgr.stamp_region_file(
    store, dirty_scheduler, nav_mgr,
    "ancient_ruin.vreg",
    voxel::ChunkKey::from_coords(0, 0, 0),
    /*recreate_delay_seconds=*/15.0f,
    voxel::StampBlendMode::Union,
    "AncientShrine");

// When your edit/explosion pass dirties a chunk:
stamp_mgr.notify_chunk_damaged(ck);
// Once per frame:
stamp_mgr.process_reconstitution(delta_seconds, store, dirty_scheduler, nav_mgr);
// Reset (restore the pre-stamp chunks):
stamp_mgr.remove_structure(id, store, dirty_scheduler, nav_mgr);
```

`set_structure_damaged_callback` / `set_structure_recreated_callback` / `set_structure_removed_callback` receive `(structure_id, name)` notifications; `find_structure` and `active_structure_count` expose internal state. In-memory stamping is also available via `stamp_region()` if you already hold a `BakedRegionData` (e.g. from `VoxelMeshBaker::bake_mesh_to_region`).

---

## 6. Testing

- CMake/CTest: suite `test_offline_asset_baker` (point-triangle/OBJ parsing, baking to `.vreg` with checksum verification, 3-hit hard-rock destruction → reconstitution, permanent mode, RGB5551 outer-skin color baking, raw crater interiors, color persistence, and `remove_structure` pre-stamp restoration).
- `test_gdextension_bindings` includes `test_gdextension_moai_stamp_baked_colors_and_remove_structure` (painted vertex stream committed through `ServerSlotManager`, reset binding).
- `run_all_tests.sh` also builds the `voxel_baker` CLI alongside its ASan and TSan passes.

```bash
ctest --test-dir build -C Release -R test_offline_asset_baker --output-on-failure
# or the full run:
./run_all_tests.sh
```

---

## 7. Troubleshooting

| Symptom | Cause / fix |
| :--- | :--- |
| `stamp_asset` returns `0` | File not found under the given path, bad magic/version, or the engine isn't in the tree yet. The demo pushes a warning on failure. Use a project-relative or absolute path (the core opens it with `fopen`). |
| Godot fails to load the extension after merging C++ changes | The editor locks the deployed DLL/SO. Close Godot, rebuild `voxel_engine`, then relaunch. |
| Nothing visible after stamping | The region landed outside the streamable band (`y` must be `0 … world_height_chunks-1`), or the mesh is too small/thin at 1 voxel per unit — raise `--scale` (and/or `--thickness`). |
| Asset floats / sinks by ~a few meters | Baked regions center at local sample `(128, 128, 128)` (or `--center-x/y/z`) = chunk `(4, 4, 4)`. Offset your origin accordingly, or bake with `--no-center` and author coordinates for the region base. |
| Statue looks inflated after a mesh swap | Shell mode's isosurface sits `T/2` outside the source surface. Bake closed sculpts with `--watertight` so the surface stays on the source mesh. |
| Craters show stretched texture instead of raw rock | `--color-depth` is too large relative to the cavity depth (or the model was baked in shell mode, where the whole shell is painted). Use `--watertight --color-depth 1.8` so deep interior voxels stay unpainted. |
| `.glb`/`.fbx` models | The baker reads Wavefront `.obj` only; export from your DCC first (the demo asset was exported with Blender and scaled 3x to 9 m). |
| `.png` textures | The baker samples binary PPM (`P6`/`P3`); convert the albedo to PPM first (e.g. Blender export or a one-off conversion script). |
| A destroyed structure never returns | Its stamp used `recreate_delay_seconds <= 0` (permanent), or hits keep restarting the timer (restoration waits for a quiet period). |
| Reconstituted structure has stale navmesh/physics | Reconstitution dirties chunks through the standard edit path; give the worker pool a frame or two — navmesh rebakes are asynchronous. |
