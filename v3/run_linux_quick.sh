#!/usr/bin/env bash
set -euo pipefail

source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-${source_dir}/build-linux-perf}"

cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release \
  -DXMD_MEMORY_POOL_BUILD_TESTS=ON \
  -DXMD_MEMORY_POOL_BUILD_EXAMPLES=OFF \
  -DXMD_MEMORY_POOL_ENABLE_STATS=OFF \
  -DXMD_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure
"${build_dir}/workload_bench" pool 8 2000000
"${build_dir}/workload_bench" malloc 8 2000000
