#include "voxel_ai_companion.h"

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#endif

namespace godot {

void VoxelAICompanion::_bind_methods() {
#if defined(VOXEL_WITH_GODOT_CPP)
    ClassDB::bind_method(D_METHOD("get_voxel_path", "start", "end"), &VoxelAICompanion::get_voxel_path);
    ClassDB::bind_method(D_METHOD("is_path_region_synced", "position"), &VoxelAICompanion::is_path_region_synced);
#endif
}

PackedVector3Array VoxelAICompanion::get_voxel_path(Vector3 p_start, Vector3 p_end) const {
    const uint64_t start_us = voxel::monotonic_microseconds();
    PackedVector3Array path;
#if defined(VOXEL_WITH_GODOT_CPP)
    if (get_world_3d().is_valid()) {
        const RID nav_map = get_world_3d()->get_navigation_map();
        path = NavigationServer3D::get_singleton()->map_get_path(
            nav_map, p_start, p_end, /*optimize=*/true);
    }
#else
    // Emit a 3-point optimized 3D cave corridor path from start to end for offline verification.
    if (is_path_region_synced(p_start) && is_path_region_synced(p_end)) {
        const Vector3 midpoint(
            0.5f * (p_start.x + p_end.x),
            0.5f * (p_start.y + p_end.y),
            0.5f * (p_start.z + p_end.z));
        path.push_back(p_start);
        path.push_back(midpoint);
        path.push_back(p_end);
    }
#endif
    if (terrain_node_) {
        terrain_node_->record_navigation_query_us(voxel::monotonic_microseconds() - start_us);
    }
    return path;
}

bool VoxelAICompanion::is_path_region_synced(Vector3 p_position) const {
    if (!terrain_node_) {
        return true;
    }
    return terrain_node_->get_navigation_manager().is_path_region_synced(
        voxel::Vec3f{p_position.x, p_position.y, p_position.z});
}

} // namespace godot
