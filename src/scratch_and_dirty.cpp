#include "voxel/scratch_and_dirty.hpp"

namespace voxel {

// ============================================================================
// WorkerScratchWindow36 Implementation
// ============================================================================
WorkerScratchWindow36::WorkerScratchWindow36(MemoryLedger& ledger)
    : ledger_(ledger),
      tsdf_(kTotalSamples, TsdfFormat::kClampedAir),
      material_(kTotalSamples, 0) {
    reserved_ = ledger_.try_reserve(kScratchBytes, MemoryPoolId::WorkerScratch);
    assert(reserved_ && "WorkerScratchWindow36 reservation must succeed within budget");
}

WorkerScratchWindow36::~WorkerScratchWindow36() {
    if (reserved_) {
        ledger_.release(kScratchBytes, MemoryPoolId::WorkerScratch);
    }
}

// ============================================================================
// Neighborhood Scratch Window Gather & Validation
// ============================================================================
NeighborhoodVersionStamp gather_scratch_window_36(
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

bool validate_neighborhood_stamp(
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
// DualDirtyScheduler Implementation
// ============================================================================
void DualDirtyScheduler::set_focus_chunk(ChunkKey focus) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    focus_ = focus;
}

void DualDirtyScheduler::mark_edit_dirty(ChunkKey key) {
    std::lock_guard<std::mutex> lock(mutex_);
    insert_edit_dirty_unlocked(key);
}

void DualDirtyScheduler::publish_edit_transaction_dirty_batch(const std::vector<ChunkKey>& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ChunkKey k : keys) {
        insert_edit_dirty_unlocked(k);
    }
}

void DualDirtyScheduler::mark_stream_dirty(ChunkKey key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (edit_set_.find(key) != edit_set_.end()) {
        return;
    }
    stream_set_.insert(key);
}

std::optional<PoppedDirtyChunk> DualDirtyScheduler::try_pop_next_mesh_job(
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

std::size_t DualDirtyScheduler::edit_dirty_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return edit_set_.size();
}

std::size_t DualDirtyScheduler::stream_dirty_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stream_set_.size();
}

bool DualDirtyScheduler::contains_edit_dirty(ChunkKey key) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return edit_set_.find(key) != edit_set_.end();
}

bool DualDirtyScheduler::contains_stream_dirty(ChunkKey key) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stream_set_.find(key) != stream_set_.end();
}

void DualDirtyScheduler::insert_edit_dirty_unlocked(ChunkKey key) {
    stream_set_.erase(key);
    edit_set_.insert(key);
}

int64_t DualDirtyScheduler::dist_sq_to_focus_unlocked(ChunkKey k) const noexcept {
    const int64_t dx = static_cast<int64_t>(k.x()) - static_cast<int64_t>(focus_.x());
    const int64_t dy = static_cast<int64_t>(k.y()) - static_cast<int64_t>(focus_.y());
    const int64_t dz = static_cast<int64_t>(k.z()) - static_cast<int64_t>(focus_.z());
    return dx * dx + dy * dy + dz * dz;
}

ChunkKey DualDirtyScheduler::pop_nearest_unlocked(std::unordered_set<ChunkKey, ChunkKeyHash>& set) {
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

} // namespace voxel
