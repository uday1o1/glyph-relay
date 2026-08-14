#!/usr/bin/env bash
set -euo pipefail

cmake --preset linux-cpu
cmake --build --preset linux-cpu --parallel
ctest --preset linux-cpu

bash tools/run_linux_capture_analyzer.sh

cmake \
  -S . \
  -B build/linux-capture-sanitizers \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGLYPHRELAY_ENABLE_CUDA=OFF \
  -DGLYPHRELAY_ENABLE_SANITIZERS=ON
cmake --build build/linux-capture-sanitizers \
  --target glyphrelay_linux_capture_contract_tests \
  --parallel
ctest \
  --test-dir build/linux-capture-sanitizers \
  --output-on-failure \
  -R '^integration\.linux_capture_contracts$'
