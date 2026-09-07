#!/bin/bash
set -e
rsync -a --delete --exclude build --exclude build-asan --exclude build-linux-perf --exclude build-release \
  /mnt/d/面试项目/my-MemoryPool/v3/ /home/xmd/kama-mp/
cmake -S /home/xmd/kama-mp -B /home/xmd/kama-mp/build -DCMAKE_BUILD_TYPE=Release \
  -DKAMA_MEMORY_POOL_BUILD_TESTS=ON \
  -DKAMA_MEMORY_POOL_BUILD_EXAMPLES=OFF \
  -DKAMA_MEMORY_POOL_ENABLE_STATS=OFF \
  -DKAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"
cmake --build /home/xmd/kama-mp/build --parallel
/home/xmd/kama-mp/build/unit_test | tee /home/xmd/kama-mp/build/unit_final.txt | tail -3
echo "UNIT_EXIT=${PIPESTATUS[0]}"
/home/xmd/kama-mp/build/perf_test | tee /home/xmd/kama-mp/build/perf_final.txt | tail -40
echo "PERF_EXIT=${PIPESTATUS[0]}"
