#!/usr/bin/env python3
"""Keep release metadata aligned and validate version tags."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEMVER = r"[0-9]+\.[0-9]+\.[0-9]+"


def require_match(path: Path, pattern: str, label: str) -> str:
    match = re.search(pattern, path.read_text(encoding="utf-8"), re.MULTILINE)
    if match is None:
        raise ValueError(f"{path.relative_to(ROOT)} has no {label}")
    return match.group(1)


def validate(tag: str | None) -> str:
    cmake_version = require_match(
        ROOT / "CMakeLists.txt",
        rf"^project\(mint VERSION ({SEMVER})(?:\s|\))",
        "project version",
    )
    manifest = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))
    versions = {
        "CMakeLists.txt": cmake_version,
        "vcpkg.json": manifest.get("version-string"),
        "include/mint/version.hpp": require_match(
            ROOT / "include/mint/version.hpp",
            rf'^inline constexpr std::string_view version = "({SEMVER})";',
            "runtime version",
        ),
    }
    mismatches = {path: version for path, version in versions.items() if version != cmake_version}
    if mismatches:
        details = ", ".join(f"{path}={version!r}" for path, version in mismatches.items())
        raise ValueError(f"version mismatch: CMakeLists.txt={cmake_version!r}, {details}")

    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    heading = re.search(
        rf"^## {re.escape(cmake_version)} - (Unreleased|[0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}})$",
        changelog,
        re.MULTILINE,
    )
    if heading is None:
        raise ValueError(f"CHANGELOG.md has no valid section for {cmake_version}")

    if tag is not None:
        expected_tag = f"v{cmake_version}"
        if tag != expected_tag:
            raise ValueError(f"tag {tag!r} does not match {expected_tag!r}")
        if heading.group(1) == "Unreleased":
            raise ValueError(f"CHANGELOG.md must date {cmake_version} before tagging")

    return cmake_version


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", help="require an exact vMAJOR.MINOR.PATCH release tag")
    args = parser.parse_args()
    try:
        version = validate(args.tag)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Validated mint version {version}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
