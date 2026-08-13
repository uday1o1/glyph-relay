#!/usr/bin/env bash
set -euo pipefail

image="glyphrelay-linux-cpu:ubuntu-24.04-amd64"

docker build \
  --file containers/linux-cpu.Dockerfile \
  --platform linux/amd64 \
  --tag "${image}" \
  .
docker run --rm --platform linux/amd64 "${image}"
