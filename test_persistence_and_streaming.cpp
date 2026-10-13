#include "persistence_and_streaming.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using voxel::CaveNoiseGenerator;
using voxel::ChunkKey;
using voxel::ChunkLifecycleState;
using voxel::ChunkSlot;
using voxel::ChunkStorageForm;
using voxel::ChunkStore;
using voxel::LocalCoord;
using voxel::MemoryLedger;
using voxel::MemoryPoolId;
using voxel::RegionKey;
using voxel::RegionSaveManager;
using voxel::TsdfFormat;
using voxel::WorkerDensityGrid34;

int g_tests_run = 0;
int g_assertions_checked = 0;

#define REQUIRE(...)                                                                  \
    do {                                                                              \
        ++g_assertions_checked;                                                       \
        if (!(__VA_ARGS__)) {                                                         \
            std::fprintf(stderr, "[FAIL] %s:%d: Requirement failed: %s\n",            \
                         __FILE__, __LINE__, #__VA_ARGS__);                           \
            std::exit(EXIT_FAILURE);                                                  \
        }                                                                             \
    } while (0)

// ============================================================================
// 1. 3D 8x8x8 RegionKey Floor Mapping & TOC Index Verification (Section 3.6)
// ============================================================================
void test_region_key_8x8x8_mapping() {
    ++g_tests_run;

    // Negative chunk coordinates must map via floor_div(C, 8) and floor_mod(C, 8).
    const ChunkKey neg_ck = ChunkKey::from_coords(-1, -8, -9);
    const RegionKey rk = RegionKey::from_chunk_key(neg_ck);
    REQUIRE(rk.rx == -1);
    REQUIRE(rk.ry == -1);
    REQUIRE(rk.rz == -2);

    // Local coordinates inside region: (-1 mod 8 = 7), (-8 mod 8 = 0), (-9 mod 8 = 7)
    const uint16_t toc_idx = RegionKey::local_toc_index(neg_ck);
    REQUIRE(toc_idx == static_cast<uint16_t>(7 + 8 * (0 + 8 * 7)));
    REQUIRE(toc_idx < 512U);

    std::printf("[PASS] test_region_key_8x8x8_mapping (8x8x8 3D region floor math and 512-entry TOC index)\n");
}

// ============================================================================
// 2. RAM Blob Reload Before Disk Flush (Section 3.3, Section 3.6, Section 7.1)
// ============================================================================
void test_ram_blob_reload_before_disk_flush() {
    ++g_tests_run;

    const std::filesystem::path test_dir = "/tmp/voxel_test_ram_blob";
    std::filesystem::remove_all(test_dir);

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerDensityGrid34 grid34(ledger);
    CaveNoiseGenerator procgen;
    RegionSaveManager save_mgr(ledger, test_dir);

    // Section 7.1 Test Case:
    // "Edit a chunk, run PersistCompress, evict the chunk from ChunkStore *before*
    //  RegionFlush runs, then reload the chunk; verify bit-exact recovery from RegionBlobTable."
    const ChunkKey ck = ChunkKey::from_coords(3, 1, -2);
    ChunkSlot* slot = store.get_or_create_slot(ck);
    std::vector<int16_t> expected_tsdf(TsdfFormat::kChunkSamples);
    std::vector<uint8_t> expected_mat(TsdfFormat::kChunkSamples);

    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(ChunkStorageForm::Solid, /*base_mat=*/2, store.tsdf_pool(), store.material_pool());
        slot->state = ChunkLifecycleState::Resident;
        slot->version.store(1, std::memory_order_release);

        // Apply heterogeneous TSDF edits and heterogeneous material edits
        for (int32_t z = 0; z < 32; ++z) {
            for (int32_t y = 0; y < 32; ++y) {
                for (int32_t x = 0; x < 32; ++x) {
                    const LocalCoord lc{x, y, z};
                    const int16_t val = static_cast<int16_t>(((x * 73 + y * 19 + z * 7) % 4001) - 2000);
                    const uint8_t mat = static_cast<uint8_t>((x + y + z) % 6);
                    REQUIRE(slot->set_tsdf(lc, val, store.tsdf_pool()));
                    REQUIRE(slot->set_material(lc, mat, store.material_pool()));
                    expected_tsdf[lc.to_linear_index()] = val;
                    expected_mat[lc.to_linear_index()] = mat;
                }
            }
        }
        slot->dirty_persist = true;
        slot->version.fetch_add(1, std::memory_order_release);
    }

    // Run Stage 1: PersistCompress (stores compressed byte-plane blob in RegionBlobTable in RAM).
    REQUIRE(save_mgr.persist_compress_chunk(store, ck));
    REQUIRE(!slot->dirty_persist);
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::RegionBlobCache) > 0ULL);

    // Verify NO .vreg file has been written to disk yet!
    const RegionKey rk = RegionKey::from_chunk_key(ck);
    REQUIRE(!std::filesystem::exists(save_mgr.region_file_path(rk)));

    // Evict the chunk from ChunkStore before RegionFlush runs.
    REQUIRE(store.evict_slot(ck));
    REQUIRE(store.find_slot(ck) == nullptr);
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::ChunkSlab) == 0ULL);

    // Reload the chunk via load_or_generate_chunk: must recover bit-exactly from RegionBlobTable!
    REQUIRE(save_mgr.load_or_generate_chunk(store, ck, procgen, grid34));
    const ChunkSlot* reloaded = store.find_slot(ck);
    REQUIRE(reloaded != nullptr);
    REQUIRE(reloaded->state == ChunkLifecycleState::Resident);
    REQUIRE(reloaded->form == ChunkStorageForm::Dense);

    for (uint32_t i = 0; i < TsdfFormat::kChunkSamples; ++i) {
        REQUIRE(reloaded->get_tsdf_linear(i) == expected_tsdf[i]);
        REQUIRE(reloaded->get_material_linear(i) == expected_mat[i]);
    }

    save_mgr.clear_ram_cache();
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::RegionBlobCache) == 0ULL);
    std::filesystem::remove_all(test_dir);

    std::printf("[PASS] test_ram_blob_reload_before_disk_flush (32,768 TSDF + material samples bit-exact from RAM)\n");
}

// ============================================================================
// 3. Crash-Safe Atomic 8x8x8 Region Flush Verification (Section 3.6, Section 7.1)
// ============================================================================
void test_crash_safe_atomic_region_flush() {
    ++g_tests_run;

    const std::filesystem::path test_dir = "/tmp/voxel_test_atomic_flush";
    std::filesystem::remove_all(test_dir);

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerDensityGrid34 grid34(ledger);
    CaveNoiseGenerator procgen;
    RegionSaveManager save_mgr(ledger, test_dir);

    const ChunkKey ck_dense = ChunkKey::from_coords(1, 1, 1);
    const ChunkKey ck_empty = ChunkKey::from_coords(2, 1, 1);
    const RegionKey rk = RegionKey::from_chunk_key(ck_dense);

    // 1. Edit and flush revision 1 of the region to disk.
    {
        ChunkSlot* s_dense = store.get_or_create_slot(ck_dense);
        std::unique_lock<std::shared_mutex> lock(s_dense->mutex);
        s_dense->reset_to_uniform(ChunkStorageForm::Solid, 3, store.tsdf_pool(), store.material_pool());
        REQUIRE(s_dense->set_tsdf(LocalCoord{10, 10, 10}, 12345, store.tsdf_pool()));
        s_dense->state = ChunkLifecycleState::Resident;
        s_dense->version.store(1, std::memory_order_release);
    }
    {
        ChunkSlot* s_empty = store.get_or_create_slot(ck_empty);
        std::unique_lock<std::shared_mutex> lock(s_empty->mutex);
        s_empty->reset_to_uniform(ChunkStorageForm::Empty, 5, store.tsdf_pool(), store.material_pool());
        s_empty->state = ChunkLifecycleState::Resident;
        s_empty->version.store(1, std::memory_order_release);
    }

    REQUIRE(save_mgr.persist_compress_chunk(store, ck_dense));
    REQUIRE(save_mgr.persist_compress_chunk(store, ck_empty));
    REQUIRE(save_mgr.flush_region_to_disk(rk));
    REQUIRE(std::filesystem::exists(save_mgr.region_file_path(rk)));

    // 2. Section 7.1 Crash Test:
    //    Simulate a power loss / crash mid-flush by writing a truncated/corrupted `.vreg.tmp` file
    //    and clearing both the RAM cache and ChunkStore.
    save_mgr.clear_ram_cache();
    store.clear_all();

    {
        std::ofstream corrupt_tmp(save_mgr.region_tmp_path(rk), std::ios::binary | std::ios::trunc);
        const char garbage[] = "VREG_TRUNCATED_PARTIAL_WRITE";
        corrupt_tmp.write(garbage, sizeof(garbage));
    }
    REQUIRE(std::filesystem::exists(save_mgr.region_tmp_path(rk)));

    // 3. Verify the previous committed `region_0_0_0.vreg` loads cleanly with zero corruption!
    REQUIRE(save_mgr.load_or_generate_chunk(store, ck_dense, procgen, grid34));
    REQUIRE(save_mgr.load_or_generate_chunk(store, ck_empty, procgen, grid34));

    const ChunkSlot* loaded_dense = store.find_slot(ck_dense);
    const ChunkSlot* loaded_empty = store.find_slot(ck_empty);
    REQUIRE(loaded_dense != nullptr);
    REQUIRE(loaded_empty != nullptr);

    REQUIRE(loaded_dense->form == ChunkStorageForm::Dense);
    REQUIRE(loaded_dense->get_tsdf(LocalCoord{10, 10, 10}) == 12345);
    REQUIRE(loaded_dense->get_tsdf(LocalCoord{0, 0, 0}) == TsdfFormat::kClampedSolid);

    REQUIRE(loaded_empty->form == ChunkStorageForm::Empty);
    REQUIRE(loaded_empty->resident_heap_bytes() == 0ULL);
    REQUIRE(loaded_empty->base_material_id == 5);

    std::filesystem::remove_all(test_dir);
    std::printf("[PASS] test_crash_safe_atomic_region_flush (truncated .vreg.tmp ignored, committed .vreg recovered)\n");
}

} // namespace

int main() {
    std::printf("=== Running M4 Step 6 Unit Test Suite: Two-Stage Persistence & 8x8x8 Region Files ===\n");
    test_region_key_8x8x8_mapping();
    test_ram_blob_reload_before_disk_flush();
    test_crash_safe_atomic_region_flush();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
