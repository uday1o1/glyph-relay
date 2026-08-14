#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-clang++}"
if ! command -v "${compiler}" >/dev/null 2>&1; then
  echo "C++ compiler unavailable: ${compiler}" >&2
  exit 1
fi

read -r -a package_flags <<<"$(pkg-config --cflags gio-2.0 gio-unix-2.0 libpipewire-0.3)"
mkdir -p build/linux-capture-analyzer

for source in \
  src/capture/linux_capture_gdbus.cpp \
  src/capture/linux_capture_pipewire.cpp \
  tests/native/test_linux_capture_contracts.cpp; do
  report="build/linux-capture-analyzer/$(basename "${source}" .cpp).plist"
  "${compiler}" --analyze -std=c++20 -Iinclude -Isrc/capture \
    "${package_flags[@]}" \
    -o "${report}" \
    "${source}"
done
