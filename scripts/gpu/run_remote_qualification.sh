#!/usr/bin/env bash
set -euo pipefail

umask 077
export PYTHONDONTWRITEBYTECODE=1
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
if [[ "$(pwd -P)" != "${source_root}" ]]; then
  echo "run_remote_qualification.sh must run from its content-addressed source root" >&2
  exit 5
fi
exec python3 -m tools.gpu.remote_qualification "$@"
