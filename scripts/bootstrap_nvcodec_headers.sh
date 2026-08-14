#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
dependency_root="${repository_root}/.deps/nv-codec-headers-n13.1.15.0"
expected_commit="0a6fba9a2820628b8103464f4c8753ee05838baa"
expected_header_sha256="8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228"

mkdir -p "${repository_root}/.deps"
if [[ ! -d "${dependency_root}/.git" ]]; then
  if [[ -e "${dependency_root}" ]]; then
    echo "refusing non-repository dependency path: ${dependency_root}" >&2
    exit 1
  fi
  git clone --depth=1 --branch n13.1.15.0 \
    https://github.com/FFmpeg/nv-codec-headers.git "${dependency_root}"
fi

actual_commit="$(git -C "${dependency_root}" rev-parse HEAD)"
if [[ "${actual_commit}" != "${expected_commit}" ]]; then
  echo "nv-codec-headers commit mismatch: ${actual_commit}" >&2
  exit 1
fi

header="${dependency_root}/include/ffnvcodec/nvEncodeAPI.h"
if command -v sha256sum >/dev/null 2>&1; then
  actual_header_sha256="$(sha256sum "${header}" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
  actual_header_sha256="$(shasum -a 256 "${header}" | awk '{print $1}')"
else
  echo "no SHA-256 command is available" >&2
  exit 1
fi
if [[ "${actual_header_sha256}" != "${expected_header_sha256}" ]]; then
  echo "nvEncodeAPI.h hash mismatch: ${actual_header_sha256}" >&2
  exit 1
fi

echo "nv-codec-headers n13.1.15.0 verified: ${actual_commit}"
