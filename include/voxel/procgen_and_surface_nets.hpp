#pragma once

#include "voxel/edit_and_worker_pool.hpp"

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

    explicit WorkerDensityGrid34(MemoryLedger& ledger);
    ~WorkerDensityGrid34();

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
    float surface_base_height = 96.0f;
    float surface_amplitude = 18.0f;
    float cave_radius = 4.5f;
    float voxel_scale = 1.0f;
    float truncation_delta = TsdfFormat::kDefaultTruncationDelta;

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

    [[nodiscard]] static float smooth_noise_3d(float x, float y, float z, uint32_t s) noexcept;

    /// Evaluates composite surface + 3D dual-noise spaghetti cave density D(x, y, z)
    /// (Negative = solid, Positive = air) per HLD v4.0 Section 3.5.2.
    [[nodiscard]] float evaluate_composite_density(float wx, float wy, float wz) const noexcept;

    /// Populates the 34^3 ([-1..32]^3) uniform grid in a single pass, then computes
    /// canonical 32^3 TSDF samples via in-memory central differences and regularized
    /// gradient normalization max(||grad D||, 0.05) (HLD v4.0 Section 3.5.2).
    template <typename DensityFn>
    void generate_chunk_from_density_fn(
        ChunkStore& store,
        ChunkKey chunk_key,
        WorkerDensityGrid34& grid34,
        DensityFn&& density_fn,
        uint8_t base_material = 1) const {

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
        ChunkSlot* slot = store.get_or_create_slot(chunk_key);
        std::unique_lock<std::shared_mutex> lock(slot->mutex);
        slot->reset_to_uniform(ChunkStorageForm::Solid, base_material, store.tsdf_pool(), store.material_pool());
        assert(slot->promote_to_dense(store.tsdf_pool()));

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
                }
            }
        }

        // Compact back to 0-byte EMPTY or SOLID if all 32^3 canonical samples are clamped uniform.
        slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
        slot->version.store(1, std::memory_order_release);
        slot->state = ChunkLifecycleState::Resident;
    }

    void generate_chunk(ChunkStore& store, ChunkKey chunk_key, WorkerDensityGrid34& grid34) const;
};

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
    };

    /// Meshes chunk `chunk_key` from its populated 36^3 (`-1..34`) scratch window:
    ///   1. Computes central-difference corner gradients over [0..33]^3 using [-1..34]^3.
    ///   2. Computes dual vertices for all active cells in [0..32]^3 (33^3 cells).
    ///   3. Stitches quads for sign-changing interior edges owned by cells [0..31]^3.
    ///   4. Emits indexed render arrays AND pre-flattened triangle soup for Physics/Navigation.
    [[nodiscard]] static ChunkMeshPayload mesh_chunk_from_scratch_36(
        ChunkKey chunk_key,
        const WorkerScratchWindow36& scratch,
        uint64_t captured_version = 1,
        uint64_t captured_terrain_revision = 0,
        float voxel_scale = 1.0f,
        float truncation_delta = TsdfFormat::kDefaultTruncationDelta);
};

} // namespace voxel
