#pragma once

#include "chunk_store.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

namespace voxel {

// ============================================================================
// 1. 36^3 (-1..34) Worker Scratch Window & Snapshot Stamp (Sections 3.1, 3.2)
// ============================================================================

/// Snapshot version record for the 27-chunk neighborhood of a meshed chunk.
/// Used to verify optimistic snapshot isolation after Surface Nets completes (Section 3.2).
struct NeighborhoodVersionStamp {
    ChunkKey center{};
    uint64_t captured_terrain_revision = 0;

    struct Entry {
        ChunkKey key{};
        uint64_t version = 0; // 0 indicates non-resident at snapshot time
        bool resident = false;
    };

    // Stored in strictly ascending ChunkKey::raw order for deterministic lock & validation order.
    std::array<Entry, 27> entries{};
};

/// Pre-allocated thread-local 36^3 (-1..34 on each axis) scratch window for Surface Nets meshing.
/// Accounted under MemoryPoolId::WorkerScratch in MemoryLedger (Sections 3.1, 3.3).
class WorkerScratchWindow36 {
public:
    static constexpr int32_t kMinLocal = -1;
    static constexpr int32_t kMaxLocal = 34;
    static constexpr int32_t kDim = 36; // -1 .. 34 inclusive
    static constexpr std::size_t kTotalSamples = 36ULL * 36ULL * 36ULL; // 46,656 samples
    static constexpr std::size_t kScratchBytes =
        kTotalSamples * (sizeof(int16_t) + sizeof(uint8_t)); // 139,968 B (~136.7 KiB)

    explicit WorkerScratchWindow36(MemoryLedger& ledger)
        : ledger_(ledger),
          tsdf_(kTotalSamples, TsdfFormat::kClampedAir),
          material_(kTotalSamples, 0) {
        reserved_ = ledger_.try_reserve(kScratchBytes, MemoryPoolId::WorkerScratch);
        assert(reserved_ && "WorkerScratchWindow36 reservation must succeed within budget");
    }

    ~WorkerScratchWindow36() {
        if (reserved_) {
            ledger_.release(kScratchBytes, MemoryPoolId::WorkerScratch);
        }
    }

    WorkerScratchWindow36(const WorkerScratchWindow36&) = delete;
    WorkerScratchWindow36& operator=(const WorkerScratchWindow36&) = delete;

    /// Returns true if (sx, sy, sz) lies within the valid [-1, 34]^3 scratch domain.
    [[nodiscard]] static constexpr bool in_bounds(int32_t sx, int32_t sy, int32_t sz) noexcept {
        return (sx >= kMinLocal && sx <= kMaxLocal) &&
               (sy >= kMinLocal && sy <= kMaxLocal) &&
               (sz >= kMinLocal && sz <= kMaxLocal);
    }

    /// Maps scratch coordinate (sx, sy, sz) in [-1, 34]^3 to flat index in [0, 46655].
    [[nodiscard]] static constexpr std::size_t linear_index(int32_t sx, int32_t sy, int32_t sz) noexcept {
        assert(in_bounds(sx, sy, sz));
        const std::size_t ux = static_cast<std::size_t>(sx - kMinLocal);
        const std::size_t uy = static_cast<std::size_t>(sy - kMinLocal);
        const std::size_t uz = static_cast<std::size_t>(sz - kMinLocal);
        return ux + kDim * (uy + kDim * uz);
    }

    [[nodiscard]] int16_t get_tsdf(int32_t sx, int32_t sy, int32_t sz) const noexcept {
        return tsdf_[linear_index(sx, sy, sz)];
    }

    void set_tsdf(int32_t sx, int32_t sy, int32_t sz, int16_t val) noexcept {
        tsdf_[linear_index(sx, sy, sz)] = val;
    }

    [[nodiscard]] uint8_t get_material(int32_t sx, int32_t sy, int32_t sz) const noexcept {
        return material_[linear_index(sx, sy, sz)];
    }

    void set_material(int32_t sx, int32_t sy, int32_t sz, uint8_t mat) noexcept {
        material_[linear_index(sx, sy, sz)] = mat;
    }

    [[nodiscard]] const std::vector<int16_t>& raw_tsdf() const noexcept { return tsdf_; }
    [[nodiscard]] const std::vector<uint8_t>& raw_material() const noexcept { return material_; }

private:
    MemoryLedger& ledger_;
    bool reserved_ = false;
    std::vector<int16_t> tsdf_;
    std::vector<uint8_t> material_;
};

/// Gathers the 36^3 (-1..34) worker scratch window for `center` from `store` under
/// ascending ChunkKey::raw shared read locks, then releases all locks and returns
/// the NeighborhoodVersionStamp (HLD v4.0 Sections 3.1, 3.2).
[[nodiscard]] inline NeighborhoodVersionStamp gather_scratch_window_36(
    const ChunkStore& store,
    ChunkKey center,
    WorkerScratchWindow36& scratch) {

    NeighborhoodVersionStamp stamp{};
    stamp.center = center;

    struct NeighborRef {
        ChunkKey key{};
        int32_t dx = 0;
        int32_t dy = 0;
        int32_t dz = 0;
        ChunkSlot* slot = nullptr;
    };

    std::array<NeighborRef, 27> neighbors{};
    std::size_t idx = 0;
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const ChunkKey nk = center.offset(dx, dy, dz);
                neighbors[idx++] = NeighborRef{nk, dx, dy, dz, store.find_slot(nk)};
            }
        }
    }

    // Sort all 27 neighborhood entries in strictly ascending ChunkKey::raw order.
    std::sort(neighbors.begin(), neighbors.end(),
              [](const NeighborRef& a, const NeighborRef& b) { return a.key < b.key; });

    // Acquire shared read locks in ascending ChunkKey::raw order on existing slots.
    for (const NeighborRef& n : neighbors) {
        if (n.slot) {
            n.slot->mutex.lock_shared();
        }
    }

    stamp.captured_terrain_revision = store.terrain_revision();

    // Copy each neighbor's canonical slice into the 36^3 (-1..34) scratch window.
    // Axis slice mapping per Section 3.1 Table:
    //   dx = -1 -> scratch [-1 .. -1], canonical [31 .. 31]
    //   dx =  0 -> scratch [ 0 .. 31], canonical [ 0 .. 31]
    //   dx = +1 -> scratch [32 .. 34], canonical [ 0 ..  2]
    auto axis_scratch_range = [](int32_t d, int32_t& s_begin, int32_t& s_end) noexcept {
        if (d < 0) {
            s_begin = -1;
            s_end = -1;
        } else if (d == 0) {
            s_begin = 0;
            s_end = 31;
        } else {
            s_begin = 32;
            s_end = 34;
        }
    };

    const int32_t world_height = store.world_height_chunks();

    for (std::size_t i = 0; i < 27; ++i) {
        const NeighborRef& n = neighbors[i];
        const bool is_resident =
            (n.slot != nullptr && n.slot->state == ChunkLifecycleState::Resident);

        stamp.entries[i] = NeighborhoodVersionStamp::Entry{
            n.key,
            is_resident ? n.slot->version.load(std::memory_order_acquire) : 0ULL,
            is_resident
        };

        int32_t sx0 = 0, sx1 = 0;
        int32_t sy0 = 0, sy1 = 0;
        int32_t sz0 = 0, sz1 = 0;
        axis_scratch_range(n.dx, sx0, sx1);
        axis_scratch_range(n.dy, sy0, sy1);
        axis_scratch_range(n.dz, sz0, sz1);

        if (is_resident) {
            for (int32_t sz = sz0; sz <= sz1; ++sz) {
                const int32_t lz = sz - 32 * n.dz;
                for (int32_t sy = sy0; sy <= sy1; ++sy) {
                    const int32_t ly = sy - 32 * n.dy;
                    for (int32_t sx = sx0; sx <= sx1; ++sx) {
                        const int32_t lx = sx - 32 * n.dx;
                        const LocalCoord lc{lx, ly, lz};
                        scratch.set_tsdf(sx, sy, sz, n.slot->get_tsdf(lc));
                        scratch.set_material(sx, sy, sz, n.slot->get_material(lc));
                    }
                }
            }
        } else {
            // Section 3.2 Read Rule (2):
            // Below bedrock (Cy < 0) -> kClampedSolid; above sky (Cy >= world_height_chunks)
            // or non-resident -> kClampedAir.
            const int16_t fallback_tsdf = (n.key.y() < 0)
                                              ? TsdfFormat::kClampedSolid
                                              : TsdfFormat::kClampedAir;
            for (int32_t sz = sz0; sz <= sz1; ++sz) {
                for (int32_t sy = sy0; sy <= sy1; ++sy) {
                    for (int32_t sx = sx0; sx <= sx1; ++sx) {
                        scratch.set_tsdf(sx, sy, sz, fallback_tsdf);
                        scratch.set_material(sx, sy, sz, 0);
                    }
                }
            }
            (void)world_height;
        }
    }

    // Release shared read locks in descending ChunkKey::raw order before Surface Nets begins.
    for (auto it = neighbors.rbegin(); it != neighbors.rend(); ++it) {
        if (it->slot) {
            it->slot->mutex.unlock_shared();
        }
    }

    return stamp;
}

/// Validates that all 27 chunks in `stamp` still have identical residency and version numbers.
/// Returns false if any chunk in the 27-neighborhood was modified or unloaded while meshing.
[[nodiscard]] inline bool validate_neighborhood_stamp(
    const ChunkStore& store,
    const NeighborhoodVersionStamp& stamp) noexcept {
    for (const auto& entry : stamp.entries) {
        const ChunkSlot* slot = store.find_slot(entry.key);
        if (!slot) {
            if (entry.resident) {
                return false;
            }
            continue;
        }
        std::shared_lock<std::shared_mutex> lock(slot->mutex);
        const bool now_resident = (slot->state == ChunkLifecycleState::Resident);
        const uint64_t now_version = now_resident ? slot->version.load(std::memory_order_acquire) : 0ULL;
        if (now_resident != entry.resident || now_version != entry.version) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// 2. Selective Boundary Neighbor Dirtying Tracker (Sections 3.1, 3.5, 7.1)
// ============================================================================

/// Accumulates selectively dirtied neighbors for a single chunk C during an Edit phase.
/// Uses a 27-bit mask over (dx, dy, dz) in {-1, 0, +1}^3 so recording each sample mutation is O(1).
///
/// Selective dirtying rules (Section 3.1):
///   - Mutating any sample in C marks C itself (dx=0, dy=0, dz=0) dirty.
///   - Mutating local sample 0, 1, or 2 on an axis marks the -1 neighbor on that axis dirty.
///   - Mutating local sample 31 on an axis marks the +1 neighbor on that axis dirty.
///   - Mutating an interior sample in [3, 30]^3 only marks C itself dirty (0 neighbors dirtied).
struct SelectiveDirtyMask {
    uint32_t mask = 0;

    [[nodiscard]] static constexpr uint32_t bit_index(int32_t dx, int32_t dy, int32_t dz) noexcept {
        assert(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && dz >= -1 && dz <= 1);
        return static_cast<uint32_t>((dx + 1) + 3 * ((dy + 1) + 3 * (dz + 1)));
    }

    /// Records that canonical sample `local` in chunk C was mutated.
    constexpr void record_mutated_sample(LocalCoord local) noexcept {
        assert(local.is_valid());
        const int32_t dx_extra = (local.x <= 2) ? -1 : ((local.x == 31) ? +1 : 0);
        const int32_t dy_extra = (local.y <= 2) ? -1 : ((local.y == 31) ? +1 : 0);
        const int32_t dz_extra = (local.z <= 2) ? -1 : ((local.z == 31) ? +1 : 0);

        const int32_t dx_vals[2] = {0, dx_extra};
        const int32_t dy_vals[2] = {0, dy_extra};
        const int32_t dz_vals[2] = {0, dz_extra};

        const int nx = (dx_extra != 0) ? 2 : 1;
        const int ny = (dy_extra != 0) ? 2 : 1;
        const int nz = (dz_extra != 0) ? 2 : 1;

        for (int iz = 0; iz < nz; ++iz) {
            for (int iy = 0; iy < ny; ++iy) {
                for (int ix = 0; ix < nx; ++ix) {
                    mask |= (1U << bit_index(dx_vals[ix], dy_vals[iy], dz_vals[iz]));
                }
            }
        }
    }

    /// Appends all dirtied ChunkKeys (chunk C and any selectively dirtied neighbors) to `out_keys`.
    void emit_dirty_keys(ChunkKey center, std::vector<ChunkKey>& out_keys) const {
        if (mask == 0) {
            return;
        }
        for (int32_t dz = -1; dz <= 1; ++dz) {
            for (int32_t dy = -1; dy <= 1; ++dy) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    if ((mask & (1U << bit_index(dx, dy, dz))) != 0U) {
                        out_keys.push_back(center.offset(dx, dy, dz));
                    }
                }
            }
        }
    }
};

// ============================================================================
// 3. Dual Dirty-Set Priority Scheduler (Sections 3.4, 7.1)
// ============================================================================

enum class DirtyQueueLane : uint8_t {
    EditDirty = 0,  // High priority: populated on EditTransaction completion
    StreamDirty = 1 // Low priority: populated on Load/Generate completion
};

struct PoppedDirtyChunk {
    ChunkKey key{};
    DirtyQueueLane lane = DirtyQueueLane::EditDirty;
};

/// Deduplicated dual dirty-set scheduler (`dirty_edit_chunks` and `dirty_stream_chunks`).
/// Guarantees `dirty_edit_chunks` always drains before `dirty_stream_chunks`, orders chunks
/// nearest-first relative to the streaming focus chunk, and pauses `dirty_stream_chunks`
/// admission when completion queue depth reaches `kSoftCompletionCap = 8` (Section 3.4).
class DualDirtyScheduler {
public:
    static constexpr std::size_t kSoftCompletionCap = 8;
    static constexpr std::size_t kHardCompletionCap = 32;

    void set_focus_chunk(ChunkKey focus) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        focus_ = focus;
    }

    /// Marks a single chunk edit-dirty (promoting it out of `dirty_stream_chunks` if present).
    void mark_edit_dirty(ChunkKey key) {
        std::lock_guard<std::mutex> lock(mutex_);
        insert_edit_dirty_unlocked(key);
    }

    /// Atomically publishes a batch of edit-dirty chunks upon final phase completion (Section 3.2).
    void publish_edit_transaction_dirty_batch(const std::vector<ChunkKey>& keys) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const ChunkKey k : keys) {
            insert_edit_dirty_unlocked(k);
        }
    }

    /// Marks a chunk stream-dirty (ignored if already queued in high-priority `dirty_edit_chunks`).
    void mark_stream_dirty(ChunkKey key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (edit_set_.find(key) != edit_set_.end()) {
            return;
        }
        stream_set_.insert(key);
    }

    /// Pops the next chunk to mesh according to Section 3.4 priority and backpressure rules:
    ///   - If `completion_queue_depth >= 32` (hard cap), returns std::nullopt.
    ///   - Always pops nearest chunk from `dirty_edit_chunks` first.
    ///   - Only when `dirty_edit_chunks` is empty AND `completion_queue_depth < 8` (soft cap),
    ///     pops nearest chunk from `dirty_stream_chunks`.
    [[nodiscard]] std::optional<PoppedDirtyChunk> try_pop_next_mesh_job(
        std::size_t completion_queue_depth) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completion_queue_depth >= kHardCompletionCap) {
            return std::nullopt;
        }
        if (!edit_set_.empty()) {
            const ChunkKey best = pop_nearest_unlocked(edit_set_);
            return PoppedDirtyChunk{best, DirtyQueueLane::EditDirty};
        }
        if (completion_queue_depth < kSoftCompletionCap && !stream_set_.empty()) {
            const ChunkKey best = pop_nearest_unlocked(stream_set_);
            return PoppedDirtyChunk{best, DirtyQueueLane::StreamDirty};
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t edit_dirty_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return edit_set_.size();
    }

    [[nodiscard]] std::size_t stream_dirty_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_set_.size();
    }

    [[nodiscard]] bool contains_edit_dirty(ChunkKey key) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return edit_set_.find(key) != edit_set_.end();
    }

    [[nodiscard]] bool contains_stream_dirty(ChunkKey key) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_set_.find(key) != stream_set_.end();
    }

private:
    void insert_edit_dirty_unlocked(ChunkKey key) {
        stream_set_.erase(key); // Promote if previously stream-dirty
        edit_set_.insert(key);
    }

    [[nodiscard]] int64_t dist_sq_to_focus_unlocked(ChunkKey k) const noexcept {
        const int64_t dx = static_cast<int64_t>(k.x()) - static_cast<int64_t>(focus_.x());
        const int64_t dy = static_cast<int64_t>(k.y()) - static_cast<int64_t>(focus_.y());
        const int64_t dz = static_cast<int64_t>(k.z()) - static_cast<int64_t>(focus_.z());
        return dx * dx + dy * dy + dz * dz;
    }

    [[nodiscard]] ChunkKey pop_nearest_unlocked(std::unordered_set<ChunkKey, ChunkKeyHash>& set) {
        assert(!set.empty());
        auto best_it = set.begin();
        int64_t best_dist = dist_sq_to_focus_unlocked(*best_it);

        for (auto it = std::next(set.begin()); it != set.end(); ++it) {
            const int64_t d = dist_sq_to_focus_unlocked(*it);
            if (d < best_dist || (d == best_dist && *it < *best_it)) {
                best_dist = d;
                best_it = it;
            }
        }
        const ChunkKey chosen = *best_it;
        set.erase(best_it);
        return chosen;
    }

    mutable std::mutex mutex_;
    ChunkKey focus_ = ChunkKey::from_coords(0, 0, 0);
    std::unordered_set<ChunkKey, ChunkKeyHash> edit_set_;
    std::unordered_set<ChunkKey, ChunkKeyHash> stream_set_;
};

} // namespace voxel
