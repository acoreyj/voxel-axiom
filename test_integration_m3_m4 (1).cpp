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
    REQUIRE(std::abs(cfg.agent_max_slope - 45.0f) < 1e-5f);
    REQUIRE(std::abs(cfg.region_min_size - 8.0f) < 1e-5f);
    REQUIRE(cfg.vertical_padding() > 4.0f);   // P_y = 1.8 + 0.5 + 2.0 = 4.3 m
    REQUIRE(cfg.horizontal_padding() == 3.0f); // P_xz = 0.6 + 0.4 + 2.0 = 3.0 m

    // Carve a sloped ramp connecting Cy = 1 (gy = 0, y in [32, 64]) and Cy = 2 (gy = 1, y in [64, 96]).
    // Because gy = 0 has owned_aabb y in [0, 64] and vertical padding P_y = 4.3 m (baking_aabb y up to 68.3),
    // gy = 0's baking_aabb intersects Cy = 2 (y in [64, 96])!
    // Conversely, gy = 1's baking_aabb (y down to 64 - 4.3 = 59.7) intersects Cy = 1 (y in [32, 64])!
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

} // namespace

int main() {
    std::printf("=== Running M3/M4 Step 7 Integration Test Suite: Backpressure Loop, Server RIDs & 3D Cave Nav ===\n");
    test_bounded_frame_time_under_32_entry_backpressure();
    test_3d_cave_navigation_vertical_isolation();
    test_sloped_cave_vertical_boundary_halo_gathering();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
