#include "godot_wrapper_and_nav3d.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace {

using voxel::CaveNavMeshConfig;
using voxel::ChunkKey;
using voxel::ChunkMeshPayload;
using voxel::CompletionKind;
using voxel::CompletionRecord;
using voxel::EditTransactionCompletedPayload;
using voxel::NavigationGroupKey;
using voxel::NavigationGroupState;
using voxel::ProcessFrameStats;
using voxel::ServerRid;
using voxel::ServerSlotManager;
using voxel::Vec3f;
using voxel::VoxelNavigationManager;
using voxel::run_main_thread_process_frame;

int g_tests_run = 0;
int g_assertions_checked = 0;

#define REQUIRE(...)                                                                  \
    do {                                                                              \
        ++g_assertions_checked;                                                       \
        if (!(__VA_ARGS__)) {                                                         \
            std::fprintf(stderr, "[FAIL] %s:%d: Requirement failed: %s\n",            \
                         __FILE__, __LINE__, #__VA_ARGS__);                           \
            std::exit(EXIT_FAILURE);                                                  \
        }                                                                             \
    } while (0)

// ============================================================================
// 1. Bounded Frame Time Under 32-Entry Backpressure (Sections 4.3, 5.2, 7.2)
// ============================================================================
void test_bounded_frame_time_under_32_entry_backpressure() {
    ++g_tests_run;

    ServerSlotManager slot_mgr;
    slot_mgr.initialize(ServerRid{1}, ServerRid{2});

    VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(ServerRid{3});

    // Simulate a deterministic microsecond clock where each chunk commit takes 800 us (0.8 ms).
    // With upload_budget_ms = 2.0 ms:
    //   - soft_budget_us      = 2,000 us
    //   - emergency_budget_us = 4,000 us
    uint64_t simulated_clock_us = 100000ULL;
    slot_mgr.set_commit_hook([&](const ChunkMeshPayload&) {
        simulated_clock_us += 800ULL;
    });

    // Subtest A: Queue starts at exactly 32 entries (the v3.0 freeze bug scenario!).
    // In v3.0, evaluating `hard_backpressure` once before the loop caused all 32 entries
    // (32 * 800 us = 25,600 us = 25.6 ms) to drain in 1 frame.
    // In v4.0, popping the first chunk drops remaining_depth to 31 (< 32), immediately
    // restoring soft_budget_us (2,000 us) so the frame stops after 3 chunks (2,400 us)!
    std::deque<CompletionRecord> queue_a;
    for (int32_t i = 0; i < 32; ++i) {
        CompletionRecord rec{};
        rec.kind = CompletionKind::ChunkMesh;
        rec.chunk_mesh.key = ChunkKey::from_coords(i, 0, 0);
        rec.chunk_mesh.captured_version = 1;
        rec.chunk_mesh.captured_terrain_revision = 1;
        rec.chunk_mesh.collision_faces = {
            Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{1.0f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, 1.0f}};
        queue_a.push_back(std::move(rec));
    }

    const ProcessFrameStats frame_a = run_main_thread_process_frame(
        /*upload_budget_ms=*/2.0f,
        [&]() -> std::optional<CompletionRecord> {
            if (queue_a.empty()) return std::nullopt;
            CompletionRecord r = std::move(queue_a.front());
            queue_a.pop_front();
            return r;
        },
        [&]() -> std::size_t { return queue_a.size(); },
        [&]() -> uint64_t { return simulated_clock_us; },
        slot_mgr,
        nav_mgr);

    REQUIRE(frame_a.meshes_committed == 3ULL);
    REQUIRE(frame_a.elapsed_us == 2400ULL);
    REQUIRE(frame_a.elapsed_us <= 4000ULL); // Strictly within emergency_budget_us
    REQUIRE(queue_a.size() == 29ULL);       // Did NOT drain all 32 in one frame!

    // Subtest B: Workers continuously refill the queue so remaining_depth stays >= 32
    // throughout the frame (starting at 40 entries).
    // Verify the hard `emergency_budget_us = 4,000 us` ceiling stops the loop at 5 chunks (4,000 us).
    std::deque<CompletionRecord> queue_b;
    for (int32_t i = 0; i < 40; ++i) {
        CompletionRecord rec{};
        rec.kind = CompletionKind::ChunkMesh;
        rec.chunk_mesh.key = ChunkKey::from_coords(100 + i, 0, 0);
        rec.chunk_mesh.captured_version = 1;
        rec.chunk_mesh.captured_terrain_revision = 1;
        queue_b.push_back(std::move(rec));
    }

    const ProcessFrameStats frame_b = run_main_thread_process_frame(
        /*upload_budget_ms=*/2.0f,
        [&]() -> std::optional<CompletionRecord> {
            if (queue_b.empty()) return std::nullopt;
            CompletionRecord r = std::move(queue_b.front());
            queue_b.pop_front();
            return r;
        },
        [&]() -> std::size_t { return queue_b.size(); },
        [&]() -> uint64_t { return simulated_clock_us; },
        slot_mgr,
        nav_mgr);

    REQUIRE(frame_b.meshes_committed == 5ULL);
    REQUIRE(frame_b.elapsed_us == 4000ULL); // Stopped at emergency_budget_us (4.0 ms)
    REQUIRE(queue_b.size() == 35ULL);

    slot_mgr.free_all_rids();
    REQUIRE(slot_mgr.active_slot_count() == 0ULL);

    std::printf("[PASS] test_bounded_frame_time_under_32_entry_backpressure (Frame A: 2.4 ms / 3 meshes; Frame B capped at 4.0 ms)\n");
}

// ============================================================================
// 2. 3D Cave Navigation Isolation Test (Sections 4.5, 7.2)
// ============================================================================
void test_3d_cave_navigation_vertical_isolation() {
    ++g_tests_run;

    ServerSlotManager slot_mgr;
    slot_mgr.initialize(ServerRid{1}, ServerRid{2});

    VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(ServerRid{3});

    // Section 7.2 Integration Test:
    // "Create a 2-layer world (surface at Cy = 4, cave tunnel at Cy = 1).
    //  Trigger an explosion in the cave at Cy = 1. Verify only the underground
    //  gy = 0 (Cy in [0, 1]) navigation group rebakes while the surface
    //  gy = 2 (Cy in [4, 5]) navigation group records zero rebuilds."
    const ChunkKey cave_chunk    = ChunkKey::from_coords(0, 1, 0); // gy = floor_div(1, 2) = 0
    const ChunkKey surface_chunk = ChunkKey::from_coords(0, 4, 0); // gy = floor_div(4, 2) = 2

    const NavigationGroupKey cave_group_key    = NavigationGroupKey::from_chunk_key(cave_chunk);
    const NavigationGroupKey surface_group_key = NavigationGroupKey::from_chunk_key(surface_chunk);
    REQUIRE(cave_group_key.gy == 0);
    REQUIRE(surface_group_key.gy == 2);

    // Step 1: Initial load & commit of both the underground cave chunk (Cy = 1) and surface chunk (Cy = 4).
    for (const ChunkKey ck : {cave_chunk, surface_chunk}) {
        ChunkMeshPayload mesh{};
        mesh.key = ck;
        mesh.captured_version = 1;
        mesh.captured_terrain_revision = 0;
        mesh.collision_faces = {
            Vec3f{0.0f, 32.0f, 0.0f}, Vec3f{10.0f, 32.0f, 0.0f}, Vec3f{0.0f, 32.0f, 10.0f}};
        slot_mgr.commit_chunk_mesh_and_collision(mesh);
        nav_mgr.note_chunk_mesh_committed(ck, 0, slot_mgr.get_cached_nav_faces(ck));
    }

    nav_mgr.submit_ready_groups_to_server(slot_mgr);
    nav_mgr.complete_in_flight_async_bakes_and_advance_map_iteration();
    REQUIRE(nav_mgr.navigation_groups_synced());

    const NavigationGroupState* cave_group    = nav_mgr.find_group(cave_group_key);
    const NavigationGroupState* surface_group = nav_mgr.find_group(surface_group_key);
    REQUIRE(cave_group != nullptr);
    REQUIRE(surface_group != nullptr);
    REQUIRE(cave_group->total_bakes_submitted == 1ULL);
    REQUIRE(surface_group->total_bakes_submitted == 1ULL);

    // Step 2: Trigger an underground explosion in the cave at Cy = 1 (terrain_revision = 1).
    EditTransactionCompletedPayload cave_blast{};
    cave_blast.position = Vec3f{16.0f, 48.0f, 16.0f};
    cave_blast.radius = 6.0f;
    cave_blast.terrain_revision = 1;
    cave_blast.total_affected_chunks = 1;
    cave_blast.dirtied_chunks = {cave_chunk};
    cave_blast.affected_nav_groups = {cave_group_key};

    nav_mgr.apply_edit_transaction(cave_blast);

    // Cave region is now unsynced (awaiting remesh + rebake), while surface region remains synced!
    REQUIRE(!nav_mgr.is_path_region_synced(Vec3f{16.0f, 48.0f, 16.0f}));
    REQUIRE(nav_mgr.is_path_region_synced(Vec3f{16.0f, 140.0f, 16.0f}));

    // Commit updated cave mesh at terrain_revision = 1.
    {
        ChunkMeshPayload remesh{};
        remesh.key = cave_chunk;
        remesh.captured_version = 2;
        remesh.captured_terrain_revision = 1;
        remesh.collision_faces = {
            Vec3f{0.0f, 30.0f, 0.0f}, Vec3f{12.0f, 30.0f, 0.0f}, Vec3f{0.0f, 30.0f, 12.0f}};
        slot_mgr.commit_chunk_mesh_and_collision(remesh);
        nav_mgr.note_chunk_mesh_committed(cave_chunk, 1, slot_mgr.get_cached_nav_faces(cave_chunk));
    }

    nav_mgr.submit_ready_groups_to_server(slot_mgr);
    nav_mgr.complete_in_flight_async_bakes_and_advance_map_iteration();

    // Assert ONLY the underground gy = 0 group rebaked (total_bakes_submitted: 1 -> 2),
    // while the surface gy = 2 group recorded ZERO additional rebuilds (stays at 1)!
    REQUIRE(cave_group->total_bakes_submitted == 2ULL);
    REQUIRE(surface_group->total_bakes_submitted == 1ULL);
    REQUIRE(nav_mgr.navigation_groups_synced());
    REQUIRE(nav_mgr.is_path_region_synced(Vec3f{16.0f, 48.0f, 16.0f}));

    std::printf("[PASS] test_3d_cave_navigation_vertical_isolation (gy=0 cave rebaked, gy=2 surface untouched)\n");
}

// ============================================================================
// 3. Sloped Cave Tunnel 3D Border Padding Across gy=0 / gy=1 Seam (Section 7.2)
// ============================================================================
void test_sloped_cave_vertical_boundary_halo_gathering() {
    ++g_tests_run;

    ServerSlotManager slot_mgr;
    slot_mgr.initialize(ServerRid{1}, ServerRid{2});

    CaveNavMeshConfig cfg{};
    VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(ServerRid{3}, cfg);

    // Verify cave-specific NavigationMesh parameters (Section 4.5).
    REQUIRE(cfg.filter_low_hanging_obstacles);
    REQUIRE(cfg.filter_ledge_spans);
    REQUIRE(cfg.filter_walkable_low_height_spans);
    REQUIRE(std::abs(cfg.agent_max_slope - 70.0f) < 1e-5f);
    REQUIRE(std::abs(cfg.agent_max_climb - 1.5f) < 1e-5f);
    REQUIRE(std::abs(cfg.cell_size - 0.50f) < 1e-5f);
    REQUIRE(std::abs(cfg.cell_height - 0.25f) < 1e-5f);
    REQUIRE(std::abs(cfg.agent_radius - 0.35f) < 1e-5f);
    REQUIRE(std::abs(cfg.region_min_size - 8.0f) < 1e-5f);
    REQUIRE(cfg.vertical_padding() > 4.0f);   // P_y = 1.8 + 1.5 + 2.0 = 5.3 m
    // P_xz = 0.35 + 0.4 + 2.0 = 2.75 m, quantized up to 6 * cell_size = 3.00 m.
    REQUIRE(std::abs(cfg.horizontal_padding() - 3.00f) < 1e-5f);

    // Carve a sloped ramp connecting Cy = 1 (gy = 0, y in [32, 64]) and Cy = 2 (gy = 1, y in [64, 96]).
    // Because gy = 0 has owned_aabb y in [0, 64] and vertical padding P_y = 5.3 m (baking_aabb y up to 69.3),
    // gy = 0's baking_aabb intersects Cy = 2 (y in [64, 96])!
    // Conversely, gy = 1's baking_aabb (y down to 64 - 5.3 = 58.7) intersects Cy = 1 (y in [32, 64])!
    const ChunkKey lower_ramp_chunk = ChunkKey::from_coords(0, 1, 0); // gy = 0
    const ChunkKey upper_ramp_chunk = ChunkKey::from_coords(0, 2, 0); // gy = 1

    ChunkMeshPayload lower_mesh{};
    lower_mesh.key = lower_ramp_chunk;
    lower_mesh.captured_version = 1;
    lower_mesh.captured_terrain_revision = 1;
    lower_mesh.collision_faces = {
        Vec3f{10.0f, 60.0f, 10.0f}, Vec3f{14.0f, 60.0f, 10.0f}, Vec3f{12.0f, 64.0f, 14.0f}};

    ChunkMeshPayload upper_mesh{};
    upper_mesh.key = upper_ramp_chunk;
    upper_mesh.captured_version = 1;
    upper_mesh.captured_terrain_revision = 1;
    upper_mesh.collision_faces = {
        Vec3f{10.0f, 64.0f, 14.0f}, Vec3f{14.0f, 64.0f, 14.0f}, Vec3f{12.0f, 68.0f, 18.0f}};

    slot_mgr.commit_chunk_mesh_and_collision(lower_mesh);
    slot_mgr.commit_chunk_mesh_and_collision(upper_mesh);
    nav_mgr.note_chunk_mesh_committed(lower_ramp_chunk, 1, slot_mgr.get_cached_nav_faces(lower_ramp_chunk));
    nav_mgr.note_chunk_mesh_committed(upper_ramp_chunk, 1, slot_mgr.get_cached_nav_faces(upper_ramp_chunk));

    nav_mgr.submit_ready_groups_to_server(slot_mgr);
    nav_mgr.complete_in_flight_async_bakes_and_advance_map_iteration();

    const NavigationGroupKey gk0{0, 0, 0};
    const NavigationGroupKey gk1{0, 1, 0};
    const NavigationGroupState* g0 = nav_mgr.find_group(gk0);
    const NavigationGroupState* g1 = nav_mgr.find_group(gk1);
    REQUIRE(g0 != nullptr);
    REQUIRE(g1 != nullptr);

    // Both gy=0 and gy=1 groups must have gathered BOTH chunks' ramp triangles (6 vertices = 2 triangles)
    // across the vertical y=64 boundary thanks to P_y vertical border padding!
    REQUIRE(g0->assembled_source_faces.size() == 6ULL);
    REQUIRE(g1->assembled_source_faces.size() == 6ULL);
    REQUIRE(nav_mgr.navigation_groups_synced());

    std::printf("[PASS] test_sloped_cave_vertical_boundary_halo_gathering (3D P_y padding stitches gy=0 & gy=1)\n");
}

// ============================================================================
// 4. Collision Distance Gating on the Server Slot Manager (Section 4.4)
// ============================================================================
void test_collision_distance_gating_on_slot_manager() {
    ++g_tests_run;

    ServerSlotManager slot_mgr;
    slot_mgr.initialize(ServerRid{1}, ServerRid{2});

    ChunkMeshPayload far_mesh{};
    far_mesh.key = ChunkKey::from_coords(5, 0, 5);
    far_mesh.captured_version = 1;
    far_mesh.collision_faces = {
        Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{1.0f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, 1.0f}};
    // Committed visual-only: the shape carries no faces, but nav faces are retained.
    slot_mgr.commit_chunk_mesh_and_collision(far_mesh, /*build_collision=*/false);
    REQUIRE(slot_mgr.find_slot(far_mesh.key) != nullptr);
    REQUIRE(!slot_mgr.chunk_has_collision(far_mesh.key));
    REQUIRE(slot_mgr.get_cached_nav_faces(far_mesh.key).size() == 3ULL);

    // Promoting it into range rebuilds collision from the retained faces.
    slot_mgr.set_chunk_collision_enabled(far_mesh.key, true);
    REQUIRE(slot_mgr.chunk_has_collision(far_mesh.key));
    // Demoting it back out of range clears collision in place.
    slot_mgr.set_chunk_collision_enabled(far_mesh.key, false);
    REQUIRE(!slot_mgr.chunk_has_collision(far_mesh.key));

    // A chunk committed in range builds collision immediately.
    ChunkMeshPayload near_mesh{};
    near_mesh.key = ChunkKey::from_coords(0, 0, 0);
    near_mesh.captured_version = 1;
    near_mesh.collision_faces = far_mesh.collision_faces;
    slot_mgr.commit_chunk_mesh_and_collision(near_mesh);
    REQUIRE(slot_mgr.chunk_has_collision(near_mesh.key));

    std::printf("[PASS] test_collision_distance_gating_on_slot_manager (visual-only commit, enable/disable, faces retained)\n");
}

// ============================================================================
// 5. Mid-Path Terrain Removal (Crater) & Addition (Frost Ice Bridge) While
//    Pathing Is In Progress + Overlapping Edit During In-Flight Async Rebake
// ============================================================================
void test_terrain_added_and_removed_while_pathing_in_progress() {
    ++g_tests_run;

    ServerSlotManager slot_mgr;
    slot_mgr.initialize(ServerRid{1}, ServerRid{2});

    VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(ServerRid{3});

    const ChunkKey path_chunk_a = ChunkKey::from_coords(0, 0, 0);
    const ChunkKey path_chunk_b = ChunkKey::from_coords(1, 0, 0);
    const NavigationGroupKey gk = NavigationGroupKey::from_chunk_key(path_chunk_a);

    // Step 1: Initial flat walkway across chunks (0,0,0) and (1,0,0) at y = 16.0.
    for (const ChunkKey ck : {path_chunk_a, path_chunk_b}) {
        ChunkMeshPayload m{};
        m.key = ck;
        m.captured_version = 1;
        m.captured_terrain_revision = 0;
        m.collision_faces = {
            Vec3f{14.0f, 16.0f, 14.0f}, Vec3f{18.0f, 16.0f, 14.0f}, Vec3f{16.0f, 16.0f, 18.0f}};
        slot_mgr.commit_chunk_mesh_and_collision(m);
        nav_mgr.note_chunk_mesh_committed(ck, 0, slot_mgr.get_cached_nav_faces(ck));
    }
    nav_mgr.submit_ready_groups_to_server(slot_mgr);
    nav_mgr.complete_in_flight_async_bakes_and_advance_map_iteration();
    REQUIRE(nav_mgr.is_path_region_synced(Vec3f{16.0f, 16.0f, 16.0f}));

    const NavigationGroupState* group = nav_mgr.find_group(gk);
    REQUIRE(group != nullptr);
    REQUIRE(group->total_bakes_submitted == 1ULL);
    REQUIRE(group->assembled_source_faces.size() == 6ULL);

    // Companion starts walking along the path from x=2 to x=48 and reaches x=10 (pathing in progress).
    Vec3f companion_pos{10.0f, 16.0f, 16.0f};
    REQUIRE(nav_mgr.is_path_region_synced(companion_pos));

    // Step 2: TERRAIN REMOVED mid-path! An explosion (revision 1) carves a crater across BOTH chunks
    // (0,0,0) and (1,0,0) ahead of the companion.
    EditTransactionCompletedPayload crater_tx{};
    crater_tx.position = Vec3f{32.0f, 16.0f, 16.0f};
    crater_tx.radius = 8.0f;
    crater_tx.terrain_revision = 1;
    crater_tx.total_affected_chunks = 2;
    crater_tx.dirtied_chunks = {path_chunk_a, path_chunk_b};
    crater_tx.affected_nav_groups = {gk};
    nav_mgr.apply_edit_transaction(crater_tx);

    // Immediately after explosion: region MUST report unsynced so the companion does not trust stale waypoints.
    REQUIRE(!nav_mgr.is_path_region_synced(companion_pos));

    // Partial chunk commit: only path_chunk_a finishes meshing at revision 1; path_chunk_b is still meshing!
    // Verify submit_ready_groups_to_server DOES NOT start a premature bake on half-updated chunks!
    {
        ChunkMeshPayload m_a{};
        m_a.key = path_chunk_a;
        m_a.captured_version = 2;
        m_a.captured_terrain_revision = 1;
        m_a.collision_faces = {
            Vec3f{14.0f, 12.0f, 14.0f}, Vec3f{18.0f, 12.0f, 14.0f}, Vec3f{16.0f, 12.0f, 18.0f}};
        slot_mgr.commit_chunk_mesh_and_collision(m_a);
        nav_mgr.note_chunk_mesh_committed(path_chunk_a, 1, slot_mgr.get_cached_nav_faces(path_chunk_a));
    }
    nav_mgr.submit_ready_groups_to_server(slot_mgr);
    REQUIRE(!group->rebuild_in_flight);
    REQUIRE(group->total_bakes_submitted == 1ULL);
    REQUIRE(!nav_mgr.is_path_region_synced(companion_pos));

    // Now path_chunk_b finishes meshing at revision 1 -> group is ready and submits async bake #2!
    {
        ChunkMeshPayload m_b{};
        m_b.key = path_chunk_b;
        m_b.captured_version = 2;
        m_b.captured_terrain_revision = 1;
        m_b.collision_faces = {
            Vec3f{34.0f, 12.0f, 14.0f}, Vec3f{38.0f, 12.0f, 14.0f}, Vec3f{36.0f, 12.0f, 18.0f}};
        slot_mgr.commit_chunk_mesh_and_collision(m_b);
        nav_mgr.note_chunk_mesh_committed(path_chunk_b, 1, slot_mgr.get_cached_nav_faces(path_chunk_b));
    }
    nav_mgr.submit_ready_groups_to_server(slot_mgr);
    REQUIRE(group->rebuild_in_flight);
    REQUIRE(group->total_bakes_submitted == 2ULL);

    // Step 3: TERRAIN ADDED (Frost Ice Bridge, revision 2) arrives WHILE bake #2 (revision 1) is STILL IN FLIGHT!
    EditTransactionCompletedPayload ice_bridge_tx{};
    ice_bridge_tx.position = Vec3f{16.0f, 20.0f, 16.0f};
    ice_bridge_tx.radius = 6.0f;
    ice_bridge_tx.terrain_revision = 2;
    ice_bridge_tx.total_affected_chunks = 1;
    ice_bridge_tx.dirtied_chunks = {path_chunk_a};
    ice_bridge_tx.affected_nav_groups = {gk};
    nav_mgr.apply_edit_transaction(ice_bridge_tx);

    // Even when in-flight bake #2 (revision 1) finishes, the group MUST STILL report unsynced
    // because desired_terrain_revision is now 2 (the newly added Frost Ice Bridge)!
    nav_mgr.complete_in_flight_async_bakes_and_advance_map_iteration();
    REQUIRE(group->synced_terrain_revision == 1ULL);
    REQUIRE(group->desired_terrain_revision == 2ULL);
    REQUIRE(!nav_mgr.is_path_region_synced(companion_pos));

    // Commit the new Frost Ice Bridge mesh (adding elevated walkable triangles at y = 20.0) at revision 2.
    {
        ChunkMeshPayload m_ice{};
        m_ice.key = path_chunk_a;
        m_ice.captured_version = 3;
        m_ice.captured_terrain_revision = 2;
        m_ice.collision_faces = {
            Vec3f{14.0f, 12.0f, 14.0f}, Vec3f{18.0f, 12.0f, 14.0f}, Vec3f{16.0f, 12.0f, 18.0f},
            Vec3f{14.0f, 20.0f, 14.0f}, Vec3f{18.0f, 20.0f, 14.0f}, Vec3f{16.0f, 20.0f, 18.0f}};
        slot_mgr.commit_chunk_mesh_and_collision(m_ice);
        nav_mgr.note_chunk_mesh_committed(path_chunk_a, 2, slot_mgr.get_cached_nav_faces(path_chunk_a));
    }

    nav_mgr.submit_ready_groups_to_server(slot_mgr);
    REQUIRE(group->rebuild_in_flight);
    REQUIRE(group->total_bakes_submitted == 3ULL);

    nav_mgr.complete_in_flight_async_bakes_and_advance_map_iteration();
    REQUIRE(nav_mgr.is_path_region_synced(companion_pos));
    REQUIRE(group->synced_terrain_revision == 2ULL);
    // Assembled faces now include the crater floor + the newly added Ice bridge (9 vertices = 3 triangles)!
    REQUIRE(group->assembled_source_faces.size() == 9ULL);

    std::printf("[PASS] test_terrain_added_and_removed_while_pathing_in_progress (mid-path crater + in-flight Frost Ice bridge verified)\n");
}

} // namespace

int main() {
    std::printf("=== Running M3/M4 Step 7 Integration Test Suite: Backpressure Loop, Server RIDs & 3D Cave Nav ===\n");
    test_bounded_frame_time_under_32_entry_backpressure();
    test_3d_cave_navigation_vertical_isolation();
    test_sloped_cave_vertical_boundary_halo_gathering();
    test_collision_distance_gating_on_slot_manager();
    test_terrain_added_and_removed_while_pathing_in_progress();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
