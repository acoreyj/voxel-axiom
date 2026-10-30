#include "voxel_terrain_3d.h"

#include "terrain_raycast.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
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

template <typename StrT>
std::string to_std_string(const StrT& s) {
    if constexpr (requires { s.utf8().get_data(); }) {
        return std::string(s.utf8().get_data());
    } else {
        return std::string(s.c_str());
    }
}

/// Resolves `res://` / `user://` asset paths to a filesystem path and falls back
/// to `res://`-relative and plain relative lookups for offline/baked `.vreg` files.
std::string resolve_asset_filepath(const String& p_filepath) {
    std::string raw = to_std_string(p_filepath);
#if defined(VOXEL_WITH_GODOT_CPP)
    if (auto* ps = ProjectSettings::get_singleton()) {
        if (raw.rfind("res://", 0) == 0 || raw.rfind("user://", 0) == 0) {
            return to_std_string(ps->globalize_path(p_filepath));
        }
        if (!std::filesystem::exists(raw)) {
            const String res_path = String("res://") + p_filepath;
            const std::string globalized = to_std_string(ps->globalize_path(res_path));
            if (std::filesystem::exists(globalized)) {
                return globalized;
            }
        }
    }
#endif
    if (raw.rfind("res://", 0) == 0) {
        std::string stripped = raw.substr(6);
        if (std::filesystem::exists(stripped)) {
            return stripped;
        }
    }
    return raw;
}
} // namespace

void VoxelTerrain3D::_bind_methods() {
#if defined(VOXEL_WITH_GODOT_CPP)
    ClassDB::bind_method(D_METHOD("trigger_explosion", "position", "radius"), &VoxelTerrain3D::trigger_explosion);
    ClassDB::bind_method(D_METHOD("trigger_elemental_sphere", "position", "radius", "element_kind"),
                         &VoxelTerrain3D::trigger_elemental_sphere);
    ClassDB::bind_method(D_METHOD("add_terrain", "position", "radius"), &VoxelTerrain3D::add_terrain);
    ClassDB::bind_method(D_METHOD("add_terrain_with_material", "position", "radius", "material"),
                         &VoxelTerrain3D::add_terrain_with_material);
    ClassDB::bind_method(D_METHOD("place_boulder", "position", "radius"), &VoxelTerrain3D::place_boulder);
    ClassDB::bind_method(D_METHOD("carve_capsule", "from", "to", "radius"),
                         &VoxelTerrain3D::carve_capsule);
    ClassDB::bind_method(D_METHOD("add_capsule_with_material", "from", "to", "radius", "material"),
                         &VoxelTerrain3D::add_capsule_with_material);
    ClassDB::bind_method(D_METHOD("trigger_elemental_capsule", "from", "to", "radius", "element_kind"),
                         &VoxelTerrain3D::trigger_elemental_capsule);
    ClassDB::bind_method(D_METHOD("stamp_asset", "vreg_path", "chunk_coords", "recreate_delay_seconds"),
                         &VoxelTerrain3D::stamp_asset, DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("remove_structure", "structure_id"),
                         &VoxelTerrain3D::remove_structure);
    ClassDB::bind_method(D_METHOD("undo"), &VoxelTerrain3D::undo);
    ClassDB::bind_method(D_METHOD("redo"), &VoxelTerrain3D::redo);
    ClassDB::bind_method(D_METHOD("undo_all"), &VoxelTerrain3D::undo_all);
    ClassDB::bind_method(D_METHOD("redo_all"), &VoxelTerrain3D::redo_all);
    ClassDB::bind_method(D_METHOD("clear_undo_history"), &VoxelTerrain3D::clear_undo_history);
    ClassDB::bind_method(D_METHOD("can_undo"), &VoxelTerrain3D::can_undo);
    ClassDB::bind_method(D_METHOD("can_redo"), &VoxelTerrain3D::can_redo);
    ClassDB::bind_method(D_METHOD("get_undo_count"), &VoxelTerrain3D::get_undo_count);
    ClassDB::bind_method(D_METHOD("get_redo_count"), &VoxelTerrain3D::get_redo_count);
    ClassDB::bind_method(D_METHOD("set_max_undo_steps", "steps"), &VoxelTerrain3D::set_max_undo_steps);
    ClassDB::bind_method(D_METHOD("get_max_undo_steps"), &VoxelTerrain3D::get_max_undo_steps);
    ClassDB::bind_method(D_METHOD("raycast_terrain", "from", "to"), &VoxelTerrain3D::raycast_terrain);
    ClassDB::bind_method(D_METHOD("raycast_terrain_material", "from", "to"),
                         &VoxelTerrain3D::raycast_terrain_material);
    ClassDB::bind_method(D_METHOD("raycast_terrain_normal", "from", "to"),
                         &VoxelTerrain3D::raycast_terrain_normal);
    ClassDB::bind_method(D_METHOD("sweep_sphere", "from", "to", "radius"),
                         &VoxelTerrain3D::sweep_sphere);
    ClassDB::bind_method(D_METHOD("measure_wall_thickness", "impact_pos", "direction", "max_thickness"),
                         &VoxelTerrain3D::measure_wall_thickness);
    ClassDB::bind_method(D_METHOD("sample_sdf", "world_pos"), &VoxelTerrain3D::sample_sdf);
    ClassDB::bind_method(D_METHOD("sample_sdf_normal", "world_pos"), &VoxelTerrain3D::sample_sdf_normal);
    ClassDB::bind_method(D_METHOD("get_material_at", "world_pos"), &VoxelTerrain3D::get_material_at);
    ClassDB::bind_method(D_METHOD("get_damage_at", "world_pos"), &VoxelTerrain3D::get_damage_at);
    ClassDB::bind_method(D_METHOD("query_sphere_materials", "center", "radius"),
                         &VoxelTerrain3D::query_sphere_materials);
    ClassDB::bind_method(D_METHOD("get_dominant_material_in_sphere", "center", "radius"),
                         &VoxelTerrain3D::get_dominant_material_in_sphere);
    ClassDB::bind_method(D_METHOD("get_surface_height", "x", "z"), &VoxelTerrain3D::get_surface_height);
    ClassDB::bind_method(D_METHOD("save_world"), &VoxelTerrain3D::save_world);
    ClassDB::bind_method(D_METHOD("set_save_directory_path", "path"), &VoxelTerrain3D::set_save_directory_path);
    ClassDB::bind_method(D_METHOD("get_save_directory_path"), &VoxelTerrain3D::get_save_directory_path);
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
    ClassDB::bind_method(D_METHOD("get_voxel_scale"), &VoxelTerrain3D::get_voxel_scale);
    ClassDB::bind_method(D_METHOD("set_voxel_scale", "scale"), &VoxelTerrain3D::set_voxel_scale);
    ClassDB::bind_method(D_METHOD("get_surface_base_height"), &VoxelTerrain3D::get_surface_base_height);
    ClassDB::bind_method(D_METHOD("set_surface_base_height", "height"), &VoxelTerrain3D::set_surface_base_height);
    ClassDB::bind_method(D_METHOD("get_world_seed"), &VoxelTerrain3D::get_world_seed);
    ClassDB::bind_method(D_METHOD("set_world_seed", "seed"), &VoxelTerrain3D::set_world_seed);
    ClassDB::bind_method(D_METHOD("get_generate_caves"), &VoxelTerrain3D::get_generate_caves);
    ClassDB::bind_method(D_METHOD("set_generate_caves", "enabled"), &VoxelTerrain3D::set_generate_caves);
    ClassDB::bind_method(D_METHOD("get_agent_max_slope"), &VoxelTerrain3D::get_agent_max_slope);
    ClassDB::bind_method(D_METHOD("set_agent_max_slope", "degrees"), &VoxelTerrain3D::set_agent_max_slope);
    ClassDB::bind_method(D_METHOD("get_agent_max_climb"), &VoxelTerrain3D::get_agent_max_climb);
    ClassDB::bind_method(D_METHOD("set_agent_max_climb", "meters"), &VoxelTerrain3D::set_agent_max_climb);
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
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voxel_scale", PROPERTY_HINT_RANGE, "0.125,4.0,0.05"),
                 "set_voxel_scale", "get_voxel_scale");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_base_height", PROPERTY_HINT_RANGE, "16.0,256.0,1.0"),
                 "set_surface_base_height", "get_surface_base_height");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "world_seed", PROPERTY_HINT_RANGE, "0,4294967295,1"),
                 "set_world_seed", "get_world_seed");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"),
                 "set_terrain_material", "get_terrain_material");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "generate_caves"),
                 "set_generate_caves", "get_generate_caves");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "agent_max_slope", PROPERTY_HINT_RANGE, "1.0,89.0,0.5"),
                 "set_agent_max_slope", "get_agent_max_slope");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "agent_max_climb", PROPERTY_HINT_RANGE, "0.05,4.0,0.05"),
                 "set_agent_max_climb", "get_agent_max_climb");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "instrumentation_enabled"),
                 "set_instrumentation_enabled", "get_instrumentation_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "max_undo_steps", PROPERTY_HINT_RANGE, "1,512,1"),
                 "set_max_undo_steps", "get_max_undo_steps");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "save_directory_path", PROPERTY_HINT_GLOBAL_DIR),
                 "set_save_directory_path", "get_save_directory_path");

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
    ADD_SIGNAL(MethodInfo("structure_damaged",
        PropertyInfo(Variant::INT, "structure_id"),
        PropertyInfo(Variant::STRING, "name")));
    ADD_SIGNAL(MethodInfo("structure_recreated",
        PropertyInfo(Variant::INT, "structure_id"),
        PropertyInfo(Variant::STRING, "name")));
    ADD_SIGNAL(MethodInfo("structure_removed",
        PropertyInfo(Variant::INT, "structure_id"),
        PropertyInfo(Variant::STRING, "name")));
    ADD_SIGNAL(MethodInfo("undo_completed",
        PropertyInfo(Variant::INT, "restored_chunks"),
        PropertyInfo(Variant::INT, "remaining_undo"),
        PropertyInfo(Variant::INT, "available_redo")));
    ADD_SIGNAL(MethodInfo("redo_completed",
        PropertyInfo(Variant::INT, "restored_chunks"),
        PropertyInfo(Variant::INT, "available_undo"),
        PropertyInfo(Variant::INT, "remaining_redo")));
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

void VoxelTerrain3D::set_max_undo_steps(int p_steps) {
    max_undo_steps = std::max(1, p_steps);
    undo_redo_manager_.set_max_steps(static_cast<std::size_t>(max_undo_steps));
}

int VoxelTerrain3D::get_max_undo_steps() const {
    return max_undo_steps;
}

void VoxelTerrain3D::set_voxel_scale(float p_scale) {
    const float clamped = std::clamp(p_scale, 0.125f, 4.0f);
    if (core_engine) {
        if (std::abs(clamped - voxel_scale) > 1e-6f) {
            _log("voxel_scale can only be changed before the engine starts; "
                 "reload the scene to apply a different voxel resolution");
        }
        return;
    }
    voxel_scale = clamped;
}

void VoxelTerrain3D::set_surface_base_height(float p_height) {
    surface_base_height = p_height;
    if (core_engine) {
        core_engine->procgen.surface_base_height = p_height;
    }
}

void VoxelTerrain3D::set_world_seed(int p_seed) {
    world_seed = p_seed;
    if (core_engine) {
        core_engine->procgen.seed = static_cast<uint32_t>(p_seed);
    }
}

void VoxelTerrain3D::set_generate_caves(bool p_enabled) {
    generate_caves = p_enabled;
    if (core_engine) {
        core_engine->procgen.caves_enabled = p_enabled;
    }
}

void VoxelTerrain3D::set_agent_max_slope(float p_degrees) {
    agent_max_slope = std::clamp(p_degrees, 1.0f, 89.0f);
    if (core_engine) {
        navigation_manager.set_agent_limits(agent_max_slope, agent_max_climb);
    }
}

void VoxelTerrain3D::set_agent_max_climb(float p_meters) {
    agent_max_climb = std::clamp(p_meters, 0.05f, 4.0f);
    if (core_engine) {
        navigation_manager.set_agent_limits(agent_max_slope, agent_max_climb);
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
    const float inv_scale = 1.0f / voxel_scale;
    const voxel::ChunkKey focus = voxel::ChunkKey::from_world_sample(
        static_cast<int32_t>(std::floor(stream_focus_position.x * inv_scale)),
        static_cast<int32_t>(std::floor(stream_focus_position.y * inv_scale)),
        static_cast<int32_t>(std::floor(stream_focus_position.z * inv_scale)));
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
        voxel_scale,
        &collision_subsample_stride);
    core_engine->procgen.caves_enabled = generate_caves;
    core_engine->procgen.surface_base_height = surface_base_height;
    core_engine->procgen.seed = static_cast<uint32_t>(world_seed);

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

    voxel::CaveNavMeshConfig nav_cfg{};
    nav_cfg.voxel_scale = voxel_scale;
    nav_cfg.agent_max_slope = agent_max_slope;
    nav_cfg.agent_max_climb = agent_max_climb;
    navigation_manager.initialize(nav_map_rid, nav_cfg);
    server_slot_manager.initialize(scenario_rid, space_rid);

    stamp_manager_.set_structure_damaged_callback(
        [this](uint32_t p_structure_id, const std::string& p_name) {
#if defined(VOXEL_WITH_GODOT_CPP)
            emit_signal("structure_damaged", static_cast<int64_t>(p_structure_id),
                        String(p_name.c_str()));
#else
            EmittedSignalRecord record{"structure_damaged", {}, 0.0f, 0, {}, 0};
            record.structure_id = static_cast<int64_t>(p_structure_id);
            record.structure_name = p_name;
            emitted_signals_.push_back(std::move(record));
#endif
        });
    stamp_manager_.set_structure_recreated_callback(
        [this](uint32_t p_structure_id, const std::string& p_name) {
#if defined(VOXEL_WITH_GODOT_CPP)
            emit_signal("structure_recreated", static_cast<int64_t>(p_structure_id),
                        String(p_name.c_str()));
#else
            EmittedSignalRecord record{"structure_recreated", {}, 0.0f, 0, {}, 0};
            record.structure_id = static_cast<int64_t>(p_structure_id);
            record.structure_name = p_name;
            emitted_signals_.push_back(std::move(record));
#endif
        });
    stamp_manager_.set_structure_removed_callback(
        [this](uint32_t p_structure_id, const std::string& p_name) {
#if defined(VOXEL_WITH_GODOT_CPP)
            emit_signal("structure_removed", static_cast<int64_t>(p_structure_id),
                        String(p_name.c_str()));
#else
            EmittedSignalRecord record{"structure_removed", {}, 0.0f, 0, {}, 0};
            record.structure_id = static_cast<int64_t>(p_structure_id);
            record.structure_name = p_name;
            emitted_signals_.push_back(std::move(record));
#endif
        });
}

void VoxelTerrain3D::_exit_tree() {
    if (core_engine) {
        core_engine->worker_pool->shutdown(save_on_exit);
        core_engine.reset();
    }
    navigation_manager.shutdown();
    server_slot_manager.free_all_rids();
}

void VoxelTerrain3D::handle_completed_edit_transaction(voxel::EditTransactionCompletedPayload& tx) {
    if (!core_engine) {
        return;
    }

    auto stamps_before = stamp_manager_.capture_manager_snapshot();
    server_slot_manager.register_edit_batch(tx.terrain_revision, tx.dirtied_chunks);
    navigation_manager.apply_edit_transaction(tx);
    // Explosions / elemental reactions that carve stamped structures mark
    // them damaged and arm their reconstitution timer; fills never do.
    if (!tx.is_fill) {
        for (const voxel::ChunkKey damaged_key : tx.dirtied_chunks) {
            stamp_manager_.notify_chunk_damaged(damaged_key);
        }
    }
    auto stamps_after = stamp_manager_.capture_manager_snapshot();

    if (!tx.pre_edit_snapshots.empty() || !tx.post_edit_snapshots.empty()) {
        voxel::UndoRedoEntry entry{};
        entry.action_name = tx.is_fill ? "add_terrain" : "explosion";
        entry.before_chunks = std::move(tx.pre_edit_snapshots);
        entry.after_chunks = std::move(tx.post_edit_snapshots);
        entry.stamps_before = std::move(stamps_before);
        entry.stamps_after = std::move(stamps_after);
        undo_redo_manager_.record_entry(std::move(entry));
    }

    // Payload spheres are in canonical grid-sample space; signals are world-space.
    const Vector3 pos(tx.position.x * voxel_scale,
                      tx.position.y * voxel_scale,
                      tx.position.z * voxel_scale);
    const float world_radius = tx.radius * voxel_scale;
    const int affected = static_cast<int>(tx.total_affected_chunks);
    const bool is_fill = tx.is_fill;
#if defined(VOXEL_WITH_GODOT_CPP)
    emit_signal(is_fill ? "terrain_add_completed" : "explosion_completed",
                pos, world_radius, affected);
#else
    emitted_signals_.push_back(EmittedSignalRecord{
        is_fill ? "terrain_add_completed" : "explosion_completed",
        pos, world_radius, affected, {}, 0});
#endif
}

void VoxelTerrain3D::drain_pending_edits_for_undo_redo() {
    if (!core_engine) {
        return;
    }
    core_engine->worker_pool->wait_until_edits_idle();
    core_engine->worker_pool->drain_edit_completions(
        [this](voxel::EditTransactionCompletedPayload& tx) {
            handle_completed_edit_transaction(tx);
        });
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

    // Edit spheres are evaluated in canonical grid-sample space; convert the
    // world-space center and radius into voxel samples at the API boundary.
    const float inv_scale = 1.0f / voxel_scale;
    const voxel::Vec3f native_pos{
        p_position.x * inv_scale, p_position.y * inv_scale, p_position.z * inv_scale};
    const float grid_radius = p_radius * inv_scale;
    if (core_engine->worker_pool->enqueue_explosion(native_pos, grid_radius)) {
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

void VoxelTerrain3D::trigger_elemental_sphere(Vector3 p_position, float p_radius, int p_element_kind) {
    if (p_element_kind <= 0) {
        trigger_explosion(p_position, p_radius);
        return;
    }

    if (!core_engine || p_radius <= 0.0f) {
        const std::string reason = "engine not ready or radius is non-positive";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("explosion_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"explosion_failed", {}, 0.0f, 0, reason, 0});
#endif
        return;
    }

    // Bananza-style elemental trigger volumes: 1 = Water, 2 = Frost, 3 = Lava.
    const int clamped_kind = std::clamp(p_element_kind, 0, 3);
    const auto elem_kind = static_cast<voxel::ElementalKind>(clamped_kind);
    const float inv_scale = 1.0f / voxel_scale;
    const voxel::Vec3f native_pos{
        p_position.x * inv_scale, p_position.y * inv_scale, p_position.z * inv_scale};
    const float grid_radius = p_radius * inv_scale;

    if (core_engine->worker_pool->enqueue_elemental_sphere(native_pos, grid_radius, elem_kind)) {
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
    _enqueue_fill(p_position, p_radius, voxel::MaterialHardness::kMaterialSoil);
}

void VoxelTerrain3D::add_terrain_with_material(Vector3 p_position, float p_radius, int p_material) {
    const uint8_t material = static_cast<uint8_t>(std::clamp(p_material, 0, 255));
    _enqueue_fill(p_position, p_radius, material);
}

void VoxelTerrain3D::place_boulder(Vector3 p_position, float p_radius) {
    _enqueue_fill(p_position, p_radius, voxel::MaterialHardness::kMaterialBoulder);
}

void VoxelTerrain3D::carve_capsule(Vector3 p_from, Vector3 p_to, float p_radius) {
    trigger_elemental_capsule(p_from, p_to, p_radius, static_cast<int>(voxel::ElementalKind::None));
}

void VoxelTerrain3D::add_capsule_with_material(Vector3 p_from, Vector3 p_to, float p_radius, int p_material) {
    if (!core_engine || p_radius <= 0.0f) {
        const std::string reason = "engine not ready or radius is non-positive";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("terrain_add_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"terrain_add_failed", {}, 0.0f, 0, reason, 0});
#endif
        return;
    }

    const uint8_t material = static_cast<uint8_t>(std::clamp(p_material, 1, 7));
    const float inv_scale = 1.0f / voxel_scale;
    const voxel::Vec3f native_from{
        p_from.x * inv_scale, p_from.y * inv_scale, p_from.z * inv_scale};
    const voxel::Vec3f native_to{
        p_to.x * inv_scale, p_to.y * inv_scale, p_to.z * inv_scale};
    const float grid_radius = p_radius * inv_scale;

    if (core_engine->worker_pool->enqueue_capsule_fill(native_from, native_to, grid_radius, material)) {
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("terrain_add_requested", p_from, p_radius);
#else
        emitted_signals_.push_back(
            EmittedSignalRecord{"terrain_add_requested", p_from, p_radius, 0, {}, 0});
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

void VoxelTerrain3D::trigger_elemental_capsule(Vector3 p_from, Vector3 p_to, float p_radius, int p_element_kind) {
    if (!core_engine || p_radius <= 0.0f) {
        const std::string reason = "engine not ready or radius is non-positive";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("explosion_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"explosion_failed", {}, 0.0f, 0, reason, 0});
#endif
        return;
    }

    const int clamped_kind = std::clamp(p_element_kind, 0, 3);
    const auto elem_kind = static_cast<voxel::ElementalKind>(clamped_kind);
    const float inv_scale = 1.0f / voxel_scale;
    const voxel::Vec3f native_from{
        p_from.x * inv_scale, p_from.y * inv_scale, p_from.z * inv_scale};
    const voxel::Vec3f native_to{
        p_to.x * inv_scale, p_to.y * inv_scale, p_to.z * inv_scale};
    const float grid_radius = p_radius * inv_scale;

    if (core_engine->worker_pool->enqueue_elemental_capsule(native_from, native_to, grid_radius, elem_kind)) {
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("explosion_requested", p_from, p_radius);
#else
        emitted_signals_.push_back(
            EmittedSignalRecord{"explosion_requested", p_from, p_radius, 0, {}, 0});
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

int64_t VoxelTerrain3D::stamp_asset(const String& p_vreg_path, Vector3 p_chunk_coords, float p_recreate_delay_seconds) {
    if (!core_engine) {
        return 0;
    }
    drain_pending_edits_for_undo_redo();
    const std::string vreg_path = resolve_asset_filepath(p_vreg_path);
    const voxel::ChunkKey origin = voxel::ChunkKey::from_coords(
        static_cast<int32_t>(std::llround(p_chunk_coords.x)),
        static_cast<int32_t>(std::llround(p_chunk_coords.y)),
        static_cast<int32_t>(std::llround(p_chunk_coords.z)));

    auto stamps_before = stamp_manager_.capture_manager_snapshot();
    std::unordered_map<voxel::ChunkKey, voxel::ChunkStateSnapshot, voxel::ChunkKeyHash> before_chunks;
    std::unordered_map<voxel::ChunkKey, voxel::ChunkStateSnapshot, voxel::ChunkKeyHash> after_chunks;

    const uint32_t structure_id = stamp_manager_.stamp_region_file(
        core_engine->store,
        core_engine->dirty_scheduler,
        navigation_manager,
        vreg_path,
        origin,
        p_recreate_delay_seconds,
        voxel::StampBlendMode::Union,
        vreg_path,
        [this](voxel::ChunkStore& s, voxel::ChunkKey ck) {
            voxel::WorkerDensityGrid34 grid34(core_engine->ledger);
            (void)core_engine->save_manager->load_or_generate_chunk(
                s, ck, core_engine->procgen, grid34);
        },
        &before_chunks,
        &after_chunks,
        &server_slot_manager);

    if (structure_id != 0) {
        if (!before_chunks.empty() || !after_chunks.empty()) {
            voxel::UndoRedoEntry entry{};
            entry.action_name = "stamp_asset";
            entry.before_chunks = std::move(before_chunks);
            entry.after_chunks = std::move(after_chunks);
            entry.stamps_before = std::move(stamps_before);
            entry.stamps_after = stamp_manager_.capture_manager_snapshot();
            undo_redo_manager_.record_entry(std::move(entry));
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "stamped asset '%s' at chunk (%d, %d, %d) structure_id=%u delay=%.1fs",
                      vreg_path.c_str(), origin.x(), origin.y(), origin.z(),
                      structure_id, static_cast<double>(p_recreate_delay_seconds));
        _log(buf);
    }
    return static_cast<int64_t>(structure_id);
}

bool VoxelTerrain3D::remove_structure(int64_t p_structure_id) {
    if (!core_engine || p_structure_id <= 0) {
        return false;
    }
    drain_pending_edits_for_undo_redo();

    auto stamps_before = stamp_manager_.capture_manager_snapshot();
    std::unordered_map<voxel::ChunkKey, voxel::ChunkStateSnapshot, voxel::ChunkKeyHash> before_chunks;
    std::unordered_map<voxel::ChunkKey, voxel::ChunkStateSnapshot, voxel::ChunkKeyHash> after_chunks;

    const bool removed = stamp_manager_.remove_structure(
        static_cast<uint32_t>(p_structure_id),
        core_engine->store,
        core_engine->dirty_scheduler,
        navigation_manager,
        &before_chunks,
        &after_chunks,
        &server_slot_manager);
    if (removed) {
        if (!before_chunks.empty() || !after_chunks.empty()) {
            voxel::UndoRedoEntry entry{};
            entry.action_name = "remove_structure";
            entry.before_chunks = std::move(before_chunks);
            entry.after_chunks = std::move(after_chunks);
            entry.stamps_before = std::move(stamps_before);
            entry.stamps_after = stamp_manager_.capture_manager_snapshot();
            undo_redo_manager_.record_entry(std::move(entry));
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "removed structure_id=%lld",
                      static_cast<long long>(p_structure_id));
        _log(buf);
    }
    return removed;
}

bool VoxelTerrain3D::undo() {
    if (!core_engine) {
        return false;
    }
    drain_pending_edits_for_undo_redo();

    std::size_t affected_chunks = 0;
    const bool ok = undo_redo_manager_.undo(
        core_engine->store,
        core_engine->dirty_scheduler,
        navigation_manager,
        stamp_manager_,
        core_engine->save_manager.get(),
        &affected_chunks,
        &server_slot_manager);

    if (ok) {
        const int u_cnt = get_undo_count();
        const int r_cnt = get_redo_count();
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("undo_completed", static_cast<int>(affected_chunks), u_cnt, r_cnt);
#else
        emitted_signals_.push_back(EmittedSignalRecord{
            "undo_completed", {}, 0.0f, static_cast<int>(affected_chunks), {}, 0, 0, {}, u_cnt, r_cnt});
#endif
    }
    return ok;
}

bool VoxelTerrain3D::redo() {
    if (!core_engine) {
        return false;
    }
    drain_pending_edits_for_undo_redo();

    std::size_t affected_chunks = 0;
    const bool ok = undo_redo_manager_.redo(
        core_engine->store,
        core_engine->dirty_scheduler,
        navigation_manager,
        stamp_manager_,
        core_engine->save_manager.get(),
        &affected_chunks,
        &server_slot_manager);

    if (ok) {
        const int u_cnt = get_undo_count();
        const int r_cnt = get_redo_count();
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("redo_completed", static_cast<int>(affected_chunks), u_cnt, r_cnt);
#else
        emitted_signals_.push_back(EmittedSignalRecord{
            "redo_completed", {}, 0.0f, static_cast<int>(affected_chunks), {}, 0, 0, {}, u_cnt, r_cnt});
#endif
    }
    return ok;
}

int VoxelTerrain3D::undo_all() {
    if (!core_engine) {
        return 0;
    }
    drain_pending_edits_for_undo_redo();

    std::size_t affected_chunks = 0;
    const std::size_t steps = undo_redo_manager_.undo_all(
        core_engine->store,
        core_engine->dirty_scheduler,
        navigation_manager,
        stamp_manager_,
        core_engine->save_manager.get(),
        &affected_chunks,
        &server_slot_manager);

    if (steps > 0) {
        const int u_cnt = get_undo_count();
        const int r_cnt = get_redo_count();
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("undo_completed", static_cast<int>(affected_chunks), u_cnt, r_cnt);
#else
        emitted_signals_.push_back(EmittedSignalRecord{
            "undo_completed", {}, 0.0f, static_cast<int>(affected_chunks), {}, 0, 0, {}, u_cnt, r_cnt});
#endif
    }
    return static_cast<int>(steps);
}

int VoxelTerrain3D::redo_all() {
    if (!core_engine) {
        return 0;
    }
    drain_pending_edits_for_undo_redo();

    std::size_t affected_chunks = 0;
    const std::size_t steps = undo_redo_manager_.redo_all(
        core_engine->store,
        core_engine->dirty_scheduler,
        navigation_manager,
        stamp_manager_,
        core_engine->save_manager.get(),
        &affected_chunks,
        &server_slot_manager);

    if (steps > 0) {
        const int u_cnt = get_undo_count();
        const int r_cnt = get_redo_count();
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("redo_completed", static_cast<int>(affected_chunks), u_cnt, r_cnt);
#else
        emitted_signals_.push_back(EmittedSignalRecord{
            "redo_completed", {}, 0.0f, static_cast<int>(affected_chunks), {}, 0, 0, {}, u_cnt, r_cnt});
#endif
    }
    return static_cast<int>(steps);
}

void VoxelTerrain3D::clear_undo_history() {
    if (!core_engine) {
        return;
    }
    drain_pending_edits_for_undo_redo();
    undo_redo_manager_.clear();
}

bool VoxelTerrain3D::can_undo() const {
    return undo_redo_manager_.can_undo();
}

bool VoxelTerrain3D::can_redo() const {
    return undo_redo_manager_.can_redo();
}

int VoxelTerrain3D::get_undo_count() const {
    return static_cast<int>(undo_redo_manager_.undo_count());
}

int VoxelTerrain3D::get_redo_count() const {
    return static_cast<int>(undo_redo_manager_.redo_count());
}

void VoxelTerrain3D::_enqueue_fill(Vector3 p_position, float p_radius, uint8_t p_material) {
    if (!core_engine || p_radius <= 0.0f) {
        const std::string reason = "engine not ready or radius is non-positive";
#if defined(VOXEL_WITH_GODOT_CPP)
        emit_signal("terrain_add_failed", String(reason.c_str()));
#else
        emitted_signals_.push_back(EmittedSignalRecord{"terrain_add_failed", {}, 0.0f, 0, reason, 0});
#endif
        return;
    }

    const float inv_scale = 1.0f / voxel_scale;
    const voxel::Vec3f native_pos{
        p_position.x * inv_scale, p_position.y * inv_scale, p_position.z * inv_scale};
    const float grid_radius = p_radius * inv_scale;
    if (core_engine->worker_pool->enqueue_fill_spheres(native_pos, grid_radius, p_material)) {
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

Vector3 VoxelTerrain3D::raycast_terrain(Vector3 p_from, Vector3 p_to) const {
    constexpr float kInf = std::numeric_limits<float>::infinity();
    if (!core_engine) {
        return Vector3(kInf, kInf, kInf);
    }
    const std::optional<voxel::Vec3f> hit = voxel::raycast_terrain(
        core_engine->store,
        voxel::Vec3f{p_from.x, p_from.y, p_from.z},
        voxel::Vec3f{p_to.x, p_to.y, p_to.z},
        0.5f,
        voxel_scale);
    if (!hit.has_value()) {
        return Vector3(kInf, kInf, kInf);
    }
    return Vector3(hit->x, hit->y, hit->z);
}

int VoxelTerrain3D::raycast_terrain_material(Vector3 p_from, Vector3 p_to) const {
    if (!core_engine) {
        return -1;
    }
    const std::optional<voxel::TerrainMaterialHit> hit = voxel::raycast_terrain_material(
        core_engine->store,
        voxel::Vec3f{p_from.x, p_from.y, p_from.z},
        voxel::Vec3f{p_to.x, p_to.y, p_to.z},
        0.5f,
        voxel_scale);
    if (!hit.has_value()) {
        return -1;
    }
    return static_cast<int>(hit->material_id);
}

Vector3 VoxelTerrain3D::raycast_terrain_normal(Vector3 p_from, Vector3 p_to) const {
    const Vector3 hit = raycast_terrain(p_from, p_to);
    if (!std::isfinite(hit.x)) {
        return hit;
    }
    return sample_sdf_normal(hit);
}

Vector3 VoxelTerrain3D::sweep_sphere(Vector3 p_from, Vector3 p_to, float p_radius) const {
    const float inf = std::numeric_limits<float>::infinity();
    if (!core_engine) {
        return Vector3(inf, inf, inf);
    }
    const float r = std::max(0.0f, p_radius);
    if (r <= 1e-4f) {
        return raycast_terrain(p_from, p_to);
    }

    const float dx = p_to.x - p_from.x;
    const float dy = p_to.y - p_from.y;
    const float dz = p_to.z - p_from.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 1e-5f) {
        return Vector3(inf, inf, inf);
    }

    const float s = (voxel_scale > 0.0f) ? voxel_scale : 1.0f;
    const float step = 0.35f * s;
    const int steps = std::max(1, static_cast<int>(std::ceil(len / step)));

    float prev_clearance = sample_sdf(p_from) - r;
    float prev_t = 0.0f;

    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const Vector3 cur_center(p_from.x + dx * t, p_from.y + dy * t, p_from.z + dz * t);
        const float cur_clearance = sample_sdf(cur_center) - r;
        if (cur_clearance <= 0.0f) {
            const float denom = prev_clearance - cur_clearance;
            const float frac = (denom > 1e-6f) ? std::clamp(prev_clearance / denom, 0.0f, 1.0f) : 0.5f;
            const float hit_t = (prev_clearance <= 0.0f) ? 0.0f : (prev_t + frac * (t - prev_t));
            const Vector3 hit_center(p_from.x + dx * hit_t, p_from.y + dy * hit_t, p_from.z + dz * hit_t);
            const Vector3 surf_n = sample_sdf_normal(hit_center);
            const float center_sdf = std::max(0.0f, sample_sdf(hit_center));
            return Vector3(
                hit_center.x - surf_n.x * center_sdf,
                hit_center.y - surf_n.y * center_sdf,
                hit_center.z - surf_n.z * center_sdf);
        }
        prev_clearance = cur_clearance;
        prev_t = t;
    }
    return Vector3(inf, inf, inf);
}

float VoxelTerrain3D::measure_wall_thickness(Vector3 p_impact_pos, Vector3 p_direction,
                                             float p_max_thickness) const {
    if (!core_engine || p_max_thickness <= 0.0f) {
        return 0.0f;
    }
    const float dir_len = std::sqrt(
        p_direction.x * p_direction.x +
        p_direction.y * p_direction.y +
        p_direction.z * p_direction.z);
    if (dir_len <= 1e-5f) {
        return p_max_thickness + 1.0f;
    }
    const float nx = p_direction.x / dir_len;
    const float ny = p_direction.y / dir_len;
    const float nz = p_direction.z / dir_len;

    const float s = (voxel_scale > 0.0f) ? voxel_scale : 1.0f;
    const float step = 0.25f * s;
    const int max_steps = std::max(2, static_cast<int>(std::ceil(p_max_thickness / step)));

    bool entered_solid = false;
    float prev_d = sample_sdf(p_impact_pos);
    if (prev_d <= 0.0f) {
        entered_solid = true;
    }
    float prev_dist = 0.0f;

    for (int i = 1; i <= max_steps; ++i) {
        const float dist = std::min(p_max_thickness, static_cast<float>(i) * step);
        const Vector3 pt(
            p_impact_pos.x + nx * dist,
            p_impact_pos.y + ny * dist,
            p_impact_pos.z + nz * dist);
        const float cur_d = sample_sdf(pt);
        if (cur_d <= 0.0f) {
            entered_solid = true;
        } else if (entered_solid && cur_d > 0.0f) {
            const float denom = cur_d - prev_d;
            const float frac = (denom > 1e-6f) ? std::clamp(-prev_d / denom, 0.0f, 1.0f) : 0.5f;
            return prev_dist + frac * (dist - prev_dist);
        }
        prev_d = cur_d;
        prev_dist = dist;
    }
    return entered_solid ? (p_max_thickness + 1.0f) : 0.0f;
}

float VoxelTerrain3D::sample_sdf(Vector3 p_world_pos) const {
    if (!core_engine) {
        return std::numeric_limits<float>::infinity();
    }
    const float s = (voxel_scale > 0.0f) ? voxel_scale : 1.0f;
    const float inv_s = 1.0f / s;
    const float gx = p_world_pos.x * inv_s;
    const float gy = p_world_pos.y * inv_s;
    const float gz = p_world_pos.z * inv_s;

    const int32_t ix = static_cast<int32_t>(std::floor(gx));
    const int32_t iy = static_cast<int32_t>(std::floor(gy));
    const int32_t iz = static_cast<int32_t>(std::floor(gz));

    const float fx = gx - static_cast<float>(ix);
    const float fy = gy - static_cast<float>(iy);
    const float fz = gz - static_cast<float>(iz);

    auto sample_lattice = [&](int32_t sx, int32_t sy, int32_t sz) -> float {
        const auto mapped = voxel::map_world_sample(sx, sy, sz);
        if (const voxel::ChunkSlot* slot = core_engine->store.find_slot(mapped.chunk)) {
            std::shared_lock<std::shared_mutex> rl(slot->mutex);
            if (slot->state == voxel::ChunkLifecycleState::Resident) {
                return voxel::TsdfFormat::decode(slot->get_tsdf(mapped.local));
            }
        }
        // Fall back to the procedural field for unloaded chunks. The generator
        // returns world meters, so divide by the voxel size to keep every lattice
        // sample in canonical voxel units; the final interpolation result is
        // converted back to world meters below.
        return core_engine->procgen.evaluate_composite_density(
                   static_cast<float>(sx) * s,
                   static_cast<float>(sy) * s,
                   static_cast<float>(sz) * s) / s;
    };

    const float c000 = sample_lattice(ix,     iy,     iz);
    const float c100 = sample_lattice(ix + 1, iy,     iz);
    const float c010 = sample_lattice(ix,     iy + 1, iz);
    const float c110 = sample_lattice(ix + 1, iy + 1, iz);
    const float c001 = sample_lattice(ix,     iy,     iz + 1);
    const float c101 = sample_lattice(ix + 1, iy,     iz + 1);
    const float c011 = sample_lattice(ix,     iy + 1, iz + 1);
    const float c111 = sample_lattice(ix + 1, iy + 1, iz + 1);

    const float c00 = c000 + fx * (c100 - c000);
    const float c10 = c010 + fx * (c110 - c010);
    const float c01 = c001 + fx * (c101 - c001);
    const float c11 = c011 + fx * (c111 - c011);

    const float c0 = c00 + fy * (c10 - c00);
    const float c1 = c01 + fy * (c11 - c01);

    return (c0 + fz * (c1 - c0)) * s;
}

Vector3 VoxelTerrain3D::sample_sdf_normal(Vector3 p_world_pos) const {
    if (!core_engine) {
        return Vector3(0.0f, 1.0f, 0.0f);
    }
    const float s = (voxel_scale > 0.0f) ? voxel_scale : 1.0f;
    const float h = 0.5f * s;

    float gx = sample_sdf(Vector3(p_world_pos.x + h, p_world_pos.y, p_world_pos.z)) -
               sample_sdf(Vector3(p_world_pos.x - h, p_world_pos.y, p_world_pos.z));
    float gy = sample_sdf(Vector3(p_world_pos.x, p_world_pos.y + h, p_world_pos.z)) -
               sample_sdf(Vector3(p_world_pos.x, p_world_pos.y - h, p_world_pos.z));
    float gz = sample_sdf(Vector3(p_world_pos.x, p_world_pos.y, p_world_pos.z + h)) -
               sample_sdf(Vector3(p_world_pos.x, p_world_pos.y, p_world_pos.z - h));

    float len_sq = gx * gx + gy * gy + gz * gz;
    if (len_sq <= 1e-10f) {
        const float hw = 2.0f * s;
        gx = sample_sdf(Vector3(p_world_pos.x + hw, p_world_pos.y, p_world_pos.z)) -
             sample_sdf(Vector3(p_world_pos.x - hw, p_world_pos.y, p_world_pos.z));
        gy = sample_sdf(Vector3(p_world_pos.x, p_world_pos.y + hw, p_world_pos.z)) -
             sample_sdf(Vector3(p_world_pos.x, p_world_pos.y - hw, p_world_pos.z));
        gz = sample_sdf(Vector3(p_world_pos.x, p_world_pos.y, p_world_pos.z + hw)) -
             sample_sdf(Vector3(p_world_pos.x, p_world_pos.y, p_world_pos.z - hw));
        len_sq = gx * gx + gy * gy + gz * gz;
    }

    if (len_sq <= 1e-10f || !std::isfinite(len_sq)) {
        return Vector3(0.0f, 1.0f, 0.0f);
    }
    const float inv_len = 1.0f / std::sqrt(len_sq);
    return Vector3(gx * inv_len, gy * inv_len, gz * inv_len);
}

int VoxelTerrain3D::get_material_at(Vector3 p_world_pos) const {
    if (!core_engine) {
        return -1;
    }
    const float s = (voxel_scale > 0.0f) ? voxel_scale : 1.0f;
    const float inv_s = 1.0f / s;
    const float gx = p_world_pos.x * inv_s;
    const float gy = p_world_pos.y * inv_s;
    const float gz = p_world_pos.z * inv_s;

    const int32_t ix = static_cast<int32_t>(std::floor(gx));
    const int32_t iy = static_cast<int32_t>(std::floor(gy));
    const int32_t iz = static_cast<int32_t>(std::floor(gz));

    auto fetch_sample = [&](int32_t sx, int32_t sy, int32_t sz, int16_t* out_q, uint8_t* out_mat) -> bool {
        const auto mapped = voxel::map_world_sample(sx, sy, sz);
        if (const voxel::ChunkSlot* slot = core_engine->store.find_slot(mapped.chunk)) {
            std::shared_lock<std::shared_mutex> rl(slot->mutex);
            if (slot->state == voxel::ChunkLifecycleState::Resident) {
                *out_q = slot->get_tsdf(mapped.local);
                *out_mat = slot->get_material(mapped.local);
                return true;
            }
        }
        return false;
    };

    int16_t q0 = 0;
    uint8_t m0 = voxel::MaterialHardness::kMaterialAir;
    const bool resident0 = fetch_sample(ix, iy, iz, &q0, &m0);
    if (resident0 && q0 < 0 && m0 != voxel::MaterialHardness::kMaterialAir) {
        return static_cast<int>(m0);
    }

    // If p_world_pos is right on the d=0 zero-crossing skin (e.g. a raycast hit point
    // or character foot), inspect the 1-voxel neighborhood and pick the closest solid sample.
    float best_dist_sq = 1e30f;
    uint8_t best_mat = voxel::MaterialHardness::kMaterialAir;
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                int16_t q = 0;
                uint8_t mat = voxel::MaterialHardness::kMaterialAir;
                if (fetch_sample(ix + dx, iy + dy, iz + dz, &q, &mat) &&
                    q < 0 && mat != voxel::MaterialHardness::kMaterialAir) {
                    const float rx = static_cast<float>(ix + dx) - gx;
                    const float ry = static_cast<float>(iy + dy) - gy;
                    const float rz = static_cast<float>(iz + dz) - gz;
                    const float d2 = rx * rx + ry * ry + rz * rz;
                    if (d2 < best_dist_sq) {
                        best_dist_sq = d2;
                        best_mat = mat;
                    }
                }
            }
        }
    }

    if (best_mat != voxel::MaterialHardness::kMaterialAir) {
        return static_cast<int>(best_mat);
    }
    if (!resident0) {
        const float d = core_engine->procgen.evaluate_composite_density(
            p_world_pos.x, p_world_pos.y, p_world_pos.z);
        return (d <= 0.0f) ? static_cast<int>(voxel::MaterialHardness::kMaterialSoil)
                           : static_cast<int>(voxel::MaterialHardness::kMaterialAir);
    }
    return static_cast<int>(m0);
}

float VoxelTerrain3D::get_damage_at(Vector3 p_world_pos) const {
    if (!core_engine) {
        return 0.0f;
    }
    const float s = (voxel_scale > 0.0f) ? voxel_scale : 1.0f;
    const float inv_s = 1.0f / s;
    const float gx = p_world_pos.x * inv_s;
    const float gy = p_world_pos.y * inv_s;
    const float gz = p_world_pos.z * inv_s;

    const int32_t ix = static_cast<int32_t>(std::floor(gx));
    const int32_t iy = static_cast<int32_t>(std::floor(gy));
    const int32_t iz = static_cast<int32_t>(std::floor(gz));

    float best_dist_sq = 1e30f;
    float best_ratio = 0.0f;

    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const auto mapped = voxel::map_world_sample(ix + dx, iy + dy, iz + dz);
                if (const voxel::ChunkSlot* slot = core_engine->store.find_slot(mapped.chunk)) {
                    std::shared_lock<std::shared_mutex> rl(slot->mutex);
                    if (slot->state == voxel::ChunkLifecycleState::Resident &&
                        slot->get_tsdf(mapped.local) < 0) {
                        const uint8_t mat = slot->get_material(mapped.local);
                        const uint8_t dmg = slot->get_damage(mapped.local);
                        const uint8_t thresh = voxel::MaterialHardness::break_threshold(mat);
                        const float ratio = (thresh > 0U)
                            ? std::clamp(static_cast<float>(dmg) / static_cast<float>(thresh), 0.0f, 1.0f)
                            : 0.0f;
                        const float rx = static_cast<float>(ix + dx) - gx;
                        const float ry = static_cast<float>(iy + dy) - gy;
                        const float rz = static_cast<float>(iz + dz) - gz;
                        const float d2 = rx * rx + ry * ry + rz * rz;
                        if (d2 < best_dist_sq) {
                            best_dist_sq = d2;
                            best_ratio = ratio;
                        }
                    }
                }
            }
        }
    }
    return best_ratio;
}

PackedInt32Array VoxelTerrain3D::query_sphere_materials(Vector3 p_center, float p_radius) const {
    PackedInt32Array counts;
    counts.resize(8);
    for (int i = 0; i < 8; ++i) {
        counts[i] = 0;
    }
    if (!core_engine || p_radius <= 0.0f) {
        return counts;
    }

    const float s = (voxel_scale > 0.0f) ? voxel_scale : 1.0f;
    const float inv_s = 1.0f / s;
    const float cx = p_center.x * inv_s;
    const float cy = p_center.y * inv_s;
    const float cz = p_center.z * inv_s;
    const float r_vox = p_radius * inv_s;
    const float r_sq = r_vox * r_vox;

    const int32_t min_x = static_cast<int32_t>(std::floor(cx - r_vox));
    const int32_t min_y = static_cast<int32_t>(std::floor(cy - r_vox));
    const int32_t min_z = static_cast<int32_t>(std::floor(cz - r_vox));
    const int32_t max_x = static_cast<int32_t>(std::ceil(cx + r_vox));
    const int32_t max_y = static_cast<int32_t>(std::ceil(cy + r_vox));
    const int32_t max_z = static_cast<int32_t>(std::ceil(cz + r_vox));

    const int32_t chunk_x0 = voxel::ChunkKey::floor_div(min_x, 32);
    const int32_t chunk_y0 = std::max<int32_t>(0, voxel::ChunkKey::floor_div(min_y, 32));
    const int32_t chunk_z0 = voxel::ChunkKey::floor_div(min_z, 32);
    const int32_t chunk_x1 = voxel::ChunkKey::floor_div(max_x, 32);
    const int32_t chunk_y1 = std::min<int32_t>(world_height_chunks - 1,
                                               voxel::ChunkKey::floor_div(max_y, 32));
    const int32_t chunk_z1 = voxel::ChunkKey::floor_div(max_z, 32);

    for (int32_t ckz = chunk_z0; ckz <= chunk_z1; ++ckz) {
        for (int32_t cky = chunk_y0; cky <= chunk_y1; ++cky) {
            for (int32_t ckx = chunk_x0; ckx <= chunk_x1; ++ckx) {
                const voxel::ChunkKey ck = voxel::ChunkKey::from_coords(ckx, cky, ckz);
                const voxel::ChunkSlot* slot = core_engine->store.find_slot(ck);
                if (!slot) {
                    continue;
                }
                std::shared_lock<std::shared_mutex> rl(slot->mutex);
                if (slot->state != voxel::ChunkLifecycleState::Resident) {
                    continue;
                }

                const voxel::IVec3 cmin = ck.min_world_sample();
                const int32_t lx0 = std::max<int32_t>(0, min_x - cmin.x);
                const int32_t ly0 = std::max<int32_t>(0, min_y - cmin.y);
                const int32_t lz0 = std::max<int32_t>(0, min_z - cmin.z);
                const int32_t lx1 = std::min<int32_t>(31, max_x - cmin.x);
                const int32_t ly1 = std::min<int32_t>(31, max_y - cmin.y);
                const int32_t lz1 = std::min<int32_t>(31, max_z - cmin.z);

                for (int32_t lz = lz0; lz <= lz1; ++lz) {
                    const float dz = static_cast<float>(cmin.z + lz) - cz;
                    for (int32_t ly = ly0; ly <= ly1; ++ly) {
                        const float dy = static_cast<float>(cmin.y + ly) - cy;
                        for (int32_t lx = lx0; lx <= lx1; ++lx) {
                            const float dx = static_cast<float>(cmin.x + lx) - cx;
                            if (dx * dx + dy * dy + dz * dz > r_sq) {
                                continue;
                            }
                            const voxel::LocalCoord lc{lx, ly, lz};
                            if (slot->get_tsdf(lc) >= 0) {
                                counts[0] = counts[0] + 1;
                            } else {
                                const uint8_t mat = slot->get_material(lc);
                                const int idx = (mat < 8U) ? static_cast<int>(mat) : 1;
                                counts[idx] = counts[idx] + 1;
                            }
                        }
                    }
                }
            }
        }
    }
    return counts;
}

int VoxelTerrain3D::get_dominant_material_in_sphere(Vector3 p_center, float p_radius) const {
    const PackedInt32Array counts = query_sphere_materials(p_center, p_radius);
    int best_mat = static_cast<int>(voxel::MaterialHardness::kMaterialAir);
    int32_t best_count = 0;
    for (int mat = 1; mat < static_cast<int>(counts.size()); ++mat) {
        if (counts[mat] > best_count) {
            best_count = counts[mat];
            best_mat = mat;
        }
    }
    return best_mat;
}

float VoxelTerrain3D::get_surface_height(float p_x, float p_z) const {
    if (!core_engine) {
        return surface_base_height;
    }
    return voxel::sample_top_surface_y(core_engine->store, core_engine->procgen, p_x, p_z);
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
    const float inv_scale = 1.0f / voxel_scale;
    const voxel::ChunkKey focus_ck = voxel::ChunkKey::from_world_sample(
        static_cast<int32_t>(std::floor(p_position.x * inv_scale)),
        static_cast<int32_t>(std::floor(p_position.y * inv_scale)),
        static_cast<int32_t>(std::floor(p_position.z * inv_scale)));
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
           !server_slot_manager.has_pending_edit_batch() &&
           !server_slot_manager.has_staged_edit_meshes() &&
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

void VoxelTerrain3D::_process(double delta) {
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
            // If an edit batch has already finished its CSG phase and its chunk meshes
            // are finishing on the worker threads within this frame's soft upload budget,
            // briefly yield so all chunks of the crater can commit atomically on this frame.
            const uint64_t elapsed_us = monotonic_ticks_usec() - frame_start_us;
            if (server_slot_manager.has_pending_edit_batch() &&
                !core_idle() &&
                elapsed_us < soft_budget_us) {
                std::this_thread::yield();
                continue;
            }
            break;
        }

        switch (done->kind) {
            case voxel::CompletionKind::ChunkMesh: {
                voxel::ScopedTimer commit_timer(timings, voxel::Stage::Commit);
                (void)server_slot_manager.stage_or_commit_chunk_mesh(
                    done->chunk_mesh,
                    chunk_in_collision_range(done->chunk_mesh.key),
                    navigation_manager,
                    &non_empty_meshes_committed);
                break;
            }

            case voxel::CompletionKind::ChunkUnloaded:
                server_slot_manager.free_chunk_slot(done->unloaded_key);
                navigation_manager.note_chunk_unloaded(done->unloaded_key);
                break;

            case voxel::CompletionKind::EditTransactionCompleted: {
                handle_completed_edit_transaction(done->edit_transaction);
                break;
            }
        }

        const uint64_t elapsed_us = monotonic_ticks_usec() - frame_start_us;
        const std::size_t remaining_depth =
            core_engine->worker_pool->completion_queue_depth();
        if (remaining_depth == 0 && !server_slot_manager.has_pending_edit_batch()) {
            break;
        }

        // Never split an in-flight edit batch across frames while completions are
        // available; once the edit batch flushes atomically, resume normal soft-budget
        // pacing for background stream meshes.
        if (!server_slot_manager.has_pending_edit_batch()) {
            const bool currently_over_cap = (remaining_depth >= 32ULL);
            // Commit up to 8 non-empty chunk meshes per frame (still bounded by upload_budget_ms)
            // so a single 8-chunk blast clears in one frame instead of re-dirtying and rebaking
            // neighbouring navigation groups across five frames.
            if (!currently_over_cap &&
                (elapsed_us >= soft_budget_us || non_empty_meshes_committed >= 8ULL)) {
                break;
            }
            if (elapsed_us >= emergency_budget_us) {
                break;
            }
        } else if (elapsed_us >= emergency_budget_us && remaining_depth == 0) {
            break;
        }
    }

    // Safety net: if all workers and queues are completely idle, flush any remaining
    // staged edit meshes so no edit batch can ever remain stranded.
    if (core_idle() &&
        core_engine->worker_pool->completion_queue_depth() == 0 &&
        server_slot_manager.has_staged_edit_meshes()) {
        (void)server_slot_manager.flush_staged_edit_meshes(
            navigation_manager, &non_empty_meshes_committed);
    }

    // Advance reconstitution timers for stamped assets: damaged structures with a
    // positive recreate delay are restored (exact pre-damage TSDF + materials) once
    // their quiet period elapses; delay <= 0 structures stay permanently destroyed.
    if (delta > 0.0) {
        (void)stamp_manager_.process_reconstitution(
            static_cast<float>(delta),
            core_engine->store,
            core_engine->dirty_scheduler,
            navigation_manager,
            &server_slot_manager);
    }

    // Only submit NavigationGroup async bakes once all finished chunk meshes in
    // completion_queue have been committed, preventing multi-frame repeated rebakes.
    if (core_idle() && core_engine->worker_pool->completion_queue_depth() == 0) {
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
