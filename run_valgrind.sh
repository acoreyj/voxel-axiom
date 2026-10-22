#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build-valgrind}"
LOG_DIR="${LOG_DIR:-${SCRIPT_DIR}/build-valgrind/logs}"
SUPP="${SUPP:-${SCRIPT_DIR}/valgrind.supp}"

TESTS=(
    "test_chunk_key"
    "test_chunk_store"
    "test_stage_timings"
    "test_scratch_and_dirty"
    "test_edit_and_worker_pool"
    "test_procgen_and_surface_nets"
    "test_terrain_raycast"
    "test_offline_asset_baker"
    "test_persistence_and_streaming"
    "test_integration_m3_m4"
    "test_gdextension_bindings"
)

echo "=================================================================="
echo " Building Voxel Engine v4.0 suites for Valgrind Memcheck"
echo "=================================================================="

mkdir -p "${LOG_DIR}"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DVOXEL_BUILD_TESTS=ON \
    -DVOXEL_BUILD_GDEXTENSION=OFF \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-g -O1 -fno-omit-frame-pointer"
cmake --build "${BUILD_DIR}" -j "$(nproc)"

echo "=================================================================="
echo " Running Memcheck (leak-check=full, definite leaks fail the run)"
echo "=================================================================="

# test_gdextension_bindings loads project-relative .vreg fixtures.
# Track origins is opt-in: the extra slowdown can miss the fully-committed
# deadline under Valgrind, so the deadline is widened here.
cd "${SCRIPT_DIR}"
export VOXEL_TEST_COMMIT_TIMEOUT_SECONDS="${VOXEL_TEST_COMMIT_TIMEOUT_SECONDS:-120}"

for t in "${TESTS[@]}"; do
    echo "--- ${t} ---"
    valgrind --tool=memcheck \
        --leak-check=full \
        --show-leak-kinds=all \
        --errors-for-leak-kinds=definite \
        --track-origins="${TRACK_ORIGINS:-no}" \
        --suppressions="${SUPP}" \
        --error-exitcode=99 \
        --log-file="${LOG_DIR}/${t}.log" \
        "${BUILD_DIR}/${t}"
    grep -E "definitely lost|indirectly lost|possibly lost|still reachable|ERROR SUMMARY" \
        "${LOG_DIR}/${t}.log" | tail -5
    rm -f "${BUILD_DIR}/${t}.log"
done

echo "=================================================================="
echo " MEMCHECK CLEAN: no definite or indirect leaks, no memory errors"
echo "=================================================================="
