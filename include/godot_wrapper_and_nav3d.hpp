#pragma once

#include "persistence_and_streaming.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(VOXEL_WITH_GODOT_CPP)
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/navigation_mesh.hpp>
#include <godot_cpp/classes/navigation_mesh_source_geometry_data3d.hpp>
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#endif

namespace voxel {

// ============================================================================
// 1. Direct Server RID Slot Manager (HLD v4.0 Sections 4.1, 4.4, 5.3)
// ============================================================================

struct ServerRid {
    uint64_t id = 0;
#if defined(VOXEL_WITH_GODOT_CPP)
    godot::RID native_rid{};
#endif
    [[nodiscard]] bool is_valid() const noexcept {
#if defined(VOXEL_WITH_GODOT_CPP)
        return native_rid.is_valid() || id != 0;
#else
        return id != 0;
#endif
    }
    bool operator==(const ServerRid& o) const noexcept { return id == o.id; }
};

struct ChunkServerSlot {
    ServerRid mesh_rid{};
    ServerRid instance_rid{};
    ServerRid body_rid{};
    ServerRid shape_rid{};
    std::vector<Vec3f> cached_nav_faces; // Retained for 3D NavigationGroup rebakes
#if defined(VOXEL_WITH_GODOT_CPP)
    godot::PackedVector3Array godot_nav_faces;
#endif
    uint64_t committed_version = 0;
    uint64_t committed_terrain_revision = 0;
    /// True while this chunk owns a live PhysicsServer3D concave shape carrying its
    /// triangles. Distant chunks are committed visual-only (collision_enabled == false)
    /// under `collision_distance_chunks`; the shape RID still exists but holds no faces.
    bool collision_enabled = false;
};

/// Manages per-chunk RenderingServer and PhysicsServer3D RIDs directly without
/// creating child MeshInstance3D or StaticBody3D nodes in the SceneTree (Section 4.1).
class ServerSlotManager {
public:
    using CommitHookFn = std::function<void(const ChunkMeshPayload&)>;

    void initialize(ServerRid scenario_rid, ServerRid space_rid) noexcept {
        scenario_rid_ = scenario_rid;
        space_rid_ = space_rid;
        initialized_ = true;
    }

#if defined(VOXEL_WITH_GODOT_CPP)
    void set_material_override(godot::RID material_rid) noexcept {
        material_override_rid_ = material_rid;
    }
#endif

    void set_commit_hook(CommitHookFn hook) {
        commit_hook_ = std::move(hook);
    }

    /// @param build_collision  When false, the chunk still uploads its visual mesh and
    ///   caches its nav faces, but its physics shape is left empty so no concave BVH is
    ///   built for it. Used to gate distant chunks under `collision_distance_chunks`.
    void commit_chunk_mesh_and_collision(const ChunkMeshPayload& mesh, bool build_collision = true) {
        assert(initialized_);
        if (commit_hook_) {
            commit_hook_(mesh);
        }

#if defined(VOXEL_WITH_GODOT_CPP)
        // Fast path in Godot builds: if a chunk has 0 triangles (pure air or pure solid chunk
        // with no surface crossing), do NOT allocate RenderingServer or PhysicsServer3D RIDs!
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            auto it = slots_.find(mesh.key);
            if (it != slots_.end()) {
                release_slot_rids(it->second);
                slots_.erase(it);
            }
            return;
        }
#endif

        ChunkServerSlot& slot = slots_[mesh.key];
        if (!slot.mesh_rid.is_valid()) {
            slot.mesh_rid = allocate_rid();
            slot.instance_rid = allocate_rid();
            slot.body_rid = allocate_rid();
            slot.shape_rid = allocate_rid();

#if defined(VOXEL_WITH_GODOT_CPP)
            auto* rs = godot::RenderingServer::get_singleton();
            auto* ps = godot::PhysicsServer3D::get_singleton();
            if (rs && ps) {
                slot.mesh_rid.native_rid = rs->mesh_create();
                slot.instance_rid.native_rid = rs->instance_create();
                rs->instance_set_base(slot.instance_rid.native_rid, slot.mesh_rid.native_rid);
                if (scenario_rid_.native_rid.is_valid()) {
                    rs->instance_set_scenario(slot.instance_rid.native_rid, scenario_rid_.native_rid);
                }
                if (material_override_rid_.is_valid()) {
                    rs->instance_geometry_set_material_override(
                        slot.instance_rid.native_rid, material_override_rid_);
                }

                slot.body_rid.native_rid = ps->body_create();
                ps->body_set_mode(slot.body_rid.native_rid, godot::PhysicsServer3D::BODY_MODE_STATIC);
                if (space_rid_.native_rid.is_valid()) {
                    ps->body_set_space(slot.body_rid.native_rid, space_rid_.native_rid);
                }
                slot.shape_rid.native_rid = ps->concave_polygon_shape_create();
                ps->body_add_shape(slot.body_rid.native_rid, slot.shape_rid.native_rid);
            }
#endif
        }

#if defined(VOXEL_WITH_GODOT_CPP)
        auto* rs = godot::RenderingServer::get_singleton();
        auto* ps = godot::PhysicsServer3D::get_singleton();
        if (rs && ps && slot.mesh_rid.native_rid.is_valid()) {
            rs->mesh_clear(slot.mesh_rid.native_rid);

            if (!mesh.vertices.empty() && !mesh.indices.empty()) {
                godot::PackedVector3Array verts;
                verts.resize(static_cast<int64_t>(mesh.vertices.size()));
                std::memcpy(verts.ptrw(), mesh.vertices.data(), mesh.vertices.size() * sizeof(Vec3f));

                godot::PackedVector3Array norms;
                norms.resize(static_cast<int64_t>(mesh.normals.size()));
                std::memcpy(norms.ptrw(), mesh.normals.data(), mesh.normals.size() * sizeof(Vec3f));

                godot::PackedColorArray colors;
                colors.resize(static_cast<int64_t>(mesh.vertices.size()));
                godot::Color* color_ptr = colors.ptrw();
                for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                    const Vec3f c = (i < mesh.colors.size()) ? mesh.colors[i] : Vec3f{};
                    color_ptr[i] = godot::Color(c.x, c.y, c.z, 1.0f);
                }

                godot::PackedInt32Array indices;
                indices.resize(static_cast<int64_t>(mesh.indices.size()));
                std::memcpy(indices.ptrw(), mesh.indices.data(), mesh.indices.size() * sizeof(int32_t));

                godot::Array surface_arrays;
                surface_arrays.resize(godot::RenderingServer::ARRAY_MAX);
                surface_arrays[godot::RenderingServer::ARRAY_VERTEX] = verts;
                surface_arrays[godot::RenderingServer::ARRAY_NORMAL] = norms;
                surface_arrays[godot::RenderingServer::ARRAY_COLOR]  = colors;
                surface_arrays[godot::RenderingServer::ARRAY_INDEX]  = indices;

                rs->mesh_add_surface_from_arrays(
                    slot.mesh_rid.native_rid,
                    godot::RenderingServer::PRIMITIVE_TRIANGLES,
                    surface_arrays);
            }

            // Copy pre-unpacked triangle soup once and share between PhysicsServer3D and cached_nav_faces.
            slot.godot_nav_faces.resize(static_cast<int64_t>(mesh.collision_faces.size()));
            if (!mesh.collision_faces.empty()) {
                std::memcpy(slot.godot_nav_faces.ptrw(),
                            mesh.collision_faces.data(),
                            mesh.collision_faces.size() * sizeof(Vec3f));
            }
            // Distant chunks keep their cached faces (for navigation and later promotion)
            // but receive an empty shape, so PhysicsServer3D never builds their concave BVH.
            const bool faces_present = !mesh.collision_faces.empty();
            slot.collision_enabled = build_collision && faces_present;
            godot::Dictionary shape_dict;
            shape_dict["faces"] = slot.collision_enabled ? slot.godot_nav_faces
                                                         : godot::PackedVector3Array();
            shape_dict["backface_collision"] = false;
            ps->shape_set_data(slot.shape_rid.native_rid, shape_dict);
        }
#else
        slot.cached_nav_faces = mesh.collision_faces;
        slot.collision_enabled = build_collision && !mesh.collision_faces.empty();
#endif

        slot.committed_version = mesh.captured_version;
        slot.committed_terrain_revision = mesh.captured_terrain_revision;
    }

    void free_chunk_slot(ChunkKey key) noexcept {
        auto it = slots_.find(key);
        if (it != slots_.end()) {
            release_slot_rids(it->second);
            freed_rids_count_ += 4;
            slots_.erase(it);
        }
    }

    /// Step 7 of Ordered Teardown (Section 5.3): Free all RenderingServer & PhysicsServer3D RIDs.
    void free_all_rids() noexcept {
        for (auto& [ck, slot] : slots_) {
            release_slot_rids(slot);
        }
        freed_rids_count_ += slots_.size() * 4;
        slots_.clear();
        initialized_ = false;
    }

    [[nodiscard]] const std::vector<Vec3f>& get_cached_nav_faces(ChunkKey key) const noexcept {
        static const std::vector<Vec3f> kEmpty;
        auto it = slots_.find(key);
        return (it != slots_.end()) ? it->second.cached_nav_faces : kEmpty;
    }

    [[nodiscard]] const ChunkServerSlot* find_slot(ChunkKey key) const noexcept {
        auto it = slots_.find(key);
        return (it != slots_.end()) ? &it->second : nullptr;
    }

    /// Enables or disables a resident chunk's physics collision in place from its cached
    /// nav faces, without re-uploading its visual mesh. Used when the streaming focus
    /// moves a chunk into or out of `collision_distance_chunks`; no-op if the state
    /// already matches. Disabling swaps in an empty shape (BVH freed); enabling rebuilds
    /// it from the retained faces.
    void set_chunk_collision_enabled(ChunkKey key, bool enabled) noexcept {
        auto it = slots_.find(key);
        if (it == slots_.end() || it->second.collision_enabled == enabled) {
            return;
        }
        it->second.collision_enabled = enabled;
#if defined(VOXEL_WITH_GODOT_CPP)
        auto* ps = godot::PhysicsServer3D::get_singleton();
        if (ps == nullptr || !it->second.shape_rid.native_rid.is_valid()) {
            return;
        }
        godot::Dictionary shape_dict;
        shape_dict["faces"] = enabled ? it->second.godot_nav_faces
                                      : godot::PackedVector3Array();
        shape_dict["backface_collision"] = false;
        ps->shape_set_data(it->second.shape_rid.native_rid, shape_dict);
#endif
    }

    [[nodiscard]] bool chunk_has_collision(ChunkKey key) const noexcept {
        auto it = slots_.find(key);
        return it != slots_.end() && it->second.collision_enabled;
    }

    /// Visits every resident chunk slot on the main thread. Used by
    /// `VoxelTerrain3D::_reconcile_collision_range` to re-evaluate collision gating
    /// after the streaming focus crosses a chunk boundary.
    template <typename Fn>
    void for_each_slot(Fn&& fn) {
        for (auto& entry : slots_) {
            fn(entry.first, entry.second);
        }
    }

    [[nodiscard]] std::size_t active_slot_count() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t freed_rids_count() const noexcept { return freed_rids_count_; }

private:
    [[nodiscard]] ServerRid allocate_rid() noexcept {
        return ServerRid{++next_rid_counter_};
    }

    static void release_slot_rids(ChunkServerSlot& slot) noexcept {
#if defined(VOXEL_WITH_GODOT_CPP)
        auto* rs = godot::RenderingServer::get_singleton();
        auto* ps = godot::PhysicsServer3D::get_singleton();
        if (rs) {
            if (slot.instance_rid.native_rid.is_valid()) rs->free_rid(slot.instance_rid.native_rid);
            if (slot.mesh_rid.native_rid.is_valid()) rs->free_rid(slot.mesh_rid.native_rid);
        }
        if (ps) {
            if (slot.body_rid.native_rid.is_valid()) ps->free_rid(slot.body_rid.native_rid);
            if (slot.shape_rid.native_rid.is_valid()) ps->free_rid(slot.shape_rid.native_rid);
        }
#else
        (void)slot;
#endif
    }

    bool initialized_ = false;
    ServerRid scenario_rid_{};
    ServerRid space_rid_{};
#if defined(VOXEL_WITH_GODOT_CPP)
    godot::RID material_override_rid_{};
#endif
    uint64_t next_rid_counter_ = 0;
    std::size_t freed_rids_count_ = 0;
    CommitHookFn commit_hook_;
    std::unordered_map<ChunkKey, ChunkServerSlot, ChunkKeyHash> slots_;
};

// ============================================================================
// 2. 3D Cave Navigation Group Manager (`4 x 2 x 4` Chunks, Section 4.5)
// ============================================================================

struct CaveNavMeshConfig {
    float voxel_scale = 1.0f;
    float cell_size = 0.25f;   // 0.25 * s
    float cell_height = 0.20f; // 0.20 * s
    float agent_radius = 0.6f;
    float agent_height = 1.8f;
    float agent_max_climb = 0.5f;
    float agent_max_slope = 45.0f;
    float region_connection_margin = 0.4f;
    float region_min_size = 8.0f;

    bool filter_low_hanging_obstacles = true;
    bool filter_ledge_spans = true;
    bool filter_walkable_low_height_spans = true;

    [[nodiscard]] constexpr float horizontal_padding() const noexcept {
        return agent_radius + region_connection_margin + 2.0f * voxel_scale;
    }

    [[nodiscard]] constexpr float vertical_padding() const noexcept {
        return agent_height + agent_max_climb + 2.0f * voxel_scale;
    }
};

struct Aabb3f {
    Vec3f min{};
    Vec3f max{};

    [[nodiscard]] constexpr bool intersects(const Aabb3f& o) const noexcept {
        return (min.x <= o.max.x && max.x >= o.min.x) &&
               (min.y <= o.max.y && max.y >= o.min.y) &&
               (min.z <= o.max.z && max.z >= o.min.z);
    }
};

struct NavigationGroupState {
    bool live = false;
    ServerRid region_rid{};
    CaveNavMeshConfig nav_mesh_config{};
    Aabb3f owned_aabb{};
    Aabb3f baking_aabb{};
    float border_size = 0.0f;

#if defined(VOXEL_WITH_GODOT_CPP)
    godot::Ref<godot::NavigationMesh> nav_mesh;
    godot::Ref<godot::NavigationMeshSourceGeometryData3D> source_data;
#endif

    std::vector<Vec3f> assembled_source_faces;

    uint64_t desired_terrain_revision = 0;
    uint64_t submitted_terrain_revision = 0;
    uint64_t synced_terrain_revision = 0;

    uint64_t desired_source_epoch = 0;
    uint64_t submitted_source_epoch = 0;
    uint64_t synced_source_epoch = 0;

    bool rebuild_in_flight = false;
    bool bake_callback_received = false;
    uint32_t pending_callback_map_iteration = 0;
    std::size_t total_bakes_submitted = 0;
};

/// Orchestrates 3D `4 x 2 x 4` chunk NavigationGroups (`128 x 64 x 128` voxel cells)
/// with 3D border padding and zero-parse async source geometry baking (Section 4.5).
class VoxelNavigationManager {
public:
    void initialize(ServerRid nav_map_rid, CaveNavMeshConfig cfg = {}) noexcept {
        nav_map_rid_ = nav_map_rid;
        config_ = cfg;
        map_iteration_id_ = 1;
        initialized_ = true;
    }

    void set_bake_on_stream_enabled(bool enabled) noexcept {
        bake_on_stream_enabled_ = enabled;
    }

    void shutdown() noexcept {
#if defined(VOXEL_WITH_GODOT_CPP)
        if (auto* ns = godot::NavigationServer3D::get_singleton()) {
            for (auto& [gk, g] : groups_) {
                if (g.region_rid.native_rid.is_valid()) {
                    ns->free_rid(g.region_rid.native_rid);
                }
            }
        }
#endif
        groups_.clear();
        chunk_revisions_.clear();
        required_chunk_revisions_.clear();
        waiting_chunks_.clear();
        newly_committed_chunks_.clear();
        initialized_ = false;
    }

    [[nodiscard]] Aabb3f compute_owned_aabb(NavigationGroupKey key) const noexcept {
        const float s = config_.voxel_scale;
        return Aabb3f{
            Vec3f{static_cast<float>(128 * key.gx) * s,
                  static_cast<float>(64 * key.gy) * s,
                  static_cast<float>(128 * key.gz) * s},
            Vec3f{static_cast<float>(128 * (key.gx + 1)) * s,
                  static_cast<float>(64 * (key.gy + 1)) * s,
                  static_cast<float>(128 * (key.gz + 1)) * s}
        };
    }

    [[nodiscard]] Aabb3f compute_baking_aabb(NavigationGroupKey key) const noexcept {
        const Aabb3f owned = compute_owned_aabb(key);
        const float pxz = config_.horizontal_padding();
        const float py = config_.vertical_padding();
        return Aabb3f{
            Vec3f{owned.min.x - pxz, owned.min.y - py, owned.min.z - pxz},
            Vec3f{owned.max.x + pxz, owned.max.y + py, owned.max.z + pxz}
        };
    }

    /// Ensures the 3D `4 x 2 x 4` navigation group containing `world_pos` has been baked at least
    /// once on demand (useful when `bake_on_stream_enabled` is false at startup and the user
    /// requests a path before firing an explosion in that group).
    void ensure_group_baked_for_position(Vec3f world_pos) {
        const float s = config_.voxel_scale;
        const int32_t px = static_cast<int32_t>(std::floor(world_pos.x / s));
        const int32_t py = static_cast<int32_t>(std::floor(world_pos.y / s));
        const int32_t pz = static_cast<int32_t>(std::floor(world_pos.z / s));
        const ChunkKey ck = ChunkKey::from_world_sample(px, py, pz);
        const NavigationGroupKey gk = NavigationGroupKey::from_chunk_key(ck);
        NavigationGroupState& g = get_or_create_group(gk);
        if (g.total_bakes_submitted == 0 && g.desired_source_epoch == 0) {
            g.desired_source_epoch = 1;
        }
    }

    void note_chunk_mesh_committed(ChunkKey key,
                                   uint64_t captured_terrain_revision,
                                   const std::vector<Vec3f>& /*faces*/) {
        chunk_revisions_[key] = captured_terrain_revision;
        waiting_chunks_.insert(key);

        if (!bake_on_stream_enabled_ && captured_terrain_revision == 0) {
            return;
        }

        const Aabb3f chunk_box = chunk_aabb(key);
        const NavigationGroupKey owner_gk = NavigationGroupKey::from_chunk_key(key);

        for (int32_t dgz = -1; dgz <= 1; ++dgz) {
            for (int32_t dgy = -1; dgy <= 1; ++dgy) {
                for (int32_t dgx = -1; dgx <= 1; ++dgx) {
                    const NavigationGroupKey candidate{
                        owner_gk.gx + dgx, owner_gk.gy + dgy, owner_gk.gz + dgz};
                    if (compute_baking_aabb(candidate).intersects(chunk_box)) {
                        if (candidate == owner_gk || groups_.find(candidate) != groups_.end()) {
                            NavigationGroupState& g = get_or_create_group(candidate);
                            g.desired_source_epoch += 1;
                            g.desired_terrain_revision =
                                std::max(g.desired_terrain_revision, captured_terrain_revision);
                        }
                    }
                }
            }
        }
    }

    void note_chunk_unloaded(ChunkKey key) {
        chunk_revisions_.erase(key);
        required_chunk_revisions_.erase(key);
        waiting_chunks_.erase(key);
        if (!bake_on_stream_enabled_) {
            return;
        }
        const NavigationGroupKey owner_gk = NavigationGroupKey::from_chunk_key(key);
        auto it = groups_.find(owner_gk);
        if (it != groups_.end()) {
            it->second.desired_source_epoch += 1;
        }
    }

    void apply_edit_transaction(const EditTransactionCompletedPayload& tx) {
        for (const ChunkKey dk : tx.dirtied_chunks) {
            required_chunk_revisions_[dk] =
                std::max(required_chunk_revisions_[dk], tx.terrain_revision);
        }
        for (const NavigationGroupKey gk : tx.affected_nav_groups) {
            NavigationGroupState& g = get_or_create_group(gk);
            g.desired_terrain_revision = std::max(g.desired_terrain_revision, tx.terrain_revision);
        }
    }

    void submit_ready_groups_to_server(const ServerSlotManager& slot_mgr) {
        // Throttle to at most 1 navigation group submission per frame so Recast never
        // floods Godot's worker threads or blocks NavigationServer3D synchronization.
        std::size_t groups_submitted_this_frame = 0;

        for (auto& [gk, g] : groups_) {
            if (g.rebuild_in_flight) {
                continue;
            }
            const bool dirty = (g.desired_terrain_revision > g.synced_terrain_revision) ||
                               (g.desired_source_epoch > g.synced_source_epoch);
            if (!dirty) {
                continue;
            }

            bool all_chunks_ready = true;
            for (const auto& [ck, req_rev] : required_chunk_revisions_) {
                if (NavigationGroupKey::from_chunk_key(ck) == gk) {
                    const auto rev_it = chunk_revisions_.find(ck);
                    const uint64_t cur_rev = (rev_it != chunk_revisions_.end()) ? rev_it->second : 0ULL;
                    if (cur_rev < req_rev) {
                        all_chunks_ready = false;
                        break;
                    }
                }
            }
            if (!all_chunks_ready) {
                continue;
            }

#if !defined(VOXEL_WITH_GODOT_CPP)
            g.assembled_source_faces.clear();
#else
            if (g.source_data.is_valid()) {
                g.source_data->clear();
            }
#endif
            const int32_t cx_min = gk.gx * 4 - 1;
            const int32_t cx_max = (gk.gx + 1) * 4;
            const int32_t cy_min = gk.gy * 2 - 1;
            const int32_t cy_max = (gk.gy + 1) * 2;
            const int32_t cz_min = gk.gz * 4 - 1;
            const int32_t cz_max = (gk.gz + 1) * 4;

            for (int32_t cz = cz_min; cz <= cz_max; ++cz) {
                for (int32_t cy = cy_min; cy <= cy_max; ++cy) {
                    for (int32_t cx = cx_min; cx <= cx_max; ++cx) {
                        const ChunkKey ck = ChunkKey::from_coords(cx, cy, cz);
                        if (!g.baking_aabb.intersects(chunk_aabb(ck))) {
                            continue;
                        }
#if !defined(VOXEL_WITH_GODOT_CPP)
                        const auto& faces = slot_mgr.get_cached_nav_faces(ck);
                        g.assembled_source_faces.insert(
                            g.assembled_source_faces.end(), faces.begin(), faces.end());
#else
                        if (const ChunkServerSlot* cslot = slot_mgr.find_slot(ck);
                            cslot != nullptr && !cslot->godot_nav_faces.is_empty() && g.source_data.is_valid()) {
                            g.source_data->add_faces(cslot->godot_nav_faces, godot::Transform3D());
                        }
#endif
                    }
                }
            }

            g.rebuild_in_flight = true;
            g.bake_callback_received = false;
            g.submitted_terrain_revision = g.desired_terrain_revision;
            g.submitted_source_epoch = g.desired_source_epoch;
            g.total_bakes_submitted += 1;

#if defined(VOXEL_WITH_GODOT_CPP)
            if (auto* ns = godot::NavigationServer3D::get_singleton();
                ns != nullptr && g.nav_mesh.is_valid() && g.source_data.is_valid()) {
                ns->bake_from_source_geometry_data_async(
                    g.nav_mesh,
                    g.source_data,
                    godot::Callable());
            }
            if (++groups_submitted_this_frame >= 1ULL) {
                break;
            }
#else
            (void)groups_submitted_this_frame;
#endif
        }
    }

    void complete_in_flight_async_bakes_and_advance_map_iteration() {
        for (auto& [gk, g] : groups_) {
            if (g.rebuild_in_flight && !g.bake_callback_received) {
                g.bake_callback_received = true;
                g.pending_callback_map_iteration = map_iteration_id_;
            }
        }
        ++map_iteration_id_;
        poll_server_synchronization();
    }

    void poll_server_synchronization() {
#if defined(VOXEL_WITH_GODOT_CPP)
        auto* ns = godot::NavigationServer3D::get_singleton();
        if (ns != nullptr && nav_map_rid_.native_rid.is_valid()) {
            map_iteration_id_ = ns->map_get_iteration_id(nav_map_rid_.native_rid);
        }
        for (auto& [gk, g] : groups_) {
            if (g.rebuild_in_flight && !g.bake_callback_received) {
                // Step 1: Wait until the async Recast bake on WorkerThreadPool finishes, then
                // push the freshly baked NavigationMesh to the NavigationRegion RID!
                if (ns != nullptr && g.nav_mesh.is_valid() && !ns->is_baking_navigation_mesh(g.nav_mesh)) {
                    if (g.region_rid.native_rid.is_valid()) {
                        ns->region_set_navigation_mesh(g.region_rid.native_rid, g.nav_mesh);
                    }
                    g.bake_callback_received = true;
                    g.pending_callback_map_iteration = map_iteration_id_;
                }
            } else if (g.rebuild_in_flight && g.bake_callback_received) {
                // Step 2: Once region_set_navigation_mesh has been called, NavigationServer3D
                // increments map_get_iteration_id on its next sync step (or immediately if >=).
                if (map_iteration_id_ > g.pending_callback_map_iteration) {
                    g.synced_terrain_revision = g.submitted_terrain_revision;
                    g.synced_source_epoch = g.submitted_source_epoch;
                    g.rebuild_in_flight = false;
                    g.bake_callback_received = false;
                }
            }
        }
#else
        for (auto& [gk, g] : groups_) {
            if (g.rebuild_in_flight &&
                g.bake_callback_received &&
                map_iteration_id_ > g.pending_callback_map_iteration) {
                g.synced_terrain_revision = g.submitted_terrain_revision;
                g.synced_source_epoch = g.submitted_source_epoch;
                g.rebuild_in_flight = false;
                g.bake_callback_received = false;
            }
        }
#endif

        for (auto it = waiting_chunks_.begin(); it != waiting_chunks_.end();) {
            const NavigationGroupKey gk = NavigationGroupKey::from_chunk_key(*it);
            if (is_group_synced(gk)) {
                newly_committed_chunks_.push_back(*it);
                it = waiting_chunks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] std::vector<ChunkKey> drain_newly_committed_chunks() {
        std::vector<ChunkKey> out;
        out.swap(newly_committed_chunks_);
        return out;
    }

    [[nodiscard]] bool is_group_synced(NavigationGroupKey gk) const noexcept {
        auto it = groups_.find(gk);
        if (it == groups_.end()) {
            return true;
        }
        const NavigationGroupState& g = it->second;
        return !g.rebuild_in_flight &&
               g.synced_terrain_revision == g.desired_terrain_revision &&
               g.synced_source_epoch == g.desired_source_epoch;
    }

    [[nodiscard]] bool is_path_region_synced(Vec3f world_pos) const noexcept {
        const float s = config_.voxel_scale;
        const int32_t px = static_cast<int32_t>(std::floor(world_pos.x / s));
        const int32_t py = static_cast<int32_t>(std::floor(world_pos.y / s));
        const int32_t pz = static_cast<int32_t>(std::floor(world_pos.z / s));
        const ChunkKey ck = ChunkKey::from_world_sample(px, py, pz);
        return is_group_synced(NavigationGroupKey::from_chunk_key(ck));
    }

    [[nodiscard]] bool navigation_groups_synced() const noexcept {
        for (const auto& [gk, g] : groups_) {
            if (g.rebuild_in_flight ||
                g.synced_terrain_revision != g.desired_terrain_revision ||
                g.synced_source_epoch != g.desired_source_epoch) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const NavigationGroupState* find_group(NavigationGroupKey gk) const noexcept {
        auto it = groups_.find(gk);
        return (it != groups_.end()) ? &it->second : nullptr;
    }

private:
    [[nodiscard]] Aabb3f chunk_aabb(ChunkKey ck) const noexcept {
        const float s = config_.voxel_scale;
        const IVec3 min_s = ck.min_world_sample();
        return Aabb3f{
            Vec3f{static_cast<float>(min_s.x) * s,
                  static_cast<float>(min_s.y) * s,
                  static_cast<float>(min_s.z) * s},
            Vec3f{static_cast<float>(min_s.x + 32) * s,
                  static_cast<float>(min_s.y + 32) * s,
                  static_cast<float>(min_s.z + 32) * s}
        };
    }

    NavigationGroupState& get_or_create_group(NavigationGroupKey gk) {
        auto it = groups_.find(gk);
        if (it != groups_.end()) {
            return it->second;
        }
        NavigationGroupState g{};
        g.live = true;
        g.region_rid = ServerRid{++next_region_rid_};
        g.nav_mesh_config = config_;
        g.owned_aabb = compute_owned_aabb(gk);
        g.baking_aabb = compute_baking_aabb(gk);
        g.border_size = config_.horizontal_padding();

#if defined(VOXEL_WITH_GODOT_CPP)
        if (auto* ns = godot::NavigationServer3D::get_singleton()) {
            g.region_rid.native_rid = ns->region_create();
            if (nav_map_rid_.native_rid.is_valid()) {
                ns->region_set_map(g.region_rid.native_rid, nav_map_rid_.native_rid);
            }
        }
        g.nav_mesh.instantiate();
        // Configure Recast heightfield so 30-45 deg ramps and narrow Ice Bridges across caverns
        // never get disconnected by quantized height steps (walkableClimb = floor(0.90 / 0.20) = 4 cells)
        // or eroded away on bridge edges (filter_ledge_spans = false).
        g.nav_mesh->set_cell_size(0.4f);
        g.nav_mesh->set_cell_height(0.2f);
        g.nav_mesh->set_agent_radius(config_.agent_radius);
        g.nav_mesh->set_agent_height(config_.agent_height);
        g.nav_mesh->set_agent_max_climb(0.9f);
        g.nav_mesh->set_agent_max_slope(52.0f);
        g.nav_mesh->set_region_min_size(2.0f);
        g.nav_mesh->set_border_size(g.border_size);
        g.nav_mesh->set_filter_baking_aabb(godot::AABB(
            godot::Vector3(g.baking_aabb.min.x, g.baking_aabb.min.y, g.baking_aabb.min.z),
            godot::Vector3(g.baking_aabb.max.x - g.baking_aabb.min.x,
                           g.baking_aabb.max.y - g.baking_aabb.min.y,
                           g.baking_aabb.max.z - g.baking_aabb.min.z)));
        g.nav_mesh->set_filter_low_hanging_obstacles(config_.filter_low_hanging_obstacles);
        g.nav_mesh->set_filter_ledge_spans(false);
        g.nav_mesh->set_filter_walkable_low_height_spans(config_.filter_walkable_low_height_spans);
        g.source_data.instantiate();
#endif

        auto [ins_it, _] = groups_.emplace(gk, std::move(g));
        return ins_it->second;
    }

    bool initialized_ = false;
    bool bake_on_stream_enabled_ = true;
    ServerRid nav_map_rid_{};
    CaveNavMeshConfig config_{};
    uint32_t map_iteration_id_ = 1;
    uint64_t next_region_rid_ = 1000;

    std::unordered_map<NavigationGroupKey, NavigationGroupState, NavigationGroupKeyHash> groups_;
    std::unordered_map<ChunkKey, uint64_t, ChunkKeyHash> chunk_revisions_;
    std::unordered_map<ChunkKey, uint64_t, ChunkKeyHash> required_chunk_revisions_;
    std::unordered_set<ChunkKey, ChunkKeyHash> waiting_chunks_;
    std::vector<ChunkKey> newly_committed_chunks_;
};

// ============================================================================
// 3. Main-Thread `_process()` Dynamic Backpressure Loop (Sections 4.3, 5.2)
// ============================================================================
struct ProcessFrameStats {
    std::size_t records_popped = 0;
    std::size_t meshes_committed = 0;
    std::size_t edit_completions_processed = 0;
    uint64_t elapsed_us = 0;
};

template <typename PopFn, typename DepthFn, typename ClockUsFn>
inline ProcessFrameStats run_main_thread_process_frame(
    float upload_budget_ms,
    PopFn&& try_pop_completion,
    DepthFn&& completion_queue_depth,
    ClockUsFn&& get_ticks_usec,
    ServerSlotManager& server_slot_manager,
    VoxelNavigationManager& navigation_manager) {

    ProcessFrameStats stats{};
    const uint64_t frame_start_us = get_ticks_usec();
    const uint64_t soft_budget_us = static_cast<uint64_t>(upload_budget_ms * 1000.0f);
    const uint64_t emergency_budget_us = soft_budget_us * 2ULL;

    navigation_manager.poll_server_synchronization();

    while (true) {
        std::optional<CompletionRecord> done = try_pop_completion();
        if (!done.has_value()) {
            break;
        }
        ++stats.records_popped;

        switch (done->kind) {
            case CompletionKind::ChunkMesh:
                server_slot_manager.commit_chunk_mesh_and_collision(done->chunk_mesh);
                navigation_manager.note_chunk_mesh_committed(
                    done->chunk_mesh.key,
                    done->chunk_mesh.captured_terrain_revision,
                    server_slot_manager.get_cached_nav_faces(done->chunk_mesh.key));
                ++stats.meshes_committed;
                break;

            case CompletionKind::ChunkUnloaded:
                server_slot_manager.free_chunk_slot(done->unloaded_key);
                navigation_manager.note_chunk_unloaded(done->unloaded_key);
                break;

            case CompletionKind::EditTransactionCompleted:
                navigation_manager.apply_edit_transaction(done->edit_transaction);
                ++stats.edit_completions_processed;
                break;
        }

        const uint64_t elapsed_us = get_ticks_usec() - frame_start_us;
        stats.elapsed_us = elapsed_us;

        const std::size_t remaining_depth = completion_queue_depth();
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
    (void)navigation_manager.drain_newly_committed_chunks();
    return stats;
}

} // namespace voxel
