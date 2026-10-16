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

} // namespace

int main() {
    std::printf("=== Running GDExtension End-to-End Streaming, Destruction & Nav3D Test Suite ===\n");
    test_generate_caves_flag_roundtrip();
    test_collision_toggles_roundtrip();
    test_terrain_depth_slider_roundtrip();
    test_world_seed_roundtrip();
    test_gdextension_streaming_explosion_and_nav_lifecycle();
    test_gdextension_mid_path_terrain_removal_and_frost_bridge_addition();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
