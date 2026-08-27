#!/usr/bin/env python3
"""Check that CI lanes and public CMake presets describe the same native builds."""

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
    "preset",
    "system_packages",
}
EXPECTED = {
    ("windows", "x64"): ("Windows", "x64-windows", "X64", "windows-2022", []),
    ("windows", "arm64"): ("Windows", "arm64-windows", "ARM64", "windows-11-arm", []),
    ("macos", "x64"): ("Darwin", "x64-osx", "X64", "macos-15-intel", []),
    ("macos", "arm64"): ("Darwin", "arm64-osx", "ARM64", "macos-15", []),
    ("linux", "x64"): ("Linux", "x64-linux", "X64", "ubuntu-24.04", ["bubblewrap"]),
    ("linux", "arm64"): (
        "Linux",
        "arm64-linux",
        "ARM64",
        "ubuntu-24.04-arm",
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
        string_fields = REQUIRED_FIELDS - {"system_packages"}
        if any(not isinstance(scenario[field], str) or not scenario[field] for field in string_fields):
            errors.append(f"matrix.include[{index}] string fields must be non-empty")
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
        if scenario["id"] != f"{platform}-{arch}":
            errors.append(f"{context}: id must match platform-arch")
        if key in seen:
            errors.append(f"{context}: duplicate platform and architecture")
        seen.add(key)

        expected = EXPECTED.get(key)
        if expected is None:
            errors.append(f"{context}: unsupported platform or architecture")
            continue
        host_system, triplet, runner_arch, runner, system_packages = expected
        if scenario["runner_arch"] != runner_arch:
            errors.append(f"{context}: runner_arch must be {runner_arch}")
        if scenario["runner"] != runner:
            errors.append(f"{context}: runner must be {runner}")
        if scenario["system_packages"] != system_packages:
            errors.append(f"{context}: system_packages must be {system_packages}")

        preset_name = scenario["preset"]
        preset = configure_presets.get(preset_name)
        if preset is None:
            errors.append(f"{context}: configure preset {preset_name!r} does not exist")
            continue
        condition = preset.get("condition")
        if not isinstance(condition, dict) or condition.get("rhs") != host_system:
            errors.append(f"{context}: configure preset must target host {host_system}")
        cache = preset.get("cacheVariables")
        if not isinstance(cache, dict) or cache.get("VCPKG_TARGET_TRIPLET") != triplet:
            errors.append(f"{context}: configure preset must use triplet {triplet}")
        if not isinstance(cache, dict) or cache.get("CMAKE_BUILD_TYPE") != "Debug":
            errors.append(f"{context}: configure preset must use Debug")

        for kind, entries in (("build", build_presets), ("test", test_presets)):
            entry = entries.get(preset_name)
            if entry is None or entry.get("configurePreset") != preset_name:
                errors.append(f"{context}: matching {kind} preset is missing")

    missing_scenarios = EXPECTED.keys() - seen
    extra_scenarios = seen - EXPECTED.keys()
    for platform, arch in sorted(missing_scenarios):
        errors.append(f"missing scenario: {platform}-{arch}")
    for platform, arch in sorted(extra_scenarios):
        errors.append(f"unexpected scenario: {platform}-{arch}")
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
    print(f"Validated {len(EXPECTED)} native build scenarios.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
