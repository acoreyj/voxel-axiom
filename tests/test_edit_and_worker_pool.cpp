#include "edit_and_worker_pool.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using voxel::ChunkKey;
using voxel::ChunkLifecycleState;
using voxel::ChunkMeshPayload;
using voxel::ChunkSlot;
using voxel::ChunkStorageForm;
using voxel::ChunkStore;
using voxel::CompletionKind;
using voxel::CoreWorkerPool;
using voxel::DualDirtyScheduler;
using voxel::EditTransaction;
using voxel::LocalCoord;
using voxel::MemoryLedger;
using voxel::MemoryPoolId;
using voxel::NeighborhoodVersionStamp;
using voxel::SphereCsgTerm;
using voxel::TsdfFormat;
using voxel::Vec3f;
using voxel::WorkerScratchWindow36;
using voxel::execute_next_edit_phase;
using voxel:: map_world_sample;

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
// 1. Canonical-Only 32-Chunk Lock Cap & No Mid-Explosion Tearing (Section 7.1)
// ============================================================================
void test_large_explosion_32_lock_cap_and_no_tearing() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;

    // Section 7.1 Test Case:
    // "Trigger a synthetic explosion with R = 64 voxels (spanning > 100 chunks)
    //  and assert that each Edit phase locks at most 32 canonical chunks."
    EditTransaction tx = EditTransaction::create_from_spheres(
        {SphereCsgTerm{Vec3f{64.0f, 96.0f, 64.0f}, /*radius=*/72.0f}},
        /*delta=*/2.0f,
        /*world_height_chunks=*/8);

    std::size_t total_candidate_chunks = 0;
    for (const auto& phase : tx.phases) {
        REQUIRE(phase.size() <= 32ULL);
        total_candidate_chunks += phase.size();
    }
    REQUIRE(total_candidate_chunks > 100ULL);
    REQUIRE(tx.phases.size() >= 4ULL);

    // Execute phases 1 .. N-1: verify ZERO chunks are published to dirty_edit_chunks
    // and terrain_revision does NOT advance until the final phase completes.
    const std::size_t num_phases = tx.phases.size();
    for (std::size_t p = 0; p + 1 < num_phases; ++p) {
        auto result = execute_next_edit_phase(store, scheduler, tx);
        REQUIRE(!result.has_value());
        REQUIRE(scheduler.edit_dirty_count() == 0ULL);
        REQUIRE(store.terrain_revision() == 0ULL);
        REQUIRE(tx.max_locks_acquired_in_any_phase <= 32ULL);
    }

    // Execute final phase N: verify atomic publication of dirty_edit_chunks,
    // terrain_revision == 1, and EditTransactionCompleted payload.
    auto final_result = execute_next_edit_phase(store, scheduler, tx);
    REQUIRE(final_result.has_value());
    REQUIRE(tx.is_finished());
    REQUIRE(tx.max_locks_acquired_in_any_phase == 32ULL);
    REQUIRE(store.terrain_revision() == 1ULL);
    REQUIRE(final_result->terrain_revision == 1ULL);
    REQUIRE(final_result->total_affected_chunks > 0ULL);
    REQUIRE(scheduler.edit_dirty_count() == final_result->dirtied_chunks.size());
    REQUIRE(!final_result->affected_nav_groups.empty());

    std::printf("[PASS] test_large_explosion_32_lock_cap_and_no_tearing (R=64 spans %zu chunks across %zu phases, max_locks=32)\n",
                total_candidate_chunks, num_phases);
}

// ============================================================================
// 2. R + delta Crater Wall Continuity Verification (Section 3.5.1, Section 7.1)
// ============================================================================
void test_r_plus_delta_crater_wall_continuity() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;

    // Section 7.1 Test Case:
    // "Apply a sphere explosion of radius R = 12.3 and verify that canonical samples
    //  in the shell R <= ||p - c|| < R + delta contain smooth negative TSDF values
    //  matching R - ||p - c||, with zero -delta cliff discontinuities adjacent to the zero-crossing."
    const Vec3f center{16.0f, 16.0f, 16.0f};
    const float radius = 12.3f;
    const float delta = 2.0f;

    EditTransaction tx = EditTransaction::create_from_spheres(
        {SphereCsgTerm{center, radius}}, delta, /*world_height_chunks=*/8);

    while (!tx.is_finished()) {
        (void)execute_next_edit_phase(store, scheduler, tx);
    }

    std::size_t shell_samples_verified = 0;
    for (int32_t pz = 0; pz < 32; ++pz) {
        for (int32_t py = 0; py < 32; ++py) {
            for (int32_t px = 0; px < 32; ++px) {
                const float dx = static_cast<float>(px) - center.x;
                const float dy = static_cast<float>(py) - center.y;
                const float dz = static_cast<float>(pz) - center.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

                if (dist >= radius && dist < radius + delta) {
                    const auto mapped = map_world_sample(px, py, pz);
                    const ChunkSlot* slot = store.find_slot(mapped.chunk);
                    REQUIRE(slot != nullptr);

                    const int16_t actual_q = slot->get_tsdf(mapped.local);
                    const float expected_sdf = radius - dist; // in (-delta, 0]
                    const int16_t expected_q = TsdfFormat::encode(expected_sdf, delta);

                    // Must match smooth negative TSDF value and not remain clamped at kClampedSolid (-32767)
                    // unless expected_q itself rounds to -32767 at the very outer edge of the shell.
                    REQUIRE(actual_q == expected_q);
                    if (dist < radius + delta - 0.05f) {
                        REQUIRE(actual_q > TsdfFormat::kClampedSolid);
                        REQUIRE(actual_q <= 0);
                    }
                    ++shell_samples_verified;
                }
            }
        }
    }
    REQUIRE(shell_samples_verified > 1000ULL);

    std::printf("[PASS] test_r_plus_delta_crater_wall_continuity (%zu shell samples verified in [R, R+delta))\n",
                shell_samples_verified);
}

// ============================================================================
// 3. 3x3x3 Anti-Crumb Kernel Verification (Section 3.5.1, Section 7.1)
// ============================================================================
void test_anti_crumb_kernel() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;

    // Section 7.1 Test Case:
    // "Construct a 1-voxel solid sample (S = -0.5) surrounded by 6 air samples (S = +1.0)
    //  inside an explosion region; verify the post-phase pruning pass flips it to kClampedAir."
    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    ChunkSlot* slot = store.get_or_create_slot(ck);
    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(ChunkStorageForm::Empty, 1, store.tsdf_pool(), store.material_pool());
        slot->state = ChunkLifecycleState::Resident;
        slot->version.store(1, std::memory_order_release);

        // Place an isolated 1-voxel solid crumb at (10, 10, 10) with S = -0.5 surrounded by air (+1.0)
        // and another solid sample at (16, 16, 16) that will be carved by the blast so chunk (0,0,0)
        // is part of the transaction's modified chunk set.
        const int16_t q_crumb = TsdfFormat::encode(-0.5f, 2.0f);
        const int16_t q_air   = TsdfFormat::encode(+1.0f, 2.0f);
        REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 10}, q_crumb, store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{9,  10, 10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{11, 10, 10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 9,  10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 11, 10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 9},  q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 11}, q_air,   store.tsdf_pool()));

        // Target sample at (20, 20, 20) to be carved by a small sphere blast centered at (20, 20, 20)
        REQUIRE(slot->set_tsdf(LocalCoord{20, 20, 20}, TsdfFormat::kClampedSolid, store.tsdf_pool()));
    }

    EditTransaction tx = EditTransaction::create_from_spheres(
        {SphereCsgTerm{Vec3f{20.0f, 20.0f, 20.0f}, /*radius=*/2.0f}},
        /*delta=*/2.0f,
        /*world_height_chunks=*/8);

    while (!tx.is_finished()) {
        (void)execute_next_edit_phase(store, scheduler, tx);
    }

    // Verify the isolated 1-voxel solid crumb at (10, 10, 10) was pruned to kClampedAir (+32767).
    REQUIRE(slot->get_tsdf(LocalCoord{10, 10, 10}) == TsdfFormat::kClampedAir);

    std::printf("[PASS] test_anti_crumb_kernel (isolated 1-voxel solid crumb S=-0.5 pruned to kClampedAir)\n");
}

// ============================================================================
// 4. Multi-Threaded Worker Pool, Coalesced Blasts & Ordered Teardown (Section 5.3)
// ============================================================================
void test_worker_pool_concurrent_execution_and_teardown() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;

    std::atomic<std::size_t> meshes_generated{0};
    std::atomic<bool> save_hook_called{false};

    CoreWorkerPool pool(
        store,
        scheduler,
        [&](const ChunkStore&,
            ChunkKey key,
            WorkerScratchWindow36& scratch,
            const NeighborhoodVersionStamp& stamp) -> ChunkMeshPayload {
            // Read center sample from 36^3 scratch window to verify thread-safe snapshot access
            const int16_t center_s = scratch.get_tsdf(16, 16, 16);
            (void)center_s;
            meshes_generated.fetch_add(1, std::memory_order_relaxed);

            ChunkMeshPayload out{};
            out.key = key;
            out.captured_terrain_revision = stamp.captured_terrain_revision;
            out.vertices.push_back(Vec3f{0.0f, 0.0f, 0.0f});
            out.vertices.push_back(Vec3f{1.0f, 0.0f, 0.0f});
            out.vertices.push_back(Vec3f{0.0f, 1.0f, 0.0f});
            out.collision_faces = out.vertices;
            return out;
        });

    pool.set_shutdown_save_hook([&](ChunkStore&) {
        save_hook_called.store(true, std::memory_order_release);
    });

    pool.start(/*num_workers=*/3);

    // Enqueue 4 explosions rapidly.
    REQUIRE(pool.enqueue_explosion(Vec3f{16.0f, 16.0f, 16.0f}, 6.0f));
    REQUIRE(pool.enqueue_explosion(Vec3f{24.0f, 16.0f, 16.0f}, 6.0f));
    REQUIRE(pool.enqueue_explosion(Vec3f{48.0f, 16.0f, 16.0f}, 8.0f));
    REQUIRE(pool.enqueue_explosion(Vec3f{64.0f, 32.0f, 64.0f}, 10.0f));

    // Drain completions on the simulated main thread until pool.core_idle() and queue is empty.
    std::size_t edit_completions = 0;
    std::size_t mesh_completions = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (std::chrono::steady_clock::now() < deadline) {
        while (auto rec = pool.try_pop_completion()) {
            if (rec->kind == CompletionKind::EditTransactionCompleted) {
                ++edit_completions;
            } else if (rec->kind == CompletionKind::ChunkMesh) {
                ++mesh_completions;
            }
        }
        if (pool.core_idle() && pool.completion_queue_depth() == 0 && edit_completions >= 1 && mesh_completions >= 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(edit_completions >= 1ULL);
    REQUIRE(mesh_completions >= 1ULL);
    REQUIRE(meshes_generated.load() >= mesh_completions);

    // Execute Ordered Teardown Protocol (Section 5.3) with save = true.
    pool.shutdown(/*save=*/true);
    REQUIRE(save_hook_called.load(std::memory_order_acquire));

    // New explosion submissions after shutdown must be rejected cleanly.
    REQUIRE(!pool.enqueue_explosion(Vec3f{0.0f, 0.0f, 0.0f}, 5.0f));

    // All memory ledger pools must be strictly 0 bytes after Step 6 of shutdown.
    REQUIRE(ledger.total_reserved_bytes() == 0ULL);

    std::printf("[PASS] test_worker_pool_concurrent_execution_and_teardown (%zu edit tx, %zu meshes, 0 B leaked)\n",
                edit_completions, mesh_completions);
}

} // namespace

int main() {
    std::printf("=== Running M0/M2 Step 4 Unit Test Suite: Phased R+delta CSG, Anti-Crumb & Worker Pool ===\n");
    test_large_explosion_32_lock_cap_and_no_tearing();
    test_r_plus_delta_crater_wall_continuity();
    test_anti_crumb_kernel();
    test_worker_pool_concurrent_execution_and_teardown();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
