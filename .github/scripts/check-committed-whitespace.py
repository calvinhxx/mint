#!/usr/bin/env python3
"""Run git's whitespace check against the commits represented by an Actions event."""

from __future__ import annotations

import argparse
import os
import subprocess
from collections.abc import Callable
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ZERO_SHA = "0" * 40


def git_commit_exists(root: Path, revision: str) -> bool:
    result = subprocess.run(
        ["git", "cat-file", "-e", f"{revision}^{{commit}}"],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def check_command(
    event: str,
    before: str,
    base: str,
    head: str,
    commit_exists: Callable[[str], bool],
) -> list[str]:
    if not head:
        raise ValueError("head revision is required")
    if event == "pull_request":
        if not base:
            raise ValueError("pull request base revision is required")
        return ["git", "diff", "--check", f"{base}...{head}"]
    if before and before != ZERO_SHA and commit_exists(before):
        return ["git", "diff", "--check", f"{before}..{head}"]
    return ["git", "show", "--check", "--format=", head]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--event", default=os.getenv("GITHUB_EVENT_NAME", ""))
    parser.add_argument("--before", default=os.getenv("MINT_BEFORE_SHA", ""))
    parser.add_argument("--base", default=os.getenv("MINT_PR_BASE_SHA", ""))
    parser.add_argument("--head", default=os.getenv("GITHUB_SHA", "HEAD"))
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        command = check_command(
            args.event,
            args.before,
            args.base,
            args.head,
            lambda revision: git_commit_exists(args.root, revision),
        )
    except ValueError as error:
        parser.error(str(error))
    return subprocess.run(command, cwd=args.root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
