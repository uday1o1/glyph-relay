#!/usr/bin/env bash
set -euo pipefail

compiler="${CXX:-clang++}"
if ! command -v "${compiler}" >/dev/null 2>&1; then
  echo "C++ compiler unavailable: ${compiler}" >&2
  exit 1
fi

mkdir -p build/linux-recording-analyzer
"${compiler}" --analyze -std=c++20 -Iinclude \
  -DGLYPHRELAY_HAS_OPENH264=1 \
  -o build/linux-recording-analyzer/recording_linux.plist \
  src/recording/recording_linux.cpp

read -r -a capture_flags <<<"$(pkg-config --cflags gio-2.0 gio-unix-2.0 libpipewire-0.3)"
"${compiler}" --analyze -std=c++20 -Iinclude \
  "${capture_flags[@]}" \
  -DGLYPHRELAY_HAS_OPENH264=1 \
  -o build/linux-recording-analyzer/record_command_linux.plist \
  src/app/record_command_linux.cpp
