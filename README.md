# Voxel Engine v4.0

A real-time, destructible voxel terrain engine for **Godot 4**, written in modern **C++20**.
The authoritative voxel core has **zero Godot dependencies** and is statically linked into
both the test suite and a **GDExtension** written with `godot-cpp`.

The engine combines:

- Seam-free **Surface Nets** meshing over a truncated signed distance field (TSDF).
- Multi-threaded chunk generation, meshing, streaming, and persistence.
- Real-time spherical CSG destruction with elemental material reactions.
- Procedural 3D caves and overhangs.
- Asynchronous 3D cave navigation for AI characters.
- Offline `.obj` -> `.vreg` asset voxelization with damage and reconstitution.
- Direct `RenderingServer` / `PhysicsServer3D` RID management without per-chunk nodes.

This README is the entry point. Every feature below links to a focused design or usage
document. For a one-screen map of all documentation, jump to the
[documentation index](#documentation-index).

---

## Table of contents

- [Quick start](#quick-start)
- [Feature overview](#feature-overview)
- [Feature guides](#feature-guides)
  - [Chunk streaming and residency](#chunk-streaming-and-residency)
  - [Surface Nets meshing](#surface-nets-meshing)
  - [Procedural terrain and caves](#procedural-terrain-and-caves)
  - [Real-time destruction](#real-time-destruction)
  - [Elemental reactions](#elemental-reactions)
  - [Terrain queries and raycasting](#terrain-queries-and-raycasting)
  - [AI companion and 3D cave navigation](#ai-companion-and-3d-cave-navigation)
  - [Persistence and region files](#persistence-and-region-files)
  - [Asset baking and stamping](#asset-baking-and-stamping)
  - [Undo and redo](#undo-and-redo)
  - [Physics collision](#physics-collision)
  - [Triplanar shading](#triplanar-shading)
  - [Performance instrumentation](#performance-instrumentation)
  - [Automated benchmark suite](#automated-benchmark-suite)
- [Node and API reference](#node-and-api-reference)
- [Demo scene and controls](#demo-scene-and-controls)
- [Building from source](#building-from-source)
- [Tests and sanitizers](#tests-and-sanitizers)
- [Production builds](#production-builds)
- [Voxel scale, depth, and model detail](#voxel-scale-depth-and-model-detail)
- [Project structure](#project-structure)
- [Documentation index](#documentation-index)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Quick start

### Requirements

| Requirement | Notes |
| --- | --- |
| C++20 compiler | GCC 11+, Clang 14+, or MSVC 2022 (v143+) |
| CMake | 3.22 or newer |
| Godot | 4.2+; the project file targets Godot 4.7 |
| `godot-cpp` | Branch matching your Godot version, built with the same toolchain |

### Build and run in three steps

```bash
# 1. Configure and build the pure C++ core, baker, and tests
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVOXEL_BUILD_TESTS=ON
cmake --build build -j

# 2. Build the Godot extension (separate build directory, with godot-cpp)
cmake -B build-gdext -DVOXEL_BUILD_GDEXTENSION=ON -DGODOT_CPP_DIR=/path/to/godot-cpp
cmake --build build-gdext --config Release -j

# 3. Open project.godot in Godot 4.2+ and run voxel_terrain_demo.tscn (F5)
```

On Windows PowerShell:

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVOXEL_BUILD_TESTS=ON
cmake --build build --config Release -j

cmake -B build-gdext -DVOXEL_BUILD_GDEXTENSION=ON -DGODOT_CPP_DIR="../godot-cpp"
cmake --build build-gdext --config Release -j
```

If you only want the demo, step 2 is the only build you need. If you want the offline
asset baker or tests, step 1 produces `voxel_baker` and the CTest suites.

> With **Ninja** or **Makefiles**, `CMAKE_BUILD_TYPE` selects Debug/Release. With the
> **Visual Studio** or **Xcode** generators, pass `--config Release` or `--config Debug`
> at build time instead. See [Building from source](#building-from-source).

---

## Feature overview

| Feature | What it gives you | Deep dive |
| --- | --- | --- |
| **Chunk streaming** | 32x32x32 sample chunks with view/prefetch radii, memory ledger, and frame-budgeted commits | [Design v4, Section 3.3](docs/Voxel%20Design%20Doc%20v4.md#33-chunk-streaming-and-residency-management), [voxel scale guide](voxel_size_configuration_guide.md) |
| **Surface Nets meshing** | Watertight dual-contour meshes across chunk seams using a 36x36x36 scratch window | [Design v4, Section 3.1](docs/Voxel%20Design%20Doc%20v4.md#31-data-layout-and-chunk-store), [`procgen_and_surface_nets.hpp`](include/procgen_and_surface_nets.hpp) |
| **Procedural terrain and caves** | Noise-generated heightmaps plus 3D cave worms and Swiss-cheese cavities; caves can be disabled per world | [Design v4, Section 3.5](docs/Voxel%20Design%20Doc%20v4.md#35-system-implementations), [terrain depth](docs/terrain%20depth.md) |
| **Real-time destruction** | Asynchronous sphere carve/add/boulder edits with expanded TSDF bands and anti-crumb pruning | [Design v4, Sections 1.2 and 3.4](docs/Voxel%20Design%20Doc%20v4.md#34-worker-pool-and-scheduling), [performance analysis](docs/Voxel%20Performance%20Analysis.md) |
| **Elemental reactions** | Water, frost, and lava spheres that transform materials on contact | [`terrain_demo.gd`](terrain_demo.gd), [`voxel_triplanar.gdshader`](voxel_triplanar.gdshader) |
| **Terrain queries** | Voxel-space raycasts, material lookup, and surface-height queries without physics colliders | [AI & navigation, Section 7.1](docs/AI%20Companion%20%26%20Navigation.md#71-pill-companion-patrol), [`terrain_raycast.hpp`](include/terrain_raycast.hpp) |
| **AI navigation** | 3D cave navigation groups, asynchronous Recast bakes, and path queries through `VoxelAICompanion` | [AI Companion & Navigation](docs/AI%20Companion%20%26%20Navigation.md) |
| **Persistence** | Two-stage saving into 8x8x8 chunk region files with LZ4 compression and atomic writes | [Design v4, Section 3.6](docs/Voxel%20Design%20Doc%20v4.md), [`persistence_and_streaming.hpp`](include/persistence_and_streaming.hpp) |
| **Asset stamping** | Bake OBJ meshes to `.vreg`, stamp them into live terrain, damage them, and restore them | [Asset stamping and reconstitution](docs/asset%20stamping%20and%20reconstitution.md) |
| **Undo / redo** | Bit-exact per-chunk snapshot history for edits, stamps, and structure removals, plus a coalesced full-world reset | [Undo and redo](#undo-and-redo) |
| **Physics collision** | Per-chunk concave `PhysicsServer3D` bodies with distance gating and optional subsampling | [Design v4, Section 4.4](docs/Voxel%20Design%20Doc%20v4.md#44-physics-collision) |
| **Triplanar shading** | Material-aware PBR shader with grass, soil, rock, lava, water, obsidian, ice, and crack glow | [`voxel_triplanar.gdshader`](voxel_triplanar.gdshader) |
| **First-person pickaxe** | Procedurally built arm and pickaxe viewmodel with windup, strike, follow-through, solid-hit recoil, bob, mouse sway, and impact sparks | [`terrain_demo.gd`](terrain_demo.gd) |
| **Clean-screen UI toggle** | One button (or `U`) hides the stats readout, controls overlay, world labels, and the button itself, leaving only the reticle and pickaxe for screenshots | [`terrain_demo.gd`](terrain_demo.gd) |
| **Instrumentation** | Lock-free per-stage timings for generate, mesh, commit, edit, streaming, and navigation | [Performance analysis](docs/Voxel%20Performance%20Analysis.md), [`stage_timings.hpp`](include/stage_timings.hpp) |
| **Benchmark suite** | Five streaming/collision presets measure frame percentiles, streaming, CSG latency, navigation, VRAM, RAM, and draw calls; writes Markdown and JSON reports | [Automated benchmark suite](#automated-benchmark-suite) |

---

## Feature guides

### Chunk streaming and residency

The world is split into `32 x 32 x 32`-sample chunks. `VoxelTerrain3D` streams chunks
around a focus position (normally the camera), keeping a configurable prefetch ring ahead
of the active radius. Streaming is chunk-coordinate based, so `voxel_scale` changes how
large a chunk is in meters without changing its memory layout.

Key properties:

| Property | Default | Purpose |
| --- | --- | --- |
| `view_distance_chunks` | `2` | Active Chebyshev radius in chunks. Automatically reduced by 2 when the memory ledger is exhausted. |
| `prefetch_distance_chunks` | `1` | Extra ring streamed ahead of the active radius. |
| `resident_memory_budget_mb` | `512` | Hard budget for resident chunk data. |
| `upload_budget_ms` | `2.0` | Per-frame main-thread budget for mesh, physics, and navigation uploads. |
| `voxel_scale` | `1.0` | Meters per voxel sample, clamped to `[0.125, 4.0]`. Set before the node enters the tree. |
| `world_height_chunks` | `4` | Vertical ceiling in 32-sample chunks. Baked in at startup. |

```gdscript
var terrain := VoxelTerrain3D.new()
terrain.voxel_scale = 0.5
terrain.view_distance_chunks = 8
terrain.prefetch_distance_chunks = 2
terrain.world_height_chunks = 8
terrain.resident_memory_budget_mb = 1024
add_child(terrain)

# Usually called every frame with the camera position and velocity.
terrain.set_stream_focus(camera.global_position, camera_velocity)
```

The demo scene uses `voxel_scale = 0.5`, `view_distance_chunks = 12`,
`prefetch_distance_chunks = 4`, and `world_height_chunks = 8`.

**Read more**

- [Design v4: Chunk streaming and residency](docs/Voxel%20Design%20Doc%20v4.md#33-chunk-streaming-and-residency-management)
- [Voxel size guide: what changes when you change scale](voxel_size_configuration_guide.md#what-to-adjust-when-you-change-scale)
- [Performance analysis: streaming and commit budgets](docs/Voxel%20Performance%20Analysis.md#1-what-the-engine-already-handles-well)

### Surface Nets meshing

Each chunk owns canonical `32^3` TSDF samples. To mesh without cracks at chunk borders,
meshing gathers a thread-local `36^3` window that reaches one sample into negative
neighbors and two samples into positive neighbors. Surface Nets emits one dual vertex per
sign-changing cell and connects adjacent cells into quads; central differences provide
smooth vertex normals.

Meshing is deliberately resolution-uniform: there is no LOD or Transvoxel transition in
v4.0. Decimation happens only on the collision/navigation copy, never on the visual mesh.

```text
32^3 owned samples
      |
      v
36^3 scratch window  ->  Surface Nets  ->  visual mesh (32^3 cells)
      |                                  ->  collision/nav soup (optional 16^3 subsample)
      +-- selective neighbor dirtying on boundary samples
```

**Read more**

- [Design v4: Data layout and the 36^3 window](docs/Voxel%20Design%20Doc%20v4.md#31-data-layout-and-chunk-store)
- [`include/procgen_and_surface_nets.hpp`](include/procgen_and_surface_nets.hpp)
- [`tests/test_procgen_and_surface_nets.cpp`](tests/test_procgen_and_surface_nets.cpp)

### Procedural terrain and caves

Terrain is generated from a 2D height field plus optional 3D cave noise. The surface sits
around `surface_base_height` meters above the `y = 0` bedrock; `world_height_chunks`
sets the vertical ceiling. `generate_caves = false` removes the 3D cave term entirely.

| Property | Default | Purpose |
| --- | --- | --- |
| `surface_base_height` | `68.0` | Average surface height above bedrock, in meters. |
| `world_height_chunks` | `4` | Vertical ceiling. Total height is `world_height_chunks * 32 * voxel_scale` meters. |
| `world_seed` | `1337` | Procedural seed. Changes affect chunks generated afterwards. |
| `generate_caves` | `true` | When false, generate cave-free terrain. Read per chunk at generation time. |

Caves are enabled in the main demo scene. Set `generate_caves = false` in the Inspector or
at runtime for cave-free terrain; the former standalone `voxel_terrain_no_caves_demo.tscn`
was removed so the demo has one source scene.

```gdscript
$VoxelTerrain3D.generate_caves = false
$VoxelTerrain3D.surface_base_height = 132.0
$VoxelTerrain3D.world_height_chunks = 8
```

**Read more**

- [Design v4: System implementations](docs/Voxel%20Design%20Doc%20v4.md#35-system-implementations)
- [Terrain depth guide](docs/terrain%20depth.md)
- [Voxel size guide: world height and view distance](voxel_size_configuration_guide.md#1-world-height)
- [`include/procgen_and_surface_nets.hpp`](include/procgen_and_surface_nets.hpp)

### Real-time destruction

Destruction is asynchronous CSG on the TSDF. A sphere edit is phased into bounded
lock sets, expands the evaluation band by the narrow-band delta, and prunes isolated
floating voxels. Carves and fills run on the edit worker lane so they cannot starve
streaming or meshing work.

| Method | Effect |
| --- | --- |
| `trigger_explosion(position, radius)` | Carve a sphere. |
| `add_terrain(position, radius)` | Add a sphere of soil. |
| `add_terrain_with_material(position, radius, material)` | Add a sphere with an explicit material ID. |
| `place_boulder(position, radius)` | Add a boulder-material sphere. |
| `trigger_elemental_sphere(position, radius, kind)` | Apply water, frost, or lava reactions. |

```gdscript
var terrain := $VoxelTerrain3D

terrain.trigger_explosion(Vector3(16, 70, 16), 8.5)
terrain.add_terrain(Vector3(16, 70, 16), 6.0)
terrain.place_boulder(Vector3(24, 68, 16), 4.0)
```

Edits report through signals:

| Signal | Payload |
| --- | --- |
| `explosion_requested` | `position`, `radius` |
| `explosion_completed` | `position`, `radius`, `affected_chunks` |
| `explosion_failed` | `reason` |
| `terrain_add_requested` | `position`, `radius` |
| `terrain_add_completed` | `position`, `radius`, `affected_chunks` |
| `terrain_add_failed` | `reason` |
| `chunk_committed` | `chunk_key` |

`chunk_committed` fires when the chunk's mesh, collision, and navigation group are
consistent, not just when its visual mesh is uploaded.

**Read more**

- [Design v4: Worker pool and scheduling](docs/Voxel%20Design%20Doc%20v4.md#34-worker-pool-and-scheduling)
- [Design v4: Core capabilities](docs/Voxel%20Design%20Doc%20v4.md#12-core-capabilities)
- [Performance analysis: edit and de-crumb findings](docs/Voxel%20Performance%20Analysis.md#3-findings--recommendations)
- [`include/edit_and_worker_pool.hpp`](include/edit_and_worker_pool.hpp)

### Elemental reactions

`trigger_elemental_sphere(position, radius, kind)` applies a material-aware reaction
sphere. The kinds are integers shared with the C++ core:

| Kind | Element | Behavior |
| --- | --- | --- |
| `1` | Water | Quenches lava into 3-hit obsidian and cools cracked rock. |
| `2` | Frost / ice | Builds a flat-topped, ramped ice bridge and crater plug; converts air and water to ice; quenches lava to obsidian. |
| `3` | Lava / magma | Injects lava; melts ice to air; converts water to obsidian. |
| `<= 0` | Plain carve | Falls back to `trigger_explosion`. |

```gdscript
terrain.trigger_elemental_sphere(hit_point, 8.5, 1) # Water
terrain.trigger_elemental_sphere(hit_point, 8.5, 2) # Frost / Ice
terrain.trigger_elemental_sphere(hit_point, 8.5, 3) # Lava / Magma
```

In the demo, keys `8`, `9`, and `0` select water, frost, and lava. The shader colors the
resulting materials (lava emissive, ice pale blue, obsidian nearly black) and can show
crack glow on damaged hard rock.

**Read more**

- [`terrain_demo.gd`](terrain_demo.gd) - tool palette and reaction calls
- [`voxel_triplanar.gdshader`](voxel_triplanar.gdshader) - material colors and crack glow
- [Design v4: Core capabilities](docs/Voxel%20Design%20Doc%20v4.md#12-core-capabilities)

### Terrain queries and raycasting

Queries run against resident TSDF data, not against physics colliders. This means aiming,
spawning, and patrol logic still work when `collision_distance_chunks` has gated distant
chunks out of the physics server.

| Method | Returns |
| --- | --- |
| `raycast_terrain(from, to)` | First surface hit in world space, or `Vector3(INF, INF, INF)` when nothing is hit. |
| `raycast_terrain_material(from, to)` | Material ID of the solid voxel behind the surface, or `-1` when nothing is hit. |
| `get_surface_height(x, z)` | Topmost air-to-solid crossing of the resident column, with a procedural fallback. |

Material IDs:

| ID | Material |
| --- | --- |
| `0` | Air |
| `1` | Soil |
| `2` | Hard rock |
| `3` | Lava |
| `4` | Water |
| `5` | Obsidian |
| `6` | Ice |
| `7` | Boulder |

```gdscript
var hit: Vector3 = terrain.raycast_terrain(camera.global_position, aim_point)
if hit.x != INF:
    var material: int = terrain.raycast_terrain_material(camera.global_position, aim_point)
    print("Looking at material ", material, " at ", hit)

var ground_y: float = terrain.get_surface_height(16.0, 16.0)
```

`get_surface_height` is main-thread only. It reflects edited resident chunks; far from the
streaming focus it falls back to the pristine generated surface.

**Read more**

- [AI & navigation: surface-height semantics](docs/AI%20Companion%20%26%20Navigation.md#8-gotchas-quirks--limitations)
- [`include/terrain_raycast.hpp`](include/terrain_raycast.hpp)
- [`tests/test_terrain_raycast.cpp`](tests/test_terrain_raycast.cpp)

### AI companion and 3D cave navigation

`VoxelAICompanion` is a thin query facade over Godot's `NavigationServer3D`. The engine
partitions the world into `4 x 2 x 4`-chunk navigation groups, shares the de-indexed
collision triangle soup as Recast source geometry, and bakes each group asynchronously.
An underground edit rebakes its local group without touching surface navigation.

| Method | Behavior |
| --- | --- |
| `get_voxel_path(start, end)` | Returns optimized navigation waypoints, or an empty array when no baked region covers the endpoints. |
| `is_path_region_synced(position)` | True when the group containing the position has no bake in flight and matches its desired revision. |
| `set_terrain_node(terrain)` | C++-only link used for the sync check and navigation timing. Not bound to GDScript. |

```gdscript
if companion.is_path_region_synced(target):
    var path: PackedVector3Array = companion.get_voxel_path(
        companion.global_position, target)
```

Important operational notes:

- Check `is_path_region_synced()` before querying; otherwise a path may silently be empty.
- In live Godot builds, navigation groups are created and rebaked by edits, not by initial
  streaming. A freshly loaded, unmodified world may have no regions yet.
- `agent_max_slope` and `agent_max_climb` are exported on `VoxelTerrain3D`.
- The demo drives a cyan capsule along the returned path, applies gravity when the ground
  is carved away, and re-queries from the landed position.

**Read more**

- [AI Companion & Navigation](docs/AI%20Companion%20%26%20Navigation.md) - mental model, lifecycle, group geometry, gotchas
- [Design v4: Class mapping](docs/Voxel%20Design%20Doc%20v4.md)
- [`include/godot_wrapper_and_nav3d.hpp`](include/godot_wrapper_and_nav3d.hpp)
- [`src/voxel_ai_companion.cpp`](src/voxel_ai_companion.cpp)

### Persistence and region files

Chunks are persisted in two stages: homogeneous chunks are elided, dense chunks are split
into byte planes and RLE-compressed, and regions are written atomically as `8 x 8 x 8`
chunk files (up to 512 chunks per region). A small in-memory blob cache avoids re-reading
a region for every chunk.

```gdscript
$VoxelTerrain3D.save_world()
```

Saving normally happens on exit and through background streaming eviction. The save path
is the `save_directory_path` field in `src/voxel_terrain_3d.h` (default
`/tmp/voxel_world_saves`). Worlds at different `voxel_scale` values must not share a save
directory because chunk indices are resolution-relative.

**Read more**

- [Design v4: Two-stage persistence and 8x8x8 regions](docs/Voxel%20Design%20Doc%20v4.md)
- [`include/persistence_and_streaming.hpp`](include/persistence_and_streaming.hpp)
- [Voxel size guide: save directory](voxel_size_configuration_guide.md#4-save-directory)
- [`tests/test_persistence_and_streaming.cpp`](tests/test_persistence_and_streaming.cpp)

### Asset baking and stamping

The standalone `voxel_baker` CLI converts a Wavefront `.obj` into a native `.vreg` region
file using a narrow-band TSDF. The result can carry per-voxel color sampled from a PPM
albedo (RGB5551, outer skin only). At runtime, `stamp_asset()` unions the region into the
live world, where it is carved with normal voxel physics.

Demo model textures (Ape, grenade, pickaxe, and TNT) import with S3TC/BPTC VRAM
compression (`compress/mode=2`) so prop and statue surfaces stay compressed on the GPU.
Baked `.vreg` albedo is separate and remains RGB5551 in the voxel data.

```bash
voxel_baker model.obj model.vreg \
  --voxel-scale 0.5 \
  --watertight \
  --texture model_albedo.ppm \
  --color-depth 2.8 \
  --center-x 160 --center-y 160 --center-z 128
```

```gdscript
var structure_id := terrain.stamp_asset(
    "res://model.vreg",
    origin_chunk,
    15.0 # quiet period before reconstitution; 0.0 keeps it destroyed
)
```

`AssetStampManager` snapshots the pre-stamp and post-stamp state of every touched chunk.
When an explosion or elemental sphere dirties a stamped structure, the structure is marked
damaged and its timer restarts. When the quiet period elapses, the exact baseline TSDF,
materials, and colors are restored. `remove_structure(id)` restores the pre-stamp world
immediately.

| Signal | Payload |
| --- | --- |
| `structure_damaged` | `structure_id`, `name` |
| `structure_recreated` | `structure_id`, `name` |
| `structure_removed` | `structure_id`, `name` |

The demo shows three statue pairs (Easter Island, Atlas, Ape), each with a mesh-swap
option and a baked-voxel-color option. Press `R` to reset all six statues, or
`Shift + R` to undo every prior terrain edit and return the whole world to its baseline.

**Read more**

- [Asset stamping and reconstitution](docs/asset%20stamping%20and%20reconstitution.md) - baker options, blending, C++ API, testing, troubleshooting
- [`include/voxel_mesh_baker.hpp`](include/voxel_mesh_baker.hpp)
- [`include/asset_stamp_manager.hpp`](include/asset_stamp_manager.hpp)
- [Model detail guide for 0.5 m voxels](voxel_05_model_prompt_and_detail_guide.md)
- [`models/easter-island/README.md`](models/easter-island/README.md), [`models/atlas/README.md`](models/atlas/README.md), [`models/ape/README.md`](models/ape/README.md)

### Undo and redo

Every recorded action stores a bit-exact snapshot of each touched chunk before and after the
edit: storage form, base material, 16-bit TSDF, 8-bit material, 8-bit persistent crack
damage, and 16-bit RGB5551 color planes. Stamped structures also capture the
`AssetStampManager` registry, so undo/redo restores both the voxels and the structure
bookkeeping.

The history covers every phased CSG and elemental edit path, `stamp_asset()`, and
`remove_structure()`. Restoring a chunk marks it resident and dirty, increments its version,
and republishes edit/navigation dirty batches. In-flight worker meshes whose captured terrain
revision is older than a completed synchronous undo/redo are discarded, so a stale mesh can
never overwrite a restored chunk.

| Method | Returns | Behavior |
| --- | --- | --- |
| `undo()` | `bool` | Restore the most recent action. |
| `redo()` | `bool` | Re-apply the most recently undone action. |
| `undo_all()` | `int` | Coalesce the whole undo stack into one restore pass and one remesh batch; returns the number of actions reverted. |
| `redo_all()` | `int` | Coalesce the whole redo stack into one re-apply pass; returns the number of actions re-applied. |
| `clear_undo_history()` | `void` | Drain pending edits, then clear both stacks. |
| `can_undo()` / `can_redo()` | `bool` | Stack availability. |
| `get_undo_count()` / `get_redo_count()` | `int` | Current stack depths. |
| `set_max_undo_steps(steps)` / `get_max_undo_steps()` | `void` / `int` | History depth, clamped to the `max_undo_steps` property. |

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `max_undo_steps` | `int` | `64` | Range 1-512. Lowering it trims the oldest undo entries. |

```gdscript
var terrain := $VoxelTerrain3D

terrain.trigger_explosion(Vector3(16, 70, 16), 8.5)
if terrain.can_undo():
    terrain.undo()

# Reset every edit and stamp in one coalesced pass.
var reverted := terrain.undo_all()
print("Reverted ", reverted, " actions; redo depth = ", terrain.get_redo_count())

# Lock the current world in as the new baseline.
terrain.clear_undo_history()
```

Signals:

| Signal | Payload |
| --- | --- |
| `undo_completed` | `restored_chunks`, `remaining_undo`, `available_redo` |
| `redo_completed` | `restored_chunks`, `available_undo`, `remaining_redo` |

The demo groups a continuous drag stroke, a pickaxe swing, and an Option 1 mesh-to-voxel swap
with the explosion it triggers into one logical history action, so a single `Z` reverts the
whole gesture.

**Read more**

- [`include/asset_stamp_manager.hpp`](include/asset_stamp_manager.hpp) - `VoxelUndoRedoManager` and structure snapshots
- [`include/edit_and_worker_pool.hpp`](include/edit_and_worker_pool.hpp) - `ChunkStateSnapshot` and edit capture
- [`src/voxel_terrain_3d.cpp`](src/voxel_terrain_3d.cpp) - binding, drain, and signal wiring
- [`tests/test_gdextension_bindings.cpp`](tests/test_gdextension_bindings.cpp) - undo/redo/undo_all/redo_all regression suite

### Physics collision

Each resident chunk creates a concave `PhysicsServer3D` shape directly, without a
`StaticBody3D` node. Two properties keep physics cost down:

| Property | Default | Effect |
| --- | --- | --- |
| `collision_distance_chunks` | `-1` | Build collision only within this 3D Chebyshev radius of the focus chunk. `-1` collides every resident chunk. |
| `collision_subsample_stride` | `1` | `2` builds collision and navigation from a `16^3` stride-2 subsample, roughly 4x fewer physics triangles. The visual mesh stays full resolution. |

```gdscript
$VoxelTerrain3D.collision_distance_chunks = 3
$VoxelTerrain3D.collision_subsample_stride = 2
```

Collision gating only affects the physics BVH. Voxel CSG edits and terrain raycasts still
work at any distance.

**Read more**

- [Design v4: Physics collision](docs/Voxel%20Design%20Doc%20v4.md#44-physics-collision)
- [Performance analysis: main-thread and memory budgets](docs/Voxel%20Performance%20Analysis.md)
- [AI & navigation: collision gating is independent of navigation](docs/AI%20Companion%20%26%20Navigation.md#8-gotchas-quirks--limitations)

### Triplanar shading

`voxel_triplanar.gdshader` blends world-space triplanar material colors by slope, adds
procedural noise detail, mixes in per-voxel baked albedo, and computes chiseled facet
normals for hard rock and painted stone. It supports grass, soil, rock, hard rock, boulder,
lava, water, obsidian, and ice, plus progressive Voronoi crack glow for damaged rock.

Shader uniforms include:

- `grass_color`, `dirt_color`, `rock_color`, `hard_rock_color`, `boulder_color`
- `lava_color`, `water_color`, `obsidian_color`, `ice_color`, `crack_glow_color`
- `slope_sharpness`, `noise_scale`, `chisel_normal_strength`
- `debug_damage_indicator`

The surface-to-rock height blend currently uses a world-Y band hard-coded in the shader
(`45..65 m`). If you move the terrain surface substantially, adjust that band or the
matching depth settings described in the terrain-depth guide.

**Read more**

- [`voxel_triplanar.gdshader`](voxel_triplanar.gdshader)
- [Terrain depth guide](docs/terrain%20depth.md)
- [Asset stamping: baked albedo in `COLOR.a`, `UV`, and `UV2`](docs/asset%20stamping%20and%20reconstitution.md#baked-albedo-in-the-mesh-colora--uv--uv2)

### Performance instrumentation

Every engine stage records into a lock-free accumulator. Worker threads write; the main
thread drains once per frame and prints one summary line for frames that did work.

| Stage | Measures |
| --- | --- |
| `generate` | Per-chunk procedural generation and disk load |
| `mesh` | 36^3 gather plus Surface Nets meshing |
| `commit` | Main-thread mesh upload, collision, and navigation submission |
| `stream_focus` | Streaming focus update |
| `edit` | CSG sphere edit execution |
| `navigation` | Navigation path and distance queries |

```text
[voxel] generate x12 total 4.53ms max 0.62ms avg 0.38ms | mesh x8 total 9.10ms max 2.30ms avg 1.14ms | commit x8 total 1.20ms max 0.31ms avg 0.15ms
[voxel] initial fill complete in 1840.2 ms
```

Disable logging per node:

```gdscript
$VoxelTerrain3D.instrumentation_enabled = false
```

**Read more**

- [Performance analysis](docs/Voxel%20Performance%20Analysis.md) - measured baselines, P0/P1/P2 findings, roadmap, measurement ledger
- [`include/stage_timings.hpp`](include/stage_timings.hpp)
- [`tests/test_stage_timings.cpp`](tests/test_stage_timings.cpp)

### Automated benchmark suite

The demo includes a five-preset benchmark suite that exercises streaming, CSG destruction,
and 3D navigation under different residency settings. Press `M` or `F8` to start it; press
either key again to abort and still write a partial report. The suite disables VSync for
the run, then restores the terrain settings, camera pose, VSync mode, and all six demo
statues.

Before every preset pass, the suite calls `undo_all()` and clears the history so each run
starts from the same pristine world: the 3-hit Hard Rock boulder, Molten Lava dome, stamped
statues, and zero prior craters or Frost ice bridges. This removes cross-preset
contamination from the FPS, CSG latency, and navigation measurements.

It can also start automatically from the command line:

```powershell
# Run the suite and leave the demo open when it finishes
godot --path . -- --benchmark

# Run the suite and quit after writing the reports (useful for CI)
godot --path . -- --benchmark-quit
```

Each preset runs four phases:

| Phase | Duration | What it measures |
| --- | --- | --- |
| Settle & warmup | Up to 3.5 s | Waits for streaming and commits to become fully idle. |
| Stream flight | 4.5 s | S-curve sweep down the statue corridor while chunks stream. |
| Destruction & CSG | 4.0 s | Eight deterministic explosion, hard-rock, water, frost, and large-blast edits. |
| 3D nav pathing | 2.5 s | Waits for navigation groups to sync, then times a path query and waypoint count. |

| Preset | View | Prefetch | Upload budget | Collision distance | Memory budget |
| --- | ---: | ---: | ---: | ---: | ---: |
| Low / Fast Stream | 6 | 1 | 1.5 ms | 2 | 512 MB |
| Balanced Default | 8 | 2 | 2.5 ms | 4 | 1024 MB |
| High Throughput Upload | 10 | 3 | 5.0 ms | 4 | 1536 MB |
| Extended Horizon | 14 | 4 | 6.0 ms | 6 | 2048 MB |
| Full Collision Stress | 12 | 2 | 8.0 ms | 10 | 2048 MB |

The report records CPU/GPU/OS/RAM details, average and 1% low FPS, p50/p95/p99/max frame
times, streaming commit rate, blast latency, navigation sync/query time, and peak VRAM,
RAM, draw calls, and primitives. It also records start/completion timestamps in local and
UTC time, Unix timestamps, total suite duration, and per-preset duration plus the terrain
reset method. It writes both formats to the user data directory:

- `user://voxel_benchmark_report.md`
- `user://voxel_benchmark_results.json`
- `user://voxel_benchmark_report_<YYYY-MM-DD_HH-MM-SS>.md`
- `user://voxel_benchmark_results_<YYYY-MM-DD_HH-MM-SS>.json`

In editor runs, copies are also written to `res://voxel_benchmark_report.md` and
`res://voxel_benchmark_results.json`. Exported builds treat `res://` as read-only, so read
the files from the Godot user data directory instead. The timestamped archives are only
written under `user://`, so repeated benchmark runs never overwrite earlier results.

---

## Node and API reference

### `VoxelTerrain3D`

Exported properties:

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `view_distance_chunks` | `int` | `2` | Range 1-24. |
| `prefetch_distance_chunks` | `int` | `1` | Range 0-8. |
| `collision_distance_chunks` | `int` | `-1` | Range -1-24; `-1` disables gating. |
| `collision_subsample_stride` | `int` | `1` | Clamped to 1 or 2. |
| `resident_memory_budget_mb` | `int` | `512` | Range 128-4096 MB. |
| `upload_budget_ms` | `float` | `2.0` | Range 0.5-16 ms. |
| `world_height_chunks` | `int` | `4` | Range 1-12. Baked in at startup. |
| `voxel_scale` | `float` | `1.0` | Range 0.125-4.0. Set before entering the tree. |
| `surface_base_height` | `float` | `68.0` | Range 16-256 m. |
| `world_seed` | `int` | `1337` | Range 0-4294967295. |
| `terrain_material` | `Material` | `null` | Applied to committed chunk surfaces. |
| `generate_caves` | `bool` | `true` | Read when chunks are generated. |
| `agent_max_slope` | `float` | `70.0` | Navigation slope limit in degrees. |
| `agent_max_climb` | `float` | `1.5` | Navigation step height in meters. |
| `instrumentation_enabled` | `bool` | `true` | Per-frame stage logging. |
| `max_undo_steps` | `int` | `64` | Range 1-512. Maximum retained undo actions. |

Runtime methods:

| Method | Returns | Purpose |
| --- | --- | --- |
| `trigger_explosion(position, radius)` | `void` | Asynchronous sphere carve. |
| `trigger_elemental_sphere(position, radius, kind)` | `void` | Water (`1`), frost (`2`), or lava (`3`) reaction. |
| `add_terrain(position, radius)` | `void` | Add soil. |
| `add_terrain_with_material(position, radius, material)` | `void` | Add with an explicit material ID. |
| `place_boulder(position, radius)` | `void` | Add boulder material. |
| `stamp_asset(vreg_path, chunk_coords, recreate_delay_seconds)` | `int` | Stamp a `.vreg`; returns `structure_id`, or 0 on failure. |
| `remove_structure(structure_id)` | `bool` | Restore the pre-stamp world. |
| `raycast_terrain(from, to)` | `Vector3` | Surface hit, or `+INF` on miss. |
| `raycast_terrain_material(from, to)` | `int` | Material ID, or -1 on miss. |
| `get_surface_height(x, z)` | `float` | Top surface height at world X/Z. |
| `undo()` / `redo()` | `bool` | Revert or re-apply one history action. |
| `undo_all()` / `redo_all()` | `int` | Coalesced full-stack revert or re-apply. |
| `clear_undo_history()` | `void` | Clear both history stacks. |
| `can_undo()` / `can_redo()` | `bool` | Stack availability. |
| `get_undo_count()` / `get_redo_count()` | `int` | Current stack depths. |
| `set_max_undo_steps(steps)` / `get_max_undo_steps()` | `void` / `int` | History depth accessors. |
| `save_world()` | `void` | Flush dirty regions. |
| `set_stream_focus(position, velocity)` | `void` | Move the streaming focus. |
| `core_idle()` | `bool` | True when the worker pool is idle. |
| `fully_committed()` | `bool` | True when meshes, completion queue, and navigation are settled. |
| `get_effective_view_distance_chunks()` | `int` | Current radius after memory-pressure reduction. |

Signals: `explosion_requested`, `explosion_completed`, `explosion_failed`,
`terrain_add_requested`, `terrain_add_completed`, `terrain_add_failed`,
`chunk_committed`, `structure_damaged`, `structure_recreated`, `structure_removed`,
`undo_completed`, `redo_completed`.

### `VoxelAICompanion`

| Method | Returns | Purpose |
| --- | --- | --- |
| `get_voxel_path(start, end)` | `PackedVector3Array` | Query the 3D navigation map. |
| `is_path_region_synced(position)` | `bool` | Check whether the containing navigation group is baked and current. |
| `set_terrain_node(terrain)` | `void` | C++-only terrain link; not bound to GDScript. |

The node is a query facade, not a movement controller. Gameplay code consumes the
waypoints. See the [AI Companion & Navigation](docs/AI%20Companion%20%26%20Navigation.md)
document for lifecycle details and known limitations.

---

## Demo scene and controls

Open `project.godot` in Godot 4.2+ and run `voxel_terrain_demo.tscn`. For cave-free terrain,
toggle `generate_caves` off in the Inspector or at runtime; the separate no-caves scene was
removed.

### Controls

| Input | Action |
| --- | --- |
| `F` | Toggle Fly / Play mode |
| `M` / `F8` | Start or abort the automated benchmark suite |
| `W` / `A` / `S` / `D` | Move (fly) or run (play) |
| `Space` / `E` | Fly up; jump in Play mode |
| `Ctrl` / `Q` | Fly down |
| `Shift` | Sprint / speed boost |
| Mouse | Look |
| `1` | Add terrain |
| `2` | Paint terrain (hold and drag) |
| `3` | Paint delete (hold and drag) |
| `4` | Place boulder |
| `5` | Small hit chip blast |
| `6` | Explode |
| `7` | Large explode |
| `8` | Water quench |
| `9` | Frost / ice bridge |
| `0` | Lava / magma |
| `B` | Stamp configured asset with reconstitution timer |
| `Shift + B` | Stamp configured asset permanently |
| `R` | Reset all six demo statues |
| `Z` / `Ctrl + Z` | Undo the last edit, stroke, stamp, or statue swap |
| `Y` / `Ctrl + Y` / `Ctrl + Shift + Z` | Redo the last undone action |
| `Shift + R` / `Ctrl + R` | Undo every terrain edit and restore all statues to the pristine baseline |
| `T` | Toggle add/paint material (soil / hard rock) |
| `P` | Place the companion at the crosshair |
| `Right Click` | Query a 3D cave path to the crosshair |
| `Shift + Right Click` | Place the companion and query a path |
| `Left Click` (Fly) | Apply the active terrain tool |
| `Left Click` (Play) | Swing the pickaxe and chip the terrain; hold for repeated swings |
| `H` | Show or hide the controls overlay |
| `U` | Hide all UI except the reticle and pickaxe; press again to restore |
| `Escape` | Release mouse capture; click to recapture |

The HUD also has a **Hide UI (U)** button in the top-right corner. It toggles the same
clean-screen mode as `U`: the stats label, controls overlay, world `Label3D` guidance, and
the button itself disappear, while the targeting reticle and the pickaxe viewmodel stay
visible. Because the mouse is captured during play, either press `U` or press `Escape`
first if you prefer to click the button.

### Terrain tool tuning

Tool radii and pickaxe tuning are exported on the demo controller and editable live in the
Inspector:

| Export | Default | Used by |
| --- | --- | --- |
| `add_radius` | `6.0` | Add terrain |
| `paint_radius` | `3.5` | Paint terrain |
| `paint_delete_radius` | `3.5` | Paint delete |
| `boulder_radius` | `4.0` | Place boulder |
| `small_hit_radius` | `1.5` | Small hit |
| `explosion_radius` | `8.5` | Explode |
| `large_explosion_radius` | `18.0` | Large explode |
| `elemental_radius` | `8.5` | Water / frost / lava |
| `brush_spacing_factor` | `0.6` | Drag-brush step distance |
| `brush_interval` | `0.05` | Minimum seconds between drag-brush steps |
| `max_ray_distance` | `250.0` | Crosshair raycast length |
| `pickaxe_hit_radius` | `1.5` | Play-mode pickaxe chip radius |
| `pickaxe_impact_fraction` | `0.42` | Swing point where the chip lands |

### First-person pickaxe viewmodel

Play mode uses a procedurally built arm and pickaxe anchored to the camera; it has no
external model or rig dependency. Shoulder, elbow, and wrist pivots drive a windup, strike,
follow-through, and solid-hit recoil, with idle/walk bob, mouse sway, emissive metal
accents, and impact sparks. Hold left mouse to keep swinging; each chip lands at
`pickaxe_impact_fraction` of the 0.55 s swing.

### Companion behavior

The cyan capsule is created by `terrain_demo.gd`. The first right-click places it; later
right-clicks set a goal. When the world is `fully_committed()` and both endpoint
navigation groups are synced, the demo queries a path, drapes it onto the terrain surface,
draws it as a green ribbon, and walks the capsule along it. If terrain is carved out from
under the capsule, gravity drops it into the crater and the path is re-queried.

| Export | Default | Purpose |
| --- | --- | --- |
| `companion_walk_speed` | `10.0` | Horizontal walking speed along waypoints. |
| `companion_gravity` | `28.0` | Downward acceleration when ground is removed. |
| `boom_snap_depth_factor` | `0.5` | Depth tolerance for stacking repeated blasts on the same spot. |
| `boom_snap_lateral` | `0.75` | Lateral tolerance for repeating the same blast spot. |

### Statue pairs

The demo seeds three side-by-side statue pairs:

| Statue | Position | Style A | Style B |
| --- | --- | --- | --- |
| Easter Island | `z = 0` | Pristine textured mesh swaps to voxels on first blast | Stamped `.vreg` with baked RGB5551 albedo |
| Atlas | `z = -64` | Pristine textured mesh swaps to voxels on first blast | Stamped `.vreg` with baked RGB5551 albedo |
| Ape | `z = -128` | Pristine textured mesh swaps to voxels on first blast | Stamped `.vreg` with baked RGB5551 albedo |

Each baked statue uses 3-hit hard-rock damage, exposes raw rock in crater interiors, and
reconstitutes after 15 seconds of quiet. `R` resets all six.

**Read more**

- [Asset stamping: demo usage](docs/asset%20stamping%20and%20reconstitution.md#4-using-it-in-the-demo)
- [Model detail guide](voxel_05_model_prompt_and_detail_guide.md)
- [`terrain_demo.gd`](terrain_demo.gd)

---

## Building from source

### 1. Clone `godot-cpp`

Use the branch that matches your Godot engine version. `godot-cpp` must be built with the
same compiler and toolchain as this project.

```bash
git clone --recurse-submodules -b 4.2 https://github.com/godotengine/godot-cpp.git ../godot-cpp
```

Replace `4.2` with the branch matching your engine (for example `4.7`).

### 2. Configure the GDExtension build

`GODOTCPP_TARGET` selects the godot-cpp template linked into the library. Use
`template_debug` (the default) for editor development and `template_release` for a genuine
release extension.

```bash
cmake -B build-gdext \
  -DVOXEL_BUILD_GDEXTENSION=ON \
  -DGODOT_CPP_DIR=/path/to/godot-cpp
```

For the release slot, use a separate directory:

```bash
cmake -B build-release \
  -DVOXEL_BUILD_GDEXTENSION=ON \
  -DGODOTCPP_TARGET=template_release \
  -DGODOT_CPP_DIR=/path/to/godot-cpp
```

### 3. Build

```bash
# Ninja / Makefiles (single-config)
cmake --build build-gdext -j

# Visual Studio / Xcode (multi-config)
cmake --build build-gdext --config Release -j
```

### Build artifacts

CMake copies the library into `bin/` under the filename `voxel_engine.gdextension`
expects:

| Platform | Filename |
| --- | --- |
| Windows | `bin/libvoxel_engine.windows.<debug\|release>.x86_64.dll` |
| Linux | `bin/libvoxel_engine.linux.<debug\|release>.x86_64.so` |
| macOS | `bin/libvoxel_engine.macos.<debug\|release>.dylib` |

The `<debug|release>` suffix follows the CMake build configuration. The extension
descriptor maps both editor slots to the release DLL on Windows by default; see
[Running the Production Demo](running-prod.md) before changing that mapping.

### CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `VOXEL_BUILD_TESTS` | `ON` | Build the CTest suites. |
| `VOXEL_BUILD_GDEXTENSION` | `OFF` | Build the Godot shared library. Requires `godot-cpp`. |
| `VOXEL_ENABLE_ASAN` | `OFF` | AddressSanitizer plus UndefinedBehaviorSanitizer (non-MSVC). |
| `VOXEL_ENABLE_TSAN` | `OFF` | ThreadSanitizer (non-MSVC). |

### Build modes

| Use case | `GODOTCPP_TARGET` | CMake configuration |
| --- | --- | --- |
| Running and debugging in the Godot editor | `template_debug` (default) | `Release` (optimized) or `Debug` |
| Exporting a release game | `template_release` | `Release` |

Different `GODOTCPP_TARGET` values need separate configured build directories.

---

## Tests and sanitizers

The C++ core, mesher, persistence, raycasts, asset baker, worker pool, and GDExtension
bindings each have a dedicated suite under `tests/`.

### CMake and CTest

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVOXEL_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

With a multi-config generator:

```bash
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
```

### Sanitizers

```bash
# AddressSanitizer and UndefinedBehaviorSanitizer
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVOXEL_ENABLE_ASAN=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# ThreadSanitizer
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVOXEL_ENABLE_TSAN=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### All suites script

On Linux, macOS, or WSL:

```bash
chmod +x run_all_tests.sh
./run_all_tests.sh
```

The script runs all milestone suites under ASan and TSan and also builds the standalone
`voxel_baker` CLI.

### Test suites

| Suite | Covers |
| --- | --- |
| `test_chunk_key` | Packed chunk coordinates and spatial hashing |
| `test_chunk_store` | TSDF storage, compact forms, memory ledger |
| `test_stage_timings` | Lock-free instrumentation accumulator |
| `test_scratch_and_dirty` | 36^3 scratch windows and dirty tracking |
| `test_edit_and_worker_pool` | Worker scheduling, phased edits, de-crumb |
| `test_procgen_and_surface_nets` | Terrain generation and seam-free meshing |
| `test_terrain_raycast` | Voxel-space rays and surface-height queries |
| `test_offline_asset_baker` | OBJ loading, `.vreg` baking, colors, reconstitution |
| `test_persistence_and_streaming` | Region files, compression, streaming focus |
| `test_integration_m3_m4` | Collision gating, navigation groups, bounded commits |
| `test_gdextension_bindings` | Class registration, signals, stamping, lifecycle, undo/redo, coalesced benchmark reset |

**Read more**

- [Design v4: Verification strategy](docs/Voxel%20Design%20Doc%20v4.md)
- [AI & navigation: verification map](docs/AI%20Companion%20%26%20Navigation.md#9-verification-map)
- [Asset stamping: testing](docs/asset%20stamping%20and%20reconstitution.md#6-testing)

---

## Production builds

A production run has two independent release layers:

1. The native GDExtension library, compiled optimized and linked against godot-cpp's
   release template.
2. The exported project, bundled by a release export preset.

Running from the editor is not the same as a production build.

```powershell
cmake -B build-release `
  -DVOXEL_BUILD_GDEXTENSION=ON `
  -DGODOTCPP_TARGET=template_release `
  -DGODOT_CPP_DIR=C:/path/to/godot-cpp `
  -DVOXEL_BUILD_TESTS=OFF

cmake --build build-release --config Release
```

Then install matching export templates and export the project. `run-prod.ps1` automates
the check-build-launch loop on Windows.

**Read more**

- [Running the Production (Release) Demo](running-prod.md) - prerequisites, export presets, which library Godot loads, troubleshooting
- [Design v4: Build and deployment](docs/Voxel%20Design%20Doc%20v4.md)

---

## Voxel scale, depth, and model detail

These three guides cover the settings that most often need hand tuning:

| Guide | Use it for |
| --- | --- |
| [Voxel Size (`voxel_scale`) Usage Guide](voxel_size_configuration_guide.md) | Changing physical voxel size, world height, view distance, memory budget, save directories, and re-baking assets. |
| [Terrain Depth](docs/terrain%20depth.md) | Choosing `world_height_chunks`, `surface_base_height`, shader threshold, and camera height. |
| [0.5 m Voxel Scale Model Detail Guide](voxel_05_model_prompt_and_detail_guide.md) | Minimum feature sizes, image-to-3D prompts, and baking generated models at 0.5 m. |

Quick rules:

- A chunk is always `32^3` samples; its world size is `32 * voxel_scale` meters.
- World height is `world_height_chunks * 32 * voxel_scale` meters.
- Keep `surface_base_height + surface_amplitude` at least 20 m below the ceiling.
- Halving `voxel_scale` needs roughly 8x the chunk count for the same volume.
- Re-bake `.vreg` assets for the scale you stamp them into.

---

## Project structure

```text
voxel/
|-- bin/                          Compiled GDExtension libraries and .gdignore
|-- build/                        CMake build tree and .gdignore
|-- docs/                         Design, navigation, stamping, depth, and performance docs
|-- include/                      Pure C++20 voxel core (no Godot headers)
|   |-- asset_stamp_manager.hpp   .vreg stamping, damage, reconstitution, undo/redo
|   |-- chunk_key.hpp             Packed chunk coordinates and spatial hashing
|   |-- chunk_store.hpp           TSDF chunk storage, compact chunks, memory ledger
|   |-- edit_and_worker_pool.hpp  Worker pool, phased CSG edits, chunk snapshots
|   |-- godot_wrapper_and_nav3d.hpp Navigation groups and Godot wrapper contracts
|   |-- persistence_and_streaming.hpp Region files, compression, streaming
|   |-- procgen_and_surface_nets.hpp  Terrain noise and Surface Nets mesher
|   |-- scratch_and_dirty.hpp     36^3 scratch windows and dirty scheduling
|   |-- stage_timings.hpp         Lock-free performance instrumentation
|   |-- terrain_raycast.hpp       Voxel-space raycasts
|   `-- voxel_mesh_baker.hpp      Offline OBJ -> .vreg voxelizer
|-- models/                       Demo statues and prop models
|   |-- ape/                      Ape GLB, OBJ, albedo, baked .vreg, README
|   |-- atlas/                    Atlas GLB, OBJ, albedo, baked .vreg, README
|   |-- easter-island/            Easter Island GLB, OBJ, albedo, baked .vreg, README
|   |-- grenade/                  Prop model
|   |-- pickaxe/                  Prop model
|   `-- tnt/                      Prop model
|-- src/                          Godot 4 GDExtension bindings
|   |-- register_types.cpp/.h     Entry point and class registration
|   |-- voxel_ai_companion.cpp/.h VoxelAICompanion query facade
|   |-- voxel_baker_main.cpp      voxel_baker CLI entry point
|   `-- voxel_terrain_3d.cpp/.h   VoxelTerrain3D node
|-- tests/                        CTest unit and integration suites
|   `-- assets/                   Baked test fixtures, including `moai_statue.vreg`
|-- addons/                       godot_ai and phantom_camera editor addons
|-- CMakeLists.txt                Build configuration
|-- project.godot                 Godot project definition
|-- run_all_tests.sh              ASan/TSan test runner
|-- run-prod.ps1                  Release library build and launch helper
|-- running-prod.md               Production build guide
|-- terrain_demo.gd               Demo controller, benchmark suite, and pickaxe viewmodel
|-- voxel_engine.gdextension      GDExtension library mapping
|-- voxel_terrain_demo.tscn       Main demo scene (caves enabled)
`-- voxel_triplanar.gdshader      Material-aware triplanar terrain shader
```

---

## Documentation index

### Current architecture and feature guides

| Document | What it covers |
| --- | --- |
| [Voxel Design Doc v4](docs/Voxel%20Design%20Doc%20v4.md) | Current architecture: chunk store, concurrency, streaming, worker pool, persistence, GDExtension wrapper, physics, navigation, build, verification, milestones. Start here for a deep technical read. |
| [AI Companion & Navigation](docs/AI%20Companion%20%26%20Navigation.md) | `VoxelAICompanion` API, 4x2x4 navigation groups, revision/epoch model, bake lifecycle, cave-tuned NavMesh parameters, demo usage, gotchas, verification map. |
| [Asset Stamping and Reconstitution](docs/asset%20stamping%20and%20reconstitution.md) | `voxel_baker` options, narrow-band TSDF baking, RGB5551 color, Union/Overwrite blending, damage tracking, C++ API, tests, troubleshooting. |
| [Voxel Performance Analysis](docs/Voxel%20Performance%20Analysis.md) | Measured baselines, P0/P1/P2 findings, fixed/open status, verification plan, measurement ledger, repo hygiene. |
| [Terrain Depth](docs/terrain%20depth.md) | Depth profiles and the interaction between `surface_base_height`, `world_height_chunks`, shader threshold, and camera height. |
| [Voxel Size (`voxel_scale`) Usage Guide](voxel_size_configuration_guide.md) | Scale selection, chunk math, memory and view-distance implications, stamp-origin math, troubleshooting. |
| [0.5 m Model Detail Guide](voxel_05_model_prompt_and_detail_guide.md) | Nyquist limits for TSDF voxelization, feature-size table, image-to-3D prompts, baking recipe. |
| [Running the Production Demo](running-prod.md) | Release-slot library, export templates, export presets, `run-prod.ps1`, library slot resolution, troubleshooting. |

### Historical design documents

These are kept for provenance. Use v4 for current behavior.

| Document | Status |
| --- | --- |
| [Voxel Design Doc v2](docs/Voxel%20Design%20Doc%20v2.md) | Historical baseline. |
| [Voxel Design Doc v2.1 - Streaming](docs/Voxel%20Design%20Doc%20v2.1%20-%20Streaming.md) | Historical streaming revision. |
| [Voxel Design Doc v3 - Streaming](docs/Voxel%20Design%20Doc%20v3%20-%20Streaming.md) | Historical streaming revision. |

### Supporting documents

| Document | Contents |
| --- | --- |
| [`TODO.md`](TODO.md) | Architecture comparison notes and ideas under consideration. |
| [`models/easter-island/README.md`](models/easter-island/README.md) | Easter Island asset provenance and baking notes. |
| [`models/atlas/README.md`](models/atlas/README.md) | Atlas asset provenance and baking notes. |
| [`models/ape/README.md`](models/ape/README.md) | Ape asset decimation and baking notes. |
| [`addons/godot_ai/README.md`](addons/godot_ai/README.md) | Editor AI integration addon documentation. |

---

## Troubleshooting

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Godot cannot find `VoxelTerrain3D` | Extension library missing or wrong slot | Build the GDExtension and check `bin/` and `voxel_engine.gdextension`; see [production guide](running-prod.md#which-library-godot-loads). |
| Library fails to load after a Godot upgrade | `godot-cpp` does not match the engine | Rebuild `godot-cpp` on the matching branch and rebuild the extension. |
| Terrain is cut off at the top or bottom | World ceiling too low | Raise `world_height_chunks` before the node enters the tree; see [terrain depth](docs/terrain%20depth.md). |
| `voxel_scale` change has no effect | Set after the node entered the tree | Set it before `add_child()` or in the Inspector, then reload the scene. |
| Effective view distance drops | Memory budget exhausted | Raise `resident_memory_budget_mb` or lower view/height chunks; see [voxel size guide](voxel_size_configuration_guide.md#3-memory-budget). |
| Low frame rate while flying | Streaming or commit cost too high | Lower `view_distance_chunks`, set `collision_distance_chunks`, use `collision_subsample_stride = 2`, and inspect stage timings; see [performance analysis](docs/Voxel%20Performance%20Analysis.md). |
| Navigation path is empty | Groups not baked or not synced | Check `is_path_region_synced()` and `fully_committed()`; see [navigation gotchas](docs/AI%20Companion%20%26%20Navigation.md#8-gotchas-quirks--limitations). |
| Stamped asset is too small or offset | Asset baked at a different scale or stamp origin wrong | Re-bake at the matching `--voxel-scale` and use the [stamp-origin formula](voxel_size_configuration_guide.md#compute-the-stamp-origin-chunk). |
| Stamped asset never returns | Reconstitution timer disabled | Pass a positive `recreate_delay_seconds`; `0.0` means permanently destroyed. |
| Crater interiors show stretched texture | Baked color depth too deep | Lower `--color-depth`; see [asset stamping](docs/asset%20stamping%20and%20reconstitution.md#7-troubleshooting). |
| Craters look blockier than expected | Voxel resolution too coarse | Lower `voxel_scale` and re-bake assets; see [voxel size guide](voxel_size_configuration_guide.md#choosing-a-scale). |
| Benchmark reports are missing in an exported build | `res://` is read-only in exports | Read `user://voxel_benchmark_report.md` and `user://voxel_benchmark_results.json` from the Godot user data directory; see [automated benchmark suite](#automated-benchmark-suite). |
| `undo()` / `redo()` missing or "Nothing to undo" | Loaded GDExtension predates the undo bindings, or history was cleared | Rebuild the extension and confirm `bin/libvoxel_engine.windows.release.x86_64.dll` is current; see [undo and redo](#undo-and-redo). |
| Pickaxe viewmodel is invisible | It is only shown in Play mode | Press `F` to enter Play mode. |
| CMake cannot find `godot::cpp` | `GODOT_CPP_DIR` not set | Pass `-DGODOT_CPP_DIR=/path/to/godot-cpp` or add the godot-cpp subdirectory. |

---

## License

This project is licensed under the MIT License.
