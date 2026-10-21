#pragma once

#include "voxel_mesh_baker.hpp"
#include "godot_wrapper_and_nav3d.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace voxel {

enum class StampBlendMode : uint8_t {
    Union = 0,     // Solid voxels from asset merge with existing world (q_new = min(q_world, q_asset))
    Overwrite = 1  // Asset completely replaces chunk voxels where asset is non-empty
};

struct StampedStructureInstance {
    uint32_t id = 0;
    std::string name;
    ChunkKey origin_chunk{};
    StampBlendMode blend_mode = StampBlendMode::Union;
    float recreate_delay_seconds = 0.0f; // <= 0 means stay destroyed permanently

    bool is_damaged = false;
    float time_since_damaged = 0.0f;

    std::vector<ChunkKey> stamped_chunk_keys;

    using ChunkBaseline = ChunkStateSnapshot;

    // Pre-stamp chunk state captured before stamping (restored by remove_structure)
    std::unordered_map<ChunkKey, ChunkBaseline, ChunkKeyHash> pre_stamp_baselines;
    // Post-stamp chunk state captured immediately after stamping (restored by process_reconstitution)
    std::unordered_map<ChunkKey, ChunkBaseline, ChunkKeyHash> baselines;
};

class AssetStampManager {
public:
    using StructureEventFn = std::function<void(uint32_t structure_id, const std::string& name)>;

    /// Snapshot of the whole structure registry used by Undo/Redo entries so a
    /// restored chunk state is accompanied by the matching structure bookkeeping.
    struct ManagerSnapshot {
        uint32_t next_structure_id = 0;
        std::unordered_map<uint32_t, StampedStructureInstance> structures;
    };

    void set_structure_damaged_callback(StructureEventFn cb) {
        on_structure_damaged_ = std::move(cb);
    }

    void set_structure_recreated_callback(StructureEventFn cb) {
        on_structure_recreated_ = std::move(cb);
    }

    void set_structure_removed_callback(StructureEventFn cb) {
        on_structure_removed_ = std::move(cb);
    }

    [[nodiscard]] ManagerSnapshot capture_manager_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ManagerSnapshot{next_structure_id_, structures_};
    }

    void restore_manager_snapshot(const ManagerSnapshot& snap) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_structure_id_ = snap.next_structure_id;
        structures_ = snap.structures;
    }

    /// Stamps an offline baked BakedRegionData into the live ChunkStore at the given origin chunk.
    [[nodiscard]] uint32_t stamp_region(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        const BakedRegionData& region_data,
        ChunkKey origin_chunk,
        float recreate_delay_seconds = 0.0f,
        StampBlendMode blend_mode = StampBlendMode::Union,
        std::string asset_name = "ruin",
        const std::function<void(ChunkStore&, ChunkKey)>& ensure_chunk_ready_fn = {},
        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash>* out_before_chunks = nullptr,
        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash>* out_after_chunks = nullptr) {

        std::lock_guard<std::mutex> lock(mutex_);
        const uint32_t struct_id = ++next_structure_id_;

        StampedStructureInstance inst{};
        inst.id = struct_id;
        inst.name = std::move(asset_name);
        inst.origin_chunk = origin_chunk;
        inst.blend_mode = blend_mode;
        inst.recreate_delay_seconds = recreate_delay_seconds;
        inst.is_damaged = false;
        inst.time_since_damaged = 0.0f;

        std::unordered_set<ChunkKey, ChunkKeyHash> unique_dirtied_chunks;
        std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash> affected_nav_groups;

        // Decompress and stamp each non-empty chunk from the region data
        for (std::size_t i = 0; i < RegionKey::kChunksPerRegion; ++i) {
            const RegionTocEntry& entry = region_data.toc[i];
            const auto form = static_cast<SerializedChunkForm>(entry.form);
            if (form == SerializedChunkForm::Untouched || form == SerializedChunkForm::Empty) {
                continue;
            }

            const int32_t lx = static_cast<int32_t>(i % 8);
            const int32_t ly = static_cast<int32_t>((i / 8) % 8);
            const int32_t lz = static_cast<int32_t>(i / 64);

            const ChunkKey target_key = origin_chunk.offset(lx, ly, lz);
            ChunkSlot* slot = store.get_or_create_slot(target_key);

            if (ensure_chunk_ready_fn) {
                const int32_t world_h = store.world_height_chunks();
                for (int32_t dz = -1; dz <= 1; ++dz) {
                    for (int32_t dy = -1; dy <= 1; ++dy) {
                        const int32_t ny = target_key.y() + dy;
                        if (ny < 0 || ny >= world_h) continue;
                        for (int32_t dx = -1; dx <= 1; ++dx) {
                            const ChunkKey nk = target_key.offset(dx, dy, dz);
                            ChunkSlot* nslot = store.get_or_create_slot(nk);
                            bool needs_gen = false;
                            {
                                std::shared_lock<std::shared_mutex> rl(nslot->mutex);
                                needs_gen = (nslot->state != ChunkLifecycleState::Resident);
                            }
                            if (needs_gen) {
                                ensure_chunk_ready_fn(store, nk);
                            }
                        }
                    }
                }
            }

            std::unique_lock<std::shared_mutex> slot_lock(slot->mutex);

            // 1. Capture pre-stamp baseline before modifying the chunk
            inst.pre_stamp_baselines[target_key] = capture_baseline_unlocked(*slot);

            if (form == SerializedChunkForm::Solid) {
                if (blend_mode == StampBlendMode::Overwrite || slot->form == ChunkStorageForm::Empty) {
                    slot->reset_to_uniform(ChunkStorageForm::Solid, entry.base_material_id,
                                           store.tsdf_pool(), store.material_pool());
                } else if (slot->form == ChunkStorageForm::Dense) {
                    // Union with solid means entire chunk becomes solid
                    slot->reset_to_uniform(ChunkStorageForm::Solid, entry.base_material_id,
                                           store.tsdf_pool(), store.material_pool());
                }
            } else if (form == SerializedChunkForm::Dense) {
                const bool has_col = (entry.has_color_plane != 0);
                const std::size_t hdr_bytes = has_col ? 20ULL : 12ULL;
                if (entry.payload_size < hdr_bytes) {
                    continue;
                }
                const uint8_t* p = region_data.payload.data() + entry.payload_offset;
                uint32_t low_sz = 0, high_sz = 0, mat_sz = 0, col_low_sz = 0, col_high_sz = 0;
                std::memcpy(&low_sz, p + 0, sizeof(uint32_t));
                std::memcpy(&high_sz, p + 4, sizeof(uint32_t));
                std::memcpy(&mat_sz, p + 8, sizeof(uint32_t));
                if (has_col) {
                    std::memcpy(&col_low_sz, p + 12, sizeof(uint32_t));
                    std::memcpy(&col_high_sz, p + 16, sizeof(uint32_t));
                }

                std::vector<uint8_t> low_plane(TsdfFormat::kChunkSamples);
                std::vector<uint8_t> high_plane(TsdfFormat::kChunkSamples);
                std::vector<uint8_t> mat_plane(TsdfFormat::kChunkSamples);
                std::vector<uint16_t> col_plane(TsdfFormat::kChunkSamples, VoxelColor::kUnpainted);

                const uint8_t* ptr = p + hdr_bytes;
                if (!BytePlaneCodec::decompress_plane(ptr, low_sz, low_plane.data(), low_plane.size())) continue;
                ptr += low_sz;
                if (!BytePlaneCodec::decompress_plane(ptr, high_sz, high_plane.data(), high_plane.size())) continue;
                ptr += high_sz;

                std::vector<int16_t> asset_tsdf(TsdfFormat::kChunkSamples);
                BytePlaneCodec::merge_int16_planes(low_plane.data(), high_plane.data(), asset_tsdf.data());

                const bool has_mat = (entry.has_material_plane != 0) && (mat_sz > 0);
                if (has_mat) {
                    (void)BytePlaneCodec::decompress_plane(ptr, mat_sz, mat_plane.data(), mat_plane.size());
                    ptr += mat_sz;
                } else {
                    std::fill(mat_plane.begin(), mat_plane.end(), entry.base_material_id);
                }

                if (has_col && col_low_sz > 0 && col_high_sz > 0) {
                    std::vector<uint8_t> col_low(TsdfFormat::kChunkSamples);
                    std::vector<uint8_t> col_high(TsdfFormat::kChunkSamples);
                    if (BytePlaneCodec::decompress_plane(ptr, col_low_sz, col_low.data(), col_low.size())) {
                        ptr += col_low_sz;
                        if (BytePlaneCodec::decompress_plane(ptr, col_high_sz, col_high.data(), col_high.size())) {
                            BytePlaneCodec::merge_int16_planes(
                                col_low.data(), col_high.data(), reinterpret_cast<int16_t*>(col_plane.data()));
                        }
                    }
                }

                if (blend_mode == StampBlendMode::Overwrite || slot->form == ChunkStorageForm::Empty) {
                    slot->reset_to_uniform(ChunkStorageForm::Solid, entry.base_material_id,
                                           store.tsdf_pool(), store.material_pool());
                    (void)slot->promote_to_dense(store.tsdf_pool());
                    std::memcpy(slot->tsdf_data, asset_tsdf.data(), TsdfFormat::kTsdfSlabBytes);
                    for (uint32_t s = 0; s < TsdfFormat::kChunkSamples; ++s) {
                        (void)slot->set_material_linear(s, mat_plane[s], store.material_pool());
                        if (col_plane[s] != VoxelColor::kUnpainted) {
                            (void)slot->set_color_linear(s, col_plane[s], store.tsdf_pool());
                        }
                    }
                } else {
                    // Union mode: min(q_existing, q_asset)
                    (void)slot->promote_to_dense(store.tsdf_pool());
                    for (uint32_t s = 0; s < TsdfFormat::kChunkSamples; ++s) {
                        const int16_t q_cur = slot->get_tsdf_linear(s);
                        const int16_t q_ast = asset_tsdf[s];
                        const int16_t q_res = std::min(q_cur, q_ast);
                        slot->tsdf_data[s] = q_res;
                        if (q_ast < 0) {
                            (void)slot->set_material_linear(s, mat_plane[s], store.material_pool());
                            if (col_plane[s] != VoxelColor::kUnpainted) {
                                (void)slot->set_color_linear(s, col_plane[s], store.tsdf_pool());
                            } else {
                                (void)slot->clear_color(LocalCoord::from_linear_index(s));
                            }
                        }
                    }
                }
            }

            slot->try_demote_if_uniform(store.tsdf_pool(), store.material_pool());
            slot->version.fetch_add(1, std::memory_order_release);
            slot->state = ChunkLifecycleState::Resident;
            slot->dirty_persist = true;

            // 2. Capture post-stamp baseline for reconstitution
            inst.baselines[target_key] = capture_baseline_unlocked(*slot);

            inst.stamped_chunk_keys.push_back(target_key);
            add_chunk_and_resident_neighbors_to_dirty(
                store, target_key, unique_dirtied_chunks, affected_nav_groups);
        }

        if (out_before_chunks) {
            *out_before_chunks = inst.pre_stamp_baselines;
        }
        if (out_after_chunks) {
            *out_after_chunks = inst.baselines;
        }

        publish_dirty_and_nav_update(store, dirty_scheduler, nav_mgr, unique_dirtied_chunks, affected_nav_groups);

        structures_.emplace(struct_id, std::move(inst));
        return struct_id;
    }

    /// Stamps an asset from a .vreg file on disk.
    [[nodiscard]] uint32_t stamp_region_file(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        const std::string& vreg_filepath,
        ChunkKey origin_chunk,
        float recreate_delay_seconds = 0.0f,
        StampBlendMode blend_mode = StampBlendMode::Union,
        std::string asset_name = "",
        const std::function<void(ChunkStore&, ChunkKey)>& ensure_chunk_ready_fn = {},
        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash>* out_before_chunks = nullptr,
        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash>* out_after_chunks = nullptr) {

        FILE* fp = std::fopen(vreg_filepath.c_str(), "rb");
        if (!fp) {
            return 0;
        }

        BakedRegionData region{};
        if (std::fread(&region.header, sizeof(RegionFileHeader), 1, fp) != 1 ||
            std::memcmp(region.header.magic, "VREG", 4) != 0) {
            std::fclose(fp);
            return 0;
        }

        if (std::fread(region.toc.data(), sizeof(RegionTocEntry), RegionKey::kChunksPerRegion, fp) !=
            RegionKey::kChunksPerRegion) {
            std::fclose(fp);
            return 0;
        }

        const long payload_start = std::ftell(fp);
        std::fseek(fp, 0, SEEK_END);
        const long file_end = std::ftell(fp);
        const std::size_t payload_len = static_cast<std::size_t>(file_end - payload_start);
        region.payload.resize(payload_len);
        std::fseek(fp, payload_start, SEEK_SET);
        if (payload_len > 0 && std::fread(region.payload.data(), 1, payload_len, fp) != payload_len) {
            std::fclose(fp);
            return 0;
        }
        std::fclose(fp);

        if (asset_name.empty()) {
            asset_name = vreg_filepath;
        }

        return stamp_region(store, dirty_scheduler, nav_mgr, region, origin_chunk,
                            recreate_delay_seconds, blend_mode, std::move(asset_name),
                            ensure_chunk_ready_fn, out_before_chunks, out_after_chunks);
    }

    /// Removes a previously stamped structure by `structure_id`, restoring every affected chunk
    /// to its exact pre-stamp baseline (TSDF, materials, and colors) and triggering remeshing + nav sync.
    bool remove_structure(
        uint32_t structure_id,
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash>* out_before_chunks = nullptr,
        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash>* out_after_chunks = nullptr) {

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = structures_.find(structure_id);
        if (it == structures_.end()) {
            return false;
        }

        const StampedStructureInstance inst = std::move(it->second);
        structures_.erase(it);

        std::unordered_set<ChunkKey, ChunkKeyHash> unique_dirtied_chunks;
        std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash> affected_nav_groups;

        for (const auto& [ck, pre_base] : inst.pre_stamp_baselines) {
            ChunkSlot* slot = store.get_or_create_slot(ck);
            std::unique_lock<std::shared_mutex> slot_lock(slot->mutex);
            if (out_before_chunks) {
                (*out_before_chunks)[ck] = capture_baseline_unlocked(*slot);
            }
            restore_baseline_unlocked(*slot, pre_base, store);
            if (out_after_chunks) {
                (*out_after_chunks)[ck] = pre_base;
            }
            add_chunk_and_resident_neighbors_to_dirty(
                store, ck, unique_dirtied_chunks, affected_nav_groups);
        }

        publish_dirty_and_nav_update(store, dirty_scheduler, nav_mgr, unique_dirtied_chunks, affected_nav_groups);

        if (on_structure_removed_) {
            on_structure_removed_(inst.id, inst.name);
        }
        return true;
    }

    /// Notifies the stamp manager that a chunk was damaged by an explosion or elemental blast.
    void notify_chunk_damaged(ChunkKey ck) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, inst] : structures_) {
            if (std::find(inst.stamped_chunk_keys.begin(), inst.stamped_chunk_keys.end(), ck) !=
                inst.stamped_chunk_keys.end()) {
                if (!inst.is_damaged) {
                    inst.is_damaged = true;
                    inst.time_since_damaged = 0.0f;
                    if (on_structure_damaged_) {
                        on_structure_damaged_(inst.id, inst.name);
                    }
                } else {
                    // Reset damage timer on subsequent hits so it only recreates after a quiet period
                    inst.time_since_damaged = 0.0f;
                }
            }
        }
    }

    /// Processes reconstitution timers for all stamped structures.
    /// Reconstitutes structures whose timer has elapsed, restoring exact original TSDF, materials, and colors.
    std::vector<uint32_t> process_reconstitution(
        float delta_seconds,
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr) {

        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint32_t> recreated_ids;

        for (auto& [id, inst] : structures_) {
            if (!inst.is_damaged || inst.recreate_delay_seconds <= 0.0f) {
                // If recreate_delay_seconds <= 0, structure stays destroyed permanently!
                continue;
            }

            inst.time_since_damaged += delta_seconds;
            if (inst.time_since_damaged >= inst.recreate_delay_seconds) {
                // Timer expired -> Recreate structure!
                std::unordered_set<ChunkKey, ChunkKeyHash> unique_dirtied_chunks;
                std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash> affected_nav_groups;

                for (const auto& [ck, base] : inst.baselines) {
                    ChunkSlot* slot = store.get_or_create_slot(ck);
                    std::unique_lock<std::shared_mutex> slot_lock(slot->mutex);
                    restore_baseline_unlocked(*slot, base, store);
                    add_chunk_and_resident_neighbors_to_dirty(
                        store, ck, unique_dirtied_chunks, affected_nav_groups);
                }

                publish_dirty_and_nav_update(
                    store, dirty_scheduler, nav_mgr, unique_dirtied_chunks, affected_nav_groups);

                inst.is_damaged = false;
                inst.time_since_damaged = 0.0f;
                recreated_ids.push_back(inst.id);

                if (on_structure_recreated_) {
                    on_structure_recreated_(inst.id, inst.name);
                }
            }
        }

        return recreated_ids;
    }

    [[nodiscard]] const StampedStructureInstance* find_structure(uint32_t id) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = structures_.find(id);
        return (it != structures_.end()) ? &it->second : nullptr;
    }

    [[nodiscard]] std::size_t active_structure_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return structures_.size();
    }

private:
    [[nodiscard]] static StampedStructureInstance::ChunkBaseline capture_baseline_unlocked(
        const ChunkSlot& slot) {
        return ChunkStateSnapshot::capture_unlocked(slot);
    }

    static void restore_baseline_unlocked(
        ChunkSlot& slot,
        const StampedStructureInstance::ChunkBaseline& base,
        ChunkStore& store) {
        ChunkStateSnapshot::restore_unlocked(slot, base, store);
    }

public:
    static void add_chunk_and_resident_neighbors_to_dirty(
        const ChunkStore& store,
        ChunkKey ck,
        std::unordered_set<ChunkKey, ChunkKeyHash>& unique_dirtied,
        std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash>& affected_nav_groups) {
        unique_dirtied.insert(ck);
        affected_nav_groups.insert(NavigationGroupKey::from_chunk_key(ck));
        const int32_t world_h = store.world_height_chunks();
        for (int32_t dz = -1; dz <= 1; ++dz) {
            for (int32_t dy = -1; dy <= 1; ++dy) {
                const int32_t ny = ck.y() + dy;
                if (ny < 0 || ny >= world_h) continue;
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    const ChunkKey nk = ck.offset(dx, dy, dz);
                    if (store.find_slot(nk) != nullptr) {
                        unique_dirtied.insert(nk);
                        affected_nav_groups.insert(NavigationGroupKey::from_chunk_key(nk));
                    }
                }
            }
        }
    }

    static void publish_dirty_and_nav_update(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        const std::unordered_set<ChunkKey, ChunkKeyHash>& unique_dirtied_chunks,
        const std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash>& affected_nav_groups) {
        if (unique_dirtied_chunks.empty()) {
            return;
        }
        std::vector<ChunkKey> dirtied_chunks(unique_dirtied_chunks.begin(), unique_dirtied_chunks.end());
        std::sort(dirtied_chunks.begin(), dirtied_chunks.end());

        const uint64_t new_rev = store.increment_terrain_revision();
        dirty_scheduler.publish_edit_transaction_dirty_batch(dirtied_chunks);

        EditTransactionCompletedPayload payload{};
        payload.terrain_revision = new_rev;
        payload.total_affected_chunks = dirtied_chunks.size();
        payload.dirtied_chunks = std::move(dirtied_chunks);
        payload.affected_nav_groups.assign(affected_nav_groups.begin(), affected_nav_groups.end());
        std::sort(payload.affected_nav_groups.begin(), payload.affected_nav_groups.end());
        nav_mgr.apply_edit_transaction(payload);
    }

private:
    mutable std::mutex mutex_;
    uint32_t next_structure_id_ = 0;
    std::unordered_map<uint32_t, StampedStructureInstance> structures_;
    StructureEventFn on_structure_damaged_;
    StructureEventFn on_structure_recreated_;
    StructureEventFn on_structure_removed_;
};

// ============================================================================
// Full Chunk-Snapshot & Structure-State Undo/Redo Manager
// ============================================================================
struct UndoRedoEntry {
    std::string action_name;
    std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash> before_chunks;
    std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash> after_chunks;
    AssetStampManager::ManagerSnapshot stamps_before;
    AssetStampManager::ManagerSnapshot stamps_after;
};

class VoxelUndoRedoManager {
public:
    explicit VoxelUndoRedoManager(std::size_t max_steps = 64) noexcept
        : max_steps_(std::max<std::size_t>(1, max_steps)) {}

    void set_max_steps(std::size_t max_steps) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_steps_ = std::max<std::size_t>(1, max_steps);
        while (undo_stack_.size() > max_steps_) {
            undo_stack_.pop_front();
        }
    }

    [[nodiscard]] std::size_t max_steps() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_steps_;
    }

    void record_entry(UndoRedoEntry entry) {
        if (entry.before_chunks.empty() && entry.after_chunks.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        redo_stack_.clear();
        undo_stack_.push_back(std::move(entry));
        while (undo_stack_.size() > max_steps_) {
            undo_stack_.pop_front();
        }
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        undo_stack_.clear();
        redo_stack_.clear();
    }

    [[nodiscard]] bool can_undo() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return !undo_stack_.empty();
    }

    [[nodiscard]] bool can_redo() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return !redo_stack_.empty();
    }

    [[nodiscard]] std::size_t undo_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return undo_stack_.size();
    }

    [[nodiscard]] std::size_t redo_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return redo_stack_.size();
    }

    bool undo(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        AssetStampManager& stamp_mgr,
        RegionSaveManager* save_mgr = nullptr,
        std::size_t* out_affected_chunks = nullptr) {

        UndoRedoEntry entry{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (undo_stack_.empty()) {
                if (out_affected_chunks) *out_affected_chunks = 0;
                return false;
            }
            entry = std::move(undo_stack_.back());
            undo_stack_.pop_back();
        }

        const std::size_t affected = apply_chunk_map_and_stamps(
            store, dirty_scheduler, nav_mgr, stamp_mgr, save_mgr,
            entry.before_chunks, entry.stamps_before);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            redo_stack_.push_back(std::move(entry));
        }
        if (out_affected_chunks) {
            *out_affected_chunks = affected;
        }
        return true;
    }

    bool redo(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        AssetStampManager& stamp_mgr,
        RegionSaveManager* save_mgr = nullptr,
        std::size_t* out_affected_chunks = nullptr) {

        UndoRedoEntry entry{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (redo_stack_.empty()) {
                if (out_affected_chunks) *out_affected_chunks = 0;
                return false;
            }
            entry = std::move(redo_stack_.back());
            redo_stack_.pop_back();
        }

        const std::size_t affected = apply_chunk_map_and_stamps(
            store, dirty_scheduler, nav_mgr, stamp_mgr, save_mgr,
            entry.after_chunks, entry.stamps_after);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            undo_stack_.push_back(std::move(entry));
            while (undo_stack_.size() > max_steps_) {
                undo_stack_.pop_front();
            }
        }
        if (out_affected_chunks) {
            *out_affected_chunks = affected;
        }
        return true;
    }

    /// Undoes all recorded operations in a single coalesced pass, restoring the earliest
    /// pre-edit state of every modified chunk without intermediate remeshing.
    std::size_t undo_all(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        AssetStampManager& stamp_mgr,
        RegionSaveManager* save_mgr = nullptr,
        std::size_t* out_affected_chunks = nullptr) {

        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash> coalesced_before;
        AssetStampManager::ManagerSnapshot earliest_stamps{};
        std::size_t steps_undone = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (undo_stack_.empty()) {
                if (out_affected_chunks) *out_affected_chunks = 0;
                return 0;
            }
            steps_undone = undo_stack_.size();
            earliest_stamps = undo_stack_.front().stamps_before;

            // Walk from newest to oldest so the oldest entry overwrites with the earliest pre-edit state
            for (auto it = undo_stack_.rbegin(); it != undo_stack_.rend(); ++it) {
                for (const auto& [ck, snap] : it->before_chunks) {
                    coalesced_before[ck] = snap;
                }
            }
            while (!undo_stack_.empty()) {
                redo_stack_.push_back(std::move(undo_stack_.back()));
                undo_stack_.pop_back();
            }
        }

        const std::size_t affected = apply_chunk_map_and_stamps(
            store, dirty_scheduler, nav_mgr, stamp_mgr, save_mgr,
            coalesced_before, earliest_stamps);

        if (out_affected_chunks) {
            *out_affected_chunks = affected;
        }
        return steps_undone;
    }

    /// Redoes all operations currently on the redo stack in a single coalesced pass.
    std::size_t redo_all(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        AssetStampManager& stamp_mgr,
        RegionSaveManager* save_mgr = nullptr,
        std::size_t* out_affected_chunks = nullptr) {

        std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash> coalesced_after;
        AssetStampManager::ManagerSnapshot latest_stamps{};
        std::size_t steps_redone = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (redo_stack_.empty()) {
                if (out_affected_chunks) *out_affected_chunks = 0;
                return 0;
            }
            steps_redone = redo_stack_.size();
            latest_stamps = redo_stack_.front().stamps_after;

            // Walk redo_stack_ from back (oldest undone) to front (most recently undone = latest state)
            for (auto it = redo_stack_.rbegin(); it != redo_stack_.rend(); ++it) {
                for (const auto& [ck, snap] : it->after_chunks) {
                    coalesced_after[ck] = snap;
                }
            }
            while (!redo_stack_.empty()) {
                undo_stack_.push_back(std::move(redo_stack_.back()));
                redo_stack_.pop_back();
            }
        }

        const std::size_t affected = apply_chunk_map_and_stamps(
            store, dirty_scheduler, nav_mgr, stamp_mgr, save_mgr,
            coalesced_after, latest_stamps);

        if (out_affected_chunks) {
            *out_affected_chunks = affected;
        }
        return steps_redone;
    }

private:
    static std::size_t apply_chunk_map_and_stamps(
        ChunkStore& store,
        DualDirtyScheduler& dirty_scheduler,
        VoxelNavigationManager& nav_mgr,
        AssetStampManager& stamp_mgr,
        RegionSaveManager* save_mgr,
        const std::unordered_map<ChunkKey, ChunkStateSnapshot, ChunkKeyHash>& chunk_map,
        const AssetStampManager::ManagerSnapshot& stamps_snap) {

        std::unordered_set<ChunkKey, ChunkKeyHash> unique_dirtied_chunks;
        std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash> affected_nav_groups;

        std::vector<ChunkKey> sorted_keys;
        sorted_keys.reserve(chunk_map.size());
        for (const auto& [ck, _] : chunk_map) {
            sorted_keys.push_back(ck);
        }
        std::sort(sorted_keys.begin(), sorted_keys.end());

        for (const ChunkKey ck : sorted_keys) {
            const auto it = chunk_map.find(ck);
            if (it == chunk_map.end()) continue;
            ChunkSlot* slot = store.get_or_create_slot(ck);
            {
                std::unique_lock<std::shared_mutex> slot_lock(slot->mutex);
                ChunkStateSnapshot::restore_unlocked(*slot, it->second, store);
            }
            if (save_mgr != nullptr) {
                (void)save_mgr->persist_compress_chunk(store, ck);
            }
            AssetStampManager::add_chunk_and_resident_neighbors_to_dirty(
                store, ck, unique_dirtied_chunks, affected_nav_groups);
        }

        stamp_mgr.restore_manager_snapshot(stamps_snap);
        AssetStampManager::publish_dirty_and_nav_update(
            store, dirty_scheduler, nav_mgr, unique_dirtied_chunks, affected_nav_groups);
        return sorted_keys.size();
    }

    mutable std::mutex mutex_;
    std::size_t max_steps_ = 64;
    std::deque<UndoRedoEntry> undo_stack_;
    std::deque<UndoRedoEntry> redo_stack_;
};

} // namespace voxel
