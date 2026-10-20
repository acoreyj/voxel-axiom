#include "voxel_mesh_baker.hpp"
#include "asset_stamp_manager.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

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
// 1. OBJ Mesh Loader & Point-Triangle Closest Point Math Test
// ============================================================================
void test_obj_loader_and_distance_math() {
    ++g_tests_run;

    const voxel::Vec3f a{0.0f, 0.0f, 0.0f};
    const voxel::Vec3f b{10.0f, 0.0f, 0.0f};
    const voxel::Vec3f c{0.0f, 10.0f, 0.0f};

    float bu = 0.0f, bv = 0.0f, bw = 0.0f;
    const voxel::Vec3f cp_face = voxel::point_triangle_closest_point_barycentric(
        voxel::Vec3f{2.0f, 2.0f, 5.0f}, a, b, c, bu, bv, bw);
    REQUIRE(std::abs(cp_face.x - 2.0f) < 1e-4f);
    REQUIRE(std::abs(cp_face.y - 2.0f) < 1e-4f);
    REQUIRE(std::abs(cp_face.z - 0.0f) < 1e-4f);
    REQUIRE(std::abs((bu + bv + bw) - 1.0f) < 1e-4f);
    REQUIRE(std::abs(bv - 0.2f) < 1e-4f);
    REQUIRE(std::abs(bw - 0.2f) < 1e-4f);

    const voxel::Vec3f cp_a = voxel::point_triangle_closest_point(voxel::Vec3f{-3.0f, -4.0f, 0.0f}, a, b, c);
    REQUIRE(std::abs(cp_a.x - 0.0f) < 1e-4f);
    REQUIRE(std::abs(cp_a.y - 0.0f) < 1e-4f);

    std::string test_obj =
        "# Sample ruin pillar\n"
        "usemtl stone_hard_rock\n"
        "v 0.0 0.0 0.0\n"
        "v 4.0 0.0 0.0\n"
        "v 4.0 8.0 0.0\n"
        "v 0.0 8.0 0.0\n"
        "f 1 2 3 4\n"
        "usemtl dirt_soil\n"
        "v 4.0 0.0 4.0\n"
        "v 4.0 8.0 4.0\n"
        "f 2 5 6 3\n";

    std::stringstream ss(test_obj);
    voxel::RawObjMesh mesh{};
    REQUIRE(voxel::ObjMeshLoader::load_from_stream(ss, mesh));
    REQUIRE(mesh.vertices.size() == 6ULL);
    REQUIRE(mesh.triangles.size() == 4ULL);
    REQUIRE(mesh.triangles[0].material_id == voxel::MaterialHardness::kMaterialHardRock);
    REQUIRE(mesh.triangles[1].material_id == voxel::MaterialHardness::kMaterialHardRock);
    REQUIRE(mesh.triangles[2].material_id == voxel::MaterialHardness::kMaterialSoil);
    REQUIRE(mesh.triangles[3].material_id == voxel::MaterialHardness::kMaterialSoil);

    REQUIRE(mesh.aabb_min.x == 0.0f);
    REQUIRE(mesh.aabb_max.x == 4.0f);
    REQUIRE(mesh.aabb_min.y == 0.0f);
    REQUIRE(mesh.aabb_max.y == 8.0f);

    std::printf("[PASS] test_obj_loader_and_distance_math (barycentric Voronoi + OBJ quads & materials)\n");
}

// ============================================================================
// 2. Offline Mesh Voxelization to 8x8x8 Region (.vreg) Test
// ============================================================================
void test_mesh_voxelization_to_vreg_region() {
    ++g_tests_run;

    const std::filesystem::path temp_vreg = "/tmp/test_ruin_temple.vreg";
    std::filesystem::remove(temp_vreg);

    // Build a hand-crafted ancient ruin shrine mesh (box shrine with walls)
    std::string shrine_obj =
        "v -10.0 0.0 -10.0\n"
        "v  10.0 0.0 -10.0\n"
        "v  10.0 0.0  10.0\n"
        "v -10.0 0.0  10.0\n"
        "v -10.0 12.0 -10.0\n"
        "v  10.0 12.0 -10.0\n"
        "v  10.0 12.0  10.0\n"
        "v -10.0 12.0  10.0\n"
        "usemtl ancient_stone\n"
        "# Floor\n"
        "f 1 2 3 4\n"
        "# Wall 1\n"
        "f 1 5 6 2\n"
        "# Wall 2\n"
        "f 2 6 7 3\n"
        "# Wall 3\n"
        "f 3 7 8 4\n"
        "# Roof\n"
        "f 5 6 7 8\n";

    std::stringstream ss(shrine_obj);
    voxel::RawObjMesh mesh{};
    REQUIRE(voxel::ObjMeshLoader::load_from_stream(ss, mesh));

    voxel::AssetBakerConfig cfg{};
    cfg.auto_center_in_region = true; // Centers at (128, 128, 128)
    cfg.shell_thickness = 2.0f;
    cfg.default_material = voxel::MaterialHardness::kMaterialHardRock;
    cfg.region_coords = voxel::RegionKey{0, 0, 0};

    const voxel::BakedRegionData baked = voxel::VoxelMeshBaker::bake_mesh_to_region(mesh, cfg);

    REQUIRE(baked.header.version == 4);
    REQUIRE(baked.header.magic[0] == 'V');
    REQUIRE(baked.header.magic[1] == 'R');
    REQUIRE(baked.header.magic[2] == 'E');
    REQUIRE(baked.header.magic[3] == 'G');
    REQUIRE(baked.active_chunk_count > 0);
    REQUIRE(baked.dense_chunk_count > 0);
    REQUIRE(baked.empty_chunk_count > 0);
    REQUIRE(baked.payload.size() > 0);
    REQUIRE(baked.header.payload_checksum != 0);

    REQUIRE(voxel::VoxelMeshBaker::save_baked_region_to_file(temp_vreg.string(), baked));
    REQUIRE(std::filesystem::exists(temp_vreg));
    REQUIRE(std::filesystem::file_size(temp_vreg) > sizeof(voxel::RegionFileHeader));

    std::printf("[PASS] test_mesh_voxelization_to_vreg_region (baked %zu active chunks into .vreg with valid FNV-1a checksum)\n",
                baked.active_chunk_count);
}

// ============================================================================
// 3. Asset Stamping, Destruction Physics & Automated Reconstitution Test
// ============================================================================
void test_stamping_destruction_and_reconstitution() {
    ++g_tests_run;

    const std::filesystem::path temp_vreg = "/tmp/test_ruin_temple.vreg";
    REQUIRE(std::filesystem::exists(temp_vreg));

    voxel::MemoryLedger ledger;
    voxel::ChunkStore store(ledger, /*world_height_chunks=*/8);
    voxel::DualDirtyScheduler dirty_scheduler;
    voxel::ServerSlotManager slot_mgr;
    slot_mgr.initialize(voxel::ServerRid{1}, voxel::ServerRid{2});
    voxel::VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(voxel::ServerRid{3});

    voxel::AssetStampManager stamp_mgr;

    bool saw_damaged_cb = false;
    bool saw_recreated_cb = false;
    stamp_mgr.set_structure_damaged_callback([&](uint32_t, const std::string&) {
        saw_damaged_cb = true;
    });
    stamp_mgr.set_structure_recreated_callback([&](uint32_t, const std::string&) {
        saw_recreated_cb = true;
    });

    // Stamp the ruin at chunk origin (0, 0, 0) with a 2.0 second reconstitution timer!
    const voxel::ChunkKey origin = voxel::ChunkKey::from_coords(0, 0, 0);
    const float recreate_delay = 2.0f;
    const uint32_t struct_id = stamp_mgr.stamp_region_file(
        store, dirty_scheduler, nav_mgr, temp_vreg.string(), origin,
        recreate_delay, voxel::StampBlendMode::Union, "AncientShrine");

    REQUIRE(struct_id > 0);
    REQUIRE(stamp_mgr.active_structure_count() == 1ULL);
    REQUIRE(dirty_scheduler.edit_dirty_count() > 0);

    // Verify shrine wall chunk (4, 4, 4) contains solid Hard Rock ruin
    const voxel::ChunkKey wall_chunk = voxel::ChunkKey::from_coords(4, 4, 4);
    voxel::ChunkSlot* wall_slot = store.find_slot(wall_chunk);
    REQUIRE(wall_slot != nullptr);
    REQUIRE(wall_slot->state == voxel::ChunkLifecycleState::Resident);

    // Wall 2 is at world x = 138, y = 130, z = 130 -> inside chunk (4, 4, 4) at local coord (10, 2, 2)
    const voxel::LocalCoord wall_lc{10, 2, 2};
    REQUIRE(wall_slot->get_tsdf(wall_lc) < 0);
    REQUIRE(wall_slot->get_material(wall_lc) == voxel::MaterialHardness::kMaterialHardRock);

    // --- STEP 1: DESTROY PART OF THE RUIN WITH VOXEL PHYSICS ---
    // Blast directly centered on Wall 2 at (138, 130, 130)
    const voxel::Vec3f blast_pos{138.0f, 130.0f, 130.0f};
    const float blast_radius = 5.0f;

    // Fire 3 consecutive explosions at the ruin wall (3 hits to shatter Hard Rock)
    for (int hit = 1; hit <= 3; ++hit) {
        voxel::EditTransaction tx = voxel::EditTransaction::create_from_spheres(
            {voxel::SphereCsgTerm{blast_pos, blast_radius}}, 2.0f, 8);
        while (!tx.is_finished()) {
            auto payload = voxel::execute_next_edit_phase(store, dirty_scheduler, tx);
            if (payload.has_value()) {
                for (const voxel::ChunkKey dk : payload->dirtied_chunks) {
                    stamp_mgr.notify_chunk_damaged(dk);
                }
            }
        }
    }

    REQUIRE(saw_damaged_cb);
    const voxel::StampedStructureInstance* inst = stamp_mgr.find_structure(struct_id);
    REQUIRE(inst != nullptr);
    REQUIRE(inst->is_damaged);
    REQUIRE(inst->time_since_damaged == 0.0f);

    // Verify the blast center voxel was indeed carved out into air
    REQUIRE(wall_slot->get_tsdf(wall_lc) > 0); // Solid shattered to air!

    // --- STEP 2: ADVANCE TIME PARTIALLY (1.0s < 2.0s delay) ---
    auto restored_early = stamp_mgr.process_reconstitution(1.0f, store, dirty_scheduler, nav_mgr);
    REQUIRE(restored_early.empty());
    REQUIRE(inst->is_damaged);
    REQUIRE(inst->time_since_damaged >= 1.0f);
    REQUIRE(wall_slot->get_tsdf(wall_lc) > 0); // Still destroyed!

    // --- STEP 3: ADVANCE TIME PAST TIMER (additional 1.2s -> 2.2s >= 2.0s) ---
    auto restored_ready = stamp_mgr.process_reconstitution(1.2f, store, dirty_scheduler, nav_mgr);
    REQUIRE(restored_ready.size() == 1ULL);
    REQUIRE(restored_ready[0] == struct_id);
    REQUIRE(saw_recreated_cb);
    REQUIRE(!inst->is_damaged);

    // Verify the structure fully reconstituted its original solid TSDF and material!
    REQUIRE(wall_slot->get_tsdf(wall_lc) < 0);
    REQUIRE(wall_slot->get_material(wall_lc) == voxel::MaterialHardness::kMaterialHardRock);
    REQUIRE(wall_slot->get_damage(wall_lc) == 0U);

    std::printf("[PASS] test_stamping_destruction_and_reconstitution (stamped ruin destroyed by 3x blast, reconstituted after 2.0s timer)\n");
}

// ============================================================================
// 4. Permanent Destruction Mode Verification (recreate_delay <= 0)
// ============================================================================
void test_permanent_destruction_mode() {
    ++g_tests_run;

    const std::filesystem::path temp_vreg = "/tmp/test_ruin_temple.vreg";
    voxel::MemoryLedger ledger;
    voxel::ChunkStore store(ledger, /*world_height_chunks=*/8);
    voxel::DualDirtyScheduler dirty_scheduler;
    voxel::VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(voxel::ServerRid{3});

    voxel::AssetStampManager stamp_mgr;

    // Stamp with recreate_delay_seconds = 0.0f -> Permanent destruction!
    const voxel::ChunkKey origin = voxel::ChunkKey::from_coords(0, 0, 0);
    const uint32_t struct_id = stamp_mgr.stamp_region_file(
        store, dirty_scheduler, nav_mgr, temp_vreg.string(), origin,
        /*recreate_delay_seconds=*/0.0f, voxel::StampBlendMode::Union, "PermanentRuin");

    REQUIRE(struct_id > 0);

    const voxel::ChunkKey wall_chunk = voxel::ChunkKey::from_coords(4, 4, 4);
    voxel::ChunkSlot* wall_slot = store.find_slot(wall_chunk);
    REQUIRE(wall_slot != nullptr);

    // Destroy it at (138, 130, 130)
    const voxel::Vec3f blast_pos{138.0f, 130.0f, 130.0f};
    const voxel::LocalCoord wall_lc{10, 2, 2};
    REQUIRE(wall_slot->get_tsdf(wall_lc) < 0);

    for (int hit = 1; hit <= 3; ++hit) {
        voxel::EditTransaction tx = voxel::EditTransaction::create_from_spheres(
            {voxel::SphereCsgTerm{blast_pos, 5.0f}}, 2.0f, 8);
        while (!tx.is_finished()) {
            auto payload = voxel::execute_next_edit_phase(store, dirty_scheduler, tx);
            if (payload.has_value()) {
                for (const voxel::ChunkKey dk : payload->dirtied_chunks) {
                    stamp_mgr.notify_chunk_damaged(dk);
                }
            }
        }
    }

    REQUIRE(wall_slot->get_tsdf(wall_lc) > 0);

    // Advance time by 100 seconds
    auto restored = stamp_mgr.process_reconstitution(100.0f, store, dirty_scheduler, nav_mgr);
    REQUIRE(restored.empty());
    REQUIRE(wall_slot->get_tsdf(wall_lc) > 0); // Stays permanently destroyed!

    std::filesystem::remove(temp_vreg);
    std::printf("[PASS] test_permanent_destruction_mode (recreate_delay=0 stays permanently destroyed over 100s)\n");
}

// ============================================================================
// 5. Per-Voxel RGB5551 Color Channel, Outer-Shell Baking, Crater Raw Interior,
//    .vreg Persistence & remove_structure() Pre-Stamp Restoration Test
// ============================================================================
void test_per_voxel_color_channel_and_remove_structure() {
    ++g_tests_run;

    const std::filesystem::path temp_moai_vreg = "/tmp/test_painted_moai.vreg";
    const std::filesystem::path temp_save_dir = "/tmp/test_painted_moai_saves";
    std::filesystem::remove(temp_moai_vreg);
    std::filesystem::remove_all(temp_save_dir);

    // 1. Build a watertight painted Moai box block [-6, +6] x [-10, +10] x [-6, +6]
    //    with bright green moss vertex colors on the crown (y = +10) and stone below.
    std::string moai_box_obj =
        "usemtl moai_hard_rock\n"
        "v -6.0 -10.0 -6.0 0.55 0.50 0.44\n"
        "v  6.0 -10.0 -6.0 0.55 0.50 0.44\n"
        "v  6.0 -10.0  6.0 0.55 0.50 0.44\n"
        "v -6.0 -10.0  6.0 0.55 0.50 0.44\n"
        "v -6.0  10.0 -6.0 0.48 0.76 0.18\n"
        "v  6.0  10.0 -6.0 0.48 0.76 0.18\n"
        "v  6.0  10.0  6.0 0.48 0.76 0.18\n"
        "v -6.0  10.0  6.0 0.48 0.76 0.18\n"
        "# Bottom (-Y)\n"
        "f 1 2 3 4\n"
        "# Top (+Y)\n"
        "f 5 8 7 6\n"
        "# Front (+Z)\n"
        "f 4 3 7 8\n"
        "# Back (-Z)\n"
        "f 2 1 5 6\n"
        "# Right (+X)\n"
        "f 3 2 6 7\n"
        "# Left (-X)\n"
        "f 1 4 8 5\n";

    std::stringstream ss(moai_box_obj);
    voxel::RawObjMesh mesh{};
    REQUIRE(voxel::ObjMeshLoader::load_from_stream(ss, mesh));

    voxel::AssetBakerConfig cfg{};
    cfg.mode = voxel::VoxelizationMode::WatertightSolid;
    cfg.bake_surface_colors = true;
    cfg.color_shell_depth = 1.8f; // Outer 1.8m shell is painted; deep interior (>1.8m) is unpainted raw rock!
    cfg.auto_center_in_region = true; // Centers at (128, 128, 128)

    const voxel::BakedRegionData baked = voxel::VoxelMeshBaker::bake_mesh_to_region(mesh, cfg);
    REQUIRE(baked.active_chunk_count > 0);
    REQUIRE(voxel::VoxelMeshBaker::save_baked_region_to_file(temp_moai_vreg.string(), baked));

    voxel::MemoryLedger ledger;
    voxel::ChunkStore store(ledger, /*world_height_chunks=*/8);
    voxel::DualDirtyScheduler dirty_scheduler;
    voxel::VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(voxel::ServerRid{3});
    voxel::WorkerScratchWindow36 scratch(ledger);
    voxel::AssetStampManager stamp_mgr;

    // Pre-populate chunk (4, 4, 4) with a distinctive pre-stamp terrain voxel at (20, 20, 20)
    // so we can verify remove_structure() restores the pre-stamp chunk state bit-exactly!
    const voxel::ChunkKey ck444 = voxel::ChunkKey::from_coords(4, 4, 4);
    voxel::ChunkSlot* slot444 = store.get_or_create_slot(ck444);
    {
        std::unique_lock<std::shared_mutex> lk(slot444->mutex);
        slot444->reset_to_uniform(
            voxel::ChunkStorageForm::Empty, voxel::MaterialHardness::kMaterialSoil,
            store.tsdf_pool(), store.material_pool());
        slot444->state = voxel::ChunkLifecycleState::Resident;
        slot444->version.store(1, std::memory_order_release);
        REQUIRE(slot444->set_tsdf(voxel::LocalCoord{20, 20, 20}, -5000, store.tsdf_pool()));
        REQUIRE(slot444->set_material(
            voxel::LocalCoord{20, 20, 20}, voxel::MaterialHardness::kMaterialSoil, store.material_pool()));
    }

    // Stamp the painted Moai at origin (0, 0, 0)
    bool saw_removed_cb = false;
    stamp_mgr.set_structure_removed_callback([&](uint32_t, const std::string&) {
        saw_removed_cb = true;
    });

    const uint32_t struct_id = stamp_mgr.stamp_region_file(
        store, dirty_scheduler, nav_mgr, temp_moai_vreg.string(),
        voxel::ChunkKey::from_coords(0, 0, 0), 5.0f, voxel::StampBlendMode::Union, "PaintedMoai");
    REQUIRE(struct_id > 0);
    REQUIRE(slot444->has_color_array());

    // Verify outer skin voxel at (128+5, 128+9, 128+5) -> local (5, 9, 5) in chunk (4, 4, 4) IS PAINTED
    const voxel::LocalCoord outer_skin_lc{5, 9, 5};
    REQUIRE(slot444->get_tsdf(outer_skin_lc) < 0);
    const uint16_t skin_col = slot444->get_color(outer_skin_lc);
    REQUIRE(voxel::VoxelColor::has_color(skin_col));
    float sr = 0.0f, sg = 0.0f, sb = 0.0f;
    voxel::VoxelColor::decode(skin_col, sr, sg, sb);
    REQUIRE(sg > 0.55f); // Bright green moss near crown!

    // Verify deep interior core voxel at (128+1, 128+1, 128+1) -> local (1, 1, 1) IS SOLID BUT UNPAINTED (raw rock!)
    const voxel::LocalCoord deep_core_lc{1, 1, 1};
    REQUIRE(slot444->get_tsdf(deep_core_lc) < 0);
    REQUIRE(!voxel::VoxelColor::has_color(slot444->get_color(deep_core_lc)));

    // Mesh chunk (4, 4, 4) before carving: outer surface vertices must have baked_albedo_weights == 1.0f
    {
        const auto stamp = voxel::gather_scratch_window_36(store, ck444, scratch);
        const auto mesh_initial = voxel::SurfaceNetsMesher::mesh_chunk_from_scratch_36(
            ck444, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(!mesh_initial.vertices.empty());
        REQUIRE(mesh_initial.baked_albedos.size() == mesh_initial.vertices.size());
        REQUIRE(mesh_initial.baked_albedo_weights.size() == mesh_initial.vertices.size());

        std::size_t painted_verts = 0;
        for (std::size_t i = 0; i < mesh_initial.vertices.size(); ++i) {
            if (mesh_initial.baked_albedo_weights[i] > 0.5f) {
                ++painted_verts;
            }
        }
        REQUIRE(painted_verts > 50ULL);
    }

    // Verify two-stage .vreg persistence preserves color_data across disk flush & reload
    {
        voxel::RegionSaveManager save_mgr(ledger, temp_save_dir);
        voxel::CaveNoiseGenerator procgen;
        voxel::WorkerDensityGrid34 grid34(ledger);

        REQUIRE(save_mgr.persist_compress_chunk(store, ck444));
        REQUIRE(save_mgr.flush_region_to_disk(voxel::RegionKey::from_chunk_key(ck444)));
        save_mgr.clear_ram_cache();

        // Clear color in live slot, then reload from disk .vreg
        (void)slot444->clear_color(outer_skin_lc);
        REQUIRE(!voxel::VoxelColor::has_color(slot444->get_color(outer_skin_lc)));

        REQUIRE(save_mgr.load_or_generate_chunk(store, ck444, procgen, grid34));
        REQUIRE(slot444->get_color(outer_skin_lc) == skin_col);
    }

    // Blast a 3-hit crater into the front face at (133, 133, 134) so the outer painted skin is carved away
    // and the unpainted interior rock is exposed!
    for (int hit = 1; hit <= 3; ++hit) {
        voxel::EditTransaction tx = voxel::EditTransaction::create_from_spheres(
            {voxel::SphereCsgTerm{voxel::Vec3f{133.0f, 133.0f, 134.0f}, 4.5f}}, 2.0f, 8);
        while (!tx.is_finished()) {
            (void)voxel::execute_next_edit_phase(store, dirty_scheduler, tx);
        }
    }

    {
        const auto stamp = voxel::gather_scratch_window_36(store, ck444, scratch);
        const auto mesh_carved = voxel::SurfaceNetsMesher::mesh_chunk_from_scratch_36(
            ck444, scratch, 1, stamp.captured_terrain_revision);
        REQUIRE(!mesh_carved.vertices.empty());
        std::size_t unpainted_interior_verts = 0;
        for (float w : mesh_carved.baked_albedo_weights) {
            if (w == 0.0f) {
                ++unpainted_interior_verts;
            }
        }
        REQUIRE(unpainted_interior_verts > 10ULL);
    }

    // Now test remove_structure(struct_id): must restore chunk (4, 4, 4) to its exact pre-stamp state!
    REQUIRE(stamp_mgr.remove_structure(struct_id, store, dirty_scheduler, nav_mgr));
    REQUIRE(saw_removed_cb);
    REQUIRE(stamp_mgr.active_structure_count() == 0ULL);
    REQUIRE(!slot444->has_color_array());
    REQUIRE(slot444->get_tsdf(outer_skin_lc) == voxel::TsdfFormat::kClampedAir);
    REQUIRE(slot444->get_tsdf(voxel::LocalCoord{20, 20, 20}) == -5000);
    REQUIRE(slot444->get_material(voxel::LocalCoord{20, 20, 20}) == voxel::MaterialHardness::kMaterialSoil);

    std::filesystem::remove(temp_moai_vreg);
    std::filesystem::remove_all(temp_save_dir);
    std::printf("[PASS] test_per_voxel_color_channel_and_remove_structure (RGB5551 outer skin, raw crater interior, .vreg color save & remove_structure verified)\n");
}

// ============================================================================
// 6. Cracked Watertight Mesh Must Not Extrude Ray-Parity Needles
// ============================================================================
void test_watertight_crack_does_not_extrude_needles() {
    ++g_tests_run;

    const std::filesystem::path temp_vreg = "/tmp/test_cracked_cube.vreg";
    std::filesystem::remove(temp_vreg);

    // Closed 8 m cube with one triangle deliberately omitted from the +Z face.
    // A +Z parity ray entering the -Z face exits through that hole, so the naive
    // parity test sees an odd crossing count and marks the whole shadow column
    // behind the cube solid: a one-voxel-wide spike for every affected column.
    const std::string cracked_cube_obj =
        "usemtl moai_hard_rock\n"
        "v -4.0 -4.0 -4.0\n"
        "v  4.0 -4.0 -4.0\n"
        "v  4.0  4.0 -4.0\n"
        "v -4.0  4.0 -4.0\n"
        "v -4.0 -4.0  4.0\n"
        "v  4.0 -4.0  4.0\n"
        "v  4.0  4.0  4.0\n"
        "v -4.0  4.0  4.0\n"
        "# -Z face\n"
        "f 1 3 2\n"
        "f 1 4 3\n"
        "# +Z face: only one of its two triangles (the other leaves a crack)\n"
        "f 5 7 8\n"
        "# -Y face\n"
        "f 1 6 5\n"
        "f 1 2 6\n"
        "# +Y face\n"
        "f 4 7 3\n"
        "f 4 8 7\n"
        "# -X face\n"
        "f 1 8 4\n"
        "f 1 5 8\n"
        "# +X face\n"
        "f 2 7 6\n"
        "f 2 3 7\n";

    std::stringstream ss(cracked_cube_obj);
    voxel::RawObjMesh mesh{};
    REQUIRE(voxel::ObjMeshLoader::load_from_stream(ss, mesh));

    voxel::AssetBakerConfig cfg{};
    cfg.mode = voxel::VoxelizationMode::WatertightSolid;
    cfg.voxel_scale = 1.0f;
    cfg.truncation_delta = 2.0f;
    cfg.auto_center_in_region = true; // Cube center lands on sample (128, 128, 128)

    const voxel::BakedRegionData baked = voxel::VoxelMeshBaker::bake_mesh_to_region(mesh, cfg);
    REQUIRE(baked.active_chunk_count > 0);
    REQUIRE(voxel::VoxelMeshBaker::save_baked_region_to_file(temp_vreg.string(), baked));

    voxel::MemoryLedger ledger;
    voxel::ChunkStore store(ledger, /*world_height_chunks=*/8);
    voxel::DualDirtyScheduler dirty_scheduler;
    voxel::VoxelNavigationManager nav_mgr;
    nav_mgr.initialize(voxel::ServerRid{3});
    voxel::AssetStampManager stamp_mgr;

    const uint32_t struct_id = stamp_mgr.stamp_region_file(
        store, dirty_scheduler, nav_mgr, temp_vreg.string(),
        voxel::ChunkKey::from_coords(0, 0, 0), 5.0f,
        voxel::StampBlendMode::Union, "CrackedCube");
    REQUIRE(struct_id > 0);

    auto tsdf_at = [&](int32_t sx, int32_t sy, int32_t sz) -> int16_t {
        voxel::ChunkSlot* slot = store.find_slot(
            voxel::ChunkKey::from_coords(sx / 32, sy / 32, sz / 32));
        if (!slot) {
            return voxel::TsdfFormat::kClampedAir;
        }
        return slot->get_tsdf(voxel::LocalCoord{sx % 32, sy % 32, sz % 32});
    };

    // The cube interior is still solid.
    REQUIRE(tsdf_at(128, 128, 128) < 0);

    // The two sample layers directly behind the -Z face, inside the old parity
    // shadow of the +Z crack, must all be air. Before the crack-robust
    // classification these columns baked as solid needles.
    std::size_t back_samples = 0;
    std::size_t back_air = 0;
    for (int32_t sy = 120; sy <= 136; ++sy) {
        for (int32_t sx = 120; sx <= 136; ++sx) {
            for (int32_t sz : {122, 123}) {
                ++back_samples;
                if (tsdf_at(sx, sy, sz) >= 0) {
                    ++back_air;
                }
            }
        }
    }
    REQUIRE(back_samples > 0);
    REQUIRE(back_air == back_samples);

    // Just outside the intact -X face must be air too.
    REQUIRE(tsdf_at(121, 128, 128) >= 0);

    REQUIRE(stamp_mgr.remove_structure(struct_id, store, dirty_scheduler, nav_mgr));
    std::filesystem::remove(temp_vreg);
    std::printf("[PASS] test_watertight_crack_does_not_extrude_needles (hole in +Z face leaves no solid parity shadow)\n");
}

} // namespace

int main() {
    std::printf("=== Running Offline Asset Voxelization, RGB5551 Color & Reconstitution Test Suite ===\n");
    test_obj_loader_and_distance_math();
    test_mesh_voxelization_to_vreg_region();
    test_stamping_destruction_and_reconstitution();
    test_permanent_destruction_mode();
    test_per_voxel_color_channel_and_remove_structure();
    test_watertight_crack_does_not_extrude_needles();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
