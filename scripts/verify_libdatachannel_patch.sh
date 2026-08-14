#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
"${repository_root}/scripts/bootstrap_libdatachannel.sh"

dependency_root="${repository_root}/.deps/libdatachannel-v0.24.1"
build_root="${repository_root}/build/transport-contract"
if command -v cmake >/dev/null 2>&1; then
  cmake_command=(cmake)
elif command -v uvx >/dev/null 2>&1; then
  cmake_command=(uvx --from cmake==4.1.0 cmake)
else
  echo "CMake 4.1.0 or uvx is required" >&2
  exit 1
fi

"${cmake_command[@]}" -S "${repository_root}" -B "${build_root}" \
  -DGLYPHRELAY_ENABLE_CUDA=OFF \
  -DGLYPHRELAY_ENABLE_WEBRTC_CONTRACTS=ON \
  -DGLYPHRELAY_LIBDATACHANNEL_ROOT="${dependency_root}" \
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
  --target glyphrelay glyphrelay_m0_webrtc_sender glyphrelay_m0_transport_fixture \
           glyphrelay_libdatachannel_contract_tests glyphrelay_owner_signaling_tests \
           glyphrelay_control_protocol_tests glyphrelay_peer_sender_tests \
           glyphrelay_owner_signaling_fixture \
           glyphrelay-juice-egress-tests --parallel
"${build_root}/glyphrelay" --help
"${build_root}/glyphrelay_m0_webrtc_sender" --help
"${build_root}/glyphrelay_m0_transport_fixture" --help
"${build_root}/glyphrelay_libdatachannel_contract_tests"
"${build_root}/glyphrelay_owner_signaling_tests"
"${build_root}/glyphrelay_control_protocol_tests"
"${build_root}/glyphrelay_peer_sender_tests"
node "${repository_root}/tooling/signaling/verify-native-owner.ts" \
  --executable "${build_root}/glyphrelay_owner_signaling_fixture"
node "${repository_root}/tooling/signaling/verify-native-owner.ts" \
  --executable "${build_root}/glyphrelay_owner_signaling_fixture" --tls
"${build_root}/libdatachannel-v0.24.1/deps/libjuice/glyphrelay-juice-egress-tests"

echo "patched libdatachannel packetization, recovery, and final-egress tests passed"
