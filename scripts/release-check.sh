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

cmake --preset dev
cmake --build --preset dev
cmake --build --preset dev --target format-check
ctest --preset dev

cmake --preset release
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake
cmake -DMINT_BUILD_DIR=build/release -P cmake/VerifyPackage.cmake

cmake --preset dev -B build/sanitize -DMINT_ENABLE_SANITIZERS=ON
cmake --build build/sanitize --parallel 4
ctest --test-dir build/sanitize --output-on-failure --parallel 4

state_dir="$(mktemp -d "${TMPDIR:-/tmp}/mint-release-check.XXXXXX")"
trap 'rm -rf -- "$state_dir"' EXIT

./build/release/mint init \
    --root "$project_root" \
    --state-dir "$state_dir" \
    --json \
    >/dev/null

./build/release/mint run \
    --root "$project_root" \
    --state-dir "$state_dir" \
    --demo \
    --json \
    "summarize this project" >/dev/null
