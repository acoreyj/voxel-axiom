#include "scratch_and_dirty.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using voxel::ChunkKey;
using voxel::ChunkLifecycleState;
using voxel::ChunkSlot;
using voxel::ChunkStorageForm;
using voxel::ChunkStore;
using voxel::DirtyQueueLane;
using voxel::DualDirtyScheduler;
using voxel::LocalCoord;
using voxel::MemoryLedger;
using voxel::MemoryPoolId;
using voxel::NeighborhoodVersionStamp;
using voxel::SelectiveDirtyMask;
using voxel::TsdfFormat;
using voxel::WorkerScratchWindow36;
using voxel::gather_scratch_window_36;
using voxel::validate_neighborhood_stamp;

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
// 1. Selective Neighbor Dirtying Unit Tests (Section 7.1 Exact Cases)
// ============================================================================
void test_selective_neighbor_dirtying() {
    ++g_tests_run;

    const ChunkKey c = ChunkKey::from_coords(4, 2, -3);

    // Case 1 (Section 7.1): Mutate interior sample (15, 15, 15) in chunk C:
    // Assert ONLY chunk C is dirtied (0 neighbors dirtied).
    {
        SelectiveDirtyMask mask;
        mask.record_mutated_sample(LocalCoord{15, 15, 15});
        std::vector<ChunkKey> dirty;
        mask.emit_dirty_keys(c, dirty);
        REQUIRE(dirty.size() == 1ULL);
        REQUIRE(dirty[0] == c);
    }

    // Verify the entire interior box [3..30]^3 never dirties any neighbor.
    {
        SelectiveDirtyMask mask;
        for (int32_t z = 3; z <= 30; ++z) {
            for (int32_t y = 3; y <= 30; ++y) {
                for (int32_t x = 3; x <= 30; ++x) {
                    mask.record_mutated_sample(LocalCoord{x, y, z});
                }
            }
        }
        std::vector<ChunkKey> dirty;
        mask.emit_dirty_keys(c, dirty);
        REQUIRE(dirty.size() == 1ULL);
        REQUIRE(dirty[0] == c);
    }

    // Case 2 (Section 7.1): Mutate sample (2, 15, 15) in chunk C:
    // Assert ONLY chunk C and C - x_hat are dirtied.
    {
        SelectiveDirtyMask mask;
        mask.record_mutated_sample(LocalCoord{2, 15, 15});
        std::vector<ChunkKey> dirty;
        mask.emit_dirty_keys(c, dirty);
        REQUIRE(dirty.size() == 2ULL);
        REQUIRE(std::find(dirty.begin(), dirty.end(), c) != dirty.end());
        REQUIRE(std::find(dirty.begin(), dirty.end(), c.offset(-1, 0, 0)) != dirty.end());
    }

    // Also verify samples 0 and 1 on X dirty C and C - x_hat.
    for (int32_t lx : {0, 1}) {
        SelectiveDirtyMask mask;
        mask.record_mutated_sample(LocalCoord{lx, 15, 15});
        std::vector<ChunkKey> dirty;
        mask.emit_dirty_keys(c, dirty);
        REQUIRE(dirty.size() == 2ULL);
        REQUIRE(std::find(dirty.begin(), dirty.end(), c) != dirty.end());
        REQUIRE(std::find(dirty.begin(), dirty.end(), c.offset(-1, 0, 0)) != dirty.end());
    }

    // Case 3 (Section 7.1): Mutate sample (31, 15, 15) in chunk C:
    // Assert ONLY chunk C and C + x_hat are dirtied.
    {
        SelectiveDirtyMask mask;
        mask.record_mutated_sample(LocalCoord{31, 15, 15});
        std::vector<ChunkKey> dirty;
        mask.emit_dirty_keys(c, dirty);
        REQUIRE(dirty.size() == 2ULL);
        REQUIRE(std::find(dirty.begin(), dirty.end(), c) != dirty.end());
        REQUIRE(std::find(dirty.begin(), dirty.end(), c.offset(+1, 0, 0)) != dirty.end());
    }

    // Case 4: Corner sample (0, 31, 2) dirties 2 * 2 * 2 = 8 chunks (C and 7 edge/face/corner neighbors).
    {
        SelectiveDirtyMask mask;
        mask.record_mutated_sample(LocalCoord{0, 31, 2});
        std::vector<ChunkKey> dirty;
        mask.emit_dirty_keys(c, dirty);
        REQUIRE(dirty.size() == 8ULL);
        REQUIRE(std::find(dirty.begin(), dirty.end(), c.offset(-1, +1, -1)) != dirty.end());
    }

    std::printf("[PASS] test_selective_neighbor_dirtying (Section 7.1 interior/low/high boundary rules)\n");
}

// ============================================================================
// 2. 36^3 (-1..34) Scratch Window Gather & Snapshot Isolation (Sections 3.1, 3.2)
// ============================================================================
void test_36_cubed_scratch_gather_and_stamp() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerScratchWindow36 scratch(ledger);

    // Verify scratch window reserved bytes in MemoryPoolId::WorkerScratch.
    REQUIRE(ledger.pool_reserved_bytes(MemoryPoolId::WorkerScratch) == WorkerScratchWindow36::kScratchBytes);

    const ChunkKey center = ChunkKey::from_coords(0, 0, 0);

    // Populate center chunk (0, 0, 0) as Resident SOLID, with a distinctive sample at (10, 10, 10).
    ChunkSlot* c_slot = store.get_or_create_slot(center);
    {
        std::unique_lock<std::shared_mutex> lock(c_slot->mutex);
        c_slot->reset_to_uniform(ChunkStorageForm::Solid, /*mat=*/3, store.tsdf_pool(), store.material_pool());
        REQUIRE(c_slot->set_tsdf(LocalCoord{10, 10, 10}, -1234, store.tsdf_pool()));
        REQUIRE(c_slot->set_material(LocalCoord{10, 10, 10}, 7, store.material_pool()));
        c_slot->version.store(1, std::memory_order_release);
        c_slot->state = ChunkLifecycleState::Resident;
    }

    // Populate -X neighbor (-1, 0, 0) with a distinctive value at canonical local x = 31 (scratch x = -1).
    ChunkSlot* neg_x = store.get_or_create_slot(center.offset(-1, 0, 0));
    {
        std::unique_lock<std::shared_mutex> lock(neg_x->mutex);
        neg_x->reset_to_uniform(ChunkStorageForm::Empty, /*mat=*/1, store.tsdf_pool(), store.material_pool());
        REQUIRE(neg_x->set_tsdf(LocalCoord{31, 5, 5}, -4321, store.tsdf_pool()));
        REQUIRE(neg_x->set_material(LocalCoord{31, 5, 5}, 12, store.material_pool()));
        neg_x->version.store(1, std::memory_order_release);
        neg_x->state = ChunkLifecycleState::Resident;
    }

    // Populate +X neighbor (+1, 0, 0) with distinctive values at canonical local x = 0, 1, 2 (scratch x = 32, 33, 34).
    ChunkSlot* pos_x = store.get_or_create_slot(center.offset(+1, 0, 0));
    {
        std::unique_lock<std::shared_mutex> lock(pos_x->mutex);
        pos_x->reset_to_uniform(ChunkStorageForm::Empty, /*mat=*/2, store.tsdf_pool(), store.material_pool());
        REQUIRE(pos_x->set_tsdf(LocalCoord{0, 8, 8}, 100, store.tsdf_pool()));
        REQUIRE(pos_x->set_tsdf(LocalCoord{1, 8, 8}, 200, store.tsdf_pool()));
        REQUIRE(pos_x->set_tsdf(LocalCoord{2, 8, 8}, 300, store.tsdf_pool()));
        REQUIRE(pos_x->set_material(LocalCoord{2, 8, 8}, 19, store.material_pool()));
        pos_x->version.store(1, std::memory_order_release);
        pos_x->state = ChunkLifecycleState::Resident;
    }

    // Gather the 36^3 scratch window for center (0, 0, 0).
    const NeighborhoodVersionStamp stamp = gather_scratch_window_36(store, center, scratch);

    // 1. Verify center sample (10, 10, 10)
    REQUIRE(scratch.get_tsdf(10, 10, 10) == -1234);
    REQUIRE(scratch.get_material(10, 10, 10) == 7);

    // 2. Verify -X apron at scratch (-1, 5, 5) came from neg_x canonical (31, 5, 5)
    REQUIRE(scratch.get_tsdf(-1, 5, 5) == -4321);
    REQUIRE(scratch.get_material(-1, 5, 5) == 12);

    // 3. Verify +X apron at scratch (32..34, 8, 8) came from pos_x canonical (0..2, 8, 8)
    REQUIRE(scratch.get_tsdf(32, 8, 8) == 100);
    REQUIRE(scratch.get_tsdf(33, 8, 8) == 200);
    REQUIRE(scratch.get_tsdf(34, 8, 8) == 300);
    REQUIRE(scratch.get_material(34, 8, 8) == 19);

    // 4. Verify non-resident below-bedrock neighbor (Cy = -1 -> scratch y = -1) filled with kClampedSolid.
    REQUIRE(scratch.get_tsdf(15, -1, 15) == TsdfFormat::kClampedSolid);

    // 5. Verify non-resident above-bedrock neighbor (Cy = +1 -> scratch y = 32..34) filled with kClampedAir.
    REQUIRE(scratch.get_tsdf(15, 32, 15) == TsdfFormat::kClampedAir);
    REQUIRE(scratch.get_tsdf(15, 34, 15) == TsdfFormat::kClampedAir);

    // 6. Verify full [-1..34]^3 bounds scan under ASan (corner normals -1..34, corners 0..33, cells 0..32).
    int64_t checksum = 0;
    for (int32_t sz = -1; sz <= 34; ++sz) {
        for (int32_t sy = -1; sy <= 34; ++sy) {
            for (int32_t sx = -1; sx <= 34; ++sx) {
                checksum += scratch.get_tsdf(sx, sy, sz);
            }
        }
    }
    REQUIRE(checksum != 0);

    // 7. Verify snapshot stamp validation succeeds before mutation and fails if a neighbor version advances.
    REQUIRE(validate_neighborhood_stamp(store, stamp));
    pos_x->version.fetch_add(1, std::memory_order_acq_rel);
    REQUIRE(!validate_neighborhood_stamp(store, stamp));

    std::printf("[PASS] test_36_cubed_scratch_gather_and_stamp (36^3 [-1..34] gather, bedrock/sky fill, stamp check)\n");
}

// ============================================================================
// 3. Dual Dirty-Set Priority & Soft/Hard Backpressure (Sections 3.4, 7.1)
// ============================================================================
void test_dual_dirty_scheduler_priority_and_caps() {
    ++g_tests_run;

    DualDirtyScheduler scheduler;
    scheduler.set_focus_chunk(ChunkKey::from_coords(0, 0, 0));

    // Section 7.1 test case:
    // "Enqueue 50 chunks in dirty_stream_chunks and 4 chunks in dirty_edit_chunks;
    //  assert the 4 edit-dirty chunks are meshed first."
    for (int32_t i = 1; i <= 50; ++i) {
        scheduler.mark_stream_dirty(ChunkKey::from_coords(i, 0, 0));
    }
    const std::vector<ChunkKey> edit_batch = {
        ChunkKey::from_coords(0, 0, 4), // dist^2 = 16
        ChunkKey::from_coords(0, 0, 1), // dist^2 = 1
        ChunkKey::from_coords(0, 0, 3), // dist^2 = 9
        ChunkKey::from_coords(0, 0, 2)  // dist^2 = 4
    };
    scheduler.publish_edit_transaction_dirty_batch(edit_batch);

    REQUIRE(scheduler.edit_dirty_count() == 4ULL);
    REQUIRE(scheduler.stream_dirty_count() == 50ULL);

    // Even when completion_queue_depth == 8 (at soft cap, where stream-dirty is paused),
    // edit-dirty chunks MUST still be admitted and popped in nearest-first order (z = 1, 2, 3, 4).
    for (int32_t expected_z = 1; expected_z <= 4; ++expected_z) {
        auto popped = scheduler.try_pop_next_mesh_job(/*completion_queue_depth=*/8);
        REQUIRE(popped.has_value());
        REQUIRE(popped->lane == DirtyQueueLane::EditDirty);
        REQUIRE(popped->key == ChunkKey::from_coords(0, 0, expected_z));
    }
    REQUIRE(scheduler.edit_dirty_count() == 0ULL);

    // At completion_queue_depth == 8 (soft cap), stream-dirty chunks must be paused.
    REQUIRE(!scheduler.try_pop_next_mesh_job(/*completion_queue_depth=*/8).has_value());

    // Once completion_queue_depth drops below 8 (e.g., 7), stream-dirty chunks drain FIFO in
    // insertion order (Audit F3), which here is x = 1..50.
    auto first_stream = scheduler.try_pop_next_mesh_job(/*completion_queue_depth=*/7);
    REQUIRE(first_stream.has_value());
    REQUIRE(first_stream->lane == DirtyQueueLane::StreamDirty);
    REQUIRE(first_stream->key == ChunkKey::from_coords(1, 0, 0));

    // Promoting an existing stream-dirty chunk (x = 10) to edit-dirty removes it from stream-dirty
    // and schedules it immediately in the EditDirty lane.
    const ChunkKey promoted_key = ChunkKey::from_coords(10, 0, 0);
    REQUIRE(scheduler.contains_stream_dirty(promoted_key));
    scheduler.mark_edit_dirty(promoted_key);
    REQUIRE(!scheduler.contains_stream_dirty(promoted_key));
    REQUIRE(scheduler.contains_edit_dirty(promoted_key));

    // At hard cap (completion_queue_depth == 32), even edit-dirty admission pauses.
    REQUIRE(!scheduler.try_pop_next_mesh_job(/*completion_queue_depth=*/32).has_value());

    // At depth 31, the promoted edit-dirty chunk pops immediately ahead of remaining stream-dirty chunks.
    auto popped_promoted = scheduler.try_pop_next_mesh_job(/*completion_queue_depth=*/31);
    REQUIRE(popped_promoted.has_value());
    REQUIRE(popped_promoted->lane == DirtyQueueLane::EditDirty);
    REQUIRE(popped_promoted->key == promoted_key);

    std::printf("[PASS] test_dual_dirty_scheduler_priority_and_caps (4 edit before 50 stream, soft=8 / hard=32 caps)\n");
}

} // namespace

int main() {
    std::printf("=== Running M0 Step 3 Unit Test Suite: 36^3 Scratch Gather, Selective Dirtying & Dual Queues ===\n");
    test_selective_neighbor_dirtying();
    test_36_cubed_scratch_gather_and_stamp();
    test_dual_dirty_scheduler_priority_and_caps();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
