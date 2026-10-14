#pragma once

#include "edit_and_worker_pool.hpp"
#include "terrain_raycast.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace voxel {

// ============================================================================
// 1. Single-Pass 34^3 Grid Procedural Cave & Terrain Generator (Section 3.5.2)
// ============================================================================

/// Pre-allocated thread-local 34^3 ([-1..32] on each axis) float density buffer
/// for single-pass uniform grid evaluation and in-memory central differences.
/// Accounted under MemoryPoolId::WorkerScratch in MemoryLedger (Section 3.3, 3.5.2).
class WorkerDensityGrid34 {
public:
    static constexpr int32_t kMinLocal = -1;
    static constexpr int32_t kMaxLocal = 32;
    static constexpr int32_t kDim = 34; // -1 .. 32 inclusive
    static constexpr std::size_t kTotalSamples = 34ULL * 34ULL * 34ULL; // 39,304 floats
    static constexpr std::size_t kBufferBytes = kTotalSamples * sizeof(float); // 157,216 B (~153.5 KiB)

    explicit WorkerDensityGrid34(MemoryLedger& ledger)
        : ledger_(ledger), density_(kTotalSamples, 0.0f) {
        reserved_ = ledger_.try_reserve(kBufferBytes, MemoryPoolId::WorkerScratch);
        assert(reserved_ && "WorkerDensityGrid34 reservation must succeed within budget");
    }

    ~WorkerDensityGrid34() {
        if (reserved_) {
            ledger_.release(kBufferBytes, MemoryPoolId::WorkerScratch);
        }
    }

    WorkerDensityGrid34(const WorkerDensityGrid34&) = delete;
    WorkerDensityGrid34& operator=(const WorkerDensityGrid34&) = delete;

    [[nodiscard]] static constexpr bool in_bounds(int32_t lx, int32_t ly, int32_t lz) noexcept {
        return (lx >= kMinLocal && lx <= kMaxLocal) &&
               (ly >= kMinLocal && ly <= kMaxLocal) &&
               (lz >= kMinLocal && lz <= kMaxLocal);
    }

    [[nodiscard]] static constexpr std::size_t linear_index(int32_t lx, int32_t ly, int32_t lz) noexcept {
        assert(in_bounds(lx, ly, lz));
        const std::size_t ux = static_cast<std::size_t>(lx - kMinLocal);
        const std::size_t uy = static_cast<std::size_t>(ly - kMinLocal);
        const std::size_t uz = static_cast<std::size_t>(lz - kMinLocal);
        return ux + kDim * (uy + kDim * uz);
    }

    [[nodiscard]] float get(int32_t lx, int32_t ly, int32_t lz) const noexcept {
        return density_[linear_index(lx, ly, lz)];
    }

    void set(int32_t lx, int32_t ly, int32_t lz, float val) noexcept {
        density_[linear_index(lx, ly, lz)] = val;
    }

    [[nodiscard]] float* data() noexcept { return density_.data(); }
    [[nodiscard]] const float* data() const noexcept { return density_.data(); }

private:
    MemoryLedger& ledger_;
    bool reserved_ = false;
    std::vector<float> density_;
};

/// Fast deterministic 3D gradient / value noise helper for standalone SIMD-friendly
/// uniform grid evaluation when external FastNoise2 DLL is not linked in unit tests.
struct CaveNoiseGenerator {
    uint32_t seed = 1337U;
    float surface_base_height = 68.0f;
    float surface_amplitude = 14.0f;
    float cave_radius = 4.2f;
    float voxel_scale = 1.0f;
    float truncation_delta = TsdfFormat::kDefaultTruncationDelta;
    /// When false, `evaluate_composite_density` returns the surface term only and
    /// skips the 3D dual-noise cave term entirely (cave-free terrain). Read once per
    /// sample, so toggling it affects only chunks generated afterwards.
    bool caves_enabled = true;

    // Prominent 3-hit Hard Rock boulder right in front of the demo camera at (16, 70, 24)
    Vec3f demo_hard_boulder_center{16.0f, 70.0f, 24.0f};
    float demo_hard_boulder_radius = 7.5f;

    static constexpr float kEpsilonGrad = 0.05f; // Regularization floor (Section 3.5.2)

    [[nodiscard]] static constexpr float fade(float t) noexcept {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    [[nodiscard]] static constexpr float lerp(float a, float b, float t) noexcept {
        return a + t * (b - a);
    }

    [[nodiscard]] static constexpr float lattice_val(int32_t ix, int32_t iy, int32_t iz, uint32_t s) noexcept {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(ix)) * 0x9E3779B185EBCA87ULL ^
                     static_cast<uint64_t>(static_cast<uint32_t>(iy)) * 0xC2B2AE3D27D4EB4FULL ^
                     static_cast<uint64_t>(static_cast<uint32_t>(iz)) * 0x165667B19E3779F9ULL ^
                     static_cast<uint64_t>(s) * 0x27D4EB2F165667C5ULL;
        h = ChunkKey::hash_raw(h);
        const float unit = static_cast<float>(h & 0xFFFFFFULL) / static_cast<float>(0x800000ULL);
        return unit - 1.0f;
    }

    [[nodiscard]] static float smooth_noise_3d(float x, float y, float z, uint32_t s) noexcept {
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

    [[nodiscard]] float hard_boulder_sdf(float wx, float wy, float wz) const noexcept {
        const float dx = wx - demo_hard_boulder_center.x;
        const float dy = wy - demo_hard_boulder_center.y;
        const float dz = wz - demo_hard_boulder_center.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz) - demo_hard_boulder_radius;
    }

    /// Returns true if world position (wx, wy, wz) belongs to a 3-hit Hard Rock formation
    /// (either the showcase boulder in front of the camera or procedural hard-rock outcrops).
    [[nodiscard]] bool is_hard_rock_region(float wx, float wy, float wz) const noexcept {
        if (hard_boulder_sdf(wx, wy, wz) <= 1.5f) {
            return true;
        }
        const float vein = smooth_noise_3d(wx * 0.045f, wy * 0.045f, wz * 0.045f, seed + 707U);
        return vein > 0.38f;
    }

    /// Evaluates composite surface + 3D dual-noise spaghetti cave density D(x, y, z)
    /// (Negative = solid, Positive = air) per HLD v4.0 Section 3.5.2.
    [[nodiscard]] float evaluate_composite_density(float wx, float wy, float wz) const noexcept {
        // 1. 2D surface height + 3D overhang term: D_surf(x,y,z) = (y - H_2D(x,z)) + N_3D_overhang(x,y,z)
        const float h_2d = surface_base_height +
                           surface_amplitude * smooth_noise_3d(wx * 0.012f, 0.0f, wz * 0.012f, seed);
        const float overhang = 3.0f * smooth_noise_3d(wx * 0.03f, wy * 0.03f, wz * 0.03f, seed + 101U);
        const float d_surf = (wy - h_2d) + overhang;

        // Cave generation is optional: when disabled, the terrain is the pure
        // 2D surface + overhang field with no 3D cavities carved into it.
        if (!caves_enabled) {
            return std::min(d_surf, hard_boulder_sdf(wx, wy, wz));
        }

        // 2. 3D Dual-Noise Ridge Intersection Cave Density:
        //    D_cave(x,y,z) = r_cave(y) - sqrt(N_A(x,y,z)^2 + N_B(x,y,z)^2)
        //    Attenuate r_cave(y) near y = 0 bedrock so caves never breach the bottom of the world,
        //    and taper near the surface crust so the hills stay intact rather than swiss-cheesed.
        const float bedrock_atten = std::clamp((wy - 4.0f) / 10.0f, 0.0f, 1.0f);
        const float roof_atten = std::clamp((h_2d - 6.0f - wy) / 10.0f, 0.0f, 1.0f);
        const float r_y = cave_radius * bedrock_atten * roof_atten;

        const float n_a = 20.0f * smooth_noise_3d(wx * 0.022f, wy * 0.028f, wz * 0.022f, seed + 202U);
        const float n_b = 20.0f * smooth_noise_3d(wx * 0.022f, wy * 0.028f, wz * 0.022f, seed + 303U);
        const float d_cave = r_y - std::sqrt(n_a * n_a + n_b * n_b);

        // 3. Combined density: D(x,y,z) = min(max(D_surf, D_cave), D_boulder)
        const float terrain_and_caves = std::max(d_surf, d_cave);
        return std::min(terrain_and_caves, hard_boulder_sdf(wx, wy, wz));
    }

    /// Pass 2 of chunk population: converts an already-filled 34^3 ([-1..32]^3) density
    /// grid into the canonical 32^3 TSDF samples via in-memory central differences and
    /// regularized gradient normalization max(||grad D||, 0.05), then publishes the slot
    /// as Resident (HLD v4.0 Section 3.5.2). Shared by the generic density-fn path and the
    /// cached composite-density path so both stay bit-identical.
    /// @param mark_hard_rock  When true, solid samples inside a Hard Rock formation
    ///   (showcase boulder or procedural hard-rock veins) are tagged
    ///   `MaterialHardness::kMaterialHardRock` so explosions crack them over 3 hits
    ///   instead of shattering them on the first.
    void encode_chunk_from_density_grid(
        ChunkStore& store,
        ChunkKey chunk_key,
        WorkerDensityGrid34& grid34,
        uint8_t base_material = MaterialHardness::kMaterialSoil,
        bool mark_hard_rock = false) const {

        ChunkSlot* slot = store.get_or_create_slot(chunk_key);
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(ChunkStorageForm::Solid, base_material, store.tsdf_pool(), store.material_pool());
        REQUIRE_OR_ASSERT(slot->promote_to_dense(store.tsdf_pool()));

        const IVec3 chunk_min = chunk_key.min_world_sample();
        const float inv_2s = 1.0f / (2.0f * voxel_scale);
        for (int32_t lz = 0; lz < 32; ++lz) {
            for (int32_t ly = 0; ly < 32; ++ly) {
                for (int32_t lx = 0; lx < 32; ++lx) {
                    const float d_val = grid34.get(lx, ly, lz);
                    const float gx = (grid34.get(lx + 1, ly, lz) - grid34.get(lx - 1, ly, lz)) * inv_2s;
                    const float gy = (grid34.get(lx, ly + 1, lz) - grid34.get(lx, ly - 1, lz)) * inv_2s;
                    const float gz = (grid34.get(lx, ly, lz + 1) - grid34.get(lx, ly, lz - 1)) * inv_2s;

                    const float grad_norm = std::sqrt(gx * gx + gy * gy + gz * gz);
                    const float reg_grad = std::max(grad_norm, kEpsilonGrad);
                    const float sdf_approx =
                        std::clamp(d_val / (reg_grad * voxel_scale), -truncation_delta, +truncation_delta);

                    const int16_t q = TsdfFormat::encode(sdf_approx, truncation_delta);
                    const LocalCoord lc{lx, ly, lz};
                    (void)slot->set_tsdf(lc, q, store.tsdf_pool());

                    if (mark_hard_rock && q < 0) {
                        const float wx = static_cast<float>(chunk_min.x + lx) * voxel_scale;
                        const float wy = static_cast<float>(chunk_min.y + ly) * voxel_scale;
                        const float wz = static_cast<float>(chunk_min.z + lz) * voxel_scale;
                        if (is_hard_rock_region(wx, wy, wz)) {
                            (void)slot->set_material(
                                lc, MaterialHardness::kMaterialHardRock, store.material_pool());
                        }
                    }
                }
            }
        }

        // Compact back to 0-byte EMPTY or SOLID if all 32^3 canonical samples are clamped uniform.
        slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
        slot->version.store(1, std::memory_order_release);
        slot->state = ChunkLifecycleState::Resident;
    }

    /// Populates the 34^3 ([-1..32]^3) uniform grid in a single pass from an arbitrary
    /// density function, then hands it to `encode_chunk_from_density_grid` for the
    /// canonical 32^3 central-difference/TSDF pass (HLD v4.0 Section 3.5.2).
    template <typename DensityFn>
    void generate_chunk_from_density_fn(
        ChunkStore& store,
        ChunkKey chunk_key,
        WorkerDensityGrid34& grid34,
        DensityFn&& density_fn,
        uint8_t base_material = MaterialHardness::kMaterialSoil) const {

        const IVec3 chunk_min = chunk_key.min_world_sample();

        // Pass 1: Single-pass 34^3 grid evaluation over local [-1..32]^3.
        for (int32_t lz = -1; lz <= 32; ++lz) {
            const float wz = static_cast<float>(chunk_min.z + lz) * voxel_scale;
            for (int32_t ly = -1; ly <= 32; ++ly) {
                const float wy = static_cast<float>(chunk_min.y + ly) * voxel_scale;
                for (int32_t lx = -1; lx <= 32; ++lx) {
                    const float wx = static_cast<float>(chunk_min.x + lx) * voxel_scale;
                    grid34.set(lx, ly, lz, density_fn(wx, wy, wz));
                }
            }
        }

        // Pass 2: In-memory central differences over [0..31]^3 with epsilon_grad = 0.05 floor.
        encode_chunk_from_density_grid(store, chunk_key, grid34, base_material);
    }

    /// Fast path for the built-in composite surface/cave generator.
    /// Audit Finding F1: precomputes the 34x34 2D surface height field once per chunk
    /// instead of re-evaluating it for all 34 Y-slices. This is an exact refactor of
    /// `evaluate_composite_density` (F1a), so generated terrain is bit-identical — the
    /// `caves_enabled` toggle is honoured exactly as before.
    void generate_chunk(ChunkStore& store, ChunkKey chunk_key, WorkerDensityGrid34& grid34) const {
        const IVec3 chunk_min = chunk_key.min_world_sample();

        float h_2d_cache[34][34];
        for (int32_t lz = -1; lz <= 32; ++lz) {
            const float wz = static_cast<float>(chunk_min.z + lz) * voxel_scale;
            for (int32_t lx = -1; lx <= 32; ++lx) {
                const float wx = static_cast<float>(chunk_min.x + lx) * voxel_scale;
                h_2d_cache[lx + 1][lz + 1] =
                    surface_base_height +
                    surface_amplitude * smooth_noise_3d(wx * 0.012f, 0.0f, wz * 0.012f, seed);
            }
        }

        for (int32_t lz = -1; lz <= 32; ++lz) {
            const float wz = static_cast<float>(chunk_min.z + lz) * voxel_scale;
            for (int32_t ly = -1; ly <= 32; ++ly) {
                const float wy = static_cast<float>(chunk_min.y + ly) * voxel_scale;
                const float bedrock_atten = std::clamp((wy - 4.0f) / 10.0f, 0.0f, 1.0f);

                for (int32_t lx = -1; lx <= 32; ++lx) {
                    const float wx = static_cast<float>(chunk_min.x + lx) * voxel_scale;
                    const float h_2d = h_2d_cache[lx + 1][lz + 1];

                    const float overhang = 3.0f * smooth_noise_3d(wx * 0.03f, wy * 0.03f, wz * 0.03f, seed + 101U);
                    const float d_surf = (wy - h_2d) + overhang;

                    // Cave generation is optional: when disabled, the terrain is the pure
                    // 2D surface + overhang field with no 3D cavities carved into it.
                    if (!caves_enabled) {
                        grid34.set(lx, ly, lz, std::min(d_surf, hard_boulder_sdf(wx, wy, wz)));
                        continue;
                    }

                    const float roof_atten = std::clamp((h_2d - 6.0f - wy) / 10.0f, 0.0f, 1.0f);
                    const float r_y = cave_radius * bedrock_atten * roof_atten;

                    const float n_a = 20.0f * smooth_noise_3d(wx * 0.022f, wy * 0.028f, wz * 0.022f, seed + 202U);
                    const float n_b = 20.0f * smooth_noise_3d(wx * 0.022f, wy * 0.028f, wz * 0.022f, seed + 303U);
                    const float d_cave = r_y - std::sqrt(n_a * n_a + n_b * n_b);

                    grid34.set(lx, ly, lz, std::min(std::max(d_surf, d_cave), hard_boulder_sdf(wx, wy, wz)));
                }
            }
        }

        encode_chunk_from_density_grid(
            store, chunk_key, grid34, MaterialHardness::kMaterialSoil, /*mark_hard_rock=*/true);
    }

private:
    static void REQUIRE_OR_ASSERT(bool ok) noexcept {
        assert(ok);
        (void)ok;
    }
};

/// World-space top-surface height query for gameplay (spawn points, patrols).
///
/// Casts a vertical ray against the resident TSDF column under (wx, wz) via
/// `raycast_terrain` and returns the world-space y of the top-most air -> solid
/// crossing: the surface a character would stand on (overhangs resolve to the
/// highest crossing). The procedural generator is evaluated as a fallback whenever
/// the ray finds no crossing (column not streamed, ray swallowed by a missing
/// chunk, or terrain taller than the world ceiling), so edited terrain is
/// reflected wherever chunks are streamed, and pristine terrain everywhere else.
///
/// Main-thread only: takes a shared slot lock per sampled chunk.
[[nodiscard]] inline float sample_top_surface_y(
    const ChunkStore& store,
    const CaveNoiseGenerator& procgen,
    float wx,
    float wz) {

    const float ceiling = static_cast<float>(store.world_height_chunks() * ChunkKey::kChunkDim) - 1.0f;
    if (const std::optional<Vec3f> hit =
            raycast_terrain(store, Vec3f{wx, ceiling, wz}, Vec3f{wx, 0.0f, wz})) {
        return hit->y;
    }

    // Analytic fallback on the procedural generator. The +-3 m overhang term means
    // the surface sits within +-3 m of the 2D height, so 4 m above it is safe air.
    const float h_2d = procgen.surface_base_height +
                       procgen.surface_amplitude *
                           CaveNoiseGenerator::smooth_noise_3d(wx * 0.012f, 0.0f, wz * 0.012f, procgen.seed);
    const float step = 0.5f;
    float air_y = std::min(h_2d + 4.0f, ceiling);
    float solid_y = air_y - step;
    while (solid_y > 0.0f && procgen.evaluate_composite_density(wx, solid_y, wz) >= 0.0f) {
        air_y = solid_y;
        solid_y -= step;
    }
    if (solid_y <= 0.0f) {
        return 0.0f; // all air down to bedrock; nothing to stand on.
    }
    for (int i = 0; i < 12; ++i) {
        const float mid = 0.5f * (air_y + solid_y);
        if (procgen.evaluate_composite_density(wx, mid, wz) < 0.0f) {
            solid_y = mid;
        } else {
            air_y = mid;
        }
    }
    return 0.5f * (air_y + solid_y);
}

// ============================================================================
// 2. Seamless Surface Nets Mesher over the 36^3 Scratch Window (Section 3.5.3)
// ============================================================================
class SurfaceNetsMesher {
public:
    static constexpr int32_t kCornerGridDim = 34; // Corners [0..33]^3
    static constexpr int32_t kCellGridDim   = 33; // Dual cells [0..32]^3
    static constexpr uint32_t kInvalidVertex = UINT32_MAX;

    struct DualCellData {
        uint32_t vertex_index = kInvalidVertex;
        Vec3f local_pos{};
        Vec3f normal{};
        uint8_t material_id = 0;
        uint8_t damage = 0;
    };

    /// Meshes chunk `chunk_key` from its populated 36^3 (`-1..34`) scratch window:
    ///   1. Computes central-difference corner gradients over [0..33]^3 using [-1..34]^3.
    ///   2. Computes dual vertices for all active cells in [0..32]^3 (33^3 cells).
    ///   3. Stitches quads for sign-changing interior edges owned by cells [0..31]^3,
    ///      using Godot 4 clockwise front-face winding so `cull_back` renders solid exteriors.
    ///   4. Emits indexed render arrays AND pre-flattened triangle soup for Physics/Navigation.
    /// @param collision_stride  When > 1, the de-indexed physics/navigation triangle
    ///   soup is rebuilt from a stride-N subsample of the same scratch window (HLD
    ///   v4.0 Section 4.4 escape hatch), while the visual mesh stays full resolution.
    ///   Stride 2 halves the collision lattice to 16^3 (~4x fewer physics triangles).
    ///   Only 1 or 2 are seamless: the 36^3 window reaches just two voxels into the
    ///   neighbouring chunk, so a larger stride would crack collision across seams.
    [[nodiscard]] static ChunkMeshPayload mesh_chunk_from_scratch_36(
        ChunkKey chunk_key,
        const WorkerScratchWindow36& scratch,
        uint64_t captured_version = 1,
        uint64_t captured_terrain_revision = 0,
        float voxel_scale = 1.0f,
        float truncation_delta = TsdfFormat::kDefaultTruncationDelta,
        int32_t collision_stride = 1) {

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
                    uint8_t max_solid_damage = 0;

                    for (int c = 0; c < 8; ++c) {
                        const int32_t cx = i + (c & 1);
                        const int32_t cy = j + ((c >> 1) & 1);
                        const int32_t cz = k + ((c >> 2) & 1);
                        const int16_t q = scratch.get_tsdf(cx, cy, cz);
                        corner_q[c] = q;
                        if (q < 0) {
                            mask |= static_cast<uint8_t>(1U << c);
                            max_solid_damage = std::max(max_solid_damage, scratch.get_damage(cx, cy, cz));
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

                    for (const auto& [c0, c1] : kCubeEdges) {
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
                    cell.damage = max_solid_damage;
                }
            }
        }

        auto get_or_emit_vertex = [&](int32_t ci, int32_t cj, int32_t ck) -> uint32_t {
            DualCellData& cell = dual_grid[cell_flat_index(ci, cj, ck)];
            if (cell.vertex_index == kInvalidVertex) {
                cell.vertex_index = static_cast<uint32_t>(out.vertices.size());
                out.vertices.push_back(cell.local_pos);
                out.normals.push_back(cell.normal);
                // Pack damage ratio in [0, 1] into colors.x (R) and hard-rock flag into colors.y (G)
                const float dmg_ratio = static_cast<float>(cell.damage) / 255.0f;
                const float is_hard_rock =
                    (cell.material_id == MaterialHardness::kMaterialHardRock) ? 1.0f : 0.0f;
                out.colors.push_back(Vec3f{dmg_ratio, is_hard_rock, 0.0f});
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

            // At stride 1 the collision soup is exactly the render triangle soup.
            // A larger stride rebuilds it from a coarser lattice after stitching.
            if (collision_stride <= 1) {
                out.collision_faces.push_back(out.vertices[tri0_a]);
                out.collision_faces.push_back(out.vertices[tri0_b]);
                out.collision_faces.push_back(out.vertices[tri0_c]);

                out.collision_faces.push_back(out.vertices[tri1_a]);
                out.collision_faces.push_back(out.vertices[tri1_b]);
                out.collision_faces.push_back(out.vertices[tri1_c]);
            }
        };

        // Step 2: Quad Stitching across owned interior edges for cells (i, j, k) in [0..31]^3.
        // Note on Godot 4 Front-Face Winding:
        // Godot's RenderingServer considers a triangle front-facing when its vertices are
        // ordered clockwise when viewed from outside (against the outward normal +grad S).
        // Therefore `flip_winding = !s_tip` winds front faces toward the air side (+grad S).
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
                                      /*flip_winding=*/!s_tip);
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
                                      /*flip_winding=*/!s_tip);
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
                                      /*flip_winding=*/!s_tip);
                        }
                    }
                }
            }
        }

        if (collision_stride > 1) {
            append_collision_soup_at_stride(
                scratch, chunk_key, collision_stride, voxel_scale, truncation_delta,
                out.collision_faces);
        }

        return out;
    }

private:
    /// HLD v4.0 Section 4.4 escape hatch: de-indexed triangle soup for physics and
    /// navigation built from a stride-N subsample of the 36^3 scratch window. This
    /// is the same Surface Nets stitching as the full-resolution pass, evaluated on
    /// a coarse lattice whose cells span `stride` voxels on every axis, so adjacent
    /// chunks still agree on the shared boundary samples (no collision seam crack).
    /// Roughly stride^3 fewer triangles: 4x at stride 2, 16x at stride 4.
    static void append_collision_soup_at_stride(
        const WorkerScratchWindow36& scratch,
        ChunkKey chunk_key,
        int32_t stride,
        float voxel_scale,
        float truncation_delta,
        std::vector<Vec3f>& out_soup) {

        if (stride <= 1 || (ChunkKey::kChunkDim % stride) != 0) {
            return;
        }

        const IVec3 chunk_min = chunk_key.min_world_sample();
        const float origin_x = static_cast<float>(chunk_min.x) * voxel_scale;
        const float origin_y = static_cast<float>(chunk_min.y) * voxel_scale;
        const float origin_z = static_cast<float>(chunk_min.z) * voxel_scale;

        const int32_t cells_per_axis = ChunkKey::kChunkDim / stride; // 16 at stride 2
        const int32_t dual_dim = cells_per_axis + 1;

        struct CoarseCell {
            Vec3f pos{};
        };
        std::vector<CoarseCell> dual(
            static_cast<std::size_t>(dual_dim) * dual_dim * dual_dim);

        auto cell_index = [dual_dim](int32_t gi, int32_t gj, int32_t gk) noexcept -> std::size_t {
            return static_cast<std::size_t>(gi + dual_dim * (gj + dual_dim * gk));
        };

        static constexpr std::array<std::pair<int, int>, 12> kCubeEdges = {{
            {0, 1}, {2, 3}, {4, 5}, {6, 7},
            {0, 2}, {1, 3}, {4, 6}, {5, 7},
            {0, 4}, {1, 5}, {2, 6}, {3, 7}
        }};

        // Step 1: Position coarse dual vertices for lattice cells [0 .. cells_per_axis].
        for (int32_t gk = 0; gk < dual_dim; ++gk) {
            for (int32_t gj = 0; gj < dual_dim; ++gj) {
                for (int32_t gi = 0; gi < dual_dim; ++gi) {
                    const int32_t i = gi * stride;
                    const int32_t j = gj * stride;
                    const int32_t k = gk * stride;

                    int16_t corner_q[8];
                    uint8_t mask = 0;
                    for (int c = 0; c < 8; ++c) {
                        const int32_t cx = i + ((c & 1) ? stride : 0);
                        const int32_t cy = j + (((c >> 1) & 1) ? stride : 0);
                        const int32_t cz = k + (((c >> 2) & 1) ? stride : 0);
                        const int16_t q = scratch.get_tsdf(cx, cy, cz);
                        corner_q[c] = q;
                        if (q < 0) {
                            mask |= static_cast<uint8_t>(1U << c);
                        }
                    }

                    if (mask == 0U || mask == 0xFFU) {
                        continue;
                    }

                    float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f;
                    int crossings = 0;
                    for (const auto& [c0, c1] : kCubeEdges) {
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
                        const float p0x = (c0 & 1) ? static_cast<float>(stride) : 0.0f;
                        const float p0y = ((c0 >> 1) & 1) ? static_cast<float>(stride) : 0.0f;
                        const float p0z = ((c0 >> 2) & 1) ? static_cast<float>(stride) : 0.0f;
                        const float p1x = (c1 & 1) ? static_cast<float>(stride) : 0.0f;
                        const float p1y = ((c1 >> 1) & 1) ? static_cast<float>(stride) : 0.0f;
                        const float p1z = ((c1 >> 2) & 1) ? static_cast<float>(stride) : 0.0f;
                        sum_x += p0x + t * (p1x - p0x);
                        sum_y += p0y + t * (p1y - p0y);
                        sum_z += p0z + t * (p1z - p0z);
                        ++crossings;
                    }
                    if (crossings == 0) {
                        continue;
                    }
                    const float inv = 1.0f / static_cast<float>(crossings);
                    const float ux = sum_x * inv;
                    const float uy = sum_y * inv;
                    const float uz = sum_z * inv;

                    dual[cell_index(gi, gj, gk)].pos = Vec3f{
                        origin_x + (static_cast<float>(i) + ux) * voxel_scale,
                        origin_y + (static_cast<float>(j) + uy) * voxel_scale,
                        origin_z + (static_cast<float>(k) + uz) * voxel_scale
                    };
                }
            }
        }

        auto cell_pos = [&](int32_t gi, int32_t gj, int32_t gk) -> const Vec3f& {
            return dual[cell_index(gi, gj, gk)].pos;
        };

        auto emit_quad = [&](int32_t a0, int32_t a1, int32_t a2,
                             int32_t b0, int32_t b1, int32_t b2,
                             int32_t c0, int32_t c1, int32_t c2,
                             int32_t d0, int32_t d1, int32_t d2,
                             bool flip_winding) {
            const Vec3f& v0 = cell_pos(a0, a1, a2);
            const Vec3f& v1 = cell_pos(b0, b1, b2);
            const Vec3f& v2 = cell_pos(c0, c1, c2);
            const Vec3f& v3 = cell_pos(d0, d1, d2);

            const Vec3f& t0b = flip_winding ? v2 : v1;
            const Vec3f& t0c = flip_winding ? v1 : v2;
            const Vec3f& t1b = flip_winding ? v3 : v2;
            const Vec3f& t1c = flip_winding ? v2 : v3;

            out_soup.push_back(v0);  out_soup.push_back(t0b); out_soup.push_back(t0c);
            out_soup.push_back(v0);  out_soup.push_back(t1b); out_soup.push_back(t1c);
        };

        // Step 2: Stitch quads across owned coarse interior edges (cells [0 .. cells_per_axis-1]).
        for (int32_t gk = 0; gk < cells_per_axis; ++gk) {
            for (int32_t gj = 0; gj < cells_per_axis; ++gj) {
                for (int32_t gi = 0; gi < cells_per_axis; ++gi) {
                    const int32_t i = gi * stride;
                    const int32_t j = gj * stride;
                    const int32_t k = gk * stride;

                    const bool s_tip = (scratch.get_tsdf(i + stride, j + stride, k + stride) < 0);

                    // +X edge: (i, j+stride, k+stride) -> (i+stride, j+stride, k+stride)
                    {
                        const bool s_tail = (scratch.get_tsdf(i, j + stride, k + stride) < 0);
                        if (s_tail != s_tip) {
                            emit_quad(gi, gj,     gk,
                                      gi, gj + 1, gk,
                                      gi, gj + 1, gk + 1,
                                      gi, gj,     gk + 1,
                                      /*flip_winding=*/!s_tip);
                        }
                    }

                    // +Y edge: (i+stride, j, k+stride) -> (i+stride, j+stride, k+stride)
                    {
                        const bool s_tail = (scratch.get_tsdf(i + stride, j, k + stride) < 0);
                        if (s_tail != s_tip) {
                            emit_quad(gi,     gj, gk,
                                      gi,     gj, gk + 1,
                                      gi + 1, gj, gk + 1,
                                      gi + 1, gj, gk,
                                      /*flip_winding=*/!s_tip);
                        }
                    }

                    // +Z edge: (i+stride, j+stride, k) -> (i+stride, j+stride, k+stride)
                    {
                        const bool s_tail = (scratch.get_tsdf(i + stride, j + stride, k) < 0);
                        if (s_tail != s_tip) {
                            emit_quad(gi,     gj,     gk,
                                      gi + 1, gj,     gk,
                                      gi + 1, gj + 1, gk,
                                      gi,     gj + 1, gk,
                                      /*flip_winding=*/!s_tip);
                        }
                    }
                }
            }
        }
    }
};

} // namespace voxel
