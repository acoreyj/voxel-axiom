#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/voxel_engine_build}"
mkdir -p "${BUILD_DIR}"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++20 -O2 -Wall -Wextra -Werror -pedantic -pthread -I"${SCRIPT_DIR}/include" -I"${SCRIPT_DIR}/src")
ASAN_FLAGS=(-fsanitize=address,undefined)
TSAN_FLAGS=(-fsanitize=thread)

echo "=================================================================="
echo " Building & Running All Voxel Engine v4.0 M0-M4 Test Suites (ASan)"
echo "=================================================================="

SIMPLE_TESTS=(
    "test_chunk_key"
    "test_chunk_store"
    "test_stage_timings"
    "test_scratch_and_dirty"
    "test_edit_and_worker_pool"
    "test_procgen_and_surface_nets"
    "test_persistence_and_streaming"
    "test_integration_m3_m4"
)

for t in "${SIMPLE_TESTS[@]}"; do
    "${CXX}" "${CXXFLAGS[@]}" "${ASAN_FLAGS[@]}" \
        "${SCRIPT_DIR}/tests/${t}.cpp" -o "${BUILD_DIR}/${t}"
    "${BUILD_DIR}/${t}"
done

"${CXX}" "${CXXFLAGS[@]}" "${ASAN_FLAGS[@]}" \
    "${SCRIPT_DIR}/tests/test_gdextension_bindings.cpp" \
    "${SCRIPT_DIR}/src/register_types.cpp" \
    "${SCRIPT_DIR}/src/voxel_terrain_3d.cpp" \
    "${SCRIPT_DIR}/src/voxel_ai_companion.cpp" \
    -o "${BUILD_DIR}/test_gdextension_bindings"
"${BUILD_DIR}/test_gdextension_bindings"

echo "=================================================================="
echo " Running Multi-Threaded Concurrency & Teardown Suites Under TSan"
echo "=================================================================="

"${CXX}" "${CXXFLAGS[@]}" "${TSAN_FLAGS[@]}" \
    "${SCRIPT_DIR}/tests/test_edit_and_worker_pool.cpp" \
    -o "${BUILD_DIR}/test_edit_and_worker_pool_tsan"
"${BUILD_DIR}/test_edit_and_worker_pool_tsan"

"${CXX}" "${CXXFLAGS[@]}" "${TSAN_FLAGS[@]}" \
    "${SCRIPT_DIR}/tests/test_gdextension_bindings.cpp" \
    "${SCRIPT_DIR}/src/register_types.cpp" \
    "${SCRIPT_DIR}/src/voxel_terrain_3d.cpp" \
    "${SCRIPT_DIR}/src/voxel_ai_companion.cpp" \
    -o "${BUILD_DIR}/test_gdextension_bindings_tsan"
"${BUILD_DIR}/test_gdextension_bindings_tsan"

echo "=================================================================="
echo " ALL 8 TEST SUITES PASSED UNDER ASAN/UBSAN AND TSAN!"
echo "=================================================================="
