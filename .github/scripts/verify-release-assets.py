#!/usr/bin/env python3
"""Verify the exact release asset set described by the native build matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MATRIX = ROOT / ".github" / "build-matrix.json"
PROJECT_VERSION = re.compile(
    r"^project\(mint VERSION ([0-9]+\.[0-9]+\.[0-9]+)(?:\s|\))", re.MULTILINE
)
CHECKSUM = re.compile(r"([0-9a-f]{64})  ([^\r\n]+)\n?")


def read_version(path: Path = ROOT / "CMakeLists.txt") -> str:
    match = PROJECT_VERSION.search(path.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"{path}: mint project version not found")
    return match.group(1)


def read_matrix(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        entries = document["matrix"]["include"]
    except (KeyError, OSError, TypeError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: invalid build matrix: {error}") from error
    if not isinstance(entries, list):
        raise ValueError(f"{path}: matrix.include must be an array")
    return document


def expected_assets(document: dict[str, Any], version: str) -> set[str]:
    archives: set[str] = set()
    for entry in document["matrix"]["include"]:
        if not isinstance(entry, dict) or "release" not in entry.get("tiers", []):
            continue
        platform = entry.get("platform")
        arch = entry.get("arch")
        if not isinstance(platform, str) or not isinstance(arch, str):
            raise ValueError("release matrix entries require platform and arch")
        extension = "zip" if platform == "windows" else "tar.gz"
        archives.add(f"mint-{version}-{platform}-{arch}.{extension}")
    if not archives:
        raise ValueError("release tier has no package scenarios")
    return archives | {f"{archive}.sha256" for archive in archives}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify(directory: Path, document: dict[str, Any], version: str) -> int:
    if not directory.is_dir():
        raise ValueError(f"{directory}: release asset directory does not exist")
    entries = list(directory.iterdir())
    invalid = sorted(
        path.name for path in entries if not path.is_file() or path.is_symlink()
    )
    if invalid:
        raise ValueError(f"release assets must be regular files: {', '.join(invalid)}")

    expected = expected_assets(document, version)
    actual = {path.name for path in entries}
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing or extra:
        details = []
        if missing:
            details.append(f"missing: {', '.join(missing)}")
        if extra:
            details.append(f"unexpected: {', '.join(extra)}")
        detail = "; ".join(details)
        raise ValueError(f"release asset set does not match matrix ({detail})")

    for checksum_name in sorted(name for name in expected if name.endswith(".sha256")):
        checksum_path = directory / checksum_name
        match = CHECKSUM.fullmatch(checksum_path.read_text(encoding="utf-8"))
        archive_name = checksum_name.removesuffix(".sha256")
        if match is None or match.group(2) != archive_name:
            raise ValueError(f"{checksum_name}: checksum must name {archive_name}")
        if match.group(1) != sha256(directory / archive_name):
            raise ValueError(f"{checksum_name}: checksum mismatch")
    return len(expected)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX)
    args = parser.parse_args()
    try:
        count = verify(args.directory, read_matrix(args.matrix), read_version())
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(str(error))
    print(f"Verified {count} release asset files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
