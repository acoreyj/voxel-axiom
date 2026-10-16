#pragma once

#include "voxel_terrain_3d.h"

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/classes/navigation_server3d.hpp>
#endif

namespace godot {

#if !defined(VOXEL_WITH_GODOT_CPP)
class CharacterBody3D {
public:
    virtual ~CharacterBody3D() = default;
};
#endif

/// VoxelAICompanion Node (HLD v4.0 Section 4.2, 4.5).
/// Queries 3D cave navigation paths via NavigationServer3D and checks 3D `4 x 2 x 4`
/// navigation group synchronization state.
class VoxelAICompanion : public CharacterBody3D {
#if defined(VOXEL_WITH_GODOT_CPP)
    GDCLASS(VoxelAICompanion, CharacterBody3D)
#endif

public:
    static void _bind_methods();

    VoxelAICompanion() = default;
    ~VoxelAICompanion() override = default;

    void set_terrain_node(VoxelTerrain3D* p_terrain) noexcept {
        terrain_node_ = p_terrain;
    }

    /// Queries NavigationServer3D::map_get_path() on the terrain's 3D navigation map
    /// with optimize = true (Section 4.2).
    [[nodiscard]] PackedVector3Array get_voxel_path(Vector3 p_start, Vector3 p_end);

    /// Returns whether the 3D `4 x 2 x 4` navigation group containing `p_position`
    /// is currently synchronized to its `desired_terrain_revision` (Section 4.2),
    /// queuing an on-demand bake if the group has never been baked yet.
    [[nodiscard]] bool is_path_region_synced(Vector3 p_position);

private:
    void resolve_terrain_node_if_needed();
    VoxelTerrain3D* terrain_node_ = nullptr;
};

} // namespace godot
