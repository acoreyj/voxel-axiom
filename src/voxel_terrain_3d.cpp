#include "voxel_terrain_3d.h"

#include <chrono>

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>
#endif

namespace godot {

namespace {
uint64_t monotonic_ticks_usec() noexcept {
#if defined(VOXEL_WITH_GODOT_CPP)
    return Time::get_singleton()->get_ticks_usec();
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
#endif
}
} // namespace

void VoxelTerrain3D::_bind_methods() {
#if defined(VOXEL_WITH_GODOT_CPP)
    ClassDB::bind_method(D_METHOD("trigger_explosion", "position", "radius"), &VoxelTerrain3D::trigger_explosion);
    ClassDB::bind_method(D_METHOD("save_world"), &VoxelTerrain3D::save_world);
    ClassDB::bind_method(D_METHOD("set_stream_focus", "position", "velocity"), &VoxelTerrain3D::set_stream_focus);
    ClassDB::bind_method(D_METHOD("core_idle"), &VoxelTerrain3D::core_idle);
    ClassDB::bind_method(D_METHOD("fully_committed"), &VoxelTerrain3D::fully_committed);
    ClassDB::bind_method(D_METHOD("get_effective_view_distance_chunks"), &VoxelTerrain3D::get_effective_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_view_distance_chunks"), &VoxelTerrain3D::get_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("set_view_distance_chunks", "distance"), &VoxelTerrain3D::set_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_prefetch_distance_chunks"), &VoxelTerrain3D::get_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("set_prefetch_distance_chunks", "distance"), &VoxelTerrain3D::set_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_resident_memory_budget_mb"), &VoxelTerrain3D::get_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("set_resident_memory_budget_mb", "mb"), &VoxelTerrain3D::set_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("get_upload_budget_ms"), &VoxelTerrain3D::get_upload_budget_ms);
    ClassDB::bind_method(D_METHOD("set_upload_budget_ms", "ms"), &VoxelTerrain3D::set_upload_budget_ms);
    ClassDB::bind_method(D_METHOD("get_terrain_material"), &VoxelTerrain3D::get_terrain_material);
    ClassDB::bind_method(D_METHOD("set_terrain_material", "material"), &VoxelTerrain3D::set_terrain_material);

    ADD_PROPERTY(PropertyInfo(Variant::INT, "view_distance_chunks", PROPERTY_HINT_RANGE, "1,24,1"),
                 "set_view_distance_chunks", "get_view_distance_chunks");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "prefetch_distance_chunks", PROPERTY_HINT_RANGE, "0,8,1"),
                 "set_prefetch_distance_chunks", "get_prefetch_distance_chunks");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "resident_memory_budget_mb", PROPERTY_HINT_RANGE, "128,4096,64"),
                 "set_resident_memory_budget_mb", "get_resident_memory_budget_mb");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upload_budget_ms", PROPERTY_HINT_RANGE, "0.5,16.0,0.5"),
                 "set_upload_budget_ms", "get_upload_budget_ms");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"),
                 "set_terrain_material", "get_terrain_material");

    ADD_SIGNAL(MethodInfo("explosion_requested",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius")));
    ADD_SIGNAL(MethodInfo("explosion_completed",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius"),
        PropertyInfo(Variant::INT, "affected_chunks")));
    ADD_SIGNAL(MethodInfo("explosion_failed",
        PropertyInfo(Variant::STRING, "reason")));
    ADD_SIGNAL(MethodInfo("chunk_committed",
        PropertyInfo(Variant::INT, "chunk_key")));
#endif
}

VoxelTerrain3D::VoxelTerrain3D() = default;

VoxelTerrain3D::~VoxelTerrain3D() {
    if (core_engine) {
        _exit_tree();
    }
}

#if defined(VOXEL_WITH_GODOT_CPP)
void VoxelTerrain3D::set_terrain_material(const Ref<Material>& p_mat) {
    terrain_material = p_mat;
    server_slot_manager.set_material_override(
        p_mat.is_valid() ? p_mat->get_rid() : RID());
}
#endif

void VoxelTerrain3D::set_resident_memory_budget_mb(int p_mb) {
    resident_memory_budget_mb = p_mb;
    if (core_engine) {
        core_engine->ledger.set_budget_bytes(
            static_cast<std::size_t>(p_mb) * voxel::MemoryLedger::kMiB);
    }
}

void VoxelTerrain3D::_enter_tree() {
    const std::size_t budget_bytes =
        static_cast<std::size_t>(resident_memory_budget_mb) * voxel::MemoryLedger::kMiB;
    core_engine = std::make_unique<CoreEngineBundle>(
        budget_bytes,
        static_cast<int32_t>(world_height_chunks),
#if defined(VOXEL_WITH_GODOT_CPP)
        std::string(save_directory_path.utf8().get_data()));
#else
        std::string(save_directory_path.c_str()));
#endif
    core_engine->worker_pool->start();

    voxel::ServerRid scenario_rid{2};
    voxel::ServerRid space_rid{3};
    voxel::ServerRid nav_map_rid{1};

#if defined(VOXEL_WITH_GODOT_CPP)
    if (get_world_3d().is_valid()) {
        scenario_rid.native_rid = get_world_3d()->get_scenario();
        space_rid.native_rid = get_world_3d()->get_space();
        nav_map_rid.native_rid = get_world_3d()->get_navigation_map();
    }
    if (terrain_material.is_valid()) {
        server_slot_manager.set_material_override(terrain_material->get_rid());
    }
#endif

    navigation_manager.initialize(nav_map_rid);
    server_slot_manager.initialize(scenario_rid, space_rid);
}

void VoxelTerrain3D::_exit_tree() {
    if (core_engine) {
        core_engine->worker_pool->shutdown(save_on_exit);
        core_engine.reset();
    }
    navigation_manager.shutdown();
    server_slot_manager.free_all_rids();
}

void VoxelTerrain3D::trigger_explosion(Vector3 p_position, float p_radius) {
    if (!core_engine || p_radius <= 0.0f) {
        const std::string reason = "engine not ready or radius is non-positive";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("explosion_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"explosion_failed", {}, 0.0f, 0, reason, 0});
#endif
        return;
    }

    const voxel::Vec3f native_pos{p_position.x, p_position.y, p_position.z};
    if (core_engine->worker_pool->enqueue_explosion(native_pos, p_radius)) {
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("explosion_requested", p_position, p_radius);
#else
        emitted_signals_.push_back(
            EmittedSignalRecord{"explosion_requested", p_position, p_radius, 0, {}, 0});
#endif
    } else {
        const std::string reason = "edit queue full, engine shutting down, or out of bounds";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("explosion_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"explosion_failed", {}, 0.0f, 0, reason, 0});
#endif
    }
}

void VoxelTerrain3D::save_world() {
    if (core_engine && core_engine->save_manager) {
        core_engine->save_manager->flush_all_dirty_regions();
    }
}

void VoxelTerrain3D::set_stream_focus(Vector3 p_position, Vector3 p_velocity) {
    stream_focus_position = p_position;
    stream_focus_velocity = p_velocity;
    if (core_engine && core_engine->streaming_manager) {
        const std::vector<voxel::ChunkKey> unloaded =
            core_engine->streaming_manager->update_stream_focus(
                voxel::Vec3f{p_position.x, p_position.y, p_position.z},
                voxel::Vec3f{p_velocity.x, p_velocity.y, p_velocity.z},
                get_effective_view_distance_chunks(),
                prefetch_distance_chunks,
                core_engine->stream_grid34);
        for (const voxel::ChunkKey uk : unloaded) {
            server_slot_manager.free_chunk_slot(uk);
            navigation_manager.note_chunk_unloaded(uk);
        }
    }
}

bool VoxelTerrain3D::core_idle() const {
    return core_engine && core_engine->worker_pool->core_idle();
}

bool VoxelTerrain3D::fully_committed() const {
    return core_idle() &&
           core_engine &&
           core_engine->worker_pool->completion_queue_depth() == 0 &&
           navigation_manager.navigation_groups_synced();
}

int VoxelTerrain3D::get_effective_view_distance_chunks() const {
    if (!core_engine) {
        return view_distance_chunks;
    }
    const std::size_t reserved = core_engine->ledger.total_reserved_bytes();
    const std::size_t budget = core_engine->ledger.budget_bytes();
    if (reserved >= budget && view_distance_chunks > 1) {
        return std::max(1, view_distance_chunks - 2);
    }
    return view_distance_chunks;
}

void VoxelTerrain3D::_process(double /*delta*/) {
    if (!core_engine) {
        return;
    }

    const uint64_t frame_start_us = monotonic_ticks_usec();
    const uint64_t soft_budget_us = static_cast<uint64_t>(upload_budget_ms * 1000.0f);
    const uint64_t emergency_budget_us = soft_budget_us * 2ULL;

    navigation_manager.poll_server_synchronization();

    while (true) {
        std::optional<voxel::CompletionRecord> done =
            core_engine->worker_pool->try_pop_completion();
        if (!done.has_value()) {
            break;
        }

        switch (done->kind) {
            case voxel::CompletionKind::ChunkMesh:
                server_slot_manager.commit_chunk_mesh_and_collision(done->chunk_mesh);
                navigation_manager.note_chunk_mesh_committed(
                    done->chunk_mesh.key,
                    done->chunk_mesh.captured_terrain_revision,
                    server_slot_manager.get_cached_nav_faces(done->chunk_mesh.key));
                break;

            case voxel::CompletionKind::ChunkUnloaded:
                server_slot_manager.free_chunk_slot(done->unloaded_key);
                navigation_manager.note_chunk_unloaded(done->unloaded_key);
                break;

            case voxel::CompletionKind::EditTransactionCompleted: {
                navigation_manager.apply_edit_transaction(done->edit_transaction);
                const Vector3 pos(done->edit_transaction.position.x,
                                  done->edit_transaction.position.y,
                                  done->edit_transaction.position.z);
                const int affected =
                    static_cast<int>(done->edit_transaction.total_affected_chunks);
#if defined(VOXEL_WITH_GODOT_CPP)
                emit_signal("explosion_completed", pos, done->edit_transaction.radius, affected);
#else
                emitted_signals_.push_back(EmittedSignalRecord{
                    "explosion_completed", pos, done->edit_transaction.radius, affected, {}, 0});
#endif
                break;
            }
        }

        const uint64_t elapsed_us = monotonic_ticks_usec() - frame_start_us;
        const std::size_t remaining_depth =
            core_engine->worker_pool->completion_queue_depth();
        if (remaining_depth == 0) {
            break;
        }

        const bool currently_over_cap = (remaining_depth >= 32ULL);
        if (!currently_over_cap && elapsed_us >= soft_budget_us) {
            break;
        }
        if (elapsed_us >= emergency_budget_us) {
            break;
        }
    }

    navigation_manager.submit_ready_groups_to_server(server_slot_manager);

    for (const voxel::ChunkKey key : navigation_manager.drain_newly_committed_chunks()) {
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("chunk_committed", static_cast<int64_t>(key.raw));
#else
        emitted_signals_.push_back(
            EmittedSignalRecord{"chunk_committed", {}, 0.0f, 0, {}, static_cast<int64_t>(key.raw)});
#endif
    }
}

} // namespace godot
