#pragma once

#include "register_types.h"
#include "godot_wrapper_and_nav3d.hpp"
#include "stage_timings.hpp"

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node3d.hpp>
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace godot {

#if !defined(VOXEL_WITH_GODOT_CPP)
class Node3D {
public:
    virtual ~Node3D() = default;
};
#endif

struct EmittedSignalRecord {
    std::string name;
    Vector3 position{};
    float radius = 0.0f;
    int affected_chunks = 0;
    std::string reason;
    int64_t chunk_key = 0;
};

/// VoxelTerrain3D Node (HLD v4.0 Sections 4.2, 5.2).
/// Owns CoreVoxelEngine components, StreamingManager, direct RenderingServer/PhysicsServer3D
/// RID slots, and 3D `4 x 2 x 4` NavigationGroups.
class VoxelTerrain3D : public Node3D {
#if defined(VOXEL_WITH_GODOT_CPP)
    GDCLASS(VoxelTerrain3D, Node3D)
#endif

public:
    static void _bind_methods();

    VoxelTerrain3D();
    ~VoxelTerrain3D() override;

    void _enter_tree();
    void _exit_tree();
    void _process(double delta);

    // Core GDScript-exposed API (Section 4.2)
    void trigger_explosion(Vector3 p_position, float p_radius);
    void save_world();
    void set_stream_focus(Vector3 p_position, Vector3 p_velocity);
    [[nodiscard]] bool core_idle() const;
    [[nodiscard]] bool fully_committed() const;
    [[nodiscard]] int get_effective_view_distance_chunks() const;

    // Properties (Section 4.2, 5.2)
    void set_save_directory_path(const String& p_path) { save_directory_path = p_path; }
    [[nodiscard]] String get_save_directory_path() const { return save_directory_path; }

    void set_view_distance_chunks(int p_dist) { view_distance_chunks = p_dist; }
    [[nodiscard]] int get_view_distance_chunks() const { return view_distance_chunks; }

    void set_prefetch_distance_chunks(int p_dist) { prefetch_distance_chunks = p_dist; }
    [[nodiscard]] int get_prefetch_distance_chunks() const { return prefetch_distance_chunks; }

    void set_resident_memory_budget_mb(int p_mb);
    [[nodiscard]] int get_resident_memory_budget_mb() const { return resident_memory_budget_mb; }

    void set_upload_budget_ms(float p_ms) { upload_budget_ms = p_ms; }
    [[nodiscard]] float get_upload_budget_ms() const { return upload_budget_ms; }

    void set_world_height_chunks(int p_height) { world_height_chunks = p_height; }
    [[nodiscard]] int get_world_height_chunks() const { return world_height_chunks; }

    void set_save_on_exit(bool p_save) { save_on_exit = p_save; }
    [[nodiscard]] bool get_save_on_exit() const { return save_on_exit; }

    void set_generate_caves(bool p_enabled);
    [[nodiscard]] bool get_generate_caves() const { return generate_caves; }

    // Instrumentation (HLD v4.0 Section 7.3): per-operation wall-clock logging.
    void set_instrumentation_enabled(bool p_enabled);
    [[nodiscard]] bool get_instrumentation_enabled() const { return instrumentation_enabled; }

    /// Redirects timing output. Defaults to Godot's `print`; tests inject a
    /// capture sink, and an empty sink silences logging.
    void set_log_sink(std::function<void(const std::string&)> p_sink) {
        log_sink_ = std::move(p_sink);
    }

    /// Records a navigation path/distance query duration (called by VoxelAICompanion).
    void record_navigation_query_us(uint64_t p_us) const;

#if defined(VOXEL_WITH_GODOT_CPP)
    void set_terrain_material(const Ref<Material>& p_mat);
    [[nodiscard]] Ref<Material> get_terrain_material() const { return terrain_material; }
#endif

    // Accessors for VoxelAICompanion & integration tests
    [[nodiscard]] voxel::VoxelNavigationManager& get_navigation_manager() noexcept {
        return navigation_manager;
    }
    [[nodiscard]] const voxel::VoxelNavigationManager& get_navigation_manager() const noexcept {
        return navigation_manager;
    }
    [[nodiscard]] voxel::ServerSlotManager& get_server_slot_manager() noexcept {
        return server_slot_manager;
    }
    [[nodiscard]] const std::vector<EmittedSignalRecord>& emitted_signals() const noexcept {
        return emitted_signals_;
    }

private:
    struct CoreEngineBundle {
        voxel::MemoryLedger ledger;
        voxel::ChunkStore store;
        voxel::DualDirtyScheduler dirty_scheduler;
        voxel::CaveNoiseGenerator procgen;
        std::unique_ptr<voxel::RegionSaveManager> save_manager;
        std::unique_ptr<voxel::CoreWorkerPool> worker_pool;
        std::unique_ptr<voxel::StreamingManager> streaming_manager;

        CoreEngineBundle(std::size_t budget_bytes, int32_t height_chunks, const std::string& save_dir)
            : ledger(budget_bytes),
              store(ledger, height_chunks) {
            save_manager = std::make_unique<voxel::RegionSaveManager>(ledger, save_dir);
            worker_pool = std::make_unique<voxel::CoreWorkerPool>(
                store,
                dirty_scheduler,
                [](const voxel::ChunkStore&,
                   voxel::ChunkKey ck,
                   voxel::WorkerScratchWindow36& scratch,
                   const voxel::NeighborhoodVersionStamp& stamp) {
                    return voxel::SurfaceNetsMesher::mesh_chunk_from_scratch_36(
                        ck, scratch, 1, stamp.captured_terrain_revision);
                },
                // Audit F10: thread-local WorkerDensityGrid34 preallocated per worker thread
                // (0 heap allocations per chunk instead of constructing one per generate call).
                [this](voxel::ChunkStore& s, voxel::ChunkKey ck) {
                    thread_local voxel::WorkerDensityGrid34 thread_grid34(ledger);
                    (void)save_manager->load_or_generate_chunk(s, ck, procgen, thread_grid34);
                });
            streaming_manager = std::make_unique<voxel::StreamingManager>(
                store, dirty_scheduler, *save_manager, *worker_pool);
            worker_pool->set_shutdown_save_hook([this](voxel::ChunkStore& s) {
                (void)s;
                save_manager->flush_all_dirty_regions();
                save_manager->clear_ram_cache();
            });
        }
    };

    String save_directory_path = "/tmp/voxel_world_saves";
    int view_distance_chunks = 2;
    int prefetch_distance_chunks = 1;
    int resident_memory_budget_mb = 512;
    float upload_budget_ms = 2.0f;
    int world_height_chunks = 4;
    bool save_on_exit = true;
    bool generate_caves = true;

#if defined(VOXEL_WITH_GODOT_CPP)
    Ref<Material> terrain_material;
#endif

    Vector3 stream_focus_position{};
    Vector3 stream_focus_velocity{};

    void _apply_stage_timings();
    void _log(const std::string& p_message) const;

    std::unique_ptr<CoreEngineBundle> core_engine;
    voxel::ServerSlotManager server_slot_manager;
    voxel::VoxelNavigationManager navigation_manager;
    std::vector<EmittedSignalRecord> emitted_signals_;

    bool instrumentation_enabled = true;
    mutable voxel::StageTimings stage_timings_;
    std::function<void(const std::string&)> log_sink_;
    uint64_t startup_begin_us_ = 0;
    bool startup_armed_ = false;
    bool startup_reported_ = false;
};

} // namespace godot
