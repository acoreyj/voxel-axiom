#include "chunk_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using voxel::ChunkKey;
using voxel::ChunkLifecycleState;
using voxel::ChunkSlot;
using voxel::ChunkStorageForm;
using voxel::ChunkStore;
using voxel::LocalCoord;
using voxel::MemoryLedger;
using voxel::MemoryPoolId;
using voxel::TsdfFormat;

int g_tests_run = 0;
int g_assertions_checked = 0;

#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        ++g_assertions_checked;                                                       \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "[FAIL] %s:%d: Requirement failed: %s\n",            \
                         __FILE__, __LINE__, #cond);                                  \
            std::exit(EXIT_FAILURE);                                                  \
        }                                                                             \
    } while (0)

// ============================================================================
// 1. TSDF Quantization & Symmetry Verification (Section 3.1)
// ============================================================================
void test_tsdf_encoding_and_symmetry() {
    ++g_tests_run;

    // Clamped extremes must be symmetric (-32767 and +32767; never -32768).
    REQUIRE(TsdfFormat::kClampedSolid == -32767);
    REQUIRE(TsdfFormat::kClampedAir == +32767);
    REQUIRE(-TsdfFormat::kClampedSolid == TsdfFormat::kClampedAir);

    // Zero isosurface maps to 0.
    REQUIRE(TsdfFormat::encode(0.0f) == 0);
    REQUIRE(TsdfFormat::decode(0) == 0.0f);

    // Clamping outside [-delta, +delta] (delta = 2.0)
    REQUIRE(TsdfFormat::encode(-2.0f) == TsdfFormat::kClampedSolid);
    REQUIRE(TsdfFormat::encode(-5.0f) == TsdfFormat::kClampedSolid);
    REQUIRE(TsdfFormat::encode(+2.0f) == TsdfFormat::kClampedAir);
    REQUIRE(TsdfFormat::encode(+10.0f) == TsdfFormat::kClampedAir);

    // Mid-band accuracy and sign symmetry
    const int16_t q_neg_half = TsdfFormat::encode(-1.0f, 2.0f);
    const int16_t q_pos_half = TsdfFormat::encode(+1.0f, 2.0f);
    REQUIRE(q_neg_half == -q_pos_half);
    REQUIRE(std::abs(TsdfFormat::decode(q_neg_half, 2.0f) - (-1.0f)) < 1e-4f);
    REQUIRE(std::abs(TsdfFormat::decode(q_pos_half, 2.0f) - (+1.0f)) < 1e-4f);

    std::printf("[PASS] test_tsdf_encoding_and_symmetry (symmetric +/-32767 bounds, 0 isosurface)\n");
}

// ============================================================================
// 2. Compact Residency (EMPTY / SOLID / DENSE) & Demotion (Section 3.1)
// ============================================================================
void test_compact_residency_and_demotion() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger);
    ChunkSlot* slot = store.get_or_create_slot(ChunkKey::from_coords(0, 0, 0));

    // Default chunk is EMPTY with 0 bytes allocated in ChunkStore or ledger.
    REQUIRE(slot->form == ChunkStorageForm::Empty);
    REQUIRE(slot->resident_heap_bytes() == 0ULL);
    REQUIRE(ledger.total_reserved_bytes() == 0ULL);
    REQUIRE(slot->get_tsdf(LocalCoord{15, 15, 15}) == TsdfFormat::kClampedAir);

    // Resetting to SOLID keeps 0 bytes allocated and reads kClampedSolid everywhere.
    slot->reset_to_uniform(ChunkStorageForm::Solid, 1, store.tsdf_pool(), store.material_pool());
    REQUIRE(slot->form == ChunkStorageForm::Solid);
    REQUIRE(slot->resident_heap_bytes() == 0ULL);
    REQUIRE(ledger.total_reserved_bytes() == 0ULL);
    REQUIRE(slot->get_tsdf(LocalCoord{0, 0, 0}) == TsdfFormat::kClampedSolid);
    REQUIRE(slot->get_tsdf(LocalCoord{31, 31, 31}) == TsdfFormat::kClampedSolid);

    // Writing kClampedSolid to a SOLID chunk is a no-op (stays 0 bytes).
    bool changed = false;
    REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 10}, TsdfFormat::kClampedSolid, store.tsdf_pool(), &changed));
    REQUIRE(!changed);
    REQUIRE(slot->form == ChunkStorageForm::Solid);
    REQUIRE(slot->resident_heap_bytes() == 0ULL);

    // Carving a single sample promotes SOLID -> DENSE (allocating 64.0 KiB = 65,536 B).
    REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 10}, 0, store.tsdf_pool(), &changed));
    REQUIRE(changed);
    REQUIRE(slot->form == ChunkStorageForm::Dense);
    REQUIRE(slot->resident_heap_bytes() == 65536ULL);
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 65536ULL);
    REQUIRE(slot->get_tsdf(LocalCoord{10, 10, 10}) == 0);
    REQUIRE(slot->get_tsdf(LocalCoord{9, 10, 10}) == TsdfFormat::kClampedSolid);

    // Heterogeneous chunk must NOT demote.
    REQUIRE(!slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool()));
    REQUIRE(slot->form == ChunkStorageForm::Dense);

    // Hollowing out all 32^3 samples to kClampedAir demotes DENSE -> EMPTY and frees 64.0 KiB.
    for (uint32_t i = 0; i < TsdfFormat::kChunkSamples; ++i) {
        REQUIRE(slot->set_tsdf_linear(i, TsdfFormat::kClampedAir, store.tsdf_pool()));
    }
    REQUIRE(slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool()));
    REQUIRE(slot->form == ChunkStorageForm::Empty);
    REQUIRE(slot->resident_heap_bytes() == 0ULL);
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 0ULL);

    std::printf("[PASS] test_compact_residency_and_demotion (0 B EMPTY/SOLID, 64 KiB DENSE, auto-demotion)\n");
}

// ============================================================================
// 3. Lazy Material Channel Allocation & Compaction (Section 3.1)
// ============================================================================
void test_lazy_material_channel() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger);
    ChunkSlot* slot = store.get_or_create_slot(ChunkKey::from_coords(1, 2, 3));
    slot->reset_to_uniform(ChunkStorageForm::Solid, /*uniform_material=*/4, store.tsdf_pool(), store.material_pool());

    // Uniform material uses 0 bytes.
    REQUIRE(!slot->has_material_array());
    REQUIRE(slot->get_material(LocalCoord{5, 6, 7}) == 4);
    REQUIRE(slot->resident_heap_bytes() == 0ULL);

    // Writing the same material ID (4) keeps the array unallocated.
    REQUIRE(slot->set_material(LocalCoord{5, 6, 7}, 4, store.material_pool()));
    REQUIRE(!slot->has_material_array());
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 0ULL);

    // Writing a distinct material ID (9) lazily allocates the 32.0 KiB material slab.
    REQUIRE(slot->set_material(LocalCoord{5, 6, 7}, 9, store.material_pool()));
    REQUIRE(slot->has_material_array());
    REQUIRE(slot->resident_heap_bytes() == 32768ULL);
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 32768ULL);
    REQUIRE(slot->get_material(LocalCoord{5, 6, 7}) == 9);
    REQUIRE(slot->get_material(LocalCoord{0, 0, 0}) == 4);

    // Combined DENSE TSDF (64 KiB) + heterogeneous material (32 KiB) == 96.0 KiB (98,304 B).
    REQUIRE(slot->promote_to_dense(store.tsdf_pool()));
    REQUIRE(slot->resident_heap_bytes() == 98304ULL);
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 98304ULL);

    // Restoring sample (5, 6, 7) to material 4 and running compaction frees the 32.0 KiB material slab.
    REQUIRE(slot->set_material(LocalCoord{5, 6, 7}, 4, store.material_pool()));
    slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
    REQUIRE(!slot->has_material_array());
    REQUIRE(slot->get_material(LocalCoord{31, 31, 31}) == 4);

    std::printf("[PASS] test_lazy_material_channel (lazy 32 KiB allocation, 96 KiB max per chunk)\n");
}

// ============================================================================
// 4. Hard Memory Budget Ledger Enforcement (Section 3.3)
// ============================================================================
void test_memory_budget_ledger_ceiling() {
    ++g_tests_run;

    // Configure ledger with exactly 128 KiB (enough for two 64 KiB TSDF slabs).
    MemoryLedger ledger(128ULL * 1024ULL);
    ChunkStore store(ledger);

    ChunkSlot* c0 = store.get_or_create_slot(ChunkKey::from_coords(0, 0, 0));
    ChunkSlot* c1 = store.get_or_create_slot(ChunkKey::from_coords(1, 0, 0));
    ChunkSlot* c2 = store.get_or_create_slot(ChunkKey::from_coords(2, 0, 0));

    REQUIRE(c0->promote_to_dense(store.tsdf_pool()));
    REQUIRE(c1->promote_to_dense(store.tsdf_pool()));
    REQUIRE(ledger.total_reserved_bytes() == 128ULL * 1024ULL);

    // Third 64 KiB allocation must be rejected cleanly by the hard budget ceiling.
    REQUIRE(!c2->promote_to_dense(store.tsdf_pool()));
    REQUIRE(c2->form == ChunkStorageForm::Empty);

    // Evicting c0 returns its 64 KiB block to the slab free list and releases ledger bytes,
    // allowing c2 to promote by reusing the pooled page-aligned block.
    REQUIRE(store.evict_slot(ChunkKey::from_coords(0, 0, 0)));
    REQUIRE(ledger.total_reserved_bytes() == 64ULL * 1024ULL);
    REQUIRE(store.tsdf_pool().cached_free_blocks() == 1ULL);

    REQUIRE(c2->promote_to_dense(store.tsdf_pool()));
    REQUIRE(ledger.total_reserved_bytes() == 128ULL * 1024ULL);

    // Verify clean teardown releases all remaining blocks to zero.
    store.clear_all();
    REQUIRE(ledger.total_reserved_bytes() == 0ULL);

    std::printf("[PASS] test_memory_budget_ledger_ceiling (hard budget cap, slab reuse, zero leak teardown)\n");
}

// ============================================================================
// 5. Canonical-Only 32-Chunk Phase Write Lock Ordering (Section 3.2)
// ============================================================================
void test_scoped_phase_write_lock_ordering() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger);

    // Build an unsorted 32-chunk list with duplicates and negative/positive coordinates.
    std::vector<ChunkKey> keys;
    for (int32_t z = -1; z <= 2; ++z) {
        for (int32_t y = 0; y <= 1; ++y) {
            for (int32_t x = -2; x <= 1; ++x) {
                keys.push_back(ChunkKey::from_coords(x, y, z));
            }
        }
    }
    REQUIRE(keys.size() == 32ULL);
    // Add duplicate keys to verify deduplication.
    keys.push_back(keys.front());
    keys.push_back(keys.back());

    {
        ChunkStore::ScopedPhaseWriteLock phase_lock(store, keys);
        REQUIRE(phase_lock.size() == 32ULL);

        // Verify slots are locked in strictly ascending ChunkKey::raw order.
        for (std::size_t i = 0; i < phase_lock.size(); ++i) {
            if (i > 0) {
                REQUIRE(phase_lock.slots()[i - 1]->key.raw < phase_lock.slots()[i]->key.raw);
            }
            // Exclusive lock must already be held (try_lock_shared must fail).
            REQUIRE(!phase_lock.slots()[i]->mutex.try_lock_shared());
            phase_lock.slots()[i]->version.fetch_add(1, std::memory_order_release);
        }
    }

    // After ScopedPhaseWriteLock destructor, all locks are released.
    for (const ChunkKey k : keys) {
        ChunkSlot* slot = store.find_slot(k);
        REQUIRE(slot != nullptr);
        REQUIRE(slot->mutex.try_lock_shared());
        slot->mutex.unlock_shared();
        REQUIRE(slot->version.load(std::memory_order_acquire) == 1ULL);
    }

    std::printf("[PASS] test_scoped_phase_write_lock_ordering (32-chunk cap, ascending raw lock order)\n");
}

} // namespace

int main() {
    std::printf("=== Running M0 Step 2 Unit Test Suite: ChunkStore, Compact Residency & Slab Pools ===\n");
    test_tsdf_encoding_and_symmetry();
    test_compact_residency_and_demotion();
    test_lazy_material_channel();
    test_memory_budget_ledger_ceiling();
    test_scoped_phase_write_lock_ordering();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
