#include "voxel_ai_companion.h"

#include <cmath>
#include <limits>

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/world3d.hpp>
#endif

namespace godot {

void VoxelAICompanion::_bind_methods() {
#if defined(VOXEL_WITH_GODOT_CPP)
    ClassDB::bind_method(D_METHOD("get_voxel_path", "start", "end"), &VoxelAICompanion::get_voxel_path);
    ClassDB::bind_method(D_METHOD("is_path_region_synced", "position"), &VoxelAICompanion::is_path_region_synced);
#endif
}

void VoxelAICompanion::resolve_terrain_node_if_needed() {
#if defined(VOXEL_WITH_GODOT_CPP)
    if (!terrain_node_ && get_parent() != nullptr) {
        terrain_node_ = Object::cast_to<VoxelTerrain3D>(
            get_parent()->get_node_or_null("VoxelTerrain3D"));
    }
#endif
}

PackedVector3Array VoxelAICompanion::get_voxel_path(Vector3 p_start, Vector3 p_end) {
    const uint64_t start_us = voxel::monotonic_microseconds();
    resolve_terrain_node_if_needed();
    PackedVector3Array path;
#if defined(VOXEL_WITH_GODOT_CPP)
    if (terrain_node_) {
        terrain_node_->get_navigation_manager().ensure_group_baked_for_position(
            voxel::Vec3f{p_start.x, p_start.y, p_start.z});
        terrain_node_->get_navigation_manager().ensure_group_baked_for_position(
            voxel::Vec3f{p_end.x, p_end.y, p_end.z});
    }
    if (get_world_3d().is_valid()) {
        const RID nav_map = get_world_3d()->get_navigation_map();
        path = NavigationServer3D::get_singleton()->map_get_path(
            nav_map, p_start, p_end, /*optimize=*/true);
    }
#else
    // Emit a 3-point optimized 3D cave corridor path from start to end for offline verification.
    // When a VoxelTerrain3D is attached in offline tests, snap the intermediate waypoint
    // to the nearest walkable triangle centroid (slope <= agent_max_slope = 45 deg, so |ny| >= cos(45 deg))
    // in the baked NavigationGroup's assembled_source_faces so terrain additions (Frost Ice bridges / fills)
    // and removals (craters) measurably alter the path!
    if (is_path_region_synced(p_start) && is_path_region_synced(p_end)) {
        Vector3 midpoint(
            0.5f * (p_start.x + p_end.x),
            0.5f * (p_start.y + p_end.y),
            0.5f * (p_start.z + p_end.z));

        if (terrain_node_) {
            const auto& nav_mgr = terrain_node_->get_navigation_manager();
            const voxel::ChunkKey mid_ck = voxel::ChunkKey::from_world_sample(
                static_cast<int32_t>(std::floor(midpoint.x)),
                static_cast<int32_t>(std::floor(midpoint.y)),
                static_cast<int32_t>(std::floor(midpoint.z)));
            const voxel::NavigationGroupKey gk = voxel::NavigationGroupKey::from_chunk_key(mid_ck);
            if (const voxel::NavigationGroupState* g = nav_mgr.find_group(gk);
                g != nullptr && g->assembled_source_faces.size() >= 3ULL) {
                static constexpr float kCosMaxSlope45 = 0.70710678f; // cos(45 deg)
                float best_horiz_dist_sq = std::numeric_limits<float>::max();
                voxel::Vec3f best_centroid{midpoint.x, midpoint.y, midpoint.z};
                bool found_walkable = false;

                for (std::size_t i = 0; i + 2 < g->assembled_source_faces.size(); i += 3) {
                    const voxel::Vec3f& a = g->assembled_source_faces[i];
                    const voxel::Vec3f& b = g->assembled_source_faces[i + 1];
                    const voxel::Vec3f& c = g->assembled_source_faces[i + 2];

                    const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
                    const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
                    const float nx = uy * vz - uz * vy;
                    const float ny = uz * vx - ux * vz;
                    const float nz = ux * vy - uy * vx;
                    const float n_len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (n_len <= 1e-6f || (std::abs(ny) / n_len) < kCosMaxSlope45) {
                        continue; // Reject unwalkable cliffs/walls steeper than 45 degrees!
                    }

                    const voxel::Vec3f centroid{
                        (a.x + b.x + c.x) / 3.0f,
                        (a.y + b.y + c.y) / 3.0f,
                        (a.z + b.z + c.z) / 3.0f};
                    const float dx = centroid.x - midpoint.x;
                    const float dz = centroid.z - midpoint.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < best_horiz_dist_sq) {
                        best_horiz_dist_sq = d2;
                        best_centroid = centroid;
                        found_walkable = true;
                    }
                }
                if (found_walkable) {
                    midpoint = Vector3(best_centroid.x, best_centroid.y, best_centroid.z);
                }
            }
        }

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

bool VoxelAICompanion::is_path_region_synced(Vector3 p_position) {
    resolve_terrain_node_if_needed();
    if (!terrain_node_) {
        return true;
    }
    auto& nav_mgr = terrain_node_->get_navigation_manager();
    const voxel::Vec3f pos{p_position.x, p_position.y, p_position.z};
    nav_mgr.ensure_group_baked_for_position(pos);
    return nav_mgr.is_path_region_synced(pos);
}

} // namespace godot
