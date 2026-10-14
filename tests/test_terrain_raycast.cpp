#include "terrain_raycast.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <shared_mutex>

namespace {

using voxel::ChunkKey;
using voxel::ChunkLifecycleState;
using voxel::ChunkSlot;
using voxel::ChunkStorageForm;
using voxel::ChunkStore;
using voxel::LocalCoord;
using voxel::MemoryLedger;
using voxel::TsdfFormat;
using voxel::Vec3f;
using voxel::raycast_terrain;

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

ChunkSlot* make_resident_slot(ChunkStore& store, ChunkKey ck, ChunkStorageForm form) {
    ChunkSlot* slot = store.get_or_create_slot(ck);
    std::unique_lock<std::shared_mutex> lock(slot->mutex);
    slot->reset_to_uniform(form, 1, store.tsdf_pool(), store.material_pool());
    slot->state = ChunkLifecycleState::Resident;
    slot->version.store(1, std::memory_order_release);
    return slot;
}

void build_flat_ground_chunk(ChunkStore& store, ChunkKey ck, int32_t surface_local_y) {
    const float delta = TsdfFormat::kDefaultTruncationDelta;
    ChunkSlot* slot = make_resident_slot(store, ck, ChunkStorageForm::Solid);
    std::unique_lock<std::shared_mutex> lock(slot->mutex);
    for (int32_t y = surface_local_y; y < 32; ++y) {
        for (int32_t z = 0; z < 32; ++z) {
            for (int32_t x = 0; x < 32; ++x) {
                (void)slot->set_tsdf(LocalCoord{x, y, z}, TsdfFormat::encode(+1.0f, delta),
                                     store.tsdf_pool());
            }
        }
    }
    for (int32_t z = 0; z < 32; ++z) {
        for (int32_t x = 0; x < 32; ++x) {
            (void)slot->set_tsdf(LocalCoord{x, surface_local_y - 1, z},
                                 TsdfFormat::encode(-0.5f, delta), store.tsdf_pool());
        }
    }
}

void test_downward_ray_hits_flat_ground() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    build_flat_ground_chunk(store, ChunkKey::from_coords(0, 0, 0), /*surface_local_y=*/16);

    const auto hit = raycast_terrain(store, Vec3f{16.0f, 24.0f, 16.0f}, Vec3f{16.0f, 8.0f, 16.0f});
    REQUIRE(hit.has_value());
    // Surface between sample y=16 (S=+1.0) and y=15 (S=-0.5): t = 1.0/1.5.
    REQUIRE(std::abs(hit->y - 15.3333f) < 0.05f);
    REQUIRE(std::abs(hit->x - 16.0f) < 0.01f);
    REQUIRE(std::abs(hit->z - 16.0f) < 0.01f);

    std::printf("[PASS] test_downward_ray_hits_flat_ground (hit at y=%.3f)\n", hit->y);
}

void test_ray_into_sky_misses() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    build_flat_ground_chunk(store, ChunkKey::from_coords(0, 0, 0), /*surface_local_y=*/16);

    const auto hit = raycast_terrain(store, Vec3f{16.0f, 24.0f, 16.0f}, Vec3f{16.0f, 40.0f, 16.0f});
    REQUIRE(!hit.has_value());

    std::printf("[PASS] test_ray_into_sky_misses (no surface above the ground plane)\n");
}

void test_ray_crosses_chunk_boundary() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    build_flat_ground_chunk(store, ChunkKey::from_coords(0, 0, 0), /*surface_local_y=*/16);

    // Solid wall at world x = 40..47 inside chunk (1, 0, 0); explicit +1.0 air
    // at world x = 39 so the face interpolates between two smooth samples.
    const float delta = TsdfFormat::kDefaultTruncationDelta;
    ChunkSlot* wall = make_resident_slot(store, ChunkKey::from_coords(1, 0, 0), ChunkStorageForm::Empty);
    {
        std::unique_lock<std::shared_mutex> lock(wall->mutex);
        for (int32_t y = 0; y < 32; ++y) {
            for (int32_t z = 0; z < 32; ++z) {
                (void)wall->set_tsdf(LocalCoord{7, y, z}, TsdfFormat::encode(+1.0f, delta),
                                     store.tsdf_pool());
                for (int32_t x = 8; x < 16; ++x) {
                    (void)wall->set_tsdf(LocalCoord{x, y, z}, TsdfFormat::encode(-1.0f, delta),
                                         store.tsdf_pool());
                }
            }
        }
    }

    const auto hit = raycast_terrain(store, Vec3f{16.0f, 20.0f, 16.0f}, Vec3f{60.0f, 20.0f, 16.0f});
    REQUIRE(hit.has_value());
    // Surface between world x=39 (S=+1.0) and x=40 (S=-1.0): t = 0.5.
    REQUIRE(std::abs(hit->x - 39.5f) < 0.05f);
    REQUIRE(std::abs(hit->y - 20.0f) < 0.05f);
    REQUIRE(std::abs(hit->z - 16.0f) < 0.05f);

    std::printf("[PASS] test_ray_crosses_chunk_boundary (hit at x=%.3f)\n", hit->x);
}

void test_ray_starting_inside_solid_misses() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    build_flat_ground_chunk(store, ChunkKey::from_coords(0, 0, 0), /*surface_local_y=*/16);

    // y = 10 is deep inside the solid half of the chunk.
    const auto hit = raycast_terrain(store, Vec3f{16.0f, 10.0f, 16.0f}, Vec3f{16.0f, 0.0f, 16.0f});
    REQUIRE(!hit.has_value());

    std::printf("[PASS] test_ray_starting_inside_solid_misses (no self-hit from inside rock)\n");
}

} // namespace

int main() {
    std::printf("=== Running Terrain Voxel Raycast Test Suite ===\n");
    test_downward_ray_hits_flat_ground();
    test_ray_into_sky_misses();
    test_ray_crosses_chunk_boundary();
    test_ray_starting_inside_solid_misses();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
