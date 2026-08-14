#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
dependency_root="${repository_root}/.deps/libdatachannel-v0.24.1"
bootstrap_marker="${dependency_root}.bootstrap-incomplete"
patch_file="${repository_root}/patches/libdatachannel-v0.24.1/glyphrelay-final-egress.patch"
expected_commit="a02b751917ac8afc8c58dc6f4461d25ff9465d48"
expected_patch_sha256="9120c0b5df31a1a54198cd1d3bfb972ff2d1b3ab48c8546f1f4a82258778031b"

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "no SHA-256 command is available" >&2
    return 1
  fi
}

actual_patch_sha256="$(sha256_file "${patch_file}")"
if [[ "${actual_patch_sha256}" != "${expected_patch_sha256}" ]]; then
  echo "libdatachannel patch hash mismatch: ${actual_patch_sha256}" >&2
  exit 1
fi

mkdir -p "${repository_root}/.deps"
if [[ ! -d "${dependency_root}/.git" ]]; then
  if [[ -e "${dependency_root}" ]]; then
    echo "refusing non-repository dependency path: ${dependency_root}" >&2
    exit 1
  fi
  touch "${bootstrap_marker}"
  git clone --depth=1 --branch v0.24.1 --recurse-submodules --shallow-submodules \
    https://github.com/paullouisageneau/libdatachannel.git "${dependency_root}"
fi

actual_commit="$(git -C "${dependency_root}" rev-parse HEAD)"
if [[ "${actual_commit}" != "${expected_commit}" ]]; then
  echo "libdatachannel commit mismatch: ${actual_commit}" >&2
  exit 1
fi

if git -C "${dependency_root}" submodule status --recursive | awk '$1 ~ /^[+-]/ { repair = 1 } END { exit !repair }'; then
  git -C "${dependency_root}" submodule sync --recursive
  git -C "${dependency_root}" submodule update --init --recursive --depth=1
fi

declare -a expected_submodules=(
  "deps/json:55f93686c01528224f448c19128836e7df245f72"
  "deps/libjuice:5948a4162d37bc213d6051b67ee2876ccc5a99a6"
  "deps/libsrtp:ee1a77c9f9dc02c42bda9901038c500c5efe4cfa"
  "deps/plog:94899e0b926ac1b0f4750bfbd495167b4a6ae9ef"
  "deps/usrsctp:fec583d54493f879d2ae44a743423bf8a04371ab"
)
for submodule_lock in "${expected_submodules[@]}"; do
  submodule_path="${submodule_lock%%:*}"
  expected_submodule_commit="${submodule_lock##*:}"
  if [[ ! -d "${dependency_root}/${submodule_path}/.git" && \
        ! -f "${dependency_root}/${submodule_path}/.git" ]]; then
    echo "missing libdatachannel submodule: ${submodule_path}" >&2
    exit 1
  fi
  actual_submodule_commit="$(git -C "${dependency_root}/${submodule_path}" rev-parse HEAD)"
  if [[ "${actual_submodule_commit}" != "${expected_submodule_commit}" ]]; then
    echo "${submodule_path} commit mismatch: ${actual_submodule_commit}" >&2
    exit 1
  fi
  submodule_status="$(git -C "${dependency_root}/${submodule_path}" status --porcelain --untracked-files=no)"
  if [[ -f "${bootstrap_marker}" && -n "${submodule_status}" ]] && \
      ! grep -Ev '^(D | D)' <<<"${submodule_status}" >/dev/null; then
    git -C "${dependency_root}/${submodule_path}" restore --source=HEAD --staged --worktree .
  fi
done

if git -C "${dependency_root}" apply --reverse --check "${patch_file}" >/dev/null 2>&1; then
  rm -f "${bootstrap_marker}"
  echo "libdatachannel v0.24.1 patch already verified"
  exit 0
fi

if [[ -n "$(git -C "${dependency_root}" status --porcelain --untracked-files=no)" ]]; then
  echo "refusing to patch a modified libdatachannel dependency checkout" >&2
  exit 1
fi
for submodule_lock in "${expected_submodules[@]}"; do
  submodule_path="${submodule_lock%%:*}"
  if [[ -n "$(git -C "${dependency_root}/${submodule_path}" status --porcelain --untracked-files=no)" ]]; then
    echo "refusing to patch a modified ${submodule_path} checkout" >&2
    exit 1
  fi
done

git -C "${dependency_root}" apply --check "${patch_file}"
git -C "${dependency_root}" apply "${patch_file}"
git -C "${dependency_root}" apply --reverse --check "${patch_file}"
rm -f "${bootstrap_marker}"
echo "libdatachannel v0.24.1 final-egress patch verified: ${actual_commit}"
