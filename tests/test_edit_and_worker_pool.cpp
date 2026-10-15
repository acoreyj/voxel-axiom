#include "procgen_and_surface_nets.hpp"

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
using voxel::MaterialHardness;
using voxel::MemoryLedger;
using voxel::MemoryPoolId;
using voxel::NeighborhoodVersionStamp;
using voxel::SphereCsgTerm;
using voxel::SurfaceNetsMesher;
using voxel::TsdfFormat;
using voxel::Vec3f;
using voxel::WorkerScratchWindow36;
using voxel::execute_next_edit_phase;
using voxel::gather_scratch_window_36;
using voxel::map_world_sample;

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

    EditTransaction tx = EditTransaction::create_from_spheres(
        {SphereCsgTerm{Vec3f{64.0f, 96.0f, 64.0f}, /*radius=*/64.0f}},
        /*delta=*/2.0f,
        /*world_height_chunks=*/8);

    std::size_t total_candidate_chunks = 0;
    for (const auto& phase : tx.phases) {
        REQUIRE(phase.size() <= 32ULL);
        total_candidate_chunks += phase.size();
    }
    REQUIRE(total_candidate_chunks > 100ULL);
    REQUIRE(tx.phases.size() >= 4ULL);

    const std::size_t num_phases = tx.phases.size();
    for (std::size_t p = 0; p + 1 < num_phases; ++p) {
        auto result = execute_next_edit_phase(store, scheduler, tx);
        REQUIRE(!result.has_value());
        REQUIRE(scheduler.edit_dirty_count() == 0ULL);
        REQUIRE(store.terrain_revision() == 0ULL);
        REQUIRE(tx.max_locks_acquired_in_any_phase <= 32ULL);
    }

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
                    const float expected_sdf = radius - dist;
                    const int16_t expected_q = TsdfFormat::encode(expected_sdf, delta);

                    REQUIRE(actual_q == expected_q);
                    // Verify the (-delta, 0) crater wall skin in kMaterialSoil retains ZERO crack damage!
                    REQUIRE(slot->get_damage(mapped.local) == 0U);
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

    std::printf("[PASS] test_r_plus_delta_crater_wall_continuity (%zu shell samples verified in [R, R+delta) with 0 soil damage)\n",
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

    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    ChunkSlot* slot = store.get_or_create_slot(ck);
    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(ChunkStorageForm::Empty, 1, store.tsdf_pool(), store.material_pool());
        slot->state = ChunkLifecycleState::Resident;
        slot->version.store(1, std::memory_order_release);

        const int16_t q_crumb = TsdfFormat::encode(-0.5f, 2.0f);
        const int16_t q_air   = TsdfFormat::encode(+1.0f, 2.0f);
        REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 10}, q_crumb, store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{9,  10, 10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{11, 10, 10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 9,  10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 11, 10}, q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 9},  q_air,   store.tsdf_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{10, 10, 11}, q_air,   store.tsdf_pool()));

        REQUIRE(slot->set_tsdf(LocalCoord{20, 20, 20}, TsdfFormat::kClampedSolid, store.tsdf_pool()));
    }

    EditTransaction tx = EditTransaction::create_from_spheres(
        {SphereCsgTerm{Vec3f{20.0f, 20.0f, 20.0f}, /*radius=*/2.0f}},
        /*delta=*/2.0f,
        /*world_height_chunks=*/8);

    while (!tx.is_finished()) {
        (void)execute_next_edit_phase(store, scheduler, tx);
    }

    REQUIRE(slot->get_tsdf(LocalCoord{10, 10, 10}) == TsdfFormat::kClampedAir);

    std::printf("[PASS] test_anti_crumb_kernel (isolated 1-voxel solid crumb S=-0.5 pruned to kClampedAir)\n");
}

// ============================================================================
// 4. Persistent Damage Thresholds: 3-Hit Hard Rock Boulder (Bananza-Style)
// ============================================================================
void test_persistent_damage_3_hit_hard_rock() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;
    WorkerScratchWindow36 scratch(ledger);

    // Create a chunk containing a Hard Rock (kMaterialHardRock = 2) boulder of radius 5.0
    // centered at (16, 16, 16).
    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    ChunkSlot* slot = store.get_or_create_slot(ck);
    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(
            ChunkStorageForm::Empty, MaterialHardness::kMaterialHardRock, store.tsdf_pool(), store.material_pool());
        slot->state = ChunkLifecycleState::Resident;
        slot->version.store(1, std::memory_order_release);

        for (int32_t z = 0; z < 32; ++z) {
            for (int32_t y = 0; y < 32; ++y) {
                for (int32_t x = 0; x < 32; ++x) {
                    const float dx = static_cast<float>(x) - 16.0f;
                    const float dy = static_cast<float>(y) - 16.0f;
                    const float dz = static_cast<float>(z) - 16.0f;
                    const float sdf = std::sqrt(dx * dx + dy * dy + dz * dz) - 5.0f;
                    const int16_t q = TsdfFormat::encode(std::clamp(sdf, -2.0f, +2.0f), 2.0f);
                    REQUIRE(slot->set_tsdf(LocalCoord{x, y, z}, q, store.tsdf_pool()));
                }
            }
        }
    }

    const LocalCoord boulder_core{16, 16, 16};
    REQUIRE(slot->get_tsdf(boulder_core) < 0);
    REQUIRE(slot->get_damage(boulder_core) == 0U);
    REQUIRE(!slot->has_damage_array()); // 0 bytes allocated before damage

    // --- EXPLOSION 1 (radius = 9.5 covers the 5.0 boulder + both 2.0 delta bands) ---
    {
        EditTransaction tx1 = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/9.5f}}, 2.0f, 8);
        while (!tx1.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx1);
        }
    }
    // After Hit 1: Hard Rock must STILL be solid (TSDF < 0), damage == 85, and mesh shows Stage 1 cracks!
    REQUIRE(slot->has_damage_array());
    REQUIRE(slot->get_damage(boulder_core) == 85U);
    REQUIRE(slot->get_tsdf(boulder_core) < 0);
    {
        const auto stamp = gather_scratch_window_36(store, ck, scratch);
        const auto mesh1 = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(!mesh1.vertices.empty());
        REQUIRE(mesh1.colors.size() == mesh1.vertices.size());
        // Verify vertices carry damage_ratio = 85 / 255 (~0.333), is_hard_rock = 1.0
        // and hits_to_break = 3 / 255 for the shader's hits-remaining damage ramp.
        REQUIRE(std::abs(mesh1.colors.front().x - (85.0f / 255.0f)) < 1e-3f);
        REQUIRE(std::abs(mesh1.colors.front().y - 1.0f) < 1e-3f);
        REQUIRE(std::abs(mesh1.colors.front().z - (3.0f / 255.0f)) < 1e-6f);
    }

    // --- EXPLOSION 2 ---
    {
        EditTransaction tx2 = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/9.5f}}, 2.0f, 8);
        while (!tx2.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx2);
        }
    }
    // After Hit 2: Hard Rock must STILL be solid (TSDF < 0), damage == 170, and mesh shows Stage 2 cracks!
    REQUIRE(slot->get_damage(boulder_core) == 170U);
    REQUIRE(slot->get_tsdf(boulder_core) < 0);
    {
        const auto stamp = gather_scratch_window_36(store, ck, scratch);
        const auto mesh2 = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(!mesh2.vertices.empty());
        REQUIRE(std::abs(mesh2.colors.front().x - (170.0f / 255.0f)) < 1e-3f);
    }

    // --- EXPLOSION 3 ---
    {
        EditTransaction tx3 = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/9.5f}}, 2.0f, 8);
        while (!tx3.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx3);
        }
    }
    // After Hit 3: Cumulative damage reaches 255 >= 255 -> Hard Rock boulder SHATTERS to air!
    REQUIRE(slot->get_tsdf(boulder_core) == TsdfFormat::kClampedAir);
    REQUIRE(slot->form == ChunkStorageForm::Empty); // Auto-demoted to 0-byte Empty chunk!
    REQUIRE(!slot->has_damage_array());             // Damage slab automatically returned to pool!
    {
        const auto stamp = gather_scratch_window_36(store, ck, scratch);
        const auto mesh3 = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(mesh3.vertices.empty()); // Boulder completely destroyed (0 triangles left)
    }

    std::printf("[PASS] test_persistent_damage_3_hit_hard_rock (Hit 1: dmg=85 solid; Hit 2: dmg=170 solid; Hit 3: shattered!)\n");
}

// ============================================================================
// 4b. Regression Test: Hitting Partial Crater in Hard Rock 3x at Exact Same Spot
//     Leaves 0 Residual Damage on the Newly Carved Concave Crater Wall/Rim
// ============================================================================
void test_hard_rock_partial_crater_clears_wall_damage_after_3_hits() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;
    WorkerScratchWindow36 scratch(ledger);

    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    ChunkSlot* slot = store.get_or_create_slot(ck);
    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(
            ChunkStorageForm::Solid, MaterialHardness::kMaterialHardRock, store.tsdf_pool(), store.material_pool());
        slot->state = ChunkLifecycleState::Resident;
        slot->version.store(1, std::memory_order_release);
    }

    // Fire 3 explosions of radius 6.0 at the EXACT same spot (16, 16, 16) inside solid Hard Rock
    const Vec3f blast_pos{16.0f, 16.0f, 16.0f};
    const float blast_r = 6.0f;
    for (int hit = 1; hit <= 3; ++hit) {
        EditTransaction tx = EditTransaction::create_from_spheres(
            {SphereCsgTerm{blast_pos, blast_r}}, 2.0f, 8);
        while (!tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx);
        }
    }

    // After Hit 3 at the exact same spot:
    // 1. Center (16, 16, 16) is carved to air (q > 0)
    REQUIRE(slot->get_tsdf(LocalCoord{16, 16, 16}) > 0);
    // 2. Concave crater wall in [R, R+delta) (e.g. x=23, dist=7.0 in [6.0, 8.0)) has smooth negative TSDF
    //    AND 0 residual damage (damage slab is completely freed!)
    const int16_t wall_q = slot->get_tsdf(LocalCoord{23, 16, 16});
    REQUIRE(wall_q == TsdfFormat::encode(6.0f - 7.0f, 2.0f));
    REQUIRE(!slot->has_damage_array());

    // 3. Every vertex on the newly carved concave hard-rock crater wall has 0.0 damage_ratio!
    const auto stamp = gather_scratch_window_36(store, ck, scratch);
    const auto mesh = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
    REQUIRE(!mesh.vertices.empty());
    for (const Vec3f& col : mesh.colors) {
        REQUIRE(col.x == 0.0f);
        REQUIRE(col.y == 1.0f);
    }

    std::printf("[PASS] test_hard_rock_partial_crater_clears_wall_damage_after_3_hits (0 residual cracks on crater bowl)\n");
}

// ============================================================================
// 4c. Regression Test: 3 Offset Clicks with Partial Overlap ONLY Carve the 3-Hit
//     Intersection (Never Blow Up the 1-Hit or 2-Hit Areas of the 3rd Click!)
// ============================================================================
void test_offset_overlapping_clicks_only_carve_3_hit_intersection() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;

    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    ChunkSlot* slot = store.get_or_create_slot(ck);
    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(
            ChunkStorageForm::Solid, MaterialHardness::kMaterialHardRock, store.tsdf_pool(), store.material_pool());
        slot->state = ChunkLifecycleState::Resident;
        slot->version.store(1, std::memory_order_release);

        // Flat hard-rock surface at y = 16
        for (int32_t z = 0; z < 32; ++z) {
            for (int32_t y = 0; y < 32; ++y) {
                for (int32_t x = 0; x < 32; ++x) {
                    const float sdf = static_cast<float>(y) - 16.2f;
                    const int16_t q = TsdfFormat::encode(std::clamp(sdf, -2.0f, +2.0f), 2.0f);
                    REQUIRE(slot->set_tsdf(LocalCoord{x, y, z}, q, store.tsdf_pool()));
                }
            }
        }
    }

    auto fire = [&](Vec3f c, float r) {
        EditTransaction tx = EditTransaction::create_from_spheres({SphereCsgTerm{c, r}}, 2.0f, 8);
        while (!tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx);
        }
    };

    const float R = 5.0f;
    const Vec3f c1{10.0f, 16.0f, 16.0f};
    const Vec3f c2{14.0f, 16.0f, 16.0f};
    const Vec3f c3{18.0f, 16.0f, 16.0f};

    // Click 1 at x=10 and Click 2 at x=14 create a 2-hit overlap in x=[9..15]
    fire(c1, R);
    fire(c2, R);

    // Verify no voxels broke after 2 clicks, and x=13..15 has 2 hits (170) while x=17 has 1 hit (85)
    REQUIRE(slot->get_tsdf(LocalCoord{14, 15, 16}) < 0);
    REQUIRE(slot->get_damage(LocalCoord{14, 15, 16}) == 170U);
    REQUIRE(slot->get_damage(LocalCoord{17, 15, 16}) == 85U);
    REQUIRE(slot->get_damage(LocalCoord{21, 15, 16}) == 0U);

    // Click 3 at x=18 (radius 5.0 spans x=[13..23]):
    // - x=14, y=15, z=16 is inside c1 (dist=4.12<=5), c2 (dist=1.0<=5), AND c3 (dist=4.12<=5) -> 3 hits -> MUST CARVE!
    // - x=17, y=15, z=16 is inside c2 and c3 ONLY (2 hits total) -> MUST NOT CARVE (stays solid with damage=170)!
    // - x=21, y=15, z=16 is inside c3 ONLY (1 hit total) -> MUST NOT CARVE (stays solid with damage=85)!
    fire(c3, R);

    // 1. 3-hit intersection voxel (14, 15, 16) carved to air and reset damage to 0
    REQUIRE(slot->get_tsdf(LocalCoord{14, 15, 16}) > 0);
    REQUIRE(slot->get_damage(LocalCoord{14, 15, 16}) == 0U);

    // 2. 2-hit voxel (17, 15, 16) inside Click 3's sphere stayed SOLID and advanced from 85 -> 170
    REQUIRE(slot->get_tsdf(LocalCoord{17, 15, 16}) < 0);
    REQUIRE(slot->get_damage(LocalCoord{17, 15, 16}) == 170U);

    // 3. 1-hit voxel (21, 15, 16) inside Click 3's sphere stayed SOLID and advanced from 0 -> 85
    REQUIRE(slot->get_tsdf(LocalCoord{21, 15, 16}) < 0);
    REQUIRE(slot->get_damage(LocalCoord{21, 15, 16}) == 85U);

    // 4. Exhaustive check across the entire chunk: NO voxel with < 3 hits was carved to air!
    std::size_t carved_count = 0;
    for (int32_t z = 0; z < 32; ++z) {
        for (int32_t y = 0; y <= 16; ++y) {
            for (int32_t x = 0; x < 32; ++x) {
                const auto in_sphere = [&](Vec3f c) {
                    const float dx = static_cast<float>(x) - c.x;
                    const float dy = static_cast<float>(y) - c.y;
                    const float dz = static_cast<float>(z) - c.z;
                    return (dx * dx + dy * dy + dz * dz) <= (R * R);
                };
                const int hits = (in_sphere(c1) ? 1 : 0) + (in_sphere(c2) ? 1 : 0) + (in_sphere(c3) ? 1 : 0);
                const int16_t q = slot->get_tsdf(LocalCoord{x, y, z});
                if (hits == 3) {
                    REQUIRE(q > 0);
                    ++carved_count;
                } else {
                    REQUIRE(q < 0);
                }
            }
        }
    }
    REQUIRE(carved_count > 10ULL);

    std::printf("[PASS] test_offset_overlapping_clicks_only_carve_3_hit_intersection (%zu 3-hit voxels carved; 0 1-hit/2-hit voxels carved)\n",
                carved_count);
}

// ============================================================================
// 4d. Regression Test: Soil Never Accumulates Cracks, Boundary Never Bleeds,
//     and Fill Path Resets Old Damage
// ============================================================================
void test_soil_never_accumulates_or_bleeds_cracks_and_fill_resets_damage() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;
    WorkerScratchWindow36 scratch(ledger);

    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    ChunkSlot* slot = store.get_or_create_slot(ck);
    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(
            ChunkStorageForm::Solid, MaterialHardness::kMaterialSoil, store.tsdf_pool(), store.material_pool());
        slot->state = ChunkLifecycleState::Resident;
        slot->version.store(1, std::memory_order_release);

        for (int32_t z = 0; z < 32; ++z) {
            for (int32_t y = 0; y < 32; ++y) {
                for (int32_t x = 0; x < 32; ++x) {
                    const float sdf = static_cast<float>(y) - 16.2f;
                    const int16_t q = TsdfFormat::encode(std::clamp(sdf, -2.0f, +2.0f), 2.0f);
                    REQUIRE(slot->set_tsdf(LocalCoord{x, y, z}, q, store.tsdf_pool()));
                    if (x >= 16 && y <= 16) {
                        REQUIRE(slot->set_material(
                            LocalCoord{x, y, z}, MaterialHardness::kMaterialHardRock, store.material_pool()));
                    }
                }
            }
        }
    }

    {
        EditTransaction tx = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/6.0f}}, 2.0f, 8);
        while (!tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx);
        }
    }

    for (int32_t z = 0; z < 32; ++z) {
        for (int32_t y = 0; y < 32; ++y) {
            for (int32_t x = 0; x < 16; ++x) {
                const LocalCoord lc{x, y, z};
                REQUIRE(slot->get_material(lc) == MaterialHardness::kMaterialSoil);
                REQUIRE(slot->get_damage(lc) == 0U);
            }
        }
    }

    const auto stamp = gather_scratch_window_36(store, ck, scratch);
    const auto mesh = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
    REQUIRE(!mesh.vertices.empty());
    for (const Vec3f& col : mesh.colors) {
        if (col.y < 0.5f) {
            REQUIRE(col.x == 0.0f);
        }
    }

    {
        SphereCsgTerm fill_term{};
        fill_term.center = Vec3f{18.0f, 15.0f, 16.0f};
        fill_term.radius = 4.0f;
        fill_term.is_fill = true;
        fill_term.fill_material = MaterialHardness::kMaterialSoil;

        EditTransaction fill_tx = EditTransaction::create_from_spheres({fill_term}, 2.0f, 8);
        while (!fill_tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, fill_tx);
        }
    }
    REQUIRE(slot->get_material(LocalCoord{18, 15, 16}) == MaterialHardness::kMaterialSoil);
    REQUIRE(slot->get_damage(LocalCoord{18, 15, 16}) == 0U);

    std::printf("[PASS] test_soil_never_accumulates_or_bleeds_cracks_and_fill_resets_damage\n");
}

// ============================================================================
// 4e. Add-Terrain (Fill) CSG Union Semantics
// ============================================================================
void test_fill_add_terrain_union_semantics() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;

    const Vec3f center{16.0f, 16.0f, 16.0f};
    const float radius = 10.0f;
    const float delta = 2.0f;

    SphereCsgTerm fill_term{};
    fill_term.center = center;
    fill_term.radius = radius;
    fill_term.is_fill = true;

    EditTransaction tx = EditTransaction::create_from_spheres({fill_term}, delta, /*world_height_chunks=*/8);

    while (!tx.is_finished()) {
        (void)execute_next_edit_phase(store, scheduler, tx);
    }

    const ChunkSlot* slot = store.find_slot(ChunkKey::from_coords(0, 0, 0));
    REQUIRE(slot != nullptr);

    // Deep inside the added sphere the sample must be solid (fill, not carve).
    REQUIRE(slot->get_tsdf(LocalCoord{16, 16, 16}) == TsdfFormat::kClampedSolid);
    REQUIRE(slot->get_tsdf(LocalCoord{12, 16, 16}) < 0);

    // Air-side shell in [R, R+delta): smooth positive TSDF matching ||p-c|| - R.
    std::size_t shell_samples_verified = 0;
    for (int32_t pz = 0; pz < 32; ++pz) {
        for (int32_t py = 0; py < 32; ++py) {
            for (int32_t px = 0; px < 32; ++px) {
                const float dx = static_cast<float>(px) - center.x;
                const float dy = static_cast<float>(py) - center.y;
                const float dz = static_cast<float>(pz) - center.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

                if (dist >= radius && dist < radius + delta) {
                    const int16_t actual_q = slot->get_tsdf(LocalCoord{px, py, pz});
                    const int16_t expected_q = TsdfFormat::encode(dist - radius, delta);
                    REQUIRE(actual_q == expected_q);
                    ++shell_samples_verified;
                } else if (dist >= radius + delta + 1.0f) {
                    // Beyond the influence window the chunk must remain untouched air.
                    REQUIRE(slot->get_tsdf(LocalCoord{px, py, pz}) == TsdfFormat::kClampedAir);
                }
            }
        }
    }
    REQUIRE(shell_samples_verified > 500ULL);

    // Adding inside deep solid material is a no-op: no revision bump, no dirty chunks.
    MemoryLedger solid_ledger;
    ChunkStore solid_store(solid_ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler solid_scheduler;
    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    ChunkSlot* solid_slot = solid_store.get_or_create_slot(ck);
    {
        std::unique_lock<std::shared_mutex> lock(solid_slot->mutex);
        solid_slot->reset_to_uniform(ChunkStorageForm::Solid, 1, solid_store.tsdf_pool(),
                                     solid_store.material_pool());
        solid_slot->state = ChunkLifecycleState::Resident;
        solid_slot->version.store(1, std::memory_order_release);
    }

    SphereCsgTerm noop_term{};
    noop_term.center = Vec3f{16.0f, 16.0f, 16.0f};
    noop_term.radius = 4.0f;
    noop_term.is_fill = true;

    EditTransaction noop_tx = EditTransaction::create_from_spheres({noop_term}, delta, /*world_height_chunks=*/8);
    while (!noop_tx.is_finished()) {
        (void)execute_next_edit_phase(solid_store, solid_scheduler, noop_tx);
    }
    REQUIRE(solid_store.terrain_revision() == 0ULL);
    REQUIRE(solid_scheduler.edit_dirty_count() == 0ULL);

    std::printf("[PASS] test_fill_add_terrain_union_semantics (%zu shell samples verified, no-op inside solid)\n",
                shell_samples_verified);
}

// ============================================================================
// 4f. Fill Transactions Tag the Selected Material (rock add / boulder placement)
// ============================================================================
void test_fill_tags_selected_material() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;
    WorkerScratchWindow36 scratch(ledger);

    SphereCsgTerm fill_term{};
    fill_term.center = Vec3f{16.0f, 16.0f, 16.0f};
    fill_term.radius = 10.0f;
    fill_term.is_fill = true;
    fill_term.fill_material = MaterialHardness::kMaterialHardRock;

    EditTransaction tx = EditTransaction::create_from_spheres({fill_term}, 2.0f, /*world_height_chunks=*/8);
    while (!tx.is_finished()) {
        (void)execute_next_edit_phase(store, scheduler, tx);
    }

    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    const ChunkSlot* slot = store.find_slot(ck);
    REQUIRE(slot != nullptr);

    // Solid samples created by the fill carry the selected material.
    REQUIRE(slot->get_tsdf(LocalCoord{16, 16, 16}) < 0);
    REQUIRE(slot->get_material(LocalCoord{16, 16, 16}) == MaterialHardness::kMaterialHardRock);
    REQUIRE(slot->get_material(LocalCoord{12, 16, 16}) == MaterialHardness::kMaterialHardRock);

    // Air outside the filled sphere is not tagged as rock.
    REQUIRE(slot->get_tsdf(LocalCoord{2, 2, 2}) == TsdfFormat::kClampedAir);
    REQUIRE(slot->get_material(LocalCoord{2, 2, 2}) == MaterialHardness::kMaterialSoil);

    // The mesher flags filled rock in vertex COLOR.g so the shader tints it.
    const auto stamp = gather_scratch_window_36(store, ck, scratch);
    const auto mesh = SurfaceNetsMesher::mesh_chunk_from_scratch_36(
        ck, scratch, 1, stamp.captured_terrain_revision);
    REQUIRE(!mesh.vertices.empty());
    REQUIRE(mesh.colors.size() == mesh.vertices.size());
    REQUIRE(mesh.colors.front().y == 1.0f);
    // Filled hard rock packs hits_to_break = 3 / 255 into COLOR.b.
    REQUIRE(std::abs(mesh.colors.front().z - (3.0f / 255.0f)) < 1e-6f);

    std::printf("[PASS] test_fill_tags_selected_material (filled rock tagged + mesher COLOR.g)\n");
}

// ============================================================================
// 4g. Persistent Damage: 5-Hit Boulder Placement
// ============================================================================
void test_persistent_damage_5_hit_boulder() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;
    WorkerScratchWindow36 scratch(ledger);

    // Place a boulder: a radius-5 sphere of kMaterialBoulder centred at (16, 16, 16).
    {
        SphereCsgTerm boulder_term{};
        boulder_term.center = Vec3f{16.0f, 16.0f, 16.0f};
        boulder_term.radius = 5.0f;
        boulder_term.is_fill = true;
        boulder_term.fill_material = MaterialHardness::kMaterialBoulder;

        EditTransaction place = EditTransaction::create_from_spheres({boulder_term}, 2.0f, /*world_height_chunks=*/8);
        while (!place.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, place);
        }
    }

    ChunkSlot* slot = store.find_slot(ChunkKey::from_coords(0, 0, 0));
    REQUIRE(slot != nullptr);
    const LocalCoord core{16, 16, 16};
    REQUIRE(slot->get_tsdf(core) < 0);
    REQUIRE(slot->get_material(core) == MaterialHardness::kMaterialBoulder);

    // Four explosions only crack it: 51, 102, 153, 204 (< 255).
    for (uint8_t expected_dmg = 51U; expected_dmg <= 204U; expected_dmg += 51U) {
        EditTransaction hit = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/9.5f}}, 2.0f, 8);
        while (!hit.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, hit);
        }
        REQUIRE(slot->get_damage(core) == expected_dmg);
        REQUIRE(slot->get_tsdf(core) < 0);
    }
    REQUIRE(slot->has_damage_array());

    // The pre-shatter state (204 / 255) packs damage_ratio 204/255 and hits_to_break 5/255,
    // so the shader renders it in the same bright-red band as the 3-hit rock's final state.
    {
        const auto stamp = gather_scratch_window_36(store, ChunkKey::from_coords(0, 0, 0), scratch);
        const auto mesh = SurfaceNetsMesher::mesh_chunk_from_scratch_36(
            ChunkKey::from_coords(0, 0, 0), scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(!mesh.vertices.empty());
        REQUIRE(!mesh.colors.empty());
        REQUIRE(std::abs(mesh.colors.front().x - (204.0f / 255.0f)) < 1e-3f);
        REQUIRE(std::abs(mesh.colors.front().z - (5.0f / 255.0f)) < 1e-6f);
    }

    // Fifth explosion reaches 255 >= threshold -> the boulder shatters to air.
    {
        EditTransaction hit = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/9.5f}}, 2.0f, 8);
        while (!hit.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, hit);
        }
    }
    REQUIRE(slot->get_tsdf(core) == TsdfFormat::kClampedAir);
    REQUIRE(slot->form == ChunkStorageForm::Empty);
    REQUIRE(!slot->has_damage_array());

    std::printf("[PASS] test_persistent_damage_5_hit_boulder (51x4 cracks, 5th shatters)\n");
}

// ============================================================================
// 5. Multi-Threaded Worker Pool, Coalesced Blasts & Ordered Teardown (Section 5.3)
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

    REQUIRE(pool.enqueue_explosion(Vec3f{16.0f, 16.0f, 16.0f}, 6.0f));
    REQUIRE(pool.enqueue_explosion(Vec3f{24.0f, 16.0f, 16.0f}, 6.0f));
    REQUIRE(pool.enqueue_explosion(Vec3f{48.0f, 16.0f, 16.0f}, 8.0f));
    REQUIRE(pool.enqueue_explosion(Vec3f{64.0f, 32.0f, 64.0f}, 10.0f));

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

    pool.shutdown(/*save=*/true);
    REQUIRE(save_hook_called.load(std::memory_order_acquire));
    REQUIRE(!pool.enqueue_explosion(Vec3f{0.0f, 0.0f, 0.0f}, 5.0f));
    REQUIRE(ledger.total_reserved_bytes() == 0ULL);

    std::printf("[PASS] test_worker_pool_concurrent_execution_and_teardown (%zu edit tx, %zu meshes, 0 B leaked)\n",
                edit_completions, mesh_completions);
}

// ============================================================================
// 6. Fill and Carve Transactions Stay Distinct Through the Worker Pool
// ============================================================================
void test_fill_and_carve_do_not_coalesce() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    DualDirtyScheduler scheduler;

    CoreWorkerPool pool(store, scheduler);
    pool.start(/*num_workers=*/2);

    REQUIRE(pool.enqueue_explosion(Vec3f{16.0f, 16.0f, 16.0f}, 4.0f));
    REQUIRE(pool.enqueue_fill_spheres(Vec3f{48.0f, 16.0f, 16.0f}, 4.0f));

    std::size_t carve_completions = 0;
    std::size_t fill_completions = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (std::chrono::steady_clock::now() < deadline) {
        while (auto rec = pool.try_pop_completion()) {
            if (rec->kind == CompletionKind::EditTransactionCompleted) {
                if (rec->edit_transaction.is_fill) {
                    ++fill_completions;
                    REQUIRE(rec->edit_transaction.radius == 4.0f);
                    REQUIRE(rec->edit_transaction.position.x == 48.0f);
                } else {
                    ++carve_completions;
                }
            }
        }
        if (pool.core_idle() && pool.completion_queue_depth() == 0 &&
            carve_completions >= 1ULL && fill_completions >= 1ULL) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(carve_completions == 1ULL);
    REQUIRE(fill_completions == 1ULL);

    pool.shutdown(/*save=*/false);
    REQUIRE(ledger.total_reserved_bytes() == 0ULL);

    std::printf("[PASS] test_fill_and_carve_do_not_coalesce (1 carve + 1 fill transaction, 0 B leaked)\n");
}

} // namespace

int main() {
    std::printf("=== Running M0/M2 Step 4 Unit Test Suite: Phased R+delta CSG, Hard-Rock Damage & Worker Pool ===\n");
    test_large_explosion_32_lock_cap_and_no_tearing();
    test_r_plus_delta_crater_wall_continuity();
    test_anti_crumb_kernel();
    test_persistent_damage_3_hit_hard_rock();
    test_hard_rock_partial_crater_clears_wall_damage_after_3_hits();
    test_offset_overlapping_clicks_only_carve_3_hit_intersection();
    test_soil_never_accumulates_or_bleeds_cracks_and_fill_resets_damage();
    test_fill_add_terrain_union_semantics();
    test_fill_tags_selected_material();
    test_persistent_damage_5_hit_boulder();
    test_worker_pool_concurrent_execution_and_teardown();
    test_fill_and_carve_do_not_coalesce();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}