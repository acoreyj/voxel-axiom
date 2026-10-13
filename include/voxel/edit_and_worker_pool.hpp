#pragma once

#include "voxel/scratch_and_dirty.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace voxel {

// ============================================================================
// 1. 3D Cave Navigation Group Key (4 x 2 x 4 Chunks, HLD v4.0 Section 4.5)
// ============================================================================
struct NavigationGroupKey {
    int32_t gx = 0; // ChunkKey::floor_div(Cx, 4)
    int32_t gy = 0; // ChunkKey::floor_div(Cy, 2)
    int32_t gz = 0; // ChunkKey::floor_div(Cz, 4)

    [[nodiscard]] static constexpr NavigationGroupKey from_chunk_key(ChunkKey ck) noexcept {
        return NavigationGroupKey{
            ChunkKey::floor_div(ck.x(), 4),
            ChunkKey::floor_div(ck.y(), 2),
            ChunkKey::floor_div(ck.z(), 4)
        };
    }

    constexpr bool operator==(const NavigationGroupKey&) const noexcept = default;
    constexpr auto operator<=>(const NavigationGroupKey&) const noexcept = default;
};

struct NavigationGroupKeyHash {
    [[nodiscard]] constexpr std::size_t operator()(const NavigationGroupKey& k) const noexcept {
        const ChunkKey packed = ChunkKey::from_coords(k.gx, k.gy, k.gz);
        return static_cast<std::size_t>(packed.hash());
    }
};

// ============================================================================
// 2. Sphere CSG Primitives & Phased Edit Transaction (Sections 3.2, 3.4, 3.5)
// ============================================================================
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr bool operator==(const Vec3f&) const noexcept = default;
};

struct SphereCsgTerm {
    Vec3f center{};
    float radius = 0.0f;
};

/// Builds a phased explosion EditTransaction with expanded R + delta influence bounds,
/// partitioned into phases of at most 32 canonical chunks (Sections 3.2, 3.5.1).
struct EditTransaction {
    static constexpr std::size_t kMaxCoalescedSpheres = 16;
    static constexpr std::size_t kMaxPhaseChunks = ChunkStore::kMaxEditPhaseCoreChunks; // 32

    std::vector<SphereCsgTerm> spheres;
    float truncation_delta = TsdfFormat::kDefaultTruncationDelta;

    // Expanded sample AABB: [floor(c - (R + delta)), ceil(c + (R + delta))]
    IVec3 sample_min{};
    IVec3 sample_max{};

    // Partitioned canonical chunks (at most 32 chunks per phase)
    std::vector<std::vector<ChunkKey>> phases;
    std::size_t next_phase_index = 0;

    // Accumulated across phases, published ONLY when the final phase completes (no mid-explosion tearing)
    std::unordered_map<ChunkKey, SelectiveDirtyMask, ChunkKeyHash> modified_chunk_masks;
    std::unordered_set<NavigationGroupKey, NavigationGroupKeyHash> affected_nav_groups;
    std::size_t max_locks_acquired_in_any_phase = 0;

    [[nodiscard]] static EditTransaction create_from_spheres(
        std::vector<SphereCsgTerm> terms,
        float delta = TsdfFormat::kDefaultTruncationDelta,
        int32_t world_height_chunks = 8);

    [[nodiscard]] bool is_finished() const noexcept {
        return next_phase_index >= phases.size();
    }
};

// ============================================================================
// 3. MPSC Completion Queue Records (HLD v4.0 Section 5.2)
// ============================================================================
enum class CompletionKind : uint8_t {
    ChunkMesh = 0,
    ChunkUnloaded = 1,
    EditTransactionCompleted = 2
};

struct ChunkMeshPayload {
    ChunkKey key{};
    uint64_t captured_version = 0;
    uint64_t captured_terrain_revision = 0;
    std::vector<Vec3f> vertices;
    std::vector<Vec3f> normals;
    std::vector<int32_t> indices;
    std::vector<Vec3f> collision_faces; // Pre-unpacked triangle soup (3 vertices per triangle)
    std::size_t reserved_ledger_bytes = 0;
};

struct EditTransactionCompletedPayload {
    Vec3f position{};
    float radius = 0.0f;
    uint64_t terrain_revision = 0;
    std::size_t total_affected_chunks = 0;
    std::vector<ChunkKey> dirtied_chunks;
    std::vector<NavigationGroupKey> affected_nav_groups;
};

struct CompletionRecord {
    CompletionKind kind = CompletionKind::EditTransactionCompleted;
    ChunkMeshPayload chunk_mesh{};
    ChunkKey unloaded_key{};
    EditTransactionCompletedPayload edit_transaction{};
};

// ============================================================================
// 4. Phased CSG Executor & 3x3x3 Anti-Crumb Kernel (Section 3.5.1)
// ============================================================================

/// Executes a single phase (at most 32 canonical chunks) of `tx` against `store`.
/// Does NOT publish dirty chunks during intermediate phases.
/// On the final phase, runs the 3x3x3 Anti-Crumb Kernel across modified chunks,
/// increments `terrain_revision` once, publishes the full deduplicated dirty batch
/// to `dirty_scheduler`, and returns the `EditTransactionCompletedPayload`.
[[nodiscard]] std::optional<EditTransactionCompletedPayload> execute_next_edit_phase(
    ChunkStore& store,
    DualDirtyScheduler& dirty_scheduler,
    EditTransaction& tx);

// ============================================================================
// 5. Core Worker Pool & Ordered Teardown Protocol (Sections 3.4, 5.2, 5.3)
// ============================================================================
class CoreWorkerPool {
public:
    static constexpr std::size_t kEditStarvationBound = 8;
    static constexpr std::size_t kMaxPendingEditTransactions = 64;

    using MeshGeneratorFn = std::function<ChunkMeshPayload(
        const ChunkStore&, ChunkKey, WorkerScratchWindow36&, const NeighborhoodVersionStamp&)>;
    using ShutdownSaveHookFn = std::function<void(ChunkStore&)>;

    CoreWorkerPool(ChunkStore& store,
                   DualDirtyScheduler& dirty_scheduler,
                   MeshGeneratorFn mesh_fn = {});
    ~CoreWorkerPool();

    CoreWorkerPool(const CoreWorkerPool&) = delete;
    CoreWorkerPool& operator=(const CoreWorkerPool&) = delete;

    void set_shutdown_save_hook(ShutdownSaveHookFn hook);
    void start(std::size_t num_workers = 0);

    /// Enqueues an explosion asynchronously (< 0.1 ms). Coalesces with the tail queued
    /// transaction if it has not started executing yet and has < 16 spheres (Section 3.4).
    [[nodiscard]] bool enqueue_explosion(Vec3f position, float radius);

    [[nodiscard]] std::optional<CompletionRecord> try_pop_completion();
    [[nodiscard]] std::size_t completion_queue_depth() const noexcept;
    [[nodiscard]] bool core_idle() const noexcept;

    /// Deterministic Ordered Teardown Protocol (HLD v4.0 Section 5.3 Steps 1-6).
    void shutdown(bool save);

private:
    void push_completion(CompletionRecord rec);
    void worker_loop();

    ChunkStore& store_;
    DualDirtyScheduler& dirty_scheduler_;
    MeshGeneratorFn mesh_fn_;
    ShutdownSaveHookFn shutdown_save_hook_;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::deque<EditTransaction> edit_queue_;
    bool edit_queue_in_progress_ = false;

    mutable std::mutex completion_mutex_;
    std::deque<CompletionRecord> completion_queue_;
    std::atomic<std::size_t> completion_depth_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<std::size_t> active_workers_{0};
    std::vector<std::thread> workers_;
};

} // namespace voxel
