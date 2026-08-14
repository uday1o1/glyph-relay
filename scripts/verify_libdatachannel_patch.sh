#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
"${repository_root}/scripts/bootstrap_libdatachannel.sh"

dependency_root="${repository_root}/.deps/libdatachannel-v0.24.1"
build_root="${repository_root}/build/libdatachannel-v0.24.1"
if command -v cmake >/dev/null 2>&1; then
  cmake_command=(cmake)
elif command -v uvx >/dev/null 2>&1; then
  cmake_command=(uvx --from cmake==4.1.0 cmake)
else
  echo "CMake 4.1.0 or uvx is required" >&2
  exit 1
fi

"${cmake_command[@]}" -S "${dependency_root}" -B "${build_root}" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_SHARED_DEPS_LIBS=OFF \
  -DNO_EXAMPLES=ON \
  -DNO_MEDIA=OFF \
  -DNO_TESTS=OFF \
  -DNO_WEBSOCKET=OFF \
  -DPREFER_SYSTEM_LIB=OFF \
  -DUSE_GNUTLS=OFF \
  -DUSE_MBEDTLS=OFF \
  -DUSE_NICE=OFF \
  -DUSE_SYSTEM_JSON=OFF \
  -DUSE_SYSTEM_JUICE=OFF \
  -DUSE_SYSTEM_PLOG=OFF \
  -DUSE_SYSTEM_SRTP=OFF \
  -DUSE_SYSTEM_USRSCTP=OFF \
  -DWARNINGS_AS_ERRORS=ON
"${cmake_command[@]}" --build "${build_root}" \
  --target datachannel glyphrelay-juice-egress-tests --parallel
"${build_root}/deps/libjuice/glyphrelay-juice-egress-tests"

echo "patched libdatachannel and final-egress loopback test passed"
