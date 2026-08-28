#!/usr/bin/env python3

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import release_evidence  # noqa: E402


def reports(source_digest: str) -> tuple[dict, dict]:
    ids, matrix_digest = release_evidence._provider_matrix(ROOT)
    provider = {
        "schema_version": 1,
        "operation": "provider_regression",
        "mode": "live",
        "generated_at_utc": "2026-08-29T00:00:00Z",
        "mint_version": "1.5.0",
        "source_sha256": source_digest,
        "matrix_sha256": matrix_digest,
        "status": "passed",
        "profiles": [
            {
                "id": profile_id,
                "status": "passed",
                "acceptance": {
                    "requests": 2,
                    "checks": {
                        "function_call": True,
                        "arguments_round_trip": True,
                        "tool_result_continuation": True,
                    },
                },
            }
            for profile_id in ids
        ],
    }
    config = ROOT / "configs" / "providers" / "openai-responses.json"
    fixture = {
        "schema_version": 1,
        "operation": "fixture_regression",
        "mode": "live",
        "generated_at_utc": "2026-08-29T00:01:00Z",
        "mint_version": "1.5.0",
        "source_sha256": source_digest,
        "config_sha256": release_evidence._file_sha256(config),
        "fixture_sha256": release_evidence._fixture_digest(ROOT),
        "profile": {"provider": "openai", "adapter": "responses", "stream": True},
        "status": "passed",
        "baseline": {"configure": "passed", "build": "passed", "test": "failed_as_expected"},
        "agent": {
            "completed": True,
            "status": "completed",
            "verification_status": "passed",
            "execution": {"verification_commands": 1},
            "model": {"provider": "openai", "adapter": "responses", "streamed_calls": 3},
            "changes": {"files": ["FIX_REPORT.md", "src/calculator.cpp"]},
        },
        "independent_verification": {"configure": "passed", "build": "passed", "test": "passed"},
    }
    return provider, fixture


class ReleaseEvidenceTests(unittest.TestCase):
    def test_source_digest_ignores_only_release_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "release" / "evidence" / "v1.5.0").mkdir(parents=True)
            (root / "src.cpp").write_text("one\n", encoding="utf-8")
            (root / "CHANGELOG.md").write_text("draft\n", encoding="utf-8")
            evidence = root / "release" / "evidence" / "v1.5.0" / "provider-regression.json"
            evidence.write_text("{}\n", encoding="utf-8")
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)

            before = release_evidence.release_source_digest(root)
            (root / "CHANGELOG.md").write_text("dated\n", encoding="utf-8")
            evidence.write_text('{"status":"passed"}\n', encoding="utf-8")
            self.assertEqual(release_evidence.release_source_digest(root), before)
            (root / "src.cpp").write_text("two\n", encoding="utf-8")
            self.assertNotEqual(release_evidence.release_source_digest(root), before)

    def test_complete_live_reports_pass(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)
        provider, fixture = reports(source_digest)
        with tempfile.TemporaryDirectory() as directory:
            evidence_dir = Path(directory)
            (evidence_dir / "provider-regression.json").write_text(
                json.dumps(provider), encoding="utf-8"
            )
            (evidence_dir / "fixture-regression.json").write_text(
                json.dumps(fixture), encoding="utf-8"
            )
            self.assertEqual(
                release_evidence.validate_release_evidence(ROOT, "1.5.0", evidence_dir),
                evidence_dir,
            )

    def test_non_live_failed_or_stale_reports_fail(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)
        provider, fixture = reports(source_digest)
        for field, value, message in (
            ("mode", "inspect", "not a live result"),
            ("status", "failed", "did not pass"),
            ("source_sha256", "0" * 64, "source digest is stale"),
        ):
            invalid = copy.deepcopy(provider)
            invalid[field] = value
            with self.assertRaisesRegex(release_evidence.EvidenceError, message):
                release_evidence._validate_provider(invalid, ROOT, "1.5.0", source_digest)

        fixture["independent_verification"]["test"] = "failed"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "verification failed"):
            release_evidence._validate_fixture(fixture, ROOT, "1.5.0", source_digest)

    def test_raw_secret_and_incomplete_tool_evidence_fail(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)
        provider, fixture = reports(source_digest)
        provider["profiles"][0]["acceptance"]["raw_response"] = "private"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "unsupported fields"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        fixture["agent"]["secret"] = "private"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "unsupported fields"):
            release_evidence._validate_fixture(fixture, ROOT, "1.5.0", source_digest)

        provider, _ = reports(source_digest)
        provider["profiles"][0]["acceptance"]["checks"]["function_call"] = False
        with self.assertRaisesRegex(release_evidence.EvidenceError, "tool checks failed"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)


if __name__ == "__main__":
    unittest.main()
