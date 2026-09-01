#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$script_dir/.." && pwd)"
cd "$project_root"

: "${VCPKG_ROOT:?Set VCPKG_ROOT to the vcpkg checkout before running release checks}"

git diff --check
python3 scripts/validate-version.py
python3 .github/scripts/validate-build-matrix.py
python3 .github/scripts/validate-workflows.py

cmake --preset vcpkg-dev
cmake --build --preset vcpkg-dev
cmake --build --preset vcpkg-dev --target format-check
ctest --preset vcpkg-dev

cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
cpack --config build/vcpkg-release/CPackConfig.cmake
cmake -DMINT_BUILD_DIR=build/vcpkg-release -P cmake/VerifyPackage.cmake

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
