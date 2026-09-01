#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "eval-regression.py"
sys.path.insert(0, str(ROOT / "scripts"))

import eval_regression  # noqa: E402


def execution(tool_calls: int) -> dict:
    return {"tool_calls": tool_calls, "tool_errors": 0}


def model(prompt: int = 100, completion: int = 20, cached: int = 25) -> dict:
    return {
        "adapter": "chat_completions",
        "provider": "fixture",
        "model": "offline-fixture",
        "prompt_tokens": prompt,
        "completion_tokens": completion,
        "total_tokens": prompt + completion,
        "cached_tokens": cached,
        "cache_hit_rate": cached / prompt if prompt else None,
    }


def result_document(
    scenario_id: str,
    *,
    tools: list[str],
    turns: int = 2,
    changed_files: list[str] | None = None,
    verification: str = "not_required",
    prompt: int = 100,
    completion: int = 20,
    cached: int = 25,
    duration: int = 50,
) -> dict:
    changed = changed_files or []
    return {
        "schema_version": 1,
        "scenario_id": scenario_id,
        "agent": {
            "completed": True,
            "status": "completed",
            "stop_reason": None,
            "turns": turns,
            "duration_ms": duration,
            "verification_status": verification,
            "execution": execution(len(tools)),
            "model": model(prompt, completion, cached),
            "changes": {"files": changed},
        },
    }


def event(seq: int, event_type: str, data: dict) -> dict:
    return {
        "schema_version": 1,
        "seq": seq,
        "timestamp": f"2026-09-01T00:00:{seq:02d}.000Z",
        "type": event_type,
        "data": data,
    }


def event_trace(document: dict, tools: list[str]) -> list[dict]:
    agent = document["agent"]
    records = [event(1, "task_started", {"require_verification": bool(agent["changes"]["files"])})]
    sequence = 2
    for turn in range(1, agent["turns"] + 1):
        records.append(event(sequence, "model_completed", {"turn": turn}))
        sequence += 1
    for index, name in enumerate(tools, start=1):
        call_id = f"call-{index}"
        records.append(
            event(sequence, "tool_started", {"turn": 1, "tool_call_id": call_id, "name": name})
        )
        sequence += 1
        records.append(
            event(sequence, "tool_completed", {"turn": 1, "tool_call_id": call_id, "name": name})
        )
        sequence += 1
    records.append(
        event(
            sequence,
            "task_finished",
            {
                "status": agent["status"],
                "turns": agent["turns"],
                "duration_ms": agent["duration_ms"],
                "verification_status": agent["verification_status"],
                "tool_calls": agent["execution"]["tool_calls"],
            },
        )
    )
    return records


def manifest(scenarios: list[dict]) -> dict:
    return {
        "schema_version": 1,
        "suite": "test-suite",
        "description": "Small deterministic test suite.",
        "scenarios": scenarios,
    }


def scenario(
    scenario_id: str,
    *,
    required_tools: list[str],
    changed_files: list[str] | None = None,
    verification: str = "not_required",
) -> dict:
    return {
        "id": scenario_id,
        "category": "test",
        "task": "Perform the deterministic test task.",
        "workspace": ".",
        "oracle": {
            "status": "completed",
            "verification_status": verification,
            "changed_files": changed_files or [],
            "required_tools": required_tools,
            "forbidden_tools": ["run_command"] if "run_command" not in required_tools else [],
            "max_turns": 8,
            "max_tool_calls": 10,
            "max_tool_errors": 0,
        },
    }


class EvalRegressionTests(unittest.TestCase):
    def write_suite(
        self, directory: Path, definitions: list[tuple[dict, dict, list[str]]]
    ) -> tuple[Path, Path]:
        manifest_path = directory / "scenarios.json"
        artifact_dir = directory / "artifacts"
        artifact_dir.mkdir()
        manifest_path.write_text(
            json.dumps(manifest([definition[0] for definition in definitions])), encoding="utf-8"
        )
        for definition, result, tools in definitions:
            scenario_id = definition["id"]
            (artifact_dir / f"{scenario_id}.result.json").write_text(
                json.dumps(result), encoding="utf-8"
            )
            (artifact_dir / f"{scenario_id}.events.jsonl").write_text(
                "".join(json.dumps(item) + "\n" for item in event_trace(result, tools)),
                encoding="utf-8",
            )
        return manifest_path, artifact_dir

    def test_committed_manifest_is_a_nontrivial_strict_suite(self) -> None:
        suite, description, scenarios = eval_regression.load_manifest(
            ROOT / "evals" / "scenarios.json"
        )

        self.assertEqual(suite, "mint-core")
        self.assertTrue(description)
        self.assertGreaterEqual(len(scenarios), 6)
        self.assertIn("repair-with-report", {item["id"] for item in scenarios})
        self.assertTrue(
            any(item["oracle"]["verification_status"] == "passed" for item in scenarios)
        )

    def test_offline_report_aggregates_success_verification_and_efficiency(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inspect_tools = ["read_file"]
            repair_tools = ["read_file", "apply_patch", "run_recipe"]
            inspect = scenario("inspect", required_tools=inspect_tools)
            repair = scenario(
                "repair",
                required_tools=repair_tools,
                changed_files=["src/calculator.cpp"],
                verification="passed",
            )
            definitions = [
                (
                    inspect,
                    result_document(
                        "inspect", tools=inspect_tools, prompt=100, completion=20, cached=25
                    ),
                    inspect_tools,
                ),
                (
                    repair,
                    result_document(
                        "repair",
                        tools=repair_tools,
                        changed_files=["src/calculator.cpp"],
                        verification="passed",
                        prompt=300,
                        completion=40,
                        cached=75,
                        duration=150,
                    ),
                    repair_tools,
                ),
            ]
            manifest_path, artifacts = self.write_suite(root, definitions)

            report = eval_regression.evaluate_offline(manifest_path, artifacts)

        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["mode"], "offline")
        self.assertEqual(
            report["profile"],
            {
                "adapter": "chat_completions",
                "model": "offline-fixture",
                "provider": "fixture",
            },
        )
        self.assertEqual(report["scope"], "seed_contract_regression")
        self.assertEqual(report["evidence"]["live_model_evaluation"], "not_run")
        self.assertEqual(
            report["metrics"]["task_success"], {"passed": 2, "total": 2, "rate": 1.0}
        )
        self.assertEqual(
            report["metrics"]["verification"], {"passed": 1, "required": 1, "rate": 1.0}
        )
        self.assertEqual(report["metrics"]["tool_calls"], {"total": 4, "mean": 2.0})
        self.assertEqual(report["metrics"]["turns"], {"total": 4, "mean": 2.0})
        self.assertEqual(report["metrics"]["tokens"]["total"], 460)
        self.assertEqual(report["metrics"]["tokens"]["cache_hit_rate"], 0.25)
        self.assertEqual(report["metrics"]["tokens"]["usage_coverage"], {"legacy_unknown": 2})
        self.assertEqual(report["metrics"]["duration_ms"], {"total": 200, "mean": 100.0})
        self.assertRegex(report["manifest_sha256"], r"^[0-9a-f]{64}$")

    def test_offline_report_rejects_mixed_model_profiles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = result_document("first", tools=["read_file"])
            second = result_document("second", tools=["read_file"])
            second["agent"]["model"]["model"] = "different-model"
            manifest_path, artifacts = self.write_suite(
                root,
                [
                    (scenario("first", required_tools=["read_file"]), first, ["read_file"]),
                    (scenario("second", required_tools=["read_file"]), second, ["read_file"]),
                ],
            )

            with self.assertRaisesRegex(eval_regression.EvalError, "same provider"):
                eval_regression.evaluate_offline(manifest_path, artifacts)

    def test_valid_artifact_can_fail_the_oracle_without_becoming_schema_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = scenario("inspect", required_tools=["read_file"])
            tools = ["list_files"]
            result = result_document("inspect", tools=tools)
            manifest_path, artifacts = self.write_suite(root, [(definition, result, tools)])

            report = eval_regression.evaluate_offline(manifest_path, artifacts)

        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["scenarios"][0]["checks"]["required_tools"])
        self.assertEqual(report["metrics"]["task_success"]["rate"], 0.0)

    def test_unknown_tool_is_scored_as_a_failure_instead_of_dropping_the_run(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = scenario("inspect", required_tools=["read_file"])
            result = result_document("inspect", tools=["hallucinated_tool"])
            result["agent"]["execution"]["tool_errors"] = 1
            manifest_path, artifacts = self.write_suite(
                root, [(definition, result, ["hallucinated_tool"])]
            )

            report = eval_regression.evaluate_offline(manifest_path, artifacts)

        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["scenarios"][0]["checks"]["tool_errors"])

    def test_result_rejects_unknown_fields_and_inconsistent_cache_rate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "case.result.json"
            document = result_document("case", tools=["read_file"])
            document["agent"]["answer"] = "must never enter sanitized evidence"
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(eval_regression.EvalError, "unsupported fields: answer"):
                eval_regression.load_result(path, "case")

            document["agent"].pop("answer")
            document["agent"]["model"]["cache_hit_rate"] = 0.5
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(
                eval_regression.EvalError, "cache hit rate is inconsistent"
            ):
                eval_regression.load_result(path, "case")

    def test_result_accepts_legacy_no_budget_and_current_budget_summaries(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "case.result.json"
            document = result_document("case", tools=["read_file"])
            path.write_text(json.dumps(document), encoding="utf-8")
            self.assertNotIn("token_budget", eval_regression.load_result(path, "case")["model"])

            document["agent"]["model"]["token_budget"] = {
                "max_total_tokens": 100,
                "reported_total_tokens": 120,
                "usage_coverage": "complete",
                "enforcement": "reported_usage",
                "exhausted": True,
            }
            path.write_text(json.dumps(document), encoding="utf-8")
            current = eval_regression.load_result(path, "case")
            self.assertTrue(current["model"]["token_budget"]["exhausted"])

            document["agent"]["model"]["token_budget"]["reported_total_tokens"] = 119
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(eval_regression.EvalError, "budget total is inconsistent"):
                eval_regression.load_result(path, "case")

    def test_budget_exhausted_result_and_event_are_replayed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            document = result_document("budget", tools=[], turns=1, cached=25)
            agent = document["agent"]
            agent.update(
                completed=False,
                status="budget_exhausted",
                stop_reason="max_total_tokens_exhausted",
            )
            agent["model"]["token_budget"] = {
                "max_total_tokens": 100,
                "reported_total_tokens": 120,
                "usage_coverage": "complete",
                "enforcement": "reported_usage",
                "exhausted": True,
            }
            result_path = root / "budget.result.json"
            events_path = root / "budget.events.jsonl"
            result_path.write_text(json.dumps(document), encoding="utf-8")
            records = event_trace(document, [])
            records.insert(
                -1,
                event(
                    3,
                    "token_budget_exhausted",
                    {
                        "max_total_tokens": 100,
                        "reported_total_tokens": 120,
                        "model_calls": 1,
                        "usage_reports": 1,
                        "pending_tool_call_count": 0,
                    },
                ),
            )
            records[-1]["seq"] = 4
            records[-1]["timestamp"] = "2026-09-01T00:00:04.000Z"
            events_path.write_text(
                "".join(json.dumps(item) + "\n" for item in records), encoding="utf-8"
            )

            loaded = eval_regression.load_result(result_path, "budget")
            tools = eval_regression.load_events(events_path, "budget", loaded)

        self.assertEqual(tools, [])

    def test_result_rejects_noncanonical_changed_file_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "case.result.json"
            document = result_document(
                "case",
                tools=["read_file"],
                changed_files=["src/z.cpp", "src/a.cpp"],
                verification="not_run",
            )
            path.write_text(json.dumps(document), encoding="utf-8")

            with self.assertRaisesRegex(eval_regression.EvalError, "must be sorted"):
                eval_regression.load_result(path, "case")

    def test_events_require_contiguous_sequence_and_result_consistency(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = result_document("case", tools=["read_file"])
            result_path = root / "case.result.json"
            events_path = root / "case.events.jsonl"
            result_path.write_text(json.dumps(result), encoding="utf-8")
            records = event_trace(result, ["read_file"])
            records[1]["seq"] = 7
            events_path.write_text(
                "".join(json.dumps(item) + "\n" for item in records), encoding="utf-8"
            )
            agent = eval_regression.load_result(result_path, "case")

            with self.assertRaisesRegex(eval_regression.EvalError, "sequence is not contiguous"):
                eval_regression.load_events(events_path, "case", agent)

    def test_events_require_matched_tool_lifecycle(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = result_document("case", tools=["read_file"])
            result_path = root / "case.result.json"
            events_path = root / "case.events.jsonl"
            result_path.write_text(json.dumps(result), encoding="utf-8")
            records = event_trace(result, ["read_file"])
            records[4]["data"]["tool_call_id"] = "wrong-id"
            events_path.write_text(
                "".join(json.dumps(item) + "\n" for item in records), encoding="utf-8"
            )
            agent = eval_regression.load_result(result_path, "case")

            with self.assertRaisesRegex(eval_regression.EvalError, "no matching tool start"):
                eval_regression.load_events(events_path, "case", agent)

    def test_collector_strips_sensitive_payloads_and_requires_force_to_replace(self) -> None:
        marker = "SENSITIVE-MARKER"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = scenario("collect-case", required_tools=["read_file"])
            manifest_path = root / "scenarios.json"
            manifest_path.write_text(json.dumps(manifest([definition])), encoding="utf-8")
            wrapped = result_document("collect-case", tools=["read_file"])
            raw = dict(wrapped["agent"])
            raw["schema_version"] = 1
            raw["answer"] = marker
            raw["model"] = {**raw["model"], "last_response_id": marker}
            raw["changes"] = {
                "files": [],
                "details": [{"path": marker, "status": "modified"}],
                "unified_diff": marker,
                "diff_truncated": False,
            }
            raw_result = root / "raw-result.json"
            raw_result.write_text(json.dumps(raw), encoding="utf-8")
            records = event_trace(wrapped, ["read_file"])
            for record in records:
                data = record["data"]
                if record["type"] == "task_started":
                    data["workspace_root"] = marker
                elif record["type"] == "model_completed":
                    data["metadata"] = {"response_id": marker}
                    data["usage"] = {"prompt": marker}
                elif record["type"] == "tool_started":
                    data["arguments_summary"] = {"path": marker}
                elif record["type"] == "tool_completed":
                    data["result"] = {"error": marker}
                elif record["type"] == "task_finished":
                    data["model"] = {"last_response_id": marker}
            raw_events = root / "raw-events.jsonl"
            raw_events.write_text(
                "".join(json.dumps(item) + "\n" for item in records), encoding="utf-8"
            )
            artifacts = root / "artifacts"

            report = eval_regression.collect_offline(
                manifest_path,
                "collect-case",
                raw_result,
                raw_events,
                artifacts,
                False,
            )
            result_path = artifacts / "collect-case.result.json"
            events_path = artifacts / "collect-case.events.jsonl"
            combined = result_path.read_text(encoding="utf-8") + events_path.read_text(
                encoding="utf-8"
            )

            self.assertNotIn(marker, combined)
            self.assertNotIn("answer", combined)
            self.assertNotIn("arguments_summary", combined)
            self.assertNotIn("unified_diff", combined)
            self.assertFalse(report["overwritten"])
            with self.assertRaisesRegex(eval_regression.EvalError, "pass --force"):
                eval_regression.collect_offline(
                    manifest_path,
                    "collect-case",
                    raw_result,
                    raw_events,
                    artifacts,
                    False,
                )
            forced = eval_regression.collect_offline(
                manifest_path,
                "collect-case",
                raw_result,
                raw_events,
                artifacts,
                True,
            )
            self.assertTrue(forced["overwritten"])

    def test_collector_rejects_scenarios_outside_the_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest_path = root / "scenarios.json"
            manifest_path.write_text(
                json.dumps(manifest([scenario("declared", required_tools=["read_file"])])),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(eval_regression.EvalError, "not declared"):
                eval_regression.collect_offline(
                    manifest_path,
                    "undeclared",
                    root / "missing-result.json",
                    root / "missing-events.jsonl",
                    root / "artifacts",
                    False,
                )

    def test_collector_force_replaces_symlink_without_touching_its_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = scenario("collect-case", required_tools=["read_file"])
            manifest_path = root / "scenarios.json"
            manifest_path.write_text(json.dumps(manifest([definition])), encoding="utf-8")
            wrapped = result_document("collect-case", tools=["read_file"])
            raw = dict(wrapped["agent"])
            raw["schema_version"] = 1
            raw_result = root / "raw-result.json"
            raw_result.write_text(json.dumps(raw), encoding="utf-8")
            raw_events = root / "raw-events.jsonl"
            raw_events.write_text(
                "".join(json.dumps(item) + "\n" for item in event_trace(wrapped, ["read_file"])),
                encoding="utf-8",
            )
            artifacts = root / "artifacts"
            artifacts.mkdir()
            target = root / "must-not-change.txt"
            target.write_text("protected\n", encoding="utf-8")
            result_path = artifacts / "collect-case.result.json"
            try:
                result_path.symlink_to(target)
            except OSError as error:
                self.skipTest(f"symbolic links are unavailable: {error}")

            eval_regression.collect_offline(
                manifest_path,
                "collect-case",
                raw_result,
                raw_events,
                artifacts,
                True,
            )

            self.assertEqual(target.read_text(encoding="utf-8"), "protected\n")
            self.assertFalse(result_path.is_symlink())
            self.assertEqual(
                json.loads(result_path.read_text(encoding="utf-8"))["scenario_id"],
                "collect-case",
            )

    def test_cli_rejects_live_mode_without_any_execution_path(self) -> None:
        result = subprocess.run(
            [sys.executable, str(MODULE_PATH), "live"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid choice", result.stderr)
        self.assertNotIn("api", result.stdout.lower())


if __name__ == "__main__":
    unittest.main()
