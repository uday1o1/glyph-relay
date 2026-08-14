#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-clang++}"
if ! command -v "${compiler}" >/dev/null 2>&1; then
  echo "C++ compiler unavailable: ${compiler}" >&2
  exit 1
fi

output_directory="build/clang-analyzer"
mkdir -p "${output_directory}"

for source in \
  src/app/doctor.cpp \
  src/app/doctor_probes.cpp \
  src/app/main.cpp \
  src/core/m0_protocol.cpp \
  src/core/quality_metrics.cpp \
  src/core/sha256.cpp \
  src/core/synthetic_source.cpp \
  src/gpu/cuda_context.cpp \
  src/gpu/gpu_contracts.cpp \
  src/gpu/nvenc_probe.cpp \
  src/media/annex_b.cpp \
  src/media/h264_sps.cpp \
  src/media/i420.cpp \
  src/media/openh264_encoder.cpp \
  src/media/recording_profile.cpp \
  tests/native/test_doctor.cpp \
  tests/native/test_gpu_contracts.cpp \
  tests/native/test_media_contracts.cpp \
  tests/native/test_m0_protocol.cpp \
  tests/native/test_openh264_integration.cpp \
  tools/freeze_m0_protocol.cpp; do
  report="${output_directory}/$(basename "${source}" .cpp).plist"
  "${compiler}" --analyze -std=c++20 -Iinclude \
    -DGLYPHRELAY_VERSION=\"0.1.0\" \
    -DGLYPHRELAY_HAS_CUDA_COMPILER=0 \
    -DGLYPHRELAY_HAS_CUDA_DRIVER=0 \
    -DGLYPHRELAY_HAS_OPENH264=0 \
    -DGLYPHRELAY_HAS_NVENC=0 \
    -DGLYPHRELAY_CUDA_COMPILER_VERSION=\"\" \
    -DGLYPHRELAY_SOURCE_DIR=\".\" \
    -DGLYPHRELAY_M0_PROTOCOL_SHA256=\"3428958bf30b487e34c106614f83b59fe2526cfd46e1beb9a9249b70f2b1c717\" \
    -o "${report}" \
    "${source}"
done
