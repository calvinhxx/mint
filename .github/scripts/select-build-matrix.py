#!/usr/bin/env python3
"""Select one CI tier from the checked native build matrix."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MATRIX = ROOT / ".github" / "build-matrix.json"
TIERS = ("fast", "full", "release")


def load_matrix(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        entries = document["matrix"]["include"]
    except (KeyError, OSError, TypeError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: invalid build matrix: {error}") from error
    if not isinstance(entries, list) or any(
        not isinstance(entry, dict) for entry in entries
    ):
        raise ValueError(f"{path}: matrix.include must be an object array")
    return document


def select_tier(
    document: dict[str, Any], tier: str
) -> dict[str, list[dict[str, Any]]]:
    if tier not in TIERS:
        raise ValueError(f"unknown build tier: {tier}")
    entries = document["matrix"]["include"]
    selected = [entry for entry in entries if tier in entry.get("tiers", [])]
    if not selected:
        raise ValueError(f"build tier {tier!r} has no native scenarios")
    return {"include": selected}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tier", required=True, choices=TIERS)
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX)
    args = parser.parse_args()
    try:
        matrix = select_tier(load_matrix(args.matrix), args.tier)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(matrix, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
