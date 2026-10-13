#pragma once

#include "scratch_and_dirty.hpp"

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
    int32_t gx = 0; // ChunkKey::floor_div(Cx, 4)
    int32_t gy = 0; // ChunkKey::floor_div(Cy, 2)
    int32_t gz = 0; // ChunkKey::floor_div(Cz, 4)

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
};

/// Builds a phased explosion EditTransaction with expanded R + delta influence bounds,
/// partitioned into phases of at most 32 canonical chunks (Sections 3.2, 3.5.1).
struct EditTransaction {
    static constexpr std::size_t kMaxCoalescedSpheres = 16;
    static constexpr std::size_t kMaxPhaseChunks = ChunkStore::kMaxEditPhaseCoreChunks; // 32

    std::vector<SphereCsgTerm> spheres;
    float truncation_delta = TsdfFormat::kDefaultTruncationDelta;

    // Expanded sample AABB: [floor(c - (R + delta)), ceil(c + (R + delta))]
    IVec3 sample_min{};
    IVec3 sample_max{};

    // Partitioned canonical chunks (at most 32 chunks per phase)
    std::vector<std::vector<ChunkKey>> phases;
    std::size_t next_phase_index = 0;

    // Accumulated across phases, published ONLY when the final phase completes (no mid-explosion tearing)
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

        // Compute union of expanded (R_m + delta) world sample AABBs.
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

        // Gather all canonical chunks whose 32^3 sample box is within expanded sphere distance.
        std::vector<ChunkKey> candidate_chunks;
        for (int32_t cz = cz_min; cz <= cz_max; ++cz) {
            for (int32_t cy = cy_min; cy <= cy_max; ++cy) {
                for (int32_t cx = cx_min; cx <= cx_max; ++cx) {
                    const float box_min_x = static_cast<float>(cx * 32);
                    const float box_min_y = static_cast<float>(cy * 32);
                    const float box_min_z = static_cast<float>(cz * 32);
                    const float box_max_x = box_min_x + 31.0f;
                    const float box_max_y = box_min_y + 31.0f;
                    const float box_max_z = box_min_z + 31.0f;

                    bool intersects = false;
                    for (const SphereCsgTerm& s : tx.spheres) {
                        const float r_eff = s.radius + delta;
                        const float clamped_x = std::clamp(s.center.x, box_min_x, box_max_x);
                        const float clamped_y = std::clamp(s.center.y, box_min_y, box_max_y);
                        const float clamped_z = std::clamp(s.center.z, box_min_z, box_max_z);
                        const float dx = s.center.x - clamped_x;
                        const float dy = s.center.y - clamped_y;
                        const float dz = s.center.z - clamped_z;
                        if (dx * dx + dy * dy + dz * dz <= r_eff * r_eff) {
                            intersects = true;
                            break;
                        }
                    }
                    if (intersects) {
                        candidate_chunks.push_back(ChunkKey::from_coords(cx, cy, cz));
                    }
                }
            }
        }

        std::sort(candidate_chunks.begin(), candidate_chunks.end());

        // Partition into spatial phases of at most 32 canonical chunks per phase.
        for (std::size_t i = 0; i < candidate_chunks.size(); i += kMaxPhaseChunks) {
            const std::size_t end = std::min(candidate_chunks.size(), i + kMaxPhaseChunks);
            tx.phases.emplace_back(candidate_chunks.begin() + static_cast<std::ptrdiff_t>(i),
                                   candidate_chunks.begin() + static_cast<std::ptrdiff_t>(end));
        }
        if (tx.phases.empty()) {
            // Ensure at least 1 phase so completion logic always runs deterministically.
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
    std::vector<int32_t> indices;
    std::vector<Vec3f> collision_faces; // Pre-unpacked triangle soup (3 vertices per triangle)
    std::size_t reserved_ledger_bytes = 0;
};

struct EditTransactionCompletedPayload {
    Vec3f position{};
    float radius = 0.0f;
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
// 4. Phased CSG Executor & 3x3x3 Anti-Crumb Kernel (Section 3.5.1)
// ============================================================================

/// Executes a single phase (at most 32 canonical chunks) of `tx` against `store`.
/// Does NOT publish dirty chunks during intermediate phases.
/// On the final phase, runs the 3x3x3 Anti-Crumb Kernel across modified chunks,
/// increments `terrain_revision` once, publishes the full deduplicated dirty batch
/// to `dirty_scheduler`, and returns the `EditTransactionCompletedPayload`.
inline std::optional<EditTransactionCompletedPayload> execute_next_edit_phase(
    ChunkStore& store,
    DualDirtyScheduler& dirty_scheduler,
    EditTransaction& tx) {

    assert(!tx.is_finished());
    const std::size_t phase_idx = tx.next_phase_index++;
    const std::vector<ChunkKey>& phase_keys = tx.phases[phase_idx];

    if (!phase_keys.empty()) {
        // Step 1: Ensure all canonical chunks in the phase are Resident (with no locks held).
        for (const ChunkKey ck : phase_keys) {
            ChunkSlot* slot = store.get_or_create_slot(ck);
            std::unique_lock<std::shared_mutex> init_lock(slot->mutex);
            if (slot->state == ChunkLifecycleState::Absent ||
                slot->state == ChunkLifecycleState::Requested) {
                // Default uninitialized ground chunks below surface to Solid (or Empty above).
                slot->reset_to_uniform(ChunkStorageForm::Solid, 1, store.tsdf_pool(), store.material_pool());
                slot->version.store(1, std::memory_order_release);
                slot->state = ChunkLifecycleState::Resident;
            }
        }

        // Step 2: Acquire exclusive write locks on the phase's canonical chunks (<= 32 chunks)
        // in strictly ascending ChunkKey::raw order.
        ChunkStore::ScopedPhaseWriteLock phase_lock(store, phase_keys);
        tx.max_locks_acquired_in_any_phase =
            std::max(tx.max_locks_acquired_in_any_phase, phase_lock.size());

        const float delta = tx.truncation_delta;

        for (ChunkSlot* slot : phase_lock.slots()) {
            // Subtracting a sphere from pure air (kClampedAir) is a no-op (Section 3.5.1).
            if (slot->form == ChunkStorageForm::Empty) {
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

            for (int32_t lz = lz0; lz <= lz1; ++lz) {
                const float pz = static_cast<float>(chunk_min.z + lz);
                for (int32_t ly = ly0; ly <= ly1; ++ly) {
                    const float py = static_cast<float>(chunk_min.y + ly);
                    for (int32_t lx = lx0; lx <= lx1; ++lx) {
                        const float px = static_cast<float>(chunk_min.x + lx);

                        // Composite CSG union: d_blast(p) = max_m (R_m - ||p - c_m||)
                        float d_blast = -1e30f;
                        for (const SphereCsgTerm& s : tx.spheres) {
                            const float dx = px - s.center.x;
                            const float dy = py - s.center.y;
                            const float dz = pz - s.center.z;
                            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                            d_blast = std::max(d_blast, s.radius - dist);
                        }

                        // Evaluate across the full R + delta shell (d_blast > -delta).
                        if (d_blast > -delta) {
                            const int16_t q_blast = TsdfFormat::encode(d_blast, delta);
                            const LocalCoord lc{lx, ly, lz};
                            const int16_t q_cur = slot->get_tsdf(lc);
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

            if (chunk_modified) {
                // Demote to EMPTY if all 32^3 canonical samples are now kClampedAir (+32767).
                slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
                slot->dirty_persist = true;
                slot->version.fetch_add(1, std::memory_order_release);

                tx.modified_chunk_masks[slot->key].mask |= local_dirty_mask.mask;
                tx.affected_nav_groups.insert(NavigationGroupKey::from_chunk_key(slot->key));
            }
        }
    }

    // If more phases remain, return std::nullopt without publishing dirty chunks or completion!
    if (!tx.is_finished()) {
        return std::nullopt;
    }

    // ========================================================================
    // Final Phase Completion:
    //   1. Run 3x3x3 Anti-Crumb Kernel across modified chunks
    //   2. Increment terrain_revision once
    //   3. Publish deduplicated dirty chunks to dirty_edit_chunks
    //   4. Return single EditTransactionCompletedPayload
    // ========================================================================
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
                    // Check near-surface solid samples (-delta < S(p) < 0)
                    if (q > TsdfFormat::kClampedSolid && q < 0) {
                        auto sample_at = [&](int32_t nx, int32_t ny, int32_t nz) -> int16_t {
                            if (nx >= 0 && nx < 32 && ny >= 0 && ny < 32 && nz >= 0 && nz < 32) {
                                return slot->get_tsdf(LocalCoord{nx, ny, nz});
                            }
                            return -1; // Conservative at chunk boundary if neighbor isn't locked here
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
        dirty_scheduler.publish_edit_transaction_dirty_batch(deduplicated_dirty);
    }

    EditTransactionCompletedPayload payload{};
    if (!tx.spheres.empty()) {
        payload.position = tx.spheres.front().center;
        payload.radius = tx.spheres.front().radius;
    }
    payload.terrain_revision = new_revision;
    payload.total_affected_chunks = tx.modified_chunk_masks.size();
    payload.dirtied_chunks = std::move(deduplicated_dirty);
    payload.affected_nav_groups.assign(tx.affected_nav_groups.begin(), tx.affected_nav_groups.end());
    std::sort(payload.affected_nav_groups.begin(), payload.affected_nav_groups.end());
    return payload;
}

// ============================================================================
// 5. Core Worker Pool & Ordered Teardown Protocol (Sections 3.4, 5.2, 5.3)
// ============================================================================
class CoreWorkerPool {
public:
    static constexpr std::size_t kEditStarvationBound = 8;
    static constexpr std::size_t kMaxPendingEditTransactions = 64;

    using MeshGeneratorFn = std::function<ChunkMeshPayload(
        const ChunkStore&, ChunkKey, WorkerScratchWindow36&, const NeighborhoodVersionStamp&)>;
    using ShutdownSaveHookFn = std::function<void(ChunkStore&)>;

    CoreWorkerPool(ChunkStore& store,
                   DualDirtyScheduler& dirty_scheduler,
                   MeshGeneratorFn mesh_fn = {})
        : store_(store),
          dirty_scheduler_(dirty_scheduler),
          mesh_fn_(std::move(mesh_fn)) {}

    ~CoreWorkerPool() {
        if (running_.load(std::memory_order_acquire)) {
            shutdown(/*save=*/false);
        }
    }

    CoreWorkerPool(const CoreWorkerPool&) = delete;
    CoreWorkerPool& operator=(const CoreWorkerPool&) = delete;

    void set_shutdown_save_hook(ShutdownSaveHookFn hook) {
        shutdown_save_hook_ = std::move(hook);
    }

    void start(std::size_t num_workers = 0) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        shutting_down_.store(false, std::memory_order_release);

        if (num_workers == 0) {
            const unsigned int hc = std::thread::hardware_concurrency();
            num_workers = (hc > 2U) ? static_cast<std::size_t>(hc - 2U) : 1ULL;
        }

        workers_.reserve(num_workers);
        for (std::size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    /// Enqueues an explosion asynchronously (< 0.1 ms). Coalesces with the tail queued
    /// transaction if it has not started executing yet and has < 16 spheres (Section 3.4).
    [[nodiscard]] bool enqueue_explosion(Vec3f position, float radius) {
        if (radius <= 0.0f ||
            !running_.load(std::memory_order_acquire) ||
            shutting_down_.load(std::memory_order_acquire)) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (shutting_down_.load(std::memory_order_relaxed)) {
                return false;
            }
            if (!edit_queue_.empty() &&
                edit_queue_.back().next_phase_index == 0 &&
                edit_queue_.back().spheres.size() < EditTransaction::kMaxCoalescedSpheres) {
                std::vector<SphereCsgTerm> combined = edit_queue_.back().spheres;
                combined.push_back(SphereCsgTerm{position, radius});
                edit_queue_.back() = EditTransaction::create_from_spheres(
                     std::move(combined),
                    TsdfFormat::kDefaultTruncationDelta,
                    store_.world_height_chunks());
            } else {
                if (edit_queue_.size() >= kMaxPendingEditTransactions) {
                    return false;
                }
                edit_queue_.push_back(EditTransaction::create_from_spheres(
                    {SphereCsgTerm{position, radius}},
                    TsdfFormat::kDefaultTruncationDelta,
                    store_.world_height_chunks()));
            }
        }
        cv_.notify_all();
        return true;
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
        return edit_queue_.empty() &&
               active_workers_.load(std::memory_order_acquire) == 0 &&
               dirty_scheduler_.edit_dirty_count() == 0 &&
               dirty_scheduler_.stream_dirty_count() == 0;
    }

    /// Deterministic Ordered Teardown Protocol (HLD v4.0 Section 5.3 Steps 1-6).
    void shutdown(bool save) {
        // Step 1: Set shutting_down = true. Reject all new job submissions. (Caller / Main)
        shutting_down_.store(true, std::memory_order_release);

        // Step 2: Wake all worker threads. Workers finish active job or current Edit phase (<= 32 chunks),
        // release all locks, and exit their run loop.
        cv_.notify_all();

        // Step 3: Join all worker threads. (Caller / Main)
        for (std::thread& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        workers_.clear();
        running_.store(false, std::memory_order_release);

        // Step 4: If save == true, run remaining PersistCompress & RegionFlush inline on caller thread
        // (exclusive, lock-free access after join).
        if (save && shutdown_save_hook_) {
            shutdown_save_hook_(store_);
        }

        // Step 5: Drain and discard all remaining entries in the MPSC completion queue,
        // releasing their memory reservations.
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

        // Step 6: Release ChunkStore and slab pools. Assert all reference counts and memory ledger bytes are zero.
        store_.clear_all();
        assert(store_.ledger().pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 0ULL);
        assert(store_.ledger().pool_reserved_bytes(MemoryPoolId::WorkerScratch) == 0ULL);
        assert(store_.ledger().pool_reserved_bytes(MemoryPoolId::CompletionQueue) == 0ULL);
    }

private:
    void push_completion(CompletionRecord rec) {
        if (rec.kind == CompletionKind::ChunkMesh) {
            const std::size_t bytes =
                rec.chunk_mesh.vertices.size() * sizeof(Vec3f) +
                rec.chunk_mesh.normals.size() * sizeof(Vec3f) +
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

    void worker_loop() {
        WorkerScratchWindow36 scratch(store_.ledger());
        std::size_t consecutive_edit_phases = 0;

        while (true) {
            if (shutting_down_.load(std::memory_order_acquire)) {
                break;
            }

            const std::size_t cur_depth = completion_depth_.load(std::memory_order_acquire);
            const bool under_hard_cap = (cur_depth < DualDirtyScheduler::kHardCompletionCap);

            // Check if we should run an Edit phase (respecting kEditStarvationBound = 8).
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

                    auto completed_payload = execute_next_edit_phase(store_, dirty_scheduler_, tx);
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

            // Try popping a Mesh job (Edit-Dirty first, then Stream-Dirty if depth < 8).
            if (!ran_work) {
                auto maybe_mesh = dirty_scheduler_.try_pop_next_mesh_job(
                    completion_depth_.load(std::memory_order_acquire));
                if (maybe_mesh.has_value()) {
                    consecutive_edit_phases = 0;
                    active_workers_.fetch_add(1, std::memory_order_acq_rel);

                    const ChunkKey target_key = maybe_mesh->key;
                    const NeighborhoodVersionStamp stamp =
                        gather_scratch_window_36(store_, target_key, scratch);

                    ChunkMeshPayload mesh_payload{};
                    if (mesh_fn_) {
                        mesh_payload = mesh_fn_(store_, target_key, scratch, stamp);
                    } else {
                        mesh_payload.key = target_key;
                        mesh_payload.captured_terrain_revision = stamp.captured_terrain_revision;
                    }

                    // Snapshot Isolation check (Section 3.2 Read Rule 4):
                    // Publish only if neighborhood versions still match; otherwise re-dirty.
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

            if (!ran_work) {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(2), [this]() {
                    return shutting_down_.load(std::memory_order_acquire) ||
                           (!edit_queue_.empty() && !edit_queue_in_progress_) ||
                           dirty_scheduler_.edit_dirty_count() > 0 ||
                           dirty_scheduler_.stream_dirty_count() > 0;
                });
            }
        }
    }

    ChunkStore& store_;
    DualDirtyScheduler& dirty_scheduler_;
    MeshGeneratorFn mesh_fn_;
    ShutdownSaveHookFn shutdown_save_hook_;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::deque<EditTransaction> edit_queue_;
    bool edit_queue_in_progress_ = false;

    mutable std::mutex completion_mutex_;
    std::deque<CompletionRecord> completion_queue_;
    std::atomic<std::size_t> completion_depth_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<std::size_t> active_workers_{0};
    std::vector<std::thread> workers_;
};

} // namespace voxel
