#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
cd "$repository_root"
bash scripts/build_corpus_image.sh
mkdir -p build
temporary="$(mktemp -d build/corpus-regeneration.XXXXXX)"
trap 'rm -rf "$temporary"' EXIT

docker run --rm --init --ipc=host --platform linux/amd64 \
  --volume "$repository_root:/workspace" \
  --workdir /workspace \
  --env HOME=/tmp \
  --env PLAYWRIGHT_BROWSERS_PATH=/ms-playwright \
  glyphrelay-corpus:protocol-v1 \
  node tooling/corpus/generate-manifests.ts \
    --split development \
    --output "/workspace/$temporary/development.json"

docker run --rm --init --ipc=host --platform linux/amd64 \
  --volume "$repository_root:/workspace" \
  --workdir /workspace \
  --env HOME=/tmp \
  --env PLAYWRIGHT_BROWSERS_PATH=/ms-playwright \
  glyphrelay-corpus:protocol-v1 \
  node tooling/corpus/generate-manifests.ts \
    --split validation \
    --output "/workspace/$temporary/validation.json"

cmp corpus/manifests/development.json "$temporary/development.json"
cmp corpus/manifests/validation.json "$temporary/validation.json"
echo "corpus manifest regeneration passed"
