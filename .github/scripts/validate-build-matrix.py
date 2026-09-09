#!/usr/bin/env python3
"""Validate the native CI matrix and its shared CMake build profiles."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
MATRIX_PATH = ROOT / ".github" / "build-matrix.json"
PRESETS_PATH = ROOT / "CMakePresets.json"

REQUIRED_FIELDS = {
    "id",
    "name",
    "platform",
    "arch",
    "runner",
    "runner_arch",
    "triplet",
    "build_jobs",
    "tiers",
    "system_packages",
}
ALLOWED_TIERS = {"fast", "full", "release"}
EXPECTED = {
    ("windows", "x64"): ("x64-windows", "X64", "windows-2022", 1, []),
    ("windows", "arm64"): ("arm64-windows", "ARM64", "windows-11-arm", 1, []),
    ("macos", "x64"): ("x64-osx", "X64", "macos-15-intel", 4, []),
    ("macos", "arm64"): ("arm64-osx", "ARM64", "macos-15", 4, []),
    ("linux", "x64"): (
        "x64-linux",
        "X64",
        "ubuntu-24.04",
        4,
        ["bubblewrap", "clang-format"],
    ),
    ("linux", "arm64"): (
        "arm64-linux",
        "ARM64",
        "ubuntu-24.04-arm",
        4,
        ["bubblewrap"],
    ),
}


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: cannot read valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path}: root must be an object")
    return value


def named_entries(document: dict[str, Any], key: str) -> dict[str, dict[str, Any]]:
    entries = document.get(key)
    if not isinstance(entries, list):
        raise ValueError(f"{PRESETS_PATH}: {key} must be an array")
    return {
        entry["name"]: entry
        for entry in entries
        if isinstance(entry, dict) and isinstance(entry.get("name"), str)
    }


def public_names(presets: dict[str, dict[str, Any]]) -> set[str]:
    return {name for name, preset in presets.items() if preset.get("hidden") is not True}


def resolved_cache(
    name: str,
    presets: dict[str, dict[str, Any]],
    stack: tuple[str, ...] = (),
) -> dict[str, Any]:
    if name in stack:
        raise ValueError(f"{PRESETS_PATH}: configure preset inheritance cycle at {name!r}")
    preset = presets.get(name)
    if preset is None:
        raise ValueError(f"{PRESETS_PATH}: configure preset {name!r} does not exist")

    inherited = preset.get("inherits", [])
    if isinstance(inherited, str):
        parents = [inherited]
    elif isinstance(inherited, list) and all(isinstance(parent, str) for parent in inherited):
        parents = inherited
    else:
        raise ValueError(f"{PRESETS_PATH}: {name!r} has invalid inherits")

    cache: dict[str, Any] = {}
    for parent in parents:
        cache.update(resolved_cache(parent, presets, (*stack, name)))
    own_cache = preset.get("cacheVariables", {})
    if not isinstance(own_cache, dict):
        raise ValueError(f"{PRESETS_PATH}: {name!r} cacheVariables must be an object")
    cache.update(own_cache)
    return cache


def validate_presets(
    configure_presets: dict[str, dict[str, Any]],
    build_presets: dict[str, dict[str, Any]],
    test_presets: dict[str, dict[str, Any]],
) -> list[str]:
    errors: list[str] = []
    expected_public = {
        "configure": ({"dev", "release"}, configure_presets),
        "build": ({"dev", "release"}, build_presets),
        "test": ({"dev"}, test_presets),
    }
    for kind, (expected, presets) in expected_public.items():
        actual = public_names(presets)
        if actual != expected:
            errors.append(
                f"public {kind} presets must be exactly: {', '.join(sorted(expected))}"
            )

    for name in ("dev", "release"):
        build = build_presets.get(name)
        if build is None or build.get("configurePreset") != name:
            errors.append(f"matching build preset is missing for {name!r}")

    dev_test = test_presets.get("dev")
    if dev_test is None or dev_test.get("configurePreset") != "dev":
        errors.append("matching test preset is missing for 'dev'")
    if "release" in test_presets:
        errors.append("release must not have a test preset")

    if "dev" in configure_presets:
        cache = resolved_cache("dev", configure_presets)
        if cache.get("CMAKE_BUILD_TYPE") != "Debug":
            errors.append("dev preset must use Debug")
        if cache.get("BUILD_TESTING") is not True:
            errors.append("dev preset must enable BUILD_TESTING")
        if cache.get("VCPKG_MANIFEST_FEATURES") != "tests":
            errors.append("dev preset must install the tests feature")

    if "release" in configure_presets:
        cache = resolved_cache("release", configure_presets)
        if cache.get("CMAKE_BUILD_TYPE") != "Release":
            errors.append("release preset must use Release")
        if cache.get("BUILD_TESTING") is not False:
            errors.append("release preset must disable BUILD_TESTING")
        if cache.get("MINT_ENABLE_DEVELOPER_TOOLS") is not False:
            errors.append("release preset must disable developer tools")
        if cache.get("VCPKG_MANIFEST_FEATURES"):
            errors.append("release preset must not install test features")
    return errors


def validate() -> list[str]:
    matrix_document = load_json(MATRIX_PATH)
    presets_document = load_json(PRESETS_PATH)
    errors: list[str] = []

    if matrix_document.get("schema_version") != 1:
        errors.append("build matrix schema_version must be 1")
    matrix = matrix_document.get("matrix")
    scenarios = matrix.get("include") if isinstance(matrix, dict) else None
    if not isinstance(scenarios, list):
        return [*errors, "build matrix must contain matrix.include array"]

    configure_presets = named_entries(presets_document, "configurePresets")
    build_presets = named_entries(presets_document, "buildPresets")
    test_presets = named_entries(presets_document, "testPresets")
    errors.extend(validate_presets(configure_presets, build_presets, test_presets))

    seen: set[tuple[str, str]] = set()
    for index, scenario in enumerate(scenarios):
        if not isinstance(scenario, dict):
            errors.append(f"matrix.include[{index}] must be an object")
            continue
        missing = REQUIRED_FIELDS - scenario.keys()
        extra = scenario.keys() - REQUIRED_FIELDS
        if missing:
            errors.append(f"matrix.include[{index}] is missing: {', '.join(sorted(missing))}")
        if extra:
            errors.append(
                f"matrix.include[{index}] has unknown fields: {', '.join(sorted(extra))}"
            )
        if missing:
            continue

        string_fields = REQUIRED_FIELDS - {"build_jobs", "system_packages", "tiers"}
        if any(
            not isinstance(scenario[field], str) or not scenario[field]
            for field in string_fields
        ):
            errors.append(f"matrix.include[{index}] string fields must be non-empty")
            continue
        build_jobs = scenario["build_jobs"]
        if type(build_jobs) is not int or build_jobs < 1:
            errors.append(f"matrix.include[{index}].build_jobs must be a positive integer")
            continue

        tiers = scenario["tiers"]
        if (
            not isinstance(tiers, list)
            or not tiers
            or any(not isinstance(tier, str) or tier not in ALLOWED_TIERS for tier in tiers)
            or len(tiers) != len(set(tiers))
        ):
            errors.append(
                f"matrix.include[{index}].tiers must contain unique values from "
                f"{sorted(ALLOWED_TIERS)}"
            )
            continue
        packages = scenario["system_packages"]
        if not isinstance(packages, list) or any(
            not isinstance(package, str) or not package for package in packages
        ):
            errors.append(f"matrix.include[{index}].system_packages must be a string array")
            continue

        platform = scenario["platform"]
        arch = scenario["arch"]
        key = (platform, arch)
        context = scenario["id"]
        if context != f"{platform}-{arch}":
            errors.append(f"{context}: id must match platform-arch")
        if key in seen:
            errors.append(f"{context}: duplicate platform and architecture")
        seen.add(key)

        expected = EXPECTED.get(key)
        if expected is None:
            errors.append(f"{context}: unsupported platform or architecture")
            continue
        triplet, runner_arch, runner, build_jobs, system_packages = expected
        if scenario["triplet"] != triplet:
            errors.append(f"{context}: triplet must be {triplet}")
        if scenario["runner_arch"] != runner_arch:
            errors.append(f"{context}: runner_arch must be {runner_arch}")
        if scenario["runner"] != runner:
            errors.append(f"{context}: runner must be {runner}")
        if scenario["build_jobs"] != build_jobs:
            errors.append(f"{context}: build_jobs must be {build_jobs}")
        if scenario["system_packages"] != system_packages:
            errors.append(f"{context}: system_packages must be {system_packages}")

    missing_scenarios = EXPECTED.keys() - seen
    extra_scenarios = seen - EXPECTED.keys()
    for platform, arch in sorted(missing_scenarios):
        errors.append(f"missing scenario: {platform}-{arch}")
    for platform, arch in sorted(extra_scenarios):
        errors.append(f"unexpected scenario: {platform}-{arch}")

    tier_members = {
        tier: {
            scenario.get("id")
            for scenario in scenarios
            if isinstance(scenario, dict)
            and isinstance(scenario.get("tiers"), list)
            and tier in scenario["tiers"]
        }
        for tier in ALLOWED_TIERS
    }
    expected_ids = {f"{platform}-{arch}" for platform, arch in EXPECTED}
    for tier in ("full", "release"):
        if tier_members[tier] != expected_ids:
            errors.append(f"{tier} tier must contain every native build scenario")
    if tier_members["fast"] != {"linux-x64"}:
        errors.append("fast tier must contain only linux-x64")
    return errors


def main() -> int:
    try:
        errors = validate()
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"{MATRIX_PATH}: {error}", file=sys.stderr)
        return 1
    print(f"Validated {len(EXPECTED)} native build scenarios with shared presets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
