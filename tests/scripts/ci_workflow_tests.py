#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path
from types import ModuleType


ROOT = Path(__file__).resolve().parents[2]


def load_script(filename: str) -> ModuleType:
    path = ROOT / ".github" / "scripts" / filename
    spec = importlib.util.spec_from_file_location(filename.replace("-", "_"), path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


matrix_selector = load_script("select-build-matrix.py")
whitespace_check = load_script("check-committed-whitespace.py")
asset_verifier = load_script("verify-release-assets.py")


class BuildMatrixTests(unittest.TestCase):
    def test_tiers_select_expected_lanes(self) -> None:
        document = matrix_selector.load_matrix(matrix_selector.DEFAULT_MATRIX)
        fast = matrix_selector.select_tier(document, "fast")["include"]
        full = matrix_selector.select_tier(document, "full")["include"]
        release = matrix_selector.select_tier(document, "release")["include"]

        self.assertEqual([entry["id"] for entry in fast], ["linux-x64"])
        self.assertEqual(len(full), 6)
        self.assertEqual(len(release), 6)
        self.assertEqual(len(asset_verifier.expected_assets(document, "1.2.3")), 12)

    def test_unknown_tier_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown build tier"):
            matrix_selector.select_tier({"matrix": {"include": []}}, "nightly")


class WhitespaceRangeTests(unittest.TestCase):
    def test_pull_request_uses_merge_base_range(self) -> None:
        command = whitespace_check.check_command(
            "pull_request", "", "base", "head", lambda _: False
        )
        self.assertEqual(command, ["git", "diff", "--check", "base...head"])

    def test_push_uses_before_range_when_commit_exists(self) -> None:
        command = whitespace_check.check_command(
            "push", "before", "", "head", lambda revision: revision == "before"
        )
        self.assertEqual(command, ["git", "diff", "--check", "before..head"])

    def test_new_ref_checks_head_commit(self) -> None:
        command = whitespace_check.check_command(
            "push", whitespace_check.ZERO_SHA, "", "head", lambda _: False
        )
        self.assertEqual(command, ["git", "show", "--check", "--format=", "head"])


class ReleaseAssetTests(unittest.TestCase):
    MATRIX = {
        "matrix": {
            "include": [
                {"platform": "windows", "arch": "x64", "tiers": ["release"]},
                {"platform": "linux", "arch": "arm64", "tiers": ["release"]},
            ]
        }
    }

    def write_assets(self, directory: Path) -> None:
        names = asset_verifier.expected_assets(self.MATRIX, "1.2.3")
        archives = sorted(name for name in names if not name.endswith(".sha256"))
        for archive_name in archives:
            content = archive_name.encode("utf-8")
            (directory / archive_name).write_bytes(content)
            digest = hashlib.sha256(content).hexdigest()
            (directory / f"{archive_name}.sha256").write_text(
                f"{digest}  {archive_name}\n", encoding="utf-8"
            )

    def test_exact_asset_set_and_checksums_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.write_assets(directory)
            self.assertEqual(asset_verifier.verify(directory, self.MATRIX, "1.2.3"), 4)

    def test_unexpected_asset_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.write_assets(directory)
            (directory / "old-package.zip").write_bytes(b"stale")
            with self.assertRaisesRegex(ValueError, "unexpected: old-package.zip"):
                asset_verifier.verify(directory, self.MATRIX, "1.2.3")


if __name__ == "__main__":
    unittest.main()
