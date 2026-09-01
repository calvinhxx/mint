#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from argparse import Namespace
from contextlib import ExitStack
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

import release_evidence  # noqa: E402

GROQ_CONFIG = ROOT / "configs" / "providers" / "groq-chat.json"
FIXTURE = ROOT / "tests" / "fixtures" / "v1_broken_project"
BASELINE = {
    "configure": "passed",
    "build": "passed",
    "test": "failed_as_expected",
}
VERIFICATION = {"configure": "passed", "build": "passed", "test": "passed"}
GROQ_INSPECTION = {
    "operation": "inspect",
    "provider": "groq",
    "adapter": "chat_completions",
    "endpoint": "https://api.groq.com/openai/v1/chat/completions",
    "model": "test-model",
    "stream": False,
    "authentication": "environment",
    "api_key_env": "GROQ_API_KEY",
    "limits": {
        "max_request_tokens": 8000,
        "max_request_tokens_source": "config",
        "response_header_max_request_tokens": None,
        "request_token_safety_margin": 256,
        "request_token_estimate_bytes_per_token": 2,
        "max_completion_tokens": 1024,
        "max_attempts_per_request": 3,
    },
}


def repair_project(project: Path) -> None:
    implementation = project / "src" / "calculator.cpp"
    implementation.write_text(
        implementation.read_text(encoding="utf-8").replace(
            "return left - right;", "return left + right;"
        ),
        encoding="utf-8",
    )
    (project / "FIX_REPORT.md").write_text("Fixed and verified.\n", encoding="utf-8")


def completed_agent_result() -> dict:
    return {
        "completed": True,
        "status": "completed",
        "stop_reason": None,
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
            "last_file_change_call": 2,
            "last_command_call": 4,
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
            "cache_hit_rate": 0.0,
            "streamed_calls": 0,
            "stream_events": 0,
            "streamed_bytes": 0,
            "duration_ms": 40,
            "provider": "groq",
            "adapter": "chat_completions",
            "model": "test-model",
            "max_request_tokens": 7000,
            "max_request_tokens_source": "response_header",
            "response_header_max_request_tokens": 7000,
            "request_token_estimate_bytes_per_token": 2,
        },
        "changes": {
            "files": ["FIX_REPORT.md", "src/calculator.cpp"],
            "unified_diff": "must not be retained",
        },
        "answer": "must not be retained",
    }


def write_model_events(path: Path, events: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(
            json.dumps(
                {
                    "schema_version": 1,
                    "seq": sequence,
                    "timestamp": "2026-08-31T00:00:00.000Z",
                    "type": "model_progress",
                    "data": event,
                }
            )
            + "\n"
            for sequence, event in enumerate(events, start=1)
        ),
        encoding="utf-8",
    )


def execute_live(agent, *, mock_baseline: bool = False, verification: dict | None = None):
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "result.json"
        args = Namespace(
            mint=Path("mint"), config=GROQ_CONFIG, fixture=FIXTURE, live=True, output=output
        )
        patches = [
            mock.patch.object(
                fixture_regression, "inspect_provider", return_value=GROQ_INSPECTION
            ),
            mock.patch.object(fixture_regression, "mint_version", return_value="1.5.0"),
            mock.patch.object(fixture_regression, "run_agent", side_effect=agent),
            mock.patch.object(
                fixture_regression,
                "command_sandbox_preflight",
                return_value={"status": "passed", "backend": "macos-seatbelt"},
            ),
            mock.patch.dict(os.environ, {"GROQ_API_KEY": "set"}, clear=False),
        ]
        if mock_baseline:
            patches.append(
                mock.patch.object(fixture_regression, "run_baseline", return_value=BASELINE)
            )
        if verification is not None:
            patches.append(
                mock.patch.object(
                    fixture_regression, "independent_verification", return_value=verification
                )
            )
        with ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)
            exit_code = fixture_regression.execute(args)
        return exit_code, json.loads(output.read_text(encoding="utf-8"))


class FixtureRegressionTests(unittest.TestCase):
    def setUp(self) -> None:
        release_tree = mock.patch.object(fixture_regression, "require_release_source_tree")
        release_tree.start()
        self.addCleanup(release_tree.stop)

    def test_committed_fixture_has_the_expected_safety_boundary(self) -> None:
        inventory = fixture_regression.validate_fixture(FIXTURE)

        self.assertEqual(len(inventory), 6)
        self.assertRegex(fixture_regression.inventory_digest(inventory), r"^[0-9a-f]{64}$")
        self.assertEqual(
            fixture_regression.fixture_limits(GROQ_INSPECTION),
            {
                "max_turns": 16,
                "max_context_bytes": 24576,
                "max_seconds": 600,
                "max_context_estimated_tokens": 12288,
                "max_request_tokens": 8000,
                "max_request_tokens_source": "config",
                "response_header_max_request_tokens": None,
                "request_token_safety_margin": 256,
                "request_token_estimate_bytes_per_token": 2,
                "max_completion_tokens_per_request": 1024,
                "max_attempts_per_request": 3,
            },
        )
        self.assertIn("apply_patch on src/calculator.cpp", fixture_regression.TASK)
        self.assertIn("apply_patch on FIX_REPORT.md", fixture_regression.TASK)
        self.assertIn("configure, build, and test recipes", fixture_regression.TASK)
        self.assertIn("do not repeat it", fixture_regression.TASK)
        self.assertNotIn("apply_changeset", fixture_regression.TASK)

    def test_runtime_limits_allow_providers_without_a_limit_header(self) -> None:
        agent = {
            "model": {
                "max_request_tokens": 8000,
                "max_request_tokens_source": "automatic",
                "response_header_max_request_tokens": None,
                "request_token_estimate_bytes_per_token": 2,
            }
        }

        self.assertEqual(
            fixture_regression.effective_agent_limits(agent),
            {
                "max_request_tokens": 8000,
                "max_request_tokens_source": "automatic",
                "response_header_max_request_tokens": None,
                "request_token_estimate_bytes_per_token": 2,
            },
        )
        agent["model"]["max_request_tokens_source"] = "response_header"
        with self.assertRaisesRegex(
            fixture_regression.FixtureRegressionError, "invalid effective request limits"
        ):
            fixture_regression.effective_agent_limits(agent)

    def test_macos_sandbox_preflight_fails_before_provider_work(self) -> None:
        failure = fixture_regression.CommandResult(
            71, "", "sandbox_apply: Operation not permitted"
        )
        with (
            mock.patch.object(fixture_regression.sys, "platform", "darwin"),
            mock.patch.object(fixture_regression, "run_command", return_value=failure),
            self.assertRaises(fixture_regression.FixtureRegressionError) as raised,
        ):
            fixture_regression.command_sandbox_preflight()

        self.assertEqual(raised.exception.stage, "command_sandbox_preflight")
        self.assertEqual(
            raised.exception.details,
            {"process_status": "failed", "exit_code": 71},
        )

    def test_non_macos_sandbox_preflight_is_not_claimed(self) -> None:
        with mock.patch.object(fixture_regression.sys, "platform", "linux"):
            self.assertIsNone(fixture_regression.command_sandbox_preflight())

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
            fixture_regression.validate_source_boundary(changed, root)

    def test_source_boundary_failure_retains_actual_changes_and_safe_agent_summary(self) -> None:
        def incomplete_repair(_executable: Path, _config: Path, project: Path, _runtime: Path):
            implementation = project / "src" / "calculator.cpp"
            implementation.write_text(
                implementation.read_text(encoding="utf-8").replace(
                    "return left - right;", "return left + right;"
                ),
                encoding="utf-8",
            )
            return 17, completed_agent_result()

        exit_code, report = execute_live(incomplete_repair, mock_baseline=True)
        self.assertEqual(exit_code, 1)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["failed_stage"], "source_boundary")
        self.assertEqual(report["agent"]["status"], "completed")
        self.assertEqual(report["agent"]["turns"], 4)
        self.assertEqual(report["agent"]["changes"]["files"], ["src/calculator.cpp"])
        self.assertEqual(report["agent_process"], {"status": "failed", "exit_code": 17})
        self.assertEqual(
            report["failure"]["source_boundary"],
            {
                "expected_files": ["FIX_REPORT.md", "src/calculator.cpp"],
                "observed_files": ["src/calculator.cpp"],
                "missing_files": ["FIX_REPORT.md"],
                "unexpected_files": [],
                "report_status": "missing",
            },
        )
        self.assertEqual(
            report["independent_verification"],
            {"status": "not_run", "reason": "source_boundary"},
        )
        self.assertNotIn("must not be retained", json.dumps(report))

    def test_safe_agent_report_drops_answer_diff_response_id_and_events(self) -> None:
        raw = {
            "completed": True,
            "status": "completed",
            "answer": "secret answer",
            "error": "secret provider response",
            "task_id": "secret task",
            "task_directory": "/secret/task",
            "diagnostic_log": "/secret/diagnostic.log",
            "turns": 4,
            "duration_ms": 50,
            "verification_status": "passed",
            "execution": {
                "tool_calls": 3,
                "last_file_change_call": 2,
                "last_command_call": 3,
                "last_command_outcome": "passed",
                "last_command_verification_eligible": True,
                "debug": "secret execution",
            },
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
        self.assertIsNone(safe["stop_reason"])
        self.assertEqual(safe["execution"]["last_file_change_call"], 2)
        self.assertEqual(safe["execution"]["last_command_call"], 3)
        self.assertEqual(safe["execution"]["last_command_outcome"], "passed")
        self.assertTrue(safe["execution"]["last_command_verification_eligible"])

    def test_complete_safe_agent_report_matches_release_schema(self) -> None:
        safe = fixture_regression.safe_agent_report(
            completed_agent_result(), ["FIX_REPORT.md", "src/calculator.cpp"]
        )

        self.assertEqual(set(safe), release_evidence.AGENT_FIELDS)
        self.assertEqual(set(safe["execution"]), release_evidence.EXECUTION_FIELDS)
        self.assertEqual(
            set(safe["model"]),
            release_evidence.MODEL_FIELDS | release_evidence.OPTIONAL_MODEL_FIELDS,
        )

    def test_workspace_integrity_failure_retains_only_safe_diagnostics(self) -> None:
        raw = {
            "completed": False,
            "status": "failed",
            "stop_reason": "workspace_integrity_failed",
            "error": "private failure body",
            "changes": {
                "details": [
                    {"path": "CMakeLists.txt", "status": "policy_violation"},
                    {"path": "<workspace>", "status": "unauditable"},
                    {"path": "../private", "status": "policy_violation"},
                    {"path": "src/calculator.cpp", "status": "modified"},
                    {"path": "/private/secret", "status": "policy_violation"},
                ],
                "unified_diff": "private diff",
            },
        }

        safe_report = fixture_regression.safe_agent_report(raw, ["src/calculator.cpp"])
        safe_failure = fixture_regression.safe_agent_failure(raw, Path("missing"), 3)

        self.assertEqual(safe_report["stop_reason"], "workspace_integrity_failed")
        self.assertEqual(
            safe_failure,
            {
                "category": "workspace_integrity_failed",
                "workspace_risks": [
                    {"path": "CMakeLists.txt", "status": "policy_violation"},
                    {"path": "<workspace>", "status": "unauditable"},
                ],
            },
        )
        serialized = json.dumps({"agent": safe_report, "failure": safe_failure})
        self.assertNotIn("private", serialized)
        self.assertNotIn("secret", serialized)

    def test_workspace_risk_paths_are_checked_with_cross_platform_rules(self) -> None:
        def retained_paths(*paths: str) -> list[str]:
            document = {
                "changes": {
                    "details": [
                        {"path": path, "status": "policy_violation"} for path in paths
                    ]
                }
            }
            return [risk["path"] for risk in fixture_regression.safe_workspace_risks(document)]

        safe_paths = ("docs/guide.md", r"src\calculator.cpp", "<workspace>")
        self.assertEqual(retained_paths(*safe_paths), list(safe_paths))

        unsafe_paths = (
            "/private/posix",
            r"C:\private\drive",
            "D:/private/drive",
            r"\\server\share\private",
            "//server/share/private",
            r"\private\rooted",
            "../private/traversal",
            r"..\private\traversal",
        )
        self.assertEqual(retained_paths(*unsafe_paths), [])

    def test_json_error_retains_only_safe_model_failure(self) -> None:
        def failed_after_repair(
            _executable: Path, _config: Path, project: Path, runtime: Path
        ) -> tuple[int, dict]:
            repair_project(project)
            write_model_events(
                runtime / "events.jsonl",
                [
                    {
                        "kind": "request_failed",
                        "attempt": 2,
                        "max_attempts": 3,
                        "http_status": 413,
                        "api_key": "sk-event-secret",
                        "response_body": "private event response",
                    },
                ],
            )
            with (runtime / "events.jsonl").open("ab") as events:
                events.write(b"{broken json}\n\xff\n")
            return 1, {
                "status": "error",
                "completed": False,
                "error": "HTTP 413: Bearer sk-json-secret; private response body",
                "task_directory": "/private/task",
            }

        exit_code, report = execute_live(
            failed_after_repair, mock_baseline=True, verification=VERIFICATION
        )
        self.assertEqual(exit_code, 1)
        self.assertEqual(report["failed_stage"], "agent")
        self.assertEqual(
            report["agent_process"],
            {
                "status": "failed",
                "exit_code": 1,
                "failure": {
                    "category": "model_request_failed",
                    "http_status": 413,
                    "attempts": 2,
                    "retries": 1,
                    "max_attempts": 3,
                },
            },
        )
        self.assertEqual(report["agent"]["status"], "error")
        self.assertEqual(report["independent_verification"], VERIFICATION)
        serialized = json.dumps(report)
        for private in ("sk-", "private"):
            self.assertNotIn(private, serialized)

    def test_json_error_without_terminal_model_failure_is_generic(self) -> None:
        error_document = {"status": "error", "completed": False, "error": "private"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                fixture_regression.safe_agent_failure(error_document, root / "missing.jsonl", 3),
                {"category": "agent_error"},
            )
            stale = root / "stale.jsonl"
            write_model_events(
                stale,
                [
                    {
                        "kind": "request_failed",
                        "attempt": 1,
                        "max_attempts": 3,
                        "http_status": 429,
                    },
                    {
                        "kind": "request_succeeded",
                        "attempt": 1,
                        "max_attempts": 3,
                        "http_status": 200,
                    },
                ],
            )
            self.assertEqual(
                fixture_regression.safe_agent_failure(error_document, stale, 3),
                {"category": "agent_error"},
            )
            self.assertIsNone(
                fixture_regression.safe_agent_failure({"status": "timed_out"}, stale, 3)
            )

    def test_model_failure_rejects_untrusted_progress_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            events = Path(directory) / "events.jsonl"
            write_model_events(
                events,
                [
                    {
                        "kind": "request_failed",
                        "attempt": True,
                        "max_attempts": 99,
                        "http_status": 600,
                        "api_key": "sk-progress-secret",
                        "response_body": "private response body",
                    }
                ],
            )

            safe = fixture_regression.safe_agent_failure(
                {"status": "error", "completed": False, "error": "private error"},
                events,
                3,
            )

            self.assertEqual(safe, {"category": "model_request_failed"})
            serialized = json.dumps(safe)
            self.assertNotIn("sk-progress-secret", serialized)
            self.assertNotIn("private", serialized)

    def test_run_agent_invalid_json_does_not_retain_process_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory) / "runtime"
            result = fixture_regression.CommandResult(
                7,
                "Bearer sk-stdout-secret; private response",
                "private stderr",
            )
            with (
                mock.patch.object(fixture_regression, "run_command", return_value=result),
                self.assertRaises(fixture_regression.FixtureRegressionError) as raised,
            ):
                fixture_regression.run_agent(
                    Path("mint"), GROQ_CONFIG, Path(directory) / "project", runtime
                )

            self.assertEqual(raised.exception.stage, "agent")
            self.assertEqual(
                raised.exception.details,
                {"process_status": "invalid_json", "exit_code": 7},
            )
            self.assertNotIn("sk-stdout-secret", str(raised.exception))
            self.assertNotIn("private", str(raised.exception))

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
            "limits": {
                "max_request_tokens": 8000,
                "max_request_tokens_source": "automatic",
                "response_header_max_request_tokens": None,
                "request_token_safety_margin": 256,
                "request_token_estimate_bytes_per_token": 2,
                "max_completion_tokens": 2048,
                "max_attempts_per_request": 3,
            },
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
        def repair_copy(_executable: Path, _config: Path, project: Path, _runtime: Path):
            repair_project(project)
            return 0, completed_agent_result()

        exit_code, report = execute_live(repair_copy)
        self.assertEqual(exit_code, 0)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["profile_id"], "groq-chat")
        self.assertRegex(report["source_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(report["baseline"]["test"], "failed_as_expected")
        self.assertEqual(report["independent_verification"]["test"], "passed")
        self.assertEqual(report["limits"]["max_request_tokens"], 7000)
        self.assertEqual(report["limits"]["max_request_tokens_source"], "response_header")
        self.assertEqual(report["limits"]["response_header_max_request_tokens"], 7000)
        self.assertNotIn("must not be retained", json.dumps(report))

    def test_failed_agent_still_records_independent_verification(self) -> None:
        def timed_out_agent(_executable: Path, _config: Path, project: Path, _runtime: Path):
            repair_project(project)
            result = completed_agent_result()
            result.update(
                completed=False,
                status="timed_out",
                turns=12,
                duration_ms=600_000,
            )
            return 124, result

        exit_code, report = execute_live(timed_out_agent)
        self.assertEqual(exit_code, 1)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["failed_stage"], "agent")
        self.assertEqual(report["failure"]["process_status"], "failed")
        self.assertEqual(report["failure"]["exit_code"], 124)
        self.assertEqual(report["independent_verification"]["test"], "passed")

    def test_failure_details_cannot_replace_report_fields(self) -> None:
        error = fixture_regression.FixtureRegressionError(
            "agent",
            "private error",
            {
                "status": "passed",
                "agent": {"answer": "private"},
                "source_sha256": "0" * 64,
                "exit_code": 9,
                "process_status": "failed",
            },
        )

        self.assertEqual(
            fixture_regression.safe_failure_details(error),
            {"stage": "agent", "process_status": "failed", "exit_code": 9},
        )

    def test_invalid_utf8_report_is_a_structured_source_boundary_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "FIX_REPORT.md").write_bytes(b"\xff")

            with self.assertRaises(fixture_regression.FixtureRegressionError) as raised:
                fixture_regression.validate_source_boundary(
                    ["FIX_REPORT.md", "src/calculator.cpp"], project
                )

            safe = fixture_regression.safe_failure_details(raised.exception)
            self.assertEqual(safe["source_boundary"]["report_status"], "invalid_utf8")


if __name__ == "__main__":
    unittest.main()
