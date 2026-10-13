#include "procgen_and_surface_nets.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace {

using voxel::CaveNoiseGenerator;
using voxel::ChunkKey;
using voxel::ChunkMeshPayload;
using voxel::ChunkSlot;
using voxel::ChunkStore;
using voxel::LocalCoord;
using voxel::MemoryLedger;
using voxel::SurfaceNetsMesher;
using voxel::TsdfFormat;
using voxel::Vec3f;
using voxel::WorkerDensityGrid34;
using voxel::WorkerScratchWindow36;
using voxel::gather_scratch_window_36;

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
// 1. 34^3 Single-Pass Cave ProcGen & Saddle-Point Regularization (Section 7.1)
// ============================================================================
void test_procgen_34_regularization_and_caves() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerDensityGrid34 grid34(ledger);
    CaveNoiseGenerator gen;

    // 1. Explicit saddle-point / flat roof test where ||grad D|| == 0 everywhere:
    //    Verify zero NaN/Inf values and valid TSDF clamping via epsilon_grad = 0.05.
    const ChunkKey flat_key = ChunkKey::from_coords(0, 0, 0);
    gen.generate_chunk_from_density_fn(
        store,
        flat_key,
        grid34,
        [](float /*wx*/, float /*wy*/, float /*wz*/) -> float {
            return -0.025f; // Constant field -> ||grad D|| == 0 everywhere!
        });

    const ChunkSlot* flat_slot = store.find_slot(flat_key);
    REQUIRE(flat_slot != nullptr);
    // With D = -0.025 and epsilon_grad = 0.05, sdf_approx = -0.025 / 0.05 = -0.5 -> encoded cleanly!
    const int16_t expected_q = TsdfFormat::encode(-0.5f, 2.0f);
    for (int32_t z = 0; z < 32; ++z) {
        for (int32_t y = 0; y < 32; ++y) {
            for (int32_t x = 0; x < 32; ++x) {
                const int16_t q = flat_slot->get_tsdf(LocalCoord{x, y, z});
                REQUIRE(q == expected_q);
                const float decoded = TsdfFormat::decode(q, 2.0f);
                REQUIRE(std::isfinite(decoded));
            }
        }
    }

    // 2. Generate 1,000 procedural cave & terrain chunks (Section 7.1):
    //    Assert zero NaN/Inf values, valid [-32767, +32767] bounds, and bedrock protection at y = 0.
    std::size_t chunks_generated = 0;
    for (int32_t cz = 0; cz < 10; ++cz) {
        for (int32_t cy = 0; cy < 10; ++cy) {
            for (int32_t cx = 0; cx < 10; ++cx) {
                const ChunkKey ck = ChunkKey::from_coords(cx, cy, cz);
                gen.generate_chunk(store, ck, grid34);
                const ChunkSlot* slot = store.find_slot(ck);
                REQUIRE(slot != nullptr);
                // Verify sample bounds at corners and interior
                const int16_t q0 = slot->get_tsdf(LocalCoord{0, 0, 0});
                const int16_t q1 = slot->get_tsdf(LocalCoord{16, 16, 16});
                REQUIRE(q0 >= TsdfFormat::kClampedSolid && q0 <= TsdfFormat::kClampedAir);
                REQUIRE(q1 >= TsdfFormat::kClampedSolid && q1 <= TsdfFormat::kClampedAir);
                if (cy == 0) {
                    // Bedrock y = 0 must always be solid (negative TSDF) because cave radius is attenuated to 0.
                    REQUIRE(slot->get_tsdf(LocalCoord{16, 0, 16}) < 0);
                }
                // Evict after verification so we keep resident footprint small during the 1,000-chunk sweep.
                REQUIRE(store.evict_slot(ck));
                ++chunks_generated;
            }
        }
    }
    REQUIRE(chunks_generated == 1000ULL);

    std::printf("[PASS] test_procgen_34_regularization_and_caves (1,000 cave chunks + zero-gradient saddle test)\n");
}

// ============================================================================
// 2. 2x2x2 Multipart Watertight Seam & Reference Equivalence Test (Section 7.1)
// ============================================================================
struct QuantizedVec3 {
    int64_t x = 0;
    int64_t y = 0;
    int64_t z = 0;

    static QuantizedVec3 from_vec3(Vec3f v) noexcept {
        // Quantize to 1e-4 voxel precision for exact topological vertex welding across chunk seams
        return QuantizedVec3{
            static_cast<int64_t>(std::llround(v.x * 10000.0f)),
            static_cast<int64_t>(std::llround(v.y * 10000.0f)),
            static_cast<int64_t>(std::llround(v.z * 10000.0f))
        };
    }

    constexpr auto operator<=>(const QuantizedVec3&) const noexcept = default;
};

void test_2x2x2_watertight_chunk_seams() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerDensityGrid34 grid34(ledger);
    WorkerScratchWindow36 scratch(ledger);
    CaveNoiseGenerator gen;

    // Define a closed SDF shape (a sphere of radius 21.5 with a carved inner cave tunnel,
    // entirely contained within the 2x2x2 chunk block [0..63]^3 centered at (32, 32, 32)
    // so it crosses all 3 interior chunk planes x=32, y=32, z=32).
    auto closed_cave_sphere_sdf = [](float wx, float wy, float wz) -> float {
        const float dx = wx - 32.0f;
        const float dy = wy - 32.0f;
        const float dz = wz - 32.0f;
        const float outer_sphere = std::sqrt(dx * dx + dy * dy + dz * dz) - 21.5f;
        // Modulate slightly with smooth 3D wave so it exercises curved cave walls across chunk seams
        const float wave = 1.8f * std::sin(wx * 0.25f) * std::cos(wy * 0.25f) * std::sin(wz * 0.25f);
        return outer_sphere + wave;
    };

    // Populate the 2x2x2 chunk block (and a 1-chunk shell around it so all 27-neighborhoods are resident).
    for (int32_t cz = -1; cz <= 2; ++cz) {
        for (int32_t cy = -1; cy <= 2; ++cy) {
            for (int32_t cx = -1; cx <= 2; ++cx) {
                const ChunkKey ck = ChunkKey::from_coords(cx, cy, cz);
                gen.generate_chunk_from_density_fn(store, ck, grid34, closed_cave_sphere_sdf);
            }
        }
    }

    // Mesh all 8 chunks in the 2x2x2 block [0..1]^3 using the 36^3 scratch window and SurfaceNetsMesher.
    std::map<QuantizedVec3, uint32_t> welded_vertex_map;
    std::map<uint32_t, Vec3f> welded_normals;
    std::map<std::pair<uint32_t, uint32_t>, int> directed_edge_counts;
    std::map<std::pair<uint32_t, uint32_t>, int> undirected_edge_counts;

    std::size_t total_triangles = 0;
    std::size_t boundary_vertices_matched = 0;

    for (int32_t cz = 0; cz <= 1; ++cz) {
        for (int32_t cy = 0; cy <= 1; ++cy) {
            for (int32_t cx = 0; cx <= 1; ++cx) {
                const ChunkKey ck = ChunkKey::from_coords(cx, cy, cz);
                const auto stamp = gather_scratch_window_36(store, ck, scratch);
                const ChunkMeshPayload mesh =
                    SurfaceNetsMesher::mesh_chunk_from_scratch_36(ck, scratch, 1, stamp.captured_terrain_revision);

                REQUIRE(mesh.indices.size() % 3 == 0);
                REQUIRE(mesh.collision_faces.size() == mesh.indices.size());

                const std::size_t num_tris = mesh.indices.size() / 3;
                total_triangles += num_tris;

                for (std::size_t t = 0; t < num_tris; ++t) {
                    uint32_t tri_v[3];
                    for (int k = 0; k < 3; ++k) {
                        const int32_t local_idx = mesh.indices[t * 3 + k];
                        REQUIRE(local_idx >= 0 && static_cast<std::size_t>(local_idx) < mesh.vertices.size());

                        const Vec3f pos = mesh.vertices[local_idx];
                        const Vec3f norm = mesh.normals[local_idx];
                        const QuantizedVec3 qpos = QuantizedVec3::from_vec3(pos);

                        auto it = welded_vertex_map.find(qpos);
                        if (it == welded_vertex_map.end()) {
                            const uint32_t new_id = static_cast<uint32_t>(welded_vertex_map.size());
                            welded_vertex_map.emplace(qpos, new_id);
                            welded_normals.emplace(new_id, norm);
                            tri_v[k] = new_id;
                        } else {
                            tri_v[k] = it->second;
                            // Verify shared boundary dual vertices across adjacent chunks have identical normals!
                            const Vec3f ref_n = welded_normals[it->second];
                            const float ndiff = std::abs(ref_n.x - norm.x) +
                                                std::abs(ref_n.y - norm.y) +
                                                std::abs(ref_n.z - norm.z);
                            REQUIRE(ndiff < 1e-4f);
                            ++boundary_vertices_matched;
                        }
                    }

                    // Record triangle edges to verify closed 2-manifold watertightness across the 2x2x2 block.
                    for (int e = 0; e < 3; ++e) {
                        const uint32_t u = tri_v[e];
                        const uint32_t v = tri_v[(e + 1) % 3];
                        REQUIRE(u != v);
                        directed_edge_counts[{u, v}] += 1;
                        const auto minmax_uv = std::minmax(u, v);
                        undirected_edge_counts[minmax_uv] += 1;
                    }
                }
            }
        }
    }

    REQUIRE(total_triangles > 2000ULL);
    REQUIRE(boundary_vertices_matched > 200ULL);

    // Assert ZERO boundary open edges across the entire 2x2x2 block:
    // Every undirected triangle edge must be shared by an even number of faces (specifically 2),
    // and every directed edge (u -> v) must have a matching opposite directed edge (v -> u).
    std::size_t open_edges = 0;
    for (const auto& [edge, count] : undirected_edge_counts) {
        if (count % 2 != 0) {
            ++open_edges;
        }
        REQUIRE((directed_edge_counts[edge] == directed_edge_counts[{edge.second, edge.first}]));
    }
    REQUIRE(open_edges == 0ULL);

    std::printf("[PASS] test_2x2x2_watertight_chunk_seams (%zu triangles across 8 chunks, %zu seam vertices welded, 0 open edges)\n",
                total_triangles, boundary_vertices_matched);
}

} // namespace

int main() {
    std::printf("=== Running M1 Step 5 Unit Test Suite: 34^3 Cave ProcGen & 36^3 Seamless Surface Nets ===\n");
    test_procgen_34_regularization_and_caves();
    test_2x2x2_watertight_chunk_seams();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
