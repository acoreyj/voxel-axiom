#include "voxel/procgen_and_surface_nets.hpp"

namespace voxel {

// ============================================================================
// WorkerDensityGrid34 Implementation
// ============================================================================
WorkerDensityGrid34::WorkerDensityGrid34(MemoryLedger& ledger)
    : ledger_(ledger), density_(kTotalSamples, 0.0f) {
    reserved_ = ledger_.try_reserve(kBufferBytes, MemoryPoolId::WorkerScratch);
    assert(reserved_ && "WorkerDensityGrid34 reservation must succeed within budget");
}

WorkerDensityGrid34::~WorkerDensityGrid34() {
    if (reserved_) {
        ledger_.release(kBufferBytes, MemoryPoolId::WorkerScratch);
    }
}

// ============================================================================
// CaveNoiseGenerator Implementation
// ============================================================================
float CaveNoiseGenerator::smooth_noise_3d(float x, float y, float z, uint32_t s) noexcept {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float fz = std::floor(z);
    const int32_t ix = static_cast<int32_t>(fx);
    const int32_t iy = static_cast<int32_t>(fy);
    const int32_t iz = static_cast<int32_t>(fz);
    const float u = fade(x - fx);
    const float v = fade(y - fy);
    const float w = fade(z - fz);

    const float c000 = lattice_val(ix,     iy,     iz,     s);
    const float c100 = lattice_val(ix + 1, iy,     iz,     s);
    const float c010 = lattice_val(ix,     iy + 1, iz,     s);
    const float c110 = lattice_val(ix + 1, iy + 1, iz,     s);
    const float c001 = lattice_val(ix,     iy,     iz + 1, s);
    const float c101 = lattice_val(ix + 1, iy,     iz + 1, s);
    const float c011 = lattice_val(ix,     iy + 1, iz + 1, s);
    const float c111 = lattice_val(ix + 1, iy + 1, iz + 1, s);

    const float x00 = lerp(c000, c100, u);
    const float x10 = lerp(c010, c110, u);
    const float x01 = lerp(c001, c101, u);
    const float x11 = lerp(c011, c111, u);

    const float y0 = lerp(x00, x10, v);
    const float y1 = lerp(x01, x11, v);
    return lerp(y0, y1, w);
}

float CaveNoiseGenerator::evaluate_composite_density(float wx, float wy, float wz) const noexcept {
    const float h_2d = surface_base_height +
                       surface_amplitude * smooth_noise_3d(wx * 0.015f, 0.0f, wz * 0.015f, seed);
    const float overhang = 4.0f * smooth_noise_3d(wx * 0.04f, wy * 0.04f, wz * 0.04f, seed + 101U);
    const float d_surf = (wy - h_2d) + overhang;

    const float bedrock_atten = std::clamp((wy - 4.0f) / 12.0f, 0.0f, 1.0f);
    const float r_y = cave_radius * bedrock_atten;

    const float n_a = 18.0f * smooth_noise_3d(wx * 0.035f, wy * 0.035f, wz * 0.035f, seed + 202U);
    const float n_b = 18.0f * smooth_noise_3d(wx * 0.035f, wy * 0.035f, wz * 0.035f, seed + 303U);
    const float d_cave = r_y - std::sqrt(n_a * n_a + n_b * n_b);

    return std::max(d_surf, d_cave);
}

void CaveNoiseGenerator::generate_chunk(ChunkStore& store, ChunkKey chunk_key, WorkerDensityGrid34& grid34) const {
    generate_chunk_from_density_fn(
        store,
        chunk_key,
        grid34,
        [this](float wx, float wy, float wz) {
            return evaluate_composite_density(wx, wy, wz);
        });
}

// ============================================================================
// SurfaceNetsMesher Implementation
// ============================================================================
ChunkMeshPayload SurfaceNetsMesher::mesh_chunk_from_scratch_36(
    ChunkKey chunk_key,
    const WorkerScratchWindow36& scratch,
    uint64_t captured_version,
    uint64_t captured_terrain_revision,
    float voxel_scale,
    float truncation_delta) {

    ChunkMeshPayload out{};
    out.key = chunk_key;
    out.captured_version = captured_version;
    out.captured_terrain_revision = captured_terrain_revision;

    const IVec3 chunk_min = chunk_key.min_world_sample();
    const float origin_x = static_cast<float>(chunk_min.x) * voxel_scale;
    const float origin_y = static_cast<float>(chunk_min.y) * voxel_scale;
    const float origin_z = static_cast<float>(chunk_min.z) * voxel_scale;

    auto corner_gradient = [&](int32_t cx, int32_t cy, int32_t cz) noexcept -> Vec3f {
        assert(cx >= 0 && cx <= 33 && cy >= 0 && cy <= 33 && cz >= 0 && cz <= 33);
        const float gx = static_cast<float>(scratch.get_tsdf(cx + 1, cy, cz)) -
                         static_cast<float>(scratch.get_tsdf(cx - 1, cy, cz));
        const float gy = static_cast<float>(scratch.get_tsdf(cx, cy + 1, cz)) -
                         static_cast<float>(scratch.get_tsdf(cx, cy - 1, cz));
        const float gz = static_cast<float>(scratch.get_tsdf(cx, cy, cz + 1)) -
                         static_cast<float>(scratch.get_tsdf(cx, cy, cz - 1));
        return Vec3f{0.5f * gx, 0.5f * gy, 0.5f * gz};
    };

    auto cell_flat_index = [](int32_t i, int32_t j, int32_t k) noexcept -> std::size_t {
        assert(i >= 0 && i <= 32 && j >= 0 && j <= 32 && k >= 0 && k <= 32);
        return static_cast<std::size_t>(i + kCellGridDim * (j + kCellGridDim * k));
    };

    std::vector<DualCellData> dual_grid(kCellGridDim * kCellGridDim * kCellGridDim);

    static constexpr std::array<std::pair<int, int>, 12> kCubeEdges = {{
        {0, 1}, {2, 3}, {4, 5}, {6, 7}, // 4 edges along X
        {0, 2}, {1, 3}, {4, 6}, {5, 7}, // 4 edges along Y
        {0, 4}, {1, 5}, {2, 6}, {3, 7}  // 4 edges along Z
    }};

    // Step 1: Position dual vertices for all active cells in [0..32]^3 (33^3 cells).
    for (int32_t k = 0; k <= 32; ++k) {
        for (int32_t j = 0; j <= 32; ++j) {
            for (int32_t i = 0; i <= 32; ++i) {
                int16_t corner_q[8];
                uint8_t mask = 0;
                int16_t best_solid_q = INT16_MIN;
                uint8_t best_material = 0;

                for (int c = 0; c < 8; ++c) {
                    const int32_t cx = i + (c & 1);
                    const int32_t cy = j + ((c >> 1) & 1);
                    const int32_t cz = k + ((c >> 2) & 1);
                    const int16_t q = scratch.get_tsdf(cx, cy, cz);
                    corner_q[c] = q;
                    if (q < 0) {
                        mask |= static_cast<uint8_t>(1U << c);
                        if (q > best_solid_q) {
                            best_solid_q = q;
                            best_material = scratch.get_material(cx, cy, cz);
                        }
                    }
                }

                if (mask == 0U || mask == 0xFFU) {
                    continue;
                }

                float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f;
                int crossings = 0;

                for (const auto [c0, c1] : kCubeEdges) {
                    const bool s0 = ((mask >> c0) & 1U) != 0U;
                    const bool s1 = ((mask >> c1) & 1U) != 0U;
                    if (s0 == s1) {
                        continue;
                    }

                    const float f0 = TsdfFormat::decode(corner_q[c0], truncation_delta);
                    const float f1 = TsdfFormat::decode(corner_q[c1], truncation_delta);
                    const float denom = f1 - f0;
                    const float t = (std::abs(denom) > 1e-8f)
                                        ? std::clamp(-f0 / denom, 0.0f, 1.0f)
                                        : 0.5f;

                    const float p0x = static_cast<float>(c0 & 1);
                    const float p0y = static_cast<float>((c0 >> 1) & 1);
                    const float p0z = static_cast<float>((c0 >> 2) & 1);

                    const float p1x = static_cast<float>(c1 & 1);
                    const float p1y = static_cast<float>((c1 >> 1) & 1);
                    const float p1z = static_cast<float>((c1 >> 2) & 1);

                    sum_x += p0x + t * (p1x - p0x);
                    sum_y += p0y + t * (p1y - p0y);
                    sum_z += p0z + t * (p1z - p0z);
                    ++crossings;
                }

                assert(crossings > 0);
                const float inv_count = 1.0f / static_cast<float>(crossings);
                const float ux = sum_x * inv_count;
                const float uy = sum_y * inv_count;
                const float uz = sum_z * inv_count;

                Vec3f g[8];
                for (int c = 0; c < 8; ++c) {
                    g[c] = corner_gradient(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                }
                auto lerp_vec = [](Vec3f a, Vec3f b, float t) noexcept -> Vec3f {
                    return Vec3f{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
                };
                const Vec3f gx00 = lerp_vec(g[0], g[1], ux);
                const Vec3f gx10 = lerp_vec(g[2], g[3], ux);
                const Vec3f gx01 = lerp_vec(g[4], g[5], ux);
                const Vec3f gx11 = lerp_vec(g[6], g[7], ux);
                const Vec3f gy0 = lerp_vec(gx00, gx10, uy);
                const Vec3f gy1 = lerp_vec(gx01, gx11, uy);
                Vec3f n = lerp_vec(gy0, gy1, uz);

                const float n_len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (n_len > 1e-6f) {
                    n.x /= n_len;
                    n.y /= n_len;
                    n.z /= n_len;
                } else {
                    n = Vec3f{0.0f, 1.0f, 0.0f};
                }

                DualCellData& cell = dual_grid[cell_flat_index(i, j, k)];
                cell.local_pos = Vec3f{
                    origin_x + (static_cast<float>(i) + ux) * voxel_scale,
                    origin_y + (static_cast<float>(j) + uy) * voxel_scale,
                    origin_z + (static_cast<float>(k) + uz) * voxel_scale
                };
                cell.normal = n;
                cell.material_id = best_material;
            }
        }
    }

    auto get_or_emit_vertex = [&](int32_t ci, int32_t cj, int32_t ck) -> uint32_t {
        DualCellData& cell = dual_grid[cell_flat_index(ci, cj, ck)];
        if (cell.vertex_index == kInvalidVertex) {
            cell.vertex_index = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(cell.local_pos);
            out.normals.push_back(cell.normal);
        }
        return cell.vertex_index;
    };

    auto emit_quad = [&](int32_t c0x, int32_t c0y, int32_t c0z,
                         int32_t c1x, int32_t c1y, int32_t c1z,
                         int32_t c2x, int32_t c2y, int32_t c2z,
                         int32_t c3x, int32_t c3y, int32_t c3z,
                         bool flip_winding) {
        const uint32_t v0 = get_or_emit_vertex(c0x, c0y, c0z);
        const uint32_t v1 = get_or_emit_vertex(c1x, c1y, c1z);
        const uint32_t v2 = get_or_emit_vertex(c2x, c2y, c2z);
        const uint32_t v3 = get_or_emit_vertex(c3x, c3y, c3z);

        const uint32_t tri0_a = v0;
        const uint32_t tri0_b = flip_winding ? v2 : v1;
        const uint32_t tri0_c = flip_winding ? v1 : v2;

        const uint32_t tri1_a = v0;
        const uint32_t tri1_b = flip_winding ? v3 : v2;
        const uint32_t tri1_c = flip_winding ? v2 : v3;

        out.indices.push_back(static_cast<int32_t>(tri0_a));
        out.indices.push_back(static_cast<int32_t>(tri0_b));
        out.indices.push_back(static_cast<int32_t>(tri0_c));

        out.indices.push_back(static_cast<int32_t>(tri1_a));
        out.indices.push_back(static_cast<int32_t>(tri1_b));
        out.indices.push_back(static_cast<int32_t>(tri1_c));

        out.collision_faces.push_back(out.vertices[tri0_a]);
        out.collision_faces.push_back(out.vertices[tri0_b]);
        out.collision_faces.push_back(out.vertices[tri0_c]);

        out.collision_faces.push_back(out.vertices[tri1_a]);
        out.collision_faces.push_back(out.vertices[tri1_b]);
        out.collision_faces.push_back(out.vertices[tri1_c]);
    };

    // Step 2: Quad Stitching across owned interior edges for cells (i, j, k) in [0..31]^3.
    for (int32_t k = 0; k < 32; ++k) {
        for (int32_t j = 0; j < 32; ++j) {
            for (int32_t i = 0; i < 32; ++i) {
                const bool s_tip = (scratch.get_tsdf(i + 1, j + 1, k + 1) < 0);

                // 1. +X edge: (i, j+1, k+1) -> (i+1, j+1, k+1)
                {
                    const bool s_tail = (scratch.get_tsdf(i, j + 1, k + 1) < 0);
                    if (s_tail != s_tip) {
                        emit_quad(i, j,     k,
                                  i, j + 1, k,
                                  i, j + 1, k + 1,
                                  i, j,     k + 1,
                                  /*flip_winding=*/s_tip);
                    }
                }

                // 2. +Y edge: (i+1, j, k+1) -> (i+1, j+1, k+1)
                {
                    const bool s_tail = (scratch.get_tsdf(i + 1, j, k + 1) < 0);
                    if (s_tail != s_tip) {
                        emit_quad(i,     j, k,
                                  i,     j, k + 1,
                                  i + 1, j, k + 1,
                                  i + 1, j, k,
                                  /*flip_winding=*/s_tip);
                    }
                }

                // 3. +Z edge: (i+1, j+1, k) -> (i+1, j+1, k+1)
                {
                    const bool s_tail = (scratch.get_tsdf(i + 1, j + 1, k) < 0);
                    if (s_tail != s_tip) {
                        emit_quad(i,     j,     k,
                                  i + 1, j,     k,
                                  i + 1, j + 1, k,
                                  i,     j + 1, k,
                                  /*flip_winding=*/s_tip);
                    }
                }
            }
        }
    }

    return out;
}

} // namespace voxel
