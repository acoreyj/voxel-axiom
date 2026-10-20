# Voxel Engine v4.0

A high-performance, real-time destructible voxel engine with 3D cave generation and dynamic navigation, built with pure **C++20** and integrated as a **Godot 4.2+ GDExtension**.

---

## Features

- **C++20 Modular Architecture**: Pure decoupled core library (`voxel_core`) with zero Godot dependencies, statically linked into tests and the GDExtension module (`libvoxel_engine`).
- **Surface Nets Dual Contouring**: Seam-free multi-threaded meshing across chunk boundaries using a $36^3$ reconstruction window and central-difference normals.
- **Real-Time CSG Destruction**: Asynchronous spherical carving with expanded $R + \delta$ TSDF boundary evaluation and an anti-crumb kernel to prune isolated floating voxels.
- **Multi-Threaded Worker Pool**: Striped chunk locks, MPSC completion queue, and dual-priority scheduling (`dirty_edit_chunks` for explosions vs. `dirty_stream_chunks` for streaming).
- **Procedural 3D Cave Generation (optional)**: FastNoise SIMD uniform grid generation composed of 2D surface heightmaps and 3D dual-noise cave worms/swiss-cheese cavities. Set `generate_caves = false` on `VoxelTerrain3D` to skip the cave term entirely and stream cave-free terrain; the toggle is read at generation time, so it affects only chunks generated afterwards.
- **3D Cave Navigation**: Dynamic integration with Godot's `NavigationServer3D`, asynchronous navigation mesh baking, and synchronized path queries for AI companions.
- **World-Space Terrain Queries**: `VoxelTerrain3D.raycast_terrain()`, `raycast_terrain_material()` and `get_surface_height()` answer against the resident TSDF field (edited terrain first, procedural generator fallback), so aiming, spawns, and patrols work with no physics-collision dependency. `raycast_terrain_material()` additionally reports the material ID of the solid voxel behind the surface; the demo's top-left HUD shows it as the "Looking at" readout. The demo's `VoxelAICompanion` carries a visible pill capsule that patrols a triangular route on the terrain surface.
- **Crash-Safe 3D Region Persistence**: $8 \times 8 \times 8$ chunk region files (up to 512 chunks per region) with LZ4 byte-plane compression and background disk streaming.
- **Offline Asset Voxelization & Reconstitution**: The standalone `voxel_baker` CLI converts hand-crafted Wavefront `.obj` meshes (ruins, arches, walls — manifold or not) into native $8 \times 8 \times 8$ `.vreg` region files via a narrow-band TSDF; `VoxelTerrain3D.stamp_asset()` unions them into the live world, where they are destroyed with identical voxel physics and either reconstituted after a timer or kept permanently destroyed. Stamping can be `Union`ed into existing terrain and emits `structure_damaged` / `structure_recreated`.
- **Triplanar Voxel Shading**: Custom triplanar shader blending rock, soil, grass, and cave interior textures with normal mapping and roughness.
- **Built-in Performance Instrumentation**: Lock-free per-operation timing (`generate`, `mesh`, `commit`, `stream_focus`, `edit`, `navigation`) accumulated per frame and logged to the Godot **Output** panel. Toggle with the `instrumentation_enabled` property.

---

## Requirements

- **C++ Compiler**: C++20 compliant compiler
  - GCC 11+ / Clang 14+ (Linux / macOS)
  - MSVC 2022 (v143+) / Visual Studio 2022 (Windows)
- **CMake**: Version 3.22 or newer
- **Godot Engine**: Godot 4.2+ (tested up through Godot 4.7)
- **godot-cpp**: C++ bindings repository matching your Godot engine version

---

## Building the C++ GDExtension

Two independent choices decide what Godot runs:

- **Artifact filename** (`…debug…` vs `…release…`) follows the CMake build
  configuration: `--config Release` writes the `release` slot file, `--config
  Debug` the `debug` slot file.
- **Which slot Godot loads** is decided at runtime by the `debug`/`release`
  feature tag, resolved through `voxel_engine.gdextension`.

`GODOTCPP_TARGET` is separate again: it selects the godot-cpp template the
library links against. Use `template_release` for a genuine release extension;
`template_debug` (the default) is right for editor development.

| Use case | `GODOTCPP_TARGET` | CMake config |
| --- | --- | --- |
| Running / debugging in the Godot **editor** | `template_debug` (default) | `Release` (optimized) or `Debug` |
| Exporting a **release** game | `template_release` | `Release` |

Different `GODOTCPP_TARGET` values still need separate configured build
directories.

### 1. Clone `godot-cpp` (matching your Godot version)

```bash
git clone --recurse-submodules -b 4.2 https://github.com/godotengine/godot-cpp.git ../godot-cpp
```
*(Use the branch that matches your engine, e.g. `4.2` … `4.7`. godot-cpp must be
built with the same compiler/toolchain as this project.)*

### 2. Configure

Pass `-DVOXEL_BUILD_GDEXTENSION=ON` and the path to godot-cpp. The default
(`template_debug`) is what the editor loads, so it is what you want during
development:

```bash
cmake -B build \
  -DVOXEL_BUILD_GDEXTENSION=ON \
  -DGODOT_CPP_DIR=/path/to/godot-cpp
```

To also produce the release slot, configure a separate directory:

```bash
cmake -B build-release \
  -DVOXEL_BUILD_GDEXTENSION=ON \
  -DGODOTCPP_TARGET=template_release \
  -DGODOT_CPP_DIR=/path/to/godot-cpp
```

> **Single-config vs. multi-config generators.** With **Ninja** or **Makefiles**,
> `-DCMAKE_BUILD_TYPE=Debug|Release` selects the configuration. With the
> **Visual Studio** and **Xcode** generators that variable is ignored — pick the
> configuration at build time with `--config` (see below).

### 3. Build

```bash
# Ninja / Makefiles (single-config)
cmake --build build -j

# Visual Studio / Xcode (multi-config)
cmake --build build --config Release -j     # or: --config Debug
```

The `-j` flag builds in parallel; on multi-config generators it maps to
MSBuild's `/m`. To build tests only (no Godot dependency) see
[Running the Unit & Integration Tests](#running-the-unit--integration-tests).

#### Windows (PowerShell) example

```powershell
cmake -B build -DVOXEL_BUILD_GDEXTENSION=ON -DGODOT_CPP_DIR="../godot-cpp"
cmake --build build --config Release -j
```

Run these from a shell where `cmake` is on `PATH` — e.g. the **x64 Native Tools
Command Prompt for VS 2022**, or any shell with CMake 3.22+ installed. MSVC is
located automatically by the Visual Studio generator.

#### Build Artifacts

The post-build step copies the library into `bin/` under the name
`voxel_engine.gdextension` expects:

| Platform | Filename |
| --- | --- |
| Windows | `bin/libvoxel_engine.windows.<debug\|release>.x86_64.dll` |
| Linux | `bin/libvoxel_engine.linux.<debug\|release>.x86_64.so` |
| macOS | `bin/libvoxel_engine.macos.<debug\|release>.dylib` |

`<debug|release>` follows the CMake build configuration: `--config Release`
writes the `release` file and `--config Debug` the `debug` file. The filename is
independent of `GODOTCPP_TARGET`, which selects the godot-cpp template linked in.

---

## Running the Unit & Integration Tests

The test suite can be built independently of Godot (`VOXEL_BUILD_TESTS=ON` is enabled by default):

### Build & Run via CMake / CTest

```bash
# Configure tests only (zero Godot dependencies required)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVOXEL_BUILD_TESTS=ON
cmake --build build -j

# Run all test suites
ctest --test-dir build --output-on-failure
```

> With a multi-config generator (Visual Studio / Xcode), add the configuration:
> `cmake --build build --config Release -j` and
> `ctest --test-dir build -C Release --output-on-failure`.

### Running with Sanitizers (ASan / UBSan / TSan)

```bash
# AddressSanitizer & UndefinedBehaviorSanitizer
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVOXEL_ENABLE_ASAN=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# ThreadSanitizer
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVOXEL_ENABLE_TSAN=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Automated Bash Script (`run_all_tests.sh`)

On Linux/macOS/WSL, you can run all milestone test suites under both ASan and TSan in one pass, plus build the standalone `voxel_baker` CLI:

```bash
chmod +x run_all_tests.sh
./run_all_tests.sh
```

---

## Running the Godot Demo

1. Build the GDExtension library (see [Building the C++ GDExtension](#building-the-c-gdextension)).
2. Open **Godot 4.2+** and select **Import**.
3. Choose this repository's folder (containing `project.godot`).
4. Run the main demo scene: `res://voxel_terrain_demo.tscn` (press `F5`).
5. Optionally open `res://voxel_terrain_no_caves_demo.tscn` and run it to see
   cave-free terrain (its `VoxelTerrain3D` has `generate_caves = false`).

### Terrain Node Properties

Both demo scenes drive a `VoxelTerrain3D` node, whose key exported properties are:

| Property | Type | Default | Description |
| --- | --- | --- | --- |
| `view_distance_chunks` | `int` | `2` | Active streaming radius in chunks (auto-shrunk by 2 under memory pressure). |
| `prefetch_distance_chunks` | `int` | `1` | Extra Chebyshev ring streamed ahead of the active radius. |
| `voxel_scale` | `float` | `1.0` | Meters per voxel sample, clamped to `[0.125, 4.0]`. Set it before the node enters the tree; the demo uses `0.5`. |
| `world_height_chunks` | `int` | `4` | Vertical chunk ceiling, in 32-sample chunks (`32 × voxel_scale` m each). Sets the top of the world; streaming only populates layer indices `0 … world_height_chunks-1`. Baked in when the node starts, so set it before running. |
| `surface_base_height` | `float` | `68.0` | Average terrain surface height above the `y = 0` bedrock, in meters. Controls how much rock sits under the surface (and how many cave layers fit). |
| `collision_distance_chunks` | `int` | `-1` | 3D Chebyshev radius (in chunks, from the focus chunk) within which chunks build a `PhysicsServer3D` collision body. Distant chunks upload only their visual mesh. `-1` disables the gate. |
| `collision_subsample_stride` | `int` | `1` | Collision-only decimation (HLD §4.4). `2` rebuilds the physics/navigation triangle soup from a `16³` stride-2 subsample (~4× fewer physics triangles) while the `32³` visual mesh stays full-resolution and watertight. Clamped to `1`/`2`. |
| `resident_memory_budget_mb` | `int` | `512` | Memory ledger budget for resident chunk data. |
| `upload_budget_ms` | `float` | `2.0` | Per-frame main-thread commit budget for mesh/physics/nav uploads. |
| `terrain_material` | `Material` | `null` | Material applied to all committed chunk surfaces. |
| `generate_caves` | `bool` | `true` | When `false`, terrain is generated without the 3D cave term (cave-free). Only affects chunks generated after the change; already-resident chunks are untouched. |
| `instrumentation_enabled` | `bool` | `true` | Logs per-frame operation timings to the Godot **Output** panel (see [Performance Instrumentation](#performance-instrumentation)). Set `false` to silence. |

```gdscript
$VoxelTerrain3D.generate_caves = false
$VoxelTerrain3D.instrumentation_enabled = false
```

### Terrain Depth

Underground depth is the distance between the `y = 0` bedrock and the average
surface height (`surface_base_height`), bounded above by the world ceiling
(`world_height_chunks`). Both are range sliders on `VoxelTerrain3D`, editable in
the Godot **Inspector** or from GDScript:

```gdscript
$VoxelTerrain3D.world_height_chunks = 6      # 6 x 32 samples x 0.5 m = 96 m ceiling
$VoxelTerrain3D.surface_base_height = 132.0  # ~132 m of underground rock
```

**What each one does**

- **`world_height_chunks`** — the vertical ceiling in 32-sample chunks, so the top
  of the world is `world_height_chunks × 32 × voxel_scale` m (96 m at `6` and
  `0.5` m voxels). The streaming manager only populates
  chunk layers `0 … world_height_chunks-1`; anything above simply does not exist.
  It is baked into the chunk store when the node enters the tree, so **set it in
  the Inspector (or before adding the node) — changing it at runtime has no
  effect until the node restarts.**
- **`surface_base_height`** — the average surface height above bedrock, in meters,
  and the main depth dial. It is forwarded to the generator at startup and on
  every change, so lowering it thins the rock and raising it deepens the world.
  Like `generate_caves`, it is read per sample, so **only chunks generated after
  the change adopt the new height.**

**Picking a value.** Keep `surface_base_height + surface_amplitude` at least 20 m
below `world_height_chunks × 32 × voxel_scale` so hilltops never clip the ceiling.
The `world_height_chunks` slider runs `1 … 12` and `surface_base_height`
`16 … 256` m. Common profiles at `voxel_scale = 1.0` (from
[`docs/terrain depth.md`](docs/terrain%20depth.md)):

| Profile | `world_height_chunks` | `surface_base_height` | Underground depth |
| --- | --- | --- | --- |
| Shallow (fastest) | `2` (64 m) | `36.0` | ~36 m, 1 cave layer |
| Default | `4` (128 m) | `68.0` | ~68 m, 2 cave layers |
| Deep caverns | `6` (192 m) | `132.0` | ~132 m, 4 cave layers |
| Abyssal | `8` (256 m) | `196.0` | ~196 m, 6 cave layers |

**Two values track depth by hand.** They are not sliders on `VoxelTerrain3D`:

- `surface_height_threshold` in `voxel_triplanar.gdshader` — set it to roughly
  `surface_base_height - 12.0` so cave walls darken to rock below the surface.
- The demo `Camera3D` initial `Y` (in `voxel_terrain_demo.tscn`) — set it to about
  `surface_base_height + 16.0` so the camera spawns above the new surface.

### Collision & Triangle Count

Two properties shrink the per-chunk physics footprint for lower-end PCs. Both are
range sliders on `VoxelTerrain3D`, editable in the Godot **Inspector** or from
GDScript:

```gdscript
$VoxelTerrain3D.collision_distance_chunks = 3   # physics only within 3 chunks (96 m)
$VoxelTerrain3D.collision_subsample_stride = 2  # 16^3 collision soup (~4x fewer tris)
```

**What each one does**

- **`collision_distance_chunks`** — the 3D Chebyshev distance (in chunks, from the
  streaming focus chunk) within which a chunk builds a `PhysicsServer3D` concave
  collision body. Beyond it, a chunk uploads only its visual `RenderingServer` mesh:
  the shape RID still exists but is empty, so no concave BVH is built for it.
  `-1` disables the gate (every resident chunk collides — the default). Setting it
  re-evaluates collision for all resident chunks immediately, and as the player
  crosses chunk boundaries chunks leaving the radius have their collision cleared
  in place while chunks entering it are rebuilt from the cached faces. It gates
  physics only: explosions are voxel CSG edits, not physics raycasts, so carving
  still works at any distance.
- **`collision_subsample_stride`** — collision-only decimation (HLD §4.4). `1`
  (default) builds the physics/navigation triangle soup at full `32³` resolution.
  `2` rebuilds it from a `16³` stride-2 subsample of the same `36³` window, about
  `4×` fewer physics triangles per chunk, while the `32³` visual mesh stays
  full-resolution and watertight (no LOD seams). The navigation source geometry
  shares this soup, so navmeshes are also coarser at stride 2. It is read at mesh
  time, so **only chunks meshed after the change adopt it** (same contract as
  `generate_caves`). Values are clamped to `1` or `2`: the `36³` window reaches two
  voxels into the neighbour, so a larger stride would crack collision across chunk
  seams.

**Suggested presets** (pairing the streaming radii with the collision toggles):

| Preset | `view_distance_chunks` | `prefetch_distance_chunks` | `collision_distance_chunks` | `collision_subsample_stride` | `upload_budget_ms` |
| --- | --- | --- | --- | --- | --- |
| Low / integrated GPU | `3` | `0` | `2` | `2` | `1.0` |
| Medium | `4` | `1` | `2` | `2` | `1.5` |
| High | `6` | `2` | `3` | `1` | `2.0` |

The shipped `voxel_terrain_demo.tscn` uses `collision_distance_chunks = 3` and
`collision_subsample_stride = 2`.

### Controls

| Input | Action |
| --- | --- |
| **W / A / S / D** | Fly forward / left / backward / right |
| **Space / E** | Fly upward |
| **Ctrl / Q** | Fly downward |
| **Shift** (Hold) | Sprint speed boost ($3\times$) |
| **Mouse Motion** | Look around (first-person pitch/yaw) |
| **1 … 5** | Select terrain tool: Add terrain / Explode / Large explode / Paint terrain / Paint delete |
| **6 … 9** | Place boulder / Water quench / Frost-Ice bridge / Lava |
| **C** | Select the **Small hit** chip blast (default radius 1.5 m) for fine carving |
| **0 / -** | Stamp the configured asset at the crosshair (timed / permanent) |
| **R** | Reset all four demo statues (remove stamps + restore pristine meshes) |
| **T** | Toggle the add/paint material (soil / 3-hit rock) |
| **Left Click** | Apply the active tool at the crosshair (hit point, or 24 m ahead when aiming at the sky) |
| **Left Click** (Hold) | Tools 4 and 5 stream terrain additions / deletions along the drag path |
| **Right Click** | Query 3D cave navigation path for AI companion |
| **Escape** | Toggle mouse capture |

### Terrain Tools

Keys `1 … 5` select the active tool (shown in the HUD). `1` adds a sphere of new
terrain, `2` and `3` carve a crater at two radii, and `4` / `5` are
hold-and-drag brushes that continuously add or remove terrain along the sweep.
Each tool's radius is an exported knob on the demo controller, tunable live in
the Inspector:

| Export | Default | Used by |
| --- | --- | --- |
| `add_radius` | `6.0` | Tool 1 (Add terrain) |
| `explosion_radius` | `8.5` | Tool 2 (Explode) |
| `large_explosion_radius` | `18.0` | Tool 3 (Large explode) |
| `paint_radius` | `3.5` | Tool 4 (Paint terrain, drag-add) |
| `paint_delete_radius` | `3.5` | Tool 5 (Paint delete, drag-erase) |
| `small_hit_radius` | `1.5` | Key C (Small hit chip blast) |
| `brush_spacing_factor` | `0.6` | Drag-brush step distance (`radius x factor`) |
| `brush_interval` | `0.05` | Minimum seconds between drag-brush steps |

Aiming uses the engine's voxel-space raycast (`VoxelTerrain3D.raycast_terrain`,
which marches the resident TSDF chunks), so tools hit the true terrain surface at
any distance regardless of `collision_distance_chunks` gating. Both operations
are asynchronous CSG sphere edits on the native engine and never block the main
thread. From GDScript:

```gdscript
$VoxelTerrain3D.trigger_explosion(Vector3(16, 70, 16), 8.5)  # carve
$VoxelTerrain3D.add_terrain(Vector3(16, 70, 16), 6.0)        # add
```

`trigger_explosion` reports through `explosion_requested` / `explosion_completed`
/ `explosion_failed`; `add_terrain` mirrors those with `terrain_add_requested` /
`terrain_add_completed` / `terrain_add_failed`.

### Offline Asset Stamping & Reconstitution

Hand-crafted meshes are voxelized **offline** into `.vreg` region files by the
standalone `voxel_baker` CLI (built by CMake into `build/bin/voxel_baker`), then
stamped into the live world at runtime:

```bash
voxel_baker <input.obj> <output.vreg> [options]
  --scale <float>      # Uniform vertex scale multiplier (default: 1.0)
  --voxel-scale <float># Meters per voxel sample (default: 1.0)
  --thickness <float>  # Shell wall thickness in meters (default: 1.6)
  --material <name>    # hard_rock (default), soil, obsidian, ice, lava
  --watertight         # Watertight solid mode (surface stays on the source mesh)
  --texture <file.ppm> # Binary PPM albedo sampled via UVs into per-voxel RGB5551
  --color-depth <float># Depth (m) that receives baked color (default: 1.8)
  --center-x/y/z <f>   # Target region center (default: 128, 128, 128)
  --no-color           # Disable per-voxel color baking
  --no-center          # Do not auto-center the mesh at local (128, 128, 128)
  --rx/--ry/--rz <int> # Target region coordinates written to the header
  --delta <float>      # Narrow-band truncation delta (default: 2.0)
```

The baker fan-triangulates polygon faces, maps `usemtl` names to engine
materials, accelerates point-to-triangle distance queries with an 8 m spatial
grid, and writes the engine's canonical format: a 512-entry TOC with far-field
chunks as uniform `Empty` (0 bytes), narrow-band chunks as byte-plane-RLE
compressed 16-bit TSDF samples, and an FNV-1a64 payload checksum. `--watertight`
fills closed interiors solid; the default shell mode gives open ruins, arches
and walls a real $T$ metre wall thickness. With `--texture`, the closest-point
UV is sampled and baked into an RGB5551 **per-voxel albedo plane** (outer
`--color-depth` skin only), carried through chunk storage, the Surface Nets
mesher (`UV`/`UV2`/`COLOR.a`), and saved `.vreg`s — so painted statues keep
their texture through carving, and deep crater walls expose raw rock. See
`include/voxel_mesh_baker.hpp` and the full walkthrough in
[`docs/asset stamping and reconstitution.md`](docs/asset%20stamping%20and%20reconstitution.md).

Stamping a baked region from GDScript (a reconstitution timer of `15.0` s here;
pass `0.0` — or use demo Key `-` — for permanent destruction):

```gdscript
var structure_id := $VoxelTerrain3D.stamp_asset(
    "res://ancient_ruin.vreg", Vector3(chunk_x, chunk_y, chunk_z), 15.0)
```

`AssetStampManager` snapshots each stamped chunk's pre-damage TSDF and materials
as its baseline. Non-fill edits (explosions / elemental reactions) that dirty a
stamped chunk mark the structure damaged, re-mesh its chunks and re-bake its
navigation groups; when the quiet period elapses it restores the exact baseline
TSDF, materials and damage state. Signals:

| Signal | Payload | Fires when |
| --- | --- | --- |
| `structure_damaged` | `structure_id`, `name` | A stamped structure's chunks were carved; its reconstitution timer is armed. |
| `structure_recreated` | `structure_id`, `name` | The quiet period elapsed and the exact pre-damage voxels were restored. |

In the demo, **Key 0** stamps the configured `stamp_asset_path` at the crosshair
with `stamp_recreate_delay_seconds` (default 15 s); **Key -** stamps it with no
timer so it stays permanently destroyed. Aim at the terrain surface first.

`voxel_terrain_demo.tscn` also seeds three 48 m statue pairs: the 2x Moai pair
at `z = 0`, the 5.33x Atlas pair at `z = -64`, and the 5.33x Ape pair at
`z = -128`, all stamped into a `0.5 m` voxel world. Each pair shows the same two
styles side by side:

| Statue | Behavior |
| --- | --- |
| **Option 1 �?" Pristine Mesh Swap** | Shows the textured `models/moai_statue.obj` / `models/atlas/atlas.obj` / `models/ape/ape.obj` mesh until the first blast reaches it; the hit hides the mesh and stamps the same statue as voxels (crater carved immediately). |
| **Option 2 �?" Baked Voxel Colors** | Stamped at startup from `moai_statue.vreg` / `models/atlas/atlas_statue.vreg` / `models/ape/ape_statue.vreg` with per-voxel albedo, 3-hit cracks, raw-rock craters, and a 15 s reconstitution timer. |

The Atlas `.vreg` is baked watertight from the GLB's extracted OBJ + albedo:

```bash
voxel_baker models/atlas/atlas.obj models/atlas/atlas_statue.vreg \
    --voxel-scale 0.5 --watertight --texture models/atlas/atlas_albedo.ppm \
    --color-depth 2.8 --center-x 160 --center-y 160 --center-z 128
```

`atlas.obj` and `atlas_albedo.ppm` are derived from `models/atlas/atlas.glb`
(the 9 m source mesh scaled 5.333x to 48 m); re-export them from the GLB before
re-baking if the source asset changes. The Moai pair is baked the same way:

```bash
voxel_baker models/moai_statue.obj moai_statue.vreg \
    --voxel-scale 0.5 --watertight --texture models/moai_albedo.ppm \
    --color-depth 2.8 --center-x 160 --center-y 160 --center-z 128
```

The Ape pair follows the same recipe from `models/ape/ape.glb` (decimated to
~28.5k triangles before baking; see `models/ape/README.md`):

```bash
voxel_baker models/ape/ape.obj models/ape/ape_statue.vreg \
    --voxel-scale 0.5 --watertight --texture models/ape/ape_albedo.ppm \
    --color-depth 2.8 --center-x 160 --center-y 160 --center-z 128
```

**Key R** resets all six statues (removes the voxel structures, restores
Option 1's pristine meshes, re-stamps every Option 2 statue); **Key T** toggles
the add/paint material.

### Pill Companion (Demo)

The demo's `VoxelAICompanion` node is not just a query facade: it carries a
visible pill capsule driven by `ai_companion_patrol.gd`, which loops a
triangular patrol route around the spawn area. Waypoint Y values are ignored —
every frame the pill is pinned to `VoxelTerrain3D.get_surface_height(x, z)`, so
it descends into fresh craters and rides over added terrain as you edit.

| Export | Default | Purpose |
| --- | --- | --- |
| `patrol_points` | `[(16, 44), (44, 8), (-12, 8)]` | Patrol loop as world XZ points (Y is terrain-derived). |
| `patrol_speed` | `6.0` | Metres per second along the route. |
| `ground_offset` | `0.0` | Vertical offset added to the queried surface height. |
| `yaw_turn_speed` | `8.0` | Facing turn rate toward the direction of travel. |
| `terrain_path` | `^"../VoxelTerrain3D"` | Terrain node used for the height queries. |

The height query itself is Godot-free and reusable from GDScript:

```gdscript
var y: float = $VoxelTerrain3D.get_surface_height(16.0, 16.0)  # top surface at (x, z)
```

---

## Performance Instrumentation

Every engine operation is timed with a lock-free, per-stage accumulator and reported
to the Godot **Output** panel as one summary line per frame that did work. Worker
threads record; the main thread drains once per frame, so a single slow call stays
visible instead of being hidden by an average.

| Stage | What it measures |
| --- | --- |
| `generate` | Per-chunk procedural generation / disk load (worker lane 3) |
| `mesh` | $36^3$ scratch gather + Surface Nets meshing (worker lane 2) |
| `commit` | Main-thread mesh upload + collision + navigation submission |
| `stream_focus` | `StreamingManager::update_stream_focus` |
| `edit` | CSG sphere edit phase execution (carve / add, worker lane 1) |
| `navigation` | Navigation path / distance query (`VoxelAICompanion.get_voxel_path`) |

Each active stage reports `xN total T ms max M ms avg A ms`:

```text
[voxel] generate x12 total 4.53ms max 0.62ms avg 0.38ms | mesh x8 total 9.10ms max 2.30ms avg 1.14ms | commit x8 total 1.20ms max 0.31ms avg 0.15ms
[voxel] initial fill complete in 1840.2 ms
```

`initial fill complete in …` is logged once, when the first streaming fill reaches
`fully_committed()`. Logging is on by default and toggled from GDScript:

```gdscript
$VoxelTerrain3D.instrumentation_enabled = false
```

The timing helper (`include/stage_timings.hpp`) is Godot-free, so the same
accumulator is exercised by the unit tests; `VoxelTerrain3D.set_log_sink()` lets
hosts redirect the output (tests capture it instead of printing).

---

## Project Structure

```text
├── bin/                          # Output directory for compiled GDExtension libraries (auto `.gdignore`)
├── build/                        # CMake build tree (auto `.gdignore` keeps Godot from importing `.obj` files)
├── docs/                         # High-level architecture and design documents (v1 - v4)
├── include/                      # Pure C++20 voxel core engine (zero Godot dependencies)
│   ├── asset_stamp_manager.hpp   # .vreg asset stamping, damage tracking, reconstitution
│   ├── chunk_key.hpp             # 64-bit packed chunk coordinates and spatial hashing
│   ├── chunk_store.hpp           # 32³ TSDF chunk storage, compact chunks, open-address map
│   ├── edit_and_worker_pool.hpp  # Multi-threaded job pool, CSG sphere edits, anti-crumb filter
│   ├── godot_wrapper_and_nav3d.hpp # 3D navigation mesh conversion and helper contracts
│   ├── persistence_and_streaming.hpp # 8×8×8 region streaming and LZ4 compression
│   ├── procgen_and_surface_nets.hpp  # Surface Nets mesher, FastNoise SIMD terrain/caves
│   ├── scratch_and_dirty.hpp     # 36³ thread-local scratch windows, dirty flags tracking
│   ├── stage_timings.hpp         # Lock-free per-operation timing accumulator (Godot-free)
│   └── voxel_mesh_baker.hpp      # Offline .obj → .vreg voxelizer (TSDF narrow band)
├── src/                          # Godot 4 GDExtension bindings
│   ├── register_types.cpp/.h     # GDExtension entrypoint and class registration
│   ├── voxel_baker_main.cpp      # Standalone `voxel_baker` CLI entry point
│   ├── voxel_ai_companion.cpp/.h # VoxelAICompanion node (3D cave pathfinding)
│   └── voxel_terrain_3d.cpp/.h   # VoxelTerrain3D node (streaming, remeshing, physics)
├── models/                       # Demo assets (Moai GLB/OBJ, extracted albedo, baked .vreg)
├── tests/                        # Milestone unit & integration tests (M0 - M4)
├── CMakeLists.txt                # Root CMake build configuration
├── project.godot                 # Godot engine project definition
├── run_all_tests.sh              # Standalone test runner with ASan/TSan
├── terrain_demo.gd               # Demo controller script (free-cam, terrain tools, nav queries)
├── ai_companion_patrol.gd        # Pill companion patrol driver (surface height follow)
├── voxel_engine.gdextension      # GDExtension library definition file
├── voxel_terrain_demo.tscn       # Main 3D test scene (caves enabled)
├── voxel_terrain_no_caves_demo.tscn # Variant scene with `generate_caves = false`
└── voxel_triplanar.gdshader      # PBR Triplanar terrain shader
```

---

## License

This project is licensed under the MIT License.

