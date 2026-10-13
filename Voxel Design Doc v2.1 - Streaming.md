# High-Level Design Document (HLD) v2
## Project Core Architecture: Voxel Engine Library with Godot GDExtension Integration

**Status:** Draft v2.1
**Supersedes:** Voxel Design Doc.md (v1)
**Review input:** potentialIssues.md (implementation-readiness review of v2)

---

## 0. Changes from v1

| Area | v1 | v2 |
| :--- | :--- | :--- |
| Threading | Explosion path runs synchronously on the main thread | Async job pipeline with an MPSC completion queue and a per-frame GPU upload budget |
| Lifetime | `delete core_engine` in destructor | Explicit ordered teardown with the acting thread named per step (5.2, 5.3) |
| Meshing | Transvoxel with no LOD structure | Uniform-resolution Surface Nets for v2.0. Transvoxel deferred to an LOD milestone |
| Chunk layout | 32³ core, no border data | 32³ cells, 33³ core samples, 35³ padded volume. Edits propagate to neighbor aprons |
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
| Coordinate model | 32³ voxels, 34³ samples | 32³ core cells, 33³ core samples, 35³ padded volume (3.1) |
| Sample ownership | Apron and core symmetric | Core samples authoritative; apron samples strictly derived (3.1, 3.2) |
| Apron sync | Edit path only | Edit, generate, load, and unload paths all defined (3.1, 3.5) |
| Noise density | Implicitly treated as SDF | Density normalized to an SDF with explicit scale and gradient magnitude (3.5) |
| Residency | 118 KB per chunk always | EMPTY/SOLID chunks use a compact representation; material channel lazily allocated (3.1) |
| Core linkage | Separate DLL, C++ ABI across boundary | Static linkage into the extension, or a C API if a DLL is required (6.1, 6.2) |
| Navigation data | Per-chunk regions, unspecified lifecycle | Region groups with defined lifecycle and border padding (4.5) |
| Save consistency | Unspecified | Version-bracketed serialization with retry-or-discard; 33³ authoritative core samples only (3.2, 3.5) |
| Shutdown | Persist flushed after join | Single ordered sequence, drain specified by thread (5.3) |
| Signals | `explosion_applied` ambiguous | `explosion_requested` / `explosion_completed` (4.2) |
| Readiness | `is_idle()` conflated | `core_idle()` and `fully_committed()` (4.2) |
| Scheduler | Priority only | Fairness and backpressure rules, lane aging (3.4) |
| Streaming / residency | `view_distance_chunks` declared but residency policy unspecified | Explicit desired set, prefetch, load/generate routing, eviction, hard memory budget, and queue interaction (3.3, 3.4) |
| Thread start | Constructor | `_enter_tree` (5.2) |

---

## 1. Executive Summary

### 1.1 Objective

Design a cross-platform, high-performance, **modular voxel engine written in C++** as a Godot-independent core library that is statically linked into the GDExtension for v2.x. The core has no dependency on Godot. The first integration wrapper targets **Godot 4.x** through the **GDExtension API** using **godot-cpp**.

### 1.2 Core Capabilities

* **Explosions:** Spherical CSG subtraction on the TSDF, applied by worker threads. Results reach the screen within a bounded number of frames.
* **AI Companion Navigation:** Navigation updates for modified regions are asynchronous and use Godot's `NavigationServer3D`.
* **Persistent Saves:** Chunk serialization with homogeneous-chunk elision, quantized SDF, and LZ4 block compression, stored in region files.
* **Procedural Generation:** Multi-threaded, deterministic terrain generation with **FastNoise2**.
* **Physics:** Static collision generated from the same meshes as the visuals, updated asynchronously when chunks are remeshed.

### 1.3 Non-Goals (v2.0)

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

Meshing and normals need samples one step outside the owned cells on every side. Each chunk therefore stores a padded cube of samples:

| Region | Local sample index range | Count per axis | Notes |
| :--- | :--- | :--- | :--- |
| Core | 0 … 32 | 33 | The corners of the 32³ owned cells |
| Low apron | −1 | 1 | Copy of the neighbor's high core sample |
| High apron | 33 | 1 | Copy of the neighbor's low core sample |

* **Padded volume: 35³ samples.** (33 core + 1 apron on each side.)
* The apron width is exactly 1 sample, which is what central-difference gradient evaluation and Surface Nets cell sampling require at the boundary of the core.
* **Core samples are authoritative.** Exactly one chunk owns each core sample. **Apron samples are derived cache data** with no independent authority (see 3.2).

#### Sample Format

Truncated SDF stored as normalized `int16_t`.

* Truncation band: ±δ, with δ = 2.0 voxel units (configurable at build time).
* Encoding: `q = clamp(round(sdf / δ * 32767), -32767, 32767)`. Decoding: `sdf = q * δ / 32767`.
* Sign convention: **negative = solid, positive = air.** The surface is the `sdf = 0` isosurface.
* `INT16_MIN` and `INT16_MAX` are reserved as the clamped solid and clamped air markers. They mean "far beyond the band," not an exact distance.

* **Correction of a v2.0 error:** the earlier "34 × 34 × 34 samples" figure was wrong. A 32³ block of cells needs 33³ core samples (one more per axis than the cell count), and a 1-sample apron on each side gives 35³. The correct padded volume is **35 × 35 × 35**.

#### Compact Residency Representation

A chunk's sample data is stored in one of three forms, and the form is updated as the chunk changes:

| Form | Memory | Meaning |
| :--- | :--- | :--- |
| `EMPTY` | 0 bytes of sample data | All **authoritative core samples** are at the clamped air marker |
| `SOLID` | 0 bytes of sample data | All **authoritative core samples** are at the clamped solid marker |
| `DENSE` | 35³ × 2 B ≈ **84 KB** | Full `int16_t` padded sample array, including derived apron samples |

* A chunk that generates as `EMPTY` or `SOLID` allocates no sample array. It must still record its form so it can be promoted to `DENSE` when an edit or a neighbor edit touches it.
* Promotion to `DENSE` fills the array with the clamped marker of the previous form before applying the edit.
* This matters because v2.0 allocated a full array for every resident chunk, making an air-heavy world cost far more memory than its actual voxel content justifies. A view distance of 16 chunks in radius around the player is roughly 36k chunks, and most are pure air or pure solid.

#### Material Channel

* Stored as a parallel `uint8_t` per core sample, **lazily allocated**.
* No material array exists while every material in a chunk is 0. The first write of a non-zero material allocates a 33³ array (apron material is taken from the owning neighbor's core sample and never written locally).
* v2.0 always allocated this channel, costing 33% of a dense chunk's memory for a feature that does not exist in v2.0.
* Chunk memory per resident chunk is therefore **84 KB dense, 0 for `EMPTY`/`SOLID`**, plus about 35 KB (`uint8_t` × 33³) when materials are present.

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

#### Chunk State Machine

```
Absent -> Generating -> Clean <-> Dirty -> Meshing -> MeshReady -> Uploaded
                             ^                                |
                             +======= (version mismatch) =====+
                             +======= (unload: evict) ========+
```

Each chunk has a monotonically increasing `version` counter. Every authoritative mutation increments it (see 3.2). A mesh result is committed only if its captured version equals the chunk's current version. Otherwise it is discarded and the chunk returns to `Dirty`.

### 3.2 Concurrency and Data Ownership Contract

This section is **normative**. The v2.0 document specified version counters but no exclusion between concurrent readers and a writer. A version counter detects a race after it has happened; it does not prevent a mesh job from reading half-rewritten samples. The rules below close that gap.

#### Authoritative State

* A chunk's **core TSDF samples** (`DENSE` and `EMPTY` / `SOLID` forms) and its **core material array** are authoritative. Nothing else in the engine is.

#### Derived State

The following are caches derived from authoritative state and carry no weight in a dispute:

* Apron samples (copies of a neighbor's core samples).
* Meshed vertex and index buffers.
* Collision shapes built from mesh buffers.
* Navigation region geometry built from mesh buffers.

#### Mutation Rule

Only **Edit** and **Generate** lane jobs may mutate authoritative core TSDF or material data, and **only while holding the chunk's exclusive write lock**. There is at most one writer per chunk at any time.

* An edit job first computes its **complete lock set**: every chunk whose authoritative core will be mutated, plus every resident neighbor whose derived apron must be refreshed. **All locks in that complete set are acquired in ascending `ChunkKey::raw` order and released in descending order.** No second lock-acquisition phase is permitted. This fixed order prevents deadlock between overlapping edits handled by different workers; the lock set grows with the edit radius.

#### Read Rule

**Mesh** and **Persist** jobs do not read authoritative core samples while an Edit job may run. They read one of:

1. **Snapshot copies.** The job captures a `DENSE`-form copy of the 35³ padded volume for the chunk(s) it needs. For a 32³ chunk this is one 84 KB allocation and copy per chunk in the job. This is the preferred mechanism because it also decouples the job from unload.
2. **Protected reads.** The job holds the chunk's read lock (shared with other readers, exclusive against writers), plus a reference count so the chunk cannot unload mid-read.

Snapshot copies are the default. Protected reads are the optimization to use only if profiling shows the copy dominates, and only for Persist (which already streams the data). Mesh always snapshots.

#### Apron Propagation Rule

Apron samples are derived and are refreshed by the owner of the affected core sample:

1. Before any write lock is acquired, the Edit or Generate job computes the **complete lock set** of affected core owners and resident neighbors whose derived aprons must be refreshed.
2. The job acquires that entire set in ascending `ChunkKey::raw` order, then writes authoritative core samples and refreshes derived apron samples while holding the relevant locks.
3. A chunk whose apron changed but whose core did not **is not** marked `Dirty` if a mesh produces identical geometry. In practice, mark it `Dirty` only when an apron sample crosses the surface (changes sign relative to its neighbor's core sample). Sign-unchanged apron updates affect only normals, which is ignored for v2.0.

Rule 3 removes the v2.0 behavior of remeshing every neighbor of an explosion. An explosion of radius `R` near a chunk boundary marks at most the chunks whose core or apron changed, and only sign-crossing apron changes force a remesh.

#### Version Rule

* Every authoritative mutation (a write to core samples or core materials) increments the chunk's `version` **while the write lock is held**.
* A Mesh or Persist job captures the versions of all chunks it snapshots at dequeue time.
* A result is accepted only if the captured version equals the current version at completion time.

#### Save Consistency Rule

A Persist job serializing chunk `C` records `version_before` before it takes its snapshot and reads `version_after` from the chunk afterward.

* If `version_before == version_after`, the snapshot is consistent and may be written.
* If they differ, the chunk was mutated concurrently. The job **discards the snapshot and re-enqueues itself** at the tail of the Persist lane. It does not retry inline.

This is the rule v2.0 omitted entirely. Without it, a save can serialize half of an old chunk and half of an edited one.

#### Unload Rule

* A chunk holds a reference count. Every job that dequeues a chunk acquires one; every job that completes releases one.
* `Unload` marks a chunk for eviction and decrements to `n - 1` references, where `n` is the in-flight count. The chunk's data is destroyed when the count reaches zero. A queued job that resolves to an evicted chunk is dropped without being executed.
* No chunk memory is freed while any job holds a reference to it. This is the structural fix for the destructor use-after-free that v1 had.

#### Completion Rule

* A job result **owns its buffers** (vertex data, index data, collision triangles, nav triangles) until the main thread commits it.
* Workers never touch Godot objects, RIDs, `Object`s, or allocator-owned shared engine state.
* The MPSC completion queue is the only path from workers to the main thread. Its entries are self-owned structs.

#### Testing of This Contract

Every rule above maps to a unit test in section 7.1. In particular the TSan runs in 7.1 are not optional; a contract violation must fail CI, not surface later under load.

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
2. **Neighbor closure:** the six face-neighbors needed by active dirty chunks for apron completion and deterministic meshing. Missing neighbors outside the world are treated as absent-air and do not consume a residency slot.
3. **Prefetch set:** chunks within `view_distance_chunks + prefetch_distance_chunks`, ranked by predicted usefulness. Prefetch never displaces the active set.

Missing chunks are admitted in this order:

1. Missing **active neighbors** needed to unblock a visible/dirty chunk.
2. Missing **active chunks**, ordered by Chebyshev distance to the focus, then movement-direction score, then `ChunkKey::raw` as a stable tie-breaker.
3. Missing **prefetch chunks**, using the same ordering but lower priority.

For each requested chunk, the Streaming Manager first checks the region-file TOC for an `EDITED` record:

* If an edited record exists, enqueue a **Load** job.
* If no edited record exists, enqueue a **Generate** job.
* Load jobs therefore consume saved state when available; Generate is the fallback for unedited procedural terrain.

A streaming request reserves the chunk's worst-case **dense core payload** before the job enters a worker queue. The reservation also includes any required material allocation plus one mesh snapshot reservation when the chunk is admitted for meshing. If the reservation would exceed the memory budget, the request remains queued but is not admitted. Prefetch requests are the first to yield under pressure. Every later snapshot, mesh result, collision buffer, or navigation buffer allocation is charged against the same budget; an allocation that cannot be reserved causes that job to defer rather than exceed the hard cap.

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
* `Loading` reads an edited chunk payload from disk and reconstructs its authoritative 33³ core samples. Aprons are reconstructed from resident neighbors; they are never loaded from disk.
* `Generating` creates the authoritative 33³ core samples procedurally, then participates in the same apron-refresh path as a loaded chunk.
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

`resident_memory_budget_mb` is a hard ceiling for **engine-managed CPU memory associated with chunk streaming**, including resident authoritative sample/material arrays, apron storage, streaming reservations, worker snapshots, and queued completion buffers owned by the core/wrapper. It is configurable and defaults to **512 MiB**.

The dense sample payload is:

* TSDF: `35^3 × 2 = 85,750` bytes ≈ **83.7 KiB** per dense chunk.
* Material: `33^3 = 35,937` bytes ≈ **35.1 KiB** when allocated.
* Conservative dense-with-material planning cost: about **119 KiB per chunk**, before mesh/collision/navigation resources.

The streaming controller therefore treats the memory limit as an admission constraint, not as an after-the-fact warning:

$$\text{resident\_bytes} + \text{reserved\_bytes} \le \text{resident\_memory\_budget\_mb} \times 2^{20}$$

`view_distance_chunks` is a **target** and is satisfied whenever the memory budget permits. When the requested radius cannot fit, the engine does not allocate past the hard limit: it first suppresses prefetch, then reduces the effective active radius to the largest affordable value. The effective radius is exposed in telemetry so the game can detect memory-constrained streaming.

For reference, a fully dense 16-chunk Chebyshev radius contains `33^3 = 35,937` chunk positions and would require roughly **2.9 GiB of TSDF storage alone** before materials or derived resources. The design therefore does **not** treat `view_distance_chunks = 16` as a memory guarantee; the memory budget is authoritative.

Godot/driver allocations that are not controlled by the extension allocator (for example GPU residency) cannot be made a strict process-level hard bound by the core. Their CPU-side estimates are tracked separately in performance telemetry, while the streaming budget above is the hard bound for memory owned by this engine pipeline.

#### Interaction with Load, Generate, and the Completion Queue

* **Load vs. Generate:** Load is a dedicated worker-lane queue for edited chunks; Generate remains the fallback for unedited chunks. Both reserve memory before admission and both produce a resident authoritative chunk that then schedules meshing.
* **Priority:** Edit and Mesh work outrank Load; Load outranks Persist and Generate for latency-sensitive streaming. Generate is lowest priority and is the first work throttled when the active view is incomplete due to memory pressure.
* **Neighbor readiness:** A Mesh job for an active chunk is not admitted until its six face-neighbors are either resident or known absent. This prevents the streaming controller from generating a visible chunk only to stall its first mesh on missing apron data.
* **Completion queue:** Load and Generate jobs do **not** enter the MPSC completion queue because they mutate only core state. Their successful completion schedules Mesh work. Only results requiring main-thread Godot commits (mesh, collision, navigation) enter the completion queue.
* **Completion backpressure:** The existing completion depth caps in 3.4 remain in force, and admission is additionally blocked when queued completion buffers consume the configured streaming memory budget. Backpressure therefore limits both queue length and bytes.
* **Eviction under load:** Eviction is a low-priority streaming action. It is never allowed to interrupt an Edit job, and it waits for the chunk's references to reach zero as required by 3.2.

#### Streaming Update Frequency

The Streaming Manager recomputes the desired set at most once per frame and only when the focus crosses a chunk boundary or the velocity direction changes enough to alter prefetch ranking. This avoids repeatedly rebuilding the same request set while the player moves within one chunk.

The stream planner itself runs on the main thread because it operates on small chunk-coordinate sets and only enqueues core jobs. Disk IO, decompression, generation, apron refresh, and meshing remain worker-side.

### 3.4 Worker Pool and Scheduling

#### Lanes (highest to lowest priority)

1. **Edit:** explosions and other density modifications.
2. **Mesh:** proximity-driven meshing and collision bake for dirty chunks.
3. **Load:** disk reads and decompression for edited chunks requested by the Streaming Manager.
4. **Persist:** serialization, compression, and disk writes.
5. **Generate:** procedural generation of new chunks.

#### Workers

`max(1, hardware_concurrency() - 2)` threads. The main thread is excluded, and one core is left for the OS and the Godot render thread.

Workers pull from lanes in priority order. Strict priority starvation is a real failure mode v2.0 did not address: sustained editing can starve Persist and Generate indefinitely, so jobs eventually never run. The rules below bound that.

#### Fairness Rules

| Rule | Value | Purpose |
| :--- | :--- | :--- |
| Aging | A job's effective priority increases by 1 lane every 250 ms in queue | Guarantees lower-priority work eventually runs without violating active-view latency |
| Edit starvation bound | At most 16 consecutive Edit jobs before the lane must yield to Mesh or Load | Prevents streaming/meshing from never running |
| Persist reservation | Persist jobs are never aged past Mesh or Load priority, but are never starved: at least 1 Persist job runs per second while the queue is non-empty | Bounds data loss on crash |
| Load reservation | Load jobs outrank Generate and are admitted whenever an active chunk has a saved edit and memory headroom exists | Saved edits should not wait behind procedural generation |
| Generate preemption | Generate may run only after active Load work is admitted and only when the active chunk set still has memory headroom | View distance is a latency requirement |
| Edit coalescing | Two Edit jobs whose explosion spheres intersect are merged into one covering job | A burst of overlapping explosions cannot flood the lane |
| Lane budget | A single worker checks `shutting_down` between every job and between every chunk of a multi-chunk job | Teardown latency |

#### Backpressure Rules

| Condition | Response |
| :--- | :--- |
| Completion queue depth > 8 chunks | Workers stop dequeuing Mesh jobs and process Edit/Load only, until depth drops below 8 |
| Completion queue depth > 32 chunks | Edit jobs are also stalled; the main thread is the bottleneck and more work only grows memory |
| Streaming memory budget exhausted | Stop admitting prefetch and Generate work; continue only work required to make the active set resident or persist edited chunks | Prevents residency oversubscription |
| Pending unload of a chunk with in-flight jobs | The unload is deferred and retried on the next frame, not forced |

The completion-queue cap exists because unbounded accumulation would let one large explosion consume unbounded memory before the main thread ever sees it.

#### Cancellation

Every job checks a shared `std::atomic<bool> shutting_down` at safe points: between chunks, and between slices of a long edit. Edits already applied to authoritative state are never rolled back by cancellation.

#### Thread Start

The worker pool is created by `VoxelTerrain3D::_enter_tree()`, not in the constructor. Godot instantiates nodes freely in the editor, in the resource loader, and in the scene-instantiation path before they enter the tree. Spawning threads in a constructor leaks a pool per preview instantiation.

### 3.5 System Implementations

#### Explosion CSG (TSDF subtraction)

An explosion payload contains center `c` and radius `R`. The core:

1. Computes the axis-aligned bounds of the sphere.
2. Computes the **complete lock set** described in 3.2: every chunk whose core samples may change plus every resident neighbor whose derived apron must be refreshed. It acquires the entire set in ascending `ChunkKey::raw` order.
3. For each core-owning chunk, promotes it to `DENSE` if it is `EMPTY` or `SOLID`, then applies, for every **core** sample `p` in that chunk:

$$\text{NewSDF}(p) = \max\big(\text{CurrentSDF}(p),\; R - \lVert p - c \rVert\big)$$

   clamped to the truncation band ±δ. The formula is subtraction under the negative-solid convention: samples within `R` of `c` become air.

   **Only core samples are written.** Apron samples are never targets of an edit, because they are not authoritative. A chunk whose core is untouched is never written, even if the explosion passes over its apron.

4. Increments `version` on each chunk whose core samples changed.
5. For every chunk whose changed core samples border another chunk, refreshes that chunk's apron (3.2, Apron Propagation Rule), marking the neighbor `Dirty` only if an apron sample changed sign.
6. Enqueues Mesh jobs for the chunks marked in steps 4 and 5.

The "write every sample in the padded volume" step from v2.0 is removed. That formulation let multiple chunks independently edit what is logically the same world sample, and the claim that it kept aprons consistent did not follow from the algorithm.

#### Procedural Generation (FastNoise2)

* Generation runs on the Generate lane, one chunk per job.
* **FastNoise2 produces a density field, not a true SDF.** v2.0 therefore treats the generated field as an **SDF approximation** obtained from a first-order local distance estimate:

$$\text{sdf}_{approx} = \frac{\text{density} - \text{iso}}{\lVert \nabla \text{density} \rVert \cdot s}$$

   where `iso` is the iso-level of the noise type, `∇density` is the gradient magnitude in voxel units, and `s` is the voxel scale. This normalization reduces local distance-scale distortion near the isosurface, but it is not an exact signed-distance transform for arbitrary noise fields. The implementation must therefore validate **surface-position error, near-surface gradient magnitude, and repeated CSG stability** before treating the approximation as sufficient for v2.0. The gradient is evaluated by finite difference of the noise, so generation costs roughly 4 noise evaluations per sample.
* The chunk is promoted to `EMPTY` or `SOLID` if generation produces a uniform result, and no sample array is allocated in that case.
* Output is deterministic for a given seed, noise graph, and SIMD level. Cross-machine determinism is **not** guaranteed by FastNoise2's SIMD dispatch. Saved edits are stored as full chunk data, so a change in generation output does not corrupt existing saves. Re-generation of unedited chunks may differ across machines; accepted for v2.0, and listed as a risk in section 8.
* Marks the chunk `Dirty` for meshing, then handles apron consistency (see Apron Consistency Across Lifecycle below).

#### Apron Consistency Across Lifecycle

Apron samples are refreshed at four points. v2.0 handled only the first.

| Lifecycle event | Apron behavior |
| :--- | :--- |
| **Edit** | Owner writes core, then refreshes neighbor aprons in the same lock round (3.2) |
| **Generate** | New chunk computes its core from noise, then refreshes the aprons of all already-resident neighbors |
| **Load** | Chunk's core comes from disk. Aprons are **not** stored on disk (they are 2× the data and fully derivable). After a chunk loads, its apron is filled from any already-resident neighbors, and its neighbors' aprons are refreshed from it |
| **Unload** | Chunk is removed; neighbors' aprons that referenced it are marked stale and refilled on the next meshing pass |

The consequence of "aprons are not stored" is that **load order matters**: a chunk loaded before its neighbor has a stale apron until that neighbor arrives. Meshing is deferred until all six face neighbors are resident or known absent, and `Absent` neighbors contribute a clamped air apron. This makes mesh output independent of load order.

#### Meshing (Surface Nets for v2.0)

* **Algorithm:** Naive Surface Nets over the 35³ padded volume.
* **Cell ownership:** A chunk emits geometry only for its 32³ owned cells (local `0..31`), using the 35³ padded samples so its outer cells close correctly against the apron. A chunk never emits the cell at local index 32; that belongs to the neighbor. This rule eliminates both duplicate faces and missing faces at chunk borders.
* **Gradient normals:** computed by central difference of the padded samples. A 1-sample apron is sufficient for every core sample, including the boundary core sample at local index 32, whose neighbors at 31 and 33 are both present.
* **Output:** Interleaved vertex buffer (position, normal, material), `uint32_t` index buffer, per-chunk bounds, and the collision triangle set (see 4.4). Ownership moves to the completion queue.
* **Why not Transvoxel for v2.0:** Transvoxel's transition cells exist to bridge chunks of different resolution. v2.0 has one resolution, so those tables add cost with no benefit. LOD milestone M5 adopts Transvoxel with an octree or clipmap structure and transition masks.

#### Save and Load Pipeline

Per chunk, in order:

1. **Homogeneous form:** If every **authoritative core sample** in the 33³ core sample array is at the clamped air marker, store `CHUNK_EMPTY`. If every authoritative core sample is at the clamped solid marker, store `CHUNK_SOLID`. Neither carries a payload. Derived apron samples are not considered for the homogeneous test.
2. **Quantize and split:** Otherwise the `int16_t` core TSDF is written. It is already quantized in memory, so no conversion occurs. The **33³ authoritative core sample array** is written as **two byte planes** (low byte, high byte) rather than interleaved raw `int16` values. Apron samples are never serialized. SDF values along a sample axis are strongly correlated, so planar separation lets LZ4 find runs it cannot find in an interleaved stream. The material channel, when present, is a third plane containing the 33³ core material samples.
3. **Compress:** `LZ4_compress_default` per plane. Compressed length is recorded in the TOC.
4. **Version bracket:** The Persist job records `version_before`, snapshots, reads `version_after`, and writes only if the versions match (3.2, Save Consistency Rule).
5. **Write:** Into a region file (see 3.6).

RLE is not in the pipeline. It gives no benefit on continuous SDF values. Only **edited** chunks are written; an unedited chunk is regenerated on load, so a save file contains the delta from generation, not the world. Chunks carry an `edited` flag, set by the first authoritative mutation and cleared only by an explicit regeneration.

#### Asynchronous Navigation (see 4.5)

Navigation is owned by Godot's `NavigationServer3D`. The core supplies triangle data for modified regions. The core does not run its own `dtTileCache`.

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
  * `void trigger_explosion(Vector3 position, float radius)`: enqueues an Edit job and returns immediately. **Never blocks.**
  * `void save_world()`: flushes the Persist queue and blocks until it completes. Intended for editor use and shutdown.
  * `void set_stream_focus(Vector3 position, Vector3 velocity)`: sets the main-thread streaming focus supplied by the game (normally the player/camera position and velocity). The wrapper passes this to the core once per frame.
  * `bool core_idle() const`: true when no core jobs are queued or in flight. Says nothing about Godot-side state.
  * `int get_effective_view_distance_chunks() const`: returns the current affordable active radius after memory-budget clamping.
  * `int get_prefetch_distance_chunks() const` / `set_prefetch_distance_chunks(int)`: controls the soft prefetch radius.
  * `int get_resident_memory_budget_mb() const` / `set_resident_memory_budget_mb(int)`: controls the hard engine-managed CPU residency budget.
  * `bool fully_committed() const`: true when core jobs are idle, the MPSC completion queue is empty, and all navigation region groups invalidated by completed edits report **synchronized** with the current terrain revision. This is the readiness check gameplay code must use; an empty completion queue alone does not mean navigation has finished rebuilding.
  * `navigation_groups_synced()`: wrapper-internal check that tracks the terrain revision of each invalidated region group and verifies the corresponding `NavigationServer3D` map synchronization state before reporting readiness.
* **Signals:**

  | Signal | Emitted when | Payload |
  | :--- | :--- | :--- |
  | `explosion_requested(position, radius)` | The job is enqueued, on the calling thread | Call's arguments |
  | `explosion_completed(position, radius, affected_chunks)` | The edit is applied and committed to authoritative state, on the main thread | World state has changed at this point |
  | `explosion_failed(reason)` | The job could not be enqueued (pool shut down, or the position is outside world bounds) | Reason string |
  | `chunk_committed(chunk_key)` | A chunk's mesh, collision, and navigation are live in Godot | Raw chunk key |

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
trigger_explosion()          [main thread] -> enqueue Edit job, return
  Edit job                   [worker]      -> lock, mutate core TSDF, refresh aprons, enqueue Mesh
  Mesh job                   [worker]      -> snapshot padded volume, Surface Nets, build collision + nav
  completion queue (MPSC)    [workers -> main]
VoxelTerrain3D::_process()   [main thread]
  drain completions while chunk_start + budget not exceeded
    commit mesh buffer, collision shape, navigation region on the main thread
  emit chunk_committed(...)
```

#### The Budget Is Soft

The loop attempts to respect `upload_budget_ms` but **cannot guarantee it**. The reasons:

* The budget is checked **before** a chunk starts, not mid-chunk. A single 32³ chunk commit may exceed 2 ms on low-end hardware.
* **Commits are never split across frames.** A chunk's mesh is unusable if half its vertices are uploaded; splitting would produce visible geometry corruption. So the loop either commits a chunk whole or defers it to the next frame.

The requirement is therefore stated as: **the average frame-time contribution of voxel commits stays under `upload_budget_ms`, and no frame exceeds `upload_budget_ms` by more than the cost of one chunk commit.** The exception is bounded and measurable per hardware target.

Commit cost is not dominated by vertex copying. `ArrayMesh` construction, `create_trimesh_shape`, and navigation region submission have substantially different and often larger costs than the `memcpy`. Section 7.3 measures them separately.

#### Queue Backpressure

If the completion queue exceeds the depth cap in 3.4, `_process` drains **without the budget check** for that frame, because an over-deep queue is the worse failure. This is the one condition where the budget is knowingly exceeded.

### 4.4 Physics Collision

* **Source:** Collision geometry is generated from the same Surface Nets output as the visual mesh. It is not a separate meshing pass.
* **Shape:** `ConcavePolygonShape3D` (equivalent to `PhysicsServer3D` `SHAPE_CONCAVE_POLYGON_SHAPE`) on a `StaticBody3D` per chunk. Chunks are static, so this is valid.
* **Triangle soup, not indexed buffers.** Godot's concave shape takes a de-indexed triangle soup: 3 floats × 3 vertices per triangle, with no shared indices. Indexed vertex data compressed 2–3× by reuse will roughly triple in memory as a collision soup. Budget for it.
* **Cost is on the main thread.** `create_trimesh_shape` runs during commit, so it counts against the upload budget and is measured separately in 7.3. It is not covered by the `memcpy` cost in 4.1.
* **Decimation:** Not in v2.0. It is treated as a **planned escape hatch**, instrumented from M3 onward, not merely a future optimization. If collision bake time or memory exceeds budget, the fallback is a decimated or voxel-aligned collision mesh generated as a separate simpler Surface Nets pass (for example on a 2× subdivided sample grid).
* **Lifecycle:** The collision shape is replaced atomically when the chunk is recommitted. Stale shapes for removed chunks are freed on the main thread.
* **Player and projectile safety:** Collision for a chunk is only committed after its visual mesh. A chunk that is `Uploaded` but not yet collidable is not a problem for characters, because spawning logic uses `fully_committed()` (4.2), not `core_idle()`.

### 4.5 Navigation

v1 proposed a standalone `dtTileCache`. That approach was removed because:

1. It duplicates the memory and update cost of Godot's built-in navigation, which is also Recast-based.
2. `dtTileCache` rasterizes 2.5D heightfields. Overhangs and caves need a different rasterization setup.
3. Godot's `NavigationAgent3D`, RVO avoidance, and the navigation debug view all depend on `NavigationServer3D`.

#### Region Ownership

* Navigation regions are **groups of chunks**, not single chunks. The default group is **4 × 4 chunks in X/Z** (a 16×16 chunk area), chosen to amortize region management and to make border handling tractable. Group size is a project property, defaulting to 4.
* Each region group owns exactly one `NavigationRegion3D` and one navigation mesh resource on the terrain's map.
* A region group is created when any of its chunks is generated or loaded, and destroyed when **all** of its chunks are unloaded.

#### Border Seams (erosion)

v2.0 said "per-chunk regions" and left the boundary undefined. This is a real correctness hole: **Recast erodes every navmesh border by the agent radius**, so a per-chunk bake leaves a gap between adjacent regions where no walkable surface exists near the region edge. A companion standing near a chunk border falls off the graph even though the ground is continuous.

* Regions are baked on the **region group's** mesh, not the individual chunk's mesh, so the interior borders do not exist.
* At the outer border of a region group, nav geometry is padded by `agent_radius + connection_radius` before rasterization, so the erosion is spent on padding rather than on usable space.
* Adjacent region groups must agree on the padding overlap and the connection margin, or seams reappear at the group boundary. Both groups bake the same shared padding region, so edge pieces coincide.
* The padded navmesh is only used for pathfinding; the visual mesh is unaffected.

#### Update Contract

* Only region groups containing chunks whose **sign-crossing** apron changes or core changes are rebuilt (3.2, Apron Propagation Rule).
* **Region ownership of an edit:** a committed edit invalidates the containing region group's navmesh and marks it for rebuild on the NavigationServer's async thread.
* **Update ordering:** an edit is only submitted to navigation after its chunks are committed (mesh live), so navigation never bakes geometry that does not exist yet.
* **Re-edit during rebuild:** if a region group is rebuilt and a new edit arrives for one of its chunks before the rebuild completes, the group is simply marked dirty again and rebuilt a second time. The rebuild is idempotent and there is no partial-state hazard.
* **Path safety after an edit:** a path query is valid only once the containing region group is marked synced. The companion should check `NavigationServer3D.map_get_iteration_id()` or the region group's sync state before trusting a path across a recently edited area.

#### Navmesh Source

* Navigation geometry initially comes from the **same Surface Nets geometry as the render mesh** for correctness validation in v2.0. This keeps the first implementation consistent by construction.
* This is not a permanent performance assumption. Navigation baking may process significantly more triangles than pathfinding requires, so M4 must profile nav bake time and memory independently.
* If profiling shows nav bake cost dominates, the planned escape hatch is a simplified navigation mesh baked from a coarser sample grid. That would intentionally allow navigation and render geometry to diverge, so it remains a tracked decision in section 8.

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

The entry point is non-blocking. Work happens on the core worker pool. Results are committed on the main thread in `_process`.

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
    ADD_SIGNAL(MethodInfo("explosion_requested", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "radius")));
    ADD_SIGNAL(MethodInfo("explosion_completed", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "radius"), PropertyInfo(Variant::INT, "affected_chunks")));
    ADD_SIGNAL(MethodInfo("explosion_failed", PropertyInfo(Variant::STRING, "reason")));
    ADD_SIGNAL(MethodInfo("chunk_committed", PropertyInfo(Variant::INT, "chunk_key")));
}

VoxelTerrain3D::VoxelTerrain3D() {
    // Constructor does NOT create the core or start threads.
    // Godot instantiates nodes freely in the editor and in the loader;
    // spawning a pool here would leak one per preview instantiation.
    core_engine = nullptr;
}

VoxelTerrain3D::~VoxelTerrain3D() = default;

void VoxelTerrain3D::_enter_tree() {
    core_engine = new CoreVoxelEngine();
    core_engine->start();            // worker pool created here (see 3.4)
    core_engine->configure_streaming(view_distance_chunks, prefetch_distance_chunks,
                                     resident_memory_budget_mb, world_height_chunks);
    core_engine->set_upload_budget_ms(upload_budget_ms);
}

void VoxelTerrain3D::_exit_tree() {
    // Shutdown sequence is fixed and ordered. See 5.3.
    if (core_engine) {
        core_engine->shutdown(save_on_exit);   // blocks until complete
        delete core_engine;
        core_engine = nullptr;
    }
}

void VoxelTerrain3D::trigger_explosion(Vector3 p_position, float p_radius) {
    CoreVector3 native_pos(p_position.x, p_position.y, p_position.z);
    const bool enqueued = core_engine->enqueue_explosion(native_pos, p_radius);

    if (enqueued) {
        emit_signal("explosion_requested", p_position, p_radius);
    } else {
        emit_signal("explosion_failed", String("engine not ready or position out of bounds"));
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
    // Streaming planning is main-thread only; the core owns all worker-side IO/generation.
    core_engine->update_stream_focus(stream_focus_position, stream_focus_velocity);

    while (true) {
        std::optional<CompletedChunk> done = core_engine->try_pop_completion();
        if (!done) {
            break;
        }
        commit_chunk(*done);   // mesh, collision, navigation: all main thread
        emit_signal("chunk_committed", static_cast<int64_t>(done->key.raw));

        if (Time::get_singleton()->get_ticks_usec() - frame_start_us >= budget_us
            && core_engine->completion_queue_depth() > 0) {
            break;   // resume next frame unless the queue is backing up (see 4.3)
        }
    }

    // Edits committed to authoritative state since the last frame are announced here.
    for (const EditResult &e : core_engine->drain_completed_edits()) {
        emit_signal("explosion_completed", e.position, e.radius, static_cast<int>(e.affected_chunks));
    }
}
```

Notes:

* `frame_start_us` and `budget_us` are captured from `Time::get_singleton()->get_ticks_usec()` and `upload_budget_ms` at the top of `_process`.
* `_process` also feeds the wrapper-owned `stream_focus_position` and `stream_focus_velocity` to the core Streaming Manager. `set_stream_focus()` updates those values on the Godot main thread.
* `emit_signal` is called only from the Godot main thread: `trigger_explosion` (which is already on the main thread), and `_process`. Never from a worker.
* `trigger_explosion` enqueues and emits `explosion_requested` on the main thread. The v2.0 comment claimed it was thread-safe; it is main-thread only, because `emit_signal` is not reentrant.
* `explosion_completed` is emitted from `_process`, not from `trigger_explosion`. Worker threads never touch Godot objects (3.2, Completion Rule).

### 5.3 Teardown Protocol

`CoreVoxelEngine::shutdown(bool save)` runs exactly one ordered sequence. v2.0 listed a Persist flush **after** the worker join, which is impossible: once workers are joined, no thread remains to process the Persist queue. Each step below names the thread that performs it.

| # | Step | Thread |
| :-- | :--- | :--- |
| 1 | Set `shutting_down = true`. Stop accepting new jobs. | Caller |
| 2 | Signal all workers. Each finishes its current chunk, returning to idle. | Workers |
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
* No step may run concurrently with another. The sequence is a single-threaded critical section, and `VoxelTerrain3D::_exit_tree` must not call into the core after step 7.

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
* **Single writer:** two Edit jobs overlapping on a chunk are serialized; TSan reports no data race.
* **Canonical lock order:** two concurrent edits touching overlapping chunk sets complete without deadlock. Run with a randomized lock-acquisition delay to force contention.
* **Snapshot isolation:** a Mesh job running on a snapshot produces a consistent mesh while an Edit job concurrently mutates the same chunk.
* **Save consistency:** a Persist job whose chunk is mutated mid-snapshot discards and re-enqueues; the written chunk is always either fully pre-edit or fully post-edit.
* **Unload under reference:** unloading a chunk with one in-flight job does not free its memory until the job completes (ASan).
* **VoxelTerrain3D pool start:** constructing the node in the constructor does not create threads; threads appear only after `_enter_tree`.
* **Streaming admission:** the planner never exceeds `resident_memory_budget_mb`, reservations are released on load/generate failure, and prefetch/generate admission stops under budget pressure.
* **Streaming order:** active neighbor dependencies outrank active chunks, active chunks outrank prefetch, Load outranks Generate, and stable tie-breaking produces deterministic request order.
* **Eviction safety:** chunks outside the prefetch radius are evicted farthest-first, edited chunks persist before eviction, and in-flight references prevent destruction.

Streaming and residency (3.3, 3.4):

* **Desired-set bound:** a Chebyshev radius `r` requests no more than `(2r + 1)^3` chunk coordinates before world bounds are applied.
* **Prefetch distance:** no chunk outside `view_distance_chunks + prefetch_distance_chunks` is requested unless explicitly retained by an in-flight job dependency.
* **Load-before-generate:** an edited chunk with a region-file record is scheduled on Load and is not regenerated. An unedited chunk schedules Generate.
* **Completion isolation:** Load and Generate never enter the MPSC completion queue; Mesh/Physics/Navigation results do.
* **Memory hard cap:** aggregate resident + reserved engine-managed CPU bytes never exceed `resident_memory_budget_mb`, including snapshots and completion buffers.
* **Budget degradation:** when the budget is exhausted, prefetch is suppressed/evicted before active chunks are dropped; effective view radius is reduced only after prefetch reaches zero.
* **Transient allocation refusal:** a mesh/collision/navigation result cannot allocate past the budget; the job defers until memory is available.
* **Eviction ordering:** chunks outside the prefetch radius are evicted farthest-first, edited chunks persist before eviction, and in-flight references prevent destruction.

Geometry (3.1, 3.5):

* **Cell ownership:** a 2×1×1 chunk pair produces no duplicate faces and no missing faces on their shared plane. Meshing both chunks yields exactly one quad set per boundary cell.
* TSDF quantize/dequantize error bounded by `δ / 32767` across the band.
* **Density-to-SDF approximation:** on representative generated terrain, measure surface-position error against a reference signed-distance estimate, near-surface gradient magnitude, and cumulative error after repeated explosion CSG operations. M1 cannot close until these remain within the project-defined tolerances.
* Explosion edit: every **core** sample within `R` of the center becomes air. Core samples outside `R` are unchanged. **Apron samples of untouched chunks are unchanged**, and apron samples of neighbors are refreshed to match their owners' cores.
* Surface Nets: a sphere SDF produces a closed mesh with no boundary edges, including at chunk boundaries.
* **Multipart meshing:** a world meshed as one 64³ region equals the union of the same world meshed as 2×2 chunks of 32³. Any vertex or index divergence is a seam bug.
* **Compact promotion:** an `EMPTY` chunk edited once promotes to `DENSE` with all air-clamped samples; an edited `SOLID` chunk promotes likewise.
* **Load-order independence:** a chunk meshed before its neighbor is loaded equals the same chunk meshed after, with the absent neighbor treated as clamped air.

Serialization (3.5, 3.6):

* Save/load round-trip is bit-exact for `int16_t` payloads. Homogeneous chunks restore as `EMPTY` or `SOLID`.
* **Version bracket:** a mutation landing between a chunk's `version_before` and `version_after` forces a discard-and-retry.
* Region file: a crash during write (simulated by truncating the temp file) leaves the last good region readable.
* **Edited-chunk-only saving:** an unedited chunk is not written to disk at all.

### 7.2 Integration Tests (Godot)

* Run the game through the Godot MCP server (`project_run`). Use `game_eval` or `game_manage` to trigger explosions and verify the completion queue drains.
* `trigger_explosion` returns in under 1 ms of main-thread time regardless of radius.
* `set_stream_focus` does not perform disk IO, generation, meshing, or Godot resource creation synchronously; it only updates the main-thread streaming focus state.
* Verify `explosion_requested` fires before `explosion_completed`, and that `explosion_completed` fires after the world state has changed.
* Verify `fully_committed()` is false while a completed chunk sits in the MPSC queue, and remains false while an invalidated navigation region group is still rebuilding. This is the case v2.0's `is_idle()` got wrong.
* Measure main-thread frame time during an explosion.
* Verify `VoxelAICompanion.get_voxel_path()` returns a path after an explosion once the affected chunks are committed **and the affected navigation region groups are synchronized**.
* Verify no navigation gap at a region-group border: path across a 4×4 region group boundary with a companion agent of the configured radius.

### 7.3 Performance Targets (to validate, not assumed)

Targets apply to the **reference hardware**: an 8-core x86-64 CPU, an SSD, and a mid-range discrete GPU. Record the actual machine in `AGENTS.md` when M0 completes; until then these numbers are targets, not commitments.

| Metric | Target | Notes |
| :--- | :--- | :--- |
| Explosion, main-thread cost, mean | < 2 ms per frame | Enforced by the soft upload budget (4.3) |
| Explosion, main-thread cost, p99 frame | < `upload_budget_ms` + one chunk commit | The budget exception is bounded and measured |
| Explosion, time to visible result | < 3 frames for a 4-chunk edit | Depends on worker load |
| Generated terrain surface error | Project-defined tolerance | Validate the density-to-SDF approximation before M1 closes |
| Generated SDF near-surface gradient | Project-defined tolerance around 1.0 | Validate local distance behavior before repeated CSG edits |
| Navigation region update, single group | < 10 ms | Asynchronous, off main thread. Verify API availability at M0 |
| Chunk memory, dense | ≤ 84 KB resident per chunk | From 3.1 |
| Chunk memory, `EMPTY`/`SOLID` | 0 bytes of sample data | From 3.1 |
| Streaming budget utilization | ≤ 100% of `resident_memory_budget_mb` | Hard admission bound for engine-managed CPU memory, including transient worker/completion buffers |
| Active chunk admission | Meets `view_distance_chunks` target when memory permits | Report effective radius separately when budget-constrained |
| Mesh commit, split cost | vertex copy / collision shape / nav submission reported separately | From 4.3: these dominate differently |

Commit cost must be reported as **average, p95, p99, and worst-case dense chunk**, for each of vertex upload, collision shape creation, and navigation submission measured independently. A single aggregate average hides the case that actually causes a visible hitch.

---

## 8. Open Questions and Risks

1. **Chunk size vs. commit cost.** A 32³ chunk with a 35³ padded volume is about 84 KB. A single chunk commit may exceed 2 ms on some hardware. Decide after profiling whether to reduce chunk size to 16³. (Committed: commits are never split, so the only lever is chunk size.)
2. **Streaming memory budget vs. view distance.** A 16-chunk Chebyshev radius contains 35,937 chunk positions and cannot fit in a 512 MiB budget if all chunks are dense. The engine therefore treats `view_distance_chunks` as a target and reports an effective radius when memory-constrained.
3. **Snapshot copy cost.** The Mesh lane snapshots 84 KB per chunk. If the copy shows up in profiling, switch Persist to protected reads (3.2). Mesh stays on snapshots; a mesh that reads live samples is a torn-mesh bug.
4. **Collision memory.** Concave shapes require a de-indexed triangle soup, which is roughly 3× the indexed vertex data (4.4). Instrument from M3. Planned escape hatch: a decimated or voxel-aligned collision mesh.
5. **Navigation region size.** 4×4 chunks is the default. Benchmark 4×4 versus 8×8 before M4 closes; both fix the border-erosion seam differently.
6. **Async navigation API availability.** Verify the required `NavigationServer3D` APIs exist at the pinned Godot version during M0 (4.5). If not, either raise `compatibility_minimum` above 4.2 or drop the async rebuild path.
7. **Cross-machine FastNoise2 determinism.** Accepted for v2.0 (3.5). Revisit if multiplayer or seed-shared worlds are added.
8. **Physics on uncommitted chunks.** Spawn and teleport logic must use `fully_committed()` (4.2), never `core_idle()`. Gameplay-side requirement.
9. **Edit coalescing.** Two intersecting explosions merge into one Edit job (3.4). The merge policy for non-intersecting bursts is undefined; add a cap if profiling shows Edit-lane saturation.
10. **Flush interval.** Region files flush per region, not per chunk (3.6). Choose a dirty-chunk count or time interval at M4 and record the resulting maximum data-loss window.

## 9. Milestones

| ID | Milestone | Exit criteria |
| :--- | :--- | :--- |
| M0 | Core skeleton + contract | ChunkKey, 35³ sample layout, compact forms, streaming planner + hard memory admission, worker pool, teardown, static linkage; **all 3.2/3.3 contract unit tests pass under TSan** |
| M1 | Procedural generation and meshing | Gradient-normalized SDF approximation; Surface Nets; multipart-meshing equivalence test passes; generated surface/gradient error remains within project-defined tolerances |
| M2 | Edits | Explosion CSG on core samples with apron refresh and canonical lock order; sphere-SDF closed-mesh test passes |
| M3 | Godot integration | `VoxelTerrain3D` renders, collides, and commits within the frame budget; commit cost split by operation |
| M4 | Persistence and navigation | Region-file saves with version bracketing; `VoxelAICompanion` path works after edits, including across region-group borders |
| M5 | LOD | Octree or clipmap LOD, Transvoxel transitions |

M0 now includes the ownership-contract tests. That is a change from v2.0, where the contract was unspecified and M0 could pass without it.

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
| Ambiguous apron ownership | Section 3.1: core authoritative, apron derived. Section 3.2: apron refresh rule |
| Ambiguous cell/sample coordinate model | Section 3.1: cells vs. samples, 33³ core, 35³ padded, inclusive-low/exclusive-high ownership |
| Budget claim self-contradictory | Section 4.3: soft budget, per-operation measurement; 7.3 reports mean/p95/p99/worst-case |
| Wrong teardown order | Section 5.3: one ordered sequence with the acting thread named per step |
| `explosion_applied` ambiguous | Section 4.2: split into `explosion_requested` / `explosion_completed` / `explosion_failed` |
| `is_idle()` too weak | Section 4.2: `core_idle()` and `fully_committed()`, with navigation synchronization included in readiness |
| No scheduler fairness | Section 3.4: aging, starvation bounds, coalescing, queue caps |
| Collision as hidden cost / tie to render mesh | Section 4.4: planned escape hatch, measured separately, instrumented from M3 |
| Navigation contract missing | Section 4.5: region groups, lifecycle, erosion padding, update ordering |
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
| Missing chunk streaming / residency management | Section 3.3: desired set, prefetch, load/generate routing, eviction, hard memory budget, and queue interaction |
| Threads spawned in constructor | Section 5.2, 3.4: pool starts in `_enter_tree` |
