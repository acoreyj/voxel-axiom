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
- **Crash-Safe 3D Region Persistence**: $8 \times 8 \times 8$ chunk region files (up to 512 chunks per region) with LZ4 byte-plane compression and background disk streaming.
- **Triplanar Voxel Shading**: Custom triplanar shader blending rock, soil, grass, and cave interior textures with normal mapping and roughness.

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

Which **slot** Godot loads (`debug` or `release`) is chosen by the `godot-cpp`
target you link against, *not* by the CMake build type. One build directory
therefore targets one slot:

| Use case | `GODOTCPP_TARGET` | Artifact slot |
| --- | --- | --- |
| Running / debugging in the Godot **editor** | `template_debug` (default) | `…debug…` |
| Exporting a **release** game | `template_release` | `…release…` |

Build a *second* build directory if you need both.

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

`<debug|release>` follows `GODOTCPP_TARGET`, so a `template_debug` build always
lands in the `debug` slot even when compiled with `--config Release`.

---

## Running the Unit & Integration Tests

The test suite can be built independently of Godot (`VOXEL_BUILD_TESTS=ON` is enabled by default):

### Build & Run via CMake / CTest

```bash
# Configure tests only (zero Godot dependencies required)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVOXEL_BUILD_TESTS=ON
cmake --build build -j

# Run all 8 test suites
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

On Linux/macOS/WSL, you can run all 8 milestone test suites under both ASan and TSan in one pass:

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
| `resident_memory_budget_mb` | `int` | `512` | Memory ledger budget for resident chunk data. |
| `upload_budget_ms` | `float` | `2.0` | Per-frame main-thread commit budget for mesh/physics/nav uploads. |
| `terrain_material` | `Material` | `null` | Material applied to all committed chunk surfaces. |
| `generate_caves` | `bool` | `true` | When `false`, terrain is generated without the 3D cave term (cave-free). Only affects chunks generated after the change; already-resident chunks are untouched. |

```gdscript
$VoxelTerrain3D.generate_caves = false
```

### Controls

| Input | Action |
| --- | --- |
| **W / A / S / D** | Fly forward / left / backward / right |
| **Space / E** | Fly upward |
| **Ctrl / Q** | Fly downward |
| **Shift** (Hold) | Sprint speed boost ($3\times$) |
| **Mouse Motion** | Look around (first-person pitch/yaw) |
| **Left Click** | Fire real-time CSG sphere crater explosion |
| **Right Click** | Query 3D cave navigation path for AI companion |
| **Escape** | Toggle mouse capture |

---

## Project Structure

```text
├── bin/                          # Output directory for compiled GDExtension libraries
├── docs/                         # High-level architecture and design documents (v1 - v4)
├── include/                      # Pure C++20 voxel core engine (zero Godot dependencies)
│   ├── chunk_key.hpp             # 64-bit packed chunk coordinates and spatial hashing
│   ├── chunk_store.hpp           # 32³ TSDF chunk storage, compact chunks, open-address map
│   ├── edit_and_worker_pool.hpp  # Multi-threaded job pool, CSG sphere edits, anti-crumb filter
│   ├── godot_wrapper_and_nav3d.hpp # 3D navigation mesh conversion and helper contracts
│   ├── persistence_and_streaming.hpp # 8×8×8 region streaming and LZ4 compression
│   ├── procgen_and_surface_nets.hpp  # Surface Nets mesher, FastNoise SIMD terrain/caves
│   └── scratch_and_dirty.hpp     # 36³ thread-local scratch windows, dirty flags tracking
├── src/                          # Godot 4 GDExtension bindings
│   ├── register_types.cpp/.h     # GDExtension entrypoint and class registration
│   ├── voxel_ai_companion.cpp/.h # VoxelAICompanion node (3D cave pathfinding)
│   └── voxel_terrain_3d.cpp/.h   # VoxelTerrain3D node (streaming, remeshing, physics)
├── tests/                        # Milestone unit & integration tests (M0 - M4)
├── CMakeLists.txt                # Root CMake build configuration
├── project.godot                 # Godot engine project definition
├── run_all_tests.sh              # Standalone test runner with ASan/TSan
├── terrain_demo.gd               # Demo controller script (free-cam, explosion, nav queries)
├── voxel_engine.gdextension      # GDExtension library definition file
├── voxel_terrain_demo.tscn       # Main 3D test scene (caves enabled)
├── voxel_terrain_no_caves_demo.tscn # Variant scene with `generate_caves = false`
└── voxel_triplanar.gdshader      # PBR Triplanar terrain shader
```

---

## License

This project is licensed under the MIT License.

