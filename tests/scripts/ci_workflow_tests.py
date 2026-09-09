#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import re
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
matrix_validator = load_script("validate-build-matrix.py")
whitespace_check = load_script("check-committed-whitespace.py")
asset_verifier = load_script("verify-release-assets.py")


class BuildMatrixTests(unittest.TestCase):
    def test_matrix_and_shared_presets_are_consistent(self) -> None:
        self.assertEqual(matrix_validator.validate(), [])

    def test_tiers_select_expected_lanes(self) -> None:
        document = matrix_selector.load_matrix(matrix_selector.DEFAULT_MATRIX)
        fast = matrix_selector.select_tier(document, "fast")["include"]
        full = matrix_selector.select_tier(document, "full")["include"]
        release = matrix_selector.select_tier(document, "release")["include"]

        self.assertEqual([entry["id"] for entry in fast], ["linux-x64"])
        self.assertEqual(len(full), 6)
        self.assertEqual(len(release), 6)
        self.assertEqual(len(asset_verifier.expected_assets(document, "1.2.3")), 12)

    def test_workflow_uses_shared_profiles_and_matrix_parameters(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "_native-build.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("cmake --preset dev", workflow)
        self.assertIn("cmake --preset release", workflow)
        self.assertIn("matrix.triplet", workflow)
        self.assertIn("matrix.build_jobs", workflow)
        self.assertNotIn("matrix.preset", workflow)
        self.assertNotIn("matrix.release_preset", workflow)

    def test_all_workflows_reference_existing_shared_presets(self) -> None:
        document = matrix_validator.load_json(ROOT / "CMakePresets.json")
        names = {
            kind: matrix_validator.public_names(
                matrix_validator.named_entries(document, f"{kind}Presets")
            )
            for kind in ("configure", "build", "test")
        }
        preset_command = re.compile(
            r"\b(cmake|ctest)[ \t]+([^\r\n]*?)--preset(?:[ \t]+|=)"
            r"(?:\"([^\"]+)\"|'([^']+)'|([^\s]+))"
        )
        workflows = ROOT / ".github" / "workflows"
        for path in sorted([*workflows.glob("*.yml"), *workflows.glob("*.yaml")]):
            content = re.sub(r"[\\`]\r?\n[ \t]*", " ", path.read_text(encoding="utf-8"))
            references = list(preset_command.finditer(content))
            self.assertEqual(
                len(references), len(re.findall(r"--preset(?:\s+|=)", content)), path.name
            )
            for match in references:
                executable, arguments, quoted, single_quoted, unquoted = match.groups()
                kind = "test" if executable == "ctest" else (
                    "build" if "--build" in arguments.split() else "configure"
                )
                preset = quoted or single_quoted or unquoted
                with self.subTest(workflow=path.name, kind=kind, preset=preset):
                    self.assertIn(preset, names[kind])

    def test_release_workflow_uses_a_separate_sanitizer_build(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "cmake --preset dev -B build/sanitize -DMINT_ENABLE_SANITIZERS=ON", workflow
        )
        self.assertIn("cmake --build build/sanitize --parallel 4", workflow)
        self.assertIn(
            "ctest --test-dir build/sanitize --output-on-failure --parallel 4", workflow
        )

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
