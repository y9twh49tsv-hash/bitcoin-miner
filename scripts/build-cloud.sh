#!/usr/bin/env bash
#
# Cloud / CI build: portable C++ core, no CUDA, no GPU required.
#
# This is the build that must always work -- on a container with nothing but a
# C++20 compiler and CMake.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

cd "${PROJECT_ROOT}"

echo "=============================================="
echo " AZD Bitcoin Miner -- cloud build (no CUDA)"
echo "=============================================="
echo

if [[ "${1:-}" == "--clean" ]]; then
    echo "Removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

# Use all available cores, portably.
if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu)"
else
    JOBS=2
fi

echo "--- Configure ---"
cmake -S . -B build -DAZD_ENABLE_CUDA=OFF

echo
echo "--- Build (${JOBS} jobs) ---"
cmake --build build -j "${JOBS}"

echo
echo "--- Test ---"
ctest --test-dir build --output-on-failure

echo
echo "--- Self-test (real hashing, real timings) ---"
"${BUILD_DIR}/bin/azd-miner" --selftest

echo
echo "=============================================="
echo " Build OK. CUDA was NOT compiled in."
echo " GPU hashrate/temperature/power are unavailable"
echo " until the project is rebuilt with"
echo "   -DAZD_ENABLE_CUDA=ON"
echo " on a machine with the NVIDIA CUDA Toolkit."
echo "=============================================="
