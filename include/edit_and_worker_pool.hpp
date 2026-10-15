#pragma once

#include "scratch_and_dirty.hpp"
#include "stage_timings.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace voxel {

// ============================================================================
// 1. 3D Cave Navigation Group Key (4 x 2 x 4 Chunks, HLD v4.0 Section 4.5)
// ============================================================================
struct NavigationGroupKey {
    int32_t gx = 0;
    int32_t gy = 0;
    int32_t gz = 0;

    [[nodiscard]] static constexpr NavigationGroupKey from_chunk_key(ChunkKey ck) noexcept {
        return NavigationGroupKey{
            ChunkKey::floor_div(ck.x(), 4),
            ChunkKey::floor_div(ck.y(), 2),
            ChunkKey::floor_div(ck.z(), 4)
        };
    }

    constexpr bool operator==(const NavigationGroupKey&) const noexcept = default;
    constexpr auto operator<=>(const NavigationGroupKey&) const noexcept = default;
};

struct NavigationGroupKeyHash {
    [[nodiscard]] constexpr std::size_t operator()(const NavigationGroupKey& k) const noexcept {
        const ChunkKey packed = ChunkKey::from_coords(k.gx, k.gy, k.gz);
        return static_cast<std::size_t>(packed.hash());
    }
};

// ============================================================================
// 2. Sphere CSG Primitives & Phased Edit Transaction (Sections 3.2, 3.4, 3.5)
// ============================================================================
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct SphereCsgTerm {
    Vec3f center{};
    float radius = 0.0f;
    bool is_fill = false;
    uint8_t fill_material = MaterialHardness::kMaterialSoil;
};

struct EditTransaction {
    static constexpr std::size_t kMaxCoalescedSpheres = 16;
    static constexpr std::size_t kMaxPhaseChunks = ChunkStore::kMaxEditPhaseCoreChunks; // 32

    std::vector<SphereCsgTerm> spheres;
    float truncation_delta = TsdfFormat::kDefaultTruncationDelta;

    IVec3 sample_min{};
    IVec3 sample_max{};

    std::vector<std::vector<ChunkKey>> phases;
    std::size_t next_phase_index = 0;

    std::unordered_map<ChunkKey, SelectiveDirtyMask, ChunkKeyHash> modified_chunk_masks;
    std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash> affected_nav_groups;
    std::size_t max_locks_acquired_in_any_phase = 0;

    [[nodiscard]] static EditTransaction create_from_spheres(
        std::vector<SphereCsgTerm> terms,
        float delta = TsdfFormat::kDefaultTruncationDelta,
        int32_t world_height_chunks = 8) {
        assert(!terms.empty() && terms.size() <= kMaxCoalescedSpheres);
        EditTransaction tx;
        tx.spheres = std::move(terms);
        tx.truncation_delta = delta;

        int32_t min_x = INT32_MAX, min_y = INT32_MAX, min_z = INT32_MAX;
        int32_t max_x = INT32_MIN, max_y = INT32_MIN, max_z = INT32_MIN;

        for (const SphereCsgTerm& s : tx.spheres) {
            assert(s.radius > 0.0f);
            const float r_eff = s.radius + delta;
            min_x = std::min(min_x, static_cast<int32_t>(std::floor(s.center.x - r_eff)));
            min_y = std::min(min_y, static_cast<int32_t>(std::floor(s.center.y - r_eff)));
            min_z = std::min(min_z, static_cast<int32_t>(std::floor(s.center.z - r_eff)));
            max_x = std::max(max_x, static_cast<int32_t>(std::ceil(s.center.x + r_eff)));
            max_y = std::max(max_y, static_cast<int32_t>(std::ceil(s.center.y + r_eff)));
            max_z = std::max(max_z, static_cast<int32_t>(std::ceil(s.center.z + r_eff)));
        }

        tx.sample_min = IVec3{min_x, min_y, min_z};
        tx.sample_max = IVec3{max_x, max_y, max_z};

        const int32_t cx_min = ChunkKey::floor_div(min_x, 32);
        const int32_t cy_min = std::max<int32_t>(0, ChunkKey::floor_div(min_y, 32));
        const int32_t cz_min = ChunkKey::floor_div(min_z, 32);

        const int32_t cx_max = ChunkKey::floor_div(max_x, 32);
        const int32_t cy_max = std::min<int32_t>(world_height_chunks - 1, ChunkKey::floor_div(max_y, 32));
        const int32_t cz_max = ChunkKey::floor_div(max_z, 32);

        for (int32_t gz = ChunkKey::floor_div(cz_min, 4); gz <= ChunkKey::floor_div(cz_max, 4); ++gz) {
            for (int32_t gy = ChunkKey::floor_div(cy_min, 2); gy <= ChunkKey::floor_div(cy_max, 2); ++gy) {
                for (int32_t gx = ChunkKey::floor_div(cx_min, 4); gx <= ChunkKey::floor_div(cx_max, 4); ++gx) {
                    std::vector<ChunkKey> block_phase;
                    for (int32_t dz = 0; dz < 4; ++dz) {
                        const int32_t cz = gz * 4 + dz;
                        if (cz < cz_min || cz > cz_max) continue;
                        for (int32_t dy = 0; dy < 2; ++dy) {
                            const int32_t cy = gy * 2 + dy;
                            if (cy < cy_min || cy > cy_max) continue;
                            for (int32_t dx = 0; dx < 4; ++dx) {
                                const int32_t cx = gx * 4 + dx;
                                if (cx < cx_min || cx > cx_max) continue;
                                block_phase.push_back(ChunkKey::from_coords(cx, cy, cz));
                            }
                        }
                    }
                    if (!block_phase.empty()) {
                        std::sort(block_phase.begin(), block_phase.end());
                        assert(block_phase.size() <= kMaxPhaseChunks);
                        tx.phases.push_back(std::move(block_phase));
                    }
                }
            }
        }

        if (tx.phases.empty()) {
            tx.phases.emplace_back();
        }
        return tx;
    }

    [[nodiscard]] bool is_finished() const noexcept {
        return next_phase_index >= phases.size();
    }
};

// ============================================================================
// 3. MPSC Completion Queue Records (HLD v4.0 Section 5.2)
// ============================================================================
enum class CompletionKind : uint8_t {
    ChunkMesh = 0,
    ChunkUnloaded = 1,
    EditTransactionCompleted = 2
};

struct ChunkMeshPayload {
    ChunkKey key{};
    uint64_t captured_version = 0;
    uint64_t captured_terrain_revision = 0;
    std::vector<Vec3f> vertices;
    std::vector<Vec3f> normals;
    // Per-vertex attributes: x = damage_ratio in [0, 1], y = is_hard_rock (0 or 1),
    // z = hits-to-break / 255 (3/255 hard rock, 5/255 boulder, else 1/255). The shader keys
    // boulder-vs-hard-rock material separation off z (hits-to-break), not y.
    std::vector<Vec3f> colors;
    std::vector<int32_t> indices;
    std::vector<Vec3f> collision_faces;
    std::size_t reserved_ledger_bytes = 0;
};

struct EditTransactionCompletedPayload {
    Vec3f position{};
    float radius = 0.0f;
    /// True when the completed transaction was an add-terrain / fill (so the Godot
    /// wrapper can emit terrain_add_completed instead of explosion_completed).
    bool is_fill = false;
    uint64_t terrain_revision = 0;
    std::size_t total_affected_chunks = 0;
    std::vector<ChunkKey> dirtied_chunks;
    std::vector<NavigationGroupKey> affected_nav_groups;
};

struct CompletionRecord {
    CompletionKind kind = CompletionKind::EditTransactionCompleted;
    ChunkMeshPayload chunk_mesh{};
    ChunkKey unloaded_key{};
    EditTransactionCompletedPayload edit_transaction{};
};

// ============================================================================
// 4. 26-Neighbor Closure Readiness Check & Phased CSG + Persistent Damage Executor
// ============================================================================

[[nodiscard]] inline bool are_all_in_bounds_neighbors_resident(
    const ChunkStore& store,
    ChunkKey center) noexcept {
    const int32_t world_h = store.world_height_chunks();
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            const int32_t ny = center.y() + dy;
            if (ny < 0 || ny >= world_h) {
                continue;
            }
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const ChunkKey nk = center.offset(dx, dy, dz);
                const ChunkSlot* slot = store.find_slot(nk);
                if (!slot || slot->state != ChunkLifecycleState::Resident) {
                    return false;
                }
            }
        }
    }
    return true;
}

inline std::optional<EditTransactionCompletedPayload> execute_next_edit_phase(
    ChunkStore& store,
    DualDirtyScheduler& dirty_scheduler,
    EditTransaction& tx,
    const std::function<void(ChunkStore&, ChunkKey)>& generate_fn = {}) {

    assert(!tx.is_finished());
    const std::size_t phase_idx = tx.next_phase_index++;
    const std::vector<ChunkKey>& phase_keys = tx.phases[phase_idx];

    bool has_any_fill = false;
    for (const SphereCsgTerm& s : tx.spheres) {
        if (s.is_fill) {
            has_any_fill = true;
            break;
        }
    }

    if (!phase_keys.empty()) {
        for (const ChunkKey ck : phase_keys) {
            ChunkSlot* slot = store.get_or_create_slot(ck);
            bool needs_init = false;
            {
                std::shared_lock<std::shared_mutex> read_lock(slot->mutex);
                needs_init = (slot->state == ChunkLifecycleState::Absent ||
                              slot->state == ChunkLifecycleState::Requested);
            }
            if (needs_init) {
                if (generate_fn) {
                    generate_fn(store, ck);
                } else {
                    std::unique_lock<std::shared_mutex> init_lock(slot->mutex);
                    if (slot->state == ChunkLifecycleState::Absent ||
                        slot->state == ChunkLifecycleState::Requested) {
                        // Fill transactions seed absent space as EMPTY so air outside the
                        // placed sphere stays air; carves seed SOLID as before.
                        const ChunkStorageForm seed_form =
                            has_any_fill ? ChunkStorageForm::Empty : ChunkStorageForm::Solid;
                        slot->reset_to_uniform(seed_form, 1, store.tsdf_pool(), store.material_pool());
                        slot->version.store(1, std::memory_order_release);
                        slot->state = ChunkLifecycleState::Resident;
                    }
                }
            }
        }

        ChunkStore::ScopedPhaseWriteLock phase_lock(store, phase_keys);
        tx.max_locks_acquired_in_any_phase =
            std::max(tx.max_locks_acquired_in_any_phase, phase_lock.size());

        const float delta = tx.truncation_delta;

        auto eval_carve_spheres = [&](float px, float py, float pz, int* out_interior_hits) noexcept -> float {
            float d_blast = -1e30f;
            int hits = 0;
            for (const SphereCsgTerm& s : tx.spheres) {
                if (s.is_fill) continue;
                const float dx = px - s.center.x;
                const float dy = py - s.center.y;
                const float dz = pz - s.center.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float d = s.radius - dist;
                d_blast = std::max(d_blast, d);
                // Strictly count hits inside the sphere radius R (d >= 0.0f) for damage,
                // NEVER in the outer (-delta, 0) TSDF wall smoothing band.
                if (d >= 0.0f) {
                    ++hits;
                }
            }
            if (out_interior_hits) {
                *out_interior_hits = hits;
            }
            return d_blast;
        };

        for (ChunkSlot* slot : phase_lock.slots()) {
            if (slot->form == ChunkStorageForm::Empty && !has_any_fill) {
                continue;
            }

            const IVec3 chunk_min = slot->key.min_world_sample();
            const int32_t lx0 = std::max<int32_t>(0, tx.sample_min.x - chunk_min.x);
            const int32_t ly0 = std::max<int32_t>(0, tx.sample_min.y - chunk_min.y);
            const int32_t lz0 = std::max<int32_t>(0, tx.sample_min.z - chunk_min.z);

            const int32_t lx1 = std::min<int32_t>(31, tx.sample_max.x - chunk_min.x);
            const int32_t ly1 = std::min<int32_t>(31, tx.sample_max.y - chunk_min.y);
            const int32_t lz1 = std::min<int32_t>(31, tx.sample_max.z - chunk_min.z);

            bool chunk_modified = false;
            SelectiveDirtyMask local_dirty_mask{};

            // Per-voxel mask of hard-rock samples in this chunk that reached break_threshold (255)
            // on this transaction. Using a per-voxel mask instead of a sphere-wide boolean ensures
            // that when a 3rd click partially overlaps a 2-hit region, ONLY the 3-hit intersection
            // carves to air while the 1-hit and 2-hit parts of the 3rd click stay solid.
            std::array<uint8_t, TsdfFormat::kChunkSamples> hard_rock_carved{};
            bool any_hard_rock_carved = false;

            // Pass 1: Apply fills, carve soft terrain immediately, and accumulate per-voxel hard-rock
            // damage strictly inside d >= 0.0f (radius R).
            for (int32_t lz = lz0; lz <= lz1; ++lz) {
                const float pz = static_cast<float>(chunk_min.z + lz);
                for (int32_t ly = ly0; ly <= ly1; ++ly) {
                    const float py = static_cast<float>(chunk_min.y + ly);
                    for (int32_t lx = lx0; lx <= lx1; ++lx) {
                        const float px = static_cast<float>(chunk_min.x + lx);

                        const LocalCoord lc{lx, ly, lz};

                        // 1. Handle Fill terms first: place solid material and always reset damage to 0
                        float d_fill_min = +1e30f;
                        uint8_t chosen_fill_mat = MaterialHardness::kMaterialSoil;
                        bool touched_by_fill = false;

                        for (const SphereCsgTerm& s : tx.spheres) {
                            if (!s.is_fill) continue;
                            const float dx = px - s.center.x;
                            const float dy = py - s.center.y;
                            const float dz = pz - s.center.z;
                            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                            const float d_fill = dist - s.radius; // Negative inside sphere (solid)
                            if (d_fill < delta) {
                                touched_by_fill = true;
                                if (d_fill < d_fill_min) {
                                    d_fill_min = d_fill;
                                    chosen_fill_mat = s.fill_material;
                                }
                            }
                        }

                        if (touched_by_fill) {
                            const int16_t q_cur = slot->get_tsdf(lc);
                            const int16_t q_fill = TsdfFormat::encode(d_fill_min, delta);
                            const int16_t q_new = std::min(q_cur, q_fill);
                            if (q_new != q_cur) {
                                bool changed = false;
                                if (slot->set_tsdf(lc, q_new, store.tsdf_pool(), &changed) && changed) {
                                    chunk_modified = true;
                                    local_dirty_mask.record_mutated_sample(lc);
                                }
                            }
                            if (d_fill_min <= 0.0f) {
                                (void)slot->set_material(lc, chosen_fill_mat, store.material_pool());
                            }
                            if (slot->clear_damage(lc)) {
                                chunk_modified = true;
                                local_dirty_mask.record_mutated_sample(lc);
                            }
                        }

                        // 2. Handle Carve / Blast terms
                        int spheres_hitting_interior = 0;
                        const float d_blast = eval_carve_spheres(px, py, pz, &spheres_hitting_interior);

                        if (d_blast > -delta) {
                            const int16_t q_cur = slot->get_tsdf(lc);
                            if (q_cur == TsdfFormat::kClampedAir) {
                                if (slot->clear_damage(lc)) {
                                    chunk_modified = true;
                                    local_dirty_mask.record_mutated_sample(lc);
                                }
                                continue;
                            }

                            const uint8_t mat_id = slot->get_material(lc);

                            // Soft terrain (soil/grass) breaks in 1 hit: never write persistent damage to the
                            // R + delta influence band (which leaves the (-delta, 0) wall skin solid).
                            if (!MaterialHardness::supports_persistent_damage(mat_id)) {
                                if (slot->clear_damage(lc)) {
                                    chunk_modified = true;
                                    local_dirty_mask.record_mutated_sample(lc);
                                }
                                const int16_t q_blast = TsdfFormat::encode(d_blast, delta);
                                const int16_t q_new = std::max(q_cur, q_blast);
                                if (q_new != q_cur) {
                                    bool changed = false;
                                    if (slot->set_tsdf(lc, q_new, store.tsdf_pool(), &changed) && changed) {
                                        chunk_modified = true;
                                        local_dirty_mask.record_mutated_sample(lc);
                                    }
                                }
                                continue;
                            }

                            // Multi-hit Hard Rock (kMaterialHardRock):
                            // Only samples inside the actual sphere radius (d >= 0.0f) accumulate damage.
                            if (spheres_hitting_interior > 0) {
                                const uint8_t threshold = MaterialHardness::break_threshold(mat_id);
                                const uint32_t added_dmg = std::min<uint32_t>(
                                    255U,
                                    static_cast<uint32_t>(spheres_hitting_interior) *
                                        static_cast<uint32_t>(MaterialHardness::damage_per_hit(mat_id)));
                                bool dmg_changed = false;
                                const uint8_t total_dmg = slot->add_damage(
                                    lc, static_cast<uint8_t>(added_dmg), store.material_pool(), &dmg_changed);
                                if (dmg_changed) {
                                    chunk_modified = true;
                                    local_dirty_mask.record_mutated_sample(lc);
                                }

                                // Strictly per-voxel break check: ONLY voxels whose own cumulative damage
                                // reaches 255 break. Voxels in the same click that only have 85 (1 hit) or
                                // 170 (2 hits) stay solid!
                                if (total_dmg >= threshold) {
                                    hard_rock_carved[lc.to_linear_index()] = 1U;
                                    any_hard_rock_carved = true;
                                    if (slot->clear_damage(lc)) {
                                        chunk_modified = true;
                                        local_dirty_mask.record_mutated_sample(lc);
                                    }
                                    if (q_cur >= 0) {
                                        const int16_t q_blast = TsdfFormat::encode(d_blast, delta);
                                        const int16_t q_new = std::max(q_cur, q_blast);
                                        if (q_new != q_cur) {
                                            bool changed = false;
                                            if (slot->set_tsdf(lc, q_new, store.tsdf_pool(), &changed) && changed) {
                                                chunk_modified = true;
                                                local_dirty_mask.record_mutated_sample(lc);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Pass 2: If any hard-rock voxels broke on this hit, update the TSDF for the carved subset
            // and smooth the negative TSDF wall gradient on surviving solid neighbors within delta.
            if (any_hard_rock_carved) {
                struct PendingTsdfUpdate {
                    LocalCoord lc;
                    int16_t q_new;
                    bool clear_underground_damage;
                };
                std::vector<PendingTsdfUpdate> updates;
                updates.reserve(512);

                const int32_t search_r = static_cast<int32_t>(std::ceil(delta)) + 1;

                for (int32_t lz = lz0; lz <= lz1; ++lz) {
                    const float pz = static_cast<float>(chunk_min.z + lz);
                    for (int32_t ly = ly0; ly <= ly1; ++ly) {
                        const float py = static_cast<float>(chunk_min.y + ly);
                        for (int32_t lx = lx0; lx <= lx1; ++lx) {
                            const float px = static_cast<float>(chunk_min.x + lx);
                            const LocalCoord lc{lx, ly, lz};
                            const uint32_t lin = lc.to_linear_index();

                            const uint8_t mat_id = slot->get_material(lc);
                            if (!MaterialHardness::supports_persistent_damage(mat_id)) {
                                continue;
                            }

                            const int16_t q_cur = slot->get_tsdf(lc);
                            const float d_blast = eval_carve_spheres(px, py, pz, nullptr);
                            if (d_blast <= -delta) {
                                continue;
                            }

                            if (hard_rock_carved[lin] != 0U) {
                                if (q_cur >= 0) {
                                    continue;
                                }
                                // Carved hard-rock voxel (becomes air): if an uncarved 1-hit/2-hit solid voxel
                                // inside the sphere is adjacent to this voxel, taper the positive air TSDF toward
                                // the boundary between the 3-hit carved pocket and the surviving solid rock.
                                float min_dist_sq_to_interior_solid = 1e30f;
                                for (int32_t dz = -search_r; dz <= search_r; ++dz) {
                                    const int32_t nz = lz + dz;
                                    if (nz < 0 || nz >= 32) continue;
                                    for (int32_t dy = -search_r; dy <= search_r; ++dy) {
                                        const int32_t ny = ly + dy;
                                        if (ny < 0 || ny >= 32) continue;
                                        for (int32_t dx = -search_r; dx <= search_r; ++dx) {
                                            const int32_t nx = lx + dx;
                                            if (nx < 0 || nx >= 32) continue;
                                            const LocalCoord n_lc{nx, ny, nz};
                                            const uint32_t n_lin = n_lc.to_linear_index();
                                            if (hard_rock_carved[n_lin] == 0U && slot->get_tsdf(n_lc) < 0) {
                                                const float npx = static_cast<float>(chunk_min.x + nx);
                                                const float npy = static_cast<float>(chunk_min.y + ny);
                                                const float npz = static_cast<float>(chunk_min.z + nz);
                                                int n_hits = 0;
                                                (void)eval_carve_spheres(npx, npy, npz, &n_hits);
                                                if (n_hits > 0) {
                                                    const float d2 = static_cast<float>(dx * dx + dy * dy + dz * dz);
                                                    min_dist_sq_to_interior_solid =
                                                        std::min(min_dist_sq_to_interior_solid, d2);
                                                }
                                            }
                                        }
                                    }
                                }

                                float d_carve = d_blast;
                                if (min_dist_sq_to_interior_solid < 1e20f) {
                                    const float dist_to_solid = std::sqrt(min_dist_sq_to_interior_solid);
                                    d_carve = std::min(d_blast, std::max(0.05f, dist_to_solid - 0.5f));
                                }
                                const int16_t q_blast =
                                    std::max<int16_t>(1, TsdfFormat::encode(d_carve, delta));
                                const int16_t q_new = std::max(q_cur, q_blast);
                                if (q_new != q_cur) {
                                    updates.push_back(PendingTsdfUpdate{lc, q_new, false});
                                }
                            } else if (q_cur < 0) {
                                // Surviving solid hard-rock voxel (either outside the sphere in (-delta, 0),
                                // or inside the sphere with only 1 or 2 hits): check distance to the nearest
                                // newly carved hard-rock voxel.
                                float min_dist_sq_to_carved = 1e30f;
                                for (int32_t dz = -search_r; dz <= search_r; ++dz) {
                                    const int32_t nz = lz + dz;
                                    if (nz < 0 || nz >= 32) continue;
                                    for (int32_t dy = -search_r; dy <= search_r; ++dy) {
                                        const int32_t ny = ly + dy;
                                        if (ny < 0 || ny >= 32) continue;
                                        for (int32_t dx = -search_r; dx <= search_r; ++dx) {
                                            const int32_t nx = lx + dx;
                                            if (nx < 0 || nx >= 32) continue;
                                            const LocalCoord n_lc{nx, ny, nz};
                                            if (hard_rock_carved[n_lc.to_linear_index()] != 0U &&
                                                slot->get_tsdf(n_lc) < 0) {
                                                const float d2 = static_cast<float>(dx * dx + dy * dy + dz * dz);
                                                min_dist_sq_to_carved = std::min(min_dist_sq_to_carved, d2);
                                            }
                                        }
                                    }
                                }

                                if (min_dist_sq_to_carved <= (delta + 0.5f) * (delta + 0.5f)) {
                                    const float dist_to_carved = std::sqrt(min_dist_sq_to_carved);
                                    const float d_grid_wall = -std::max(0.05f, dist_to_carved - 0.5f);
                                    const float d_wall =
                                        (d_blast < 0.0f) ? d_blast : d_grid_wall;
                                    // Strictly negative (<= -1) so surviving 1-hit/2-hit rock NEVER turns into air!
                                    const int16_t q_wall =
                                        std::min<int16_t>(-1, TsdfFormat::encode(d_wall, delta));
                                    const int16_t q_new = std::max(q_cur, q_wall);
                                    // If this voxel was deep underground (kClampedSolid) before the carve and is
                                    // now exposed as the new crater floor, clear any underground damage so the
                                    // rock underneath a 3-hit crater starts fresh at 0 hits; surface rock
                                    // (q_cur > kClampedSolid) keeps its 1-hit and 2-hit cracks!
                                    const bool was_deep_underground = (q_cur == TsdfFormat::kClampedSolid);
                                    if (q_new != q_cur || was_deep_underground) {
                                        updates.push_back(
                                            PendingTsdfUpdate{lc, q_new, was_deep_underground});
                                    }
                                }
                            }
                        }
                    }
                }

                for (const PendingTsdfUpdate& u : updates) {
                    if (u.q_new != slot->get_tsdf(u.lc)) {
                        bool changed = false;
                        if (slot->set_tsdf(u.lc, u.q_new, store.tsdf_pool(), &changed) && changed) {
                            chunk_modified = true;
                            local_dirty_mask.record_mutated_sample(u.lc);
                        }
                    }
                    if (u.clear_underground_damage && slot->clear_damage(u.lc)) {
                        chunk_modified = true;
                        local_dirty_mask.record_mutated_sample(u.lc);
                    }
                }
            }

            if (chunk_modified) {
                slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
                slot->dirty_persist = true;
                slot->version.fetch_add(1, std::memory_order_release);

                tx.modified_chunk_masks[slot->key].mask |= local_dirty_mask.mask;
                tx.affected_nav_groups.insert(NavigationGroupKey::from_chunk_key(slot->key));
            }
        }
    }

    if (!tx.is_finished()) {
        return std::nullopt;
    }

    for (auto& [ck, dirty_mask] : tx.modified_chunk_masks) {
        ChunkSlot* slot = store.find_slot(ck);
        if (!slot) {
            continue;
        }
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        if (slot->form != ChunkStorageForm::Dense) {
            continue;
        }

        std::vector<LocalCoord> crumbs_to_prune;
        for (int32_t lz = 0; lz < 32; ++lz) {
            for (int32_t ly = 0; ly < 32; ++ly) {
                for (int32_t lx = 0; lx < 32; ++lx) {
                    const LocalCoord lc{lx, ly, lz};
                    const int16_t q = slot->get_tsdf(lc);
                    if (q > TsdfFormat::kClampedSolid && q < 0) {
                        auto sample_at = [&](int32_t nx, int32_t ny, int32_t nz) -> int16_t {
                            if (nx >= 0 && nx < 32 && ny >= 0 && ny < 32 && nz >= 0 && nz < 32) {
                                return slot->get_tsdf(LocalCoord{nx, ny, nz});
                            }
                            return -1;
                        };

                        if (sample_at(lx - 1, ly, lz) >= 0 &&
                            sample_at(lx + 1, ly, lz) >= 0 &&
                            sample_at(lx, ly - 1, lz) >= 0 &&
                            sample_at(lx, ly + 1, lz) >= 0 &&
                            sample_at(lx, ly, lz - 1) >= 0 &&
                            sample_at(lx, ly, lz + 1) >= 0) {
                            crumbs_to_prune.push_back(lc);
                        }
                    }
                }
            }
        }

        if (!crumbs_to_prune.empty()) {
            for (const LocalCoord& lc : crumbs_to_prune) {
                (void)slot->set_tsdf(lc, TsdfFormat::kClampedAir, store.tsdf_pool());
                (void)slot->clear_damage(lc);
                dirty_mask.record_mutated_sample(lc);
            }
            slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
            slot->version.fetch_add(1, std::memory_order_release);
        }
    }

    uint64_t new_revision = store.terrain_revision();
    std::vector<ChunkKey> deduplicated_dirty;

    if (!tx.modified_chunk_masks.empty()) {
        new_revision = store.increment_terrain_revision();
        std::unordered_set<ChunkKey, ChunkKeyHash> unique_dirty;
        for (const auto& [ck, mask] : tx.modified_chunk_masks) {
            std::vector<ChunkKey> emitted;
            mask.emit_dirty_keys(ck, emitted);
            for (const ChunkKey dk : emitted) {
                unique_dirty.insert(dk);
                tx.affected_nav_groups.insert(NavigationGroupKey::from_chunk_key(dk));
            }
        }
        deduplicated_dirty.assign(unique_dirty.begin(), unique_dirty.end());
        std::sort(deduplicated_dirty.begin(), deduplicated_dirty.end());

        if (generate_fn) {
            const int32_t world_h = store.world_height_chunks();
            for (const ChunkKey dk : deduplicated_dirty) {
                for (int32_t dz = -1; dz <= 1; ++dz) {
                    for (int32_t dy = -1; dy <= 1; ++dy) {
                        const int32_t ny = dk.y() + dy;
                        if (ny < 0 || ny >= world_h) continue;
                        for (int32_t dx = -1; dx <= 1; ++dx) {
                            const ChunkKey nk = dk.offset(dx, dy, dz);
                            const ChunkSlot* nslot = store.find_slot(nk);
                            if (!nslot || nslot->state != ChunkLifecycleState::Resident) {
                                generate_fn(store, nk);
                            }
                        }
                    }
                }
            }
        }

        dirty_scheduler.publish_edit_transaction_dirty_batch(deduplicated_dirty);
    }

    EditTransactionCompletedPayload payload{};
    if (!tx.spheres.empty()) {
        payload.position = tx.spheres.front().center;
        payload.radius = tx.spheres.front().radius;
    }
    payload.is_fill = has_any_fill;
    payload.terrain_revision = new_revision;
    payload.total_affected_chunks = tx.modified_chunk_masks.size();
    payload.dirtied_chunks = std::move(deduplicated_dirty);
    payload.affected_nav_groups.assign(tx.affected_nav_groups.begin(), tx.affected_nav_groups.end());
    std::sort(payload.affected_nav_groups.begin(), payload.affected_nav_groups.end());
    return payload;
}

// ============================================================================
// 5. Core Worker Pool with 26-Neighbor Closure Gating (Sections 3.3, 3.4, 5.3)
// ============================================================================
class CoreWorkerPool {
public:
    static constexpr std::size_t kEditStarvationBound = 8;
    static constexpr std::size_t kMaxPendingEditTransactions = 64;

    using MeshGeneratorFn = std::function<ChunkMeshPayload(
        const ChunkStore&, ChunkKey, WorkerScratchWindow36&, const NeighborhoodVersionStamp&)>;
    using BackgroundGenerateFn = std::function<void(ChunkStore&, ChunkKey)>;
    using ShutdownSaveHookFn = std::function<void(ChunkStore&)>;

    CoreWorkerPool(ChunkStore& store,
                   DualDirtyScheduler& dirty_scheduler,
                   MeshGeneratorFn mesh_fn = {},
                   BackgroundGenerateFn generate_fn = {})
        : store_(store),
          dirty_scheduler_(dirty_scheduler),
          mesh_fn_(std::move(mesh_fn)),
          generate_fn_(std::move(generate_fn)) {}

    ~CoreWorkerPool() {
        if (running_.load(std::memory_order_acquire)) {
            shutdown(/*save=*/false);
        }
    }

    CoreWorkerPool(const CoreWorkerPool&) = delete;
    CoreWorkerPool& operator=(const CoreWorkerPool&) = delete;

    void set_background_generate_fn(BackgroundGenerateFn fn) {
        generate_fn_ = std::move(fn);
    }

    void set_shutdown_save_hook(ShutdownSaveHookFn hook) {
        shutdown_save_hook_ = std::move(hook);
    }

    /// Optional per-stage timing sink. Pass nullptr to disable (default).
    void set_stage_timings(StageTimings* timings) noexcept { stage_timings_ = timings; }

    void start(std::size_t num_workers = 0) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        shutting_down_.store(false, std::memory_order_release);

        if (num_workers == 0) {
            const unsigned int hc = std::thread::hardware_concurrency();
            num_workers = (hc > 2U) ? static_cast<std::size_t>(hc - 2U) : 2ULL;
        }

        workers_.reserve(num_workers);
        for (std::size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    void request_background_generate(ChunkKey key, bool in_active_set) {
        {
            std::lock_guard<std::mutex> lock(generate_mutex_);
            if (in_active_set) {
                awaiting_closure_mesh_set_.insert(key);
            }
            if (pending_generate_set_.find(key) == pending_generate_set_.end()) {
                pending_generate_set_.insert(key);
                generate_queue_.push_back(GenerateRequest{key, in_active_set});
            }
        }
        cv_.notify_one();
    }

    void register_awaiting_closure_mesh(ChunkKey key) {
        if (are_all_in_bounds_neighbors_resident(store_, key)) {
            dirty_scheduler_.mark_stream_dirty(key);
            return;
        }
        std::lock_guard<std::mutex> lock(generate_mutex_);
        awaiting_closure_mesh_set_.insert(key);
    }

    [[nodiscard]] bool enqueue_explosion(Vec3f position, float radius) {
        return enqueue_sphere_transaction(/*is_fill=*/false, position, radius,
                                          MaterialHardness::kMaterialSoil);
    }

    /// Add-terrain / boulder placement: fills a sphere with the selected material.
    /// Fill transactions never coalesce with explosions or fills of another material.
    [[nodiscard]] bool enqueue_fill_spheres(Vec3f position,
                                            float radius,
                                            uint8_t material = MaterialHardness::kMaterialSoil) {
        return enqueue_sphere_transaction(/*is_fill=*/true, position, radius, material);
    }

    [[nodiscard]] std::optional<CompletionRecord> try_pop_completion() {
        std::optional<CompletionRecord> out;
        {
            std::lock_guard<std::mutex> lock(completion_mutex_);
            if (completion_queue_.empty()) {
                return std::nullopt;
            }
            out = std::move(completion_queue_.front());
            completion_queue_.pop_front();
            completion_depth_.store(completion_queue_.size(), std::memory_order_release);
        }
        if (out->kind == CompletionKind::ChunkMesh && out->chunk_mesh.reserved_ledger_bytes > 0) {
            store_.ledger().release(out->chunk_mesh.reserved_ledger_bytes, MemoryPoolId::CompletionQueue);
            out->chunk_mesh.reserved_ledger_bytes = 0;
        }
        cv_.notify_all();
        return out;
    }

    [[nodiscard]] std::size_t completion_queue_depth() const noexcept {
        return completion_depth_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool core_idle() const noexcept {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::lock_guard<std::mutex> gen_lock(generate_mutex_);
        return edit_queue_.empty() &&
               generate_queue_.empty() &&
               active_workers_.load(std::memory_order_acquire) == 0 &&
               dirty_scheduler_.edit_dirty_count() == 0 &&
               dirty_scheduler_.stream_dirty_count() == 0;
    }

    void shutdown(bool save) {
        shutting_down_.store(true, std::memory_order_release);
        cv_.notify_all();

        for (std::thread& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        workers_.clear();
        running_.store(false, std::memory_order_release);

        if (save && shutdown_save_hook_) {
            shutdown_save_hook_(store_);
        }

        {
            std::lock_guard<std::mutex> lock(completion_mutex_);
            while (!completion_queue_.empty()) {
                CompletionRecord& rec = completion_queue_.front();
                if (rec.kind == CompletionKind::ChunkMesh && rec.chunk_mesh.reserved_ledger_bytes > 0) {
                    store_.ledger().release(rec.chunk_mesh.reserved_ledger_bytes, MemoryPoolId::CompletionQueue);
                }
                completion_queue_.pop_front();
            }
            completion_depth_.store(0, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            edit_queue_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(generate_mutex_);
            generate_queue_.clear();
            pending_generate_set_.clear();
            awaiting_closure_mesh_set_.clear();
        }

        store_.clear_all();
        assert(store_.ledger().pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 0ULL);
        assert(store_.ledger().pool_reserved_bytes(MemoryPoolId::WorkerScratch) == 0ULL);
        assert(store_.ledger().pool_reserved_bytes(MemoryPoolId::CompletionQueue) == 0ULL);
    }

private:
    struct GenerateRequest {
        ChunkKey key{};
        bool in_active_set = false;
    };

    [[nodiscard]] bool enqueue_sphere_transaction(bool is_fill,
                                                  Vec3f position,
                                                  float radius,
                                                  uint8_t fill_material) {
        if (radius <= 0.0f ||
            !running_.load(std::memory_order_acquire) ||
            shutting_down_.load(std::memory_order_acquire)) {
            return false;
        }

        SphereCsgTerm term{};
        term.center = position;
        term.radius = radius;
        term.is_fill = is_fill;
        term.fill_material = fill_material;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (shutting_down_.load(std::memory_order_relaxed)) {
                return false;
            }
            // Coalesce only with a not-yet-started transaction of the same kind, so a
            // fill never merges into an explosion blast (or a fill of another material).
            const auto back_is_compatible = [&](const EditTransaction& back) noexcept {
                if (back.next_phase_index != 0 ||
                    back.spheres.size() >= EditTransaction::kMaxCoalescedSpheres) {
                    return false;
                }
                for (const SphereCsgTerm& s : back.spheres) {
                    if (s.is_fill != is_fill) {
                        return false;
                    }
                    if (is_fill && s.fill_material != fill_material) {
                        return false;
                    }
                }
                return true;
            };
            if (!edit_queue_.empty() && back_is_compatible(edit_queue_.back())) {
                std::vector<SphereCsgTerm> combined = edit_queue_.back().spheres;
                combined.push_back(term);
                edit_queue_.back() = EditTransaction::create_from_spheres(
                    std::move(combined),
                    TsdfFormat::kDefaultTruncationDelta,
                    store_.world_height_chunks());
            } else {
                if (edit_queue_.size() >= kMaxPendingEditTransactions) {
                    return false;
                }
                edit_queue_.push_back(EditTransaction::create_from_spheres(
                    {term},
                    TsdfFormat::kDefaultTruncationDelta,
                    store_.world_height_chunks()));
            }
        }
        cv_.notify_all();
        return true;
    }

    void push_completion(CompletionRecord rec) {
        if (rec.kind == CompletionKind::ChunkMesh) {
            const std::size_t bytes =
                rec.chunk_mesh.vertices.size() * sizeof(Vec3f) +
                rec.chunk_mesh.normals.size() * sizeof(Vec3f) +
                rec.chunk_mesh.colors.size() * sizeof(Vec3f) +
                rec.chunk_mesh.indices.size() * sizeof(int32_t) +
                rec.chunk_mesh.collision_faces.size() * sizeof(Vec3f);
            if (bytes > 0 && store_.ledger().try_reserve(bytes, MemoryPoolId::CompletionQueue)) {
                rec.chunk_mesh.reserved_ledger_bytes = bytes;
            }
        }
        std::lock_guard<std::mutex> lock(completion_mutex_);
        completion_queue_.push_back(std::move(rec));
        completion_depth_.store(completion_queue_.size(), std::memory_order_release);
    }

    void promote_ready_neighbors_to_stream_dirty(ChunkKey generated_key) {
        std::vector<ChunkKey> newly_ready;
        {
            std::lock_guard<std::mutex> lock(generate_mutex_);
            if (awaiting_closure_mesh_set_.empty()) {
                return;
            }
            for (int32_t dz = -1; dz <= 1; ++dz) {
                for (int32_t dy = -1; dy <= 1; ++dy) {
                    for (int32_t dx = -1; dx <= 1; ++dx) {
                        const ChunkKey candidate = generated_key.offset(dx, dy, dz);
                        auto it = awaiting_closure_mesh_set_.find(candidate);
                        if (it != awaiting_closure_mesh_set_.end() &&
                            are_all_in_bounds_neighbors_resident(store_, candidate)) {
                            newly_ready.push_back(candidate);
                            awaiting_closure_mesh_set_.erase(it);
                        }
                    }
                }
            }
        }
        for (const ChunkKey rk : newly_ready) {
            dirty_scheduler_.mark_stream_dirty(rk);
        }
    }

    void worker_loop() {
        WorkerScratchWindow36 scratch(store_.ledger());
        std::size_t consecutive_edit_phases = 0;

        while (true) {
            if (shutting_down_.load(std::memory_order_acquire)) {
                break;
            }

            const std::size_t cur_depth = completion_depth_.load(std::memory_order_acquire);
            const bool under_hard_cap = (cur_depth < DualDirtyScheduler::kHardCompletionCap);

            // Lane 1: Edit Transactions (Highest Priority)
            bool ran_work = false;
            if (under_hard_cap &&
                (consecutive_edit_phases < kEditStarvationBound ||
                 dirty_scheduler_.edit_dirty_count() == 0)) {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                if (!edit_queue_.empty() && !edit_queue_in_progress_) {
                    edit_queue_in_progress_ = true;
                    active_workers_.fetch_add(1, std::memory_order_acq_rel);
                    EditTransaction tx = std::move(edit_queue_.front());
                    edit_queue_.pop_front();
                    lock.unlock();

                    std::optional<EditTransactionCompletedPayload> completed_payload;
                    {
                        ScopedTimer timer(stage_timings_, Stage::Edit);
                        completed_payload = execute_next_edit_phase(store_, dirty_scheduler_, tx, generate_fn_);
                    }
                    ++consecutive_edit_phases;

                    lock.lock();
                    if (completed_payload.has_value()) {
                        CompletionRecord rec{};
                        rec.kind = CompletionKind::EditTransactionCompleted;
                        rec.edit_transaction = std::move(*completed_payload);
                        push_completion(std::move(rec));
                    } else {
                        edit_queue_.push_front(std::move(tx));
                    }
                    edit_queue_in_progress_ = false;
                    active_workers_.fetch_sub(1, std::memory_order_acq_rel);
                    lock.unlock();
                    cv_.notify_all();
                    ran_work = true;
                }
            }

            if (shutting_down_.load(std::memory_order_acquire)) {
                break;
            }

            // Lane 2: Mesh Jobs (Edit-Dirty first, then closure-complete Stream-Dirty if depth < 8)
            if (!ran_work) {
                auto maybe_mesh = dirty_scheduler_.try_pop_next_mesh_job(
                    completion_depth_.load(std::memory_order_acquire));
                if (maybe_mesh.has_value()) {
                    consecutive_edit_phases = 0;
                    active_workers_.fetch_add(1, std::memory_order_acq_rel);

                    const ChunkKey target_key = maybe_mesh->key;
                    NeighborhoodVersionStamp stamp{};

                    ChunkMeshPayload mesh_payload{};
                    {
                        ScopedTimer timer(stage_timings_, Stage::Mesh);
                        stamp = gather_scratch_window_36(store_, target_key, scratch);
                        if (mesh_fn_) {
                            mesh_payload = mesh_fn_(store_, target_key, scratch, stamp);
                        } else {
                            mesh_payload.key = target_key;
                            mesh_payload.captured_terrain_revision = stamp.captured_terrain_revision;
                        }
                    }

                    if (validate_neighborhood_stamp(store_, stamp)) {
                        CompletionRecord rec{};
                        rec.kind = CompletionKind::ChunkMesh;
                        rec.chunk_mesh = std::move(mesh_payload);
                        push_completion(std::move(rec));
                    } else {
                        if (maybe_mesh->lane == DirtyQueueLane::EditDirty) {
                            dirty_scheduler_.mark_edit_dirty(target_key);
                        } else {
                            dirty_scheduler_.mark_stream_dirty(target_key);
                        }
                    }

                    active_workers_.fetch_sub(1, std::memory_order_acq_rel);
                    cv_.notify_all();
                    ran_work = true;
                }
            }

            // Lane 3: Background Procedural Generation & Disk Loading (with 26-neighbor closure gate)
            if (!ran_work && generate_fn_) {
                std::optional<GenerateRequest> gen_req;
                {
                    std::lock_guard<std::mutex> lock(generate_mutex_);
                    if (!generate_queue_.empty()) {
                        gen_req = generate_queue_.front();
                        generate_queue_.pop_front();
                        pending_generate_set_.erase(gen_req->key);
                    }
                }
                if (gen_req.has_value()) {
                    active_workers_.fetch_add(1, std::memory_order_acq_rel);
                    {
                        ScopedTimer timer(stage_timings_, Stage::Generate);
                        generate_fn_(store_, gen_req->key);
                    }
                    promote_ready_neighbors_to_stream_dirty(gen_req->key);
                    active_workers_.fetch_sub(1, std::memory_order_acq_rel);
                    cv_.notify_all();
                    ran_work = true;
                }
            }

            if (!ran_work) {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(2), [this]() {
                    if (shutting_down_.load(std::memory_order_acquire)) return true;
                    if (!edit_queue_.empty() && !edit_queue_in_progress_) return true;
                    if (dirty_scheduler_.edit_dirty_count() > 0 || dirty_scheduler_.stream_dirty_count() > 0) return true;
                    std::lock_guard<std::mutex> gen_lock(generate_mutex_);
                    return !generate_queue_.empty();
                });
            }
        }
    }

    ChunkStore& store_;
    DualDirtyScheduler& dirty_scheduler_;
    MeshGeneratorFn mesh_fn_;
    BackgroundGenerateFn generate_fn_;
    ShutdownSaveHookFn shutdown_save_hook_;
    StageTimings* stage_timings_ = nullptr;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::deque<EditTransaction> edit_queue_;
    bool edit_queue_in_progress_ = false;

    mutable std::mutex generate_mutex_;
    std::deque<GenerateRequest> generate_queue_;
    std::unordered_set<ChunkKey, ChunkKeyHash> pending_generate_set_;
    std::unordered_set<ChunkKey, ChunkKeyHash> awaiting_closure_mesh_set_;

    mutable std::mutex completion_mutex_;
    std::deque<CompletionRecord> completion_queue_;
    std::atomic<std::size_t> completion_depth_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<std::size_t> active_workers_{0};
    std::vector<std::thread> workers_;
};

} // namespace voxel