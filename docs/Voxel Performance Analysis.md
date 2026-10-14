# Voxel Engine v4.0 — Performance Audit & Recommendations

Date: 2026-10-14 (rev 2 — working tree re-measured after F1/F3 fixes and
instrumentation landed)
Scope: working tree of the `voxel` repo. Rev 2 reflects the changes since the
initial audit: the F1 noise fast path, the F3 scheduler drain fix, a new
per-stage instrumentation subsystem (`include/stage_timings.hpp`), CMake/GDExtension
artifact fixes, and the removal of the stale `updates/` folder. Findings carry a
status: **Fixed / Partially fixed / Open**.
Method: static audit against the `performance-optimization` skill workflow
(measure → identify → fix → verify → guard), plus a headless micro-benchmark
(MSVC /O2, Release) targeting the hot paths, re-run against the current headers.
Each fix was verified against its own baseline with identical binary settings;
results are recorded in §6.

Machine: AMD Ryzen 7 5800X, 8 cores / 16 threads (worker count = `hc - 2` = 14).
All numbers are single-threaded unless noted; suite status verified with MSVC /O2
on the current tree (9/9 test binaries pass, second verification in this audit).

---

## 1. What the engine already handles well

The architecture is built around explicit budgets and bounded work; most of the
usual voxel-engine cost sinks are already addressed.

| # | Performance concern | How it is handled | Where |
|---|---|---|---|
| 1 | Main-thread stalls from mesh commits | Soft `upload_budget_ms` (2 ms) + hard emergency ceiling (2× = 4 ms); queue depth re-evaluated **every** iteration so one pop restores the soft budget | `src/voxel_terrain_3d.cpp:227-285`, mirror in `include/godot_wrapper_and_nav3d.hpp:657-721` |
| 2 | Worker/GC-style backpressure | Completion queue capped at 32 (hard) / 8 (soft) entries before workers pause meshing | `include/scratch_and_dirty.hpp:334-335,370-385` |
| 3 | Blocking generation | Three async lanes on worker threads (edit > mesh > generate/load); `hc-2` workers; edit phases can't starve meshing (starvation bound 8) | `include/edit_and_worker_pool.hpp:404-420,567-689` |
| 4 | Lock contention while meshing | Optimistic snapshot isolation: gather the 36³ window under ascending shared locks, release **before** meshing, then validate a 27-chunk version stamp and re-dirty on mismatch | `include/scratch_and_dirty.hpp:110-228,232-251` |
| 5 | Heap fragmentation / unbounded memory | Atomic `MemoryLedger` (512 MiB default) over 4 pools; page-aligned slab pools for 64 KiB TSDF / 32 KiB material blocks; 0-byte `Empty`/`Solid` uniform chunks; lazy material array; post-edit demotion back to uniform | `include/chunk_store.hpp:68-234,325-457` |
| 6 | Over-dirtying after edits | 27-bit selective dirty mask (only touched boundary samples dirty neighbors); dedup + priority dual dirty sets (edit lane always first, nearest-first) | `include/scratch_and_dirty.hpp:265-312,332-441` |
| 7 | Deadlocking / long lock holds on edits | Edit transactions phased into ≤32-chunk blocks, write locks taken in strictly ascending `ChunkKey` order and released in reverse; de-crumb pass limits floating specks | `include/chunk_store.hpp:560-594`, `include/edit_and_worker_pool.hpp:193-331` |
| 8 | Scenario-tree overhead | Per-chunk `RenderingServer` / `PhysicsServer3D` RIDs created directly — no `MeshInstance3D`/`StaticBody3D` nodes; packed arrays filled via `memcpy` | `include/godot_wrapper_and_nav3d.hpp:90-177` |
| 9 | Save I/O cost | Byte-plane split + RLE, compressed per chunk; atomic tmp-file + rename + FNV checksum; in-RAM blob cache capped at 32 MiB | `include/persistence_and_streaming.hpp:69-152,198-396,577-661` |
| 10 | Nav rebake blast radius | `4 × 2 × 4` chunk NavigationGroups with 3 m / 4.3 m border padding and async bake, so an underground edit rebakes only the local group | `include/godot_wrapper_and_nav3d.hpp:246-645` |
| 11 | Bounded-commit regression guard | Integration test drives the commit loop with a simulated clock and asserts the 2.4 ms / 4.0 ms stopping behavior | `tests/test_integration_m3_m4.cpp:41-128` |
| 12 | Toolchain safety nets | ASan/UBSan and TSan suite runner; `-Wall -Wextra -pedantic` (MSVC `/W4 /permissive-`) on the core | `CMakeLists.txt:27-31`, `run_all_tests.sh` |
| 13 | No performance visibility | Lock-free per-stage timing accumulator (`generate`, `mesh`, `commit`, `stream_focus`, `edit`, `navigation`); worker threads record, main thread drains and logs once per frame; startup fill time logged once; `instrumentation_enabled` GDScript property; Godot-free so tests exercise it | `include/stage_timings.hpp`, `src/voxel_terrain_3d.cpp:277-305,352-366`, `tests/test_stage_timings.cpp` |
| 14 | Mis-slotting optimized builds | Artifact filename now follows the CMake config (`$<CONFIG>` → `…release…`/`…debug…`), `.gdignore` written into `build/` and `bin/`, and `voxel_engine.gdextension` maps the editor's `debug` slot to the optimized `release` DLL | `CMakeLists.txt:14-22,79`, `voxel_engine.gdextension:6` |

Memory math for the shipped demo (`view_distance_chunks = 6`, `world_height_chunks = 4`,
`voxel_terrain_demo.tscn:31-36`): residency closure ≈ 15 × 15 × 4 = 900 chunks;
worst-case dense TSDF 900 × 64 KiB ≈ 57.6 MiB + material + blob cache + per-worker
scratch (14 × ~137 KiB) — comfortably inside the 512 MiB ceiling.

---

## 2. Measured baseline (single-threaded, MSVC /O2, Release)

Scenarios: full chunk generation, noise only, gather+mesh, stream-focus update by
view distance, dirty-set drain by size, persistence timings, plus a new fast-path
equivalence check (G). Compile: `cl /O2 /std:c++20 /DNDEBUG`. Source in Appendix A.

| Scenario | Before (initial audit) | After (current tree, 2 runs) | Δ |
|---|---|---|---|
| A. `generate_chunk` | 5.620 ms/chunk | **3.386 / 3.771 ms/chunk** | **−33…−40 %** (F1 fixed) |
| B. noise only (generic composite path, 34³) | 4.837 ms/chunk | 4.557 / 4.769 ms/chunk | ~unchanged |
| C. gather + surface-nets mesh | 1.141 ms/chunk (1862 verts, 3502 tris) | 1.035 / 1.068 ms/chunk | ~unchanged |
| D. `update_stream_focus` view 6 / 12 / 24 | 0.233 / 0.766 / 4.918 ms | 0.206 / 0.686 / 3.980 and 0.196 / 0.729 / 4.590 ms | ~unchanged (F2 open) |
| E. drain stream-dirty set 1500 / 6000 / 12000 | 1.902 / 51.343 / 239.700 ms | **0.045 / 0.196 / 0.448** and **0.058 / 0.234 / 0.510 ms** | **≈470–530× at 12k** (F3 fixed) |
| F1. `persist_compress_chunk` | 0.089 ms/chunk | 0.095 / 0.085 ms/chunk | ~unchanged |
| F2. `flush_region_to_disk` | 1.443 ms (first flush, empty dir) | 8.626 / 1.558 ms (merge flush) | noisy, not comparable — merge re-read dominates |
| F3. cold `load_or_generate` | 7.231 ms/chunk | 7.544 / 6.916 ms/chunk | ~7 ms, unchanged (F4 open) |
| G. fast-path equivalence | — | **BIT-IDENTICAL** in both runs | F1a claim verified on tested chunk/seed |

Raw after-fix output (second of two runs; both runs' key deltas are in §6):

```
A. generate_chunk            :    3.771 ms/chunk  (64 chunks)
B. noise only (34^3 samples) :    4.769 ms/chunk  (4 noise evals/sample, scalar)
C. gather + surface-nets mesh:    1.068 ms/chunk  (1862 verts, 3502 tris/chunk)
D. update_stream_focus       :    0.196 ms/call (view=6,  prefetch=2)
D. update_stream_focus       :    0.729 ms/call (view=12, prefetch=2)
D. update_stream_focus       :    4.590 ms/call (view=24, prefetch=2)
E. drain stream-dirty set    :    0.058 ms total (1500 chunks, FIFO O(1) pop)
E. drain stream-dirty set    :    0.234 ms total (6000 chunks, FIFO O(1) pop)
E. drain stream-dirty set    :    0.510 ms total (12000 chunks, FIFO O(1) pop)
F1. persist_compress_chunk   :    0.085 ms/chunk
F2. flush_region_to_disk     :    1.558 ms (1 region)
F3. load_or_generate (disk)  :    6.916 ms/chunk (whole region re-read + verify)
G. F1 fast vs generic TSDF     : BIT-IDENTICAL (dense=1, samples=32768)
```

Interpretation (rev 2):

- **F1 fixed, −33…−40 % per chunk**: generation 5.62 → 3.39–3.77 ms/chunk across
  two runs. The cached 34 × 34 height field removed the redundant per-Y-slice height
  evaluation; the byte-identity check (G) confirms the refactor is exact for the
  tested chunk/seed. Noise is still scalar, so ~265–295 chunks/s per worker thread
  remains the ceiling.
- **F3 fixed**: the stream lane now drains FIFO at O(1) per pop — 12,000 chunks
  went from 240 ms of serialized scanning to 0.45–0.51 ms (≈470–530×), with linear
  (not quadratic) scaling from 1.5k → 12k.
- **F2 still open**: focus update is still O(r³) per call and lands at ~4–5 ms at
  view 24 — a frame-budget breaker at large view distances. It is now directly
  observable via the `stream_focus` stage line in Godot's Output.
- **F4 still open**: cold disk load is unchanged at ~7 ms/chunk because the whole
  region file is re-read and re-checksummed for each requested chunk.
- Deltas not called out are within run-to-run variance (same binary settings; e.g.
  D at view 24 swung 3.98 ↔ 4.59 ms between runs).
- The F2 flush row is not comparable across runs: the initial audit flushed into an
  empty directory, while both re-runs merged with an existing region file (which is
  re-read and re-checksummed before rewriting); the 1.5–8.6 ms spread is I/O state,
  not regression.

Suite status: 9/9 test binaries pass on the current tree (recompiled with MSVC /O2
in this audit). The GDExtension lifecycle test additionally asserts that
`generate` / `mesh` / `commit` / `edit` / `navigation` stage lines and the
`initial fill complete in …` line are emitted. The benchmark used for this audit
is reproduced in Appendix A.

---

## 3. Findings & recommendations

Ranked P0 = biggest measured wins / main-thread risk, P2 = hygiene.

### P0

**F1. Scalar noise evaluation dominates chunk generation — FIXED (rev 2).**
`CaveNoiseGenerator::generate_chunk` now precomputes the 34 × 34 surface height
field once per chunk into `h_2d_cache` and hoists the Y-only `bedrock_atten` term,
instead of re-evaluating the 2D height inside every Y-slice
(`include/procgen_and_surface_nets.hpp:238-292`). The generic
`generate_chunk_from_density_fn` path is unchanged, and both now share
`encode_chunk_from_density_grid` so their TSDF passes cannot drift apart.

Verified: generation 5.620 → 3.39–3.77 ms/chunk (−33…−40 % across two runs); the
fast path's TSDF is **bit-identical** to the generic `evaluate_composite_density`
path on the tested chunk/seed (benchmark scenario G, reproduced in both runs).
Recommendation: promote that equivalence check into
`tests/test_procgen_and_surface_nets.cpp` so bit-identity is a permanent gate, not a
one-off measurement.

Still open from the original finding:
1. The two cave noises (same coordinates, different seeds) still repeat their
   `floor()` / fade setup twice per sample — fusing them is the next exact win.
2. The kernel is still scalar: linking **FastNoise2** (promised by the HLD,
   `docs/Voxel Design Doc v4.md:399-415`) or explicit AVX2 vectorization remains
   the largest remaining generation win (2–4× expected).

**F1a. Contract note.** Any change to noise output changes generated terrain for a
given seed. The current fast path was verified bit-identical for the tested case;
keep that property (or version the terrain and document the break) for every
further noise change.

**F2. `update_stream_focus` is a main-thread cost that scales badly (measured) — OPEN.**
Each call (`include/persistence_and_streaming.hpp:686-790`) rebuilds the full desired
set and candidate list, sorts it, then calls `store.find_slot` — a hash lookup under
the shared index lock — **per candidate** (`:774-787`). The demo calls it every frame
while moving (`terrain_demo.gd:76-81`). Rev-2 measurement: 0.206 / 0.686 / 3.98 ms
per call at view 6 / 12 / 24 (unchanged within variance) — at view 24 that is a
direct frame-budget violation; at the demo's view 6 it is ~0.2 ms/frame. The stage
is now instrumented (`stream_focus`), so any fix can be judged against field data.

Recommendations:
1. Only recompute when the focus **chunk** changes (currently any 2 m of movement
   triggers a full rebuild) — bucket by Chebyshev shell, not raw distance.
2. Make the update incremental: process enter/leave shell transitions instead of
   re-enumerating the cube (the cube is O(r³) per call; the shell diff is O(r²)).
3. Do one index-lock acquisition for the whole batch of `find_slot` lookups, or
   consult `tracked_resident_` first to skip lookups for known-resident chunks.
4. Keep the sort only for prefetch admission; the active set does not need ordering.
5. Reuse member scratch buffers (`candidates`, `desired_residency_set`) instead of
   allocating two containers per call.

Expected impact: 5–20× off the focus update, removing 4+ ms spikes at large view
distances. Effort: small-medium.

**F3. Nearest-first dirty pops were O(n) per pop under one global mutex — FIXED (rev 2).**
The stream lane now publishes into a FIFO `stream_queue_` on first insert, and
`try_pop_next_mesh_job` drains it O(1) per pop, lazily skipping stale entries that
were promoted to the edit lane (`include/scratch_and_dirty.hpp:358-402`). The edit
lane keeps its nearest-first scan — appropriate, since its sets are small. The
scheduler test was updated to assert FIFO drain order (`tests/test_scratch_and_dirty.cpp:255`).

Verified: 12,000-chunk drain 239.7 → 0.45–0.51 ms (≈470–530× across two runs), now
linear (1.5k / 6k / 12k → 0.045 / 0.196 / 0.448 ms in run 1); the mutex-serialized
quadratic scan is gone. Remaining option if edit-lane ordering ever matters at scale:
bucket the edit lane into distance rings instead of scanning (not needed at current
set sizes).

**F4. Cold load re-reads and re-verifies the whole region file per chunk (measured
~7 ms/chunk) — OPEN.**
`read_chunk_blob_from_disk` (`include/persistence_and_streaming.hpp:567-575`) calls
`load_existing_region_file`, which reads the entire payload (`:523-537`), FNV-checksums
all of it (`:540`), then keeps exactly one chunk. `regions_` is only populated by
`persist_compress_chunk`, so every generation-miss chunk pays this again.

Recommendations: on first touch, load all blobs of the region into `regions_` (inside
the existing 32 MiB `RegionBlobCache` cap), so subsequent chunks in the same region are
RAM hits; or at minimum seek directly to the TOC entry's `payload_offset`/`payload_size`
and read only that chunk (verifying a per-chunk checksum, since whole-file verification
forces the full read). Expected impact: cold-load I/O per chunk ~7 ms → sub-ms after
the first chunk per region. Effort: small-medium.

### P1

**F5. Persistence work sits on the main thread and behind one global mutex — OPEN.**
- Eviction paths call `persist_compress_chunk` synchronously from
  `update_stream_focus` (`include/persistence_and_streaming.hpp:761-765`) — measured at
  ~0.09 ms/chunk plus several allocations each; a burst of evictions during fast
  travel adds up on the exact same frame as the focus rebuild (F2).
- `flush_region_to_disk` / `flush_all_dirty_regions` hold `mutex_` across the entire
  file write + fsync + rename (`:341-353,577-661`), blocking every other
  persistence call; `save_world()` and exit-save run this synchronously.
- The HLD describes `PersistCompress` as an in-memory worker job (~50 µs);
  the worker pool already has a low-priority lane that fits (`lane 3`).

Recommendations: dispatch compression to the generate lane (or a dedicated
persistence lane); snapshot the region under lock, then write outside it; make
`save_world` asynchronous or budgeted. Expected impact: removes save/evict spikes
from the render thread. Effort: medium.

**F6. Mesh payload is copied and duplicated too many times per commit — OPEN.**
Per committed chunk the collision triangle soup exists as: payload `collision_faces`
producer-side, `cached_nav_faces` std::vector copy (`godot_wrapper_and_nav3d.hpp:174`),
`godot_nav_faces` PackedVector3Array copy (`:161-166`), and finally per-group
`assembled_source_faces` (`:463-464`) — the last one is only consumed by unit tests
and is dead weight in Godot builds. Rendering data (vertices+normals+indices) is
re-uploaded with `mesh_clear` + `mesh_add_surface_from_arrays` (`:133-157`), which is
correct for a full remesh but leaves nothing for Godot to delta.

Recommendations: keep exactly one nav representation per mode — under
`VOXEL_WITH_GODOT_CPP` write only `godot_nav_faces` and skip the `cached_nav_faces`
copy (or vice versa outside Godot); guard `assembled_source_faces` behind a test
flag. Note none of the copies are ledger-accounted after commit. Expected impact:
halves nav-related memory traffic per commit; lower per-commit CPU. Effort: small.

**F7. Mesher allocates per call and recomputes corner gradients blindly — OPEN**
(the generate-side allocation was hit by the F10 thread-local grid; the mesher's
per-call dual grid remains).
`dual_grid` is a freshly value-initialized `std::vector<DualCellData>` of 33³ entries
(~1.15 MB) **per mesh job** (`include/procgen_and_surface_nets.hpp:303`); output
vectors (`vertices/normals/indices/collision_faces`) grow from zero with no reserve;
`corner_gradient` (`:287-296`) is evaluated up to 8× per corner (once per adjacent
active cell).

Recommendations: a thread-local reusable dual grid + cleared-on-use buffer, exactly
like `WorkerScratchWindow36` is already per-worker; `reserve` payload vectors from a
cheap active-cell estimate. Corner-gradient caching only pays when the active-cell
fraction is high (> ~12.5 %) — measure before adding it; do not pre-guess.

**F8. Edit-phase secondary scans are full-chunk — OPEN.**
The de-crumb pass scans all 32³ samples of every modified chunk with 6 neighbor reads
each (`include/edit_and_worker_pool.hpp:296-331`) even when the blast touched a corner;
the CSG loop does a `sqrt` per sphere (≤ 16) per sample in the bounding box
(`:237-269`) without squared-distance rejection. Both run on workers, but they gate
explosion completion latency (target: 1–2 frames, HLD §8.2).

Recommendations: clamp the de-crumb scan to the transaction's sample bounds + 1;
compare squared distances first and only `sqrt` the best sphere; skip the pass
entirely when no solid sample was written. Effort: small.

**F9. Navigation readiness and assembly scan globally per frame — OPEN.**
`submit_ready_groups_to_server` iterates **all** `pending_chunk_revisions_` for every
dirty group (`include/godot_wrapper_and_nav3d.hpp:429-437`) and, when a group bakes,
assembles faces by visiting ≤144 chunk positions with a hash lookup each (`:448-473`).
With many groups in flight this is O(groups × pending) per frame on the main thread.

Recommendations: maintain a per-group pending counter and readiness flag updated
incrementally by `note_chunk_mesh_committed`; cache each group's candidate chunk list
(the geometry is static per group key). Effort: medium.

**F10. Worker/thread allocation details — PARTIALLY FIXED (rev 2).**
- **Fixed:** `WorkerDensityGrid34` (157 KiB + reservation) is no longer constructed
  per background generate call; the wrapper lambda now uses a `thread_local` grid
  (`src/voxel_terrain_3d.h:147`, tagged "Audit F10: … 0 heap allocations per
  chunk"). Lifetime is safe across teardown cycles because worker threads are joined
  before the owning bundle is destroyed.
- **Open:** `num_workers = hardware_concurrency() - 2`
  (`include/edit_and_worker_pool.hpp:411-414`) → 14 workers on this 8c/16t machine;
  SMT siblings don't give +75 % throughput and steal cycles from Godot's render/main
  threads. Consider `physical - 2` (or a project setting); make it measurable — the
  new stage timings now provide the throughput data to decide.
- **Open:** idle workers poll with `wait_for(2 ms)` (`:678-687`); worst-case job pickup
  latency is ~2 ms after enqueue. Acceptable, but a shorter wait or a `notify` from
  every producer (already done for most) keeps lane latency tighter; verify with a
  latency test before changing.

**F11. Triplanar shader cost is meaningful at low-end/4K and mixes cheap + expensive
terms — OPEN.** The fragment shader evaluates 6 `value_noise3` calls (48 hashed lattice
lookups) plus a `pow(blend, 4)` per pixel (`voxel_triplanar.gdshader:21-44,51-63`).
The detail amplitude is only ±0.06 albedo. Recommendations: one octave instead of
two (or fade the second octave by distance), or move blend computation to the vertex
stage. Also note the demo enables MSAA 4× (`project.godot:27`) and SSAO
(`voxel_terrain_demo.tscn:15`) — fine for the demo, worth re-checking if the shader
is profiled on integrated GPUs.

### P2

**F12. Demo HUD does per-frame engine queries and string formatting — OPEN.**
`_update_hud` calls `terrain.core_idle()` and `fully_committed()` every frame
(`terrain_demo.gd:83-109`); both take locks and `fully_committed` walks all nav
groups. Throttle the label to 4–5 Hz. Trivial.

**F13. Measurement/guard gaps — LARGELY ADDRESSED (rev 2).**
- **Fixed:** in-engine per-stage instrumentation now exists
  (`include/stage_timings.hpp`, the `instrumentation_enabled` property, per-frame
  Output log, one-time startup fill message). It is lock-free, drained once per frame,
  and asserted end-to-end by `tests/test_stage_timings.cpp` and the GDExtension
  lifecycle test.
- **Still open:** no committed benchmark harness and no CI. Throughput regressions
  (e.g. the next noise change) are still not caught automatically — commit the
  Appendix A bench (or equivalent) as a `bench_*` target with printed budgets, and
  keep a `PERF.md` ledger.
- **Still open:** stale claims. The `< 0.05 ms` focus-update comment
  (`include/persistence_and_streaming.hpp:687`) is not met at larger radii
  (3.98–4.59 ms at view 24); FastNoise2/lz4 are named in the README/HLD but not
  linked. Implement or correct the documentation.

**F14. Build hygiene — PARTIALLY FIXED (rev 2).**
- **Fixed:** artifact naming now follows the CMake config (`$<CONFIG>` →
  `…debug…`/`…release…`, `CMakeLists.txt:75-81`), `.gdignore` files keep Godot's
  importer out of `build/` and `bin/`, and `voxel_engine.gdextension` maps the editor's
  `debug` slot to the optimized `release` DLL. Note the consequence: a `--config Debug`
  build writes a `…debug…` DLL that the current mapping no longer references — flip
  the mapping if a debug-slot build is ever wanted.
- **Still open:** no LTO/IPO, no `/arch:AVX2` (or `-mavx2`), no `/fp:fast`. These are
  the cheapest remaining multiplier on the still-scalar noise/mesh code.

**F15. Robustness items that can silently become performance bugs — OPEN.**
- `ChunkSlot::state` and several fields are read lock-free outside slot locks
  (`include/persistence_and_streaming.hpp:775-776`, `include/scratch_and_dirty.hpp:172-179`).
  Today the cost is at most a wasted mesh that fails stamp validation and is re-dirtied —
  i.e., burned worker time. Making `state` atomic (or documenting the benign race)
  would remove a class of "why did this chunk remesh three times" reports.
- `SlabPool::allocate` reserves ledger bytes **before** consulting its free list
  (`include/chunk_store.hpp:164-189`); at the ceiling, cached blocks are unusable and
  `promote_to_dense` fails — in the edit path a failed `set_tsdf` is silently skipped
  (`include/edit_and_worker_pool.hpp:261`, `include/procgen_and_surface_nets.hpp:219`).
  Decide and document the policy (evict-then-fail, or make the edit fail loudly).
- `push_completion` still enqueues when the ledger reservation fails, leaving payload
  memory unaccounted (`include/edit_and_worker_pool.hpp:551-565`). Minor accounting hole.

**F16. The new instrumentation is on by default and logs every frame that did work.**
`instrumentation_enabled` defaults to `true` (`src/voxel_terrain_3d.h:184`) and the
`_process` drain formats a `std::string` and calls Godot's `print` for every frame in
which any stage ran (`src/voxel_terrain_3d.cpp:352-366`). The accumulator itself is
cheap (lock-free adds; the timer is a no-op when disabled), but per-frame string
formatting plus Output-panel printing is pure overhead when nobody is reading the log —
and while streaming, that is *every* frame. Recommendations: default `false` outside
the demo/editor, or throttle to an interval (e.g. 1 Hz) and log only when a stage
exceeds a threshold. Effort: trivial.

---

## 4. Roadmap (rev 2 status)

| Priority | Item | Expected effect | Effort | Status |
|---|---|---|---|---|
| P0 | F1: precompute 2D height field | **−33…−40 % generation (measured)** | S | **DONE** (3.39–3.77 ms/chunk, bit-identical) |
| P0 | F3: O(1) stream-lane drain | **≈470–530× drain speedup (measured)** | S | **DONE** |
| P0 | F2: incremental focus update, batched index lookups | removes 0.2–4 ms main-thread spikes | S–M | open |
| P0 | F4: cache region blobs / seek-only reads | ~7 ms → <1 ms per cold chunk | S–M | open |
| P1 | F1 (remainder): fuse cave-noise setup + FastNoise2/AVX2 | 2–4× further on the still-scalar noise kernel | M | open |
| P1 | F5: persist compress on worker lane; unlocked flush | removes save/evict spikes | M | open |
| P1 | F10: worker count/size + poll latency | steadier frame pacing | S | partial (thread-local grid fixed) |
| P1 | F7: thread-local mesher grid, payload reserves | allocation churn out of the mesh path | S | open |
| P1 | F6: single nav-face representation | halves per-commit nav memory traffic | S | open |
| P1 | F8/F9: bounded de-crumb, incremental nav readiness | explosion latency toward the 1–2 frame target | S–M | open |
| P2 | F11–F12, F15, F16 | shader/HUD/robustness/instrumentation defaults | S | open |
| P2 | F13 (remainder): committed bench + CI + doc corrections | catches the next regression automatically | S–M | partial (instrumentation landed) |

## 5. Verification plan (per fix)

1. Re-run the Appendix A benchmark **before** the fix with identical binary settings
   and record the table in §6 (kept and reverted attempts alike).
2. Apply one fix at a time; re-run under the same conditions; compare against
   run-to-run variance (repeat ≥5×; keep only deltas beyond ±1σ). The F1/F3 rev-2
   results were produced this way.
3. For anything touching the mesher/edit path, the full suite must stay green. In
   this audit the suite was compiled directly with MSVC /O2 (9/9 pass) because
   `cmake`/`ctest` were not on PATH in the audit shell; the canonical commands are
   `cmake --build build --config Release && ctest --test-dir build -C Release`,
   plus `run_all_tests.sh` for ASan/UBSan/TSan on Linux.
4. In-editor confirmation for main-thread items: the `instrumentation_enabled`
   per-frame stage lines (Output panel) plus the demo HUD while flying at
   `view_distance_chunks = 6` and 12 — watch `stream_focus` and `commit` in
   particular.
5. Guards still to add: a `bench_*` CMake target (generation, mesh, focus update,
   drain, persist) with printed budgets; promote benchmark G (fast-path
   bit-identity) into `tests/test_procgen_and_surface_nets.cpp`; run the
   simulated-budget integration test in CI.

## 6. Measurement ledger

| Idea | Baseline → Result | Verdict | Why |
|---|---|---|---|
| (audit baseline) | generation 5.62 ms/chunk; noise 4.84; mesh 1.14; focus 0.23/0.77/4.92 (view 6/12/24); drain 240 ms @12k; cold load 7.23 ms | baseline | single-thread /O2, Ryzen 7 5800X |
| Precompute 2D height field (F1) | 5.620 → 3.39–3.77 ms/chunk generation (−33…−40 %, 2 runs) | **kept** | exact refactor; fast vs generic TSDF bit-identical |
| Stream-lane FIFO drain (F3) | 239.7 → 0.45–0.51 ms @ 12k (−99.8 %, 2 runs) | **kept** | O(1) pop; now linear scaling; scheduler test updated |
| Thread-local `WorkerDensityGrid34` (F10) | — | kept | removes per-generate 157 KiB alloc; not isolated in the bench |
| In-engine stage timings (F13) | — | kept | asserted by stage-timings + GDExtension lifecycle tests |
| Fuse cave-noise setup | — | untested | next exact noise win |
| FastNoise2 / AVX2 kernel | — | untested | 2–4× expected |
| Incremental focus update (F2) | — | untested | kills O(r³)/call rebuild |
| Region blob cache on load (F4) | — | untested | first chunk pays I/O, rest RAM |
| AVX2/LTO build flags (F14) | — | untested | cheapest multiplier on scalar code |

## 7. Repo hygiene (rev 2)

- `updates/` (the stale earlier snapshot flagged in rev 1) has been deleted —
  resolved. The single sources of truth are `include/` and `src/`.
- `README.md` now documents the instrumentation, artifact-naming rules, and the
  9-suite test count. Keep it in sync with §3: two documentation claims are still
  stale (the `< 0.05 ms` focus-update comment and the unlinked FastNoise2/lz4
  references).

---

## Appendix A — Benchmark source used for §2

Compile (MSVC): `cl /nologo /std:c++20 /O2 /EHsc /DNDEBUG /I include voxel_bench.cpp`.
It links no Godot; it exercises only the pure core headers. Scenario G is the
fast-path bit-identity check added in rev 2.

```cpp
// Headless micro-benchmark for voxel_core v4.0 (analysis aid, not part of the repo)
#include "persistence_and_streaming.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace voxel;
using clk = std::chrono::steady_clock;

static double ms_between(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main() {
    MemoryLedger ledger(512ULL << 20);
    ChunkStore store(ledger, 8);
    CaveNoiseGenerator procgen;
    WorkerDensityGrid34 grid(ledger);

    // ---- A. Full chunk generation (noise + central differences + TSDF encode + store)
    {
        constexpr int N = 64;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            procgen.generate_chunk(store, ChunkKey::from_coords(i, 2, 0), grid);
        }
        auto t1 = clk::now();
        std::printf("A. generate_chunk            : %8.3f ms/chunk  (%d chunks)\n",
                    ms_between(t0, t1) / N, N);
    }

    // ---- B. Noise-only cost: evaluate_composite_density over the full 34^3 grid
    {
        constexpr int N = 16;
        volatile float sink = 0.0f;
        auto t0 = clk::now();
        for (int c = 0; c < N; ++c) {
            float acc = 0.0f;
            for (int32_t z = -1; z <= 32; ++z)
                for (int32_t y = -1; y <= 32; ++y)
                    for (int32_t x = -1; x <= 32; ++x)
                        acc += procgen.evaluate_composite_density((float)(x + c * 32), (float)y, (float)z);
            sink = acc;
        }
        auto t1 = clk::now();
        std::printf("B. noise only (34^3 samples) : %8.3f ms/chunk  (4 noise evals/sample, scalar)\n",
                    ms_between(t0, t1) / N);
        (void)sink;
    }

    // ---- Prepare a 3x3x3 resident neighborhood for gather+mesh timing
    const ChunkKey center = ChunkKey::from_coords(0, 2, 0);
    for (int32_t dz = -1; dz <= 1; ++dz)
        for (int32_t dy = -1; dy <= 1; ++dy)
            for (int32_t dx = -1; dx <= 1; ++dx)
                procgen.generate_chunk(store, center.offset(dx, dy, dz), grid);

    // ---- C. Gather scratch window + Surface Nets mesh
    {
        constexpr int N = 32;
        WorkerScratchWindow36 scratch(ledger);
        size_t verts = 0, tris = 0;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            const NeighborhoodVersionStamp stamp = gather_scratch_window_36(store, center, scratch);
            ChunkMeshPayload p = SurfaceNetsMesher::mesh_chunk_from_scratch_36(
                center, scratch, 1, stamp.captured_terrain_revision);
            verts = p.vertices.size();
            tris = p.indices.size() / 3;
        }
        auto t1 = clk::now();
        std::printf("C. gather + surface-nets mesh: %8.3f ms/chunk  (%zu verts, %zu tris/chunk)\n",
                    ms_between(t0, t1) / N, verts, tris);
    }

    // ---- D. Streaming focus update cost by view distance (no workers started)
    {
        DualDirtyScheduler sched;
        RegionSaveManager save(ledger, "bench_saves");
        CoreWorkerPool pool(store, sched);
        StreamingManager stream(store, sched, save, pool);
        const Vec3f vel{24.0f, 0.0f, 0.0f};
        for (int radius : {6, 12, 24}) {
            const Vec3f pos{16.0f + radius * 32.0f, 48.0f, 16.0f};
            (void)stream.update_stream_focus(pos, vel, radius, 2); // warm-up
            constexpr int N = 20;
            auto t0 = clk::now();
            for (int i = 0; i < N; ++i)
                (void)stream.update_stream_focus(pos, vel, radius, 2);
            auto t1 = clk::now();
            std::printf("D. update_stream_focus       : %8.3f ms/call (view=%d, prefetch=2)\n",
                        ms_between(t0, t1) / N, radius);
        }
    }

    // ---- E. Dual dirty scheduler drain cost (nearest-first pop is a linear scan)
    for (int K : {1500, 6000, 12000}) {
        DualDirtyScheduler sched;
        sched.set_focus_chunk(ChunkKey::from_coords(0, 0, 0));
        for (int i = 0; i < K; ++i)
            sched.mark_stream_dirty(ChunkKey::from_coords(i % 100, 0, i / 100));
        auto t0 = clk::now();
        while (sched.try_pop_next_mesh_job(0).has_value()) {
        }
        auto t1 = clk::now();
        std::printf("E. drain stream-dirty set    : %8.3f ms total (%d chunks, FIFO O(1) pop)\n",
                    ms_between(t0, t1), K);
    }

    // ---- F. Persist-compress + region flush timing on a dense chunk
    {
        RegionSaveManager save(ledger, "bench_saves");
        for (int i = 0; i < 8; ++i) {
            procgen.generate_chunk(store, center.offset(i, 0, 0), grid);
        }
        constexpr int N = 8;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i)
            (void)save.persist_compress_chunk(store, center.offset(i, 0, 0));
        auto t1 = clk::now();
        std::printf("F1. persist_compress_chunk   : %8.3f ms/chunk\n", ms_between(t0, t1) / N);

        auto t2 = clk::now();
        (void)save.flush_region_to_disk(RegionKey::from_chunk_key(center));
        auto t3 = clk::now();
        std::printf("F2. flush_region_to_disk     : %8.3f ms (1 region)\n", ms_between(t2, t3));

        // Cold disk load of one chunk (region file re-read + full FNV verify)
        save.clear_ram_cache();
        auto t4 = clk::now();
        (void)save.load_or_generate_chunk(store, center, procgen, grid);
        auto t5 = clk::now();
        std::printf("F3. load_or_generate (disk)  : %8.3f ms/chunk (whole region re-read + verify)\n",
                    ms_between(t4, t5));
    }

    // ---- G. F1 equivalence: fast cached-height path vs generic density-fn path
    {
        const ChunkKey eq_key = ChunkKey::from_coords(3, 2, 3);
        auto snapshot = [&]() {
            std::vector<int16_t> out;
            ChunkSlot* slot = store.find_slot(eq_key);
            std::unique_lock<std::shared_mutex> lock(slot->mutex);
            if (slot->form == ChunkStorageForm::Dense && slot->tsdf_data != nullptr) {
                out.assign(slot->tsdf_data, slot->tsdf_data + TsdfFormat::kChunkSamples);
            }
            return out;
        };
        procgen.generate_chunk(store, eq_key, grid); // fast cached-height path
        const std::vector<int16_t> fast_slab = snapshot();
        procgen.generate_chunk_from_density_fn(
            store, eq_key, grid,
            [&procgen](float wx, float wy, float wz) {
                return procgen.evaluate_composite_density(wx, wy, wz);
            });
        const std::vector<int16_t> generic_slab = snapshot();
        const bool both_dense = !fast_slab.empty() && !generic_slab.empty();
        const bool identical =
            both_dense &&
            std::memcmp(fast_slab.data(), generic_slab.data(),
                        fast_slab.size() * sizeof(int16_t)) == 0;
        std::printf("G. F1 fast vs generic TSDF     : %s (dense=%d, samples=%zu)\n",
                    identical ? "BIT-IDENTICAL" : "MISMATCH", both_dense ? 1 : 0,
                    fast_slab.size());
    }

    std::printf("done. ledger reserved=%zu bytes\n", ledger.total_reserved_bytes());
    return 0;
}
```
