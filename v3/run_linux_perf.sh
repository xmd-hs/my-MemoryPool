#!/bin/bash
set -e
cd /home/xmd/kama-mp
# Rebuild release with latest sources
rsync -a --delete --exclude build --exclude build-asan --exclude build-linux-perf --exclude build-release \
  /mnt/d/面试项目/my-MemoryPool/v3/ /home/xmd/kama-mp/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DKAMA_MEMORY_POOL_BUILD_TESTS=ON \
  -DKAMA_MEMORY_POOL_BUILD_EXAMPLES=OFF \
  -DKAMA_MEMORY_POOL_ENABLE_STATS=OFF \
  -DKAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF
cmake --build build --parallel
./build/unit_test | tee build/unit_final.txt
echo "UNIT_EXIT=${PIPESTATUS[0]}"
./build/perf_test | tee build/perf_final.txt
echo "PERF_EXIT=${PIPESTATUS[0]}"
