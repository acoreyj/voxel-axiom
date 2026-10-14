#pragma once

#include "procgen_and_surface_nets.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace voxel {

// ============================================================================
// 1. 3D 8 x 8 x 8 Region Coordinate Mapping (HLD v4.0 Section 3.6)
// ============================================================================
struct RegionKey {
    int32_t rx = 0;
    int32_t ry = 0;
    int32_t rz = 0;

    static constexpr int32_t kRegionDim = 8;
    static constexpr std::size_t kChunksPerRegion = 8ULL * 8ULL * 8ULL;

    [[nodiscard]] static constexpr RegionKey from_chunk_key(ChunkKey ck) noexcept {
        return RegionKey{
            ChunkKey::floor_div(ck.x(), kRegionDim),
            ChunkKey::floor_div(ck.y(), kRegionDim),
            ChunkKey::floor_div(ck.z(), kRegionDim)
        };
    }

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
    static void split_int16_planes(const int16_t* src,
                                   uint8_t* out_low,
                                   uint8_t* out_high,
                                   std::size_t count = TsdfFormat::kChunkSamples) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            const uint16_t u = static_cast<uint16_t>(src[i]);
            out_low[i] = static_cast<uint8_t>(u & 0xFFU);
            out_high[i] = static_cast<uint8_t>((u >> 8U) & 0xFFU);
        }
    }

    static void merge_int16_planes(const uint8_t* low,
                                   const uint8_t* high,
                                   int16_t* out_dst,
                                   std::size_t count = TsdfFormat::kChunkSamples) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            const uint16_t u = static_cast<uint16_t>(low[i]) |
                               (static_cast<uint16_t>(high[i]) << 8U);
            out_dst[i] = static_cast<int16_t>(u);
        }
    }

    [[nodiscard]] static std::vector<uint8_t> compress_plane(const uint8_t* src, std::size_t len) {
        std::vector<uint8_t> out;
        out.reserve(len / 2);

        std::size_t i = 0;
        while (i < len) {
            std::size_t run_len = 1;
            while (i + run_len < len && run_len < 130ULL && src[i + run_len] == src[i]) {
                ++run_len;
            }
            if (run_len >= 3ULL) {
                out.push_back(static_cast<uint8_t>(128U + (run_len - 3ULL)));
                out.push_back(src[i]);
                i += run_len;
                continue;
            }

            const std::size_t lit_start = i;
            std::size_t lit_len = 0;
            while (i < len && lit_len < 128ULL) {
                if (i + 2 < len && src[i] == src[i + 1] && src[i] == src[i + 2]) {
                    break;
                }
                ++i;
                ++lit_len;
            }
            out.push_back(static_cast<uint8_t>(lit_len - 1ULL));
            out.insert(out.end(), src + lit_start, src + lit_start + lit_len);
        }
        return out;
    }

    [[nodiscard]] static bool decompress_plane(const uint8_t* src,
                                               std::size_t src_len,
                                               uint8_t* dst,
                                               std::size_t expected_len) noexcept {
        std::size_t in_pos = 0;
        std::size_t out_pos = 0;
        while (in_pos < src_len) {
            const uint8_t ctrl = src[in_pos++];
            if (ctrl >= 128U) {
                const std::size_t run_len = static_cast<std::size_t>(ctrl - 128U) + 3ULL;
                if (in_pos >= src_len || out_pos + run_len > expected_len) {
                    return false;
                }
                const uint8_t val = src[in_pos++];
                std::memset(dst + out_pos, val, run_len);
                out_pos += run_len;
            } else {
                const std::size_t lit_len = static_cast<std::size_t>(ctrl) + 1ULL;
                if (in_pos + lit_len > src_len || out_pos + lit_len > expected_len) {
                    return false;
                }
                std::memcpy(dst + out_pos, src + in_pos, lit_len);
                in_pos += lit_len;
                out_pos += lit_len;
            }
        }
        return out_pos == expected_len;
    }
};

// ============================================================================
// 3. Compressed Chunk Blob & Region File Format (.vreg, Section 3.6)
// ============================================================================
enum class SerializedChunkForm : uint8_t {
    Untouched = 0,
    Empty     = 1,
    Solid     = 2,
    Dense     = 3
};

struct CompressedChunkBlob {
    SerializedChunkForm form = SerializedChunkForm::Empty;
    uint8_t base_material_id = 0;
    bool has_material_plane = false;
    uint64_t captured_version = 0;
    bool dirty_for_disk = true;

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
    static constexpr std::size_t kDirtyRegionFlushThresholdBytes = 4ULL * 1024ULL * 1024ULL;

    explicit RegionSaveManager(MemoryLedger& ledger, std::filesystem::path save_dir)
        : ledger_(ledger), save_dir_(std::move(save_dir)) {
        std::error_code ec;
        std::filesystem::create_directories(save_dir_, ec);
    }

    ~RegionSaveManager() {
        clear_ram_cache();
    }

    RegionSaveManager(const RegionSaveManager&) = delete;
    RegionSaveManager& operator=(const RegionSaveManager&) = delete;

    [[nodiscard]] bool persist_compress_chunk(ChunkStore& store, ChunkKey key) {
        ChunkSlot* slot = store.find_slot(key);
        if (!slot) {
            return false;
        }

        uint64_t version_before = 0;
        ChunkStorageForm form_snapshot = ChunkStorageForm::Empty;
        uint8_t base_mat = 0;
        bool has_mat_array = false;
        std::vector<int16_t> tsdf_copy;
        std::vector<uint8_t> mat_copy;

        {
            std::shared_lock<std::shared_mutex> read_lock(slot->mutex);
            if (slot->state != ChunkLifecycleState::Resident) {
                return false;
            }
            version_before = slot->version.load(std::memory_order_acquire);
            form_snapshot = slot->form;
            base_mat = slot->base_material_id;
            has_mat_array = slot->has_material_array();

            if (form_snapshot == ChunkStorageForm::Dense) {
                tsdf_copy.assign(slot->tsdf_data, slot->tsdf_data + TsdfFormat::kChunkSamples);
            }
            if (has_mat_array) {
                mat_copy.assign(slot->material_data, slot->material_data + TsdfFormat::kChunkSamples);
            }
        }

        CompressedChunkBlob blob{};
        blob.base_material_id = base_mat;
        blob.has_material_plane = has_mat_array;
        blob.captured_version = version_before;
        blob.dirty_for_disk = true;

        if (form_snapshot == ChunkStorageForm::Empty && !has_mat_array) {
            blob.form = SerializedChunkForm::Empty;
        } else if (form_snapshot == ChunkStorageForm::Solid && !has_mat_array) {
            blob.form = SerializedChunkForm::Solid;
        } else {
            blob.form = SerializedChunkForm::Dense;
            if (tsdf_copy.empty()) {
                const int16_t fill_v = (form_snapshot == ChunkStorageForm::Solid)
                                           ? TsdfFormat::kClampedSolid
                                           : TsdfFormat::kClampedAir;
                tsdf_copy.assign(TsdfFormat::kChunkSamples, fill_v);
            }

            std::vector<uint8_t> low_plane(TsdfFormat::kChunkSamples);
            std::vector<uint8_t> high_plane(TsdfFormat::kChunkSamples);
            BytePlaneCodec::split_int16_planes(tsdf_copy.data(), low_plane.data(), high_plane.data());

            const std::vector<uint8_t> comp_low =
                BytePlaneCodec::compress_plane(low_plane.data(), low_plane.size());
            const std::vector<uint8_t> comp_high =
                BytePlaneCodec::compress_plane(high_plane.data(), high_plane.size());
            std::vector<uint8_t> comp_mat;
            if (has_mat_array) {
                comp_mat = BytePlaneCodec::compress_plane(mat_copy.data(), mat_copy.size());
            }

            const uint32_t low_sz = static_cast<uint32_t>(comp_low.size());
            const uint32_t high_sz = static_cast<uint32_t>(comp_high.size());
            const uint32_t mat_sz = static_cast<uint32_t>(comp_mat.size());

            blob.payload.resize(3 * sizeof(uint32_t) + low_sz + high_sz + mat_sz);
            std::memcpy(blob.payload.data() + 0, &low_sz, sizeof(uint32_t));
            std::memcpy(blob.payload.data() + 4, &high_sz, sizeof(uint32_t));
            std::memcpy(blob.payload.data() + 8, &mat_sz, sizeof(uint32_t));

            std::size_t offset = 12;
            std::memcpy(blob.payload.data() + offset, comp_low.data(), low_sz);
            offset += low_sz;
            std::memcpy(blob.payload.data() + offset, comp_high.data(), high_sz);
            offset += high_sz;
            if (mat_sz > 0) {
                std::memcpy(blob.payload.data() + offset, comp_mat.data(), mat_sz);
            }
        }

        {
            std::shared_lock<std::shared_mutex> verify_lock(slot->mutex);
            const uint64_t version_after = slot->version.load(std::memory_order_acquire);
            if (version_after != version_before) {
                return false;
            }
        }

        std::lock_guard<std::mutex> table_lock(mutex_);
        const RegionKey rk = RegionKey::from_chunk_key(key);
        const uint16_t toc_idx = RegionKey::local_toc_index(key);
        RegionEntry& region = regions_[rk];

        const std::size_t old_bytes =
            region.blobs[toc_idx].has_value() ? region.blobs[toc_idx]->ram_bytes() : 0ULL;
        const std::size_t new_bytes = blob.ram_bytes();

        if (new_bytes > old_bytes) {
            if (!ledger_.try_reserve(new_bytes - old_bytes, MemoryPoolId::RegionBlobCache)) {
                return false;
            }
        } else if (old_bytes > new_bytes) {
            ledger_.release(old_bytes - new_bytes, MemoryPoolId::RegionBlobCache);
        }

        if (!region.has_dirty_blobs) {
            region.has_dirty_blobs = true;
            region.first_dirty_timestamp = std::chrono::steady_clock::now();
        }
        region.dirty_bytes = region.dirty_bytes - old_bytes + new_bytes;
        region.blobs[toc_idx] = std::move(blob);

        {
            std::unique_lock<std::shared_mutex> slot_lock(slot->mutex);
            if (slot->version.load(std::memory_order_acquire) == version_before) {
                slot->dirty_persist = false;
            }
        }
        return true;
    }

    [[nodiscard]] bool flush_region_to_disk(RegionKey rk) {
        std::lock_guard<std::mutex> table_lock(mutex_);
        return flush_region_unlocked(rk);
    }

    void flush_all_dirty_regions() {
        std::lock_guard<std::mutex> table_lock(mutex_);
        for (auto& [rk, region] : regions_) {
            if (region.has_dirty_blobs) {
                (void)flush_region_unlocked(rk);
            }
        }
    }

    [[nodiscard]] bool load_or_generate_chunk(ChunkStore& store,
                                              ChunkKey key,
                                              const CaveNoiseGenerator& procgen,
                                              WorkerDensityGrid34& grid34) {
        {
            std::lock_guard<std::mutex> table_lock(mutex_);
            const RegionKey rk = RegionKey::from_chunk_key(key);
            const uint16_t toc_idx = RegionKey::local_toc_index(key);
            auto it = regions_.find(rk);
            if (it != regions_.end() && it->second.blobs[toc_idx].has_value()) {
                return populate_slot_from_blob(store, key, *it->second.blobs[toc_idx]);
            }
        }

        if (auto disk_blob = read_chunk_blob_from_disk(key); disk_blob.has_value()) {
            return populate_slot_from_blob(store, key, *disk_blob);
        }

        procgen.generate_chunk(store, key, grid34);
        return true;
    }

    void clear_ram_cache() noexcept {
        std::lock_guard<std::mutex> table_lock(mutex_);
        for (auto& [rk, region] : regions_) {
            for (auto& maybe_blob : region.blobs) {
                if (maybe_blob.has_value() && maybe_blob->ram_bytes() > 0) {
                    ledger_.release(maybe_blob->ram_bytes(), MemoryPoolId::RegionBlobCache);
                }
                maybe_blob.reset();
            }
            region.dirty_bytes = 0;
            region.has_dirty_blobs = false;
        }
        regions_.clear();
    }

    [[nodiscard]] std::filesystem::path region_file_path(RegionKey rk) const {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "region_%d_%d_%d.vreg", rk.rx, rk.ry, rk.rz);
        return save_dir_ / buf;
    }

    [[nodiscard]] std::filesystem::path region_tmp_path(RegionKey rk) const {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "region_%d_%d_%d.vreg.tmp", rk.rx, rk.ry, rk.rz);
        return save_dir_ / buf;
    }

private:
    struct RegionEntry {
        std::array<std::optional<CompressedChunkBlob>, RegionKey::kChunksPerRegion> blobs{};
        std::size_t dirty_bytes = 0;
        bool has_dirty_blobs = false;
        std::chrono::steady_clock::time_point first_dirty_timestamp{};
    };

    [[nodiscard]] static uint64_t compute_fnv1a64(const uint8_t* data, std::size_t len) noexcept {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (std::size_t i = 0; i < len; ++i) {
            hash ^= static_cast<uint64_t>(data[i]);
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    [[nodiscard]] static bool populate_slot_from_blob(ChunkStore& store,
                                                      ChunkKey key,
                                                      const CompressedChunkBlob& blob) {
        ChunkSlot* slot = store.get_or_create_slot(key);
        std::unique_lock<std::shared_mutex> lock(slot->mutex);

        if (blob.form == SerializedChunkForm::Empty) {
            slot->reset_to_uniform(ChunkStorageForm::Empty, blob.base_material_id,
                                   store.tsdf_pool(), store.material_pool());
            slot->version.store(1, std::memory_order_release);
            slot->state = ChunkLifecycleState::Resident;
            slot->dirty_persist = false;
            return true;
        }
        if (blob.form == SerializedChunkForm::Solid) {
            slot->reset_to_uniform(ChunkStorageForm::Solid, blob.base_material_id,
                                   store.tsdf_pool(), store.material_pool());
            slot->version.store(1, std::memory_order_release);
            slot->state = ChunkLifecycleState::Resident;
            slot->dirty_persist = false;
            return true;
        }

        if (blob.payload.size() < 12ULL) {
            return false;
        }
        uint32_t low_sz = 0, high_sz = 0, mat_sz = 0;
        std::memcpy(&low_sz, blob.payload.data() + 0, sizeof(uint32_t));
        std::memcpy(&high_sz, blob.payload.data() + 4, sizeof(uint32_t));
        std::memcpy(&mat_sz, blob.payload.data() + 8, sizeof(uint32_t));

        if (12ULL + low_sz + high_sz + mat_sz != blob.payload.size()) {
            return false;
        }

        std::vector<uint8_t> low_plane(TsdfFormat::kChunkSamples);
        std::vector<uint8_t> high_plane(TsdfFormat::kChunkSamples);
        const uint8_t* ptr = blob.payload.data() + 12;

        if (!BytePlaneCodec::decompress_plane(ptr, low_sz, low_plane.data(), low_plane.size())) {
            return false;
        }
        ptr += low_sz;
        if (!BytePlaneCodec::decompress_plane(ptr, high_sz, high_plane.data(), high_plane.size())) {
            return false;
        }
        ptr += high_sz;

        slot->reset_to_uniform(ChunkStorageForm::Solid, blob.base_material_id,
                               store.tsdf_pool(), store.material_pool());
        if (!slot->promote_to_dense(store.tsdf_pool())) {
            return false;
        }
        BytePlaneCodec::merge_int16_planes(low_plane.data(), high_plane.data(), slot->tsdf_data);

        if (blob.has_material_plane && mat_sz > 0) {
            std::vector<uint8_t> mat_plane(TsdfFormat::kChunkSamples);
            if (!BytePlaneCodec::decompress_plane(ptr, mat_sz, mat_plane.data(), mat_plane.size())) {
                return false;
            }
            for (uint32_t i = 0; i < TsdfFormat::kChunkSamples; ++i) {
                (void)slot->set_material_linear(i, mat_plane[i], store.material_pool());
            }
        }

        slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
        slot->version.store(1, std::memory_order_release);
        slot->state = ChunkLifecycleState::Resident;
        slot->dirty_persist = false;
        return true;
    }

    [[nodiscard]] bool load_existing_region_file(
        RegionKey rk,
        std::array<std::optional<CompressedChunkBlob>, RegionKey::kChunksPerRegion>& out_blobs) const {

        const std::filesystem::path path = region_file_path(rk);
        FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) {
            return false;
        }

        RegionFileHeader hdr{};
        if (std::fread(&hdr, sizeof(RegionFileHeader), 1, fp) != 1 ||
            std::memcmp(hdr.magic, "VREG", 4) != 0 ||
            hdr.version != 4 ||
            hdr.rx != rk.rx || hdr.ry != rk.ry || hdr.rz != rk.rz) {
            std::fclose(fp);
            return false;
        }

        std::array<RegionTocEntry, RegionKey::kChunksPerRegion> toc{};
        if (std::fread(toc.data(), sizeof(RegionTocEntry), RegionKey::kChunksPerRegion, fp) !=
            RegionKey::kChunksPerRegion) {
            std::fclose(fp);
            return false;
        }

        const long payload_start = std::ftell(fp);
        std::fseek(fp, 0, SEEK_END);
        const long file_end = std::ftell(fp);
        if (file_end < payload_start) {
            std::fclose(fp);
            return false;
        }
        const std::size_t total_payload_bytes = static_cast<std::size_t>(file_end - payload_start);
        std::vector<uint8_t> all_payloads(total_payload_bytes);
        std::fseek(fp, payload_start, SEEK_SET);
        if (total_payload_bytes > 0 &&
            std::fread(all_payloads.data(), 1, total_payload_bytes, fp) != total_payload_bytes) {
            std::fclose(fp);
            return false;
        }
        std::fclose(fp);

        if (compute_fnv1a64(all_payloads.data(), all_payloads.size()) != hdr.payload_checksum) {
            return false;
        }

        for (std::size_t i = 0; i < RegionKey::kChunksPerRegion; ++i) {
            const RegionTocEntry& e = toc[i];
            const auto form = static_cast<SerializedChunkForm>(e.form);
            if (form == SerializedChunkForm::Untouched) {
                continue;
            }
            if (static_cast<std::size_t>(e.payload_offset) + e.payload_size > all_payloads.size()) {
                return false;
            }
            CompressedChunkBlob blob{};
            blob.form = form;
            blob.base_material_id = e.base_material_id;
            blob.has_material_plane = (e.has_material_plane != 0);
            blob.dirty_for_disk = false;
            if (e.payload_size > 0) {
                blob.payload.assign(all_payloads.begin() + e.payload_offset,
                                    all_payloads.begin() + e.payload_offset + e.payload_size);
            }
            out_blobs[i] = std::move(blob);
        }
        return true;
    }

    [[nodiscard]] std::optional<CompressedChunkBlob> read_chunk_blob_from_disk(ChunkKey key) const {
        const RegionKey rk = RegionKey::from_chunk_key(key);
        const uint16_t toc_idx = RegionKey::local_toc_index(key);
        std::array<std::optional<CompressedChunkBlob>, RegionKey::kChunksPerRegion> disk_blobs{};
        if (!load_existing_region_file(rk, disk_blobs)) {
            return std::nullopt;
        }
        return disk_blobs[toc_idx];
    }

    [[nodiscard]] bool flush_region_unlocked(RegionKey rk) {
        auto it = regions_.find(rk);
        if (it == regions_.end()) {
            return true;
        }
        RegionEntry& region = it->second;

        std::array<std::optional<CompressedChunkBlob>, RegionKey::kChunksPerRegion> merged{};
        (void)load_existing_region_file(rk, merged);

        for (std::size_t i = 0; i < RegionKey::kChunksPerRegion; ++i) {
            if (region.blobs[i].has_value()) {
                merged[i] = region.blobs[i];
            }
        }

        std::array<RegionTocEntry, RegionKey::kChunksPerRegion> toc{};
        std::vector<uint8_t> combined_payload;

        for (std::size_t i = 0; i < RegionKey::kChunksPerRegion; ++i) {
            if (!merged[i].has_value()) {
                continue;
            }
            const CompressedChunkBlob& b = *merged[i];
            RegionTocEntry& e = toc[i];
            e.form = static_cast<uint8_t>(b.form);
            e.base_material_id = b.base_material_id;
            e.has_material_plane = b.has_material_plane ? 1U : 0U;
            e.payload_offset = static_cast<uint32_t>(combined_payload.size());
            e.payload_size = static_cast<uint32_t>(b.payload.size());
            combined_payload.insert(combined_payload.end(), b.payload.begin(), b.payload.end());
        }

        RegionFileHeader hdr{};
        hdr.rx = rk.rx;
        hdr.ry = rk.ry;
        hdr.rz = rk.rz;
        hdr.payload_checksum = compute_fnv1a64(combined_payload.data(), combined_payload.size());

        const std::filesystem::path tmp_path = region_tmp_path(rk);
        const std::filesystem::path live_path = region_file_path(rk);

        FILE* fp = std::fopen(tmp_path.c_str(), "wb");
        if (!fp) {
            return false;
        }
        const bool write_ok =
            (std::fwrite(&hdr, sizeof(RegionFileHeader), 1, fp) == 1) &&
            (std::fwrite(toc.data(), sizeof(RegionTocEntry), RegionKey::kChunksPerRegion, fp) ==
             RegionKey::kChunksPerRegion) &&
            (combined_payload.empty() ||
             std::fwrite(combined_payload.data(), 1, combined_payload.size(), fp) == combined_payload.size());

        std::fflush(fp);
#if defined(__unix__) || defined(__APPLE__)
        const int fd = fileno(fp);
        if (fd >= 0) {
            (void)::fsync(fd);
        }
#endif
        std::fclose(fp);

        if (!write_ok) {
            return false;
        }

        std::error_code ec;
        std::filesystem::rename(tmp_path, live_path, ec);
        if (ec) {
            return false;
        }

        for (auto& b : region.blobs) {
            if (b.has_value()) {
                b->dirty_for_disk = false;
            }
        }
        region.dirty_bytes = 0;
        region.has_dirty_blobs = false;
        return true;
    }

    MemoryLedger& ledger_;
    std::filesystem::path save_dir_;
    mutable std::mutex mutex_;
    std::unordered_map<RegionKey, RegionEntry, RegionKeyHash> regions_;
};

// ============================================================================
// 5. Continuous Asynchronous Chunk Streaming Manager (HLD v4.0 Section 3.3)
// ============================================================================
class StreamingManager {
public:
    StreamingManager(ChunkStore& store,
                     DualDirtyScheduler& dirty_scheduler,
                     RegionSaveManager& save_manager,
                     CoreWorkerPool& worker_pool)
        : store_(store),
          dirty_scheduler_(dirty_scheduler),
          save_manager_(save_manager),
          worker_pool_(worker_pool) {}

    std::vector<ChunkKey> update_stream_focus(Vec3f position,
                                              Vec3f velocity,
                                              int32_t active_radius_chunks,
                                              int32_t prefetch_radius_chunks) {
        const ChunkKey focus_ck = ChunkKey::from_world_sample(
            static_cast<int32_t>(std::floor(position.x)),
            static_cast<int32_t>(std::floor(position.y)),
            static_cast<int32_t>(std::floor(position.z)));

        if (has_last_focus_ &&
            focus_ck == last_focus_ck_ &&
            active_radius_chunks == last_active_radius_ &&
            prefetch_radius_chunks == last_prefetch_radius_) {
            return {};
        }
        has_last_focus_ = true;
        last_focus_ck_ = focus_ck;
        last_active_radius_ = active_radius_chunks;
        last_prefetch_radius_ = prefetch_radius_chunks;

        dirty_scheduler_.set_focus_chunk(focus_ck);

        const int32_t world_h = store_.world_height_chunks();
        const int32_t closure_radius = active_radius_chunks + 1;

        struct CandidateChunk {
            ChunkKey key{};
            bool in_active_set = false;
            float priority_score = 0.0f;
        };

        std::vector<CandidateChunk> candidates;
        std::unordered_set<ChunkKey, ChunkKeyHash> desired_residency_set;

        const int32_t max_r = std::max(closure_radius, active_radius_chunks + prefetch_radius_chunks);
        for (int32_t dz = -max_r; dz <= max_r; ++dz) {
            for (int32_t dy = -max_r; dy <= max_r; ++dy) {
                const int32_t cy = focus_ck.y() + dy;
                if (cy < 0 || cy >= world_h) {
                    continue;
                }
                for (int32_t dx = -max_r; dx <= max_r; ++dx) {
                    const int32_t chebyshev = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                    const ChunkKey ck = ChunkKey::from_coords(
                        focus_ck.x() + dx, cy, focus_ck.z() + dz);

                    const float dist_sq = static_cast<float>(dx * dx + dy * dy + dz * dz);
                    const float vel_dot =
                        static_cast<float>(dx) * velocity.x +
                        static_cast<float>(dy) * velocity.y +
                        static_cast<float>(dz) * velocity.z;

                    if (chebyshev <= closure_radius) {
                        desired_residency_set.insert(ck);
                        candidates.push_back(CandidateChunk{
                            ck,
                            /*in_active_set=*/(chebyshev <= active_radius_chunks),
                            dist_sq - 0.25f * vel_dot
                        });
                    } else if (vel_dot > 0.0f &&
                               store_.ledger().total_reserved_bytes() * 4ULL <
                                   store_.ledger().budget_bytes() * 3ULL) {
                        desired_residency_set.insert(ck);
                        candidates.push_back(CandidateChunk{
                            ck,
                            /*in_active_set=*/false,
                            dist_sq - 0.5f * vel_dot
                        });
                    }
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const CandidateChunk& a, const CandidateChunk& b) {
                      return a.priority_score < b.priority_score;
                  });

        // 1. Evict chunks that fell outside desired_residency_set.
        std::vector<ChunkKey> unloaded;
        std::vector<ChunkKey> to_evict;
        for (const ChunkKey rk : tracked_resident_) {
            if (desired_residency_set.find(rk) == desired_residency_set.end()) {
                to_evict.push_back(rk);
            }
        }
        for (const ChunkKey ek : to_evict) {
            if (const ChunkSlot* slot = store_.find_slot(ek); slot != nullptr && slot->dirty_persist) {
                (void)save_manager_.persist_compress_chunk(store_, ek);
            }
            if (store_.evict_slot(ek)) {
                tracked_resident_.erase(ek);
                meshed_active_.erase(ek);
                requested_chunks_.erase(ek);
                unloaded.push_back(ek);
            }
        }

        // 2. Dispatch background generation / loading on worker threads and gate active meshing
        //    on 26-neighbor closure residency so no chunk ever meshes against Absent neighbors!
        for (const CandidateChunk& cand : candidates) {
            ChunkSlot* slot = store_.find_slot(cand.key);
            const bool is_resident = (slot != nullptr && slot->state == ChunkLifecycleState::Resident);
            if (is_resident) {
                tracked_resident_.insert(cand.key);
                if (cand.in_active_set && meshed_active_.find(cand.key) == meshed_active_.end()) {
                    meshed_active_.insert(cand.key);
                    worker_pool_.register_awaiting_closure_mesh(cand.key);
                }
            } else if (requested_chunks_.find(cand.key) == requested_chunks_.end()) {
                requested_chunks_.insert(cand.key);
                if (cand.in_active_set) {
                    meshed_active_.insert(cand.key);
                }
                worker_pool_.request_background_generate(cand.key, cand.in_active_set);
            } else if (cand.in_active_set && meshed_active_.find(cand.key) == meshed_active_.end()) {
                // Previously requested as a closure/prefetch shell chunk, now promoted into active set!
                meshed_active_.insert(cand.key);
                worker_pool_.register_awaiting_closure_mesh(cand.key);
            }
        }

        return unloaded;
    }

    [[nodiscard]] std::size_t resident_chunk_count() const noexcept {
        return tracked_resident_.size();
    }

private:
    ChunkStore& store_;
    DualDirtyScheduler& dirty_scheduler_;
    RegionSaveManager& save_manager_;
    CoreWorkerPool& worker_pool_;
    bool has_last_focus_ = false;
    ChunkKey last_focus_ck_{};
    int32_t last_active_radius_ = -1;
    int32_t last_prefetch_radius_ = -1;
    std::unordered_set<ChunkKey, ChunkKeyHash> tracked_resident_;
    std::unordered_set<ChunkKey, ChunkKeyHash> meshed_active_;
    std::unordered_set<ChunkKey, ChunkKeyHash> requested_chunks_;
};

} // namespace voxel