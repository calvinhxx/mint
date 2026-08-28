#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "provider-regression.py"
SPEC = importlib.util.spec_from_file_location("provider_regression", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
provider_regression = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = provider_regression
SPEC.loader.exec_module(provider_regression)


class ProviderRegressionTests(unittest.TestCase):
    def test_committed_manifest_selects_the_three_official_profiles(self) -> None:
        manifest = ROOT / "configs" / "provider-regression.json"
        profiles = provider_regression.load_manifest(manifest)

        self.assertEqual(
            [profile.id for profile in profiles],
            ["openai-responses", "groq-chat", "deepseek-chat"],
        )
        self.assertTrue(all(profile.config.is_file() for profile in profiles))
        self.assertRegex(provider_regression.matrix_digest(manifest, profiles), r"^[0-9a-f]{64}$")

    def test_manifest_rejects_paths_outside_its_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_directory = root / "manifest"
            manifest_directory.mkdir()
            outside = root / "outside-provider.json"
            outside.write_text("{}", encoding="utf-8")
            manifest = manifest_directory / "matrix.json"
            manifest.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "profiles": [{"id": "escape", "config": "../outside-provider.json"}],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                provider_regression.RegressionError, "escapes manifest directory"
            ):
                provider_regression.load_manifest(manifest)

    def test_safe_report_keeps_metrics_but_drops_raw_and_secret_fields(self) -> None:
        report = {
            "provider": "groq",
            "adapter": "chat_completions",
            "api_key_env": "GROQ_API_KEY",
            "config": "/private/workspace/config.json",
            "api_key": "secret",
            "error": "raw provider response",
            "acceptance": {
                "requests": 2,
                "raw_response": "nested raw provider response",
                "checks": {"function_call": True, "debug_prompt": "nested secret"},
            },
        }

        sanitized = provider_regression.safe_report("groq-chat", report, "passed")

        self.assertEqual(sanitized["acceptance"]["requests"], 2)
        self.assertNotIn("config", sanitized)
        self.assertNotIn("api_key", sanitized)
        self.assertNotIn("error", sanitized)
        self.assertNotIn("secret", json.dumps(sanitized))
        self.assertNotIn("raw provider response", json.dumps(sanitized))
        self.assertNotIn("nested raw provider response", json.dumps(sanitized))

    def test_missing_credentials_is_complete_and_deterministic(self) -> None:
        reports = [
            {"api_key_env": "GROQ_API_KEY"},
            {"api_key_env": "OPENAI_API_KEY"},
            {"api_key_env": "DEEPSEEK_API_KEY"},
        ]

        self.assertEqual(
            provider_regression.missing_credentials(reports, {"GROQ_API_KEY": "set"}),
            ["DEEPSEEK_API_KEY", "OPENAI_API_KEY"],
        )

    def test_evidence_file_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "evidence.json"
            provider_regression.write_report(output, {"status": "passed"})

            with self.assertRaisesRegex(provider_regression.RegressionError, "refusing to replace"):
                provider_regression.write_report(output, {"status": "failed"})

            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "passed")


if __name__ == "__main__":
    unittest.main()
