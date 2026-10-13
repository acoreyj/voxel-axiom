#pragma once

#include "voxel/procgen_and_surface_nets.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel {

// ============================================================================
// 1. 3D 8 x 8 x 8 Region Coordinate Mapping (HLD v4.0 Section 3.6)
// ============================================================================
struct RegionKey {
    int32_t rx = 0; // ChunkKey::floor_div(Cx, 8)
    int32_t ry = 0; // ChunkKey::floor_div(Cy, 8)
    int32_t rz = 0; // ChunkKey::floor_div(Cz, 8)

    static constexpr int32_t kRegionDim = 8;
    static constexpr std::size_t kChunksPerRegion = 8ULL * 8ULL * 8ULL; // 512 chunks max

    [[nodiscard]] static constexpr RegionKey from_chunk_key(ChunkKey ck) noexcept {
        return RegionKey{
            ChunkKey::floor_div(ck.x(), kRegionDim),
            ChunkKey::floor_div(ck.y(), kRegionDim),
            ChunkKey::floor_div(ck.z(), kRegionDim)
        };
    }

    /// Returns the local chunk index in [0, 511] within the 8x8x8 region TOC.
    [[nodiscard]] static constexpr uint16_t local_toc_index(ChunkKey ck) noexcept {
        const uint32_t lx = static_cast<uint32_t>(ChunkKey::floor_mod(ck.x(), kRegionDim));
        const uint32_t ly = static_cast<uint32_t>(ChunkKey::floor_mod(ck.y(), kRegionDim));
        const uint32_t lz = static_cast<uint32_t>(ChunkKey::floor_mod(ck.z(), kRegionDim));
        return static_cast<uint16_t>(lx + 8U * (ly + 8U * lz));
    }

    constexpr bool operator==(const RegionKey&) const noexcept = default;
    constexpr auto operator<=>(const RegionKey&) const noexcept = default;
};

struct RegionKeyHash {
    [[nodiscard]] constexpr std::size_t operator()(const RegionKey& k) const noexcept {
        const ChunkKey packed = ChunkKey::from_coords(k.rx, k.ry, k.rz);
        return static_cast<std::size_t>(packed.hash());
    }
};

// ============================================================================
// 2. Byte-Plane Split + Fast Byte-Plane RLE/LZ Codec (Section 3.6)
// ============================================================================
struct BytePlaneCodec {
    /// Splits 32,768 int16_t samples into a 32,768-byte low plane and a 32,768-byte high plane.
    static void split_int16_planes(const int16_t* src,
                                   uint8_t* out_low,
                                   uint8_t* out_high,
                                   std::size_t count = TsdfFormat::kChunkSamples) noexcept;

    /// Reconstructs 32,768 int16_t samples from their low-byte and high-byte planes.
    static void merge_int16_planes(const uint8_t* low,
                                   const uint8_t* high,
                                   int16_t* out_dst,
                                   std::size_t count = TsdfFormat::kChunkSamples) noexcept;

    /// Fast PackBits / LZ-RLE byte-plane compressor.
    [[nodiscard]] static std::vector<uint8_t> compress_plane(const uint8_t* src, std::size_t len);

    [[nodiscard]] static bool decompress_plane(const uint8_t* src,
                                               std::size_t src_len,
                                               uint8_t* dst,
                                               std::size_t expected_len) noexcept;
};

// ============================================================================
// 3. Compressed Chunk Blob & Region File Format (.vreg, Section 3.6)
// ============================================================================
enum class SerializedChunkForm : uint8_t {
    Untouched = 0,
    Empty     = 1, // 0-byte payload (all kClampedAir)
    Solid     = 2, // 0-byte payload (all kClampedSolid)
    Dense     = 3  // Byte-plane split compressed payload
};

struct CompressedChunkBlob {
    SerializedChunkForm form = SerializedChunkForm::Empty;
    uint8_t base_material_id = 0;
    bool has_material_plane = false;
    uint64_t captured_version = 0;
    bool dirty_for_disk = true;

    // Populated only when form == SerializedChunkForm::Dense:
    // Header inside payload: [uint32_t low_size][uint32_t high_size][uint32_t mat_size] followed by bytes
    std::vector<uint8_t> payload;

    [[nodiscard]] std::size_t ram_bytes() const noexcept {
        return payload.size();
    }
};

#pragma pack(push, 1)
struct RegionFileHeader {
    char magic[4] = {'V', 'R', 'E', 'G'};
    uint32_t version = 4;
    int32_t rx = 0;
    int32_t ry = 0;
    int32_t rz = 0;
    uint64_t payload_checksum = 0;
};

struct RegionTocEntry {
    uint8_t form = static_cast<uint8_t>(SerializedChunkForm::Untouched);
    uint8_t base_material_id = 0;
    uint8_t has_material_plane = 0;
    uint8_t reserved = 0;
    uint32_t payload_offset = 0;
    uint32_t payload_size = 0;
};
#pragma pack(pop)

// ============================================================================
// 4. Two-Stage Persistence Manager (`RegionBlobTable` + Atomic `RegionFlush`)
// ============================================================================
class RegionSaveManager {
public:
    static constexpr std::size_t kDirtyRegionFlushThresholdBytes = 4ULL * 1024ULL * 1024ULL; // 4 MiB

    explicit RegionSaveManager(MemoryLedger& ledger, std::filesystem::path save_dir);
    ~RegionSaveManager();

    RegionSaveManager(const RegionSaveManager&) = delete;
    RegionSaveManager& operator=(const RegionSaveManager&) = delete;

    /// Stage 1: `PersistCompress` (In-Memory Worker Job, ~50 us per chunk, Section 3.2 & 3.6).
    [[nodiscard]] bool persist_compress_chunk(ChunkStore& store, ChunkKey key);

    /// Stage 2: `RegionFlush` (Atomic 8x8x8 Region Disk Flush, Section 3.6).
    [[nodiscard]] bool flush_region_to_disk(RegionKey rk);

    /// Flushes all dirty 8x8x8 regions to disk (called on `save_world()` or `shutdown(true)`).
    void flush_all_dirty_regions();

    /// Load Routing (Section 3.3).
    [[nodiscard]] bool load_or_generate_chunk(ChunkStore& store,
                                              ChunkKey key,
                                              const CaveNoiseGenerator& procgen,
                                              WorkerDensityGrid34& grid34);

    /// Releases all cached blobs in `RegionBlobTable` and returns their bytes to `MemoryLedger`.
    void clear_ram_cache() noexcept;

    [[nodiscard]] std::filesystem::path region_file_path(RegionKey rk) const;
    [[nodiscard]] std::filesystem::path region_tmp_path(RegionKey rk) const;

private:
    struct RegionEntry {
        std::array<std::optional<CompressedChunkBlob>, RegionKey::kChunksPerRegion> blobs{};
        std::size_t dirty_bytes = 0;
        bool has_dirty_blobs = false;
        std::chrono::steady_clock::time_point first_dirty_timestamp{};
    };

    [[nodiscard]] static uint64_t compute_fnv1a64(const uint8_t* data, std::size_t len) noexcept;

    [[nodiscard]] static bool populate_slot_from_blob(ChunkStore& store,
                                                      ChunkKey key,
                                                      const CompressedChunkBlob& blob);

    [[nodiscard]] bool load_existing_region_file(
        RegionKey rk,
        std::array<std::optional<CompressedChunkBlob>, RegionKey::kChunksPerRegion>& out_blobs) const;

    [[nodiscard]] std::optional<CompressedChunkBlob> read_chunk_blob_from_disk(ChunkKey key) const;

    [[nodiscard]] bool flush_region_unlocked(RegionKey rk);

    MemoryLedger& ledger_;
    std::filesystem::path save_dir_;
    mutable std::mutex mutex_;
    std::unordered_map<RegionKey, RegionEntry, RegionKeyHash> regions_;
};

} // namespace voxel
