#include "register_types.h"
#include "voxel_terrain_3d.h"
#include "voxel_ai_companion.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

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

void test_gdextension_streaming_explosion_and_nav_lifecycle() {
    ++g_tests_run;

    const std::filesystem::path save_dir = "/tmp/voxel_gdextension_e2e_test";
    std::filesystem::remove_all(save_dir);

    REQUIRE(voxel_engine_init(nullptr, nullptr, nullptr) == 1);
    REQUIRE(ClassRegistryTracker::registered_classes().size() == 2ULL);
    REQUIRE(ClassRegistryTracker::registered_classes()[0] == "VoxelTerrain3D");
    REQUIRE(ClassRegistryTracker::registered_classes()[1] == "VoxelAICompanion");

    VoxelTerrain3D terrain;
    terrain.set_save_directory_path(save_dir.string());
    terrain.set_view_distance_chunks(1);
    terrain.set_prefetch_distance_chunks(0);
    terrain.set_world_height_chunks(3);
    terrain.set_upload_budget_ms(8.0f);
    terrain._enter_tree();

    VoxelAICompanion companion;
    companion.set_terrain_node(&terrain);

    terrain.set_stream_focus(Vector3(16.0f, 48.0f, 16.0f), Vector3(1.0f, 0.0f, 0.0f));
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(terrain.get_server_slot_manager().active_slot_count() > 0ULL);

    terrain.trigger_explosion(Vector3(16.0f, 48.0f, 16.0f), -1.0f);
    REQUIRE(!terrain.emitted_signals().empty());
    REQUIRE(terrain.emitted_signals().back().name == "explosion_failed");

    terrain.trigger_explosion(Vector3(16.0f, 48.0f, 16.0f), 6.0f);
    REQUIRE(terrain.emitted_signals().back().name == "explosion_requested");

    wait_until_terrain_fully_committed(terrain);
    REQUIRE(terrain.core_idle());
    REQUIRE(companion.is_path_region_synced(Vector3(16.0f, 48.0f, 16.0f)));

    const PackedVector3Array path =
        companion.get_voxel_path(Vector3(4.0f, 48.0f, 4.0f), Vector3(28.0f, 50.0f, 28.0f));
    REQUIRE(path.size() == 3ULL);

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

    terrain.set_stream_focus(Vector3(16.0f, 68.0f, 16.0f), Vector3(0.0f, 0.0f, 0.0f));
    wait_until_terrain_fully_committed(terrain);

    const Vector3 start_pos(4.0f, 68.0f, 16.0f);
    const Vector3 goal_pos(28.0f, 68.0f, 16.0f);
    REQUIRE(companion.is_path_region_synced(start_pos));
    const PackedVector3Array initial_path = companion.get_voxel_path(start_pos, goal_pos);
    REQUIRE(initial_path.size() == 3ULL);
    const Vector3 initial_mid = initial_path[1];

    Vector3 companion_cur_pos(
        0.5f * (initial_path[0].x + initial_path[1].x),
        0.5f * (initial_path[0].y + initial_path[1].y),
        0.5f * (initial_path[0].z + initial_path[1].z));

    terrain.trigger_explosion(Vector3(16.0f, 68.0f, 16.0f), 7.5f);
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(companion.is_path_region_synced(companion_cur_pos));

    const PackedVector3Array path_after_crater = companion.get_voxel_path(companion_cur_pos, goal_pos);
    REQUIRE(path_after_crater.size() == 3ULL);
    const Vector3 crater_mid = path_after_crater[1];
    const float crater_shift = std::abs(crater_mid.x - initial_mid.x) +
                               std::abs(crater_mid.y - initial_mid.y) +
                               std::abs(crater_mid.z - initial_mid.z);
    REQUIRE(crater_shift > 0.01f);

    companion_cur_pos = Vector3(
        0.5f * (path_after_crater[0].x + path_after_crater[1].x),
        0.5f * (path_after_crater[0].y + path_after_crater[1].y),
        0.5f * (path_after_crater[0].z + path_after_crater[1].z));

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
// 3. End-to-End 2x Side-by-Side Moai Statue Stamp, Per-Voxel Baked Albedo Commit,
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
    const int opt2_id = terrain.stamp_asset("moai_statue.vreg", Vector3(-4.0f, -2.0f, -4.0f), 15.0f);
    REQUIRE(opt2_id > 0);

    terrain.set_stream_focus(Vector3(0.0f, 96.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f));
    wait_until_terrain_fully_committed(terrain);
    REQUIRE(painted_vertices_committed > 400ULL);

    // Simulate Option 1 Mesh -> Voxel Swap at chunk (-5, -2, -4) -> world center (-16, 96, 0)
    const int opt1_id = terrain.stamp_asset("moai_statue.vreg", Vector3(-5.0f, -2.0f, -4.0f), 0.0f);
    REQUIRE(opt1_id > 0);
    terrain.trigger_explosion(Vector3(-16.0f, 96.0f, 10.0f), 8.5f);
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

    std::printf("[PASS] test_gdextension_moai_stamp_baked_colors_and_remove_structure (%zu painted 2x Moai vertices committed, remove_structure verified)\n",
                painted_vertices_committed);
}

} // namespace

int main() {
    std::printf("=== Running GDExtension End-to-End Streaming, Destruction, Moai Albedo & Nav3D Test Suite ===\n");
    test_gdextension_streaming_explosion_and_nav_lifecycle();
    test_gdextension_mid_path_terrain_removal_and_frost_bridge_addition();
    test_gdextension_moai_stamp_baked_colors_and_remove_structure();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}