#!/usr/bin/env bash
set -euo pipefail

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/glyphrelay-clean-check.XXXXXX")"
case "${temporary_root}" in
  */glyphrelay-clean-check.*) ;;
  *)
    echo "refusing unexpected temporary path" >&2
    exit 1
    ;;
esac

git archive --format=tar "$(git write-tree)" | tar -xf - -C "${temporary_root}"
git -C "${temporary_root}" init --quiet
git -C "${temporary_root}" add .

(
  cd "${temporary_root}"
  corepack pnpm install --frozen-lockfile
  uv sync --locked
  make check
)

echo "clean source verification passed: ${temporary_root}"
