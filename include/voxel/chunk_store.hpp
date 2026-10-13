#pragma once

#include "voxel/chunk_key.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace voxel {

// ============================================================================
// Cross-Platform Aligned Memory Helpers
// ============================================================================
inline void* aligned_allocate(std::size_t alignment, std::size_t size) noexcept {
#if defined(_MSC_VER)
    return _aligned_malloc(size, alignment);
#elif defined(__posix__) || defined(_POSIX_C_SOURCE) || defined(__APPLE__)
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#else
    return std::aligned_alloc(alignment, size);
#endif
}

inline void aligned_deallocate(void* ptr) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

// ============================================================================
// TSDF Sample Format & Constants (HLD v4.0 Section 3.1)
// ============================================================================
struct TsdfFormat {
    static constexpr float kDefaultTruncationDelta = 2.0f;
    static constexpr int16_t kClampedSolid = -32767; // Negative = solid (INT16_MIN avoided for symmetry)
    static constexpr int16_t kClampedAir   = +32767; // Positive = air
    static constexpr std::size_t kChunkSamples = 32ULL * 32ULL * 32ULL; // 32,768 canonical samples
    static constexpr std::size_t kTsdfSlabBytes = kChunkSamples * sizeof(int16_t);     // 65,536 B (64.0 KiB)
    static constexpr std::size_t kMaterialSlabBytes = kChunkSamples * sizeof(uint8_t); // 32,768 B (32.0 KiB)

    /// Encodes a continuous signed distance value into normalized int16_t in [-32767, +32767].
    [[nodiscard]] static int16_t encode(float sdf, float delta = kDefaultTruncationDelta) noexcept {
        assert(delta > 0.0f);
        const float scaled = std::round((sdf / delta) * 32767.0f);
        if (scaled <= -32767.0f) {
            return kClampedSolid;
        }
        if (scaled >= 32767.0f) {
            return kClampedAir;
        }
        return static_cast<int16_t>(scaled);
    }

    /// Decodes a normalized int16_t TSDF sample back to continuous voxel distance in [-delta, +delta].
    [[nodiscard]] static constexpr float decode(int16_t q, float delta = kDefaultTruncationDelta) noexcept {
        const int16_t clamped = (q < kClampedSolid) ? kClampedSolid : q;
        return static_cast<float>(clamped) * (delta / 32767.0f);
    }
};

// ============================================================================
// Memory Budget Ledger & Page-Aligned Slab Pools (HLD v4.0 Sections 3.1, 3.3)
// ============================================================================
enum class MemoryPoolId : uint8_t {
    ChunkSlab = 0,
    WorkerScratch = 1,
    RegionBlobCache = 2,
    CompletionQueue = 3,
    Count = 4
};

class MemoryLedger {
public:
    static constexpr std::size_t kMiB = 1024ULL * 1024ULL;
    static constexpr std::size_t kDefaultBudgetBytes = 512ULL * kMiB;
    static constexpr std::size_t kMaxRegionBlobCacheBytes = 32ULL * kMiB;

    explicit MemoryLedger(std::size_t budget_bytes = kDefaultBudgetBytes) noexcept;

    void set_budget_bytes(std::size_t budget_bytes) noexcept {
        budget_bytes_.store(budget_bytes, std::memory_order_release);
    }

    [[nodiscard]] std::size_t budget_bytes() const noexcept {
        return budget_bytes_.load(std::memory_order_acquire);
    }

    /// Atomically attempts to reserve `bytes` in `pool` without exceeding the hard budget ceiling.
    [[nodiscard]] bool try_reserve(std::size_t bytes, MemoryPoolId pool) noexcept;

    /// Releases previously reserved bytes from `pool`.
    void release(std::size_t bytes, MemoryPoolId pool) noexcept;

    [[nodiscard]] std::size_t total_reserved_bytes() const noexcept {
        return total_reserved_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t pool_reserved_bytes(MemoryPoolId pool) const noexcept {
        return pool_reserved_[static_cast<std::size_t>(pool)].load(std::memory_order_acquire);
    }

private:
    std::atomic<std::size_t> budget_bytes_;
    std::atomic<std::size_t> total_reserved_;
    std::array<std::atomic<std::size_t>, static_cast<std::size_t>(MemoryPoolId::Count)> pool_reserved_;
};

/// Page-aligned fixed-block Slab Pool governed by MemoryLedger.
template <typename ElemT, std::size_t BlockBytes>
class SlabPool {
    static_assert(BlockBytes % 4096 == 0, "Slab blocks must be 4096-byte page multiples");

public:
    static constexpr std::size_t kPageAlignment = 4096;

    explicit SlabPool(MemoryLedger& ledger) noexcept : ledger_(ledger) {}

    ~SlabPool() {
        purge_free_list();
        assert(active_allocations_.load(std::memory_order_relaxed) == 0 &&
               "All slab blocks must be returned before SlabPool destruction");
    }

    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    [[nodiscard]] ElemT* allocate() noexcept {
        if (!ledger_.try_reserve(BlockBytes, MemoryPoolId::ChunkSlab)) {
            return nullptr;
        }
        void* raw = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!free_list_.empty()) {
                raw = free_list_.back();
                free_list_.pop_back();
            }
        }
        if (!raw) {
            raw = aligned_allocate(kPageAlignment, BlockBytes);
            if (!raw) {
                ledger_.release(BlockBytes, MemoryPoolId::ChunkSlab);
                return nullptr;
            }
        }
        active_allocations_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<ElemT*>(raw);
    }

    void deallocate(ElemT* ptr) noexcept {
        if (!ptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_list_.push_back(ptr);
        }
        const std::size_t prev = active_allocations_.fetch_sub(1, std::memory_order_relaxed);
        assert(prev >= 1);
        (void)prev;
        ledger_.release(BlockBytes, MemoryPoolId::ChunkSlab);
    }

    void purge_free_list() noexcept {
        std::vector<void*> to_free;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            to_free.swap(free_list_);
        }
        for (void* p : to_free) {
            aligned_deallocate(p);
        }
    }

    [[nodiscard]] std::size_t active_blocks() const noexcept {
        return active_allocations_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t cached_free_blocks() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_list_.size();
    }

private:
    MemoryLedger& ledger_;
    mutable std::mutex mutex_;
    std::vector<void*> free_list_;
    std::atomic<std::size_t> active_allocations_{0};
};

using TsdfSlabPool = SlabPool<int16_t, TsdfFormat::kTsdfSlabBytes>;
using MaterialSlabPool = SlabPool<uint8_t, TsdfFormat::kMaterialSlabBytes>;

// ============================================================================
// Compact Chunk Residency Representation & Lifecycle State (Section 3.1, 3.2)
// ============================================================================
enum class ChunkStorageForm : uint8_t {
    Empty = 0, // 0 bytes: all 32^3 canonical samples are kClampedAir (+32767)
    Solid = 1, // 0 bytes: all 32^3 canonical samples are kClampedSolid (-32767)
    Dense = 2  // 64.0 KiB: full int16_t[32768] canonical sample array
};

enum class ChunkLifecycleState : uint8_t {
    Absent = 0,
    Requested = 1,
    Loading = 2,
    Generating = 3,
    Resident = 4,
    Unloading = 5
};

/// Authoritative per-chunk storage slot in ChunkStore (HLD v4.0 Sections 3.1, 3.2).
/// Stores ONLY canonical 32^3 samples (never persistent boundary aprons).
struct ChunkSlot {
    ChunkKey key{};
    mutable std::shared_mutex mutex;

    ChunkLifecycleState state = ChunkLifecycleState::Absent;
    ChunkStorageForm form = ChunkStorageForm::Empty;

    int16_t* tsdf_data = nullptr;     // Non-null iff form == ChunkStorageForm::Dense (64.0 KiB)
    uint8_t* material_data = nullptr; // Lazily allocated (32.0 KiB); null while uniform base_material_id
    uint8_t base_material_id = 0;

    std::atomic<uint64_t> version{0};
    std::atomic<uint32_t> pin_count{0};
    bool dirty_persist = false;

    explicit ChunkSlot(ChunkKey k) noexcept : key(k) {}

    ChunkSlot(const ChunkSlot&) = delete;
    ChunkSlot& operator=(const ChunkSlot&) = delete;

    /// Returns true if the material channel is lazily allocated (heterogeneous materials).
    [[nodiscard]] bool has_material_array() const noexcept {
        return material_data != nullptr;
    }

    /// Returns the authoritative heap bytes currently held by this chunk slot in ChunkStore.
    [[nodiscard]] std::size_t resident_heap_bytes() const noexcept {
        std::size_t bytes = (form == ChunkStorageForm::Dense && tsdf_data != nullptr)
                                ? TsdfFormat::kTsdfSlabBytes
                                : 0ULL;
        if (material_data != nullptr) {
            bytes += TsdfFormat::kMaterialSlabBytes;
        }
        return bytes;
    }

    /// Reads a canonical TSDF sample at local coordinate L in [0, 31]^3 (caller holds shared or exclusive lock).
    [[nodiscard]] int16_t get_tsdf(LocalCoord local) const noexcept {
        return get_tsdf_linear(local.to_linear_index());
    }

    [[nodiscard]] int16_t get_tsdf_linear(uint32_t idx) const noexcept {
        assert(idx < TsdfFormat::kChunkSamples);
        switch (form) {
            case ChunkStorageForm::Empty:
                return TsdfFormat::kClampedAir;
            case ChunkStorageForm::Solid:
                return TsdfFormat::kClampedSolid;
            case ChunkStorageForm::Dense:
                assert(tsdf_data != nullptr);
                return tsdf_data[idx];
        }
        return TsdfFormat::kClampedAir;
    }

    /// Reads a canonical material ID at local coordinate L in [0, 31]^3 (caller holds shared or exclusive lock).
    [[nodiscard]] uint8_t get_material(LocalCoord local) const noexcept {
        return get_material_linear(local.to_linear_index());
    }

    [[nodiscard]] uint8_t get_material_linear(uint32_t idx) const noexcept {
        assert(idx < TsdfFormat::kChunkSamples);
        return material_data ? material_data[idx] : base_material_id;
    }

    /// Resets the chunk to uniform EMPTY or SOLID representation, returning any slabs to the pools.
    void reset_to_uniform(ChunkStorageForm uniform_form,
                          uint8_t uniform_material,
                          TsdfSlabPool& tsdf_pool,
                          MaterialSlabPool& mat_pool) noexcept;

    /// Promotes an EMPTY or SOLID chunk to DENSE by allocating a 64.0 KiB slab and filling it
    /// with kClampedAir or kClampedSolid (caller holds exclusive write lock).
    [[nodiscard]] bool promote_to_dense(TsdfSlabPool& tsdf_pool) noexcept;

    /// Writes a canonical TSDF sample, promoting from EMPTY/SOLID to DENSE if the value differs
    /// from the uniform state (caller holds exclusive write lock).
    [[nodiscard]] bool set_tsdf(LocalCoord local, int16_t value, TsdfSlabPool& tsdf_pool, bool* out_changed = nullptr) noexcept;

    [[nodiscard]] bool set_tsdf_linear(uint32_t idx, int16_t value, TsdfSlabPool& tsdf_pool, bool* out_changed = nullptr) noexcept;

    /// Writes a canonical material ID, lazily allocating the 32.0 KiB material array only when
    /// `mat_id != base_material_id` (caller holds exclusive write lock).
    [[nodiscard]] bool set_material(LocalCoord local, uint8_t mat_id, MaterialSlabPool& mat_pool) noexcept;

    [[nodiscard]] bool set_material_linear(uint32_t idx, uint8_t mat_id, MaterialSlabPool& mat_pool) noexcept;

    /// Inspects all 32^3 canonical samples after an edit or generation pass; if all samples are
    /// uniformly kClampedAir or kClampedSolid, demotes the chunk to EMPTY or SOLID and returns
    /// its 64.0 KiB buffer to the slab pool (Section 3.1). Also compacts uniform material arrays.
    bool try_demote_if_uniform(TsdfSlabPool& tsdf_pool, MaterialSlabPool& mat_pool) noexcept;
};

// ============================================================================
// ChunkStore: Flat Index, Ordered Multi-Chunk Locking & 36^3 Scratch Gather
// ============================================================================
class ChunkStore {
public:
    static constexpr std::size_t kMaxEditPhaseCoreChunks = 32;
    static constexpr std::size_t kMaxEditLockChunks = 32;

    explicit ChunkStore(MemoryLedger& ledger, int32_t world_height_chunks = 8);
    ~ChunkStore();

    ChunkStore(const ChunkStore&) = delete;
    ChunkStore& operator=(const ChunkStore&) = delete;

    [[nodiscard]] TsdfSlabPool& tsdf_pool() noexcept { return tsdf_pool_; }
    [[nodiscard]] MaterialSlabPool& material_pool() noexcept { return material_pool_; }
    [[nodiscard]] MemoryLedger& ledger() noexcept { return ledger_; }
    [[nodiscard]] int32_t world_height_chunks() const noexcept { return world_height_chunks_; }

    /// Global atomic terrain revision incremented once per completed Edit transaction (Section 3.2).
    [[nodiscard]] uint64_t terrain_revision() const noexcept {
        return terrain_revision_.load(std::memory_order_acquire);
    }

    uint64_t increment_terrain_revision() noexcept {
        return terrain_revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    /// Finds or creates a ChunkSlot for `key`.
    [[nodiscard]] ChunkSlot* get_or_create_slot(ChunkKey key);

    /// Looks up an existing ChunkSlot for `key`, or returns nullptr if absent.
    [[nodiscard]] ChunkSlot* find_slot(ChunkKey key) const noexcept;

    /// Evicts a chunk slot from ChunkStore if its pin_count == 0, freeing any slabs back to the pools.
    bool evict_slot(ChunkKey key);

    /// Releases all chunks and purges slab free lists (Section 5.3 Step 6).
    void clear_all() noexcept;

    [[nodiscard]] std::size_t slot_count() const noexcept;

    /// RAII Write Lock Guard for an Edit Phase (HLD v4.0 Section 3.2).
    /// Acquires exclusive write locks in strictly ascending `ChunkKey::raw` order
    /// and releases them in strictly descending order. Enforces `kMaxEditLockChunks = 32`.
    class ScopedPhaseWriteLock {
    public:
        ScopedPhaseWriteLock(ChunkStore& store, std::vector<ChunkKey> keys);
        ~ScopedPhaseWriteLock();

        ScopedPhaseWriteLock(const ScopedPhaseWriteLock&) = delete;
        ScopedPhaseWriteLock& operator=(const ScopedPhaseWriteLock&) = delete;

        [[nodiscard]] const std::vector<ChunkSlot*>& slots() const noexcept { return slots_; }
        [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

    private:
        std::vector<ChunkSlot*> slots_;
    };

private:
    MemoryLedger& ledger_;
    TsdfSlabPool tsdf_pool_;
    MaterialSlabPool material_pool_;
    int32_t world_height_chunks_;
    std::atomic<uint64_t> terrain_revision_{0};

    mutable std::shared_mutex index_mutex_;
    std::unordered_map<ChunkKey, std::unique_ptr<ChunkSlot>, ChunkKeyHash> index_;
};

} // namespace voxel
