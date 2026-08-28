#!/usr/bin/env python3
"""Require immutable commit references for external GitHub Actions."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
USES = re.compile(r"^\s*uses:\s*([^\s#]+)")
PINNED = re.compile(r"^[^@]+@[0-9a-f]{40}$")


def validate() -> list[str]:
    errors: list[str] = []
    for path in sorted((*WORKFLOWS.glob("*.yml"), *WORKFLOWS.glob("*.yaml"))):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = USES.match(line)
            if match is None:
                continue
            reference = match.group(1)
            if reference.startswith("./") or PINNED.fullmatch(reference):
                continue
            errors.append(f"{path.relative_to(ROOT)}:{number}: action must use a full commit SHA")
    return errors


def main() -> int:
    errors = validate()
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Validated immutable GitHub Action references.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
