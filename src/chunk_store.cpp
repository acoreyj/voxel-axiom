#include "voxel/chunk_store.hpp"

namespace voxel {

// ============================================================================
// MemoryLedger Implementation
// ============================================================================
MemoryLedger::MemoryLedger(std::size_t budget_bytes) noexcept
    : budget_bytes_(budget_bytes), total_reserved_(0) {
    for (auto& p : pool_reserved_) {
        p.store(0, std::memory_order_relaxed);
    }
}

bool MemoryLedger::try_reserve(std::size_t bytes, MemoryPoolId pool) noexcept {
    if (bytes == 0) {
        return true;
    }
    if (pool == MemoryPoolId::RegionBlobCache) {
        const std::size_t current_blob =
            pool_reserved_[static_cast<std::size_t>(pool)].load(std::memory_order_relaxed);
        if (current_blob + bytes > kMaxRegionBlobCacheBytes) {
            return false;
        }
    }

    std::size_t cur = total_reserved_.load(std::memory_order_relaxed);
    const std::size_t cap = budget_bytes_.load(std::memory_order_acquire);
    while (true) {
        if (cur + bytes > cap) {
            return false;
        }
        if (total_reserved_.compare_exchange_weak(
                cur, cur + bytes, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            pool_reserved_[static_cast<std::size_t>(pool)].fetch_add(bytes, std::memory_order_relaxed);
            return true;
        }
    }
}

void MemoryLedger::release(std::size_t bytes, MemoryPoolId pool) noexcept {
    if (bytes == 0) {
        return;
    }
    const std::size_t prev_pool =
        pool_reserved_[static_cast<std::size_t>(pool)].fetch_sub(bytes, std::memory_order_relaxed);
    assert(prev_pool >= bytes);
    (void)prev_pool;

    const std::size_t prev_total = total_reserved_.fetch_sub(bytes, std::memory_order_acq_rel);
    assert(prev_total >= bytes);
    (void)prev_total;
}

// ============================================================================
// ChunkSlot Implementation
// ============================================================================
void ChunkSlot::reset_to_uniform(ChunkStorageForm uniform_form,
                                 uint8_t uniform_material,
                                 TsdfSlabPool& tsdf_pool,
                                 MaterialSlabPool& mat_pool) noexcept {
    assert(uniform_form == ChunkStorageForm::Empty || uniform_form == ChunkStorageForm::Solid);
    if (tsdf_data) {
        tsdf_pool.deallocate(tsdf_data);
        tsdf_data = nullptr;
    }
    if (material_data) {
        mat_pool.deallocate(material_data);
        material_data = nullptr;
    }
    form = uniform_form;
    base_material_id = uniform_material;
}

bool ChunkSlot::promote_to_dense(TsdfSlabPool& tsdf_pool) noexcept {
    if (form == ChunkStorageForm::Dense) {
        assert(tsdf_data != nullptr);
        return true;
    }
    int16_t* slab = tsdf_pool.allocate();
    if (!slab) {
        return false;
    }
    const int16_t fill_val = (form == ChunkStorageForm::Solid)
                                 ? TsdfFormat::kClampedSolid
                                 : TsdfFormat::kClampedAir;
    std::fill_n(slab, TsdfFormat::kChunkSamples, fill_val);
    tsdf_data = slab;
    form = ChunkStorageForm::Dense;
    return true;
}

bool ChunkSlot::set_tsdf(LocalCoord local, int16_t value, TsdfSlabPool& tsdf_pool, bool* out_changed) noexcept {
    return set_tsdf_linear(local.to_linear_index(), value, tsdf_pool, out_changed);
}

bool ChunkSlot::set_tsdf_linear(uint32_t idx, int16_t value, TsdfSlabPool& tsdf_pool, bool* out_changed) noexcept {
    assert(idx < TsdfFormat::kChunkSamples);
    const int16_t current = get_tsdf_linear(idx);
    if (current == value) {
        if (out_changed) {
            *out_changed = false;
        }
        return true;
    }
    if (form != ChunkStorageForm::Dense) {
        if (!promote_to_dense(tsdf_pool)) {
            if (out_changed) {
                *out_changed = false;
            }
            return false;
        }
    }
    tsdf_data[idx] = value;
    if (out_changed) {
        *out_changed = true;
    }
    return true;
}

bool ChunkSlot::set_material(LocalCoord local, uint8_t mat_id, MaterialSlabPool& mat_pool) noexcept {
    return set_material_linear(local.to_linear_index(), mat_id, mat_pool);
}

bool ChunkSlot::set_material_linear(uint32_t idx, uint8_t mat_id, MaterialSlabPool& mat_pool) noexcept {
    assert(idx < TsdfFormat::kChunkSamples);
    if (!material_data) {
        if (mat_id == base_material_id) {
            return true; // Remains uniform with 0 bytes allocated
        }
        uint8_t* slab = mat_pool.allocate();
        if (!slab) {
            return false;
        }
        std::memset(slab, base_material_id, TsdfFormat::kMaterialSlabBytes);
        material_data = slab;
    }
    material_data[idx] = mat_id;
    return true;
}

bool ChunkSlot::try_demote_if_uniform(TsdfSlabPool& tsdf_pool, MaterialSlabPool& mat_pool) noexcept {
    bool demoted_tsdf = false;
    if (form == ChunkStorageForm::Dense && tsdf_data != nullptr) {
        const int16_t first = tsdf_data[0];
        if (first == TsdfFormat::kClampedAir || first == TsdfFormat::kClampedSolid) {
            bool all_same = true;
            for (std::size_t i = 1; i < TsdfFormat::kChunkSamples; ++i) {
                if (tsdf_data[i] != first) {
                    all_same = false;
                    break;
                }
            }
            if (all_same) {
                tsdf_pool.deallocate(tsdf_data);
                tsdf_data = nullptr;
                form = (first == TsdfFormat::kClampedAir)
                           ? ChunkStorageForm::Empty
                           : ChunkStorageForm::Solid;
                demoted_tsdf = true;
            }
        }
    }

    if (material_data != nullptr) {
        const uint8_t first_mat = material_data[0];
        bool all_same_mat = true;
        for (std::size_t i = 1; i < TsdfFormat::kChunkSamples; ++i) {
            if (material_data[i] != first_mat) {
                all_same_mat = false;
                break;
            }
        }
        if (all_same_mat) {
            mat_pool.deallocate(material_data);
            material_data = nullptr;
            base_material_id = first_mat;
        }
    }
    return demoted_tsdf;
}

// ============================================================================
// ChunkStore Implementation
// ============================================================================
ChunkStore::ChunkStore(MemoryLedger& ledger, int32_t world_height_chunks)
    : ledger_(ledger),
      tsdf_pool_(ledger),
      material_pool_(ledger),
      world_height_chunks_(world_height_chunks) {}

ChunkStore::~ChunkStore() {
    clear_all();
}

ChunkSlot* ChunkStore::get_or_create_slot(ChunkKey key) {
    std::unique_lock<std::shared_mutex> map_lock(index_mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        return it->second.get();
    }
    auto slot = std::make_unique<ChunkSlot>(key);
    ChunkSlot* raw = slot.get();
    index_.emplace(key, std::move(slot));
    return raw;
}

ChunkSlot* ChunkStore::find_slot(ChunkKey key) const noexcept {
    std::shared_lock<std::shared_mutex> map_lock(index_mutex_);
    auto it = index_.find(key);
    return (it != index_.end()) ? it->second.get() : nullptr;
}

bool ChunkStore::evict_slot(ChunkKey key) {
    std::unique_lock<std::shared_mutex> map_lock(index_mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    ChunkSlot* slot = it->second.get();
    std::unique_lock<std::shared_mutex> slot_lock(slot->mutex, std::try_to_lock);
    if (!slot_lock.owns_lock()) {
        return false;
    }
    if (slot->pin_count.load(std::memory_order_acquire) != 0) {
        return false;
    }
    slot->reset_to_uniform(ChunkStorageForm::Empty, 0, tsdf_pool_, material_pool_);
    slot->state = ChunkLifecycleState::Absent;
    slot_lock.unlock();
    index_.erase(it);
    return true;
}

void ChunkStore::clear_all() noexcept {
    {
        std::unique_lock<std::shared_mutex> map_lock(index_mutex_);
        for (auto& [key, slot] : index_) {
            assert(slot->pin_count.load(std::memory_order_relaxed) == 0 &&
                   "Chunk pin_count must be 0 on teardown");
            slot->reset_to_uniform(ChunkStorageForm::Empty, 0, tsdf_pool_, material_pool_);
        }
        index_.clear();
    }
    tsdf_pool_.purge_free_list();
    material_pool_.purge_free_list();
}

std::size_t ChunkStore::slot_count() const noexcept {
    std::shared_lock<std::shared_mutex> map_lock(index_mutex_);
    return index_.size();
}

// ============================================================================
// ChunkStore::ScopedPhaseWriteLock Implementation
// ============================================================================
ChunkStore::ScopedPhaseWriteLock::ScopedPhaseWriteLock(ChunkStore& store, std::vector<ChunkKey> keys) {
    // Sort and deduplicate keys in strictly ascending ChunkKey::raw order.
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    assert(keys.size() <= kMaxEditLockChunks &&
           "Edit phase exceeded max_edit_lock_chunks = 32");

    slots_.reserve(keys.size());
    for (const ChunkKey k : keys) {
        slots_.push_back(store.get_or_create_slot(k));
    }
    // Acquire write locks in ascending ChunkKey::raw order.
    for (ChunkSlot* slot : slots_) {
        slot->mutex.lock();
    }
}

ChunkStore::ScopedPhaseWriteLock::~ScopedPhaseWriteLock() {
    // Release write locks in strictly descending ChunkKey::raw order.
    for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
        (*it)->mutex.unlock();
    }
}

} // namespace voxel
