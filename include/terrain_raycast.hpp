#pragma once

#include "edit_and_worker_pool.hpp"

#include <cmath>
#include <optional>
#include <shared_mutex>

namespace voxel {

/// Result of a terrain raycast that also reports what the ray hit: the
/// interpolated surface position plus the material of the first solid lattice
/// sample behind that surface (the voxel a character would be looking at).
struct TerrainMaterialHit {
    Vec3f position{};
    uint8_t material_id = MaterialHardness::kMaterialAir;
};

/// Marches the resident TSDF chunks along a ray and returns the first
/// interpolated air-to-solid surface crossing together with the material of the
/// solid sample behind it, or std::nullopt when the ray starts inside solid,
/// ends without crossing a surface, or is degenerate.
/// Sampling is Godot-free so both the extension and unit tests share it.
[[nodiscard]] inline std::optional<TerrainMaterialHit> raycast_terrain_material(
    const ChunkStore& store,
    Vec3f origin,
    Vec3f target,
    float step = 0.5f) {

    const float dx = target.x - origin.x;
    const float dy = target.y - origin.y;
    const float dz = target.z - origin.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length <= 1e-6f || step <= 0.0f) {
        return std::nullopt;
    }
    const float inv_len = 1.0f / length;
    const float sx = dx * inv_len;
    const float sy = dy * inv_len;
    const float sz = dz * inv_len;

    struct LatticeSample {
        int16_t q = 0;
        uint8_t material = MaterialHardness::kMaterialAir;
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
    };

    auto sample = [&store](float wx, float wy, float wz) -> LatticeSample {
        const int32_t x = static_cast<int32_t>(std::lround(wx));
        const int32_t y = static_cast<int32_t>(std::lround(wy));
        const int32_t z = static_cast<int32_t>(std::lround(wz));
        if (y < 0) {
            // Bedrock below the world floor: solid stone.
            return LatticeSample{TsdfFormat::kClampedSolid,
                                 MaterialHardness::kMaterialHardRock, x, y, z};
        }
        if (y >= store.world_height_chunks() * ChunkKey::kChunkDim) {
            return LatticeSample{TsdfFormat::kClampedAir, MaterialHardness::kMaterialAir, x, y, z};
        }
        const ChunkKey ck = ChunkKey::from_world_sample(x, y, z);
        const ChunkSlot* slot = store.find_slot(ck);
        if (slot == nullptr || slot->state != ChunkLifecycleState::Resident) {
            return LatticeSample{TsdfFormat::kClampedAir, MaterialHardness::kMaterialAir, x, y, z};
        }
        std::shared_lock<std::shared_mutex> lock(slot->mutex);
        const IVec3 base = ck.min_world_sample();
        const LocalCoord lc{x - base.x, y - base.y, z - base.z};
        return LatticeSample{slot->get_tsdf(lc), slot->get_material(lc), x, y, z};
    };

    LatticeSample prev = sample(origin.x, origin.y, origin.z);
    if (prev.q < 0) {
        return std::nullopt;
    }

    for (float t = step; t <= length; t += step) {
        const float px = origin.x + sx * t;
        const float py = origin.y + sy * t;
        const float pz = origin.z + sz * t;
        const LatticeSample cur = sample(px, py, pz);
        if (cur.q < 0) {
            const float prev_f = static_cast<float>(prev.q);
            const float cur_f = static_cast<float>(cur.q);
            const float denom = prev_f - cur_f;
            const float frac = (denom > 1e-6f) ? (prev_f / denom) : 0.5f;
            TerrainMaterialHit hit{};
            hit.position = Vec3f{
                static_cast<float>(prev.x) + static_cast<float>(cur.x - prev.x) * frac,
                static_cast<float>(prev.y) + static_cast<float>(cur.y - prev.y) * frac,
                static_cast<float>(prev.z) + static_cast<float>(cur.z - prev.z) * frac};
            hit.material_id = cur.material;
            return hit;
        }
        prev = cur;
    }
    return std::nullopt;
}

/// Marches the resident TSDF chunks along a ray and returns the first
/// interpolated air-to-solid surface crossing, or std::nullopt when the ray
/// starts inside solid, ends without crossing a surface, or is degenerate.
[[nodiscard]] inline std::optional<Vec3f> raycast_terrain(
    const ChunkStore& store,
    Vec3f origin,
    Vec3f target,
    float step = 0.5f) {

    const std::optional<TerrainMaterialHit> hit =
        raycast_terrain_material(store, origin, target, step);
    if (!hit.has_value()) {
        return std::nullopt;
    }
    return hit->position;
}

} // namespace voxel
