# High-Level Design Document (HLD) v3
## Project Core Architecture: Voxel Engine Library with Godot GDExtension Integration

**Status:** Draft v3.0
**Supersedes:** Voxel Design Doc v2.1 - Streaming.md
**Review input:** v2.1 implementation-readiness review plus v3 concurrency/navigation audit

---

## 0. Changes from v1

| Area | v1 | v2 |
| :--- | :--- | :--- |
| Threading | Explosion path runs synchronously on the main thread | Async job pipeline with an MPSC completion queue and a per-frame GPU upload budget |
| Lifetime | `delete core_engine` in destructor | Explicit ordered teardown with the acting thread named per step (5.2, 5.3) |
| Meshing | Transvoxel with no LOD structure | Uniform-resolution Surface Nets for v2.0. Transvoxel deferred to an LOD milestone |
| Chunk layout | 32³ core, no border data | 32³ cells, 33³ core-sample window, 35³ padded volume. Edits propagate to neighbor aprons |
| SDF format | `int8_t` or `float`, unnormalized | Truncated SDF (TSDF) stored as normalized `int16_t` with a fixed truncation band |
| Chunk keys | Signed 21-bit packing, sign extension unspecified | Offset encoding, no sign extension |
| Storage index | "Octree" in diagram, hash map in text | Flat open-addressing hash map. No octree in v2.0 |
| Collision | Not specified | Dedicated collision pipeline (new section 4.4) |
| Navigation | Standalone `dtTileCache` | Godot `NavigationServer3D` as the single navigation authority (new section 4.5) |
| Save format | RLE then LZ4 on float SDF | Homogeneous-chunk forms, quantized `int16_t`, byte-plane split, LZ4, crash-safe region files |
| Godot API | `PackedVector3Array::to_writer()` (nonexistent) | `resize()` + `ptrw()`, optional direct `RenderingServer` path |
| Registration | `VoxelAICompanion` not registered | Both classes registered at scene init level |
| Core linkage | Separate core DLL, C++ ABI across boundary | Core statically linked into the extension (6.1) |
| Library output | Windows, Linux, macOS x86_64 release only | Debug and release entries, arm64 for macOS |

## Changes from v2.0

| Area | v2.0 | v2.1 |
| :--- | :--- | :--- |
| Concurrency | Job versioning only | Normative concurrency and ownership contract (3.2): single writer, snapshot reads, reference-counted unload |
| Coordinate model | 32³ voxels, 34³ samples | 32³ core cells, 32³ canonical samples, 35³ padded volume (3.1) |
| Sample ownership | Apron and core symmetric | Canonical 32³ sample ownership; 33³ local corner window includes derived high-boundary slots (3.1, 3.2) |
| Apron sync | Edit path only | Edit, generate, load, and unload paths all defined (3.1, 3.5) |
| Noise density | Implicitly treated as SDF | Density normalized to an SDF with explicit scale and gradient magnitude (3.5) |
| Residency | 118 KB per chunk always | EMPTY/SOLID chunks use a compact representation; material channel lazily allocated (3.1) |
| Core linkage | Separate DLL, C++ ABI across boundary | Static linkage into the extension, or a C API if a DLL is required (6.1, 6.2) |
| Navigation data | Per-chunk regions, unspecified lifecycle | Region groups with defined lifecycle and border padding (4.5) |
| Save consistency | Unspecified | Version-bracketed serialization with retry-or-discard; 32³ canonical samples only (3.2, 3.5) |
| Shutdown | Persist flushed after join | Single ordered sequence, drain specified by thread (5.3) |
| Signals | `explosion_applied` ambiguous | `explosion_requested` / `explosion_completed` (4.2) |
| Readiness | `is_idle()` conflated | `core_idle()` and `fully_committed()` (4.2) |
| Scheduler | Priority only | Fairness, queue/memory backpressure, edit-phase limits, and dirty-set mesh admission (3.4) |
| Streaming / residency | `view_distance_chunks` declared but residency policy unspecified | Explicit desired set, prefetch, load/generate routing, 26-neighbor dependency closure, eviction, hard memory budget, and queue interaction (3.3, 3.4) |
| Thread start | Constructor | `_enter_tree` (5.2) |
| Navigation sync | `navigation_groups_synced()` referenced an undefined terrain revision/group state | Explicit global `terrain_revision`, per-group revision/epoch state, group dependency closure, and sync rules (4.5) |
| Edit locking | One edit could hold one lock set spanning an arbitrarily large explosion | Large edits are partitioned into bounded phases; each phase has a complete lock set and releases all locks before downstream backpressure (3.2, 3.4) |
| Completion coupling | Edit directly enqueued Mesh work, so one large edit could flood completion state | Edits mark chunks mesh-dirty; Mesh admission consumes dirty work under queue/memory caps (3.4) |
| Sample ownership | 33³ "core" samples were all described as authoritative despite shared chunk-boundary coordinates | Canonical 32³ sample ownership with a 33³ local mesh-sample window; boundary/high-face slots are derived (3.1) |
| Neighbor closure | Six face-neighbors were treated as sufficient for apron/mesh correctness | Full 26-neighbor dependency closure is defined for cached padded samples (3.2, 3.3) |
| Load mutation | Load reconstructed authoritative samples even though only Edit/Generate were allowed to mutate them | Load has a dedicated initialization write path before a chunk becomes Resident (3.2) |
| Edit coalescing | "Covering job" could be read as changing the CSG shape | Coalescing preserves the exact union of sphere CSG terms; it never replaces them with a bounding sphere (3.4) |
| Navigation source | Chunk completion mixed per-chunk nav data with group-level bake semantics | Mesh jobs produce reusable render/collision source; navigation rebuilds consume committed chunk meshes at region-group scope (4.3, 4.5) |

---

## 1. Executive Summary

### 1.1 Objective

Design a cross-platform, high-performance, **modular voxel engine written in C++** as a Godot-independent core library that is statically linked into the GDExtension for v3.x. The core has no dependency on Godot. The first integration wrapper targets **Godot 4.x** through the **GDExtension API** using **godot-cpp**.

### 1.2 Core Capabilities

* **Explosions:** Spherical CSG subtraction on the TSDF, applied by worker threads. Results reach the screen within a bounded number of frames.
* **AI Companion Navigation:** Navigation updates for modified regions are asynchronous and use Godot's `NavigationServer3D`.
* **Persistent Saves:** Chunk serialization with homogeneous-chunk elision, quantized SDF, and LZ4 block compression, stored in region files.
* **Procedural Generation:** Multi-threaded, deterministic terrain generation with **FastNoise2**.
* **Physics:** Static collision generated from the same meshes as the visuals, updated asynchronously when chunks are remeshed.

### 1.3 Non-Goals

* LOD and Transvoxel transitions (planned for milestone M5).
* Multiplayer replication of voxel edits.
* Non-voxel material blending beyond a single material ID per voxel. A material ID channel is reserved in the chunk format (see 3.1).

---

## 2. System Architecture Overview

The architecture enforces a strict boundary: the **Core** owns state, simulation, meshing, and persistence. The **Godot layer** owns presentation, physics bodies, navigation registration, and editor integration.

```
+-----------------------------------------------------------------------+
|                            GODOT ENGINE 4.x                           |
|  +---------------------------+       +-----------------------------+  |
|  |    VoxelTerrain3D Node    |       |      VoxelAICompanion       |  |
|  +-------------+-------------+       +--------------+--------------+  |
|                |  RenderingServer / PhysicsServer3D  |  NavigationAgent3D |
+----------------|-------------------------------------|------------------+
                 |                                     |
+----------------|-------------------------------------|------------------+
|                GDEXTENSION WRAPPER LAYER (godot-cpp)                  |
|  - Marshals mesh buffers into RenderingServer / ArrayMesh              |
|  - Owns Godot node lifetime, main-thread commit, budget enforcement    |
|  - Feeds navmesh + collision shapes to Godot servers                   |
+----------------------------------|-------------------------------------+
                                   | Pure C++ API (no Godot types)
+----------------------------------|-------------------------------------+
|                        CORE VOXEL ENGINE (libvoxel_core)               |
|  ChunkStore     WorkerPool       MeshGen          SaveSystem           |
|  (hash map,     (4 priority      (Surface Nets   (quantize + LZ4,     |
|   TSDF data)    lanes, aged)     v2.0)           region files)        |
|                                                                        |
|  ProcGen (FastNoise2)     Edit (CSG on TSDF)     Completion queue      |
+-----------------------------------------------------------------------+
```

Section 3.2 (**Concurrency and Data Ownership Contract**) is normative for every box above. It is not a detail to resolve during implementation.

---

## 3. Core Engine Pipeline (Pure C++)

The core operates on raw memory arrays, math structs, and explicit task scheduling. It has no knowledge of Godot.

### 3.1 Data Layout and Chunk Store

#### Coordinate Conventions

All positions below are **world sample coordinates**: continuous real-valued positions. Sample grid points are integers. The world origin sits at sample (0, 0, 0).

A chunk with chunk coordinate `C = (Cx, Cy, Cz)` owns a 32³ block of **cells**. Cell `(i, j, k)` with `i, j, k ∈ [0, 32)` has minimum corner at world sample:

$$p_{\min} = 32 \cdot C + (i, j, k)$$

The SDF sample `S` used by that cell at corner index `(a, b, c)` is at world sample `32 \cdot C + (i + a, j + b, k + c)`.

Consequences that implementations must honor:

* **Cells, not samples, are the unit of ownership.** A chunk owns 32³ cells along each axis, indexed `0..31`.
* **Voxel indices are ambiguity traps.** "32 × 32 × 32 voxels" is not used in this document. Use the terms **cell** and **sample**.
* **Ownership is inclusive of the low face, exclusive of the high face.** Cell 31 is the last cell a chunk owns. The neighbor chunk's cell 0 abuts it. No cell is owned twice and none is skipped.

#### Sample Layout

Meshing and normals need samples one step outside the owned cells on every side. Each chunk therefore maintains a **35³ local sample window**:

| Region | Local sample index range | Ownership / source | Count per axis | Notes |
| :--- | :--- | :--- | :--- | :--- |
| Canonical owned samples | 0 … 31 | **Authoritative** | 32 | Exactly one chunk owns each world sample in this half-open coordinate range |
| High boundary window | Any slot with local index 32 | **Derived** from the canonical owner | 1 boundary layer | Required by cells `0..31`; never an edit target and never independently authoritative |
| Low apron | −1 | **Derived** from canonical owners in neighboring chunks | 1 | Used by central-difference gradients |
| High apron | 33 | **Derived** from canonical owners in neighboring chunks | 1 | Used by central-difference gradients |

* **Canonical sample ownership:** for every integer world sample coordinate `p`, its authoritative chunk coordinate is `floor_div(p, 32)` independently on X/Y/Z, where `floor_div` means mathematical floor division (not C++ integer truncation toward zero). This makes ownership deterministic at chunk boundaries, including negative coordinates.
* A chunk's 33³ `0..32` window is therefore **not** 33³ authoritative samples. It contains 32³ canonical samples plus derived boundary slots at local index 32.
* The `35³` padded window remains the mesh/snapshot representation. Derived slots are reconstructed from canonical owners and are never an edit target.
* A changed canonical sample can affect any cell in the 26 adjacent chunk positions that reference that sample. Apron/boundary refresh therefore uses the full **26-neighbor dependency closure**, not only the six face-neighbors.
* **Cells remain the unit of geometric ownership:** a chunk owns the 32³ cells `0..31` on each axis. Sample ownership is a separate, canonical rule so shared chunk-boundary samples are not double-authoritative.

#### Sample Format

Truncated SDF stored as normalized `int16_t`.

* Truncation band: ±δ, with δ = 2.0 voxel units (configurable at build time).
* Encoding: `q = clamp(round(sdf / δ * 32767), -32767, 32767)`. Decoding: `sdf = q * δ / 32767`.
* Sign convention: **negative = solid, positive = air.** The surface is the `sdf = 0` isosurface.
* `INT16_MIN` and `INT16_MAX` are reserved as the clamped solid and clamped air markers. They mean "far beyond the band," not an exact distance.

* **v3 ownership correction:** a 32³ cell chunk still needs a 33³ local corner-sample window for meshing, plus one-sample aprons, so the padded window is **35³**. The 33³ window is not 33³ independent authoritative samples: shared/high boundary samples are derived from the canonical 32³ owner set.

#### Compact Residency Representation

A chunk's sample data is stored in one of three forms, and the form is updated as the chunk changes:

| Form | Memory | Meaning |
| :--- | :--- | :--- |
| `EMPTY` | 0 bytes of sample data | All **canonical owned samples** are at the clamped air marker |
| `SOLID` | 0 bytes of sample data | All **canonical owned samples** are at the clamped solid marker |
| `DENSE` | 35³ × 2 B ≈ **84 KB** | Full `int16_t` padded sample array, including derived apron samples |

* A chunk that generates as `EMPTY` or `SOLID` allocates no sample array. It must still record its form so it can be promoted to `DENSE` when an edit or a neighbor edit touches it.
* Promotion to `DENSE` fills the array with the clamped marker of the previous form before applying the edit.
* This matters because v2.0 allocated a full array for every resident chunk, making an air-heavy world cost far more memory than its actual voxel content justifies. A view distance of 16 chunks in radius around the player is roughly 36k chunks, and most are pure air or pure solid.

#### Material Channel

* Stored as a parallel `uint8_t` per **canonical owned sample**, **lazily allocated**.
* No material array exists while every material in a chunk is 0. The first write of a non-zero material allocates a **32³** canonical array. Derived boundary/apron material values are reconstructed from the canonical owner and are never written locally.
* Chunk memory per resident chunk is therefore **about 83.7 KiB for the 35³ TSDF window**, plus about **32 KiB** (`uint8_t × 32³`) when materials are present.
* Material writes use the same canonical ownership and edit-phase locking rules as TSDF writes.

#### Chunk Key

64-bit, offset-encoded. Each axis is stored as `coord + 2^20` in 21 bits, giving a range of [-1,048,576, 1,048,575] chunk coordinates per axis.

```cpp
struct ChunkKey {
    uint64_t raw;

    static constexpr int64_t kBias = 1LL << 20;
    static constexpr uint64_t kMask = (1ULL << 21) - 1;

    static ChunkKey from_coords(int32_t x, int32_t y, int32_t z) {
        uint64_t ux = static_cast<uint64_t>(static_cast<int64_t>(x) + kBias) & kMask;
        uint64_t uy = static_cast<uint64_t>(static_cast<int64_t>(y) + kBias) & kMask;
        uint64_t uz = static_cast<uint64_t>(static_cast<int64_t>(z) + kBias) & kMask;
        return ChunkKey{ ux | (uy << 21) | (uz << 42) };
    }

    int32_t x() const { return static_cast<int32_t>(raw & kMask) - static_cast<int32_t>(kBias); }
    int32_t y() const { return static_cast<int32_t>((raw >> 21) & kMask) - static_cast<int32_t>(kBias); }
    int32_t z() const { return static_cast<int32_t>((raw >> 42) & kMask) - static_cast<int32_t>(kBias); }
};
```

Offset encoding is used rather than packing two's-complement coordinates, because `int21_t` does not exist in C++ and manual sign extension on unpack is a recurring bug source.

#### Storage Index

* Flat open-addressing hash map (for example `absl::flat_hash_map` or `robin_hood::unordered_flat_map`). Values are **pool handles, not `std::unique_ptr`**. Node-based `std::unordered_map` is not used.
* Chunks are **not freed directly**. Unload decrements a reference count; a chunk is only destroyed when its refcount reaches zero. A chunk with queued or in-flight jobs holding a reference cannot be freed (see 3.2).

#### Mesh/Visual State Machine

```
Resident -> Clean <-> Dirty -> Meshing -> MeshReady -> Uploaded
                             ^                                |
                             +======= (version mismatch) =====+
                             +======= (unload: evict) ========+
```

Each chunk has a monotonically increasing `version` counter. Every authoritative mutation increments it (see 3.2). A mesh result is committed only if its captured version equals the chunk's current version. Otherwise it is discarded and the chunk returns to `Dirty`.

### 3.2 Concurrency and Data Ownership Contract

This section is **normative**. It defines exactly which state is authoritative, which jobs may write it, how large edit operations are partitioned, and how downstream backpressure interacts with editing.

#### Authoritative State

* A chunk's **canonical owned TSDF samples** (the 32³ sample positions owned by that chunk) and its **canonical material array** are authoritative.
* A chunk's 33³ local corner-sample window contains those 32³ canonical samples plus derived high-boundary slots. The boundary slots are **not authoritative**.
* `version` is a monotonically increasing per-chunk counter for authoritative content changes.
* The Core also owns a global `uint64_t terrain_revision`. It is defined precisely in section 4.5 and is used only to identify successful terrain-edit commits; it is not a replacement for per-chunk `version`.

#### Derived State

The following are caches derived from authoritative state and carry no weight in a dispute:

* Derived boundary/apron samples in a chunk's 35³ mesh window.
* Meshed vertex and index buffers.
* Collision triangle buffers and Godot collision shapes.
* Navigation source data, navigation region resources, and navigation server state.
* Dirty flags and scheduler bookkeeping.

#### Mutation Rule

* **Resident chunks:** only **Edit** jobs may mutate authoritative canonical TSDF or material samples after a chunk is `Resident`, and only while holding the chunk's exclusive write lock. There is at most one writer per chunk at any time.
* **Initialization:** **Load** and **Generate** jobs may initialize a newly admitted chunk's authoritative canonical samples while that chunk is still in `Requested` / `Loading` / `Generating` state and before it becomes `Resident`. They must hold that chunk's exclusive write lock for the initialization and publish the `Resident` state only after the authoritative data is complete.
* Initialization is not a terrain edit and therefore does **not** increment `terrain_revision`.
* Derived boundary/apron cache updates may write only derived slots. They never increment `version`.

#### Edit Phases and Lock-Set Bound

An edit request may span many chunks, but it is **never permitted to hold one lock set for the entire radius**.

**The lock set grows with the edit radius.** In v2.1, a single large explosion could therefore hold locks on a very large chunk set in one phase and then produce downstream work faster than the completion queue could absorb it. v3 treats both as explicit bounds: lock ownership is capped per phase, and Mesh completion admission is decoupled from the Edit phase through the deduplicated dirty set and queue reservations.

* The edit planner first computes the complete set of canonical sample-owner chunks whose authoritative samples may change.
* The planner partitions that work into deterministic **edit phases**. The default safety limits are:
  * `max_edit_phase_core_chunks = 32`
  * `max_edit_lock_chunks = 96`
* A phase may contain fewer core chunks when its required dependency closure would otherwise exceed `max_edit_lock_chunks`. The closure includes the phase's core owners **plus every resident chunk in the full 26-neighbor dependency closure whose derived boundary/apron cache may need refreshing**.
* **Each phase computes its complete lock set before taking any lock.** All locks in that phase are acquired in ascending `ChunkKey::raw` order and released in descending order.
* There is **no partial lock acquisition inside a phase**, but a large edit is explicitly allowed to execute multiple such phases. This is the v3 resolution to unbounded lock residency in a single explosion.
* The phase must not wait for completion-queue capacity, memory admission, navigation synchronization, or main-thread work while any phase locks are held. Those checks occur **before the next phase begins or after all phase locks are released**.
* The current v3 edit workload is subtractive CSG, so phases are allowed to yield between one another without changing the intended result: each sphere contributes a monotonic `max(current, sphere_sdf)` operation. Future non-commutative edit types must define their own transaction semantics before using phased execution.

#### Read Rule

**Mesh** and **Persist** jobs do not read authoritative samples while an Edit job may write them.

1. **Mesh snapshot:** the job captures a private **35³ padded sample window** by reading the canonical owners of all required world samples under shared/read locks, including the full 26-neighbor dependency closure as needed. The private snapshot is then immutable and decoupled from unload.
2. **Persist snapshot:** the job captures only the chunk's **32³ canonical owned samples** (plus its canonical material array when present). It does not serialize derived boundary/apron slots.
3. **Protected reads:** Persist may use a shared/read lock instead of a copy only if profiling proves the copy is materially more expensive. Mesh always snapshots.
4. A Mesh result records the `version` of every chunk contributing authoritative samples to its snapshot. A result is accepted only if all captured versions still match when the result is committed.

#### Apron and Boundary Propagation Rule

Derived sample caches are an optimization, not a correctness dependency.

1. The authoritative owner of every changed canonical sample is computed from the canonical ownership rule in 3.1.
2. During an edit phase, all affected resident dependent chunks in the 26-neighbor closure are included in the same phase lock set so their derived cache entries can be refreshed consistently.
3. A resident chunk whose derived cache is stale may defer its refresh; the next Mesh snapshot **reconstructs the 35³ window directly from authoritative canonical owners**, so stale derived slots can never produce incorrect geometry.
4. A chunk is marked `Dirty` when its authoritative samples changed or when a derived sample used by its owned cells changed sign. Sign-only normal changes remain intentionally ignored for v3 unless profiling shows the visual cost is unacceptable.
5. Generate/Load and Unload follow the same dependency rule. Adding or removing a chunk invalidates all resident dependent chunks whose padded windows reference that chunk.

The full 26-neighbor rule is required because a corner sample can participate in cells across face, edge, and corner-adjacent chunks. A six-neighbor-only propagation rule is insufficient for cached 3D Surface Nets windows.

#### Version Rule

* Every authoritative mutation to a **resident** chunk (TSDF or material) increments that chunk's `version` **while its write lock is held**.
* Load/Generate initialization sets the initial authoritative state before the chunk becomes `Resident`; it establishes the starting version and does not count as an edit revision.
* A Mesh job captures the versions of all contributing chunks at snapshot time.
* A result is accepted only if every captured version equals the corresponding current version at completion time.

#### Save Consistency Rule

A Persist job serializing chunk `C` records `version_before`, captures the 32³ canonical sample snapshot, then reads `version_after`.

* If `version_before == version_after`, the snapshot is consistent and may be written.
* If they differ, the chunk was mutated concurrently. The job **discards the snapshot and re-enqueues itself** at the tail of the Persist lane. It does not retry inline.

Without this rule, a save can serialize a partially old and partially new chunk.

#### Unload Rule

* A resident chunk owns one **residency reference**. Every dequeued job acquires an additional **job reference**; each job releases it when complete or discarded.
* `Unload` removes the residency reference and marks the chunk `EvictPending`. Actual destruction occurs only when the total reference count reaches zero.
* A queued job that resolves to a non-resident/evict-pending chunk before dequeue is dropped without execution.
* No chunk memory is freed while any job or completion result still holds a reference to it.

#### Completion Rule

* A heavy worker result owns its buffers (vertex data, index data, collision triangles/source data) until the main thread commits it.
* Worker code never touches Godot objects, RIDs, `Object`s, or Godot-owned allocator state.
* The MPSC completion queue is the single worker-to-main completion channel. It may contain both heavy chunk results and small edit-phase control events.
* Completion entries are charged against the same engine-managed memory budget used for streaming.

#### Edit-to-Mesh Handoff

Edit workers **do not enqueue one Mesh job per changed chunk directly**.

* A completed edit phase marks affected resident chunks `mesh_dirty` and inserts their keys into a deduplicated dirty set.
* The Mesh scheduler pulls dirty chunks only when completion-queue depth and completion-buffer bytes are below their admission caps.
* A chunk is removed from the dirty set only when its Mesh job is successfully admitted. If the job becomes stale, the chunk is marked dirty again.
* This decoupling means a single large explosion can mark thousands of chunks dirty without being able to overrun the MPSC completion queue in one phase.

#### Testing of This Contract

Every rule above maps to a unit or integration test in section 7.1/7.2. The TSan/ASan runs are mandatory CI gates.

### 3.3 Chunk Streaming and Residency Management

`view_distance_chunks` is a **residency target**, not a promise that the engine will allocate every chunk in that geometric range. The Streaming Manager maintains a bounded set of resident chunks around a moving focus point while honoring the hard memory budget below.

#### Streaming Inputs and Policy

* **Focus:** The Godot wrapper supplies the world-space focus position once per frame (normally the player or camera position) through a pure C++ `update_stream_focus()` call. The core converts it to the containing chunk coordinate and computes the desired residency set.
* **View distance:** `view_distance_chunks` is a 3D **Chebyshev radius** in chunk coordinates. A target radius `r` therefore contains at most `(2r + 1)^3` chunk coordinates before world-height and memory limits are applied. This gives a deterministic upper bound for admission planning.
* **Prefetch:** `prefetch_distance_chunks` (default `2`) extends the desired radius beyond the view radius. Prefetch is **soft**: it is admitted only when memory headroom exists after the active view and its required neighbors are resident.
* **Motion bias:** Within the prefetch shell, chunks in the current movement direction are ranked ahead of lateral and rear chunks. The wrapper supplies an optional focus velocity; zero velocity falls back to distance-only ordering.
* **World bounds:** Y coordinates are clamped to `[0, world_height_chunks)`. X/Z have no additional streaming bound beyond the `ChunkKey` range.

#### Desired-Set Construction and Load Order

Each streaming update produces three sets:

1. **Active set:** chunks within `view_distance_chunks` of the focus.
2. **Dependency closure:** every chunk in the full **26-neighbor adjacency** required to reconstruct active/dirty chunks' 35³ mesh-sample windows. Missing neighbors outside the world are treated as absent-air and do not consume a residency slot.
3. **Prefetch set:** chunks within `view_distance_chunks + prefetch_distance_chunks`, ranked by predicted usefulness. Prefetch never displaces the active set.

Missing chunks are admitted in this order:

1. Missing **dependency-closure chunks** needed to unblock a visible/dirty active chunk.
2. Missing **active chunks**, ordered by Chebyshev distance to the focus, then movement-direction score, then `ChunkKey::raw` as a stable tie-breaker.
3. Missing **prefetch chunks**, using the same ordering but lower priority.

For each requested chunk, the Streaming Manager first checks the region-file TOC for an `EDITED` record:

* If an edited record exists, enqueue a **Load** job.
* If no edited record exists, enqueue a **Generate** job.
* Load jobs therefore consume saved state when available; Generate is the fallback for unedited procedural terrain.

A streaming request reserves the chunk's worst-case **dense TSDF window** before the job enters a worker queue. The reservation also includes a material allocation if needed. Mesh snapshot, mesh-result, collision-source, and other transient buffers are reserved at their ownership transition and released when ownership moves or the result is discarded. A byte is never reserved twice: the memory ledger has one owner for each live allocation.

If a reservation would exceed the memory budget, the request remains queued but is not admitted. Prefetch requests are the first to yield under pressure.

 Edit-dependent phases may request only the canonical owners for the current phase plus their dependency closure; a huge explosion therefore does not require the entire explosion footprint to be resident simultaneously. The phase scheduler may stream the next phase after the current phase releases its locks.

 Every later allocation that is owned by the core/wrapper is charged against the same budget; an allocation that cannot be reserved causes that job to defer rather than exceed the hard cap.

#### Residency State

Residency is tracked independently from the mesh state machine in 3.1:

```
Absent
  -> Requested
  -> Loading / Generating
  -> Resident
  -> EvictPending
  -> Unloading
  -> Absent
```

* `Requested` means the chunk has a streaming demand but no worker has started it yet.
* `Loading` reads an edited chunk payload from disk and reconstructs its authoritative 32³ canonical samples. Aprons are reconstructed from resident neighbors; they are never loaded from disk.
* `Generating` creates the authoritative 32³ canonical samples procedurally, then participates in the same apron-refresh path as a loaded chunk.
* `Resident` means the authoritative core exists in the chunk store. A resident chunk may independently be `Clean`, `Dirty`, `Meshing`, `MeshReady`, or `Uploaded` in the mesh state machine.
* `EvictPending` means the chunk is no longer in the active/prefetch demand set and is a candidate for removal once persistence and in-flight references are clear.
* `Unloading` removes the chunk from the desired streaming set and invalidates derived state. Reference counting in 3.2 still controls the actual memory release.

#### Prefetch and Eviction Hysteresis

A chunk is not evicted merely because it crosses the active view radius. Chunks remain resident while they are inside the **prefetch radius**, which prevents immediate unload/reload thrashing as the focus moves near a chunk boundary.

Under normal operation, a chunk becomes an eviction candidate when:

* it is outside `view_distance_chunks + prefetch_distance_chunks`, and
* it has no edit, mesh, load, generate, persist, or completion work that requires residency.

Eviction candidates are ordered **farthest-first** from the current focus. Under memory pressure, the farthest chunks in the **prefetch set are allowed to evict even before they leave the prefetch radius**; this is the hysteresis exception used to recover memory without touching the active set. If additional space is required after all prefetch chunks are gone, the engine **reduces effective prefetch to zero** before considering active-set degradation.

Edited chunks cannot be evicted until their latest authoritative version has been successfully persisted. A failed save leaves the chunk resident and records the failure for the next Persist attempt.

#### Memory Budget

`resident_memory_budget_mb` is a hard ceiling for **engine-managed CPU memory associated with chunk streaming**, including resident authoritative sample/material storage, derived-window storage, streaming reservations, worker snapshots, dirty-set bookkeeping, and queued completion buffers owned by the core/wrapper. It is configurable and defaults to **512 MiB**.

The dense sample payload is:

* TSDF: `35^3 × 2 = 85,750` bytes ≈ **83.7 KiB** per dense chunk.
* Material: `32^3 = 32,768` bytes ≈ **32.0 KiB** when allocated.
* Conservative dense-with-material planning cost: about **115.7 KiB per chunk**, before mesh/collision/navigation resources.

The streaming controller therefore treats the memory limit as an admission constraint, not as an after-the-fact warning:

$$\text{resident\_bytes} + \text{reserved\_bytes} \le \text{resident\_memory\_budget\_mb} \times 2^{20}$$

`view_distance_chunks` is a **target** and is satisfied whenever the memory budget permits. When the requested radius cannot fit, the engine does not allocate past the hard limit: it first suppresses prefetch, then reduces the effective active radius to the largest affordable value. The effective radius is exposed in telemetry so the game can detect memory-constrained streaming.

For reference, a fully dense 16-chunk Chebyshev radius contains `33^3 = 35,937` chunk positions and would require roughly **2.9 GiB of TSDF storage alone** before materials or derived resources. The design therefore does **not** treat `view_distance_chunks = 16` as a memory guarantee; the memory budget is authoritative.

Godot/driver allocations that are not controlled by the extension allocator (for example GPU residency) cannot be made a strict process-level hard bound by the core. Their CPU-side estimates are tracked separately in performance telemetry, while the streaming budget above is the hard bound for memory owned by this engine pipeline.

#### Interaction with Load, Generate, and the Completion Queue

* **Load vs. Generate:** Load is a dedicated worker-lane queue for edited chunks; Generate remains the fallback for unedited chunks. Both reserve memory before admission and both produce a resident authoritative chunk that marks itself mesh-dirty for later Mesh admission.
* **Priority:** Edit and Mesh work outrank Load; Load outranks Persist and Generate for latency-sensitive streaming. Generate is lowest priority and is the first work throttled when the active view is incomplete due to memory pressure. Edit phases may yield between phases so that this priority can take effect.
* **Neighbor readiness:** A Mesh job for an active chunk is not admitted until every chunk needed by its 26-neighbor padded-sample dependency closure is either resident or known absent. This prevents a visible chunk from stalling on diagonal boundary samples.
* **Completion queue:** Load and Generate jobs do **not** enter the MPSC completion queue as heavy results because they initialize core state. Their successful completion marks the chunk mesh-dirty. Edit-phase control records and Mesh chunk results enter the completion queue; navigation is handled at group scope.
* **Completion backpressure:** The existing completion depth caps in 3.4 remain in force, and admission is additionally blocked when queued completion buffers consume the configured streaming memory budget. Backpressure therefore limits both queue length and bytes.
* **Eviction under load:** Eviction is a low-priority streaming action. It is never allowed to interrupt an Edit job, and it waits for the chunk's references to reach zero as required by 3.2.

#### Large-Edit Interaction

A large explosion may touch far more chunks than can be safely locked or remeshed in one step.

* The Streaming Manager does **not** reserve the explosion's whole affected set up front.
* The Edit lane uses the bounded phases from 3.2. Each phase acquires at most `max_edit_lock_chunks` locks, then releases them before any scheduler, queue, memory, or navigation wait.
* Edit phases only mark `mesh_dirty`; they do not push one Mesh completion per changed chunk. The Mesh scheduler admits dirty chunks according to the completion-depth and memory limits in 3.4.
* If the completion queue is at its hard cap when an edit phase finishes, the edit can yield before starting the next phase. No phase lock is retained during that wait.
* This explicitly couples large-edit behavior to downstream backpressure without allowing one explosion to hold a giant lock set or burst the completion queue.

#### Streaming Update Frequency

The Streaming Manager recomputes the desired set at most once per frame and only when the focus crosses a chunk boundary or the velocity direction changes enough to alter prefetch ranking. This avoids repeatedly rebuilding the same request set while the player moves within one chunk.

The stream planner itself runs on the main thread because it operates on small chunk-coordinate sets and only enqueues core jobs. Disk IO, decompression, generation, apron refresh, and meshing remain worker-side.

### 3.4 Worker Pool and Scheduling

#### Lanes (highest to lowest priority)

1. **Edit:** explosions and other density modifications.
2. **Mesh:** proximity-driven meshing and collision-source generation for dirty chunks.
3. **Load:** disk reads and decompression for edited chunks requested by the Streaming Manager.
4. **Persist:** serialization, compression, and disk writes.
5. **Generate:** procedural generation of new chunks.

#### Workers

`max(1, hardware_concurrency() - 2)` threads. The main thread is excluded, and one core is left for the OS and the Godot render thread.

Workers pull from lanes in priority order. Strict priority starvation is a real failure mode: sustained editing can starve Persist and Generate indefinitely. The rules below bound that while also bounding the size of any single Edit lock round.

#### Fairness Rules

| Rule | Value | Purpose |
| :--- | :--- | :--- |
| Aging | A job's effective priority increases by 1 lane every 250 ms in queue, capped at the top lane | Guarantees lower-priority work eventually runs without unbounded priority inversion |
| Edit starvation bound | At most 16 consecutive **Edit phases/jobs** before the scheduler must yield to Mesh or Load when such work is waiting | Prevents streaming/meshing from never running |
| Persist reservation | Persist jobs are never aged past Mesh or Load priority, but at least 1 Persist job runs per second while the queue is non-empty and the worker is not inside a required phase | Prevents unbounded save starvation |
| Load reservation | Load jobs outrank Generate and are admitted whenever an active chunk has a saved edit and memory headroom exists | Saved edits should not wait behind procedural generation |
| Generate preemption | Generate may run only after active Load work is admitted and only when the active chunk set still has memory headroom | Protects active-view latency |
| Edit coalescing | Intersecting explosion requests may be batched up to `max_edit_batch_spheres = 16`; batching evaluates the exact max of every sphere SDF term and **never** replaces the union with a bounding sphere | Reduces queue pressure without changing CSG semantics |
| Edit queue cap | `max_pending_edit_transactions = 64` by default | Prevents unbounded user-side edit backlog; enqueue fails cleanly when full |
| Edit phase core cap | `max_edit_phase_core_chunks = 32` by default | Bounds mutation work per lock round |
| Edit lock cap | `max_edit_lock_chunks = 96` by default | Hard upper bound on simultaneously held chunk locks by one phase |
| Phase yield | After every Edit phase, the transaction may yield before planning/acquiring the next phase | Prevents one huge explosion from monopolizing workers while retaining bounded lock hold time |
| Lane budget | A worker checks `shutting_down` between jobs and after every Edit phase | Bounds teardown latency |

#### Edit Phase Scheduling

An Edit transaction is a stateful job with a deterministic list of phase descriptors.

* A phase is the **only** unit allowed to acquire Edit locks.
* Before starting a phase, the scheduler checks `shutting_down`, completion-queue hard backpressure, and required memory reservations.
* Before a phase can lock anything, all canonical owner chunks needed by that phase must be **Resident**. Missing owners are requested through the streaming manager at Edit-dependency priority. The phase waits for residency with **no chunk locks held**. If the current phase cannot fit the memory budget, the planner subdivides it into smaller phases; the edit is deferred only when even the minimum viable phase cannot be admitted.
* A phase reserves **one completion-queue entry before acquiring any lock** for its mandatory `EditPhaseCompleted` control event. That reservation remains held until the phase completion record is published or the phase is cancelled. This prevents a phase from reaching completion and then being unable to report the terrain revision because other workers filled the queue meanwhile.
* Once the phase begins, it must acquire its precomputed complete lock set and finish or reach a defined cancellation safe point; it does not wait for downstream queues.
* After releasing all phase locks, the transaction updates its progress and returns to the scheduler. If backpressure is active, it remains queued without holding any chunk lock.
* Mesh jobs are not created eagerly for every changed chunk. The dirty-set scheduler below performs downstream admission.

#### Backpressure Rules

| Condition | Response |
| :--- | :--- |
| Completion queue depth >= 8 entries | Stop admitting new Mesh results; continue Edit/Load work and honor the Persist reservation |
| Completion queue depth >= 32 entries | Hard backpressure: do not start another Edit phase until the main thread drains the queue. Load may continue only when it cannot add a completion result that would exceed the hard cap |
| Completion-buffer bytes at budget limit | Stop admitting heavy Mesh/collision results; run only work whose allocations already fit the budget |
| Streaming memory budget exhausted | Stop admitting prefetch and Generate work; continue only work required to keep the active dependency closure resident or persist edited chunks |
| Pending unload of a chunk with in-flight jobs | Unload is deferred and retried; reference counting controls actual memory release |
| Dirty-set backlog large | Keep dirty keys deduplicated; Mesh admission continues in priority order as completion capacity becomes available |

#### Completion Queue Contract

The queue has two explicit limits:

* **Entry-depth limit:** 32 hard entries, with 8 as the soft stop threshold for new Mesh admissions.
* **Byte limit:** all completion-owned buffers are charged to `resident_memory_budget_mb`; a result that would cross the budget is not allocated.

To make the hard limits race-free, producers use a small **completion-admission reservation** before allocating/queuing a result:

* A worker atomically reserves one completion entry before starting a Mesh result that will be published.
* It reserves the expected completion-buffer bytes before allocating those buffers. Actual allocations must also succeed through `try_reserve(bytes)`; a failed reservation aborts or defers the result before exceeding the cap.
* An `EditPhaseCompleted` control event reserves one completion entry before its phase acquires any lock; the reservation is consumed when that control event is published and released on cancellation.
* A reservation is released when the result is committed, discarded, or otherwise removed from the queue.

These are downstream limits. **An Edit transaction cannot bypass them** because an edit phase only mutates authoritative state and marks dirty keys. Mesh work is admitted later, one chunk at a time, under the same reservations.

The completion-queue cap exists because unbounded accumulation would let a large edit produce unbounded Mesh/collision buffers before the main thread can commit them. v3 prevents that by moving queue admission out of the Edit operation itself.

#### Cancellation

Every job checks a shared `std::atomic<bool> shutting_down` at safe points: between ordinary jobs and between Edit phases. Edits already applied to authoritative state are never rolled back by cancellation.

#### Thread Start

The worker pool is created by `VoxelTerrain3D::_enter_tree()`, not in the constructor. Godot instantiates nodes freely in the editor, in the resource loader, and in the scene-instantiation path before they enter the tree. Spawning a pool in a constructor leaks a pool per preview instantiation.

### 3.5 System Implementations

#### Explosion CSG (TSDF subtraction)

An explosion payload contains center `c` and radius `R`. The core:

1. Validates `R > 0` and computes the axis-aligned sphere bounds in world sample coordinates.
2. Maps every potentially changed world sample to its **canonical owner chunk** using `floor_div(sample, 32)`.
3. Computes the complete affected-owner set, then partitions it into the bounded Edit phases defined in 3.2. Every phase includes its full 26-neighbor derived-cache dependency closure in the lock set.
4. For each phase, acquires the complete lock set in ascending `ChunkKey::raw` order. While the locks are held, for every canonical sample `p` owned by the phase:

$$\text{NewSDF}(p) = \max\big(\text{CurrentSDF}(p),\; R - \lVert p - c\rVert\big)$$

The result is clamped to the truncation band ±δ. Under the negative-solid convention, samples inside the sphere become air.

5. **Only canonical samples are written.** Derived boundary/apron slots are refreshed from their canonical owners but are never edit targets.
6. Each changed resident owner chunk increments its `version` while its write lock is held and is marked `mesh_dirty`.
7. After all phase locks are released, the phase increments the global `terrain_revision` exactly once if at least one canonical sample changed. The phase completion record carries that revision and the affected navigation-group keys.
8. The phase then yields back to the scheduler. It does not wait for the completion queue while locks are held.
9. After the final phase, the logical explosion request is complete. `explosion_completed` is emitted only after its final edit completion record reaches the main thread, not merely when its first phase finishes.

The Edit worker **does not enqueue a Mesh job for every changed chunk**. Dirty chunks are deduplicated and consumed by the Mesh scheduler under the completion/memory caps in 3.4.

This phased design is intentionally different from v2.1's single giant lock round: a very large explosion may touch hundreds or thousands of chunks, but no worker ever holds locks for that entire set at once.

#### Procedural Generation (FastNoise2)

* Generation runs on the Generate lane, one chunk per job.
* **FastNoise2 produces a density field, not a true SDF.** v3 therefore treats the generated field as an **SDF approximation** obtained from a first-order local distance estimate:

$$\text{sdf}_{approx} = \frac{\text{density} - \text{iso}}{\lVert \nabla \text{density} \rVert \cdot s}$$

where `iso` is the iso-level of the noise type, `∇density` is the gradient magnitude in voxel units, and `s` is the voxel scale. This normalization reduces local distance-scale distortion near the isosurface, but it is not an exact signed-distance transform for arbitrary noise fields. The implementation must validate **surface-position error, near-surface gradient magnitude, and repeated CSG stability** before treating the approximation as sufficient. The gradient is evaluated by finite difference of the noise, so generation costs roughly 4 noise evaluations per sample.
* The generator initializes the chunk's **32³ canonical owned samples**. The local 33³ corner window and 35³ padded window are then reconstructed from canonical owners and resident neighbors.
* The chunk is promoted to `EMPTY` or `SOLID` if all canonical owned samples are homogeneous, and no TSDF sample array is allocated in that case.
* Output is deterministic for a given seed, noise graph, and SIMD level. Cross-machine determinism is **not** guaranteed by FastNoise2's SIMD dispatch. Saved edits are stored as full canonical chunk data, so a change in generation output does not corrupt existing saves. Re-generation of unedited chunks may differ across machines; accepted for v3, and listed as a risk in section 8.
* Generation uses the initialization write path from 3.2. It does not increment `terrain_revision`.
* Marks the chunk `Dirty` for meshing, then updates derived sample caches for resident dependent chunks.

#### Apron and Boundary Consistency Across Lifecycle

Derived boundary/apron samples are **cache data**. Correctness comes from canonical-owner reconstruction, with eager refresh as an optimization.

| Lifecycle event | Apron/boundary behavior |
| :--- | :--- |
| **Edit** | Changed canonical samples are propagated to all resident dependent chunks in the full 26-neighbor closure after the phase's authoritative writes |
| **Generate** | The new canonical chunk is initialized first; resident dependents refresh derived windows from canonical owners |
| **Load** | Canonical 32³ samples come from disk. The 35³ local window is reconstructed from canonical owners; no derived boundary/apron samples are loaded from disk |
| **Unload** | Resident dependents invalidate derived windows that referenced the removed chunk. The next Mesh snapshot reconstructs those values using the current canonical world and absent-air rules |

A chunk does not have to be fully surrounded by resident neighbors before its authoritative data exists. However, Mesh admission waits until every chunk in the required 26-neighbor padded-sample dependency closure is either resident or known absent, so the snapshot is deterministic.

#### Meshing (Surface Nets for v3.0)

* **Algorithm:** Naive Surface Nets over a private 35³ padded sample snapshot.
* **Snapshot source:** the job reconstructs each required sample from its canonical owner, including derived high-boundary and apron values. The snapshot is independent of later edits and unload.
* **Cell ownership:** a chunk emits geometry only for its 32³ owned cells (local `0..31`), using the 35³ padded samples so its outer cells close correctly against boundary values. A chunk never emits the cell at local index 32; that cell belongs to the neighbor.
* **Gradient normals:** computed by central difference of the padded snapshot.
* **Output:** interleaved vertex buffer (position, normal, material), `uint32_t` index buffer, per-chunk bounds, and collision triangle source data. Ownership moves to the completion queue.
* **Navigation:** Mesh does **not** produce a separate per-chunk navigation result. The committed Surface Nets mesh is reused as source geometry for region-group navigation rebuilds defined in 4.5.
* **Why not Transvoxel for v3:** Transvoxel's transition cells exist to bridge chunks of different resolution. v3 has one resolution, so those tables add cost with no benefit. LOD milestone M5 adopts Transvoxel with an octree or clipmap structure and transition masks.

#### Save and Load Pipeline

Per chunk, in order:

1. **Homogeneous form:** If every **canonical owned sample** in the 32³ chunk sample set is at the clamped air marker, store `CHUNK_EMPTY`. If every canonical owned sample is at the clamped solid marker, store `CHUNK_SOLID`. Neither carries a sample payload. Derived boundary/apron values are not considered for the homogeneous test.
2. **Serialize canonical samples:** Otherwise the chunk's **32³ canonical `int16_t` sample array** is written as two byte planes (low byte, high byte). Derived boundary/apron samples are never serialized. The material channel, when present, is a third plane containing the **32³ canonical material samples**.
3. **Compress:** `LZ4_compress_default` per plane. Compressed length is recorded in the TOC.
4. **Version bracket:** The Persist job records `version_before`, snapshots canonical samples, and reads `version_after` (3.2).
5. **Write:** Into a region file (see 3.6).

RLE is not in the pipeline. Only **edited** chunks are written; an unedited chunk is regenerated on load, so a save file contains the delta from generation, not the world. Chunks carry an `edited` flag, set by the first authoritative mutation and cleared only by an explicit regeneration.

Load is the inverse of this process: it reconstructs the canonical 32³ sample set, initializes the chunk under the Load initialization write path, then reconstructs derived boundary/apron values from canonical owners as neighbors become available.

### 3.6 Region File Format

* Chunks are grouped into **regions of 32 × 32 chunks** in X and Z, with Y as a vertical stack bounded by `world_height_chunks`.
* Each region file starts with a fixed header and a **table of contents (TOC)**:
  * Magic, format version, region coordinates.
  * Per-chunk entry: byte offset, compressed length, uncompressed length, flags (`EMPTY`, `SOLID`, `RAW`, `EDITED`), and a save timestamp.

#### Crash Safety

v2.0 described two conflictrites: "append and update the TOC," which requires an in-place TOC write, and "write a temp file and rename atomically," which rewrites the whole region. An in-place TOC update is not crash-safe, so the region format is defined as follows, with one consistent rule:

* **Durability unit is the region file, not the chunk.**
* A flush writes the complete region state to `region.tmp`, `fsync`s it, then `rename`s over the live file. `rename` is atomic on Windows and POSIX, so the live file is always a complete region.
* **There is no in-place TOC update.** The TOC lives in RAM and is only committed to disk on flush. Frequent small edits therefore batch, and the flush is triggered by a dirty-chunk count or a time interval, not per edit.
* A crash therefore loses at most the chunks written since the last flush. That is the accepted loss window, and it is why the Persist lane must not be starved (3.4) and why `save_on_exit` exists.

#### Compaction

* During normal operation, each flush rewrites the region file in full, so there is no append-log growth and no need for the separate compaction pass from v2.0.
* If a region exceeds the configured size limit, it is split into child regions at flush time and the parent TOC redirects. This is a maintenance pass triggered manually, not a runtime path.

---

## 4. GDExtension Integration Strategy (Godot Wrapper)

Godot 4 uses **GDExtension** rather than GDNative. The wrapper compiles into an extension that links against the core library.

### 4.1 Memory Allocation and Marshalling

Large buffers are copied once, from core memory into Godot-owned memory, on the main thread during commit. The copy is bounded by the upload budget (see 4.3).

```cpp
PackedVector3Array verts;
verts.resize(vertex_count);
memcpy(verts.ptrw(), core_mesh.positions, vertex_count * sizeof(Vector3));
```

Note: `Packed*Array` is copy-on-write and reference-counted. `ptrw()` triggers a unique copy if the array is shared, so call it only after `resize()` on an array you own.

For maximum throughput, the wrapper can bypass `ArrayMesh` and use `RenderingServer` directly:

```cpp
RID mesh_rid = RenderingServer::get_singleton()->mesh_create();
RenderingServer::get_singleton()->mesh_add_surface_from_arrays(mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
```

`ArrayMesh` is the default. `RenderingServer` is an opt-in optimization, to be enabled only if profiling shows `ArrayMesh` overhead matters.

### 4.2 Class Mapping

#### VoxelTerrain3D (inherits Node3D)

The main editor-visible node.

* **Properties:**
  * `String save_file_path`
  * `int view_distance_chunks`
  * `int prefetch_distance_chunks` (default 2)
  * `int resident_memory_budget_mb` (default 512)
  * `float upload_budget_ms` (default 2.0)
  * `int world_height_chunks`
  * `bool save_on_exit` (default `true`)
* **Methods:**
  * `void trigger_explosion(Vector3 position, float radius)`: enqueues an Edit transaction and returns immediately. **Never blocks.** If the edit queue is full or the engine is not active, it emits `explosion_failed`.
  * `void save_world()`: flushes the Persist queue and blocks until it completes. Intended for editor use and shutdown.
  * `void set_stream_focus(Vector3 position, Vector3 velocity)`: sets the main-thread streaming focus supplied by the game (normally the player/camera position and velocity). The wrapper passes this to the core once per frame.
  * `bool core_idle() const`: true when no core jobs are queued or in flight. Says nothing about Godot-side state.
  * `int get_effective_view_distance_chunks() const`: returns the current affordable active radius after memory-budget clamping.
  * `int get_prefetch_distance_chunks() const` / `set_prefetch_distance_chunks(int)`: controls the soft prefetch radius.
  * `int get_resident_memory_budget_mb() const` / `set_resident_memory_budget_mb(int)`: controls the hard engine-managed CPU residency budget.
  * `bool fully_committed() const`: true when core jobs are idle, the MPSC completion queue is empty, and every live/required navigation group is synchronized to its desired `(terrain_revision, source_epoch)`. This is the readiness check gameplay code must use; an empty completion queue alone does not mean navigation has finished rebuilding. It does **not** promise that the requested `view_distance_chunks` is fully resident; use the effective-view/residency telemetry for streaming readiness.
  * `navigation_groups_synced()`: wrapper-internal predicate defined in 4.5; it compares each live group's desired, submitted, and synchronized revision/epoch state and requires the associated `NavigationServer3D` update to have synchronized.
* **Signals:**

  | Signal | Emitted when | Payload |
  | :--- | :--- | :--- |
  | `explosion_requested(position, radius)` | The job is enqueued, on the calling thread | Call's arguments |
  | `explosion_completed(position, radius, affected_chunks)` | All phases of the edit transaction have applied to authoritative state, on the main thread | World state has changed; navigation may still be catching up |
  | `explosion_failed(reason)` | The job could not be enqueued (pool shut down, or the position is outside world bounds) | Reason string |
  | `chunk_committed(chunk_key)` | The chunk's visual mesh and collision are live and its navigation group is synchronized to the desired revision/epoch | Raw chunk key |

  The v2.0 signal `explosion_applied` is removed. Its meaning was stated two different ways in the doc: "after the edit is committed" in 4.2, and "on enqueue" in 5.2. A gameplay programmer must be able to distinguish **request accepted** from **world state changed**, so the two cases are now separate signals. `explosion_completed` is emitted from the `_process` drain loop, not from within `trigger_explosion`, because worker code may not call into Godot.

* **Lifecycle:** The worker pool starts in `_enter_tree()` and shuts down in `_exit_tree()`. Observers may connect to any signal before the pool starts; signals emitted before a receiver connects are missed, which is expected.

#### VoxelAICompanion (inherits CharacterBody3D)

A GDScript-facing node that uses Godot's navigation stack.

* **Methods:**
  * `PackedVector3Array get_voxel_path(Vector3 start, Vector3 end)`: wraps `NavigationServer3D.map_get_path()` on the terrain's navigation map. It does not call into the core.
  * `void set_target_position(Vector3 position)` and `float get_distance_to_target()`: standard agent plumbing for use with `NavigationAgent3D`.
* **Note:** The name "voxel path" is kept for compatibility with v1. It is a Godot navigation query and carries no voxel-specific meaning.

### 4.3 Main-Thread Commit Loop

The explosion path does not block the main thread.

```
trigger_explosion()          [main thread] -> enqueue Edit transaction, return
  Edit phase                 [worker]      -> bounded lock round, mutate canonical TSDF, mark dirty
  Mesh admission             [worker]      -> pull dirty chunk only when queue/memory caps permit
  Mesh job                   [worker]      -> snapshot padded window, Surface Nets, build collision source
  completion queue (MPSC)    [workers -> main]
VoxelTerrain3D::_process()   [main thread]
  drain completions within soft budget
    commit visual mesh + collision
    submit/advance affected navigation groups
  emit chunk_committed(...) only after group sync
```

#### The Budget Is Soft

The loop attempts to respect `upload_budget_ms` but **cannot guarantee it**. The reasons:

* The budget is checked **before** a chunk starts, not mid-chunk. A single 32³ chunk commit may exceed 2 ms on low-end hardware.
* **Commits are never split across frames.** A chunk's mesh is unusable if half its vertices are uploaded; splitting would produce visible geometry corruption. So the loop either commits a chunk whole or defers it to the next frame.

The requirement is therefore stated as: **the average frame-time contribution of voxel commits stays under `upload_budget_ms`, and no frame exceeds `upload_budget_ms` by more than the cost of one chunk commit.** The exception is bounded and measurable per hardware target.

Commit cost is not dominated by vertex copying. `ArrayMesh` construction and `create_trimesh_shape` are main-thread costs; NavigationServer3D group submission is also measured separately. The actual navigation bake is asynchronous on the server side and is not treated as a synchronous mesh-commit cost.

#### Queue Backpressure

If the completion queue reaches the hard depth cap in 3.4, `_process` drains **without the soft budget check** for that frame, because an over-deep queue is the worse failure. This is the one condition where the frame budget is knowingly exceeded. Edit phases themselves never hold locks while waiting for this condition to clear.

### 4.4 Physics Collision

* **Source:** Collision geometry is generated from the same Surface Nets output as the visual mesh. It is not a separate meshing pass.
* **Shape:** `ConcavePolygonShape3D` (equivalent to `PhysicsServer3D` `SHAPE_CONCAVE_POLYGON_SHAPE`) on a `StaticBody3D` per chunk. Chunks are static, so this is valid.
* **Triangle soup, not indexed buffers.** Godot's concave shape takes a de-indexed triangle soup: 3 floats × 3 vertices per triangle, with no shared indices. Indexed vertex data compressed 2–3× by reuse will roughly triple in memory as a collision soup. Budget for it.
* **Cost is on the main thread.** `create_trimesh_shape` runs during commit, so it counts against the upload budget and is measured separately in 7.3. It is not covered by the `memcpy` cost in 4.1.
* **Decimation:** Not in v3.0. It is treated as a **planned escape hatch**, instrumented from M3 onward, not merely a future optimization. If collision bake time or memory exceeds budget, the fallback is a decimated or voxel-aligned collision mesh generated as a separate simpler Surface Nets pass (for example on a 2× subdivided sample grid).
* **Lifecycle:** The collision shape is replaced atomically when the chunk is recommitted. Stale shapes for removed chunks are freed on the main thread.
* **Player and projectile safety:** Collision for a chunk is only committed after its visual mesh. A chunk that is `Uploaded` but not yet collidable is not a problem for characters, because spawning logic uses `fully_committed()` (4.2), not `core_idle()`.

### 4.5 Navigation

v1 proposed a standalone `dtTileCache`. That approach was removed because:

1. It duplicates the memory and update cost of Godot's built-in navigation, which is also Recast-based.
2. `dtTileCache` rasterizes 2.5D heightfields. Overhangs and caves need a different rasterization setup.
3. Godot's `NavigationAgent3D`, RVO avoidance, and the navigation debug view all depend on `NavigationServer3D`.

#### Navigation Group Ownership

* Navigation regions are **groups of chunks**, not single chunks.
* The default group size is **4 × 4 chunks in X/Z**. A group therefore covers a 16×16-chunk horizontal footprint and spans all `Y` chunk coordinates in `[0, world_height_chunks)`.
* The group key is `NavigationGroupKey = (floor_div(Cx, 4), floor_div(Cz, 4))`.
* Each live group owns exactly one `NavigationRegion3D` and one navigation mesh resource on the terrain's map.
* A group becomes live when any chunk in its footprint needs navigation and is destroyed only when all of its chunks and all dependent border-padding source chunks are no longer resident.

#### Terrain Revision and Per-Group State

The terms used by `fully_committed()` are normative and have concrete definitions.

**`terrain_revision`** is a Core-owned `uint64_t` counter:

* It starts at `0` at engine creation.
* It increments by exactly **one after each Edit phase that changed at least one authoritative canonical sample**.
* The increment occurs after that phase releases all chunk write locks.
* Load, Generate initialization, Mesh, Persist, apron-cache refresh, and Unload do **not** increment it.
* The value therefore identifies the order of committed terrain-edit phases. It is monotonic and never reused.

Each navigation group has this explicit state, stored and mutated on the Godot main thread:

```cpp
struct NavigationGroupState {
    bool live = false;

    // Latest terrain content revision that this group must reflect.
    uint64_t desired_terrain_revision = 0;

    // Latest terrain revision represented by the source submitted to Godot.
    uint64_t submitted_terrain_revision = 0;

    // Latest terrain revision known to be reflected after server synchronization.
    uint64_t synced_terrain_revision = 0;

    // Changes caused by load/generate/unload membership changes.
    uint64_t desired_source_epoch = 0;
    uint64_t submitted_source_epoch = 0;
    uint64_t synced_source_epoch = 0;

    bool rebuild_in_flight = false;
    uint64_t submitted_map_iteration = 0;
};
```

`desired_source_epoch` is a per-group monotonic counter. It increments whenever the group's **committed navigation source membership or geometry** changes for a reason **other than** an authoritative edit revision, including the successful Mesh commit of a chunk materialized by Load/Generate and the removal of a source chunk during Unload. This prevents residency/source changes from being hidden behind an unchanged `terrain_revision`.

When a group first becomes live, its `desired_source_epoch` is advanced from `0` to `1`; `synced_source_epoch` remains `0` until the first group bake is submitted and synchronized. A live group is therefore never treated as synced merely because all revision counters happen to start at zero.

An internal `EditPhaseCompleted` record sent through the MPSC completion queue contains:

* the edit transaction ID,
* the phase ID and final-phase flag,
* the newly assigned `terrain_revision`,
* the affected navigation-group keys,
* the changed-chunk count.

The heavy Mesh result does not carry a duplicated navigation mesh. This control record is small and is still charged as a completion entry.

#### Navigation Group Dependency Closure

A group depends on more than the chunks physically inside its 4×4 footprint because its outer border is padded.

* A group's **owned source** is the current committed Surface Nets geometry for resident chunks inside the group's footprint.
* Its **border source** includes the world-space padding band at the outer edge of that footprint. The padding width is `agent_radius + connection_radius`.
* The border source is taken from resident chunks whose geometry intersects that band, including chunks in adjacent groups.
* Therefore an edit can invalidate **multiple navigation groups** even when the edited chunk belongs to only one group.
* The Core reports the full affected-group set in each `EditPhaseCompleted` record. The wrapper never guesses the set from the edited chunk key alone.

#### Border Seams (erosion)

v2.1 correctly identified border erosion as a correctness issue but did not define how the padded source was tracked. v3 makes it explicit:

* Regions are baked from the **group source snapshot**, not an individual chunk mesh.
* At the outer border of a group, source geometry is padded by `agent_radius + connection_radius` before rasterization, so erosion is spent on padding rather than usable area.
* Adjacent groups use the **same world-space padding width and shared source samples**, so the same border geometry is considered on both sides.
* Padding geometry is input to the bake only; it does not become owned playable area outside the group's actual region bounds.
* The render mesh and physics mesh are unaffected by navigation padding.

#### Update Lifecycle

For every group that becomes dirty:

1. The wrapper records the latest `desired_terrain_revision` and/or `desired_source_epoch`.
2. The group waits until all source chunks needed for its owned footprint and border padding are resident and their committed Surface Nets meshes reflect the required chunk versions.
3. The main thread submits one group-level navigation update to `NavigationServer3D`. It records `submitted_terrain_revision`, `submitted_source_epoch`, and the map iteration observed at submission.
4. The server performs the navigation rebuild asynchronously.
5. The wrapper polls the pinned Godot navigation synchronization state. Once the submitted update is synchronized, it copies the submitted revision/epoch into `synced_*` and clears `rebuild_in_flight`.
6. If the group was invalidated again during the rebuild, `desired_*` is now newer than `synced_*`; the group is immediately queued for another rebuild from the latest committed source.

A new rebuild **does not overwrite** an in-flight result. Results are versioned by the `(terrain_revision, source_epoch)` pair they represent.

#### `navigation_groups_synced()`

`navigation_groups_synced()` is a concrete wrapper-internal predicate:

```cpp
bool navigation_groups_synced() const {
    for (const auto &[key, state] : navigation_groups) {
        if (!state.live) {
            continue;
        }

        if (state.rebuild_in_flight) {
            return false;
        }

        if (state.submitted_terrain_revision != state.desired_terrain_revision
            || state.synced_terrain_revision != state.desired_terrain_revision) {
            return false;
        }

        if (state.submitted_source_epoch != state.desired_source_epoch
            || state.synced_source_epoch != state.desired_source_epoch) {
            return false;
        }
    }

    return true;
}
```

The wrapper also requires the corresponding `NavigationServer3D` map synchronization/iteration condition for each submission before advancing `synced_*`. The exact Godot API sequence is pinned and verified at M0; the group state above is the engine-level contract independent of the exact Godot call sequence.

#### Path Safety and Signals

* A path query that crosses a recently invalidated group is considered safe only when that group satisfies `navigation_groups_synced()`.
* `fully_committed()` is true only when `core_idle()`, the MPSC completion queue is empty, and every live/required navigation group is synchronized to its desired `(terrain_revision, source_epoch)`.
* `chunk_committed(chunk_key)` is emitted only when the chunk's visual mesh and collision are live **and** its navigation group is synchronized to the current desired state. This signal may therefore occur several frames after the chunk's mesh result is committed.
* `explosion_completed` means the authoritative edit transaction is finished. It does **not** by itself mean navigation is synchronized; callers requiring path safety use `fully_committed()` or group-specific sync.

#### Navmesh Source

* Navigation source geometry comes from the same committed Surface Nets chunk meshes used for rendering/collision validation.
* A navigation group assembles a source snapshot from its owned chunks plus the required border-padding chunks. Source triangles are deduplicated by chunk ownership; the same triangle is never submitted twice merely because it lies in padding overlap.
* The group bake is a Godot `NavigationServer3D` operation and may execute asynchronously. The wrapper is responsible only for main-thread submission, lifecycle, and synchronization tracking.
* M4 profiles nav bake time and memory independently. If navigation cost dominates, a coarser source mesh may become a deliberate later design change; until then, render and navigation geometry remain matched.

#### Version Compatibility

The project initially targets **Godot 4.2**. The exact `NavigationServer3D` and navigation-baking APIs used by this design are verified against the pinned godot-cpp/Godot version during M0. If a required API is unavailable, either raise `compatibility_minimum` or adjust the implementation before M0 closes; the version is not treated as final until that verification passes.

---

## 5. Technical Implementation Details

### 5.1 GDExtension Entry Point (register_types.cpp)

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
GDExtensionBool GDE_EXPORT voxel_engine_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
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

### 5.2 Explosion Entry Point (voxel_terrain_3d.cpp)

The entry point is non-blocking. Work happens on the core worker pool. Authoritative changes are phased; mesh/collision commits happen on the main thread; navigation is submitted at region-group scope.

```cpp
#include "voxel_terrain_3d.h"
#include "core_voxel_engine.h"

void VoxelTerrain3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("trigger_explosion", "position", "radius"), &VoxelTerrain3D::trigger_explosion);
    ClassDB::bind_method(D_METHOD("save_world"), &VoxelTerrain3D::save_world);
    ClassDB::bind_method(D_METHOD("set_stream_focus", "position", "velocity"), &VoxelTerrain3D::set_stream_focus);
    ClassDB::bind_method(D_METHOD("core_idle"), &VoxelTerrain3D::core_idle);
    ClassDB::bind_method(D_METHOD("get_effective_view_distance_chunks"), &VoxelTerrain3D::get_effective_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_prefetch_distance_chunks"), &VoxelTerrain3D::get_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("set_prefetch_distance_chunks", "distance"), &VoxelTerrain3D::set_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_resident_memory_budget_mb"), &VoxelTerrain3D::get_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("set_resident_memory_budget_mb", "mb"), &VoxelTerrain3D::set_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("fully_committed"), &VoxelTerrain3D::fully_committed);
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
    // Constructor does NOT create the core or start threads.
    core_engine = nullptr;
}

VoxelTerrain3D::~VoxelTerrain3D() = default;

void VoxelTerrain3D::_enter_tree() {
    core_engine = new CoreVoxelEngine();
    core_engine->start();
    core_engine->configure_streaming(view_distance_chunks, prefetch_distance_chunks,
                                     resident_memory_budget_mb, world_height_chunks);
    core_engine->set_upload_budget_ms(upload_budget_ms);
}

void VoxelTerrain3D::_exit_tree() {
    if (core_engine) {
        core_engine->shutdown(save_on_exit);
        delete core_engine;
        core_engine = nullptr;
    }
}

void VoxelTerrain3D::trigger_explosion(Vector3 p_position, float p_radius) {
    if (!core_engine || p_radius <= 0.0f) {
        emit_signal("explosion_failed", String("engine not ready or radius is non-positive"));
        return;
    }

    CoreVector3 native_pos(p_position.x, p_position.y, p_position.z);
    const bool enqueued = core_engine->enqueue_explosion(native_pos, p_radius);

    if (enqueued) {
        emit_signal("explosion_requested", p_position, p_radius);
    } else {
        emit_signal("explosion_failed",
                    String("edit queue full, engine shutting down, or position out of bounds"));
    }
}

bool VoxelTerrain3D::core_idle() const {
    return core_engine && core_engine->jobs_pending() == 0;
}

bool VoxelTerrain3D::fully_committed() const {
    return core_idle()
        && core_engine
        && core_engine->completion_queue_depth() == 0
        && navigation_groups_synced();
}

void VoxelTerrain3D::_process(double delta) {
    if (!core_engine) {
        return;
    }

    const uint64_t frame_start_us = Time::get_singleton()->get_ticks_usec();
    const uint64_t budget_us = static_cast<uint64_t>(upload_budget_ms * 1000.0);

    // Streaming planning is main-thread only.
    core_engine->update_stream_focus(stream_focus_position, stream_focus_velocity);

    const bool hard_backpressure = core_engine->completion_queue_depth() >= 32;

    while (true) {
        std::optional<CompletionRecord> done = core_engine->try_pop_completion();
        if (!done) {
            break;
        }

        switch (done->kind) {
            case CompletionKind::ChunkMesh:
                commit_visual_and_collision(done->chunk_mesh);
                navigation_manager.note_chunk_mesh_committed(done->chunk_mesh.key);
                break;

            case CompletionKind::EditPhase:
                navigation_manager.apply_edit_phase(done->edit_phase);
                if (done->edit_phase.final_phase) {
                    emit_signal("explosion_completed",
                                done->edit_phase.position,
                                done->edit_phase.radius,
                                static_cast<int>(done->edit_phase.affected_chunks));
                }
                break;
        }

        if (!hard_backpressure
            && Time::get_singleton()->get_ticks_usec() - frame_start_us >= budget_us
            && core_engine->completion_queue_depth() > 0) {
            break;
        }
    }

    // Group-level navigation submissions happen only on the main thread.
    navigation_manager.submit_ready_groups_to_server();

    // Emit chunk_committed only for chunks whose render/collision commit is live
    // and whose owning navigation group has synchronized to its desired revision/epoch.
    for (const ChunkKey key : navigation_manager.drain_newly_committed_chunks()) {
        emit_signal("chunk_committed", static_cast<int64_t>(key.raw));
    }
}
```

Notes:

* `trigger_explosion()` is main-thread only. It never dereferences a null `core_engine`, and it rejects non-positive radii before enqueue.
* `EditPhase` completion records are deliberately small. Heavy mesh/collision buffers remain in `ChunkMeshCompletion` entries.
* `commit_visual_and_collision()` never performs a navigation bake. Navigation updates are submitted once per affected region group after the required chunk meshes are current.
* `explosion_completed` means the authoritative edit transaction has finished all phases. It does **not** imply navigation synchronization.
* `chunk_committed` is delayed until the chunk's visual/collision state is live and its navigation group reports synchronized.
* The main-thread upload budget remains soft. When the queue is at the hard cap, `_process` drains without the soft budget check to restore downstream capacity.

### 5.3 Teardown Protocol

`CoreVoxelEngine::shutdown(bool save)` runs exactly one ordered sequence. v2.0 listed a Persist flush **after** the worker join, which is impossible: once workers are joined, no thread remains to process the Persist queue. Each step below names the thread that performs it.

| # | Step | Thread |
| :-- | :--- | :--- |
| 1 | Set `shutting_down = true`. Stop accepting new jobs. | Caller |
| 2 | Signal all workers. Each finishes its current job/phase at the next cancellation safe point, then returns to idle. | Workers |
| 3 | **Drain the Persist queue**: the caller thread runs remaining Persist jobs inline. | **Caller** |
| 4 | Discard, in order: completion queue entries, completed mesh results, pending collision results, pending navigation submissions. | Caller |
| 5 | Join all worker threads. | Caller |
| 6 | Release the chunk store, the MPSC queue, and the pools. No chunk is freed while a job holds a reference (3.2, Unload Rule) | Caller |
| 7 | Flush and close region files. | Caller |

Clarifications:

* **Who flushes Persist: the caller, in step 3.** Workers are still alive at that point, so the drain happens before step 5's join.
* `save == false` (or `save_on_exit == false`) replaces step 3 with discarding the Persist queue. Unsaved edited chunks are then lost and regenerated on next load. This is the explicit data-loss window from 3.6.
* Pending **completion queue entries** (step 4) are chunks whose worker work is finished but whose Godot-side commit never happened. They are dropped. Their sources remain in the chunk store, so the state is not lost, but their meshes are not uploaded.
* Pending **navigation submissions** are dropped. Because navigation is derived state owned by `NavigationServer3D`, dropping them is safe; the map rebuilds from whatever regions exist.
* No step may run concurrently with another. The sequence is a single-threaded critical section. An Edit worker never retains a phase lock while teardown waits on Persist or queue state, and `VoxelTerrain3D::_exit_tree` must not call into the core after step 7.

---

## 6. Build and Deployment

### 6.1 Build Workflow and Core Linkage

#### Decision: static link, no separate `libvoxel_core`

v2.0 shipped the core as a separate shared library next to the extension. That is the wrong default for a GDExtension:

* Passing `std::optional`, `std::unique_ptr`, `std::vector`, or any std type across a shared-library boundary is a C++ ABI hazard. The extension and the core must be built with identical toolchain, flags, and stdlib version, or the ABI mismatch causes crashes that are near-impossible to diagnose.
* A separate DLL also has a loader-order problem: the extension's DLL directory is not on the OS DLL search path by default, so `libvoxel_core.dll` can fail to load.

**v2.1 links the core statically into the extension binary.** One `libvoxel_engine.*.dll` per platform, no side-by-side dependencies. This also removes the deployment burden entirely.

* CMake builds the core as a static library (`voxel_core`).
* The extension links `voxel_core` statically.
* If a separate DLL is ever required (for example a second host application sharing one core), the core must then expose a **C API only**, with opaque handles instead of `unique_ptr`, and no std types in signatures. That is a deliberate future change, not the default.

If a DLL is kept despite this, the `.gdextension` needs a `[dependencies]` section:

```ini
[dependencies]
windows.release.x86_64 = { "res://bin/libvoxel_core.windows.release.x86_64.dll" }
```

With static linkage this section is absent.

#### Build Steps

1. Build the core target `voxel_core` (static) with **CMake**. It does not depend on Godot.
2. Build the GDExtension wrapper against **godot-cpp** for the target Godot version, linking `voxel_core` statically.
3. Produce separate **debug** and **release** binaries per platform. Debug builds enable assertions, chunk-state validation, and the TSan/ASan builds used in CI.

#### Toolchain Specification

These are pinned once and recorded in `AGENTS.md` so builds are reproducible:

| Item | Requirement |
| :--- | :--- |
| Godot version | 4.2 or later, matching the godot-cpp branch |
| godot-cpp | Exact commit or tag recorded in the submodule |
| Compiler | MSVC v143 or later (Windows), Clang 15+ (Linux/macOS) |
| C++ standard | C++17 minimum; C++20 if `std::atomic`'s wait/notify is used |
| Runtime (Windows) | `/MD` (release) or `/MDd` (debug) consistently; never mix `/MT` and `/MD` between the core and the extension |
| FastNoise2 | Version pinned; note the SIMD-level behavior in tests |
| LZ4 | Version pinned |

### 6.2 Extension Descriptor (`.gdextension`)

Place the descriptor in the Godot project at `bin/voxel_engine.gdextension`.

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

Notes:

* `compatibility_minimum` is **4.2**, not 4.1. The async navigation APIs relied on in 4.5 need verification at M0; if they are unavailable at the pinned version, raise this value or drop the async path.
* `macos.debug` and `macos.release` are universal entries covering x86_64 and arm64 if the dylib is built universal. Use per-arch keys (`macos.release.arm64`) only when shipping separate binaries.
* No `[dependencies]` section, because the core is statically linked (6.1).

### 6.3 Godot Project Setup (Greenfield)

The Godot project does not exist yet. Create it with the settings below, then record the actual values once the project is created.

| Setting | Initial value | Reason |
| :--- | :--- | :--- |
| Godot version | 4.2 stable, matching the godot-cpp branch in 6.1 | ABI compatibility; also the minimum for the navigation APIs |
| `rendering/renderer/rendering_method` | Forward+ (or Mobile for mobile targets) | Compatibility renderer is inadequate for large dynamic mesh counts |
| `physics/3d/default_gravity` | Project-defined | Set from game design |
| `display/window/size/*` | Project-defined | Not a voxel-engine concern |
| `application/run/main_scene` | A test scene containing a `VoxelTerrain3D` node | Required for `project_run(mode="main")` |

Setup steps:

1. Create the Godot project in `C:\Users\kuroi\workspace\voxel\` (or the chosen location).
2. Add the godot-cpp submodule and the `voxel_core` CMake target.
3. Place `bin/voxel_engine.gdextension` (see 6.2).
4. Create the test scene and set it as the main scene.
5. Verify the extension loads and `VoxelTerrain3D` and `VoxelAICompanion` appear in the Create Node dialog.

---

## 7. Verification Strategy

### 7.1 Unit Tests (core, no Godot)

Concurrency and ownership (3.2):

* ChunkKey round-trip for coordinates from -1,048,576 to 1,048,575 on all three axes, including negative quadrants.
* **Canonical sample ownership:** every integer world sample maps to exactly one owner via `floor_div(sample, 32)`, including all positive/negative chunk boundaries.
* **Shared-boundary edits:** editing a sample on a chunk boundary mutates exactly its canonical owner and produces identical reconstructed values in every dependent chunk window.
* **Single writer:** two Edit jobs overlapping on a chunk are serialized; TSan reports no data race.
* **Canonical lock order:** two concurrent edits touching overlapping phase lock sets complete without deadlock. Run with randomized lock-acquisition delays to force contention.
* **Lock-set cap:** no Edit phase ever holds more than `max_edit_lock_chunks`; a large synthetic explosion is partitioned into multiple phases.
* **Phase backpressure:** force the completion queue to its hard cap between Edit phases and verify all phase locks are released before the edit waits.
* **Dirty-set decoupling:** one large explosion marks many chunks dirty but cannot enqueue more than the permitted Mesh/completion capacity.
* **Snapshot isolation:** a Mesh job reconstructs a consistent 35³ snapshot while an Edit job concurrently mutates contributing chunks.
* **Snapshot version set:** a Mesh result is rejected when any contributing chunk version changed after snapshot.
* **Load initialization:** Load may initialize authoritative state only before `Resident`; once resident, only Edit may mutate authoritative data.
* **Save consistency:** a Persist job whose chunk is mutated mid-snapshot discards and re-enqueues; the written chunk is always fully pre-edit or fully post-edit.
* **Unload under reference:** unloading a chunk with one in-flight job does not free its memory until the job completes (ASan).
* **VoxelTerrain3D pool start:** constructing the node does not create threads; threads appear only after `_enter_tree()`.

Streaming and residency (3.3, 3.4):

* **Desired-set bound:** a Chebyshev radius `r` requests no more than `(2r + 1)^3` chunk coordinates before world bounds are applied.
* **26-neighbor dependency:** an active/dirty chunk's mesh admission waits for every required dependency-closure chunk to be resident or known absent.
* **Prefetch distance:** no chunk outside `view_distance_chunks + prefetch_distance_chunks` is requested unless explicitly retained by an in-flight dependency.
* **Load-before-generate:** an edited chunk with a region-file record is scheduled on Load and is not regenerated. An unedited chunk schedules Generate.
* **Completion isolation:** Load and Generate never enter the MPSC completion queue as heavy results; Edit-phase control records and Mesh chunk results obey the same depth/byte caps.
* **Memory hard cap:** aggregate resident + reserved engine-managed CPU bytes never exceed `resident_memory_budget_mb`, including snapshots and completion buffers.
* **Budget degradation:** when the budget is exhausted, prefetch is suppressed/evicted before active chunks are dropped; effective view radius is reduced only after prefetch reaches zero.
* **Transient allocation refusal:** a mesh/collision result cannot allocate past the budget; the job defers until memory is available.
* **Eviction ordering:** chunks outside the prefetch radius are evicted farthest-first, edited chunks persist before eviction, and in-flight references prevent destruction.
* **Edit queue cap:** more than `max_pending_edit_transactions` requests are rejected/coalesced according to the defined policy rather than accumulating unbounded memory.

Geometry (3.1, 3.5):

* **Cell/sample boundary ownership:** a 2×1×1 chunk pair reconstructs the same shared boundary samples from canonical owners with no duplicate authoritative writes.
* **26-neighbor propagation:** a corner-boundary sample update refreshes or reconstructs correctly in all dependent adjacent chunks, including edge and corner neighbors.
* TSDF quantize/dequantize error bounded by `δ / 32767` across the band.
* **Density-to-SDF approximation:** on representative generated terrain, measure surface-position error against a reference signed-distance estimate, near-surface gradient magnitude, and cumulative error after repeated explosion CSG operations. M1 cannot close until these remain within project-defined tolerances.
* Explosion edit: every canonical sample within `R` of the center becomes air; canonical samples outside the sphere are unchanged; dependent padded windows reconstruct the new values.
* Surface Nets: a sphere SDF produces a closed mesh with no boundary edges, including at chunk boundaries.
* **Multipart meshing:** a world meshed as one 64³ region equals the union of the same world meshed as 2×2 chunks of 32³. Any vertex or index divergence is a seam bug.
* **Compact promotion:** an `EMPTY` chunk edited once promotes to `DENSE` with all canonical samples initialized from the air marker; an edited `SOLID` chunk promotes likewise.
* **Load-order independence:** loading/generating chunks in different orders produces the same mesh snapshots once the required dependency closure is resident or known absent.

Serialization (3.5, 3.6):

* Save/load round-trip is bit-exact for canonical `int16_t` payloads. Homogeneous chunks restore as `EMPTY` or `SOLID`.
* **Version bracket:** a mutation landing between a chunk's `version_before` and `version_after` forces a discard-and-retry.
* Region file: a crash during write (simulated by truncating the temp file) leaves the last good region readable.
* **Edited-chunk-only saving:** an unedited chunk is not written to disk at all.

Navigation state model (4.5):

* **Terrain revision:** an edit phase that changes canonical samples increments `terrain_revision` exactly once; Load/Generate/Unload do not.
* **Group source epoch:** load/generate/unload changes increment only the affected group's `source_epoch`.
* **Group invalidation closure:** an edit affecting a border-padding dependency invalidates every impacted group, not just the group owning the edited chunk.
* **Stale rebuild:** a navigation group rebuilt at revision `N` is rejected as synchronized when its desired revision is `> N`.
* **Rebuild race:** a second edit arriving during an in-flight group rebuild causes a second rebuild from the latest desired revision/epoch, without partial-state exposure.
* **Sync predicate:** `navigation_groups_synced()` remains false until every live/required group matches its desired revision/epoch and the corresponding server synchronization condition is satisfied.

### 7.2 Integration Tests (Godot)

* Run the game through the Godot MCP server (`project_run`). Use `game_eval` or `game_manage` to trigger explosions and verify the completion queue drains.
* `trigger_explosion` returns in under 1 ms of main-thread time for normal validation inputs and never blocks on worker execution, locking, or queue capacity.
* `set_stream_focus` does not perform disk IO, generation, meshing, or Godot resource creation synchronously; it only updates the main-thread streaming focus state.
* Verify `explosion_requested` fires before `explosion_completed`, and that `explosion_completed` fires only after all Edit phases finish.
* Verify `fully_committed()` is false while a completed chunk result sits in the MPSC queue and remains false while an invalidated navigation region group is rebuilding.
* Verify a large explosion is visibly progressive but never produces more than the configured completion-queue depth or memory budget.
* Verify `chunk_committed` is emitted only after the chunk's visual mesh/collision are live and its navigation group has synchronized to the desired revision/epoch.
* Measure main-thread frame time during small and large explosions.
* Verify `VoxelAICompanion.get_voxel_path()` returns a path after an explosion only once every affected navigation group is synchronized.
* Verify no navigation gap at a 4×4 region-group boundary with the configured agent radius and connection radius.
* Verify an edit near a region-group border invalidates both the owning group and any adjacent group whose padding source intersects the edit.
* Verify navigation groups created by Load/Generate and removed by Unload advance `source_epoch` and rebuild without changing `terrain_revision`.
* Verify `trigger_explosion` called before `_enter_tree()` or after `_exit_tree()` fails cleanly instead of dereferencing a null engine.

### 7.3 Performance Targets (to validate, not assumed)

Targets apply to the **reference hardware**: an 8-core x86-64 CPU, an SSD, and a mid-range discrete GPU. Record the actual machine in `AGENTS.md` when M0 completes; until then these numbers are targets, not commitments.

| Metric | Target | Notes |
| :--- | :--- | :--- |
| Explosion, main-thread cost, mean | < 2 ms per frame | Enforced by the soft upload budget (4.3) |
| Explosion, main-thread cost, p99 frame | < `upload_budget_ms` + one chunk commit | The budget exception is bounded and measured |
| Explosion, time to visible result | < 3 frames for a 4-chunk edit | Depends on worker load |
| Large edit phase lock set | ≤ `max_edit_lock_chunks` | Hard correctness invariant, not a tuning target |
| Generated terrain surface error | Project-defined tolerance | Validate the density-to-SDF approximation before M1 closes |
| Generated SDF near-surface gradient | Project-defined tolerance around 1.0 | Validate local distance behavior before repeated CSG edits |
| Navigation region update, single group | < 10 ms submission overhead on main thread | Actual bake is asynchronous and must be measured separately |
| Chunk memory, dense TSDF window | ≤ 83.7 KiB resident per chunk | 35³ × 2 bytes |
| Chunk memory, `EMPTY`/`SOLID` | 0 bytes of sample data | From 3.1 |
| Chunk material memory | ≤ 32 KiB when allocated | 32³ canonical `uint8_t` samples |
| Streaming budget utilization | ≤ 100% of `resident_memory_budget_mb` | Hard admission bound for engine-managed CPU memory, including transient worker/completion buffers |
| Completion queue depth | < 32 entries during steady state | 32 is the hard backpressure threshold |
| Active chunk admission | Meets `view_distance_chunks` target when memory permits | Report effective radius separately when budget-constrained |
| Mesh commit, split cost | vertex copy / collision shape / nav submission reported separately | Navigation bake is asynchronous and measured at group scope |

Commit cost must be reported as **average, p95, p99, and worst-case dense chunk**, for vertex upload and collision shape creation independently. Navigation submission and asynchronous bake time are reported per region group. A single aggregate average hides the case that actually causes a visible hitch.

---

## 8. Open Questions and Risks

1. **Chunk size vs. commit cost.** A 32³ chunk with a 35³ padded sample window is about 83.7 KiB for TSDF data. A single chunk commit may exceed 2 ms on some hardware. Decide after profiling whether to reduce chunk size to 16³.
2. **Streaming memory budget vs. view distance.** A 16-chunk Chebyshev radius contains 35,937 chunk positions and cannot fit in a 512 MiB budget if all chunks are dense. The engine therefore treats `view_distance_chunks` as a target and reports an effective radius when memory-constrained.
3. **Snapshot reconstruction cost.** Mesh snapshots may consult up to 27 canonical owner chunks. If reconstruction shows up in profiling, optimize cache population or protected-read paths without allowing Mesh to read live samples.
4. **Collision memory.** Concave shapes require a de-indexed triangle soup, which is roughly 3× the indexed vertex data (4.4). Instrument from M3. Planned escape hatch: a decimated or voxel-aligned collision mesh.
5. **Navigation group size.** 4×4 X/Z chunks is the default. Benchmark 4×4 versus 8×8 before M4 closes while preserving the explicit padding/dependency rules.
6. **Async navigation API availability.** Verify the required `NavigationServer3D` APIs exist at the pinned Godot version during M0. If not, either raise `compatibility_minimum` or change the synchronization implementation.
7. **Cross-machine FastNoise2 determinism.** Accepted for v3. Revisit if multiplayer or seed-shared worlds are added.
8. **Edit-phase tuning.** `max_edit_phase_core_chunks = 32` and `max_edit_lock_chunks = 96` are safety defaults. Tune them from measured lock hold time and worker contention; the hard invariant is that the lock cap is never exceeded.
9. **Flush interval.** Region files flush per region, not per chunk (3.6). Choose a dirty-chunk count or time interval at M4 and record the resulting maximum data-loss window.

## 9. Milestones

| ID | Milestone | Exit criteria |
| :--- | :--- | :--- |
| M0 | Core skeleton + contract | ChunkKey, canonical sample ownership, 35³ mesh window, compact forms, streaming planner + 26-neighbor dependency closure + hard memory admission, bounded Edit phases, dirty-set Mesh admission, worker pool, teardown, static linkage; **all 3.2/3.3/3.4 contract tests pass under TSan/ASan** |
| M1 | Procedural generation and meshing | Gradient-normalized SDF approximation; Surface Nets from canonical-owner snapshots; multipart-meshing equivalence and boundary-ownership tests pass; generated surface/gradient error remains within project-defined tolerances |
| M2 | Edits | Explosion CSG on canonical samples with bounded phases, 26-neighbor cache propagation, canonical lock order, completion backpressure test, and sphere-SDF closed-mesh test |
| M3 | Godot integration | `VoxelTerrain3D` renders and collides; main-thread commit remains within the measured frame-budget envelope; large edits cannot exceed completion queue depth or engine-managed memory budget |
| M4 | Persistence and navigation | Region-file saves with version bracketing; explicit navigation group revision/epoch state; `fully_committed()` and `navigation_groups_synced()` pass stale-rebuild tests; `VoxelAICompanion` path works after edits, including across region-group borders |
| M5 | LOD | Octree or clipmap LOD, Transvoxel transitions |

M0 closes only when the lock-set cap, dirty-set handoff, terrain-revision contract, and per-group navigation sync state are executable tests rather than documentation-only rules.

---

## Appendix A. Issue Resolution Map

### potentialIssues.md (v1 review)

| ID | Resolution in v2 |
| :--- | :--- |
| 1.1 Main-thread blocking | Section 3.4, 4.3, 5.2 |
| 1.2 Use-after-free on destruction | Section 3.2 (Unload Rule), 5.2, 5.3 |
| 2.1 Missing chunk apron | Section 3.1 (35³ padded volume), 3.5 |
| 2.2 Transvoxel without LOD | Section 3.5, 1.3, M5 |
| 2.3 SDF / CSG discrepancy | Section 3.1 (TSDF, normalized `int16_t`) |
| 3.1 Nonexistent `to_writer()` | Section 4.1 |
| 3.2 Physics omitted | Section 4.4 |
| 3.4 `VoxelAICompanion` not registered | Section 5.1 |
| 3.5 Parallel navigation stack | Section 4.5 |
| 4.1 Octree vs. hash map | Section 3.1 |
| 4.2 Sign-extension hazard | Section 3.1 (offset encoding) |
| 4.3 RLE on float SDF | Section 3.5 (homogeneous elision, quantized SDF) |
| 4.4 No region files | Section 3.6 |

### v2 implementation-readiness review

| Finding | Resolution in v2.1 |
| :--- | :--- |
| Edit/Mesh data race (biggest issue) | Section 3.2: single writer, snapshot reads, canonical lock order |
| Ambiguous apron ownership | Section 3.1: canonical sample ownership; 35³ derived window. Section 3.2: 26-neighbor dependency and snapshot reconstruction |
| Ambiguous cell/sample coordinate model | Section 3.1: cells vs. samples, 32³ canonical, 35³ padded, inclusive-low/exclusive-high ownership |
| Budget claim self-contradictory | Section 4.3: soft budget, per-operation measurement; 7.3 reports mean/p95/p99/worst-case |
| Wrong teardown order | Section 5.3: one ordered sequence with the acting thread named per step |
| `explosion_applied` ambiguous | Section 4.2: split into `explosion_requested` / `explosion_completed` / `explosion_failed` |
| `is_idle()` too weak | Section 4.2: `core_idle()` and `fully_committed()`, with navigation synchronization included in readiness |
| No scheduler fairness | Section 3.4: aging, starvation bounds, exact CSG coalescing, queue caps, bounded Edit phases, and dirty-set Mesh admission |
| Collision as hidden cost / tie to render mesh | Section 4.4: planned escape hatch, measured separately, instrumented from M3 |
| Navigation contract missing | Section 4.5: region groups, explicit terrain revision/source epoch, per-group state, dependency closure, lifecycle, erosion padding, update ordering, sync predicate |
| Save snapshot consistency | Section 3.2: version bracket with discard-and-retry |
| Build/deployment underspecified | Section 6.1: static linkage, toolchain table |
| Apron not rebuilt on generate/load | Section 3.5: apron consistency across all four lifecycle events |
| Residency of homogeneous chunks | Section 3.1: `EMPTY`/`SOLID` compact forms, lazy material channel |
| `.gdextension` missing `[dependencies]`; DLL ABI hazard | Section 6.1, 6.2: static linkage, dependencies section only if a DLL is reintroduced |
| Navigation erosion seams at borders | Section 4.5: region-group bake with agent-radius padding |
| Noise is not an SDF | Section 3.5: explicit gradient-normalized **SDF approximation** plus quantitative validation before M1 closes |
| Surface Nets padding ambiguity | Section 3.1: 1-sample apron is sufficient; cell ownership defined |
| Concave collision triangle soup cost | Section 4.4 |
| Region file TOC contradiction | Section 3.6: region file is the durability unit, no in-place TOC update |
| LZ4 on interleaved `int16` | Section 3.5: byte-plane split |
| Unedited chunks saved | Section 3.5: `edited` flag, only edited chunks persist |
| Missing chunk streaming / residency management | Section 3.3: desired set, prefetch, 26-neighbor dependency closure, load/generate routing, eviction, hard memory budget, and queue interaction |
| Threads spawned in constructor | Section 5.2, 3.4: pool starts in `_enter_tree` |

### v3 audit findings

| Finding | Resolution in v3 |
| :--- | :--- |
| Edit lock set grew without a bound and could span an arbitrarily large explosion | Section 3.2: explicit `max_edit_phase_core_chunks = 32` and `max_edit_lock_chunks = 96`; large edits are partitioned into phases |
| Large Edit job could flood downstream work before completion-queue backpressure took effect | Sections 3.2, 3.4: edits mark a deduplicated dirty set; Mesh admission owns downstream queue pressure; phases yield only after releasing locks |
| Completion-queue depth was measured in chunks while control/results could be heterogeneous | Section 3.4: hard entry-depth cap plus byte accounting against the global memory budget |
| `navigation_groups_synced()` and `terrain_revision` were undefined | Section 4.5: explicit terrain-revision semantics, per-group state, source epochs, rebuild lifecycle, and concrete sync predicate |
| Navigation updates were described both as per-chunk data and group-level Godot bakes | Sections 4.3, 4.5: chunk Mesh results provide reusable source; navigation rebuilds are group-level and asynchronous |
| Six-neighbor propagation was insufficient for 3D boundary/corner samples | Sections 3.1–3.3: canonical sample ownership and full 26-neighbor dependency closure |
| Shared chunk-boundary samples were simultaneously described as authoritative in multiple chunks | Section 3.1: one canonical owner via `floor_div(sample, 32)`; other local boundary slots are derived |
| Load reconstructed authoritative data despite a resident-only mutation rule | Section 3.2: Load/Generate use a dedicated pre-Resident initialization write path |
| Edit coalescing could imply replacing intersecting spheres with a bounding shape | Section 3.4: exact max-over-sphere terms; no bounding-sphere substitution |
| `trigger_explosion()` could dereference `core_engine` before `_enter_tree()` | Section 5.2: null guard and explicit enqueue failure path |
| Persist starvation guarantee conflicted with the old “Edit/Load only” completion-pressure rule | Section 3.4: Persist reservation remains active during soft completion backpressure |

