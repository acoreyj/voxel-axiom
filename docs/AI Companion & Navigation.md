# AI Companion & 3D Cave Navigation

How the `VoxelAICompanion` node and the dynamic AI pathing pipeline work.

Related documents:

- `docs/Voxel Design Doc v4.md` — Section 4.2 (class mapping), 4.5 (3D cave navigation), 7.2 (integration tests)
- `docs/Voxel Performance Analysis.md` — finding **F9** (navigation submission main-thread scans)
- `README.md` — user-facing feature summary and controls

---

## 1. Mental Model

`VoxelAICompanion` is **not** a movement controller. It is a thin query facade over
Godot's `NavigationServer3D` with two jobs:

1. Ask the world's navigation map for a path between two points (`get_voxel_path`).
2. Report whether the `4 x 2 x 4` navigation group containing a position has finished
   baking (`is_path_region_synced`).

All pathing intelligence lives in `VoxelNavigationManager`
(`include/godot_wrapper_and_nav3d.hpp`), which `VoxelTerrain3D` owns and drives from its
`_process()` loop. The companion does not carry a `NavigationAgent3D` and does not move
itself along the returned waypoints — a gameplay script is expected to consume them.

In one sentence: **the same de-indexed triangle soup that becomes physics collision is
cached per chunk, gathered into `4 x 2 x 4`-chunk navigation groups, baked by Recast
asynchronously, and swapped into `NavigationServer3D` regions; a group reports "synced"
only after an edit invalidated it and its async rebake has landed on the server.**

---

## 2. Node Reference: `VoxelAICompanion`

The class is registered at SCENE initialization level alongside `VoxelTerrain3D`
(`src/register_types.cpp:12-18`), so both appear in the Godot Node creation dialog.
Definition: `src/voxel_ai_companion.h`.

| Method | GDScript-visible | Behavior |
| --- | --- | --- |
| `PackedVector3Array get_voxel_path(Vector3 start, Vector3 end)` | yes | Calls `NavigationServer3D::map_get_path(world_nav_map, start, end, optimize=true)`. Returns waypoints, or an empty array when no baked region covers the endpoints. |
| `bool is_path_region_synced(Vector3 position)` | yes | True when the `4 x 2 x 4` navigation group containing `position` has `synced_* == desired_*` with no bake in flight. Returns `true` when no terrain node is linked. |
| `void set_terrain_node(const VoxelTerrain3D*)` | **no** | C++-only link used for the sync check and the `navigation` stage timing. See the gotcha in Section 8. |

### `get_voxel_path`

`src/voxel_ai_companion.cpp:17-42`:

- Uses the navigation map of the companion's own `World3D`
  (`get_world_3d()->get_navigation_map()`) — the same map `VoxelTerrain3D` attaches its
  regions to at `_enter_tree` (`src/voxel_terrain_3d.cpp:247-264`).
- Records its wall-clock duration via `VoxelTerrain3D::record_navigation_query_us()` when
  a terrain node is linked, which surfaces as the `navigation` line in the instrumentation
  summary.
- In non-Godot builds (offline unit tests) there is no `NavigationServer3D`; the method
  returns a synthetic 3-point `[start, midpoint, end]` path when both endpoints are
  synced. The binding test asserts `path.size() == 3` against this fallback, not against
  real Recast output.

### `is_path_region_synced`

`src/voxel_ai_companion.cpp:44-50`: delegates to
`terrain_node_->get_navigation_manager().is_path_region_synced(position)`, which maps the
world position through `floor(position / voxel_scale)` → chunk → navigation group and
checks the group's sync state (Section 6). With no terrain node linked it unconditionally
returns `true`.

### Typical query pattern

From `terrain_demo.gd:127-133` (right-click in the demo scene):

```gdscript
func _query_companion_cave_path() -> void:
    var target_pos := camera.global_position
    if not ai_companion.is_path_region_synced(target_pos):
        print("[Nav3D] cave navigation group still baking async...")
        return
    var path: PackedVector3Array = ai_companion.get_voxel_path(
        ai_companion.global_position, target_pos)
    print("[Nav3D] Companion 3D cave path waypoints: ", path.size())
```

Check sync first, then query; otherwise the path may silently come back empty. In
`voxel_terrain_demo.tscn` the companion is placed at `(16, 48, 16)` as a sibling of
`VoxelTerrain3D`.

---

## 3. Navigation Group Geometry

A chunk is `32^3` voxel cells. A navigation group is **`4 x 2 x 4` chunks =
`128 x 64 x 128` cells (32 chunks)**, keyed by floor division
(`include/edit_and_worker_pool.hpp:27-42`):

```cpp
struct NavigationGroupKey {
    int32_t gx; // ChunkKey::floor_div(Cx, 4)
    int32_t gy; // ChunkKey::floor_div(Cy, 2)
    int32_t gz; // ChunkKey::floor_div(Cz, 4)
};
```

### Why 3D groups

Full-height `4 x 4` X/Z columns (the v3.0 design) mean an underground explosion
invalidates the whole vertical column up to the sky: surface terrain and every cave layer
above the blast are re-voxelized by Recast. With `2`-chunk-tall groups, a blast at
`Cy = 1` (`gy = 0`) rebakes only the local volume; the surface group at
`gy = 2` records zero rebuilds. This isolation is asserted by
`tests/test_integration_m3_m4.cpp:133-216`.

```
gy = 2   Cy 4-5   ─────────────── surface group (untouched by a gy=0 blast)
gy = 1   Cy 2-3   ───────────────
gy = 0   Cy 0-1   ─────────────── cave group (rebakes)
            4 chunks wide (128 cells) x 4 deep
```

### Owned vs. baking AABB

`VoxelNavigationManager::compute_owned_aabb()` / `compute_baking_aabb()`
(`include/godot_wrapper_and_nav3d.hpp:413-433`):

- **Owned AABB** — the group's authoritative volume:
  `[128*g . s, 128*(g+1) . s]` (etc. per axis; `s = voxel_scale`).
- **Baking AABB** — owned AABB expanded by padding:
  - Horizontal: `P_xz = agent_radius + region_connection_margin + 2*s`
    = `0.6 + 0.4 + 2.0` = **3.0 m**
  - Vertical: `P_y = agent_height + agent_max_climb + 2*s`
    = `1.8 + 0.5 + 2.0` = **4.3 m**

Source triangles are gathered from every resident chunk whose AABB intersects the baking
AABB — the 32 owned chunks plus a one-chunk halo where the padding reaches into a
neighbour. `NavigationMesh::set_border_size(P_xz)` then clips the final navigable
polygons back to the owned AABB after Recast erosion, so adjacent groups meet
edge-to-edge and `NavigationServer3D` can stitch horizontal and sloped vertical seams.
The halo-path test (`tests/test_integration_m3_m4.cpp:221-283`) carves a ramp across
`Cy = 1 / Cy = 2` and verifies both the `gy = 0` and `gy = 1` groups gather all six ramp
vertices across the `y = 64` seam thanks to `P_y`.

---

## 4. End-to-End Lifecycle

`VoxelNavigationManager` state per group (`NavigationGroupState`,
`include/godot_wrapper_and_nav3d.hpp:351-378`):

| Field | Meaning |
| --- | --- |
| `desired_terrain_revision` / `desired_source_epoch` | Latest invalidation requested by an edit / geometry change. |
| `submitted_terrain_revision` / `submitted_source_epoch` | Values snapshotted when the async bake was handed to NavigationServer. |
| `synced_terrain_revision` / `synced_source_epoch` | Values currently live on the server. `is_group_synced` compares these to `desired_*`. |
| `rebuild_in_flight` | A bake has been submitted and not yet swapped into the region. |
| `bake_callback_received` / `pending_callback_map_iteration` | Completion-tracking state (Section 5). |
| `total_bakes_submitted` | Lifetime counter; used by the isolation test. |

### 4.1 Initialization

`VoxelTerrain3D::_enter_tree()` (`src/voxel_terrain_3d.cpp:220-265`) reads the scenario,
physics space, and navigation map RIDs from `World3D` and calls
`navigation_manager.initialize(nav_map_rid)`. In Godot builds it also calls
`set_bake_on_stream_enabled(false)` — see the gotcha in Section 8.

### 4.2 Invalidation (what makes a group dirty)

Three sources, all processed on the main thread in `VoxelTerrain3D::_process()`:

1. **Chunk mesh committed** — `note_chunk_mesh_committed()`
   (`godot_wrapper_and_nav3d.hpp:435-464`). Records the chunk's committed
   `captured_terrain_revision`, adds the chunk to `waiting_chunks_`, then walks the
   owner group plus its `-1..+1` neighbours (27 candidates) and, for every group whose
   baking AABB intersects the chunk AABB, increments `desired_source_epoch` and raises
   `desired_terrain_revision` to the chunk's revision. (With bake-on-stream disabled,
   revision-0 commits stop after the bookkeeping and invalidate nothing.)
2. **Chunk unloaded** — `note_chunk_unloaded()`
   (`godot_wrapper_and_nav3d.hpp:466-478`). Drops the chunk from the revision /
   requirement / waiting maps; with bake-on-stream enabled, bumps the owner group's
   `desired_source_epoch`.
3. **Edit transaction completed** — `apply_edit_transaction()`
   (`godot_wrapper_and_nav3d.hpp:480-489`). For every dirtied chunk, records a
   *required* revision floor (`required_chunk_revisions_`); for every
   `affected_nav_groups` entry, raises `desired_terrain_revision`. This call is also what
   **creates** group state via `get_or_create_group()` (regions, `NavigationMesh`
   resources, and source-geometry data are instantiated there:
   `godot_wrapper_and_nav3d.hpp:686-724`).

### 4.3 Submission (async bake)

`VoxelTerrain3D::_process()` only calls
`navigation_manager.submit_ready_groups_to_server()` when `core_idle()` is true
(`src/voxel_terrain_3d.cpp:480-483`), so bakes never compete with background streaming
work. Inside the manager (`godot_wrapper_and_nav3d.hpp:491-582`):

1. A group is skipped if `rebuild_in_flight` or if it is clean
   (`desired_* == synced_*` for both revision and epoch).
2. **Readiness gate:** every chunk in `required_chunk_revisions_` that belongs to this
   group must have a committed revision at or above its requirement. This is what stops
   a group from baking with stale geometry while the edit that dirtied it is still being
   remeshed.
3. **Assembly:** candidate chunks are the owned `4 x 2 x 4` block plus a one-chunk halo
   (`gx*4 - 1 .. (gx+1)*4`, `gy*2 - 1 .. (gy+1)*2`, `gz*4 - 1 .. (gz+1)*4`; at most
   `6 x 4 x 6 = 144` positions). For each resident slot that intersects the baking AABB,
   its cached nav faces are appended to the pre-allocated
   `NavigationMeshSourceGeometryData3D` with `add_faces(faces, Transform3D())`. Faces are
   read from `ChunkServerSlot::godot_nav_faces` regardless of whether the chunk's physics
   collision is enabled (Section 8.5).
4. `submitted_* = desired_*`, `rebuild_in_flight = true`, then:

```cpp
NavigationServer3D::get_singleton()->bake_from_source_geometry_data_async(
    g.nav_mesh, g.source_data, Callable());   // empty callback
g.bake_callback_received = true;
g.pending_callback_map_iteration = ns->map_get_iteration_id(nav_map_rid_);
```

In live Godot builds **at most one group is submitted per frame**; the offline unit tests
bypass the throttle.

### 4.4 Completion & map synchronization

There is no completion `Callable` in the implementation. Instead,
`poll_server_synchronization()` (`godot_wrapper_and_nav3d.hpp:595-628`) runs at the top
of every `VoxelTerrain3D::_process()` and:

1. Refreshes `map_iteration_id_` from
   `NavigationServer3D::map_get_iteration_id(nav_map_rid_)`.
2. For any in-flight group whose `map_iteration_id_` has advanced beyond the iteration
   recorded at submission, calls
   `region_set_navigation_mesh(region_rid, nav_mesh)` and promotes
   `synced_* = submitted_*`, clearing `rebuild_in_flight`. This introduces a
   **minimum one server sync tick** between submission and sync (Section 8.4).
3. Moves chunks out of `waiting_chunks_` into `newly_committed_chunks_` once their owning
   group is synced (a group that does not exist counts as synced).

`VoxelTerrain3D::_process()` drains `newly_committed_chunks_` and emits the
`chunk_committed(chunk_key)` signal for each (`src/voxel_terrain_3d.cpp:485-492`). For
edited regions this is the moment the triangle is mesh-visible, collision-enabled, **and**
pathable.

> Offline tests advance this machinery with
> `complete_in_flight_async_bakes_and_advance_map_iteration()`, which fakes the bake
> callback and advances a synthetic map iteration.

### 4.5 Query

Once synced, `VoxelAICompanion.get_voxel_path()` calls `map_get_path` on the same map the
regions were attached to. Recast's polygons from every synced group are stitched by
`NavigationServer3D` across group edges (including sloped vertical seams, thanks to the
`P_y` halo), so a path can cross multiple groups and layers.

### Sequence overview

```
main thread (VoxelTerrain3D::_process)                 workers / NavigationServer3D
-------------------------------------------------------------------------------
poll_server_synchronization()
  region swap if map iteration advanced  ─────────────▸ Recast baked source_data
  waiting chunks → chunk_committed                       on server worker threads

drain completion queue
  ChunkMesh             → slot commit (visual+collision) + note_chunk_mesh_committed
  ChunkUnloaded         → free slot + note_chunk_unloaded
  EditTransactionDone   → apply_edit_transaction (revision floors + dirty groups)

core_idle() ? submit_ready_groups_to_server()
  ≤1 dirty+ready group: gather cached faces → bake_from_source_geometry_data_async

drain newly committed → emit chunk_committed(chunk_key)
```

---

## 5. Synthesis: the Revision / Epoch Model

The two counters answer two different questions:

- **`terrain_revision`** — "has the *shape* of the voxels changed?" Bumped by edit
  transactions (`EditTransactionCompletedPayload::terrain_revision`). Only edits raise it.
- **`source_epoch`** — "has the *set of contributing chunk geometry* changed?" Bumped
  whenever chunks commit or unload while bake-on-stream is enabled.

A group is **synced** (`is_group_synced`, `godot_wrapper_and_nav3d.hpp:636-645`) when:

1. `!rebuild_in_flight`, and
2. `synced_terrain_revision == desired_terrain_revision`, and
3. `synced_source_epoch == desired_source_epoch`.

A group that has never been created is trivially synced. Aggregate checks:

| Query | Where | Meaning |
| --- | --- | --- |
| `is_path_region_synced(world_pos)` | `VoxelNavigationManager`, surfaced by the companion | Group containing the position is synced. |
| `navigation_groups_synced()` | `VoxelNavigationManager` | Every known group is synced. |
| `fully_committed()` | `VoxelTerrain3D` (`src/voxel_terrain_3d.cpp:379-384`) | `core_idle()` **and** completion queue empty **and** `navigation_groups_synced()`. |

---

## 6. Cave-Tuned NavigationMesh Parameters

`CaveNavMeshConfig` defaults (`include/godot_wrapper_and_nav3d.hpp:316-338`):

| Parameter | Default | Purpose |
| --- | --- | --- |
| `voxel_scale` | `1.0` | World meters per voxel cell. |
| `cell_size` / `cell_height` | `0.25` / `0.20` | Recast voxelization resolution. Overridden in live builds — see below. |
| `agent_radius` | `0.6` | Also feeds horizontal padding. |
| `agent_height` | `1.8` | Also feeds vertical padding. |
| `agent_max_climb` | `0.5` | Ramp/step tolerance; also feeds vertical padding. |
| `agent_max_slope` | `45.0` | Cave floors and sloped blast walls remain walkable. |
| `region_connection_margin` | `0.4` | Feeds horizontal padding. |
| `region_min_size` | `8.0` | Culls tiny unreachable islands Recast creates on blast ledges and cave ceilings. |
| `filter_low_hanging_obstacles` | `true` | Prevents pathing into stalactites/low ceilings. |
| `filter_ledge_spans` | `true` | Prevents walking off blast cliffs. |
| `filter_walkable_low_height_spans` | `true` | Strictly enforces agent-height clearance in tunnels. |

**Live-build divergence:** when a group's `NavigationMesh` is created,
`get_or_create_group()` hard-codes `set_cell_size(0.5)` / `set_cell_height(0.4)` in Godot
builds (`godot_wrapper_and_nav3d.hpp:707-709`) — coarser than the config defaults, to
avoid multi-second Recast stalls on large cave groups. The config fields keep the
`0.25/0.20` values for the offline tests. Border size is set from `P_xz` (3.0 m).

---

## 7. Demo Usage

In `voxel_terrain_demo.tscn`:

- `VoxelAICompanion` is a direct child of the scene root at `(16, 48, 16)`
  (`voxel_terrain_demo.tscn:42`), sibling to `VoxelTerrain3D`.
- Right-click queries a path from the companion's origin to the camera and prints the
  waypoint count (`terrain_demo.gd:127-133`).
- Because nothing calls `set_terrain_node()` in the scene, the live demo runs with the
  companion unlinked — see gotcha 8.2 for the consequences.

### 7.1 Pill companion patrol

Both demo scenes attach `ai_companion_patrol.gd` to the `VoxelAICompanion` node,
which gives it a visible pill capsule and drives it around a triangular patrol
route around spawn. The companion remains a query facade — this script is the
"gameplay that consumes it" that Section 1 anticipates:

- `patrol_points` are **world XZ** points looped in order; their Y is ignored.
- Each frame the node is placed at the route position with
  `y = VoxelTerrain3D.get_surface_height(x, z) + ground_offset`, so the pill
  walks down into fresh craters and over added terrain while the player edits.
- Each waypoint arrival prints `[Pill] waypoint N/M reached at (x, z) (y=…)`.
- No navigation query is involved: the patrol works on a freshly loaded world,
  where `get_voxel_path()` would still return an empty array (gotcha 8.1).

`get_surface_height(x, z)` lives on `VoxelTerrain3D` (not the companion): it casts
the vertical surface ray through the resident TSDF column via
`voxel::raycast_terrain` and falls back to the analytic generator scan in
`voxel::sample_top_surface_y` (`include/procgen_and_surface_nets.hpp`) when the
ray finds no crossing — see gotcha 8.9.

---

## 8. Gotchas, Quirks & Limitations

1. **Bake-on-stream is disabled in Godot builds.**
   `VoxelTerrain3D::_enter_tree()` calls `set_bake_on_stream_enabled(false)`
   (`src/voxel_terrain_3d.cpp:258-260`). In `note_chunk_mesh_committed`, commits with
   `captured_terrain_revision == 0` return after the bookkeeping
   (`godot_wrapper_and_nav3d.hpp:441-443`), so initial streaming **never creates or
   rebakes navigation groups**. Groups appear only when an edit transaction touches them.
   Consequence: on a freshly loaded, unmodified world, `get_voxel_path()` returns an
   empty array — there are no regions on the map yet. Since `note_chunk_unloaded()` also
   early-returns when the toggle is off, streaming unloads never invalidate group
   navmeshes in live builds; only edits do. This is a deliberate stall guard
   ("rebake only on explosions/edits!"), documented only in code.
2. **`set_terrain_node` is C++-only.** It is not bound in `_bind_methods()`, and the
   shipped scene never calls it. With `terrain_node_ == nullptr`:
   - `is_path_region_synced()` always returns `true` (the guard in
     `src/voxel_ai_companion.cpp:45-47`), so the demo's sync check never actually blocks;
   - `get_voxel_path()` never records `navigation` stage timings.
   The end-to-end binding test wires the link manually in C++
   (`tests/test_gdextension_bindings.cpp:122-123`). Binding the setter (or linking from a
   C++ host node) is required for the sync check and timing to be meaningful in a live
   scene.
3. **Throttling and preconditions.** Live builds submit at most one group per frame, and
   only when `core_idle()` is true — i.e., after background streaming has fully settled.
   A large blast spanning many groups becomes pathable over several frames. The offline
   tests bypass both restrictions (no throttle; direct helper calls).
4. **No true completion callback.** The implementation passes an empty `Callable()` to
   `bake_from_source_geometry_data_async()` and marks the "callback received"
   immediately; the region mesh is swapped in only after `map_get_iteration_id()`
   advances past the iteration recorded at submission
   (`godot_wrapper_and_nav3d.hpp:562-577, 595-617`). The design doc (Section 4.5)
   describes a `callable_mp(...)` completion callback; the code approximates it with the
   map-iteration poll. Net effect: a group syncs no earlier than one NavigationServer
   sync tick after submission.
5. **Collision gating is independent of navigation.** `collision_distance_chunks` and
   `collision_subsample_stride` gate the `PhysicsServer3D` concave BVH only. Cached nav
   faces (`godot_nav_faces`) are retained on visual-only commits and are gathered for
   baking regardless of `collision_enabled` (see
   `tests/test_integration_m3_m4.cpp:288-321`).
6. **Offline fallback paths are synthetic.** Non-Godot builds return
   `[start, midpoint, end]`; do not mistake this for real Recast output.
7. **F9 (open): per-frame global scans.** `submit_ready_groups_to_server` iterates all of
   `required_chunk_revisions_` for every dirty group, and each bake walks up to 144
   candidate chunk positions with a hash lookup each — `O(groups x pending)` on the main
   thread. Recommended fix is per-group pending counters and cached candidate lists
   (`docs/Voxel Performance Analysis.md`, finding F9).
8. **`chunk_committed` has strict semantics.** It fires only when the chunk's owning group
   has no work in flight and is fully synced (or when the group does not exist). For
   edited regions this is later than the visual mesh commit, so treat it as the
   "mesh + collision + navigation are consistent" signal.
9. **`get_surface_height` semantics.** Main-thread only (takes a shared slot lock per
   sampled chunk). It returns the top-most air→solid crossing of the resident TSDF
   column — overhangs resolve to the highest crossing — and falls back to the
   procedural generator when the ray finds no crossing. Edited terrain is
   reflected wherever chunks are resident; far from the streaming focus the fallback
   describes the pristine generated surface and ignores edits. It does not depend on
   `collision_distance_chunks`, so it works while physics collision is gated off.

---

## 9. Verification Map

| Behavior | Test |
| --- | --- |
| Registration order (`VoxelTerrain3D` then `VoxelAICompanion` at SCENE level) | `tests/test_gdextension_bindings.cpp:95-99` |
| Stream → explosion → `fully_committed()` + `is_path_region_synced()` at the blast site | `tests/test_gdextension_bindings.cpp:139-170` |
| Path query (`size == 3` on the offline fallback) and `navigation` stage timing | `tests/test_gdextension_bindings.cpp:175-181` |
| Surface-height query: resident plane (68) → edited plane (40) → generator fallback | `tests/test_procgen_and_surface_nets.cpp` (`test_surface_height_query`) |
| `chunk_committed` and `explosion_completed` emission | `tests/test_gdextension_bindings.cpp:183-194` |
| Vertical isolation: cave blast (`gy = 0`) rebakes only its group; surface (`gy = 2`) records zero rebuilds | `tests/test_integration_m3_m4.cpp:133-216` |
| Vertical halo stitching: a ramp across `Cy=1/Cy=2` appears in both groups' source faces | `tests/test_integration_m3_m4.cpp:221-283` |
| Collision gating retains nav faces and promotes/demotes chunks in place | `tests/test_integration_m3_m4.cpp:288-321` |

Run the suites with `run_all_tests.sh` (or the CMake test targets); the navigation tests
are Godot-free and run offline against the synthetic map-iteration helper.
