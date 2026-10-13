# High-Level Design Document (HLD) v4.0

## Project Core Architecture: Real-Time Destructible Voxel Engine with 3D Cave Navigation (Godot 4 GDExtension)

**Status:** Draft v4.0 (Implementation-Ready Specification)
**Supersedes:** High-Level Design Document (HLD) v3.0
**Review input:** v3.0 architecture, mathematical, real-time destruction, and 3D cave navigation audit

---

## 0. Revision History

### Changes from v1 to v2

| Area | v1 | v2 |
| --- | --- | --- |
| Threading | Explosion path runs synchronously on the main thread | Async job pipeline with an MPSC completion queue and a per-frame GPU upload budget |
| Lifetime | `delete core_engine` in destructor | Explicit ordered teardown with the acting thread named per step (5.2, 5.3) |
| Meshing | Transvoxel with no LOD structure | Uniform-resolution Surface Nets for v2.0. Transvoxel deferred to an LOD milestone |
| Chunk layout | 32³ core, no border data | 32³ cells, padded sample volume for boundary meshing |
| SDF format | `int8_t` or `float`, unnormalized | Truncated SDF (TSDF) stored as normalized `int16_t` with a fixed truncation band |
| Chunk keys | Signed 21-bit packing, sign extension unspecified | Offset encoding, no sign extension |
| Storage index | "Octree" in diagram, hash map in text | Flat open-addressing hash map. No octree in v2.0 |
| Collision | Not specified | Dedicated collision pipeline (4.4) |
| Navigation | Standalone `dtTileCache` | Godot `NavigationServer3D` as the single navigation authority (4.5) |
| Save format | RLE then LZ4 on float SDF | Homogeneous-chunk forms, quantized `int16_t`, byte-plane split, LZ4, crash-safe region files |
| Godot API | `PackedVector3Array::to_writer()` (nonexistent) | `resize()` + `ptrw()`, direct `RenderingServer` path |
| Registration | `VoxelAICompanion` not registered | Both classes registered at scene init level |
| Core linkage | Separate core DLL, C++ ABI across boundary | Core statically linked into the extension (6.1) |
| Library output | Windows, Linux, macOS x86_64 release only | Debug and release entries, arm64 for macOS |

### Changes from v2.0 to v3.0

| Area | v2.0 | v3.0 |
| --- | --- | --- |
| Concurrency | Job versioning only | Normative concurrency and ownership contract (3.2): single writer, snapshot reads, reference-counted unload |
| Sample ownership | Apron and core symmetric | Canonical 32³ sample ownership via mathematical `floor_div(p, 32)` (3.1, 3.2) |
| Residency | 118 KB per chunk always | `EMPTY`/`SOLID` chunks use a 0-byte compact representation; material channel lazily allocated (3.1) |
| Save consistency | Unspecified | Version-bracketed serialization with retry-or-discard; 32³ canonical samples only (3.2, 3.5) |
| Shutdown | Persist flushed after join | Single ordered sequence, drain specified by thread (5.3) |
| Signals | `explosion_applied` ambiguous | `explosion_requested`, `explosion_completed`, and `explosion_failed` (4.2) |
| Readiness | `is_idle()` conflated | `core_idle()` and `fully_committed()` (4.2) |
| Streaming / residency | `view_distance_chunks` declared without residency policy | Desired set, prefetch, load/generate routing, 26-neighbor closure, eviction, hard memory budget (3.3, 3.4) |
| Thread start | Constructor | `_enter_tree` (5.2) |
| Edit coalescing | "Covering job" could be read as changing CSG shape | Coalescing preserves the exact union of sphere CSG terms (3.4) |

### Changes from v3.0 to v4.0

| Area | v3.0 | v4.0 |
| --- | --- | --- |
| **Chunk residency vs. mesh window** | Stored a persistent 35³ (`-1..33`) array (83.7 KiB) in `ChunkStore` and locked 26 neighbors on edit | `ChunkStore` stores **only canonical 32³ samples (64.0 KiB)**. Mesh workers reconstruct a **36³ (`-1..34`) window** in thread-local scratch memory (3.1, 3.5) |
| **Surface Nets seam closure** | Emitted only cells `0..31` from a 35³ window, leaving a 1-cell gap between chunks and reading sample `34` out of bounds for cell `32` normals | Mesh computes dual vertices for **cells `0..32` (33³ cells)** using corners `0..33` and central-difference normals from `-1..34` (36³ samples), then stitches edges exiting cells `0..31` (3.1, 3.5) |
| **Edit phase lock bound** | `max_edit_phase_core_chunks = 32` + 26-neighbor closure required up to 144 locks, violating `max_edit_lock_chunks = 96` | Edit phases lock **only canonical core chunks** (`max_edit_phase_core_chunks = 32`, `max_edit_lock_chunks = 32`). Selective boundary dirtying marks neighbors dirty only when samples `0..2` or `31` change (3.2) |
| **Mid-explosion tearing** | Each intermediate Edit phase immediately published dirty chunks and nav updates, meshing half-carved craters | Dirty chunks and `EditTransactionCompleted` are published atomically when the **final phase** of the explosion transaction finishes (3.2, 3.4, 5.2) |
| **Explosion CSG bounds** | Bounded by sphere radius $R$, leaving samples in $[R, R+\delta)$ clamped at $-\delta$ and causing terraced crater walls | Bounded by expanded radius **$R + \delta$** so the full negative TSDF band is smooth across blast walls (3.5, 7.1) |
| **Floating cave debris** | Intersecting blasts in caves left 1-voxel floating solid crumbs that corrupted navmeshes | Final Edit phase runs a $3 \times 3 \times 3$ **Anti-Crumb Kernel** that prunes isolated 1-voxel solid artifacts to air (3.5) |
| **Dirty-set scheduling** | Single `mesh_dirty` set allowed background streaming to delay explosion remeshing | Split into **`dirty_edit_chunks`** (high priority) and **`dirty_stream_chunks`** (low priority) (3.4) |
| **ProcGen caves & SIMD** | 4 point noise calls per sample; unregularized $\lVert \nabla \text{density} \rVert \to 0$ divided by zero at cave ceilings/floors | Single `FastNoise2::GenUniformGrid3D` ($34^3$ buffer) per chunk + in-memory central differences, $\max(\lVert \nabla D \rVert, 0.05)$ floor, and 2D surface + 3D dual-noise cave composition (3.5) |
| **Region file I/O** | `32 × 32 × world_height` region files caused 50–100 MB rewrite spikes on flush | **3D `8 × 8 × 8` chunk region files** (512 chunks max) with two-stage persistence: fast in-memory LZ4 `RegionBlobTable` compression followed by throttled background disk flushes (3.4, 3.6) |
| **Main-thread commit loop** | `const bool hard_backpressure` evaluated once before the `while` loop drained all 32 meshes in 1 frame (50–100 ms freeze) | Queue depth is re-evaluated **every iteration** inside `_process()`, bounded by a hard `emergency_budget_us = 2 * soft_budget_us` frame ceiling (4.3, 5.2) |
| **Godot scene overhead** | Per-chunk `StaticBody3D` and `MeshInstance3D` nodes bloated the SceneTree | Direct **`RenderingServer` and `PhysicsServer3D` RIDs** managed per chunk with zero child `Node3D` instances (4.1, 4.4) |
| **3D Cave Navigation** | `4 × 4` X/Z groups spanning full `[0, world_height_chunks)` Y forced full-column rebakes on underground blasts; 10 ms main-thread submit | **3D `4 × 2 × 4` chunk navigation groups** (`128 × 64 × 128` cells) with 3D border padding, cave ledge filtering, and **`bake_from_source_geometry_data_async`** submission in less than 0.2 ms (4.5, 7.3) |

---

## 1. Executive Summary

### 1.1 Objective

Design a cross-platform, high-performance, **modular voxel engine written in C++** as a Godot-independent core library statically linked into a **Godot 4.2+ GDExtension** using **godot-cpp**. The engine is purpose-built for **real-time large-scale destruction**, **3D underground cave systems**, and **asynchronous AI pathfinding updates** under strict frame-time budgets.

### 1.2 Core Capabilities

* **Real-Time Large Destruction:** Phased spherical and composite CSG subtraction on a Truncated Signed Distance Field (TSDF), executed by worker threads with bounded lock sets ($32$ chunks max per phase), expanded $R + \delta$ band preservation, isolated-crumb pruning, and priority-queued mesh commits.
* **3D Cave & Overhang Generation:** Multi-threaded, SIMD-vectorized procedural terrain and 3D cave tunnel generation using **FastNoise2** uniform grid evaluation and regularized gradient distance estimation.
* **Dynamic 3D AI Navigation:** Asynchronous, 3D-partitioned (`4 × 2 × 4` chunk groups) navigation mesh rebaking through Godot's `NavigationServer3D::bake_from_source_geometry_data_async`, isolating underground cave destruction from surface navigation meshes.
* **Two-Stage Persistent Saves:** Version-bracketed chunk serialization with homogeneous-chunk elision, byte-plane split `int16_t` quantization, in-memory LZ4 blob caching (`RegionBlobTable`), and atomic `8 × 8 × 8` 3D region files.
* **Server-Direct Rendering & Physics:** Direct `RenderingServer` and `PhysicsServer3D` RID management for visual meshes and static concave triangle collision shapes without SceneTree node overhead.

### 1.3 Non-Goals for v4.0

* Multi-resolution LOD and Dual Marching Cubes / Transvoxel transitions (planned for milestone M5).
* Multiplayer network replication of voxel edits.
* Multi-material per-sample blending weights beyond a single dominant `uint8_t` material ID per canonical sample.

---

## 2. System Architecture Overview

The architecture enforces a strict separation of concerns: the **Core (`libvoxel_core`)** owns authoritative voxel state, spatial hashing, CSG editing, procedural generation, Surface Nets meshing, and persistence. The **GDExtension Wrapper** owns Godot `RenderingServer` / `PhysicsServer3D` RIDs, 3D navigation group orchestration, frame-budgeted commits, and GDScript bindings.

```
+-------------------------------------------------------------------------+
|                           GODOT ENGINE 4.2+                             |
|  +-----------------------------+       +-----------------------------+  |
|  |     VoxelTerrain3D Node     |       |      VoxelAICompanion       |  |
|  +--------------+--------------+       +--------------+--------------+  |
|                 | RenderingServer / PhysicsServer3D   | NavigationAgent3D|
+-----------------|-------------------------------------|-----------------+
                  | (Direct RIDs, zero child Nodes)     |
+-----------------|-------------------------------------|-----------------+
|                 GDEXTENSION WRAPPER LAYER (godot-cpp)                   |
|  - Drains MPSC completion queue within soft/emergency frame budgets     |
|  - Manages per-chunk RenderingServer & PhysicsServer3D RIDs             |
|  - Orchestrates 3D (4x2x4) NavigationGroups via async source geometry   |
+-----------------------------------|-------------------------------------+
                                    | Pure C++ API (no Godot types)
+-----------------------------------|-------------------------------------+
|                        CORE VOXEL ENGINE (voxel_core)                   |
|  ChunkStore          WorkerPool           MeshGen          SaveSystem   |
|  (flat hash map,     (5 priority lanes,   (36^3 scratch,   (3D 8x8x8    |
|   canonical 32^3      edit/stream dirty    Surface Nets,    regions,    |
|   TSDF + material)    queues, aged)        33^3 dual grid)  RAM LZ4)    |
|                                                                         |
|  ProcGen (FastNoise2 34^3 SIMD)    Edit (Phased R+delta CSG + De-crumb) |
+-------------------------------------------------------------------------+

```

---

## 3. Core Engine Pipeline (Pure C++)

### 3.1 Data Layout and Chunk Store

#### Coordinate Conventions

All coordinates are **world sample coordinates**: continuous real-valued positions where sample grid points lie at integers and the world origin sits at sample $(0, 0, 0)$ with scale $s$ (default $s = 1.0\text{ m}$ per voxel cell).

A chunk with coordinate $C = (C_x, C_y, C_z)$ owns a $32^3$ block of **cells** and a $32^3$ block of **canonical samples**. Cell $(i, j, k)$ with $i, j, k \in [0, 31]$ has its minimum corner at world sample:

$$p_{\min} = 32 \cdot C + (i, j, k)$$

The 8 corner samples of cell $(i, j, k)$ are located at $32 \cdot C + (i + a, j + b, k + c)$ for $a, b, c \in \{0, 1\}$.

* **Canonical sample ownership:** For every integer world sample coordinate $p = (p_x, p_y, p_z)$, its unique authoritative owner chunk is:

$$C = \big(\text{floor\_div}(p_x, 32),\; \text{floor\_div}(p_y, 32),\; \text{floor\_div}(p_z, 32)\big)$$

and its local sample coordinate within $C$ is:

$$L = \big(\text{floor\_mod}(p_x, 32),\; \text{floor\_mod}(p_y, 32),\; \text{floor\_mod}(p_z, 32)\big) \in [0, 31]^3$$

where $\text{floor\_div}$ and $\text{floor\_mod}$ use mathematical floor division toward $-\infty$ (never C++ truncation toward zero).

* **No persistent aprons in `ChunkStore`:** Every chunk in `ChunkStore` stores **only its $32^3$ canonical owned samples**. Boundary and apron samples are never stored persistently inside `ChunkStore`; they are gathered on demand into a thread-local scratch buffer when a worker meshes a chunk.

#### Seamless Surface Nets Dual-Grid & The $36^3$ Worker Scratch Window

In Naive Surface Nets, each active cell $(i, j, k)$ containing a sign change across its 12 edges produces **one dual vertex** inside that cell, and each sign-changing interior grid edge connects **4 adjacent cells** to emit a quad (2 triangles).

To close the surface across the high chunk boundary without gaps or duplicate triangles:

1. Chunk $C$ is responsible for emitting quads for all sign-changing edges whose minimum sample endpoint lies in local range $[0, 31]^3$ and points along $+X$, $+Y$, or $+Z$.
2. An edge at local index $31$ (for example along $+Z$ from $(31, 31, 31)$ to $(31, 31, 32)$) connects 4 cells that reach into local cell index **`32`** (which belongs to adjacent chunks $C + \hat{x}$, $C + \hat{y}$, $C + \hat{x} + \hat{y}$).
3. Therefore, to position the dual vertices of cells `0..32` ($33^3$ cells), the mesher needs corner SDF samples `0..33` ($34^3$ corners).
4. To compute central-difference vertex normals $\nabla S(q) = \frac{1}{2}\big(S(q+1) - S(q-1)\big)$ at corner samples `0..33`, the mesher requires one additional sample on both the low side (`-1`) and the high side (`34`).

Each worker thread therefore owns a pre-allocated **$36^3$ (`-1..34` on each axis) thread-local scratch window** (`46,656` `int16_t` samples $\approx 91.1\text{ KiB}$ plus `46,656` `uint8_t` material IDs $\approx 45.6\text{ KiB}$):

| Scratch Local Sample Range | Source Chunk Offset $(\Delta C_x, \Delta C_y, \Delta C_z)$ | Canonical Local Index in Source Chunk | Role in Surface Nets |
| --- | --- | --- | --- |
| **`-1`** | $-1$ on that axis | `31` | Low central-difference normal for corner `0` |
| **`0 … 31`** | $0$ (Chunk $C$ itself) | `0 … 31` | Canonical samples owned by Chunk $C$; corners for cells `0..31` |
| **`32`** | $+1$ on that axis | `0` | High corners of cell `31` and low corners of boundary cell `32` |
| **`33`** | $+1$ on that axis | `1` | High corners of boundary cell `32` |
| **`34`** | $+1$ on that axis | `2` | High central-difference normal for corner `33` |

Because the $36^3$ scratch window for chunk $C$ reads local sample `31` from negative neighbors ($C - 1$) and local samples `0, 1, 2` from positive neighbors ($C + 1$), **selective neighbor dirtying** applies during edits:

* Mutating a canonical sample at local index `0`, `1`, or `2` on an axis marks the negative neighbor ($C - 1$ on that axis) `mesh_dirty`.
* Mutating a canonical sample at local index `31` on an axis marks the positive neighbor ($C + 1$ on that axis) `mesh_dirty`.
* Mutating an interior canonical sample in $[3, 30]^3$ only marks chunk $C$ itself `mesh_dirty` and **never** dirties any neighbor chunk.

#### Sample Format

Truncated SDF stored as normalized `int16_t`:

* **Truncation band:** $\pm\delta$, where $\delta = 2.0$ voxel units (configurable at build time).
* **Encoding:** $q = \text{clamp}\big(\text{round}(\text{sdf} / \delta \times 32767),\; -32767,\; 32767\big)$.
* **Decoding:** $\text{sdf} = q \times (\delta / 32767.0f)$.
* **Sign convention:** **Negative = solid, positive = air.** The surface is the $\text{sdf} = 0$ isosurface.
* **Clamped markers:** `-32767` (`kClampedSolid`) and `+32767` (`kClampedAir`) represent distances at or beyond $\pm\delta$. (`INT16_MIN` $= -32768$ is avoided so negation is always symmetric).

#### Compact Residency Representation

A chunk's canonical $32^3$ sample data in `ChunkStore` is stored in one of three forms:

| Form | Heap Allocation in `ChunkStore` | Meaning |
| --- | --- | --- |
| `EMPTY` | `0 bytes` | All $32^3$ canonical samples are at `kClampedAir` ($+32767$) |
| `SOLID` | `0 bytes` | All $32^3$ canonical samples are at `kClampedSolid` ($-32767$) |
| `DENSE` | $32^3 \times 2\text{ B} = \mathbf{65,536\text{ B}}$ (**64.0 KiB**) | Full `int16_t[32768]` canonical sample array |

* Promoting `EMPTY` or `SOLID` to `DENSE` allocates the $64.0\text{ KiB}$ array and fills it with `kClampedAir` or `kClampedSolid` before applying edits.
* If an edit leaves all $32^3$ canonical samples uniformly at `kClampedAir` (for example, an explosion completely hollowing out a chunk inside its core), the chunk is demoted back to `EMPTY` and its $64.0\text{ KiB}$ buffer is returned to the chunk slab pool.

#### Material Channel

* Stored as a parallel `uint8_t[32768]` (**32.0 KiB**) per canonical sample, **lazily allocated**.
* While all canonical samples in a chunk share a single uniform `base_material_id` (default `0`), no array is allocated.
* Total authoritative memory for a dense chunk with heterogeneous materials is **$64.0\text{ KiB} + 32.0\text{ KiB} = 96.0\text{ KiB}$** (down from $115.7\text{ KiB}$ in v3.0).

#### Chunk Key

64-bit, offset-encoded. Each axis is stored as `coord + 2^20` in 21 bits, giving a range of $[-1,048,576,\; 1,048,575]$ chunk coordinates per axis.

```cpp
struct ChunkKey {
    uint64_t raw;

    static constexpr int64_t kBias = 1LL << 20;
    static constexpr uint64_t kMask = (1ULL << 21) - 1;

    static constexpr int32_t floor_div(int32_t a, int32_t b) {
        int32_t d = a / b;
        int32_t r = a % b;
        return (r != 0 && ((r < 0) != (b < 0))) ? (d - 1) : d;
    }

    static constexpr int32_t floor_mod(int32_t a, int32_t b) {
        int32_t r = a % b;
        return (r != 0 && ((r < 0) != (b < 0))) ? (r + b) : r;
    }

    static constexpr ChunkKey from_coords(int32_t x, int32_t y, int32_t z) {
        uint64_t ux = static_cast<uint64_t>(static_cast<int64_t>(x) + kBias) & kMask;
        uint64_t uy = static_cast<uint64_t>(static_cast<int64_t>(y) + kBias) & kMask;
        uint64_t uz = static_cast<uint64_t>(static_cast<int64_t>(z) + kBias) & kMask;
        return ChunkKey{ ux | (uy << 21) | (uz << 42) };
    }

    int32_t x() const { return static_cast<int32_t>(raw & kMask) - static_cast<int32_t>(kBias); }
    int32_t y() const { return static_cast<int32_t>((raw >> 21) & kMask) - static_cast<int32_t>(kBias); }
    int32_t z() const { return static_cast<int32_t>((raw >> 42) & kMask) - static_cast<int32_t>(kBias); }

    bool operator==(const ChunkKey& other) const { return raw == other.raw; }
    bool operator<(const ChunkKey& other) const { return raw < other.raw; }
};

```

#### Storage Index and Slab Allocator

* Flat open-addressing hash map (`ankerl::unordered_dense::map` or `absl::flat_hash_map`) mapping `ChunkKey` to a 32-bit pool slot handle.
* All $64.0\text{ KiB}$ TSDF blocks and $32.0\text{ KiB}$ material blocks are allocated from pre-reserved, page-aligned **Slab Pools** governed by the engine memory ledger (`resident_memory_budget_mb`), eliminating general heap fragmentation during heavy streaming and destruction.

---

### 3.2 Concurrency and Data Ownership Contract

This section is **normative**.

#### Authoritative State

* Each chunk slot owns a `std::shared_mutex` (or striped reader-writer lock), a canonical $32^3$ TSDF representation (`EMPTY`, `SOLID`, or `DENSE`), an optional $32^3$ material array, an atomic `uint64_t version`, and an atomic reference count.
* The Core owns a global atomic `uint64_t terrain_revision` incremented once per completed **Edit transaction** that modifies at least one canonical sample.

#### Mutation Rule

* **Initialization (`Loading` / `Generating`):** A Load or Generate worker populates a chunk's canonical $32^3$ data while the chunk is in `Loading` or `Generating` state under an exclusive write lock, sets `version = 1`, and transitions the chunk to `Resident`. Initialization never increments `terrain_revision`.
* **Editing (`Resident`):** Only **Edit** jobs may mutate a `Resident` chunk's canonical $32^3$ TSDF or material samples, and only while holding that chunk's exclusive write lock. Every phase that modifies a chunk increments that chunk's `version` while holding the write lock.

#### Edit Phases and Canonical-Only Lock Bound

Because `ChunkStore` stores only canonical $32^3$ samples (and no persistent derived aprons), **an Edit phase locks only the canonical owner chunks whose samples fall within that phase's edit footprint**:

* `max_edit_phase_core_chunks = 32`
* `max_edit_lock_chunks = 32` (exact 1:1 match with core chunks; neighbor chunks are not mutated during an edit phase and are never write-locked by it).
* All chunk write locks in a phase are acquired in strictly ascending `ChunkKey::raw` order and released in descending order.
* A phase never waits on I/O, memory allocation, completion queue capacity, or main-thread work while holding any chunk lock.
* **Transaction-Level Commit Visibility (No Mid-Explosion Tearing):**
An explosion spanning multiple phases accumulates its modified chunks and affected 3D navigation groups inside the `EditTransaction` state across phases. Only when the **final phase** of the transaction completes does the worker:
1. Run the boundary-crumb pruning pass across modified surface boundaries,
2. Increment `terrain_revision` once for the transaction,
3. Insert all affected chunks (and selectively dirtied 26-neighbors) into the high-priority `dirty_edit_chunks` set, and
4. Publish a single `EditTransactionCompleted` record to the MPSC completion queue.
This guarantees workers never mesh half-carved intermediate phase boundaries and `NavigationServer3D` never starts premature rebakes mid-explosion.



#### Read Rule (36³ Snapshot Isolation)

1. **Mesh Snapshot:** To mesh chunk $C$, a worker acquires shared read locks in ascending `ChunkKey::raw` order on the existing resident chunks in $C$'s 27-chunk neighborhood ($C$ plus its 26 neighbors) and copies the required canonical slices into its thread-local **$36^3$ (`-1..34`) scratch buffer** (~15–25 µs total memory copy).
2. Non-resident neighbors outside `world_height_chunks` (above the sky limit) fill their corresponding scratch slices with `kClampedAir`; below bedrock ($C_y \lt 0$) fills with `kClampedSolid`.
3. While holding the shared locks, the worker records a lightweight `NeighborhoodVersionStamp` (a hash/array of the 27 chunks' current `version` values) and the current `terrain_revision`. All read locks are released before Surface Nets execution begins.
4. Upon finishing Surface Nets, the mesh result is published only if chunk $C$'s version and neighborhood stamp still match; otherwise the result is discarded and chunk $C$ remains in its dirty set.
5. **Persist Snapshot:** A `PersistCompress` job acquires a shared read lock on chunk $C$ alone, reads `version_before`, copies/compresses only the $32^3$ canonical samples into `RegionBlobTable`, and verifies `version_after == version_before`.

---

### 3.3 Chunk Streaming and Residency Management

#### Streaming Inputs and Desired Set

* **Focus:** `update_stream_focus(position, velocity)` updates the streaming center once per frame on the main thread.
* **Active Set:** Chunks within Chebyshev radius `view_distance_chunks` of the focus chunk, clamped to $C_y \in [0, \text{world\_height\_chunks})$.
* **26-Neighbor Closure:** Every chunk in the 26-neighbor shell around active dirty chunks required to populate their $36^3$ mesh scratch windows.
* **Prefetch Set:** Chunks within `view_distance_chunks + prefetch_distance_chunks` (default $+2$), prioritized by dot product with `velocity`.

#### Load Routing (RAM Blob Cache → Region Disk TOC → Procedural Generation)

When a chunk transitions from `Absent` to `Requested`, the Streaming Manager checks in order:

1. **`RegionBlobTable` (in-memory compressed cache):** If an un-flushed or cached LZ4 blob exists in RAM for this chunk, enqueue a fast **Load** job from RAM.
2. **Region File TOC:** If the `8 × 8 × 8` region's TOC marks the chunk as `EDITED`, enqueue a disk **Load** job.
3. **Procedural Fallback:** Otherwise, enqueue a **Generate** job.

#### Memory Budget Ledger

`resident_memory_budget_mb` (default `512 MiB`) is a hard ceiling enforced via atomic reservation (`try_reserve(bytes)`) across four tracked pools:

* **Chunk Slab Pool:** $64.0\text{ KiB}$ per `DENSE` TSDF array + $32.0\text{ KiB}$ per allocated material array.
* **Worker Scratch Buffers:** Pre-allocated per worker thread at startup ($\text{num\_workers} \times 256\text{ KiB}$ total for the $36^3$ window and $34^3$ FastNoise2 buffer).
* **`RegionBlobTable` Cache:** Bounded to at most `32 MiB` of compressed LZ4 chunk blobs awaiting or cached after disk flush.
* **Completion Queue Buffers:** Vertex, index, and collision triangle arrays awaiting main-thread commit.

When total reserved bytes reach the budget ceiling:

1. Prefetch admission stops and existing prefetch chunks are evicted farthest-first.
2. `RegionBlobTable` flushes clean cached blobs.
3. If still constrained, effective active radius scales down and is reported via `get_effective_view_distance_chunks()`.

---

### 3.4 Worker Pool and Scheduling

#### Priority Lanes (Highest to Lowest)

1. **Edit:** Phased explosion/CSG transactions and isolated-crumb pruning.
2. **Mesh (Edit-Dirty):** Remeshing chunks from `dirty_edit_chunks` (ordered by distance to focus).
3. **Load:** Decompressing edited chunks from `RegionBlobTable` or region files.
4. **Mesh (Stream-Dirty):** Meshing newly loaded/generated chunks from `dirty_stream_chunks`.
5. **PersistCompress & RegionFlush:** In-memory LZ4 compression of edited chunks and throttled atomic `8 × 8 × 8` region disk writes.
6. **Generate:** Procedural terrain and cave generation for unedited chunks.

#### Dual Dirty-Set Architecture

To guarantee that background terrain streaming never delays visual or physical feedback from an explosion, dirty chunks are tracked in two separate deduplicated priority queues:

* **`dirty_edit_chunks`:** Populated when an Edit transaction completes. Always drained before `dirty_stream_chunks`.
* **`dirty_stream_chunks`:** Populated when `Load` or `Generate` initializes a chunk. Admitted only when `dirty_edit_chunks` is empty and completion queue depth is below the soft cap (`8` entries).

#### Fairness and Backpressure Bounds

| Parameter / Rule | Value | Purpose |
| --- | --- | --- |
| Worker thread count | `max(1, hardware_concurrency() - 2)` | Reserves 1 core for main thread and 1 for Godot render/server threads |
| Edit phase core/lock cap | `32` chunks max per phase | Bounds lock hold time to less than 0.5 ms per phase |
| Edit starvation bound | Max `8` consecutive Edit phases before yielding to `Mesh (Edit-Dirty)` | Ensures huge multi-phase blasts progressively stream meshes if needed |
| Soft completion cap | `8` entries | Pauses `Mesh (Stream-Dirty)` admission; reserves remaining queue slots for `Mesh (Edit-Dirty)` |
| Hard completion cap | `32` entries | Pauses all new `Mesh` admissions and new `Edit` phase starts until main thread drains below 32 |
| `PersistCompress` reservation | At least 2 chunks compressed to RAM per 250 ms | Keeps edited chunks eviction-ready without blocking on disk I/O |
| `RegionFlush` trigger | Every `10 s`, or when dirty compressed blobs exceed `4 MiB`, or on `save_world()` | Prevents disk write amplification during rapid explosions |

---

### 3.5 System Implementations

#### 1. Explosion CSG with $R + \delta$ Band Preservation & Anti-Crumb Pruning

An explosion request specifies world-space center $c$ and blast radius $R > 0$ (or a coalesced batch of up to 16 spheres $\{(c_m, R_m)\}$).

1. **Expanded Influence Bounds ($R + \delta$):**
Because the TSDF stores distances up to truncation band $\delta = 2.0$ voxel units, samples just outside the blast surface in the shell $R \le \lVert p - c \rVert \lt R + \delta$ have new signed distances in $(-\delta, 0]$. Failing to update this shell leaves those samples clamped at $-\delta$, producing severe stair-stepped crater walls.
The planner therefore computes the world sample AABB using the **expanded radius $R_{\text{eff}} = R + \delta$**:
$$\text{AABB}_{\text{sample}} = \big[\lfloor c - (R + \delta) \rfloor,\; \lceil c + (R + \delta) \rceil\big]$$


2. **Phased Canonical Mutation:**
The affected canonical owner chunks are partitioned into spatial $4 \times 2 \times 4$ phases (up to `32` chunks per phase). For each phase:
* Ensure all canonical chunks in the phase are `Resident` (loading/generating missing ones with no locks held).
* Acquire exclusive write locks on the phase's canonical chunks in ascending `ChunkKey::raw` order.
* Promote any touched `SOLID` chunk to `DENSE` (initializing samples to `kClampedSolid`). Skip `EMPTY` chunks entirely because subtracting a sphere from pure air (`kClampedAir`) is a no-op.
* For every canonical sample $p$ in the chunk inside $\text{AABB}_{\text{sample}}$:
$$d_{\text{blast}}(p) = \max_{m} \big(R_m - \lVert p - c_m \rVert\big)$$


If $d_{\text{blast}}(p) \gt -\delta$:
$$q_{\text{blast}} = \text{clamp}\!\left(\text{round}\!\left(\frac{d_{\text{blast}}(p)}{\delta} \times 32767\right), -32767, 32767\right)$$


$$S_{\text{new}}(p) = \max\big(S_{\text{current}}(p),\; q_{\text{blast}}\big)$$


* If all $32^3$ canonical samples in the chunk are now `+32767` (`kClampedAir`), demote the chunk to `EMPTY` and free its $64.0\text{ KiB}$ buffer back to the slab pool.


3. **Anti-Crumb Kernel (Isolated Cave Debris Pruning):**
In 3D caves, explosions near thin stalactites, pillars, or previous craters frequently leave 1-voxel isolated solid floaters ($\text{SDF} \lt 0$ surrounded on all 6 faces by $\text{SDF} \ge 0$). In the final phase of the transaction, for every modified chunk, a fast stencil sweep checks modified near-surface solid samples ($-\delta \lt S(p) \lt 0$): if all 6 face-adjacent samples have $S \ge 0$, $S(p)$ is set to `kClampedAir` ($+32767$). This eliminates floating visual specks and prevents tiny mid-air navmesh islands.
4. **Selective Dirtying & Transaction Completion:**
* Every chunk whose canonical samples changed is added to `dirty_edit_chunks`.
* If canonical sample `0, 1, or 2` changed on an axis, the $-1$ neighbor on that axis is added to `dirty_edit_chunks`.
* If canonical sample `31` changed on an axis, the $+1$ neighbor on that axis is added to `dirty_edit_chunks`.
* All 3D `NavigationGroupKey`s (`4 × 2 × 4` chunk groups) whose owned footprint or 3D border padding intersects the modified sample AABB are recorded in the single `EditTransactionCompleted` event and pushed to the MPSC completion queue.



#### 2. Procedural Generation (FastNoise2 Single-Pass $34^3$ Grid & 3D Caves)

To generate both realistic surface terrain and continuous, navigable 3D underground cave networks without evaluating FastNoise2 4 times per sample:

1. **Composite Surface + 3D Spaghetti Cave Field:**
For world sample $(x, y, z)$, the continuous density $D(x, y, z)$ (negative = solid, positive = air) is composed from:
* **Surface terrain density:** $D_{\text{surf}}(x,y,z) = (y - H_{\text{2D}}(x, z)) + N_{\text{3D\_overhang}}(x, y, z)$
* **3D tunnel cave density (dual-noise ridge intersection):** Two independent 3D Simplex/Perlin fields $N_{A}(x,y,z)$ and $N_{B}(x,y,z)$ define winding tunnels where both fields cross zero simultaneously:
$$D_{\text{cave}}(x,y,z) = r_{\text{cave}}(y) - \sqrt{N_A(x,y,z)^2 + N_B(x,y,z)^2}$$


where $r_{\text{cave}}(y)$ is the tunnel radius profile (attenuated near $y = 0$ bedrock so caves never breach the bottom of the world).
* **Combined density:** $D(x,y,z) = \max\big(D_{\text{surf}}(x,y,z),\; D_{\text{cave}}(x,y,z)\big)$.


2. **Single-Pass $34^3$ SIMD Grid Evaluation:**
Instead of 4 point evaluations per sample, the worker calls `FastNoise2::GenUniformGrid3D()` **once** into a thread-local **$34^3$ float buffer** covering local coordinates `[-1..32]` on each axis ($39,304$ floats $\approx 153.5\text{ KiB}$).
3. **In-Memory Central Differences & Regularized Gradient Normalization:**
For each canonical sample $(i, j, k) \in [0, 31]^3$, the gradient $\nabla D(i,j,k)$ is computed directly from adjacent entries in the $34^3$ buffer:
$$\nabla D(i,j,k) = \frac{1}{2s} \begin{bmatrix} D(i+1,j,k) - D(i-1,j,k) \\ D(i,j+1,k) - D(i,j-1,k) \\ D(i,j,k+1) - D(i,j,k-1) \end{bmatrix}$$


To prevent division by zero at cave ceilings, floors, and saddle points where $\lVert \nabla D \rVert \to 0$, the gradient magnitude is clamped with floor $\epsilon_{\text{grad}} = 0.05$:
$$\text{sdf}_{\text{approx}}(i,j,k) = \text{clamp}\!\left(\frac{D(i,j,k)}{\max\big(\lVert \nabla D(i,j,k) \rVert,\; \epsilon_{\text{grad}}\big) \cdot s},\; -\delta,\; +\delta\right)$$



#### 3. Seamless Surface Nets Meshing (over the $36^3$ Scratch Window)

1. **Scratch Reconstruction:** Gather canonical samples into the worker's $36^3$ (`-1..34`) scratch array under shared read locks.
2. **Corner Normals (`0..33`):** For any corner sample in `[0..33]³` referenced by an active cell, compute the surface normal via central differences across `[-1..34]³`.
3. **Dual Vertices (`0..32`, $33^3$ cells):** For each cell $(i, j, k) \in [0, 32]^3$, examine its 8 corners (`(i+a, j+b, k+c)` in `[0..33]³`). If all 8 corners have the same sign, the cell is inactive. Otherwise, compute the zero-crossing intersection points along all sign-changing edges of the cell, average them to position the cell's dual vertex $V(i,j,k)$, interpolate the corner normals to form the vertex normal, and select the material ID of the solid corner closest to the surface ($S \lt 0$ with largest $S$).
4. **Quad Stitching (`0..31` owned interior edges):** For each cell $(i, j, k) \in [0, 31]^3$, inspect the 3 grid edges exiting corner $(i+1, j+1, k+1)$ along $+X$, $+Y$, and $+Z$ (or equivalently the 3 interior edges owned by cell $(i,j,k)$ that connect 4 adjacent cells within `[0..32]³`). When an edge changes sign, emit 2 triangles connecting the 4 surrounding dual vertices in `[0..32]³`, winding triangles according to the sign direction of the edge.
* If a quad bridges a cell in `[0..31]³` and a boundary cell at index `32`, both dual vertices are computed identically by chunk $C$ and chunk $C+1$, producing **watertight, zero-gap, zero-overlap chunk seams**.


5. **Output Buffers:** Emit packed `Vector3` positions, `Vector3` normals, `Color` / custom vertex attributes (carrying `material_id`), `int32_t` index buffer, and a pre-flattened `Vector3` de-indexed triangle soup for `PhysicsServer3D` and `NavigationMeshSourceGeometryData3D` so the main thread performs zero CPU triangle unpacking.

---

### 3.6 Two-Stage Persistence and 3D `8 × 8 × 8` Region Files

#### Why 3D `8 × 8 × 8` Regions

v3.0 grouped chunks into full-height `32 × 32 × world_height_chunks` files (up to 16,384 chunks per file). Rewriting a whole region file on flush caused 50–100 MB disk write spikes.
v4.0 partitions the world into **3D regions of `8 × 8 × 8` chunks** (`512` chunks max per region file), addressed by:

$$\text{RegionKey} = \big(\text{floor\_div}(C_x, 8),\; \text{floor\_div}(C_y, 8),\; \text{floor\_div}(C_z, 8)\big)$$

Even when all 512 chunks in a region are heavily edited and dense, the compressed region file is typically **1.5–4.0 MiB**, allowing atomic write + `fsync` + `rename` to complete in a few milliseconds.

#### Two-Stage Save Pipeline

1. **Stage 1: `PersistCompress` (In-Memory Worker Job, ~50 µs per chunk):**
* Triggered shortly after an edited chunk stabilizes.
* Reads `version_before`, inspects the canonical $32^3$ chunk data:
* If `EMPTY` or `SOLID`, records a 0-byte `CHUNK_EMPTY` or `CHUNK_SOLID` entry in the region's in-memory `RegionBlobTable`.
* If `DENSE`, splits the $32^3$ `int16_t` canonical array into a $32,768$-byte low-byte plane and a $32,768$-byte high-byte plane (plus the $32,768$-byte material plane if allocated) and compresses each plane with `LZ4_compress_default`.


* Verifies `version_after == version_before` (if mismatched, discards and re-enqueues).
* Stores the compressed blob in the in-memory `RegionBlobTable` and marks the chunk clean for eviction. If the chunk is evicted from `ChunkStore` before a disk flush occurs, any subsequent `Load` reads directly from `RegionBlobTable`.


2. **Stage 2: `RegionFlush` (Throttled Background Disk Job):**
* Runs when a region's dirty blobs in `RegionBlobTable` exceed `4 MiB`, or `10 seconds` have elapsed since the first dirty blob in that region, or on `save_world()` / shutdown.
* Merges the in-memory `RegionBlobTable` with any existing untouched chunks from the live `.vreg` file, writes the complete header, 512-entry TOC, and compressed chunk payloads to `region_X_Y_Z.vreg.tmp`, calls `fsync` (`_commit` on Windows), and atomically `rename`s over `region_X_Y_Z.vreg`.



---

## 4. GDExtension Integration Strategy (Godot Wrapper)

### 4.1 Direct Server RID Management & Zero-Copy Prep

Creating a `MeshInstance3D` and `StaticBody3D` node per chunk at a 16-chunk view radius adds up to 70,000+ nodes to Godot's SceneTree. Instead, `VoxelTerrain3D` manages chunks via lightweight C++ `ChunkRenderSlot` structs holding direct Godot Server `RID`s:

```cpp
struct ChunkServerSlot {
    RID mesh_rid;            // RenderingServer::mesh_create()
    RID instance_rid;        // RenderingServer::instance_create()
    RID body_rid;            // PhysicsServer3D::body_create()
    RID shape_rid;           // PhysicsServer3D::concave_polygon_shape_create()
    PackedVector3Array cached_nav_faces; // Retained for 3D NavigationGroup rebakes
    uint64_t committed_version = 0;
    uint64_t committed_terrain_revision = 0;
};

```

Because the Mesh worker already builds the de-indexed triangle soup (`Vector3` triples) in core memory, committing a chunk on the main thread requires only a single `memcpy` into `PackedVector3Array::ptrw()` and passing that array to both `PhysicsServer3D::shape_set_data(shape_rid, faces)` and `ChunkServerSlot::cached_nav_faces`.

---

### 4.2 Class Mapping

#### `VoxelTerrain3D` (inherits `Node3D`)

* **Properties:**
* `String save_directory_path`
* `int view_distance_chunks` (default `12`)
* `int prefetch_distance_chunks` (default `2`)
* `int resident_memory_budget_mb` (default `512`)
* `float upload_budget_ms` (default `2.0`)
* `int world_height_chunks` (default `8`, i.e., 256 voxel cells tall)
* `bool save_on_exit` (default `true`)


* **Methods:**
* `void trigger_explosion(Vector3 position, float radius)`: Enqueues an asynchronous Edit transaction and returns immediately in less than `0.1 ms`. Never blocks.
* `void save_world()`: Flushes all pending `PersistCompress` and `RegionFlush` jobs synchronously.
* `void set_stream_focus(Vector3 position, Vector3 velocity)`: Updates the streaming center and motion bias vector.
* `bool core_idle() const`: True when all worker lanes and dirty queues are empty.
* `bool fully_committed() const`: True when `core_idle()` is true, the MPSC completion queue is empty, and `navigation_groups_synced()` is true.
* `int get_effective_view_distance_chunks() const`: Returns the active Chebyshev radius currently supported by `resident_memory_budget_mb`.


* **Signals:**
| Signal | Emitted when | Payload |
| --- | --- | --- |
| `explosion_requested(position, radius)` | Edit transaction accepted into the Edit lane | Call arguments |
| `explosion_completed(position, radius, affected_chunks)` | Final phase of the Edit transaction finishes and its `EditTransactionCompleted` record is processed on the main thread | Total modified canonical chunks across all phases |
| `explosion_failed(reason)` | Pool not running, radius $\le 0$, or edit backlog full | Diagnostic reason string |
| `chunk_committed(chunk_key)` | Visual RID, collision RID, and owning 3D navigation group are all live and synchronized | `int64_t` raw chunk key |



#### `VoxelAICompanion` (inherits `CharacterBody3D`)

* **Methods:**
* `PackedVector3Array get_voxel_path(Vector3 start, Vector3 end)`: Queries `NavigationServer3D::map_get_path()` on the terrain's 3D navigation map with `optimize = true`.
* `bool is_path_region_synced(Vector3 position) const`: Returns whether the 3D `4 × 2 × 4` navigation group containing `position` is currently synchronized to its `desired_terrain_revision`.



---

### 4.3 Main-Thread Commit Loop & Dynamic Backpressure

In v3.0, evaluating `const bool hard_backpressure = core_engine->completion_queue_depth() >= 32;` once *before* the drain `while` loop meant that whenever the queue hit 32 entries, the main thread ignored the time budget for the **entire loop** and committed all 32 chunks in one frame (causing a 50–100 ms freeze).

In v4.0:

1. **Per-Iteration Queue Depth Check:** Inside the `while` loop, `currently_over_cap = (core_engine->completion_queue_depth() >= 32)` is re-evaluated after **every single chunk commit**. If the queue entered `_process()` at 32 entries, committing just 1 chunk drops the depth to 31, immediately restoring normal `soft_budget_us` enforcement for the rest of the frame.
2. **Hard Emergency Frame Ceiling (`emergency_budget_us = 2 * soft_budget_us`):** Even under extreme worker throughput where workers refill slot 32 mid-frame, the main thread **never** spends more than `2 * upload_budget_ms` (default `4.0 ms`) in `_process()`. Workers simply pause at the 32-entry reservation gate until the next frame.

---

### 4.4 Physics Collision

* **Server-Direct Static Bodies:** Each resident chunk with non-empty geometry owns a `PhysicsServer3D` static body (`body_rid`) and a `SHAPE_CONCAVE_POLYGON` (`shape_rid`).
* **Pre-Unpacked Triangle Soup:** The Mesh worker outputs the de-indexed `Vector3` triangle soup (`3 vertices × triangle_count`) alongside the indexed render buffers. On the main thread, `commit_visual_and_collision()` copies this buffer into a `PackedVector3Array` once and shares the same `PackedVector3Array` (copy-on-write refcounted) between `PhysicsServer3D::shape_set_data()` and the chunk's `cached_nav_faces` slot.
* **Atomic Swap:** Updating a chunk's collision calls `PhysicsServer3D::shape_set_data(slot.shape_rid, faces)` in place, atomically swapping the internal BVH without removing or re-adding the body to the physics space.
* **Planned Escape Hatch (2× Half-Resolution Collision):** If profiling at M3 shows physics BVH construction exceeds `1.0 ms` per chunk on dense cave geometry, the Mesh worker can generate the collision soup from a $16^3$ stride-2 subsample of the $36^3$ scratch window ($4\times$ fewer triangles) without changing any main-thread or navigation code.

---

### 4.5 3D Cave Navigation (`4 × 2 × 4` Groups & Async Source Geometry Baking)

#### Why 3D Navigation Groups Are Mandatory for Caves

In v3.0, navigation groups were 2D `4 × 4` columns spanning the full world height `[0, world_height_chunks)`. In a multi-layered cave world, an explosion in a deep cavern at $C_y = 1$ invalidated the entire vertical column up to the sky, forcing Recast to revoxelize the surface terrain and every cave layer above it.

v4.0 partitions navigation into **3D `4 × 2 × 4` chunk groups** (`128 × 64 × 128` voxel cells = 32 chunks per group):

```cpp
struct NavigationGroupKey {
    int32_t gx; // ChunkKey::floor_div(Cx, 4)
    int32_t gy; // ChunkKey::floor_div(Cy, 2)
    int32_t gz; // ChunkKey::floor_div(Cz, 4)

    bool operator==(const NavigationGroupKey& o) const {
        return gx == o.gx && gy == o.gy && gz == o.gz;
    }
};

```

An underground explosion now invalidates only the local 3D `4 × 2 × 4` group(s) intersecting the blast, reducing Recast voxelization volume and latency by $4\times\text{–}8\times$.

#### 3D Border Padding (Horizontal & Vertical Seam Stitching)

To prevent Recast border erosion and allow AI companions to path seamlessly across horizontal (`gx`, `gz`) and vertical (`gy`, e.g., sloped cave tunnels and vertical shafts) group boundaries:

* Each group's **owned region AABB** is:
$$\text{AABB}_{\text{owned}} = \big[(128\,g_x,\; 64\,g_y,\; 128\,g_z) \cdot s,\; (128(g_x+1),\; 64(g_y+1),\; 128(g_z+1)) \cdot s\big]$$


* The group's **`NavigationMesh` baking AABB** is expanded outward by:
* **Horizontal padding ($\pm X, \pm Z$):** $P_{xz} = r_{\text{agent}} + r_{\text{connection}} + 2s$
* **Vertical padding ($\pm Y$):** $P_y = h_{\text{agent}} + h_{\text{max\_climb}} + 2s$


* Source triangles (`cached_nav_faces`) are gathered from the group's 32 owned chunks plus the adjacent border chunks whose bounding boxes intersect $\text{AABB}_{\text{owned}}$ expanded by $(P_{xz}, P_y, P_{xz})$.
* `NavigationMesh::set_border_size(P_xz)` clips the final navigable polygons cleanly back to $\text{AABB}_{\text{owned}}$ after erosion so adjacent 3D groups meet edge-to-edge without overlap or gaps, and `NavigationServer3D` stitches their edges across both horizontal and sloped vertical boundaries.

#### Cave-Specific `NavigationMesh` Parameters

Explosions inside caves create curved ceilings, steep crater walls, and small pockets. Each group's `NavigationMesh` resource is initialized with:

* `cell_size = 0.25f * s`, `cell_height = 0.20f * s`
* `agent_max_slope = 45.0f`
* `filter_low_hanging_obstacles = true` (prevents agents from pathing into low cave stalactites/ceilings)
* `filter_ledge_spans = true` (prevents agents from walking off blast cliffs)
* `filter_walkable_low_height_spans = true` (strictly enforces `agent_height` clearance inside cave tunnels)
* `region_min_size = 8.0f` (culls tiny unreachable navmesh islands created on blast ledges or cave ceilings)

#### Zero-Parse Async Baking Lifecycle (`less than 0.2 ms` Main-Thread Cost)

Instead of allowing `NavigationServer3D` to parse SceneTree nodes on the main thread, `VoxelNavigationManager` uses pre-assembled `NavigationMeshSourceGeometryData3D`:

```cpp
struct NavigationGroupState {
    bool live = false;
    RID region_rid;
    Ref<NavigationMesh> nav_mesh;
    Ref<NavigationMeshSourceGeometryData3D> source_data;

    uint64_t desired_terrain_revision = 0;
    uint64_t submitted_terrain_revision = 0;
    uint64_t synced_terrain_revision = 0;

    uint64_t desired_source_epoch = 0;
    uint64_t submitted_source_epoch = 0;
    uint64_t synced_source_epoch = 0;

    bool rebuild_in_flight = false;
    uint32_t pending_Callback_map_iteration = 0;
};

```

1. **Invalidation:** When `EditTransactionCompleted` arrives, every affected 3D `NavigationGroupKey` updates `desired_terrain_revision = edit_transaction.terrain_revision`. When a chunk loads/generates/unloads, affected groups increment `desired_source_epoch`.
2. **Readiness Check:** A dirty group is ready to submit when `rebuild_in_flight == false` and all resident chunks contributing to its owned + padded AABB have committed their meshes at or above the required version.
3. **Fast Async Submission (Main Thread, less than `0.2 ms`):**
* Clear `group.source_data->clear()`.
* For each contributing resident chunk in the group's 3D footprint + border halo, call `group.source_data->add_faces(chunk_slot.cached_nav_faces, Transform3D())`.
* Set `group.rebuild_in_flight = true`, `submitted_terrain_revision = desired_terrain_revision`, `submitted_source_epoch = desired_source_epoch`.
* Invoke:
```cpp
NavigationServer3D::get_singleton()->bake_from_source_geometry_data_async(
    group.nav_mesh,
    group.source_data,
    callable_mp(this, &VoxelNavigationManager::_on_group_bake_finished)
        .bind(key_packed, group.submitted_terrain_revision, group.submitted_source_epoch)
);

```




4. **Completion & Map Synchronization:**
* When `_on_group_bake_finished` fires on the main thread, the manager calls `NavigationServer3D::get_singleton()->region_set_navigation_mesh(group.region_rid, group.nav_mesh)` and records the current `NavigationServer3D::map_get_iteration_id(map_rid)`.
* On the next `NavigationServer3D` synchronization tick (when `map_get_iteration_id(map_rid)` advances), `synced_terrain_revision` and `synced_source_epoch` are updated to the submitted values and `rebuild_in_flight` is cleared.
* If `desired_*` advanced while the bake was in flight, the group immediately queues another async bake on the next frame.



---

## 5. Technical Implementation Details

### 5.1 GDExtension Entry Point (`register_types.cpp`)

```cpp
#include "register_types.h"
#include "voxel_terrain_3d.h"
#include "voxel_ai_companion.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_voxel_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    ClassDB::register_class<VoxelTerrain3D>();
    ClassDB::register_class<VoxelAICompanion>();
}

void uninitialize_voxel_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

extern "C" {
GDExtensionBool GDE_EXPORT voxel_engine_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization) {
    godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_voxel_module);
    init_obj.register_uninitializer(uninitialize_voxel_module);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}

```

### 5.2 Main-Thread Lifecycle & Commit Implementation (`voxel_terrain_3d.cpp`)

```cpp
#include "voxel_terrain_3d.h"
#include "core_voxel_engine.h"
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/world3d.hpp>

using namespace godot;

void VoxelTerrain3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("trigger_explosion", "position", "radius"), &VoxelTerrain3D::trigger_explosion);
    ClassDB::bind_method(D_METHOD("save_world"), &VoxelTerrain3D::save_world);
    ClassDB::bind_method(D_METHOD("set_stream_focus", "position", "velocity"), &VoxelTerrain3D::set_stream_focus);
    ClassDB::bind_method(D_METHOD("core_idle"), &VoxelTerrain3D::core_idle);
    ClassDB::bind_method(D_METHOD("fully_committed"), &VoxelTerrain3D::fully_committed);
    ClassDB::bind_method(D_METHOD("get_effective_view_distance_chunks"), &VoxelTerrain3D::get_effective_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_prefetch_distance_chunks"), &VoxelTerrain3D::get_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("set_prefetch_distance_chunks", "distance"), &VoxelTerrain3D::set_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_resident_memory_budget_mb"), &VoxelTerrain3D::get_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("set_resident_memory_budget_mb", "mb"), &VoxelTerrain3D::set_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("get_upload_budget_ms"), &VoxelTerrain3D::get_upload_budget_ms);
    ClassDB::bind_method(D_METHOD("set_upload_budget_ms", "ms"), &VoxelTerrain3D::set_upload_budget_ms);

    ADD_PROPERTY(PropertyInfo(Variant::INT, "prefetch_distance_chunks", PROPERTY_HINT_RANGE, "0,8,1"),
                 "set_prefetch_distance_chunks", "get_prefetch_distance_chunks");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "resident_memory_budget_mb", PROPERTY_HINT_RANGE, "128,4096,64"),
                 "set_resident_memory_budget_mb", "get_resident_memory_budget_mb");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upload_budget_ms", PROPERTY_HINT_RANGE, "0.5,16.0,0.5"),
                 "set_upload_budget_ms", "get_upload_budget_ms");

    ADD_SIGNAL(MethodInfo("explosion_requested",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius")));
    ADD_SIGNAL(MethodInfo("explosion_completed",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius"),
        PropertyInfo(Variant::INT, "affected_chunks")));
    ADD_SIGNAL(MethodInfo("explosion_failed",
        PropertyInfo(Variant::STRING, "reason")));
    ADD_SIGNAL(MethodInfo("chunk_committed",
        PropertyInfo(Variant::INT, "chunk_key")));
}

VoxelTerrain3D::VoxelTerrain3D() {
    core_engine = nullptr;
}

VoxelTerrain3D::~VoxelTerrain3D() = default;

void VoxelTerrain3D::_enter_tree() {
    core_engine = new CoreVoxelEngine();
    core_engine->configure_streaming(
        view_distance_chunks,
        prefetch_distance_chunks,
        resident_memory_budget_mb,
        world_height_chunks
    );
    core_engine->start();

    RID scenario_rid = get_world_3d()->get_scenario();
    RID space_rid = get_world_3d()->get_space();
    RID nav_map_rid = get_world_3d()->get_navigation_map();
    navigation_manager.initialize(nav_map_rid);
    server_slot_manager.initialize(scenario_rid, space_rid);
}

void VoxelTerrain3D::_exit_tree() {
    if (core_engine) {
        core_engine->shutdown(save_on_exit);
        delete core_engine;
        core_engine = nullptr;
    }
    navigation_manager.shutdown();
    server_slot_manager.free_all_rids();
}

void VoxelTerrain3D::trigger_explosion(Vector3 p_position, float p_radius) {
    if (!core_engine || p_radius <= 0.0f) {
        emit_signal("explosion_failed", String("engine not ready or radius is non-positive"));
        return;
    }

    CoreVector3 native_pos{ p_position.x, p_position.y, p_position.z };
    if (core_engine->enqueue_explosion(native_pos, p_radius)) {
        emit_signal("explosion_requested", p_position, p_radius);
    } else {
        emit_signal("explosion_failed", String("edit queue full, engine shutting down, or out of bounds"));
    }
}

bool VoxelTerrain3D::core_idle() const {
    return core_engine && core_engine->jobs_pending() == 0;
}

bool VoxelTerrain3D::fully_committed() const {
    return core_idle()
        && core_engine
        && core_engine->completion_queue_depth() == 0
        && navigation_manager.navigation_groups_synced();
}

void VoxelTerrain3D::_process(double delta) {
    if (!core_engine) {
        return;
    }

    const uint64_t frame_start_us = Time::get_singleton()->get_ticks_usec();
    const uint64_t soft_budget_us = static_cast<uint64_t>(upload_budget_ms * 1000.0);
    const uint64_t emergency_budget_us = soft_budget_us * 2;

    core_engine->update_stream_focus(stream_focus_position, stream_focus_velocity);
    navigation_manager.poll_server_synchronization();

    while (true) {
        std::optional<CompletionRecord> done = core_engine->try_pop_completion();
        if (!done) {
            break;
        }

        switch (done->kind) {
            case CompletionKind::ChunkMesh:
                server_slot_manager.commit_chunk_mesh_and_collision(done->chunk_mesh);
                navigation_manager.note_chunk_mesh_committed(
                    done->chunk_mesh.key,
                    done->chunk_mesh.captured_terrain_revision,
                    server_slot_manager.get_cached_nav_faces(done->chunk_mesh.key)
                );
                break;

            case CompletionKind::ChunkUnloaded:
                server_slot_manager.free_chunk_slot(done->unloaded_key);
                navigation_manager.note_chunk_unloaded(done->unloaded_key);
                break;

            case CompletionKind::EditTransactionCompleted:
                navigation_manager.apply_edit_transaction(done->edit_transaction);
                emit_signal("explosion_completed",
                            Vector3(done->edit_transaction.position.x,
                                    done->edit_transaction.position.y,
                                    done->edit_transaction.position.z),
                            done->edit_transaction.radius,
                            static_cast<int>(done->edit_transaction.total_affected_chunks));
                break;
        }

        const uint64_t elapsed_us = Time::get_singleton()->get_ticks_usec() - frame_start_us;
        const size_t remaining_depth = core_engine->completion_queue_depth();
        if (remaining_depth == 0) {
            break;
        }

        // Dynamic per-iteration check: only bypass soft_budget_us while depth is at/above 32,
        // and never exceed emergency_budget_us on any single frame.
        const bool currently_over_cap = (remaining_depth >= 32);
        if (!currently_over_cap && elapsed_us >= soft_budget_us) {
            break;
        }
        if (elapsed_us >= emergency_budget_us) {
            break;
        }
    }

    // Async navigation group submissions (< 0.2 ms main-thread cost per group).
    navigation_manager.submit_ready_groups_to_server();

    for (const ChunkKey key : navigation_manager.drain_newly_committed_chunks()) {
        emit_signal("chunk_committed", static_cast<int64_t>(key.raw));
    }
}

```

---

### 5.3 Ordered Teardown Protocol

`CoreVoxelEngine::shutdown(bool save)` executes one deterministic sequence:

| # | Step | Acting Thread |
| --- | --- | --- |
| 1 | Set `shutting_down = true`. Reject all new job submissions. | Caller (Main) |
| 2 | Wake all worker threads. Workers finish their active job or current Edit phase (max 32 chunks), release all locks, and exit their run loop. | Workers |
| 3 | Join all worker threads. | Caller (Main) |
| 4 | If `save == true`, run remaining `PersistCompress` jobs and execute `RegionFlush` inline for all dirty `8 × 8 × 8` regions in `RegionBlobTable`. If `save == false`, discard `RegionBlobTable`. | Caller (Main) |
| 5 | Drain and discard all remaining entries in the MPSC completion queue, releasing their memory reservations. | Caller (Main) |
| 6 | Release `ChunkStore`, `RegionBlobTable`, and the slab pools. Assert that all chunk reference counts and memory ledger bytes are zero. | Caller (Main) |
| 7 | Free all `NavigationRegion3D`, `RenderingServer`, and `PhysicsServer3D` RIDs on the wrapper side. | Caller (Main) |

Note on Step 3 & Step 4: Because no worker thread is running after Step 3's `join()`, the caller thread in Step 4 has exclusive, lock-free access to `ChunkStore` and `RegionBlobTable`, making inline final compression and region flushing completely race-free.

---

## 6. Build and Deployment

### 6.1 Build Workflow and Static Core Linkage

* **Static Linkage (`voxel_core` → `libvoxel_engine`):** CMake builds `voxel_core` as a position-independent static library (`CMAKE_POSITION_INDEPENDENT_CODE ON`) with zero Godot headers. The GDExtension target links `voxel_core`, `godot-cpp`, `FastNoise2`, and `lz4` into a single platform binary (`libvoxel_engine.*`).
* **Toolchain Requirements:**
* Godot: `4.2-stable` or later (verified for `NavigationServer3D::bake_from_source_geometry_data_async`).
* C++ Standard: **C++20** (for `std::shared_mutex`, `std::atomic::wait`, and `constexpr` math helpers).
* Compiler: MSVC v143+ (`/MD` release, `/MDd` debug) on Windows; Clang 15+ or GCC 12+ on Linux/macOS.



### 6.2 Extension Descriptor (`bin/voxel_engine.gdextension`)

```ini
[configuration]
entry_symbol = "voxel_engine_init"
compatibility_minimum = "4.2"

[libraries]
windows.debug.x86_64   = "res://bin/libvoxel_engine.windows.debug.x86_64.dll"
windows.release.x86_64 = "res://bin/libvoxel_engine.windows.release.x86_64.dll"
linux.debug.x86_64     = "res://bin/libvoxel_engine.linux.debug.x86_64.so"
linux.release.x86_64   = "res://bin/libvoxel_engine.linux.release.x86_64.so"
macos.debug            = "res://bin/libvoxel_engine.macos.debug.dylib"
macos.release          = "res://bin/libvoxel_engine.macos.release.dylib"

```

---

## 7. Verification Strategy

### 7.1 Unit Tests (Core C++, No Godot)

#### Concurrency, Ownership & Phased Edits (3.1, 3.2, 3.4)

* **Canonical sample mapping:** Verify `ChunkKey::floor_div` and `floor_mod` across negative, zero, and positive chunk boundaries.
* **Canonical-only lock cap:** Trigger a synthetic explosion with $R = 64$ voxels (spanning $> 100$ chunks) and assert that each Edit phase locks at most `32` canonical chunks (`max_edit_lock_chunks = 32`).
* **Selective neighbor dirtying:**
* Mutate sample $(15, 15, 15)$ in chunk $C$: assert **only** chunk $C$ is added to `dirty_edit_chunks` (0 neighbors dirtied).
* Mutate sample $(2, 15, 15)$ in chunk $C$: assert only chunk $C$ and $C - \hat{x}$ are dirtied.
* Mutate sample $(31, 15, 15)$ in chunk $C$: assert only chunk $C$ and $C + \hat{x}$ are dirtied.


* **No mid-explosion tearing:** Verify that a 4-phase explosion publishes zero chunks to `dirty_edit_chunks` during phases 1–3 and publishes the full deduplicated set on phase 4.
* **Dual dirty-set priority:** Enqueue 50 chunks in `dirty_stream_chunks` and 4 chunks in `dirty_edit_chunks`; assert the 4 edit-dirty chunks are meshed first.

#### Geometry, $36^3$ Scratch Window & Cave CSG (3.1, 3.5)

* **$36^3$ Scratch Window Bounds:** Run Surface Nets under ASan (`-fsanitize=address`) and verify all corner normal reads (`-1..34`), corner sample reads (`0..33`), and dual vertex writes (`0..32`) remain strictly within the $36^3$ scratch array.
* **Multipart Watertight Seam Test:** Mesh an arbitrary cave + sphere SDF across a $2 \times 2 \times 2$ block of $32^3$ chunks and compare against a single $64^3$ reference mesh. Assert **zero boundary open edges** (every triangle edge is shared by an even number of faces across chunk seams) and identical vertex positions/normals at chunk boundaries.
* **$R + \delta$ Crater Wall Continuity:** Apply a sphere explosion of radius $R = 12.3$ and verify that canonical samples in the shell $R \le \lVert p - c \rVert \lt R + \delta$ contain smooth negative TSDF values matching $R - \lVert p - c \rVert$, with zero $-\delta$ cliff discontinuities adjacent to the zero-crossing.
* **Anti-Crumb Kernel:** Construct a 1-voxel solid sample ($S = -0.5$) surrounded by 6 air samples ($S = +1.0$) inside an explosion region; verify the post-phase pruning pass flips it to `kClampedAir`.
* **FastNoise2 $34^3$ Regularization:** Generate 1,000 cave chunks including saddle points where $\lVert \nabla D \rVert = 0$; assert zero `NaN`/`Inf` values and valid TSDF clamping via $\epsilon_{\text{grad}} = 0.05$.

#### Two-Stage Persistence & 3D `8 × 8 × 8` Regions (3.6)

* **RAM Blob Reload:** Edit a chunk, run `PersistCompress`, evict the chunk from `ChunkStore` *before* `RegionFlush` runs, then reload the chunk; verify bit-exact recovery from `RegionBlobTable`.
* **Crash-Safe Atomic Flush:** Truncate `.vreg.tmp` mid-flush and verify the previous `.vreg` file loads cleanly.

### 7.2 Integration Tests (Godot 4.2+)

* **Bounded Frame Time Under 32-Entry Backpressure:** Trigger 10 overlapping large explosions to saturate the 32-entry completion queue. Verify `_process()` never exceeds `emergency_budget_us` (`4.0 ms`) and never drains all 32 meshes in a single frame.
* **3D Cave Navigation Isolation:** Create a 2-layer world (surface at $C_y = 4$, cave tunnel at $C_y = 1$). Trigger an explosion in the cave at $C_y = 1$. Verify only the underground `gy = 0` (`Cy in [0, 1]`) navigation group rebakes while the surface `gy = 2` (`Cy in [4, 5]`) navigation group records zero rebuilds.
* **Sloped Cave Tunnel Boundary Pathing:** Carve a $35^\circ$ ramp connecting $C_y = 1$ (`gy = 0`) to $C_y = 2$ (`gy = 1`) and verify `VoxelAICompanion.get_voxel_path()` returns a continuous path across the vertical 3D navigation group boundary.

### 7.3 Performance Targets (Reference Hardware: 8-Core x86-64 CPU, NVMe SSD, Mid-Range GPU)

| Metric | v4.0 Target | Notes |
| --- | --- | --- |
| `trigger_explosion()` main-thread call | Less than `0.1 ms` | Enqueues transaction only |
| Main-thread commit cost, steady state mean | Less than `2.0 ms` per frame | Governed by `upload_budget_ms` |
| Main-thread commit cost, backpressure ceiling | At most `4.0 ms` (`2 * upload_budget_ms`) | Enforced by dynamic depth check + `emergency_budget_us` |
| Explosion time-to-visible mesh (4-chunk blast) | `1–2 frames` (less than `33 ms` at 60 FPS) | Prioritized by `dirty_edit_chunks` lane |
| Edit phase lock set | At most `32` canonical chunks | Hard invariant (`max_edit_lock_chunks = 32`) |
| Dense chunk resident memory in `ChunkStore` | `64.0 KiB` TSDF + `32.0 KiB` material (lazy) | Down from `115.7 KiB` in v3.0 |
| Worker $36^3$ scratch snapshot gather | Less than `30 µs` per chunk | Shared read locks released before Surface Nets |
| Navigation group async submission (main thread) | Less than `0.2 ms` per `4 × 2 × 4` group | Via `bake_from_source_geometry_data_async` (down from 10 ms in v3.0) |
| Navigation group background bake time (worker) | Less than `15 ms` per `4 × 2 × 4` 3D group | Isolated to 64-voxel vertical height band |
| `PersistCompress` RAM compression | Less than `60 µs` per dense chunk | Zero disk I/O |

---

## 8. Open Questions and Risk Mitigation

1. **Physics BVH Rebuild Cost on Complex Cave Meshes:** `PhysicsServer3D::shape_set_data()` builds the concave BVH synchronously on the calling thread. If p99 chunk collision commits exceed `1.2 ms`, enable the **2× stride collision subsample** escape hatch in the Mesh worker (Section 4.4).
2. **Multi-Material Triplanar Shading:** Because Surface Nets vertices carry a dominant `uint8_t` material ID (or top-3 material IDs across a triangle's vertices), the Godot `ShaderMaterial` should use a `Texture2DArray` with triplanar projection and per-vertex barycentric weight blending to avoid hard material seams on cave walls.
3. **Structural Connectivity (Large Floating Islands):** The $3 \times 3 \times 3$ Anti-Crumb Kernel removes 1–2 voxel floating debris. If gameplay requires large severed cave pillars (e.g., $10^3$ voxels) to collapse under gravity when detached from bedrock, a background 3D connected-component flood-fill job on the TSDF sign bit can be added as an optional post-explosion task in M5.

---

## 9. Milestones

| ID | Milestone | Exit Criteria |
| --- | --- | --- |
| **M0** | **Core Skeleton, Memory Ledger & Contract** | `ChunkKey` floor math, $32^3$ canonical `ChunkStore` (`EMPTY`/`SOLID`/`DENSE`), slab pools, $36^3$ worker scratch gather, 32-lock phased edits, selective neighbor dirtying, dual dirty queues (`dirty_edit_chunks` / `dirty_stream_chunks`), and ordered teardown pass under **TSan/ASan**. |
| **M1** | **SIMD Cave ProcGen & Seamless Surface Nets** | Single-pass $34^3$ `FastNoise2::GenUniformGrid3D` with $\epsilon_{\text{grad}} = 0.05$ regularization and 3D cave tunnels; Surface Nets over the $36^3$ scratch window (`0..32` dual vertices, `0..31` edge stitching) passes the $2 \times 2 \times 2$ zero-seam watertight test. |
| **M2** | **Real-Time Destruction ($R+\delta$ CSG & De-Crumb)** | Phased explosions evaluate $R + \delta$ shells, run the $3 \times 3 \times 3$ Anti-Crumb Kernel, publish transaction-level completions without mid-explosion tearing, and prioritize `dirty_edit_chunks`. |
| **M3** | **Godot Server-Direct Rendering & Physics** | `VoxelTerrain3D` commits meshes and concave collision via direct `RenderingServer` and `PhysicsServer3D` RIDs; `_process()` dynamic backpressure loop stays under `emergency_budget_us` during 32-entry saturation. |
| **M4** | **3D Cave Navigation & Two-Stage Persistence** | 3D `4 × 2 × 4` navigation groups bake via `bake_from_source_geometry_data_async` in less than `0.2 ms` main-thread time; `VoxelAICompanion` paths through multi-level caves and across vertical/horizontal group boundaries after blasts; `8 × 8 × 8` region files and `RegionBlobTable` pass crash tests. |
| **M5** | **LOD & Structural Physics** | Octree/clipmap LOD transitions and optional detached-island collapse. |

---

## Appendix A. Complete Issue Resolution Map (v1 → v4.0)

### v3.0 to v4.0 Audit Resolution Map

| # | v3.0 Audit Finding | Resolution in v4.0 |
| --- | --- | --- |
| **1** | **Surface Nets 1-voxel boundary gap & $35^3$ out-of-bounds normal read:** Emitting only cells `0..31` without cell `32`'s dual vertex left open seams between chunks; computing central-difference normals at corner `33` of cell `32` read sample `34` (outside `35^3`). | **Sections 3.1, 3.5:** Workers reconstruct a **$36^3$ (`-1..34`) scratch window**, compute normals at corners `0..33`, position dual vertices for cells `0..32` ($33^3$), and emit quads for edges exiting cells `0..31`. |
| **2** | **Redundant persistent aprons & impossible 96-lock cap:** A 32-chunk core phase (`4 × 2 × 4`) plus its 26-neighbor apron closure required 144 locks (exceeding `max_edit_lock_chunks = 96`). | **Sections 3.1, 3.2:** `ChunkStore` stores **only canonical $32^3$ samples (64.0 KiB)**. Edit phases lock only mutated canonical chunks (`max_edit_lock_chunks = 32`). Selective boundary dirtying marks neighbors dirty only when samples `0..2` or `31` change. |
| **3** | **Explosion CSG omitted the $\delta$ truncation shell:** Capping edits at radius $R$ left samples in $[R, R+\delta)$ clamped at $-\delta$, causing terraced crater walls and broken normals. | **Sections 3.5, 7.1:** Expanded CSG AABB and evaluation radius to **$R + \delta$**, preserving a smooth negative TSDF gradient across blast walls. |
| **4** | **Full-height Y navigation columns broke 3D caves:** `4 × 4` X/Z groups spanning `[0, world_height_chunks)` forced full-column surface + cave rebakes on underground blasts; also contained a `4 × 4 = 16×16` footprint typo. | **Section 4.5:** Replaced with **3D `4 × 2 × 4` chunk navigation groups** (`128 × 64 × 128` cells, 32 chunks) with 3D horizontal and vertical border padding, cave ledge filtering, and less than `0.2 ms` async submission via `bake_from_source_geometry_data_async`. |
| **5** | **`hard_backpressure` uncapped `_process()` loop:** Evaluating `hard_backpressure` once before the `while` loop drained all 32 meshes in 1 frame, causing 50–100 ms freezes. | **Sections 4.3, 5.2:** Queue depth (`remaining_depth >= 32`) is re-evaluated **every iteration** inside the loop, bounded by a hard `emergency_budget_us = 2 * soft_budget_us` frame ceiling. |
| **6** | **Mid-explosion visual tearing & premature nav bakes:** Intermediate Edit phases immediately published dirty meshes and nav invalidations. | **Sections 3.2, 3.4, 5.2:** Dirty chunks and `EditTransactionCompleted` are published atomically when the **final phase** of the explosion transaction finishes. |
| **7** | **FastNoise2 4× point-call overhead & $\lVert \nabla D \rVert \to 0$ NaN spikes:** Evaluating noise 4 times per sample lost SIMD speed; zero gradients at cave roofs/floors divided by zero. | **Section 3.5:** Single `FastNoise2::GenUniformGrid3D` call into a **$34^3$ buffer** + in-memory central differences, regularized by $\max(\lVert \nabla D \rVert, 0.05)$, with 2D surface + 3D dual-noise cave composition. |
| **8** | **Dirty-set priority inversion & SceneTree Node bloat:** Streaming and Edit shared one `mesh_dirty` set; per-chunk `StaticBody3D` nodes bloated the SceneTree. | **Sections 3.4, 4.1, 4.4:** Split into `dirty_edit_chunks` (high priority) and `dirty_stream_chunks` (low priority); replaced per-chunk `Node3D` instances with direct `RenderingServer` and `PhysicsServer3D` RIDs. |
| **9** | **Floating cave crumbs & full-region rewrite spikes:** Intersecting cave blasts left 1-voxel mid-air crumbs; `32 × 32 × Y` region flushes rewrote 50–100 MB files. | **Sections 3.5, 3.6:** Added a $3 \times 3 \times 3$ **Anti-Crumb Kernel** in the final Edit phase; resized regions to **3D `8 × 8 × 8` chunks** (512 chunks max) with two-stage `RegionBlobTable` RAM LZ4 caching. |