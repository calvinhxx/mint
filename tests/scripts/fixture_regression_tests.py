#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "fixture-regression.py"
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("fixture_regression", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
fixture_regression = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = fixture_regression
SPEC.loader.exec_module(fixture_regression)


class FixtureRegressionTests(unittest.TestCase):
    def test_committed_fixture_has_the_expected_safety_boundary(self) -> None:
        fixture = ROOT / "tests" / "fixtures" / "v1_broken_project"

        inventory = fixture_regression.validate_fixture(fixture)

        self.assertEqual(len(inventory), 6)
        self.assertRegex(fixture_regression.inventory_digest(inventory), r"^[0-9a-f]{64}$")

    def test_source_inventory_ignores_build_outputs_and_detects_only_allowed_changes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "build").mkdir()
            (root / "src" / "calculator.cpp").write_text("before\n", encoding="utf-8")
            before = fixture_regression.source_inventory(root)
            (root / "build" / "cache.txt").write_text("generated\n", encoding="utf-8")
            (root / "src" / "calculator.cpp").write_text("after\n", encoding="utf-8")
            (root / "FIX_REPORT.md").write_text("fixed\n", encoding="utf-8")

            changed = fixture_regression.changed_sources(before, root)

            self.assertEqual(changed, ["FIX_REPORT.md", "src/calculator.cpp"])

    def test_safe_agent_report_drops_answer_diff_response_id_and_events(self) -> None:
        raw = {
            "completed": True,
            "status": "completed",
            "answer": "secret answer",
            "turns": 4,
            "duration_ms": 50,
            "verification_status": "passed",
            "execution": {"tool_calls": 3, "debug": "secret execution"},
            "model": {
                "calls": 4,
                "provider": "openai",
                "adapter": "responses",
                "model": "gpt-test",
                "last_response_id": "secret response",
            },
            "changes": {"files": ["wrong"], "unified_diff": "secret diff"},
            "events": "secret events",
        }

        safe = fixture_regression.safe_agent_report(
            raw, ["FIX_REPORT.md", "src/calculator.cpp"]
        )
        serialized = json.dumps(safe)

        self.assertNotIn("secret", serialized)
        self.assertEqual(safe["changes"]["files"], ["FIX_REPORT.md", "src/calculator.cpp"])

    def test_evidence_file_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "evidence.json"
            fixture_regression.write_report(output, {"status": "passed"})

            with self.assertRaisesRegex(
                fixture_regression.FixtureRegressionError, "refusing to replace"
            ):
                fixture_regression.write_report(output, {"status": "failed"})

            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "passed")

    def test_live_mode_stops_before_model_work_when_the_key_is_missing(self) -> None:
        fixture = ROOT / "tests" / "fixtures" / "v1_broken_project"
        config = ROOT / "configs" / "providers" / "openai-responses.json"
        inspection = {
            "operation": "inspect",
            "provider": "openai",
            "adapter": "responses",
            "endpoint": "https://api.openai.com/v1/responses",
            "model": "test-model",
            "stream": True,
            "authentication": "environment",
            "api_key_env": "OPENAI_API_KEY",
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "blocked.json"
            args = Namespace(
                mint=Path("mint"),
                config=config,
                fixture=fixture,
                live=True,
                output=output,
            )
            environment = dict(os.environ)
            environment.pop("OPENAI_API_KEY", None)
            with (
                mock.patch.object(fixture_regression, "inspect_provider", return_value=inspection),
                mock.patch.object(fixture_regression, "mint_version", return_value="1.5.0"),
                mock.patch.object(fixture_regression, "run_baseline") as baseline,
                mock.patch.dict(os.environ, environment, clear=True),
            ):
                exit_code = fixture_regression.execute(args)

            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(exit_code, 2)
            self.assertEqual(report["status"], "blocked")
            self.assertEqual(report["missing_environment"], ["OPENAI_API_KEY"])
            baseline.assert_not_called()

    def test_live_orchestration_repairs_a_copy_and_independently_verifies_it(self) -> None:
        fixture = ROOT / "tests" / "fixtures" / "v1_broken_project"
        config = ROOT / "configs" / "providers" / "openai-responses.json"
        inspection = {
            "operation": "inspect",
            "provider": "openai",
            "adapter": "responses",
            "endpoint": "https://api.openai.com/v1/responses",
            "model": "test-model",
            "stream": True,
            "authentication": "environment",
            "api_key_env": "OPENAI_API_KEY",
        }

        def repair_copy(_executable: Path, _config: Path, project: Path, _runtime: Path):
            implementation = project / "src" / "calculator.cpp"
            implementation.write_text(
                implementation.read_text(encoding="utf-8").replace(
                    "return left - right;", "return left + right;"
                ),
                encoding="utf-8",
            )
            (project / "FIX_REPORT.md").write_text("Fixed and verified.\n", encoding="utf-8")
            return 0, {
                "completed": True,
                "status": "completed",
                "turns": 4,
                "duration_ms": 50,
                "verification_status": "passed",
                "execution": {
                    "tool_calls": 4,
                    "successful_tool_calls": 4,
                    "tool_errors": 0,
                    "file_changes": 2,
                    "command_calls": 2,
                    "recipe_calls": 2,
                    "verification_commands": 1,
                    "commands_passed": 2,
                    "commands_failed": 0,
                    "commands_timed_out": 0,
                    "commands_cancelled": 0,
                    "commands_denied": 0,
                    "last_command_outcome": "passed",
                    "last_command_verification_eligible": True,
                },
                "model": {
                    "calls": 4,
                    "attempts": 4,
                    "retries": 0,
                    "usage_reports": 4,
                    "prompt_tokens": 100,
                    "completion_tokens": 20,
                    "total_tokens": 120,
                    "cached_tokens": 0,
                    "streamed_calls": 4,
                    "stream_events": 12,
                    "streamed_bytes": 100,
                    "duration_ms": 40,
                    "provider": "openai",
                    "adapter": "responses",
                    "model": "test-model",
                },
                "changes": {
                    "files": ["FIX_REPORT.md", "src/calculator.cpp"],
                    "unified_diff": "must not be retained",
                },
                "answer": "must not be retained",
            }

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "passed.json"
            args = Namespace(
                mint=Path("mint"),
                config=config,
                fixture=fixture,
                live=True,
                output=output,
            )
            with (
                mock.patch.object(fixture_regression, "inspect_provider", return_value=inspection),
                mock.patch.object(fixture_regression, "mint_version", return_value="1.5.0"),
                mock.patch.object(fixture_regression, "run_agent", side_effect=repair_copy),
                mock.patch.dict(os.environ, {"OPENAI_API_KEY": "set"}, clear=False),
            ):
                exit_code = fixture_regression.execute(args)

            report = json.loads(output.read_text(encoding="utf-8"))
            serialized = json.dumps(report)
            self.assertEqual(exit_code, 0)
            self.assertEqual(report["status"], "passed")
            self.assertRegex(report["source_sha256"], r"^[0-9a-f]{64}$")
            self.assertEqual(report["baseline"]["test"], "failed_as_expected")
            self.assertEqual(report["independent_verification"]["test"], "passed")
            self.assertNotIn("must not be retained", serialized)


if __name__ == "__main__":
    unittest.main()
