#include "register_types.h"
#include "voxel_terrain_3d.h"
#include "voxel_ai_companion.h"

#include <chrono>
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

void test_gdextension_registration_and_end_to_end_lifecycle() {
    ++g_tests_run;

    const std::filesystem::path save_dir = "/tmp/voxel_gdextension_e2e_test";
    std::filesystem::remove_all(save_dir);

    // 1. Verify voxel_engine_init registers both VoxelTerrain3D and VoxelAICompanion at SCENE level.
    REQUIRE(voxel_engine_init(nullptr, nullptr, nullptr) == 1);
    REQUIRE(ClassRegistryTracker::registered_classes().size() == 2ULL);
    REQUIRE(ClassRegistryTracker::registered_classes()[0] == "VoxelTerrain3D");
    REQUIRE(ClassRegistryTracker::registered_classes()[1] == "VoxelAICompanion");

    // 2. Instantiate VoxelTerrain3D and VoxelAICompanion and run full _enter_tree -> explosion -> _process -> _exit_tree.
    VoxelTerrain3D terrain;
    terrain.set_save_directory_path(save_dir.string());
    terrain.set_upload_budget_ms(4.0f);
    terrain._enter_tree();

    VoxelAICompanion companion;
    companion.set_terrain_node(&terrain);

    // Invalid explosion radius (<= 0) must emit explosion_failed immediately.
    terrain.trigger_explosion(Vector3(16.0f, 16.0f, 16.0f), -1.0f);
    REQUIRE(!terrain.emitted_signals().empty());
    REQUIRE(terrain.emitted_signals().back().name == "explosion_failed");

    // Valid explosion emits explosion_requested immediately (< 0.1 ms).
    terrain.set_stream_focus(Vector3(16.0f, 16.0f, 16.0f), Vector3(1.0f, 0.0f, 0.0f));
    terrain.trigger_explosion(Vector3(16.0f, 16.0f, 16.0f), 6.0f);
    REQUIRE(terrain.emitted_signals().back().name == "explosion_requested");

    // Tick _process() frames until explosion_completed and chunk_committed fire and fully_committed() is true.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        terrain._process(0.016);
        terrain.get_navigation_manager().complete_in_flight_async_bakes_and_advance_map_iteration();
        terrain._process(0.016);
        if (terrain.fully_committed()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    REQUIRE(terrain.core_idle());
    REQUIRE(terrain.fully_committed());
    REQUIRE(companion.is_path_region_synced(Vector3(16.0f, 16.0f, 16.0f)));

    const PackedVector3Array path =
        companion.get_voxel_path(Vector3(4.0f, 16.0f, 4.0f), Vector3(28.0f, 50.0f, 28.0f));
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

    // 3. Ordered teardown via _exit_tree().
    terrain._exit_tree();
    uninitialize_voxel_module(MODULE_INITIALIZATION_LEVEL_SCENE);
    std::filesystem::remove_all(save_dir);

    std::printf("[PASS] test_gdextension_registration_and_end_to_end_lifecycle (VoxelTerrain3D + VoxelAICompanion verified)\n");
}

} // namespace

int main() {
    std::printf("=== Running GDExtension End-to-End Binding & Lifecycle Test Suite ===\n");
    test_gdextension_registration_and_end_to_end_lifecycle();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
