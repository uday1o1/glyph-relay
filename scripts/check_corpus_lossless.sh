#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
cd "$repository_root"

bash scripts/build_corpus_image.sh

if [[ -e corpus/generated/development ]]; then
  echo "development corpus output already exists; refusing to overwrite it" >&2
  exit 2
fi

docker run --rm --init --ipc=host --platform linux/amd64 \
  --volume "$repository_root:/workspace" \
  --workdir /workspace \
  --env HOME=/tmp \
  --env PLAYWRIGHT_BROWSERS_PATH=/ms-playwright \
  glyphrelay-corpus:protocol-v1 \
  node tooling/corpus/render-development.ts \
    --manifest corpus/manifests/development.json \
    --output corpus/generated/development/render

docker run --rm --init --platform linux/amd64 \
  --volume "$repository_root:/workspace" \
  --workdir /workspace \
  glyphrelay-corpus:protocol-v1 \
  bash tools/corpus/run_tesseract.sh \
    corpus/generated/development/render/ocr-inputs \
    corpus/generated/development/ocr-results

uv run python tools/corpus/evaluate_ocr.py \
  --manifest corpus/manifests/development.json \
  --ocr-results corpus/generated/development/ocr-results \
  --output corpus/generated/development/lossless-ocr.json
