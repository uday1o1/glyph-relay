#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
headers_root="${repository_root}/.deps/nv-codec-headers-n13.1.15.0"
expected_header_sha256="8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228"
header="${headers_root}/include/ffnvcodec/nvEncodeAPI.h"

if [[ ! -f "${header}" ]]; then
  printf '%s\n' "NVENC compile contract requires scripts/bootstrap_nvcodec_headers.sh first" >&2
  exit 2
fi
if command -v sha256sum >/dev/null 2>&1; then
  actual_header_sha256="$(sha256sum "${header}" | awk '{print $1}')"
else
  actual_header_sha256="$(shasum -a 256 "${header}" | awk '{print $1}')"
fi
if [[ "${actual_header_sha256}" != "${expected_header_sha256}" ]]; then
  printf '%s\n' "NVENC compile contract header hash mismatch" >&2
  exit 2
fi

compiler="${CXX:-c++}"
common=(
  -std=c++20
  -fsyntax-only
  -Wall
  -Wconversion
  -Werror
  -Wextra
  -Wpedantic
  -Wshadow
  -DGLYPHRELAY_HAS_NVENC=1
  '-DGLYPHRELAY_M0_PROTOCOL_SHA256="532c2961261f8d20b24559cd5f1461a84b8adfd1d6a2f584354638e7b09c0f06"'
  -I"${repository_root}/tests/stubs"
  -I"${repository_root}/include"
  -isystem
  "${headers_root}/include"
)

"${compiler}" "${common[@]}" "${repository_root}/src/gpu/nvenc_benchmark.cpp"
"${compiler}" "${common[@]}" "${repository_root}/src/gpu/nvenc_browser_fixture.cpp"
