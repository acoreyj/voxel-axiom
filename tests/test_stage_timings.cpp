#include "stage_timings.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using voxel::ScopedTimer;
using voxel::Stage;
using voxel::StageSample;
using voxel::StageTimings;
using voxel::format_stage_summaries;
using voxel::stage_name;

int g_tests_run = 0;
int g_assertions_checked = 0;

#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        ++g_assertions_checked;                                                       \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "[FAIL] %s:%d: Requirement failed: %s\n",            \
                         __FILE__, __LINE__, #cond);                                  \
            std::exit(EXIT_FAILURE);                                                  \
        }                                                                             \
    } while (0)

void test_record_and_drain() {
    ++g_tests_run;
    StageTimings timings;
    timings.record(Stage::Generate, 100);
    timings.record(Stage::Generate, 300);
    timings.record(Stage::Mesh, 50);

    const StageSample gen = timings.drain(Stage::Generate);
    REQUIRE(gen.count == 2);
    REQUIRE(gen.total_us == 400);
    REQUIRE(gen.max_us == 300);
    REQUIRE(gen.active());

    // Drain resets: the next read sees only work recorded since.
    const StageSample gen_again = timings.drain(Stage::Generate);
    REQUIRE(gen_again.count == 0);
    REQUIRE(gen_again.total_us == 0);
    REQUIRE(gen_again.max_us == 0);
    REQUIRE(!gen_again.active());

    const StageSample mesh = timings.drain(Stage::Mesh);
    REQUIRE(mesh.count == 1);
    REQUIRE(mesh.total_us == 50);
    REQUIRE(mesh.max_us == 50);

    std::printf("[PASS] test_record_and_drain\n");
}

void test_max_tracks_largest() {
    ++g_tests_run;
    StageTimings timings;
    timings.record(Stage::Edit, 500);
    timings.record(Stage::Edit, 12);
    timings.record(Stage::Edit, 900);
    timings.record(Stage::Edit, 900); // ties must not corrupt the running max

    const StageSample edit = timings.drain(Stage::Edit);
    REQUIRE(edit.count == 4);
    REQUIRE(edit.total_us == 2312);
    REQUIRE(edit.max_us == 900);
    std::printf("[PASS] test_max_tracks_largest\n");
}

void test_scoped_timer_records_on_scope_exit() {
    ++g_tests_run;
    StageTimings timings;
    {
        ScopedTimer timer(&timings, Stage::Commit);
        (void)0;
    }
    const StageSample commit = timings.drain(Stage::Commit);
    REQUIRE(commit.count == 1);
    REQUIRE(commit.active());

    // A null accumulator makes the timer a no-op rather than a crash.
    {
        ScopedTimer timer(nullptr, Stage::Navigation);
    }
    REQUIRE(timings.drain(Stage::Navigation).count == 0);
    std::printf("[PASS] test_scoped_timer_records_on_scope_exit\n");
}

void test_format_stage_summaries() {
    ++g_tests_run;
    StageTimings timings;
    REQUIRE(format_stage_summaries(timings.drain_all()).empty());

    timings.record(Stage::Mesh, 2000);
    timings.record(Stage::Mesh, 1000);
    const std::string line = format_stage_summaries(timings.drain_all());
    REQUIRE(!line.empty());
    REQUIRE(line.find(stage_name(Stage::Mesh)) != std::string::npos);
    REQUIRE(line.find("x2") != std::string::npos);
    REQUIRE(line.find("3.00ms") != std::string::npos); // total
    REQUIRE(line.find("2.00ms") != std::string::npos); // max
    std::printf("[PASS] test_format_stage_summaries -> \"%s\"\n", line.c_str());
}

void test_concurrent_record_is_exact() {
    ++g_tests_run;
    StageTimings timings;
    constexpr int kThreads = 8;
    constexpr int kIters = 5000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&timings]() {
            for (int i = 0; i < kIters; ++i) {
                timings.record(Stage::Generate, 1);
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    const StageSample gen = timings.drain(Stage::Generate);
    REQUIRE(gen.count == static_cast<uint64_t>(kThreads) * kIters);
    REQUIRE(gen.total_us == static_cast<uint64_t>(kThreads) * kIters);
    std::printf("[PASS] test_concurrent_record_is_exact (%llu records)\n",
                static_cast<unsigned long long>(gen.count));
}

} // namespace

int main() {
    std::printf("=== Running Stage Timing Instrumentation Unit Test Suite ===\n");
    test_record_and_drain();
    test_max_tracks_largest();
    test_scoped_timer_records_on_scope_exit();
    test_format_stage_summaries();
    test_concurrent_record_is_exact();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
