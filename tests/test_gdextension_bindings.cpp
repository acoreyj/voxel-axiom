#include "register_types.h"
#include "voxel_terrain_3d.h"
#include "voxel_ai_companion.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using godot::ClassRegistryTracker;
using godot::MODULE_INITIALIZATION_LEVEL_SCENE;
using godot::PackedVector3Array;
using godot::Vector3;
using godot::VoxelAICompanion;
using godot::VoxelTerrain3D;

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

void wait_until_terrain_fully_committed(VoxelTerrain3D& terrain) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        terrain._process(0.016);
        terrain.get_navigation_manager().complete_in_flight_async_bakes_and_advance_map_iteration();
        terrain._process(0.016);
        if (terrain.fully_committed() && terrain.get_server_slot_manager().active_slot_count() > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(terrain.fully_committed());
}

void test_generate_caves_flag_roundtrip() {
    ++g_tests_run;

    VoxelTerrain3D terrain;
    REQUIRE(terrain.get_generate_caves() == true);

    terrain.set_generate_caves(false);
    REQUIRE(terrain.get_generate_caves() == false);

    terrain.set_generate_caves(true);
    REQUIRE(terrain.get_generate_caves() == true);

    std::printf("[PASS] test_generate_caves_flag_roundtrip (default caves-on, set/get round-trips)\n");
}

void test_agent_navigation_limits_roundtrip() {
    ++g_tests_run;

    VoxelTerrain3D terrain;
    REQUIRE(std::abs(terrain.get_agent_max_slope() - 70.0f) < 1e-6f);
    REQUIRE(std::abs(terrain.get_agent_max_climb() - 1.5f) < 1e-6f);

    terrain.set_agent_max_slope(60.0f);
    terrain.set_agent_max_climb(1.2f);
    REQUIRE(std::abs(terrain.get_agent_max_slope() - 60.0f) < 1e-6f);
    REQUIRE(std::abs(terrain.get_agent_max_climb() - 1.2f) < 1e-6f);

    terrain.set_agent_max_slope(120.0f);
    terrain.set_agent_max_climb(9.0f);
    REQUIRE(std::abs(terrain.get_agent_max_slope() - 89.0f) < 1e-6f);
    REQUIRE(std::abs(terrain.get_agent_max_climb() - 4.0f) < 1e-6f);

    std::printf("[PASS] test_agent_navigation_limits_roundtrip (defaults 70 deg / 1.5 m, clamped ranges)\n");
}

void test_collision_toggles_roundtrip() {
    ++g_tests_run;

    VoxelTerrain3D terrain;
    REQUIRE(terrain.get_collision_distance_chunks() == -1);
    REQUIRE(terrain.get_collision_subsample_stride() == 1);

    terrain.set_collision_distance_chunks(3);
    REQUIRE(terrain.get_collision_distance_chunks() == 3);
    terrain.set_collision_distance_chunks(-1);
    REQUIRE(terrain.get_collision_distance_chunks() == -1);

    terrain.set_collision_subsample_stride(2);
    REQUIRE(terrain.get_collision_subsample_stride() == 2);
    // Clamped to the seamless values (1 or 2) so collision never cracks at chunk seams.
    terrain.set_collision_subsample_stride(4);
    REQUIRE(terrain.get_collision_subsample_stride() == 2);
    terrain.set_collision_subsample_stride(1);
    REQUIRE(terrain.get_collision_subsample_stride() == 1);

    std::printf("[PASS] test_collision_toggles_roundtrip (collision_distance_chunks + collision_subsample_stride bind/round-trip)\n");
}

void test_terrain_depth_slider_roundtrip() {
    ++g_tests_run;

    VoxelTerrain3D terrain;
    REQUIRE(terrain.get_world_height_chunks() == 4);
    REQUIRE(terrain.get_surface_base_height() == 68.0f);

    terrain.set_world_height_chunks(6);
    REQUIRE(terrain.get_world_height_chunks() == 6);

    terrain.set_surface_base_height(132.0f);
    REQUIRE(terrain.get_surface_base_height() == 132.0f);

    std::printf("[PASS] test_terrain_depth_slider_roundtrip (world_height_chunks + surface_base_height set/get)\n");
}

void test_world_seed_roundtrip() {
    ++g_tests_run;

    VoxelTerrain3D terrain;
    REQUIRE(terrain.get_world_seed() == 1337);

    terrain.set_world_seed(424242);
    REQUIRE(terrain.get_world_seed() == 424242);

    terrain.set_world_seed(0);
    REQUIRE(terrain.get_world_seed() == 0);

    std::printf("[PASS] test_world_seed_roundtrip (default 1337, set/get round-trips)\n");
}

void test_voxel_scale_roundtrip() {
    ++g_tests_run;

    VoxelTerrain3D terrain;
    REQUIRE(std::abs(terrain.get_voxel_scale() - 1.0f) < 1e-6f);

    terrain.set_voxel_scale(0.5f);
    REQUIRE(std::abs(terrain.get_voxel_scale() - 0.5f) < 1e-6f);

    terrain.set_voxel_scale(0.01f);
    REQUIRE(std::abs(terrain.get_voxel_scale() - 0.125f) < 1e-6f);

    terrain.set_voxel_scale(16.0f);
    REQUIRE(std::abs(terrain.get_voxel_scale() - 4.0f) < 1e-6f);

    std::printf("[PASS] test_voxel_scale_roundtrip (default 1.0 m, clamped to [0.125, 4.0])\n");
}

void test_gdextension_fine_voxel_scale_streaming_and_edits() {
    ++g_tests_run;

    const std::filesystem::path save_dir = "/tmp/voxel_gdextension_voxel_scale_test";
    std::filesystem::remove_all(save_dir);

    VoxelTerrain3D terrain;
    terrain.set_save_directory_path(save_dir.string());
    terrain.set_voxel_scale(0.5f);
    terrain.set_view_distance_chunks(1);
    terrain.set_prefetch_distance_chunks(0);
    terrain.set_world_height_chunks(6);
    terrain.set_upload_budget_ms(8.0f);
    terrain._enter_tree();

    // Stream around the world-space surface. A correct focus mapping lands on
    // grid sample 136 = chunk y 4 at 0.5 m/voxel; a broken one would stream chunk y 2.
    terrain.set_stream_focus(Vector3(8.0f, 68.0f, 8.0f), Vector3(0.0f, 0.0f, 0.0f));
    wait_until_terrain_fully_committed(terrain);

    const float surface_y = terrain.get_surface_height(8.0f, 8.0f);
    // A generator that ignored voxel_scale would place the surface near world y=34.
    REQUIRE(surface_y > 40.0f);
    REQUIRE(surface_y < 92.0f);

    const Vector3 down_hit =
        terrain.raycast_terrain(Vector3(8.0f, 92.0f, 8.0f), Vector3(8.0f, 0.0f, 8.0f));
    REQUIRE(std::isfinite(down_hit.x));
    // Raycast returns world meters: the hit must sit near the queried surface.
    REQUIRE(std::abs(down_hit.y - surface_y) < 2.0f);
    const int material =
        terrain.raycast_terrain_material(Vector3(8.0f, 92.0f, 8.0f), Vector3(8.0f, 0.0f, 8.0f));
    REQUIRE(material >= 0);

    // Explosion and fill inputs are world meters; completion signals must echo them.
    const Vector3 blast_center(8.0f, surface_y, 8.0f);
    terrain.trigger_explosion(blast_center, 3.0f);
    REQUIRE(terrain.emitted_signals().back().name == "explosion_requested");
    REQUIRE(std::abs(terrain.emitted_signals().back().radius - 3.0f) < 1e-6f);

    wait_until_terrain_fully_committed(terrain);

    bool saw_explosion_completed = false;
    for (const auto& sig : terrain.emitted_signals()) {
        if (sig.name == "explosion_completed") {
            saw_explosion_completed = true;
            REQUIRE(std::abs(sig.position.x - blast_center.x) < 0.01f);
            REQUIRE(std::abs(sig.position.y - blast_center.y) < 0.01f);
            REQUIRE(std::abs(sig.position.z - blast_center.z) < 0.01f);
            REQUIRE(std::abs(sig.radius - 3.0f) < 1e-6f);
        }
    }
    REQUIRE(saw_explosion_completed);

    terrain.add_terrain(blast_center, 3.0f);
    wait_until_terrain_fully_committed(terrain);

    bool saw_fill_completed = false;
    for (const auto& sig : terrain.emitted_signals()) {
        if (sig.name == "terrain_add_completed") {
            saw_fill_completed = true;
            REQUIRE(std::abs(sig.radius - 3.0f) < 1e-6f);
        }
    }
    REQUIRE(saw_fill_completed);

    terrain._exit_tree();
    std::filesystem::remove_all(save_dir);

    std::printf("[PASS] test_gdextension_fine_voxel_scale_streaming_and_edits (0.5 m voxels: stream, raycast, world-space edit signals verified)\n");
}

void test_gdextension_streaming_explosion_and_nav_lifecycle() {
    ++g_tests_run;

    const std::filesystem::path save_dir = "/tmp/voxel_gdextension_e2e_test";
    std::filesystem::remove_all(save_dir);

    // 1. Verify voxel_engine_init registers both VoxelTerrain3D and VoxelAICompanion at SCENE level.
    REQUIRE(voxel_engine_init(nullptr, nullptr, nullptr) == 1);
    REQUIRE(ClassRegistryTracker::registered_classes().size() == 2ULL);
    REQUIRE(ClassRegistryTracker::registered_classes()[0] == "VoxelTerrain3D");
    REQUIRE(ClassRegistryTracker::registered_classes()[1] == "VoxelAICompanion");

    // 2. Instantiate VoxelTerrain3D and VoxelAICompanion.
    VoxelTerrain3D terrain;
    terrain.set_save_directory_path(save_dir.string());
    terrain.set_view_distance_chunks(1);
    terrain.set_prefetch_distance_chunks(0);
    terrain.set_world_height_chunks(3);
    terrain.set_upload_budget_ms(8.0f);
    terrain._enter_tree();

    // Capture instrumentation output instead of printing it.
    std::vector<std::string> log_lines;
    terrain.set_log_sink([&log_lines](const std::string& line) { log_lines.push_back(line); });
    const auto contains = [&log_lines](const std::string& needle) {
        for (const std::string& line : log_lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    VoxelAICompanion companion;
    companion.set_terrain_node(&terrain);

    // 3. Verify continuous terrain streaming from set_stream_focus() alone:
    terrain.set_stream_focus(Vector3(16.0f, 48.0f, 16.0f), Vector3(1.0f, 0.0f, 0.0f));

    const auto stream_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < stream_deadline) {
        terrain._process(0.016);
        terrain.get_navigation_manager().complete_in_flight_async_bakes_and_advance_map_iteration();
        terrain._process(0.016);
        if (terrain.fully_committed() && terrain.get_server_slot_manager().active_slot_count() > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(terrain.fully_committed());
    REQUIRE(terrain.get_server_slot_manager().active_slot_count() > 0ULL);

    // 3b. Instrumentation must report per-stage timing for the streaming work.
    REQUIRE(contains("generate"));
    REQUIRE(contains("mesh"));
    REQUIRE(contains("commit"));
    REQUIRE(contains("initial fill complete in"));

    // 3c. Engine-side voxel raycast: downward from above the surface must hit
    // terrain, upward into the sky must miss (INF sentinel).
    const Vector3 down_hit =
        terrain.raycast_terrain(Vector3(16.0f, 90.0f, 16.0f), Vector3(16.0f, 0.0f, 16.0f));
    REQUIRE(std::isfinite(down_hit.x));
    REQUIRE(down_hit.y > 0.0f);
    REQUIRE(down_hit.y < 90.0f);

    const Vector3 sky_hit =
        terrain.raycast_terrain(Vector3(16.0f, 90.0f, 16.0f), Vector3(16.0f, 250.0f, 16.0f));
    REQUIRE(std::isinf(sky_hit.x));

    // 3d. The material-aware raycast reports the solid sample behind the surface:
    // the streamed surface at (16, ?, 16) is soil or a hard-rock vein, and a sky
    // ray reports the -1 miss sentinel.
    const int down_material =
        terrain.raycast_terrain_material(Vector3(16.0f, 90.0f, 16.0f), Vector3(16.0f, 0.0f, 16.0f));
    REQUIRE(down_material == voxel::MaterialHardness::kMaterialSoil ||
            down_material == voxel::MaterialHardness::kMaterialHardRock);
    const int sky_material =
        terrain.raycast_terrain_material(Vector3(16.0f, 90.0f, 16.0f), Vector3(16.0f, 250.0f, 16.0f));
    REQUIRE(sky_material == -1);

    // 4. Invalid explosion radius (<= 0) must emit explosion_failed immediately.
    terrain.trigger_explosion(Vector3(16.0f, 48.0f, 16.0f), -1.0f);
    REQUIRE(!terrain.emitted_signals().empty());
    REQUIRE(terrain.emitted_signals().back().name == "explosion_failed");

    // 5. Valid explosion carves the streamed terrain.
    terrain.trigger_explosion(Vector3(16.0f, 48.0f, 16.0f), 6.0f);
    REQUIRE(terrain.emitted_signals().back().name == "explosion_requested");

    const auto blast_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < blast_deadline) {
        terrain._process(0.016);
        terrain.get_navigation_manager().complete_in_flight_async_bakes_and_advance_map_iteration();
        terrain._process(0.016);
        if (terrain.fully_committed()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(terrain.core_idle());
    REQUIRE(terrain.fully_committed());
    REQUIRE(companion.is_path_region_synced(Vector3(16.0f, 48.0f, 16.0f)));

    // 5b. The edit stage must be timed for the carve.
    REQUIRE(contains("edit"));

    const PackedVector3Array path =
        companion.get_voxel_path(Vector3(4.0f, 48.0f, 4.0f), Vector3(28.0f, 50.0f, 28.0f));
    REQUIRE(path.size() == 3ULL);

    // 5c. The path query is timed; drain it with one more frame and verify.
    terrain._process(0.016);
    REQUIRE(contains("navigation"));

    bool saw_explosion_completed = false;
    bool saw_chunk_committed = false;
    for (const auto& sig : terrain.emitted_signals()) {
        if (sig.name == "explosion_completed") {
            saw_explosion_completed = true;
            REQUIRE(sig.affected_chunks > 0);
        } else if (sig.name == "chunk_committed") {
            saw_chunk_committed = true;
        }
    }
    REQUIRE(saw_explosion_completed);
    REQUIRE(saw_chunk_committed);

    // 5d. Invalid add-terrain radius (<= 0) must emit terrain_add_failed immediately.
    terrain.add_terrain(Vector3(16.0f, 48.0f, 16.0f), -1.0f);
    REQUIRE(terrain.emitted_signals().back().name == "terrain_add_failed");

    // 5e. Valid add-terrain fills the previously carved crater back in.
    terrain.add_terrain(Vector3(16.0f, 48.0f, 16.0f), 6.0f);
    REQUIRE(terrain.emitted_signals().back().name == "terrain_add_requested");

    const auto fill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < fill_deadline) {
        terrain._process(0.016);
        terrain.get_navigation_manager().complete_in_flight_async_bakes_and_advance_map_iteration();
        terrain._process(0.016);
        if (terrain.fully_committed()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(terrain.core_idle());
    REQUIRE(terrain.fully_committed());

    bool saw_terrain_add_completed = false;
    std::size_t terrain_add_completed_count = 0;
    for (const auto& sig : terrain.emitted_signals()) {
        if (sig.name == "terrain_add_completed") {
            saw_terrain_add_completed = true;
            ++terrain_add_completed_count;
            REQUIRE(sig.affected_chunks > 0);
        }
    }
    REQUIRE(saw_terrain_add_completed);

    // 5f. Invalid material-fill / boulder radii fail cleanly through the same signals.
    terrain.add_terrain_with_material(Vector3(16.0f, 48.0f, 16.0f), 0.0f, 2);
    REQUIRE(terrain.emitted_signals().back().name == "terrain_add_failed");
    terrain.place_boulder(Vector3(16.0f, 48.0f, 16.0f), -2.0f);
    REQUIRE(terrain.emitted_signals().back().name == "terrain_add_failed");

    // 5g. Rock-material fill and boulder placement route through the fill pipeline.
    terrain.add_terrain_with_material(Vector3(20.0f, 48.0f, 16.0f), 5.0f, 2);
    REQUIRE(terrain.emitted_signals().back().name == "terrain_add_requested");
    terrain.place_boulder(Vector3(12.0f, 48.0f, 16.0f), 4.0f);
    REQUIRE(terrain.emitted_signals().back().name == "terrain_add_requested");

    const auto boulder_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < boulder_deadline) {
        terrain._process(0.016);
        terrain.get_navigation_manager().complete_in_flight_async_bakes_and_advance_map_iteration();
        terrain._process(0.016);
        if (terrain.fully_committed()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(terrain.core_idle());
    REQUIRE(terrain.fully_committed());

    std::size_t final_add_completed_count = 0;
    for (const auto& sig : terrain.emitted_signals()) {
        if (sig.name == "terrain_add_completed") {
            ++final_add_completed_count;
        }
    }
    REQUIRE(final_add_completed_count >= terrain_add_completed_count + 2ULL);

    // 6. Ordered teardown via _exit_tree().
    terrain._exit_tree();
    uninitialize_voxel_module(MODULE_INITIALIZATION_LEVEL_SCENE);
    std::filesystem::remove_all(save_dir);

    std::printf("[PASS] test_gdextension_streaming_explosion_and_nav_lifecycle (stream + blast + nav3d verified)\n");
}

// ============================================================================
// 2. End-to-End Mid-Path Terrain Removal (Explosion Crater) & Addition (Frost
//    Ice Bridge) While Companion Pathing Is In Progress
// ============================================================================
void test_gdextension_mid_path_terrain_removal_and_frost_bridge_addition() {
    ++g_tests_run;

    const std::filesystem::path save_dir = "/tmp/voxel_gdextension_midpath_test";
    std::filesystem::remove_all(save_dir);

    VoxelTerrain3D terrain;
    terrain.set_save_directory_path(save_dir.string());
    terrain.set_view_distance_chunks(1);
    terrain.set_prefetch_distance_chunks(0);
    terrain.set_world_height_chunks(3);
    terrain.set_upload_budget_ms(8.0f);
    terrain._enter_tree();

    VoxelAICompanion companion;
    companion.set_terrain_node(&terrain);

    // 1. Stream around (16, 68, 16) and establish initial baked path from start (4, 68, 16) to goal (28, 68, 16).
    terrain.set_stream_focus(Vector3(16.0f, 68.0f, 16.0f), Vector3(0.0f, 0.0f, 0.0f));
    wait_until_terrain_fully_committed(terrain);

    const Vector3 start_pos(4.0f, 68.0f, 16.0f);
    const Vector3 goal_pos(28.0f, 68.0f, 16.0f);
    REQUIRE(companion.is_path_region_synced(start_pos));
    const PackedVector3Array initial_path = companion.get_voxel_path(start_pos, goal_pos);
    REQUIRE(initial_path.size() == 3ULL);
    const Vector3 initial_mid = initial_path[1];

    // Simulate companion stepping partway along segment 0 -> 1 (pathing is now in progress at t = 0.5).
    Vector3 companion_cur_pos(
        0.5f * (initial_path[0].x + initial_path[1].x),
        0.5f * (initial_path[0].y + initial_path[1].y),
        0.5f * (initial_path[0].z + initial_path[1].z));

    // 2. REMOVE TERRAIN MID-PATH: Fire an explosion right at the path midpoint (16, 68, 16) while walking!
    terrain.trigger_explosion(Vector3(16.0f, 68.0f, 16.0f), 7.5f);
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(companion.is_path_region_synced(companion_cur_pos));

    // Re-query path from companion's current mid-walk position to goal after crater removal:
    const PackedVector3Array path_after_crater = companion.get_voxel_path(companion_cur_pos, goal_pos);
    REQUIRE(path_after_crater.size() == 3ULL);
    const Vector3 crater_mid = path_after_crater[1];
    // Verify the midpoint geometry changed after the crater was carved!
    const float crater_shift = std::abs(crater_mid.x - initial_mid.x) +
                               std::abs(crater_mid.y - initial_mid.y) +
                               std::abs(crater_mid.z - initial_mid.z);
    REQUIRE(crater_shift > 0.01f);

    // Advance companion another step along the rerouted path.
    companion_cur_pos = Vector3(
        0.5f * (path_after_crater[0].x + path_after_crater[1].x),
        0.5f * (path_after_crater[0].y + path_after_crater[1].y),
        0.5f * (path_after_crater[0].z + path_after_crater[1].z));

    // 3. ADD TERRAIN MID-PATH: Fire a Frost elemental sphere (`element_kind = 2`) at the carved midpoint
    //    to solidify air into a walkable Ice bridge while the companion is still pathing toward goal_pos!
    terrain.trigger_elemental_sphere(
        Vector3(crater_mid.x, crater_mid.y + 4.0f, crater_mid.z),
        6.5f,
        static_cast<int>(voxel::ElementalKind::Frost));
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(companion.is_path_region_synced(companion_cur_pos));

    const PackedVector3Array path_after_ice_bridge = companion.get_voxel_path(companion_cur_pos, goal_pos);
    REQUIRE(path_after_ice_bridge.size() == 3ULL);
    const Vector3 ice_mid = path_after_ice_bridge[1];
    const float bridge_shift = std::abs(ice_mid.x - crater_mid.x) +
                               std::abs(ice_mid.y - crater_mid.y) +
                               std::abs(ice_mid.z - crater_mid.z);
    REQUIRE(bridge_shift > 0.01f);

    terrain._exit_tree();
    std::filesystem::remove_all(save_dir);

    std::printf("[PASS] test_gdextension_mid_path_terrain_removal_and_frost_bridge_addition (mid-walk crater & Ice bridge reroute verified)\n");
}

// ============================================================================
// 3. End-to-End Side-by-Side Moai Statue Stamp, Per-Voxel Baked Albedo Commit,
//    and remove_structure() Reset Loop via VoxelTerrain3D
// ============================================================================
void test_gdextension_moai_stamp_baked_colors_and_remove_structure() {
    ++g_tests_run;

    const std::filesystem::path save_dir = "/tmp/voxel_gdextension_moai_test";
    std::filesystem::remove_all(save_dir);

    VoxelTerrain3D terrain;
    terrain.set_save_directory_path(save_dir.string());
    terrain.set_view_distance_chunks(2);
    terrain.set_prefetch_distance_chunks(0);
    terrain.set_world_height_chunks(4);
    terrain.set_upload_budget_ms(8.0f);
    terrain._enter_tree();

    std::size_t painted_vertices_committed = 0;
    terrain.get_server_slot_manager().set_commit_hook([&](const voxel::ChunkMeshPayload& mesh) {
        for (float w : mesh.baked_albedo_weights) {
            if (w > 0.5f) {
                ++painted_vertices_committed;
            }
        }
    });

    // Test stamping BEFORE set_stream_focus (exact _ready() order in terrain_demo.gd)
    // to verify background streaming never overwrites the stamped 2x Moai statue!
    // Seat the statue on the surface: tests/assets/moai_statue.vreg is baked centred at
    // (144,160,128) and its solid base is at local y=136, so stamping at origin
    // chunk y = round((surface_y - 136) / 32) puts the base on the terrain.
    const float opt2_surface_y = terrain.get_surface_height(16.0f, 0.0f);
    const int opt2_origin_y = static_cast<int>(std::lround((opt2_surface_y - 136.0f) / 32.0f));
    std::printf("[MoaiTest] surface at (16,0)=%.2f -> origin chunk y=%d\n", opt2_surface_y, opt2_origin_y);
    const int opt2_id = terrain.stamp_asset(
        "tests/assets/moai_statue.vreg", Vector3(-4.0f, static_cast<float>(opt2_origin_y), -4.0f), 15.0f);
    REQUIRE(opt2_id > 0);

    terrain.set_stream_focus(
        Vector3(16.0f, static_cast<float>(opt2_origin_y) * 32.0f + 160.0f, 0.0f),
        Vector3(0.0f, 0.0f, 0.0f));
    wait_until_terrain_fully_committed(terrain);
    // 2x voxel-tuned Moai: ~17k painted vertices ride into the mesh payload.
    REQUIRE(painted_vertices_committed > 4000ULL);

    // Simulate Option 1 Mesh -> Voxel Swap on the surface at world x=-16, z=0.
    const float opt1_surface_y = terrain.get_surface_height(-16.0f, 0.0f);
    const int opt1_origin_y = static_cast<int>(std::lround((opt1_surface_y - 136.0f) / 32.0f));
    const int opt1_id = terrain.stamp_asset(
        "tests/assets/moai_statue.vreg", Vector3(-5.0f, static_cast<float>(opt1_origin_y), -4.0f), 0.0f);
    REQUIRE(opt1_id > 0);
    terrain.trigger_explosion(
        Vector3(-16.0f, static_cast<float>(opt1_origin_y) * 32.0f + 160.0f, 6.0f), 6.5f);
    wait_until_terrain_fully_committed(terrain);

    // Press R to reset Option 1 via remove_structure(opt1_id)
    REQUIRE(terrain.remove_structure(opt1_id));
    REQUIRE(!terrain.remove_structure(opt1_id));
    wait_until_terrain_fully_committed(terrain);

    bool saw_structure_removed_signal = false;
    for (const auto& sig : terrain.emitted_signals()) {
        if (sig.name == "structure_removed" && sig.structure_id == opt1_id) {
            saw_structure_removed_signal = true;
        }
    }
    REQUIRE(saw_structure_removed_signal);

    terrain._exit_tree();
    std::filesystem::remove_all(save_dir);

    std::printf("[PASS] test_gdextension_moai_stamp_baked_colors_and_remove_structure (%zu painted Moai vertices committed, remove_structure verified)\n",
                painted_vertices_committed);
}

// ============================================================================
// 4. End-to-End Undo/Redo Stack, 3-Hit Hard Rock & Lava Quench Undo, and
//    Coalesced undo_all() Benchmark Pass Terrain Reset via VoxelTerrain3D
// ============================================================================
void test_gdextension_undo_redo_and_benchmark_pass_reset() {
    ++g_tests_run;

    const std::filesystem::path save_dir = "/tmp/voxel_gdextension_undo_redo_test";
    std::filesystem::remove_all(save_dir);

    VoxelTerrain3D terrain;
    terrain.set_save_directory_path(save_dir.string());
    terrain.set_view_distance_chunks(2);
    terrain.set_prefetch_distance_chunks(0);
    terrain.set_world_height_chunks(4);
    terrain.set_upload_budget_ms(8.0f);
    terrain._enter_tree();

    terrain.set_stream_focus(Vector3(16.0f, 70.0f, 16.0f), Vector3(0.0f, 0.0f, 0.0f));
    wait_until_terrain_fully_committed(terrain);

    // Build the same pristine showcase features terrain_demo.gd stamps at startup:
    // a 3-hit Hard Rock boulder and a Molten Lava dome. Seat them on the local
    // surface so the ray probes and shatter test exercise above-ground geometry.
    const float boulder_surf_y = terrain.get_surface_height(16.0f, 24.0f);
    const float lava_surf_y = terrain.get_surface_height(30.0f, 24.0f);
    terrain.add_terrain_with_material(Vector3(16.0f, boulder_surf_y, 24.0f), 4.0f,
                                      voxel::MaterialHardness::kMaterialHardRock);
    terrain.add_terrain_with_material(Vector3(30.0f, lava_surf_y, 24.0f), 4.0f,
                                      voxel::MaterialHardness::kMaterialLava);
    wait_until_terrain_fully_committed(terrain);
    terrain.clear_undo_history();
    REQUIRE(!terrain.can_undo());
    REQUIRE(!terrain.can_redo());
    REQUIRE(terrain.get_undo_count() == 0);
    REQUIRE(terrain.get_redo_count() == 0);

    // 1. Sample pristine surface height at (8, 14) and verify the baseline features.
    const float pristine_surf_y = terrain.get_surface_height(8.0f, 14.0f);
    const Vector3 boulder_probe_from(16.0f, boulder_surf_y + 15.0f, 24.0f);
    const Vector3 boulder_probe_to(16.0f, 40.0f, 24.0f);
    const Vector3 pristine_boulder_hit = terrain.raycast_terrain(boulder_probe_from, boulder_probe_to);
    REQUIRE(std::isfinite(pristine_boulder_hit.y));
    REQUIRE(terrain.raycast_terrain_material(boulder_probe_from, boulder_probe_to) ==
            static_cast<int>(voxel::MaterialHardness::kMaterialHardRock));

    const Vector3 lava_probe_from(30.0f, lava_surf_y + 15.0f, 24.0f);
    const Vector3 lava_probe_to(30.0f, 40.0f, 24.0f);
    REQUIRE(terrain.raycast_terrain_material(lava_probe_from, lava_probe_to) ==
            static_cast<int>(voxel::MaterialHardness::kMaterialLava));

    // 2. Carve a crater at (8, pristine_surf_y, 14) and verify Undo / Redo.
    terrain.trigger_explosion(Vector3(8.0f, pristine_surf_y, 14.0f), 8.5f);
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(terrain.can_undo());
    REQUIRE(terrain.get_undo_count() == 1);
    const float carved_surf_y = terrain.get_surface_height(8.0f, 14.0f);
    REQUIRE(carved_surf_y < pristine_surf_y - 2.0f);

    // Undo the crater -> surface height must return to pristine_surf_y!
    REQUIRE(terrain.undo());
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(!terrain.can_undo());
    REQUIRE(terrain.can_redo());
    REQUIRE(terrain.get_undo_count() == 0);
    REQUIRE(terrain.get_redo_count() == 1);
    REQUIRE(std::abs(terrain.get_surface_height(8.0f, 14.0f) - pristine_surf_y) < 0.05f);

    // Redo the crater -> surface height must drop back to carved_surf_y!
    REQUIRE(terrain.redo());
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(terrain.can_undo());
    REQUIRE(!terrain.can_redo());
    REQUIRE(std::abs(terrain.get_surface_height(8.0f, 14.0f) - carved_surf_y) < 0.05f);

    // 3. Simulate the rest of the benchmark pass CSG & Elemental sequence:
    //    - 3 hits on the Hard Rock boulder at (16, 70, 24) to shatter it
    //    - Water quench on the Lava dome at (30, 69, 24) -> turns Lava into Obsidian
    for (int hit = 0; hit < 3; ++hit) {
        terrain.trigger_explosion(Vector3(16.0f, boulder_surf_y, 24.0f), 8.5f);
        wait_until_terrain_fully_committed(terrain);
    }
    const Vector3 shattered_boulder_hit = terrain.raycast_terrain(boulder_probe_from, boulder_probe_to);
    REQUIRE(shattered_boulder_hit.y < pristine_boulder_hit.y - 3.0f);

    terrain.trigger_elemental_sphere(
        Vector3(30.0f, lava_surf_y, 24.0f), 8.5f, static_cast<int>(voxel::ElementalKind::Water));
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(terrain.raycast_terrain_material(lava_probe_from, lava_probe_to) ==
            static_cast<int>(voxel::MaterialHardness::kMaterialObsidian));

    REQUIRE(terrain.get_undo_count() == 5);

    // 4. Call undo_all() to reset the entire terrain before the next benchmark pass!
    const int undone_steps = terrain.undo_all();
    REQUIRE(undone_steps == 5);
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(terrain.get_undo_count() == 0);
    REQUIRE(terrain.get_redo_count() == 5);

    // Verify all terrain features are 100% restored to their pre-pass state:
    REQUIRE(std::abs(terrain.get_surface_height(8.0f, 14.0f) - pristine_surf_y) < 0.05f);
    const Vector3 restored_boulder_hit = terrain.raycast_terrain(boulder_probe_from, boulder_probe_to);
    REQUIRE(std::abs(restored_boulder_hit.y - pristine_boulder_hit.y) < 0.05f);
    REQUIRE(terrain.raycast_terrain_material(boulder_probe_from, boulder_probe_to) ==
            static_cast<int>(voxel::MaterialHardness::kMaterialHardRock));
    REQUIRE(terrain.raycast_terrain_material(lava_probe_from, lava_probe_to) ==
            static_cast<int>(voxel::MaterialHardness::kMaterialLava));

    // redo_all() re-applies all 5 steps in one coalesced batch, then undo_all() +
    // clear_undo_history() resets cleanly back to the pristine baseline.
    REQUIRE(terrain.redo_all() == 5);
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(terrain.raycast_terrain_material(lava_probe_from, lava_probe_to) ==
            static_cast<int>(voxel::MaterialHardness::kMaterialObsidian));

    REQUIRE(terrain.undo_all() == 5);
    terrain.clear_undo_history();
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(!terrain.can_undo());
    REQUIRE(!terrain.can_redo());
    REQUIRE(terrain.raycast_terrain_material(lava_probe_from, lava_probe_to) ==
            static_cast<int>(voxel::MaterialHardness::kMaterialLava));

    terrain._exit_tree();
    std::filesystem::remove_all(save_dir);

    std::printf("[PASS] test_gdextension_undo_redo_and_benchmark_pass_reset (undo/redo/undo_all/redo_all + 3-hit boulder & Lava->Obsidian reset verified)\n");
}

} // namespace

int main() {
    std::printf("=== Running GDExtension End-to-End Streaming, Destruction, Moai Albedo & Nav3D Test Suite ===\n");
    test_generate_caves_flag_roundtrip();
    test_agent_navigation_limits_roundtrip();
    test_collision_toggles_roundtrip();
    test_terrain_depth_slider_roundtrip();
    test_world_seed_roundtrip();
    test_voxel_scale_roundtrip();
    test_gdextension_fine_voxel_scale_streaming_and_edits();
    test_gdextension_streaming_explosion_and_nav_lifecycle();
    test_gdextension_mid_path_terrain_removal_and_frost_bridge_addition();
    test_gdextension_moai_stamp_baked_colors_and_remove_structure();
    test_gdextension_undo_redo_and_benchmark_pass_reset();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
