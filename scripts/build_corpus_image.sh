#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
cd "$repository_root"

uv run python tools/corpus/bootstrap_assets.py
docker build \
  --platform linux/amd64 \
  --file containers/corpus.Dockerfile \
  --tag glyphrelay-corpus:protocol-v1 \
  .cache/corpus

docker run --rm --platform linux/amd64 glyphrelay-corpus:protocol-v1 \
  bash -lc 'test "$(node --version)" = "v24.18.1" && test "$(tesseract --version 2>&1 | head -n1)" = "tesseract 5.3.4"'
