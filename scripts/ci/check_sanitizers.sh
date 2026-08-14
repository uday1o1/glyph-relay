#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
build_root="${repository_root}/build/sanitizers"

cmake \
  -S "${repository_root}" \
  -B "${build_root}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGLYPHRELAY_ENABLE_CUDA=OFF \
  -DGLYPHRELAY_ENABLE_SANITIZERS=ON
cmake --build "${build_root}" --parallel
ctest --test-dir "${build_root}" --output-on-failure
