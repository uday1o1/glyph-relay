#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -lt 1 ]]; then
  echo "usage: scripts/run_corpus_container.sh COMMAND [ARG ...]" >&2
  exit 2
fi

repository_root="$(git rev-parse --show-toplevel)"
"$repository_root/scripts/build_corpus_image.sh"

docker run --rm --init --ipc=host --platform linux/amd64 \
  --volume "$repository_root:/workspace" \
  --workdir /workspace \
  --env HOME=/tmp \
  --env PLAYWRIGHT_BROWSERS_PATH=/ms-playwright \
  glyphrelay-corpus:protocol-v1 "$@"
