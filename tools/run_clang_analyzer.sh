#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-clang++}"
if ! command -v "${compiler}" >/dev/null 2>&1; then
  echo "C++ compiler unavailable: ${compiler}" >&2
  exit 1
fi

output_directory="build/clang-analyzer"
mkdir -p "${output_directory}"

for source in src/app/doctor.cpp src/app/doctor_probes.cpp src/app/main.cpp tests/native/test_doctor.cpp; do
  report="${output_directory}/$(basename "${source}" .cpp).plist"
  "${compiler}" --analyze -std=c++20 -Iinclude \
    -DGLYPHRELAY_VERSION=\"0.1.0\" \
    -DGLYPHRELAY_HAS_CUDA_COMPILER=0 \
    -o "${report}" \
    "${source}"
done
