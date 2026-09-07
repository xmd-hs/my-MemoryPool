#!/bin/bash
set -e
cd /home/xmd/kama-mp
./build-asan/unit_test > build-asan/out.txt 2>&1 || true
echo "ASAN_EXIT=$?"
tail -30 build-asan/out.txt
echo "===="
./build/unit_test > build/unit.txt 2>&1 || true
echo "UNIT_EXIT=$?"
tail -30 build/unit.txt
