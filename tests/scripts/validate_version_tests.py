#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

SPEC = importlib.util.spec_from_file_location(
    "mint_validate_version", ROOT / "scripts" / "validate-version.py"
)
assert SPEC is not None and SPEC.loader is not None
validate_version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validate_version)


class ValidateVersionTests(unittest.TestCase):
    version = "1.2.3"

    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self._write_release_fixture()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def _write_release_fixture(self) -> None:
        (self.root / "include" / "mint").mkdir(parents=True)
        (self.root / "docs" / "getting-started").mkdir(parents=True)
        (self.root / "docs" / "project").mkdir(parents=True)
        (self.root / "CMakeLists.txt").write_text(
            f"project(mint VERSION {self.version} LANGUAGES CXX)\n", encoding="utf-8"
        )
        (self.root / "vcpkg.json").write_text(
            json.dumps({"version-string": self.version}), encoding="utf-8"
        )
        (self.root / "include" / "mint" / "version.hpp").write_text(
            f'inline constexpr std::string_view version = "{self.version}";\n',
            encoding="utf-8",
        )
        self._write_changelog("2026-08-31")

        release_url = validate_version.RELEASE_URL.format(version=self.version)
        current_release = f"Current release: [`v{self.version}`]({release_url}).\n"
        (self.root / "README.md").write_text(current_release, encoding="utf-8")
        (self.root / "docs" / "getting-started" / "quickstart.md").write_text(
            current_release, encoding="utf-8"
        )
        (self.root / "docs" / "project" / "roadmap.md").write_text(
            current_release + "\n## Released\n", encoding="utf-8"
        )

    def _write_changelog(self, release_state: str) -> None:
        (self.root / "CHANGELOG.md").write_text(
            f"# Changelog\n\n## {self.version} - {release_state}\n",
            encoding="utf-8",
        )

    def _validate_tag(self) -> str:
        with mock.patch.object(validate_version, "ROOT", self.root), mock.patch.object(
            validate_version, "validate_release_evidence"
        ) as evidence:
            version = validate_version.validate(f"v{self.version}")
            evidence.assert_called_once_with(self.root, self.version)
            return version

    def test_dated_release_without_pending_markers_passes(self) -> None:
        self.assertEqual(self._validate_tag(), self.version)

    def test_tag_rejects_current_version_marked_in_development(self) -> None:
        roadmap = self.root / "docs" / "project" / "roadmap.md"
        roadmap.write_text(
            roadmap.read_text(encoding="utf-8") + f"\n## v{self.version} 开发中\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "marks v1.2.3 as in development"):
            self._validate_tag()

    def test_tag_rejects_pending_release_section(self) -> None:
        roadmap = self.root / "docs" / "project" / "roadmap.md"
        roadmap.write_text(
            roadmap.read_text(encoding="utf-8") + "\n## 发布前剩余\n\n1. Publish.\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "pre-release checklist"):
            self._validate_tag()

    def test_tag_still_rejects_unreleased_changelog(self) -> None:
        self._write_changelog("Unreleased")
        with self.assertRaisesRegex(ValueError, "must date 1.2.3 before tagging"):
            self._validate_tag()

    def test_tag_still_requires_current_release_link(self) -> None:
        (self.root / "README.md").write_text("# mint\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "does not identify v1.2.3"):
            self._validate_tag()

    def test_tag_still_requires_current_changelog_section(self) -> None:
        (self.root / "CHANGELOG.md").write_text(
            "# Changelog\n\n## 1.2.2 - 2026-08-30\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(ValueError, "has no valid section for 1.2.3"):
            self._validate_tag()


if __name__ == "__main__":
    unittest.main()
