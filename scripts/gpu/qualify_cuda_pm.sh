#!/usr/bin/env bash
set -euo pipefail

umask 077
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "${repository_root}"
exec python3 -m tools.gpu.handoff --repository "${repository_root}"
