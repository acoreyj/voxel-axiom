#include "chunk_key.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace {

using voxel::ChunkKey;
using voxel::ChunkKeyHash;
using voxel::IVec3;
using voxel::LocalCoord;
using voxel::SampleMapping;
using voxel::map_world_sample;
using voxel::unmap_world_sample;

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

// ============================================================================
// Compile-Time (constexpr) Contract Verification
// ============================================================================
static_assert(ChunkKey::floor_div(-33, 32) == -2, "floor_div(-33, 32) must be -2");
static_assert(ChunkKey::floor_mod(-33, 32) == 31, "floor_mod(-33, 32) must be 31");
static_assert(ChunkKey::floor_div(-32, 32) == -1, "floor_div(-32, 32) must be -1");
static_assert(ChunkKey::floor_mod(-32, 32) == 0,  "floor_mod(-32, 32) must be 0");
static_assert(ChunkKey::floor_div(-31, 32) == -1, "floor_div(-31, 32) must be -1");
static_assert(ChunkKey::floor_mod(-31, 32) == 1,  "floor_mod(-31, 32) must be 1");
static_assert(ChunkKey::floor_div(-1, 32)  == -1, "floor_div(-1, 32) must be -1");
static_assert(ChunkKey::floor_mod(-1, 32)  == 31, "floor_mod(-1, 32) must be 31");
static_assert(ChunkKey::floor_div(0, 32)   == 0,  "floor_div(0, 32) must be 0");
static_assert(ChunkKey::floor_mod(0, 32)   == 0,  "floor_mod(0, 32) must be 0");
static_assert(ChunkKey::floor_div(1, 32)   == 0,  "floor_div(1, 32) must be 0");
static_assert(ChunkKey::floor_mod(1, 32)   == 1,  "floor_mod(1, 32) must be 1");
static_assert(ChunkKey::floor_div(31, 32)  == 0,  "floor_div(31, 32) must be 0");
static_assert(ChunkKey::floor_mod(31, 32)  == 31, "floor_mod(31, 32) must be 31");
static_assert(ChunkKey::floor_div(32, 32)  == 1,  "floor_div(32, 32) must be 1");
static_assert(ChunkKey::floor_mod(32, 32)  == 0,  "floor_mod(32, 32) must be 0");
static_assert(ChunkKey::floor_div(33, 32)  == 1,  "floor_div(33, 32) must be 1");
static_assert(ChunkKey::floor_mod(33, 32)  == 1,  "floor_mod(33, 32) must be 1");

static_assert(ChunkKey::from_coords(-1, -1, -1).x() == -1);
static_assert(ChunkKey::from_coords(-1, -1, -1).y() == -1);
static_assert(ChunkKey::from_coords(-1, -1, -1).z() == -1);
static_assert(unmap_world_sample(map_world_sample(-1, -32, 33)) == IVec3{-1, -32, 33});

// ============================================================================
// 1. M0 Boundary Table Tests across {-33, -32, -31, -1, 0, 1, 31, 32, 33}
// ============================================================================
void test_m0_boundary_table() {
    ++g_tests_run;

    struct ExpectedBoundary {
        int32_t sample;
        int32_t expected_chunk;
        int32_t expected_local;
    };

    constexpr std::array<ExpectedBoundary, 9> kBoundaries = {{
        {-33, -2, 31},
        {-32, -1,  0},
        {-31, -1,  1},
        { -1, -1, 31},
        {  0,  0,  0},
        {  1,  0,  1},
        { 31,  0, 31},
        { 32,  1,  0},
        { 33,  1,  1},
    }};

    for (const auto& tc : kBoundaries) {
        const int32_t c = ChunkKey::floor_div(tc.sample, 32);
        const int32_t l = ChunkKey::floor_mod(tc.sample, 32);
        REQUIRE(c == tc.expected_chunk);
        REQUIRE(l == tc.expected_local);
        REQUIRE(c * 32 + l == tc.sample);
        REQUIRE(l >= 0 && l < 32);
    }

    std::printf("[PASS] test_m0_boundary_table (9 canonical boundary points verified)\n");
}

// ============================================================================
// 2. Exhaustive Floor Division & Modulo Euclidean Invariants
// ============================================================================
void test_floor_div_mod_invariants() {
    ++g_tests_run;

    // Verify Euclidean division invariant across [-100000, +100000] for chunk dim 32,
    // navigation group dims (4, 2), and region dim (8).
    constexpr std::array<int32_t, 4> kDivisors = {32, 8, 4, 2};
    for (int32_t d : kDivisors) {
        for (int32_t p = -100000; p <= 100000; ++p) {
            const int32_t q = ChunkKey::floor_div(p, d);
            const int32_t r = ChunkKey::floor_mod(p, d);
            REQUIRE(r >= 0 && r < d);
            REQUIRE(q * d + r == p);
        }
    }

    // Verify negative divisor support as guaranteed by the formula in Section 3.1.
    for (int32_t p = -1024; p <= 1024; ++p) {
        const int32_t q = ChunkKey::floor_div(p, -32);
        const int32_t r = ChunkKey::floor_mod(p, -32);
        REQUIRE(r <= 0 && r > -32);
        REQUIRE(q * (-32) + r == p);
    }

    std::printf("[PASS] test_floor_div_mod_invariants (Euclidean invariants over 800k+ inputs)\n");
}

// ============================================================================
// 3. 64-bit Offset-Encoded ChunkKey Packing, Sign-Extension & Ordering
// ============================================================================
void test_chunk_key_offset_encoding() {
    ++g_tests_run;

    // Verify zero sign-extension bleed across adjacent 21-bit fields.
    const std::array<int32_t, 11> kTestCoords = {
        ChunkKey::kMinCoord,
        ChunkKey::kMinCoord + 1,
        -33, -32, -1, 0, 1, 32, 33,
        ChunkKey::kMaxCoord - 1,
        ChunkKey::kMaxCoord
    };

    for (int32_t cx : kTestCoords) {
        for (int32_t cy : kTestCoords) {
            for (int32_t cz : kTestCoords) {
                const ChunkKey key = ChunkKey::from_coords(cx, cy, cz);

                // Top bit (bit 63) must always be 0 (63 bits used: 3 * 21).
                REQUIRE((key.raw >> 63U) == 0ULL);

                // Exact coordinate recovery with zero cross-axis sign corruption.
                REQUIRE(key.x() == cx);
                REQUIRE(key.y() == cy);
                REQUIRE(key.z() == cz);
                REQUIRE(key.coords() == (IVec3{cx, cy, cz}));
            }
        }
    }

    // Verify exact bit patterns at extreme and zero coordinates.
    // At (kMinCoord, kMinCoord, kMinCoord) = (-2^20, -2^20, -2^20), biased components are all 0.
    const ChunkKey min_key = ChunkKey::from_coords(ChunkKey::kMinCoord, ChunkKey::kMinCoord, ChunkKey::kMinCoord);
    REQUIRE(min_key.raw == 0ULL);

    // At (0, 0, 0), each axis stores kBias = 1 << 20.
    const ChunkKey zero_key = ChunkKey::from_coords(0, 0, 0);
    REQUIRE(zero_key.raw == ((1ULL << 20U) | (1ULL << 41U) | (1ULL << 62U)));

    // At (kMaxCoord, kMaxCoord, kMaxCoord), all 63 low bits are 1.
    const ChunkKey max_key = ChunkKey::from_coords(ChunkKey::kMaxCoord, ChunkKey::kMaxCoord, ChunkKey::kMaxCoord);
    REQUIRE(max_key.raw == ((1ULL << 63U) - 1ULL));

    // Verify strict lexicographical ordering on (z, y, x) via raw < operator
    // (required for deadlock-free ascending lock acquisition in Section 3.2).
    REQUIRE(ChunkKey::from_coords(-1, 0, 0) < ChunkKey::from_coords(0, 0, 0));
    REQUIRE(ChunkKey::from_coords(0, -1, 0) < ChunkKey::from_coords(0, 0, 0));
    REQUIRE(ChunkKey::from_coords(0, 0, -1) < ChunkKey::from_coords(0, 0, 0));
    REQUIRE(min_key < zero_key);
    REQUIRE(zero_key < max_key);

    std::printf("[PASS] test_chunk_key_offset_encoding (21-bit packing, zero sign bleed, lock ordering)\n");
}

// ============================================================================
// 4. 3D World Sample <-> (ChunkKey, LocalCoord) Round-Trip Conversions
// ============================================================================
void test_world_sample_round_trip() {
    ++g_tests_run;

    // Full 3D Cartesian product of the M0 boundary values {-33, -32, -31, -1, 0, 1, 31, 32, 33}
    constexpr std::array<int32_t, 9> kBoundarySamples = {-33, -32, -31, -1, 0, 1, 31, 32, 33};

    for (int32_t px : kBoundarySamples) {
        for (int32_t py : kBoundarySamples) {
            for (int32_t pz : kBoundarySamples) {
                const IVec3 world_pos{px, py, pz};
                const SampleMapping mapped = map_world_sample(world_pos);

                // Verify local coordinates are strictly within [0, 31]^3
                REQUIRE(mapped.local.is_valid());
                REQUIRE(mapped.local.x == ChunkKey::floor_mod(px, 32));
                REQUIRE(mapped.local.y == ChunkKey::floor_mod(py, 32));
                REQUIRE(mapped.local.z == ChunkKey::floor_mod(pz, 32));

                // Verify chunk coordinates match floor_div(p, 32)
                REQUIRE(mapped.chunk.x() == ChunkKey::floor_div(px, 32));
                REQUIRE(mapped.chunk.y() == ChunkKey::floor_div(py, 32));
                REQUIRE(mapped.chunk.z() == ChunkKey::floor_div(pz, 32));

                // Verify exact round-trip back to world sample coordinate
                const IVec3 round_trip = unmap_world_sample(mapped);
                REQUIRE(round_trip == world_pos);

                // Verify linear index round-trip within canonical 32^3 chunk array
                const uint32_t linear_idx = mapped.local.to_linear_index();
                REQUIRE(linear_idx < 32768U);
                const LocalCoord reconstructed_local = LocalCoord::from_linear_index(linear_idx);
                REQUIRE(reconstructed_local == mapped.local);
            }
        }
    }

    // Additional round-trip tests at large negative and positive world coordinates
    constexpr std::array<IVec3, 6> kLargeSamples = {{
        {-32768, -32768, -32768},
        {-32769,     -1,  32767},
        { -1025,  -2048,  -4097},
        {  1023,   2048,   4096},
        {1048575, -1048576, 524287},
        {-16777216, 16777215, -1}
    }};

    for (const IVec3& p : kLargeSamples) {
        const SampleMapping mapped = map_world_sample(p);
        REQUIRE(mapped.local.is_valid());
        REQUIRE(unmap_world_sample(mapped) == p);
    }

    std::printf("[PASS] test_world_sample_round_trip (729 boundary combinations + large coordinates)\n");
}

// ============================================================================
// 5. 36^3 Worker Scratch Window (-1..34) to Canonical Owner Mapping (Section 3.1)
// ============================================================================
void test_scratch_window_mapping_contract() {
    ++g_tests_run;

    // Verify that for any chunk C, scratch local coordinates s in [-1..34] map via
    // world sample p = 32 * C + s to the exact neighbor offset and canonical index
    // specified in Section 3.1 Table ("Seamless Surface Nets Dual-Grid & The 36^3 Worker Scratch Window").
    const ChunkKey center = ChunkKey::from_coords(-3, 0, 5);
    const IVec3 base = center.min_world_sample();

    for (int32_t s = -1; s <= 34; ++s) {
        const SampleMapping m = map_world_sample(base.x + s, base.y + s, base.z + s);
        const int32_t dcx = m.chunk.x() - center.x();
        const int32_t dcy = m.chunk.y() - center.y();
        const int32_t dcz = m.chunk.z() - center.z();

        if (s == -1) {
            REQUIRE(dcx == -1 && dcy == -1 && dcz == -1);
            REQUIRE(m.local.x == 31 && m.local.y == 31 && m.local.z == 31);
        } else if (s >= 0 && s <= 31) {
            REQUIRE(dcx == 0 && dcy == 0 && dcz == 0);
            REQUIRE(m.local.x == s && m.local.y == s && m.local.z == s);
        } else {
            // s in {32, 33, 34} -> neighbor +1, canonical local {0, 1, 2}
            REQUIRE(dcx == 1 && dcy == 1 && dcz == 1);
            REQUIRE(m.local.x == (s - 32));
            REQUIRE(m.local.y == (s - 32));
            REQUIRE(m.local.z == (s - 32));
        }
    }

    std::printf("[PASS] test_scratch_window_mapping_contract (36^3 scratch [-1..34] neighbor slice table)\n");
}

// ============================================================================
// 6. Bitwise Hashing & Unordered Container Integration
// ============================================================================
void test_chunk_key_hashing() {
    ++g_tests_run;

    std::unordered_set<ChunkKey, ChunkKeyHash> visited;
    constexpr int32_t kRadius = 12; // 25^3 = 15,625 chunks
    for (int32_t z = -kRadius; z <= kRadius; ++z) {
        for (int32_t y = -kRadius; y <= kRadius; ++y) {
            for (int32_t x = -kRadius; x <= kRadius; ++x) {
                const ChunkKey k = ChunkKey::from_coords(x, y, z);
                const auto [it, inserted] = visited.insert(k);
                REQUIRE(inserted);
            }
        }
    }
    REQUIRE(visited.size() == 25ULL * 25ULL * 25ULL);

    // Check std::hash<voxel::ChunkKey> specialization as well.
    std::unordered_set<ChunkKey> std_visited;
    std_visited.insert(ChunkKey::from_coords(-1, -1, -1));
    std_visited.insert(ChunkKey::from_coords(0, 0, 0));
    REQUIRE(std_visited.size() == 2ULL);

    std::printf("[PASS] test_chunk_key_hashing (15,625 unique chunk keys hashed with zero collisions)\n");
}

} // namespace

int main() {
    std::printf("=== Running M0 Step 1 Unit Test Suite: ChunkKey & Floor Coordinate Math ===\n");
    test_m0_boundary_table();
    test_floor_div_mod_invariants();
    test_chunk_key_offset_encoding();
    test_world_sample_round_trip();
    test_scratch_window_mapping_contract();
    test_chunk_key_hashing();
    std::printf("=== ALL %d TEST SUITES PASSED (%d assertions verified) ===\n",
                g_tests_run, g_assertions_checked);
    return EXIT_SUCCESS;
}
