#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"

docker build \
  --platform linux/amd64 \
  --file "${repository_root}/containers/cuda-compile.Dockerfile" \
  --tag glyphrelay-cuda-compile:cuda-13.3.1 \
  "${repository_root}"
