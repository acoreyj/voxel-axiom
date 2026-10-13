#pragma once

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace voxel {

/// Integer 3D coordinate in world sample space or chunk space.
struct IVec3 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    constexpr bool operator==(const IVec3&) const noexcept = default;
    constexpr auto operator<=>(const IVec3&) const noexcept = default;
};

/// Canonical local sample coordinate within a 32^3 chunk: each axis in [0, 31].
struct LocalCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    constexpr bool operator==(const LocalCoord&) const noexcept = default;
    constexpr auto operator<=>(const LocalCoord&) const noexcept = default;

    /// Returns true if all axes lie within the canonical [0, 31] sample range.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return (x >= 0 && x < 32) && (y >= 0 && y < 32) && (z >= 0 && z < 32);
    }

    /// Flat index into a canonical 32^3 array (x + 32 * (y + 32 * z)) in [0, 32767].
    [[nodiscard]] constexpr uint32_t to_linear_index() const noexcept {
        assert(is_valid());
        return static_cast<uint32_t>(x) +
               (static_cast<uint32_t>(y) << 5U) +
               (static_cast<uint32_t>(z) << 10U);
    }

    /// Reconstructs LocalCoord from a flat canonical index in [0, 32767].
    [[nodiscard]] static constexpr LocalCoord from_linear_index(uint32_t idx) noexcept {
        assert(idx < 32768U);
        return LocalCoord{
            static_cast<int32_t>(idx & 31U),
            static_cast<int32_t>((idx >> 5U) & 31U),
            static_cast<int32_t>((idx >> 10U) & 31U)
        };
    }
};

/// 64-bit Offset-Encoded ChunkKey (HLD v4.0 Section 3.1).
///
/// Packs 3 signed 21-bit chunk coordinates (x, y, z) using an offset bias of 2^20:
///   - Bits [ 0..20]: ux = (x + 2^20) & 0x1FFFFF
///   - Bits [21..41]: uy = (y + 2^20) & 0x1FFFFF
///   - Bits [42..62]: uz = (z + 2^20) & 0x1FFFFF
///   - Bit  [63]    : 0 (unused, always zero)
///
/// Valid coordinate domain per axis: [-1,048,576, +1,048,575].
struct ChunkKey {
    uint64_t raw = 0;

    static constexpr int32_t kChunkDim = 32;
    static constexpr int32_t kAxisBits = 21;
    static constexpr int64_t kBias = 1LL << 20;               // 1,048,576
    static constexpr uint64_t kMask = (1ULL << kAxisBits) - 1; // 0x1FFFFF
    static constexpr int32_t kMinCoord = -static_cast<int32_t>(kBias);     // -1,048,576
    static constexpr int32_t kMaxCoord = static_cast<int32_t>(kBias) - 1;  // +1,048,575

    /// Mathematical floor division toward -infinity (never C++ truncation toward zero).
    /// Precondition: b != 0.
    [[nodiscard]] static constexpr int32_t floor_div(int32_t a, int32_t b) noexcept {
        assert(b != 0);
        int32_t d = a / b;
        int32_t r = a % b;
        return (r != 0 && ((r < 0) != (b < 0))) ? (d - 1) : d;
    }

    /// Mathematical floor modulo with the same sign as divisor b.
    /// For b > 0, guarantees result in [0, b - 1] such that:
    ///   a == floor_div(a, b) * b + floor_mod(a, b)
    /// Precondition: b != 0.
    [[nodiscard]] static constexpr int32_t floor_mod(int32_t a, int32_t b) noexcept {
        assert(b != 0);
        int32_t r = a % b;
        return (r != 0 && ((r < 0) != (b < 0))) ? (r + b) : r;
    }

    /// Returns true if chunk coordinates (x, y, z) fit within the 21-bit offset-encoded range.
    [[nodiscard]] static constexpr bool coords_in_range(int32_t x, int32_t y, int32_t z) noexcept {
        return (x >= kMinCoord && x <= kMaxCoord) &&
               (y >= kMinCoord && y <= kMaxCoord) &&
               (z >= kMinCoord && z <= kMaxCoord);
    }

    /// Packs 3D chunk coordinates (Cx, Cy, Cz) into a 64-bit offset-encoded ChunkKey.
    /// Zero sign-extension across axes.
    [[nodiscard]] static constexpr ChunkKey from_coords(int32_t x, int32_t y, int32_t z) noexcept {
        assert(coords_in_range(x, y, z));
        uint64_t ux = static_cast<uint64_t>(static_cast<int64_t>(x) + kBias) & kMask;
        uint64_t uy = static_cast<uint64_t>(static_cast<int64_t>(y) + kBias) & kMask;
        uint64_t uz = static_cast<uint64_t>(static_cast<int64_t>(z) + kBias) & kMask;
        return ChunkKey{ux | (uy << 21U) | (uz << 42U)};
    }

    /// Unpacks the X chunk coordinate in [-1,048,576, +1,048,575].
    [[nodiscard]] constexpr int32_t x() const noexcept {
        return static_cast<int32_t>(raw & kMask) - static_cast<int32_t>(kBias);
    }

    /// Unpacks the Y chunk coordinate in [-1,048,576, +1,048,575].
    [[nodiscard]] constexpr int32_t y() const noexcept {
        return static_cast<int32_t>((raw >> 21U) & kMask) - static_cast<int32_t>(kBias);
    }

    /// Unpacks the Z chunk coordinate in [-1,048,576, +1,048,575].
    [[nodiscard]] constexpr int32_t z() const noexcept {
        return static_cast<int32_t>((raw >> 42U) & kMask) - static_cast<int32_t>(kBias);
    }

    /// Unpacks all three chunk coordinates as an IVec3.
    [[nodiscard]] constexpr IVec3 coords() const noexcept {
        return IVec3{x(), y(), z()};
    }

    /// Returns a new ChunkKey offset by (dx, dy, dz) chunks.
    [[nodiscard]] constexpr ChunkKey offset(int32_t dx, int32_t dy, int32_t dz) const noexcept {
        return from_coords(x() + dx, y() + dy, z() + dz);
    }

    /// Maps an integer world sample position (px, py, pz) to its authoritative owner ChunkKey.
    [[nodiscard]] static constexpr ChunkKey from_world_sample(int32_t px, int32_t py, int32_t pz) noexcept {
        return from_coords(
            floor_div(px, kChunkDim),
            floor_div(py, kChunkDim),
            floor_div(pz, kChunkDim)
        );
    }

    [[nodiscard]] static constexpr ChunkKey from_world_sample(IVec3 p) noexcept {
        return from_world_sample(p.x, p.y, p.z);
    }

    /// Maps an integer world sample position (px, py, pz) to its local sample coordinate in [0, 31]^3.
    [[nodiscard]] static constexpr LocalCoord local_from_world_sample(int32_t px, int32_t py, int32_t pz) noexcept {
        return LocalCoord{
            floor_mod(px, kChunkDim),
            floor_mod(py, kChunkDim),
            floor_mod(pz, kChunkDim)
        };
    }

    [[nodiscard]] static constexpr LocalCoord local_from_world_sample(IVec3 p) noexcept {
        return local_from_world_sample(p.x, p.y, p.z);
    }

    /// Returns the minimum world sample coordinate (p_min = 32 * C) owned by this chunk.
    [[nodiscard]] constexpr IVec3 min_world_sample() const noexcept {
        return IVec3{
            x() * kChunkDim,
            y() * kChunkDim,
            z() * kChunkDim
        };
    }

    /// Reconstructs the integer world sample coordinate from (ChunkKey, LocalCoord):
    ///   p = 32 * C + L
    [[nodiscard]] constexpr IVec3 to_world_sample(LocalCoord local) const noexcept {
        assert(local.is_valid());
        return IVec3{
            x() * kChunkDim + local.x,
            y() * kChunkDim + local.y,
            z() * kChunkDim + local.z
        };
    }

    /// Fast 64-bit bitwise avalanching mixer (SplitMix64 / MurmurHash3 finalizer)
    /// suitable for flat open-addressing hash tables (Section 3.1).
    [[nodiscard]] static constexpr uint64_t hash_raw(uint64_t k) noexcept {
        k ^= k >> 30U;
        k *= 0xbf58476d1ce4e5b9ULL;
        k ^= k >> 27U;
        k *= 0x94d049bb133111ebULL;
        k ^= k >> 31U;
        return k;
    }

    [[nodiscard]] constexpr uint64_t hash() const noexcept {
        return hash_raw(raw);
    }

    constexpr bool operator==(const ChunkKey& other) const noexcept { return raw == other.raw; }
    constexpr bool operator!=(const ChunkKey& other) const noexcept { return raw != other.raw; }
    constexpr bool operator<(const ChunkKey& other) const noexcept { return raw < other.raw; }
    constexpr auto operator<=>(const ChunkKey& other) const noexcept { return raw <=> other.raw; }
};

/// Result of decomposing an integer world sample position into its canonical owner chunk and local sample.
struct SampleMapping {
    ChunkKey chunk;
    LocalCoord local;

    constexpr bool operator==(const SampleMapping&) const noexcept = default;
};

/// Decomposes an integer world sample coordinate p = (px, py, pz) into its canonical
/// (ChunkKey, LocalCoord) pair in a single pass.
[[nodiscard]] constexpr SampleMapping map_world_sample(int32_t px, int32_t py, int32_t pz) noexcept {
    return SampleMapping{
        ChunkKey::from_world_sample(px, py, pz),
        ChunkKey::local_from_world_sample(px, py, pz)
    };
}

[[nodiscard]] constexpr SampleMapping map_world_sample(IVec3 p) noexcept {
    return map_world_sample(p.x, p.y, p.z);
}

/// Reconstructs the integer world sample coordinate from a canonical SampleMapping.
[[nodiscard]] constexpr IVec3 unmap_world_sample(const SampleMapping& m) noexcept {
    return m.chunk.to_world_sample(m.local);
}

/// Hash functor for ChunkKey with is_avalanching tag for ankerl::unordered_dense / flat hash maps.
struct ChunkKeyHash {
    using is_avalanching = void;

    [[nodiscard]] constexpr std::size_t operator()(const ChunkKey& key) const noexcept {
        return static_cast<std::size_t>(key.hash());
    }
};

} // namespace voxel

namespace std {
template <>
struct hash<voxel::ChunkKey> {
    [[nodiscard]] constexpr std::size_t operator()(const voxel::ChunkKey& key) const noexcept {
        return static_cast<std::size_t>(key.hash());
    }
};
} // namespace std
