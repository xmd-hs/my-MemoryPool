#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-${source_dir}/build-linux-perf}"
cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release \
  -DXMD_MEMORY_POOL_BUILD_TESTS=ON -DXMD_MEMORY_POOL_BUILD_EXAMPLES=OFF \
  -DXMD_MEMORY_POOL_ENABLE_STATS=OFF -DXMD_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF
cmake --build "${build_dir}" --parallel
"${build_dir}/unit_test" | tee "${build_dir}/unit_final.txt"
"${build_dir}/perf_test" --report | tee "${build_dir}/perf_final.txt"
