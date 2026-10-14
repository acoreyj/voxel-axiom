#include "voxel_terrain_3d.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
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
    ClassDB::bind_method(D_METHOD("add_terrain", "position", "radius"), &VoxelTerrain3D::add_terrain);
    ClassDB::bind_method(D_METHOD("save_world"), &VoxelTerrain3D::save_world);
    ClassDB::bind_method(D_METHOD("set_stream_focus", "position", "velocity"), &VoxelTerrain3D::set_stream_focus);
    ClassDB::bind_method(D_METHOD("core_idle"), &VoxelTerrain3D::core_idle);
    ClassDB::bind_method(D_METHOD("fully_committed"), &VoxelTerrain3D::fully_committed);
    ClassDB::bind_method(D_METHOD("get_effective_view_distance_chunks"), &VoxelTerrain3D::get_effective_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_view_distance_chunks"), &VoxelTerrain3D::get_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("set_view_distance_chunks", "distance"), &VoxelTerrain3D::set_view_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_prefetch_distance_chunks"), &VoxelTerrain3D::get_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("set_prefetch_distance_chunks", "distance"), &VoxelTerrain3D::set_prefetch_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_collision_distance_chunks"), &VoxelTerrain3D::get_collision_distance_chunks);
    ClassDB::bind_method(D_METHOD("set_collision_distance_chunks", "distance"), &VoxelTerrain3D::set_collision_distance_chunks);
    ClassDB::bind_method(D_METHOD("get_collision_subsample_stride"), &VoxelTerrain3D::get_collision_subsample_stride);
    ClassDB::bind_method(D_METHOD("set_collision_subsample_stride", "stride"), &VoxelTerrain3D::set_collision_subsample_stride);
    ClassDB::bind_method(D_METHOD("get_resident_memory_budget_mb"), &VoxelTerrain3D::get_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("set_resident_memory_budget_mb", "mb"), &VoxelTerrain3D::set_resident_memory_budget_mb);
    ClassDB::bind_method(D_METHOD("get_upload_budget_ms"), &VoxelTerrain3D::get_upload_budget_ms);
    ClassDB::bind_method(D_METHOD("set_upload_budget_ms", "ms"), &VoxelTerrain3D::set_upload_budget_ms);
    ClassDB::bind_method(D_METHOD("get_terrain_material"), &VoxelTerrain3D::get_terrain_material);
    ClassDB::bind_method(D_METHOD("set_terrain_material", "material"), &VoxelTerrain3D::set_terrain_material);
    ClassDB::bind_method(D_METHOD("get_world_height_chunks"), &VoxelTerrain3D::get_world_height_chunks);
    ClassDB::bind_method(D_METHOD("set_world_height_chunks", "height"), &VoxelTerrain3D::set_world_height_chunks);
    ClassDB::bind_method(D_METHOD("get_surface_base_height"), &VoxelTerrain3D::get_surface_base_height);
    ClassDB::bind_method(D_METHOD("set_surface_base_height", "height"), &VoxelTerrain3D::set_surface_base_height);
    ClassDB::bind_method(D_METHOD("get_generate_caves"), &VoxelTerrain3D::get_generate_caves);
    ClassDB::bind_method(D_METHOD("set_generate_caves", "enabled"), &VoxelTerrain3D::set_generate_caves);
    ClassDB::bind_method(D_METHOD("get_instrumentation_enabled"), &VoxelTerrain3D::get_instrumentation_enabled);
    ClassDB::bind_method(D_METHOD("set_instrumentation_enabled", "enabled"), &VoxelTerrain3D::set_instrumentation_enabled);

    ADD_PROPERTY(PropertyInfo(Variant::INT, "view_distance_chunks", PROPERTY_HINT_RANGE, "1,24,1"),
                 "set_view_distance_chunks", "get_view_distance_chunks");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "prefetch_distance_chunks", PROPERTY_HINT_RANGE, "0,8,1"),
                 "set_prefetch_distance_chunks", "get_prefetch_distance_chunks");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_distance_chunks", PROPERTY_HINT_RANGE, "-1,24,1"),
                 "set_collision_distance_chunks", "get_collision_distance_chunks");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_subsample_stride", PROPERTY_HINT_RANGE, "1,2,1"),
                 "set_collision_subsample_stride", "get_collision_subsample_stride");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "resident_memory_budget_mb", PROPERTY_HINT_RANGE, "128,4096,64"),
                 "set_resident_memory_budget_mb", "get_resident_memory_budget_mb");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upload_budget_ms", PROPERTY_HINT_RANGE, "0.5,16.0,0.5"),
                 "set_upload_budget_ms", "get_upload_budget_ms");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "world_height_chunks", PROPERTY_HINT_RANGE, "1,12,1"),
                 "set_world_height_chunks", "get_world_height_chunks");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_base_height", PROPERTY_HINT_RANGE, "16.0,256.0,1.0"),
                 "set_surface_base_height", "get_surface_base_height");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"),
                 "set_terrain_material", "get_terrain_material");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "generate_caves"),
                 "set_generate_caves", "get_generate_caves");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "instrumentation_enabled"),
                 "set_instrumentation_enabled", "get_instrumentation_enabled");

    ADD_SIGNAL(MethodInfo("explosion_requested",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius")));
    ADD_SIGNAL(MethodInfo("explosion_completed",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius"),
        PropertyInfo(Variant::INT, "affected_chunks")));
    ADD_SIGNAL(MethodInfo("explosion_failed",
        PropertyInfo(Variant::STRING, "reason")));
    ADD_SIGNAL(MethodInfo("terrain_add_requested",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius")));
    ADD_SIGNAL(MethodInfo("terrain_add_completed",
        PropertyInfo(Variant::VECTOR3, "position"),
        PropertyInfo(Variant::FLOAT, "radius"),
        PropertyInfo(Variant::INT, "affected_chunks")));
    ADD_SIGNAL(MethodInfo("terrain_add_failed",
        PropertyInfo(Variant::STRING, "reason")));
    ADD_SIGNAL(MethodInfo("chunk_committed",
        PropertyInfo(Variant::INT, "chunk_key")));
#endif
}

VoxelTerrain3D::VoxelTerrain3D() {
#if defined(VOXEL_WITH_GODOT_CPP)
    log_sink_ = [](const std::string& p_line) {
        UtilityFunctions::print(String("[voxel] ") + String(p_line.c_str()));
    };
#endif
}

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

void VoxelTerrain3D::set_surface_base_height(float p_height) {
    surface_base_height = p_height;
    if (core_engine) {
        core_engine->procgen.surface_base_height = p_height;
    }
}

void VoxelTerrain3D::set_generate_caves(bool p_enabled) {
    generate_caves = p_enabled;
    if (core_engine) {
        core_engine->procgen.caves_enabled = p_enabled;
    }
}

void VoxelTerrain3D::set_collision_distance_chunks(int p_dist) {
    collision_distance_chunks = p_dist;
    collision_focus_valid_ = false; // force a re-evaluation on the next focus update
    if (core_engine) {
        _reconcile_collision_range();
    }
}

void VoxelTerrain3D::set_collision_subsample_stride(int p_stride) {
    // The 36^3 window reaches only two voxels into the neighbour chunk, so stride > 2
    // would crack collision across chunk seams; clamp to the seamless values (1 or 2).
    collision_subsample_stride = (p_stride >= 2) ? 2 : 1;
}

bool VoxelTerrain3D::chunk_in_collision_range(voxel::ChunkKey p_key) const {
    if (collision_distance_chunks < 0) {
        return true;
    }
    const voxel::ChunkKey focus = voxel::ChunkKey::from_world_sample(
        static_cast<int32_t>(std::floor(stream_focus_position.x)),
        static_cast<int32_t>(std::floor(stream_focus_position.y)),
        static_cast<int32_t>(std::floor(stream_focus_position.z)));
    const int32_t dx = std::abs(p_key.x() - focus.x());
    const int32_t dy = std::abs(p_key.y() - focus.y());
    const int32_t dz = std::abs(p_key.z() - focus.z());
    return std::max({dx, dy, dz}) <= collision_distance_chunks;
}

void VoxelTerrain3D::_reconcile_collision_range() {
    server_slot_manager.for_each_slot([this](voxel::ChunkKey key, voxel::ChunkServerSlot& slot) {
        const bool in_range = chunk_in_collision_range(key);
#if defined(VOXEL_WITH_GODOT_CPP)
        const bool has_faces = !slot.godot_nav_faces.is_empty();
#else
        const bool has_faces = !slot.cached_nav_faces.empty();
#endif
        server_slot_manager.set_chunk_collision_enabled(key, in_range && has_faces);
    });
}

void VoxelTerrain3D::set_instrumentation_enabled(bool p_enabled) {
    instrumentation_enabled = p_enabled;
    _apply_stage_timings();
}

void VoxelTerrain3D::_apply_stage_timings() {
    voxel::StageTimings* const timings = instrumentation_enabled ? &stage_timings_ : nullptr;
    if (core_engine) {
        core_engine->worker_pool->set_stage_timings(timings);
        core_engine->streaming_manager->set_stage_timings(timings);
    }
}

void VoxelTerrain3D::_log(const std::string& p_message) const {
    if (log_sink_) {
        log_sink_(p_message);
    }
}

void VoxelTerrain3D::record_navigation_query_us(uint64_t p_us) const {
    if (instrumentation_enabled) {
        stage_timings_.record(voxel::Stage::Navigation, p_us);
    }
}

void VoxelTerrain3D::_enter_tree() {
    const std::size_t budget_bytes =
        static_cast<std::size_t>(resident_memory_budget_mb) * voxel::MemoryLedger::kMiB;
    core_engine = std::make_unique<CoreEngineBundle>(
        budget_bytes,
        static_cast<int32_t>(world_height_chunks),
#if defined(VOXEL_WITH_GODOT_CPP)
        std::string(save_directory_path.utf8().get_data()),
#else
        std::string(save_directory_path.c_str()),
#endif
        &collision_subsample_stride);
    core_engine->procgen.caves_enabled = generate_caves;
    core_engine->procgen.surface_base_height = surface_base_height;

    // Cap worker count to physical core budget (at most 6 workers) so SMT hyperthreads
    // never starve Godot's main, render, physics, or NavigationServer threads.
    const unsigned int hc = std::thread::hardware_concurrency();
    const std::size_t num_workers = std::clamp<std::size_t>(hc / 2, 2ULL, 6ULL);
    core_engine->worker_pool->start(num_workers);

    startup_armed_ = false;
    startup_reported_ = false;
    _apply_stage_timings();

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
    // Disable automatic Recast rebaking on initial background streaming chunks so startup
    // and flying never trigger 80M-voxel Recast stalls; rebake only on explosions/edits!
    navigation_manager.set_bake_on_stream_enabled(false);
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

void VoxelTerrain3D::add_terrain(Vector3 p_position, float p_radius) {
    if (!core_engine || p_radius <= 0.0f) {
        const std::string reason = "engine not ready or radius is non-positive";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("terrain_add_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"terrain_add_failed", {}, 0.0f, 0, reason, 0});
#endif
        return;
    }

    const voxel::Vec3f native_pos{p_position.x, p_position.y, p_position.z};
    if (core_engine->worker_pool->enqueue_fill_spheres(native_pos, p_radius)) {
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("terrain_add_requested", p_position, p_radius);
#else
        emitted_signals_.push_back(
            EmittedSignalRecord{"terrain_add_requested", p_position, p_radius, 0, {}, 0});
#endif
    } else {
        const std::string reason = "edit queue full, engine shutting down, or out of bounds";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("terrain_add_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"terrain_add_failed", {}, 0.0f, 0, reason, 0});
#endif
    }
}

void VoxelTerrain3D::save_world() {
    if (core_engine && core_engine->save_manager) {
        core_engine->save_manager->flush_all_dirty_regions();
    }
}

void VoxelTerrain3D::set_stream_focus(Vector3 p_position, Vector3 p_velocity) {
    if (!startup_armed_) {
        startup_armed_ = true;
        startup_reported_ = false;
        startup_begin_us_ = monotonic_ticks_usec();
    }
    stream_focus_position = p_position;
    stream_focus_velocity = p_velocity;
    if (core_engine && core_engine->streaming_manager) {
        const std::vector<voxel::ChunkKey> unloaded =
            core_engine->streaming_manager->update_stream_focus(
                voxel::Vec3f{p_position.x, p_position.y, p_position.z},
                voxel::Vec3f{p_velocity.x, p_velocity.y, p_velocity.z},
                get_effective_view_distance_chunks(),
                prefetch_distance_chunks);
        for (const voxel::ChunkKey uk : unloaded) {
            server_slot_manager.free_chunk_slot(uk);
            navigation_manager.note_chunk_unloaded(uk);
        }
    }

    // Collision gating follows the focus chunk: only re-evaluate when it crosses a
    // chunk boundary (or after a runtime change to collision_distance_chunks).
    const voxel::ChunkKey focus_ck = voxel::ChunkKey::from_world_sample(
        static_cast<int32_t>(std::floor(p_position.x)),
        static_cast<int32_t>(std::floor(p_position.y)),
        static_cast<int32_t>(std::floor(p_position.z)));
    if (collision_distance_chunks >= 0 &&
        (!collision_focus_valid_ || focus_ck != last_collision_focus_ck_)) {
        collision_focus_valid_ = true;
        last_collision_focus_ck_ = focus_ck;
        _reconcile_collision_range();
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
    voxel::StageTimings* const timings = instrumentation_enabled ? &stage_timings_ : nullptr;

    navigation_manager.poll_server_synchronization();

    std::size_t non_empty_meshes_committed = 0;

    while (true) {
        std::optional<voxel::CompletionRecord> done =
            core_engine->worker_pool->try_pop_completion();
        if (!done.has_value()) {
            break;
        }

        switch (done->kind) {
            case voxel::CompletionKind::ChunkMesh: {
                voxel::ScopedTimer commit_timer(timings, voxel::Stage::Commit);
                const bool has_geometry = !done->chunk_mesh.vertices.empty();
                server_slot_manager.commit_chunk_mesh_and_collision(
                    done->chunk_mesh, chunk_in_collision_range(done->chunk_mesh.key));
                navigation_manager.note_chunk_mesh_committed(
                    done->chunk_mesh.key,
                    done->chunk_mesh.captured_terrain_revision,
                    server_slot_manager.get_cached_nav_faces(done->chunk_mesh.key));
                if (has_geometry) {
                    ++non_empty_meshes_committed;
                }
                break;
            }

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
                const bool is_fill =
                    (done->edit_transaction.operation == voxel::EditOperation::Fill);
#if defined(VOXEL_WITH_GODOT_CPP)
                emit_signal(is_fill ? "terrain_add_completed" : "explosion_completed",
                            pos, done->edit_transaction.radius, affected);
#else
                emitted_signals_.push_back(EmittedSignalRecord{
                    is_fill ? "terrain_add_completed" : "explosion_completed",
                    pos, done->edit_transaction.radius, affected, {}, 0});
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
        // Cap non-empty PhysicsServer3D concave BVH commits to at most 2 per frame when not over cap
        // so Godot's internal PhysicsServer3D sync phase never stalls the main loop!
        if (!currently_over_cap &&
            (elapsed_us >= soft_budget_us || non_empty_meshes_committed >= 2ULL)) {
            break;
        }
        if (elapsed_us >= emergency_budget_us) {
            break;
        }
    }

    // Only submit navigation group bakes after initial streaming settles (core_idle)
    if (core_idle()) {
        navigation_manager.submit_ready_groups_to_server(server_slot_manager);
    }

    for (const voxel::ChunkKey key : navigation_manager.drain_newly_committed_chunks()) {
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("chunk_committed", static_cast<int64_t>(key.raw));
#else
        emitted_signals_.push_back(
            EmittedSignalRecord{"chunk_committed", {}, 0.0f, 0, {}, static_cast<int64_t>(key.raw)});
#endif
    }

    if (timings == nullptr) {
        return;
    }

    const std::string summary = voxel::format_stage_summaries(stage_timings_.drain_all());
    if (!summary.empty()) {
        _log(summary);
    }

    if (startup_armed_ && !startup_reported_ && fully_committed()) {
        startup_reported_ = true;
        const double elapsed_ms =
            static_cast<double>(monotonic_ticks_usec() - startup_begin_us_) / 1000.0;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "initial fill complete in %.1f ms", elapsed_ms);
        _log(buf);
    }
}

} // namespace godot
