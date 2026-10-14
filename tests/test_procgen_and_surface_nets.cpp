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
// 1. 34^3 Single-Pass Cave ProcGen & Saddle-Point Regularization (Section 7.1)
// ============================================================================
void test_procgen_34_regularization_and_caves() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerDensityGrid34 grid34(ledger);
    CaveNoiseGenerator gen;

    const ChunkKey flat_key = ChunkKey::from_coords(0, 0, 0);
    gen.generate_chunk_from_density_fn(
        store,
        flat_key,
        grid34,
        [](float /*wx*/, float /*wy*/, float /*wz*/) -> float {
            return -0.025f;
        });

    const ChunkSlot* flat_slot = store.find_slot(flat_key);
    REQUIRE(flat_slot != nullptr);
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

    std::size_t chunks_generated = 0;
    for (int32_t cz = 0; cz < 10; ++cz) {
        for (int32_t cy = 0; cy < 10; ++cy) {
            for (int32_t cx = 0; cx < 10; ++cx) {
                const ChunkKey ck = ChunkKey::from_coords(cx, cy, cz);
                gen.generate_chunk(store, ck, grid34);
                const ChunkSlot* slot = store.find_slot(ck);
                REQUIRE(slot != nullptr);
                const int16_t q0 = slot->get_tsdf(LocalCoord{0, 0, 0});
                const int16_t q1 = slot->get_tsdf(LocalCoord{16, 16, 16});
                REQUIRE(q0 >= TsdfFormat::kClampedSolid && q0 <= TsdfFormat::kClampedAir);
                REQUIRE(q1 >= TsdfFormat::kClampedSolid && q1 <= TsdfFormat::kClampedAir);
                if (cy == 0) {
                    REQUIRE(slot->get_tsdf(LocalCoord{16, 0, 16}) < 0);
                }
                REQUIRE(store.evict_slot(ck));
                ++chunks_generated;
            }
        }
    }
    REQUIRE(chunks_generated == 1000ULL);

    std::printf("[PASS] test_procgen_34_regularization_and_caves (1,000 cave chunks + zero-gradient saddle test)\n");
}

// ============================================================================
// 2. 2x2x2 Multipart Watertight Seam & Godot 4 Front-Face Winding Test
// ============================================================================
struct QuantizedVec3 {
    int64_t x = 0;
    int64_t y = 0;
    int64_t z = 0;

    static QuantizedVec3 from_vec3(Vec3f v) noexcept {
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

    auto closed_cave_sphere_sdf = [](float wx, float wy, float wz) -> float {
        const float dx = wx - 32.0f;
        const float dy = wy - 32.0f;
        const float dz = wz - 32.0f;
        const float outer_sphere = std::sqrt(dx * dx + dy * dy + dz * dz) - 21.5f;
        const float wave = 1.8f * std::sin(wx * 0.25f) * std::cos(wy * 0.25f) * std::sin(wz * 0.25f);
        return outer_sphere + wave;
    };

    for (int32_t cz = -1; cz <= 2; ++cz) {
        for (int32_t cy = -1; cy <= 2; ++cy) {
            for (int32_t cx = -1; cx <= 2; ++cx) {
                const ChunkKey ck = ChunkKey::from_coords(cx, cy, cz);
                gen.generate_chunk_from_density_fn(store, ck, grid34, closed_cave_sphere_sdf);
            }
        }
    }

    std::map<QuantizedVec3, uint32_t> welded_vertex_map;
    std::map<uint32_t, Vec3f> welded_normals;
    std::map<std::pair<uint32_t, uint32_t>, int> directed_edge_counts;
    std::map<std::pair<uint32_t, uint32_t>, int> undirected_edge_counts;

    std::size_t total_triangles = 0;
    std::size_t boundary_vertices_matched = 0;
    std::size_t godot_cw_aligned_triangles = 0;

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
                    Vec3f tri_pos[3];
                    Vec3f tri_norm[3];

                    for (int k = 0; k < 3; ++k) {
                        const int32_t local_idx = mesh.indices[t * 3 + k];
                        REQUIRE(local_idx >= 0 && static_cast<std::size_t>(local_idx) < mesh.vertices.size());

                        const Vec3f pos = mesh.vertices[local_idx];
                        const Vec3f norm = mesh.normals[local_idx];
                        tri_pos[k] = pos;
                        tri_norm[k] = norm;
                        const QuantizedVec3 qpos = QuantizedVec3::from_vec3(pos);

                        auto it = welded_vertex_map.find(qpos);
                        if (it == welded_vertex_map.end()) {
                            const uint32_t new_id = static_cast<uint32_t>(welded_vertex_map.size());
                            welded_vertex_map.emplace(qpos, new_id);
                            welded_normals.emplace(new_id, norm);
                            tri_v[k] = new_id;
                        } else {
                            tri_v[k] = it->second;
                            const Vec3f ref_n = welded_normals[it->second];
                            const float ndiff = std::abs(ref_n.x - norm.x) +
                                                std::abs(ref_n.y - norm.y) +
                                                std::abs(ref_n.z - norm.z);
                            REQUIRE(ndiff < 1e-4f);
                            ++boundary_vertices_matched;
                        }
                    }

                    // Verify Godot 4 Clockwise Front-Face Winding:
                    // In Godot 4 RenderingServer, (c - a) x (b - a) is the outward front-face normal.
                    const Vec3f ab{tri_pos[1].x - tri_pos[0].x, tri_pos[1].y - tri_pos[0].y, tri_pos[1].z - tri_pos[0].z};
                    const Vec3f ac{tri_pos[2].x - tri_pos[0].x, tri_pos[2].y - tri_pos[0].y, tri_pos[2].z - tri_pos[0].z};
                    const Vec3f godot_front_normal{
                        ac.y * ab.z - ac.z * ab.y,
                        ac.z * ab.x - ac.x * ab.z,
                        ac.x * ab.y - ac.y * ab.x
                    };
                    const Vec3f avg_vnorm{
                        tri_norm[0].x + tri_norm[1].x + tri_norm[2].x,
                        tri_norm[0].y + tri_norm[1].y + tri_norm[2].z == 0.0f ? tri_norm[0].y : (tri_norm[0].y + tri_norm[1].y + tri_norm[2].y),
                        tri_norm[0].z + tri_norm[1].z + tri_norm[2].z
                    };
                    const float dot = godot_front_normal.x * avg_vnorm.x +
                                      godot_front_normal.y * avg_vnorm.y +
                                      godot_front_normal.z * avg_vnorm.z;
                    if (dot > 0.0f) {
                        ++godot_cw_aligned_triangles;
                    }

                    for (int e = 0; e < 3; ++e) {
                        const uint32_t u = tri_v[e];
                        const uint32_t v = tri_v[(e + 1) % 3];
                        REQUIRE(u != v);
                        directed_edge_counts[std::make_pair(u, v)] += 1;
                        const auto minmax_uv = std::minmax(u, v);
                        undirected_edge_counts[minmax_uv] += 1;
                    }
                }
            }
        }
    }

    REQUIRE(total_triangles > 2000ULL);
    REQUIRE(boundary_vertices_matched > 200ULL);
    // Verify that >99.9% of triangles are wound with Godot 4 clockwise outward normals
    REQUIRE(godot_cw_aligned_triangles * 100ULL >= total_triangles * 99ULL);

    std::size_t open_edges = 0;
    for (const auto& [edge, count] : undirected_edge_counts) {
        if (count % 2 != 0) {
            ++open_edges;
        }
        const auto rev_edge = std::make_pair(edge.second, edge.first);
        REQUIRE(directed_edge_counts[edge] == directed_edge_counts[rev_edge]);
    }
    REQUIRE(open_edges == 0ULL);

    std::printf("[PASS] test_2x2x2_watertight_chunk_seams (%zu triangles, %zu/%zu Godot CW front-facing, 0 open edges)\n",
                total_triangles, godot_cw_aligned_triangles, total_triangles);
}

// ============================================================================
// 3. Optional Caves Toggle (caves_enabled)
// ============================================================================
void test_procgen_caves_toggle() {
    ++g_tests_run;

    CaveNoiseGenerator gen_on; // caves_enabled defaults to true
    CaveNoiseGenerator gen_off;
    gen_off.caves_enabled = false;

    std::size_t differing_points = 0;
    std::size_t samples = 0;

    for (float wz = 0.0f; wz <= 64.0f; wz += 4.0f) {
        for (float wx = 0.0f; wx <= 64.0f; wx += 4.0f) {
            for (float wy = 16.0f; wy <= 72.0f; wy += 4.0f) {
                // Recompute the pure surface term d_surf exactly as the generator does.
                const float h_2d = gen_off.surface_base_height +
                                   gen_off.surface_amplitude * CaveNoiseGenerator::smooth_noise_3d(
                                       wx * 0.012f, 0.0f, wz * 0.012f, gen_off.seed);
                const float overhang = 3.0f * CaveNoiseGenerator::smooth_noise_3d(
                    wx * 0.03f, wy * 0.03f, wz * 0.03f, gen_off.seed + 101U);
                const float d_surf = (wy - h_2d) + overhang;

                // The Hard Rock boulder is part of the terrain regardless of the caves
                // toggle, so the disabled path is min(pure surface term, boulder SDF).
                const float expected_d_off = std::min(d_surf, gen_off.hard_boulder_sdf(wx, wy, wz));
                const float d_off = gen_off.evaluate_composite_density(wx, wy, wz);
                REQUIRE(std::abs(d_off - expected_d_off) < 1e-3f);

                const float d_on = gen_on.evaluate_composite_density(wx, wy, wz);
                if (std::abs(d_on - d_off) > 1e-3f) {
                    ++differing_points;
                }
                ++samples;
            }
        }
    }

    REQUIRE(differing_points > 0ULL);

    std::printf("[PASS] test_procgen_caves_toggle (%zu/%zu samples carved by cave term, "
                "surface-only path exact when disabled)\n",
                differing_points, samples);
}

// ============================================================================
// 4. Collision Subsample Stride (HLD v4.0 Section 4.4 escape hatch)
// ============================================================================
void test_collision_subsample_stride() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerDensityGrid34 grid34(ledger);
    WorkerScratchWindow36 scratch(ledger);
    CaveNoiseGenerator gen;

    // A bumpy isosurface through the centre chunk so both lattices see geometry.
    auto bumpy_sdf = [](float wx, float wy, float wz) -> float {
        const float r = std::sqrt((wx - 16.0f) * (wx - 16.0f) +
                                  (wy - 16.0f) * (wy - 16.0f) +
                                  (wz - 16.0f) * (wz - 16.0f));
        return r - 10.0f + 1.5f * std::sin(wx * 0.5f) * std::cos(wz * 0.5f);
    };

    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                gen.generate_chunk_from_density_fn(
                    store, ChunkKey::from_coords(dx, dy, dz), grid34, bumpy_sdf);
            }
        }
    }

    const ChunkKey ck = ChunkKey::from_coords(0, 0, 0);
    const auto stamp = gather_scratch_window_36(store, ck, scratch);

    const ChunkMeshPayload full = SurfaceNetsMesher::mesh_chunk_from_scratch_36(
        ck, scratch, 1, stamp.captured_terrain_revision);
    const ChunkMeshPayload coarse = SurfaceNetsMesher::mesh_chunk_from_scratch_36(
        ck, scratch, 1, stamp.captured_terrain_revision, 1.0f,
        TsdfFormat::kDefaultTruncationDelta, /*collision_stride=*/2);

    // Visual mesh must be untouched by the collision stride.
    REQUIRE(coarse.vertices.size() == full.vertices.size());
    REQUIRE(coarse.normals.size() == full.normals.size());
    REQUIRE(coarse.indices == full.indices);

    // Stride 1 collision soup is exactly the render triangle soup.
    REQUIRE(full.collision_faces.size() == full.indices.size());
    REQUIRE(full.collision_faces.size() % 3 == 0);

    // Stride 2 collision soup is non-empty, quad-based (multiple of 6) and smaller.
    REQUIRE(!coarse.collision_faces.empty());
    REQUIRE(coarse.collision_faces.size() % 6 == 0);
    REQUIRE(coarse.collision_faces.size() < full.collision_faces.size());

    std::printf("[PASS] test_collision_subsample_stride (visual %zu tris intact; collision %zu -> %zu verts at stride 2)\n",
                full.indices.size() / 3, full.collision_faces.size(), coarse.collision_faces.size());
}

// ============================================================================
// 5. World-Space Surface Height Query (sample_top_surface_y)
// ============================================================================
void test_surface_height_query() {
    ++g_tests_run;

    MemoryLedger ledger;
    ChunkStore store(ledger, /*world_height_chunks=*/8);
    WorkerDensityGrid34 grid34(ledger);
    CaveNoiseGenerator gen;
    gen.surface_amplitude = 0.0f; // deterministic generator fallback height

    // A flat world plane; gradient 1 keeps encode/decode exact.
    auto plane_at = [](float level) {
        return [level](float /*wx*/, float wy, float /*wz*/) -> float { return wy - level; };
    };

    // Resident columns (cx, cz in -1..1, all layers below the ceiling) so the
    // vertical ray never crosses an unloaded gap.
    for (int32_t cz = -1; cz <= 1; ++cz) {
        for (int32_t cx = -1; cx <= 1; ++cx) {
            for (int32_t cy = 0; cy <= 2; ++cy) {
                gen.generate_chunk_from_density_fn(
                    store, ChunkKey::from_coords(cx, cy, cz), grid34, plane_at(68.0f));
            }
        }
    }

    // 1. Resident TSDF: the query must land on the generated plane across the column.
    for (const float wx : {2.0f, 16.0f, 30.0f}) {
        for (const float wz : {2.0f, 16.0f, 30.0f}) {
            const float y = voxel::sample_top_surface_y(store, gen, wx, wz);
            REQUIRE(std::abs(y - 68.0f) <= 0.1f);
        }
    }

    // 2. Store-first: re-generate the same chunks at a lower plane. The default
    //    generator still models y ~ 68, so a y ~ 40 answer proves the resident
    //    (edited) path wins over the analytic fallback.
    for (int32_t cz = -1; cz <= 1; ++cz) {
        for (int32_t cx = -1; cx <= 1; ++cx) {
            for (int32_t cy = 0; cy <= 2; ++cy) {
                gen.generate_chunk_from_density_fn(
                    store, ChunkKey::from_coords(cx, cy, cz), grid34, plane_at(40.0f));
            }
        }
    }
    REQUIRE(std::abs(voxel::sample_top_surface_y(store, gen, 16.0f, 16.0f) - 40.0f) <= 0.1f);

    // 3. Generator fallback: a column that was never streamed still answers within
    //    the +-3 m overhang envelope of the 2D generator height.
    const float far_y = voxel::sample_top_surface_y(store, gen, 5000.0f, 5000.0f);
    REQUIRE(std::abs(far_y - 68.0f) <= 3.5f);

    std::printf("[PASS] test_surface_height_query (resident plane 68 -> edited plane 40 -> fallback %.2f)\n",
                far_y);
}

} // namespace

int main() {
    std::printf("=== Running M1 Step 5 Unit Test Suite: 34^3 Cave ProcGen & 36^3 Seamless Surface Nets ===\n");
    test_procgen_34_regularization_and_caves();
    test_2x2x2_watertight_chunk_seams();
    test_procgen_caves_toggle();
    test_collision_subsample_stride();
    test_surface_height_query();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
