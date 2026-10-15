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
using voxel::ElementalKind;
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
    REQUIRE(!slot->has_damage_array());

    // --- EXPLOSION 1 ---
    {
        EditTransaction tx1 = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/9.5f}}, 2.0f, 8);
        while (!tx1.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx1);
        }
    }
    REQUIRE(slot->has_damage_array());
    REQUIRE(slot->get_damage(boulder_core) == 85U);
    REQUIRE(slot->get_tsdf(boulder_core) < 0);
    {
        const auto stamp = gather_scratch_window_36(store, ck, scratch);
        const auto mesh1 = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(!mesh1.vertices.empty());
        REQUIRE(mesh1.colors.size() == mesh1.vertices.size());
        REQUIRE(std::abs(mesh1.colors.front().x - (85.0f / 255.0f)) < 1e-3f);
        REQUIRE(std::abs(mesh1.colors.front().y - 1.0f) < 1e-3f);
    }

    // --- EXPLOSION 2 ---
    {
        EditTransaction tx2 = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/9.5f}}, 2.0f, 8);
        while (!tx2.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx2);
        }
    }
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
    REQUIRE(slot->get_tsdf(boulder_core) == TsdfFormat::kClampedAir);
    REQUIRE(slot->form == ChunkStorageForm::Empty);
    REQUIRE(!slot->has_damage_array());
    {
        const auto stamp = gather_scratch_window_36(store, ck, scratch);
        const auto mesh3 = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(mesh3.vertices.empty());
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

    const Vec3f blast_pos{16.0f, 16.0f, 16.0f};
    const float blast_r = 6.0f;
    for (int hit = 1; hit <= 3; ++hit) {
        EditTransaction tx = EditTransaction::create_from_spheres(
            {SphereCsgTerm{blast_pos, blast_r}}, 2.0f, 8);
        while (!tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx);
        }
    }

    REQUIRE(slot->get_tsdf(LocalCoord{16, 16, 16}) > 0);
    const int16_t wall_q = slot->get_tsdf(LocalCoord{23, 16, 16});
    REQUIRE(wall_q == TsdfFormat::encode(6.0f - 7.0f, 2.0f));
    REQUIRE(!slot->has_damage_array());

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

    fire(c1, R);
    fire(c2, R);

    REQUIRE(slot->get_tsdf(LocalCoord{14, 15, 16}) < 0);
    REQUIRE(slot->get_damage(LocalCoord{14, 15, 16}) == 170U);
    REQUIRE(slot->get_damage(LocalCoord{17, 15, 16}) == 85U);
    REQUIRE(slot->get_damage(LocalCoord{21, 15, 16}) == 0U);

    fire(c3, R);

    REQUIRE(slot->get_tsdf(LocalCoord{14, 15, 16}) > 0);
    REQUIRE(slot->get_damage(LocalCoord{14, 15, 16}) == 0U);

    REQUIRE(slot->get_tsdf(LocalCoord{17, 15, 16}) < 0);
    REQUIRE(slot->get_damage(LocalCoord{17, 15, 16}) == 170U);

    REQUIRE(slot->get_tsdf(LocalCoord{21, 15, 16}) < 0);
    REQUIRE(slot->get_damage(LocalCoord{21, 15, 16}) == 85U);

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
// 5. Bananza-Style Elemental / Environmental Simulation:
//    - Water Trigger Quenches 1-Hit Lava -> 3-Hit Obsidian
//    - Frost Trigger Flips Air -> Solid Ice
//    - 6-Neighbor Cellular Automata: Lava + Water/Ice -> Obsidian & Melts Ice
// ============================================================================
void test_elemental_simulation_lava_to_obsidian_and_air_to_ice() {
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
            ChunkStorageForm::Empty, MaterialHardness::kMaterialLava, store.tsdf_pool(), store.material_pool());
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

    const LocalCoord core{16, 16, 16};
    REQUIRE(slot->get_material(core) == MaterialHardness::kMaterialLava);
    REQUIRE(!MaterialHardness::supports_persistent_damage(MaterialHardness::kMaterialLava));

    {
        SphereCsgTerm water_term{};
        water_term.center = Vec3f{16.0f, 16.0f, 16.0f};
        water_term.radius = 8.0f;
        water_term.elemental_kind = ElementalKind::Water;

        EditTransaction tx = EditTransaction::create_from_spheres({water_term}, 2.0f, 8);
        while (!tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, tx);
        }
    }
    REQUIRE(slot->get_material(core) == MaterialHardness::kMaterialObsidian);
    REQUIRE(MaterialHardness::supports_persistent_damage(slot->get_material(core)));

    {
        const auto stamp = gather_scratch_window_36(store, ck, scratch);
        const auto mesh_obs = SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(!mesh_obs.vertices.empty());
        REQUIRE(std::abs(mesh_obs.colors.front().y - 1.0f) < 1e-3f);
        REQUIRE(std::abs(mesh_obs.colors.front().z - 0.75f) < 1e-3f);
    }

    {
        EditTransaction hit1 = EditTransaction::create_from_spheres(
            {SphereCsgTerm{Vec3f{16.0f, 16.0f, 16.0f}, /*radius=*/8.0f}}, 2.0f, 8);
        while (!hit1.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, hit1);
        }
    }
    REQUIRE(slot->get_tsdf(core) < 0);
    REQUIRE(slot->get_damage(core) == 85U);

    const LocalCoord air_pt{25, 25, 25};
    REQUIRE(slot->get_tsdf(air_pt) > 0);
    {
        SphereCsgTerm frost_term{};
        frost_term.center = Vec3f{25.0f, 25.0f, 25.0f};
        frost_term.radius = 3.5f;
        frost_term.elemental_kind = ElementalKind::Frost;

        EditTransaction frost_tx = EditTransaction::create_from_spheres({frost_term}, 2.0f, 8);
        while (!frost_tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, frost_tx);
        }
    }
    REQUIRE(slot->get_tsdf(air_pt) < 0);
    REQUIRE(slot->get_material(air_pt) == MaterialHardness::kMaterialIce);

    {
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        REQUIRE(slot->set_tsdf(LocalCoord{5, 5, 5}, TsdfFormat::kClampedSolid, store.tsdf_pool()));
        REQUIRE(slot->set_material(LocalCoord{5, 5, 5}, MaterialHardness::kMaterialLava, store.material_pool()));
        REQUIRE(slot->set_tsdf(LocalCoord{6, 5, 5}, TsdfFormat::kClampedSolid, store.tsdf_pool()));
        REQUIRE(slot->set_material(LocalCoord{6, 5, 5}, MaterialHardness::kMaterialIce, store.material_pool()));
    }
    {
        SphereCsgTerm touch_term{};
        touch_term.center = Vec3f{2.0f, 2.0f, 2.0f};
        touch_term.radius = 1.5f;
        touch_term.elemental_kind = ElementalKind::Water;

        EditTransaction ca_tx = EditTransaction::create_from_spheres({touch_term}, 2.0f, 8);
        while (!ca_tx.is_finished()) {
            (void)execute_next_edit_phase(store, scheduler, ca_tx);
        }
    }
    REQUIRE(slot->get_material(LocalCoord{5, 5, 5}) == MaterialHardness::kMaterialObsidian);
    REQUIRE(slot->get_tsdf(LocalCoord{6, 5, 5}) > 0);

    std::printf("[PASS] test_elemental_simulation_lava_to_obsidian_and_air_to_ice (Lava+Water->3-Hit Obsidian, Air+Frost->Solid Ice, CA Lava+Ice reaction)\n");
}

// ============================================================================
// 6. Multi-Threaded Worker Pool, Coalesced Blasts & Ordered Teardown (Section 5.3)
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
    REQUIRE(pool.enqueue_elemental_sphere(Vec3f{24.0f, 16.0f, 16.0f}, 6.0f, ElementalKind::Water));
    REQUIRE(pool.enqueue_elemental_sphere(Vec3f{48.0f, 16.0f, 16.0f}, 8.0f, ElementalKind::Frost));
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

} // namespace

int main() {
    std::printf("=== Running M0/M2 Step 4 Unit Test Suite: Phased R+delta CSG, Hard-Rock Damage, Elemental Sim & Worker Pool ===\n");
    test_large_explosion_32_lock_cap_and_no_tearing();
    test_r_plus_delta_crater_wall_continuity();
    test_anti_crumb_kernel();
    test_persistent_damage_3_hit_hard_rock();
    test_hard_rock_partial_crater_clears_wall_damage_after_3_hits();
    test_offset_overlapping_clicks_only_carve_3_hit_intersection();
    test_soil_never_accumulates_or_bleeds_cracks_and_fill_resets_damage();
    test_elemental_simulation_lava_to_obsidian_and_air_to_ice();
    test_worker_pool_concurrent_execution_and_teardown();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}