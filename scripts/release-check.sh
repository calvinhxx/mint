#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$script_dir/.." && pwd)"
cd "$project_root"

: "${VCPKG_ROOT:?Set VCPKG_ROOT to the vcpkg checkout before running release checks}"

project_version="$(
    sed -nE 's/^project\(mint VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt
)"
manifest_version="$(
    sed -nE 's/^[[:space:]]*"version-string":[[:space:]]*"([^"]+)".*/\1/p' vcpkg.json
)"

if [[ -z "$project_version" || "$project_version" != "$manifest_version" ]]; then
    echo "Version mismatch: CMake='$project_version', vcpkg='$manifest_version'" >&2
    exit 1
fi
if ! grep -Eq "^## $project_version - (Unreleased|[0-9]{4}-[0-9]{2}-[0-9]{2})$" \
    CHANGELOG.md; then
    echo "CHANGELOG.md has no valid section for $project_version" >&2
    exit 1
fi

git diff --check
python3 .github/scripts/validate-build-matrix.py

cmake --preset vcpkg-dev
cmake --build --preset vcpkg-dev
cmake --build --preset vcpkg-dev --target format-check
ctest --preset vcpkg-dev

cmake --preset vcpkg-release
cmake --build --preset vcpkg-release

cmake --preset vcpkg-sanitize
cmake --build --preset vcpkg-sanitize
ctest --preset vcpkg-sanitize

state_dir="$(mktemp -d "${TMPDIR:-/tmp}/mint-release-check.XXXXXX")"
trap 'rm -rf -- "$state_dir"' EXIT

./build/vcpkg-release/mint init \
    --root "$project_root" \
    --state-dir "$state_dir" \
    --json \
    >/dev/null

./build/vcpkg-release/mint run \
    --root "$project_root" \
    --state-dir "$state_dir" \
    --demo \
    --json \
    "summarize this project" >/dev/null
