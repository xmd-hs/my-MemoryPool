#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-${source_dir}/build-linux-unit}"
cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Debug \
  -DXMD_MEMORY_POOL_BUILD_TESTS=ON -DXMD_MEMORY_POOL_ENABLE_DEBUG_GUARDS=ON \
  -DXMD_MEMORY_POOL_ENABLE_ASAN=ON -DXMD_MEMORY_POOL_ENABLE_UBSAN=ON
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" -R xmd_unit_test --output-on-failure
