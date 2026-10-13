#pragma once

#include "persistence_and_streaming.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace voxel {

// ============================================================================
// 1. Direct Server RID Slot Manager (HLD v4.0 Sections 4.1, 4.4, 5.3)
// ============================================================================

/// Lightweight handle representing a Godot RenderingServer or PhysicsServer3D RID.
struct ServerRid {
    uint64_t id = 0;
    [[nodiscard]] constexpr bool is_valid() const noexcept { return id != 0; }
    constexpr bool operator==(const ServerRid&) const noexcept = default;
};

struct ChunkServerSlot {
    ServerRid mesh_rid{};
    ServerRid instance_rid{};
    ServerRid body_rid{};
    ServerRid shape_rid{};
    std::vector<Vec3f> cached_nav_faces; // Retained for 3D NavigationGroup rebakes
    uint64_t committed_version = 0;
    uint64_t committed_terrain_revision = 0;
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

    void set_commit_hook(CommitHookFn hook) {
        commit_hook_ = std::move(hook);
    }

    void commit_chunk_mesh_and_collision(const ChunkMeshPayload& mesh) {
        assert(initialized_);
        if (commit_hook_) {
            commit_hook_(mesh);
        }

        ChunkServerSlot& slot = slots_[mesh.key];
        if (!slot.mesh_rid.is_valid()) {
            slot.mesh_rid = allocate_rid();
            slot.instance_rid = allocate_rid();
            slot.body_rid = allocate_rid();
            slot.shape_rid = allocate_rid();
        }

        // Atomic swap of pre-unpacked triangle soup shared between PhysicsServer3D
        // concave polygon shape and cached_nav_faces (Section 4.1, 4.4).
        slot.cached_nav_faces = mesh.collision_faces;
        slot.committed_version = mesh.captured_version;
        slot.committed_terrain_revision = mesh.captured_terrain_revision;
    }

    void free_chunk_slot(ChunkKey key) noexcept {
        auto it = slots_.find(key);
        if (it != slots_.end()) {
            freed_rids_count_ += 4;
            slots_.erase(it);
        }
    }

    /// Step 7 of Ordered Teardown (Section 5.3): Free all RenderingServer & PhysicsServer3D RIDs.
    void free_all_rids() noexcept {
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

    [[nodiscard]] std::size_t active_slot_count() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t freed_rids_count() const noexcept { return freed_rids_count_; }

private:
    [[nodiscard]] ServerRid allocate_rid() noexcept {
        return ServerRid{++next_rid_counter_};
    }

    bool initialized_ = false;
    ServerRid scenario_rid_{};
    ServerRid space_rid_{};
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

    /// Horizontal border padding: P_xz = r_agent + r_connection + 2*s (Section 4.5)
    [[nodiscard]] constexpr float horizontal_padding() const noexcept {
        return agent_radius + region_connection_margin + 2.0f * voxel_scale;
    }

    /// Vertical border padding: P_y = h_agent + h_max_climb + 2*s (Section 4.5)
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

    void shutdown() noexcept {
        groups_.clear();
        chunk_revisions_.clear();
        waiting_chunks_.clear();
        newly_committed_chunks_.clear();
        initialized_ = false;
    }

    /// Computes the owned AABB for a 3D `4 x 2 x 4` navigation group:
    ///   [(128*gx, 64*gy, 128*gz)*s, (128*(gx+1), 64*(gy+1), 128*(gz+1))*s]
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

    /// Computes the padded baking AABB expanded outward by (P_xz, P_y, P_xz) (Section 4.5).
    [[nodiscard]] Aabb3f compute_baking_aabb(NavigationGroupKey key) const noexcept {
        const Aabb3f owned = compute_owned_aabb(key);
        const float pxz = config_.horizontal_padding();
        const float py = config_.vertical_padding();
        return Aabb3f{
            Vec3f{owned.min.x - pxz, owned.min.y - py, owned.min.z - pxz},
            Vec3f{owned.max.x + pxz, owned.max.y + py, owned.max.z + pxz}
        };
    }

    /// Called when a chunk mesh is committed on the main thread.
    void note_chunk_mesh_committed(ChunkKey key,
                                   uint64_t captured_terrain_revision,
                                   const std::vector<Vec3f>& /*faces*/) {
        chunk_revisions_[key] = captured_terrain_revision;
        waiting_chunks_.insert(key);

        // Touch owning 3D navigation group AND any adjacent 3D navigation group whose
        // 3D border padding halo (P_xz, P_y, P_xz) intersects this chunk's 32^3 AABB!
        const Aabb3f chunk_box = chunk_aabb(key);
        const NavigationGroupKey owner_gk = NavigationGroupKey::from_chunk_key(key);

        for (int32_t dgz = -1; dgz <= 1; ++dgz) {
            for (int32_t dgy = -1; dgy <= 1; ++dgy) {
                for (int32_t dgx = -1; dgx <= 1; ++dgx) {
                    const NavigationGroupKey candidate{
                        owner_gk.gx + dgx, owner_gk.gy + dgy, owner_gk.gz + dgz};
                    if (compute_baking_aabb(candidate).intersects(chunk_box)) {
                        // Only instantiate neighbor groups that already exist or are the owner group.
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
        waiting_chunks_.erase(key);
        const NavigationGroupKey owner_gk = NavigationGroupKey::from_chunk_key(key);
        auto it = groups_.find(owner_gk);
        if (it != groups_.end()) {
            it->second.desired_source_epoch += 1;
        }
    }

    /// Invalidates affected 3D `4 x 2 x 4` navigation groups when an EditTransaction completes.
    void apply_edit_transaction(const EditTransactionCompletedPayload& tx) {
        for (const NavigationGroupKey gk : tx.affected_nav_groups) {
            NavigationGroupState& g = get_or_create_group(gk);
            g.desired_terrain_revision = std::max(g.desired_terrain_revision, tx.terrain_revision);
        }
    }

    /// Submits ready dirty 3D navigation groups to NavigationServer3D async baking (< 0.2 ms).
    void submit_ready_groups_to_server(const ServerSlotManager& slot_mgr) {
        for (auto& [gk, g] : groups_) {
            if (g.rebuild_in_flight) {
                continue;
            }
            const bool dirty = (g.desired_terrain_revision > g.synced_terrain_revision) ||
                               (g.desired_source_epoch > g.synced_source_epoch);
            if (!dirty) {
                continue;
            }

            // Readiness Check (Section 4.5 Step 2):
            // Ensure all resident chunks in this group have committed meshes at >= desired_terrain_revision.
            bool all_chunks_ready = true;
            for (const auto& [ck, rev] : chunk_revisions_) {
                if (NavigationGroupKey::from_chunk_key(ck) == gk && rev < g.desired_terrain_revision) {
                    all_chunks_ready = false;
                    break;
                }
            }
            if (!all_chunks_ready) {
                continue;
            }

            // Fast Async Submission: clear source_data and gather cached_nav_faces from
            // the group's 32 owned chunks plus any border chunks intersecting baking_aabb.
            g.assembled_source_faces.clear();
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
                        if (! g.baking_aabb.intersects(chunk_aabb(ck))) {
                            continue;
                        }
                        const auto& faces = slot_mgr.get_cached_nav_faces(ck);
                        g.assembled_source_faces.insert(
                            g.assembled_source_faces.end(), faces.begin(), faces.end());
                    }
                }
            }

            g.rebuild_in_flight = true;
            g.bake_callback_received = false;
            g.submitted_terrain_revision = g.desired_terrain_revision;
            g.submitted_source_epoch = g.desired_source_epoch;
            g.total_bakes_submitted += 1;
        }
    }

    /// Simulates the background `bake_from_source_geometry_data_async` completion callback
    /// and subsequent `NavigationServer3D` map iteration synchronization (Section 4.5 Step 4).
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

        // Any waiting chunk whose owning 3D navigation group is now synced emits chunk_committed.
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

    /// Used by `VoxelAICompanion::is_path_region_synced(Vector3 position)` (Section 4.2).
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
        auto [ins_it, _] = groups_.emplace(gk, std::move(g));
        return ins_it->second;
    }

    bool initialized_ = false;
    ServerRid nav_map_rid_{};
    CaveNavMeshConfig config_{};
    uint32_t map_iteration_id_ = 1;
    uint64_t next_region_rid_ = 1000;

    std::unordered_map<NavigationGroupKey, NavigationGroupState, NavigationGroupKeyHash> groups_;
    std::unordered_map<ChunkKey, uint64_t, ChunkKeyHash> chunk_revisions_;
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

/// Executes the exact v4.0 `_process()` main-thread commit loop (Section 4.3, 5.2):
///   - Re-evaluates `currently_over_cap = (remaining_depth >= 32)` on EVERY iteration.
///   - Enforces `soft_budget_us` whenever `remaining_depth < 32`.
///   - Enforces `emergency_budget_us = 2 * soft_budget_us` unconditionally on every frame.
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

        // Dynamic per-iteration check (Section 4.3, 5.2):
        // Only bypass soft_budget_us while depth is at/above 32, and NEVER exceed emergency_budget_us.
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
