#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace voxel {

// ============================================================================
// Instrumentation: per-operation wall-clock timing (HLD v4.0 Section 7.3)
//
// Worker threads record durations into a lock-free StageTimings accumulator;
// the main thread drains it once per frame and logs a summary. The header is
// deliberately Godot-free so the pure C++ core and its tests can time work
// without pulling in engine headers.
// ============================================================================

enum class Stage : std::size_t {
    Generate = 0, // Per-chunk procedural generation / disk load
    Mesh,         // 36^3 scratch gather + Surface Nets meshing
    Commit,       // Main-thread mesh upload + collision + nav submission
    StreamFocus,  // StreamingManager::update_stream_focus
    Edit,         // CSG edit transaction phase execution
    Navigation,   // Navigation path / distance query
    Count,
};

inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);

[[nodiscard]] inline const char* stage_name(Stage stage) noexcept {
    switch (stage) {
        case Stage::Generate: return "generate";
        case Stage::Mesh: return "mesh";
        case Stage::Commit: return "commit";
        case Stage::StreamFocus: return "stream_focus";
        case Stage::Edit: return "edit";
        case Stage::Navigation: return "navigation";
        case Stage::Count: break;
    }
    return "unknown";
}

struct StageSample {
    uint64_t count = 0;
    uint64_t total_us = 0;
    uint64_t max_us = 0;

    [[nodiscard]] bool active() const noexcept { return count > 0; }
    [[nodiscard]] double avg_us() const noexcept {
        return count != 0 ? static_cast<double>(total_us) / static_cast<double>(count) : 0.0;
    }
};

/// Thread-safe per-stage timing accumulator. `record` is called from worker and
/// main threads; `drain`/`drain_all` read and reset, so callers only ever see
/// the work done since the previous drain.
class StageTimings {
public:
    void record(Stage stage, uint64_t elapsed_us) noexcept {
        AtomicSlot& slot = slots_[static_cast<std::size_t>(stage)];
        slot.count.fetch_add(1, std::memory_order_relaxed);
        slot.total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
        uint64_t current = slot.max_us.load(std::memory_order_relaxed);
        while (elapsed_us > current &&
               !slot.max_us.compare_exchange_weak(current, elapsed_us,
                                                  std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] StageSample drain(Stage stage) noexcept {
        AtomicSlot& slot = slots_[static_cast<std::size_t>(stage)];
        return StageSample{
            slot.count.exchange(0, std::memory_order_relaxed),
            slot.total_us.exchange(0, std::memory_order_relaxed),
            slot.max_us.exchange(0, std::memory_order_relaxed)};
    }

    [[nodiscard]] std::array<StageSample, kStageCount> drain_all() noexcept {
        std::array<StageSample, kStageCount> out{};
        for (std::size_t i = 0; i < kStageCount; ++i) {
            out[i] = drain(static_cast<Stage>(i));
        }
        return out;
    }

private:
    struct AtomicSlot {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> total_us{0};
        std::atomic<uint64_t> max_us{0};
    };

    std::array<AtomicSlot, kStageCount> slots_{};
};

[[nodiscard]] inline uint64_t monotonic_microseconds() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

/// RAII timer. A null accumulator makes this a no-op, so the timing hooks can
/// stay in place while instrumentation is disabled.
class ScopedTimer {
public:
    ScopedTimer(StageTimings* timings, Stage stage) noexcept
        : timings_(timings),
          stage_(stage),
          start_us_(timings != nullptr ? monotonic_microseconds() : 0) {}

    ~ScopedTimer() {
        if (timings_ != nullptr) {
            timings_->record(stage_, monotonic_microseconds() - start_us_);
        }
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    StageTimings* timings_;
    Stage stage_;
    uint64_t start_us_;
};

/// Formats the drained samples as one line, e.g.
/// "generate x12 total 4.53ms max 0.62ms avg 0.38ms | mesh x8 ...".
/// Returns an empty string when nothing ran this frame.
[[nodiscard]] inline std::string format_stage_summaries(
    const std::array<StageSample, kStageCount>& samples) {
    std::string out;
    for (std::size_t i = 0; i < kStageCount; ++i) {
        const StageSample& s = samples[i];
        if (!s.active()) {
            continue;
        }
        if (!out.empty()) {
            out += " | ";
        }
        char buf[160];
        const auto stage = static_cast<Stage>(i);
        std::snprintf(buf, sizeof(buf), "%s x%llu total %.2fms max %.2fms avg %.2fms",
                      stage_name(stage),
                      static_cast<unsigned long long>(s.count),
                      static_cast<double>(s.total_us) / 1000.0,
                      static_cast<double>(s.max_us) / 1000.0,
                      s.avg_us() / 1000.0);
        out += buf;
    }
    return out;
}

} // namespace voxel
