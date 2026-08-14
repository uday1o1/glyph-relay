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
  src/app/record_command.cpp \
  src/app/record_command_stub.cpp \
  src/capture/capture.cpp \
  src/capture/linux_capture_stub.cpp \
  src/core/m0_protocol.cpp \
  src/core/nv12_scaler.cpp \
  src/core/color_conversion.cpp \
  src/core/quality_metrics.cpp \
  src/core/sha256.cpp \
  src/core/synthetic_source.cpp \
  src/gpu/cuda_context.cpp \
  src/gpu/cuda_preprocess_stub.cpp \
  src/gpu/gpu_contracts.cpp \
  src/gpu/nvenc_probe.cpp \
  src/gpu/preprocess_pool.cpp \
  src/gpu/saliency.cpp \
  src/media/annex_b.cpp \
  src/media/h264_sps.cpp \
  src/media/i420.cpp \
  src/media/openh264_encoder.cpp \
  src/media/recording_profile.cpp \
  src/recording/recording_common.cpp \
  src/recording/recording_stub.cpp \
  src/transport/media_egress.cpp \
  src/transport/rtp_transport.cpp \
  tests/native/test_doctor.cpp \
  tests/native/test_capture_contracts.cpp \
  tests/native/test_cuda_preprocess_stub.cpp \
  tests/native/test_gpu_contracts.cpp \
  tests/native/test_media_contracts.cpp \
  tests/native/test_media_egress.cpp \
  tests/native/test_m0_protocol.cpp \
  tests/native/test_openh264_integration.cpp \
  tests/native/test_preprocess_pool.cpp \
  tests/native/test_rtp_transport.cpp \
  tests/native/test_saliency.cpp \
  tools/freeze_m0_protocol.cpp \
  tools/qualify_cuda_saliency.cpp \
  tools/render_saliency_preview.cpp; do
  report="${output_directory}/$(basename "${source}" .cpp).plist"
  "${compiler}" --analyze -std=c++20 -Iinclude \
    -DGLYPHRELAY_VERSION=\"0.1.0\" \
    -DGLYPHRELAY_HAS_CUDA_COMPILER=0 \
    -DGLYPHRELAY_HAS_CUDA_DRIVER=0 \
    -DGLYPHRELAY_HAS_OPENH264=0 \
    -DGLYPHRELAY_HAS_LINUX_CAPTURE=0 \
    -DGLYPHRELAY_HAS_DURABLE_RECORDING=0 \
    -DGLYPHRELAY_HAS_NVENC=0 \
    -DGLYPHRELAY_CUDA_COMPILER_VERSION=\"\" \
    -DGLYPHRELAY_SOURCE_DIR=\".\" \
    -DGLYPHRELAY_M0_PROTOCOL_SHA256=\"5443417595e3ccb88c89adc3a2d22842fde3206c736d3069042b62cd1c8ab708\" \
    -o "${report}" \
    "${source}"
done
