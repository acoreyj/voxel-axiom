#include "register_types.h"
#include "voxel_terrain_3d.h"
#include "voxel_ai_companion.h"

#include <chrono>
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

    // 6. Ordered teardown via _exit_tree().
    terrain._exit_tree();
    uninitialize_voxel_module(MODULE_INITIALIZATION_LEVEL_SCENE);
    std::filesystem::remove_all(save_dir);

    std::printf("[PASS] test_gdextension_streaming_explosion_and_nav_lifecycle (stream + blast + nav3d verified)\n");
}

} // namespace

int main() {
    std::printf("=== Running GDExtension End-to-End Streaming, Destruction & Nav3D Test Suite ===\n");
    test_generate_caves_flag_roundtrip();
    test_gdextension_streaming_explosion_and_nav_lifecycle();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
