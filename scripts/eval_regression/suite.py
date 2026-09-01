"""Aggregate offline scenario scores and expose the command-line interface."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Mapping, Sequence

from . import contracts
from .artifacts import collect_offline


def score_scenario(
    scenario: Mapping[str, object],
    agent: Mapping[str, object],
    tools: Sequence[str],
    result_path: Path,
    events_path: Path,
) -> dict[str, object]:
    oracle = contracts.object_value(scenario.get("oracle"), "scenario oracle")
    execution = contracts.object_value(agent.get("execution"), "agent execution")
    model = contracts.object_value(agent.get("model"), "agent model")
    changes = contracts.object_value(agent.get("changes"), "agent changes")
    checks = {
        "status": agent.get("status") == oracle.get("status"),
        "verification": agent.get("verification_status") == oracle.get("verification_status"),
        "changed_files": changes.get("files") == oracle.get("changed_files"),
        "required_tools": set(oracle.get("required_tools", [])) <= set(tools),
        "forbidden_tools": not set(oracle.get("forbidden_tools", [])) & set(tools),
        "turn_budget": agent.get("turns", 0) <= oracle.get("max_turns", 0),
        "tool_budget": execution.get("tool_calls", 0) <= oracle.get("max_tool_calls", 0),
        "tool_errors": execution.get("tool_errors", 0) <= oracle.get("max_tool_errors", 0),
    }
    metrics = {
        field: model.get(field)
        for field in (
            "prompt_tokens",
            "completion_tokens",
            "total_tokens",
            "cached_tokens",
            "cache_hit_rate",
        )
    }
    metrics.update(
        tool_calls=execution.get("tool_calls"),
        turns=agent.get("turns"),
        duration_ms=agent.get("duration_ms"),
        token_usage_coverage=(
            model["token_budget"].get("usage_coverage")
            if isinstance(model.get("token_budget"), Mapping)
            else "legacy_unknown"
        ),
    )
    return {
        "id": scenario.get("id"),
        "category": scenario.get("category"),
        "status": "passed" if all(checks.values()) else "failed",
        "checks": checks,
        "metrics": metrics,
        "result_sha256": contracts.sha256(result_path),
        "events_sha256": contracts.sha256(events_path),
    }


def rate(numerator: int, denominator: int) -> float | None:
    return numerator / denominator if denominator else None


def evaluate_offline(manifest: Path, artifacts: Path) -> dict[str, object]:
    suite, _description, scenarios = contracts.load_manifest(manifest)
    contracts.require(artifacts.is_dir(), f"artifact directory does not exist: {artifacts}")
    scores: list[dict[str, object]] = []
    total_fields = (
        "tool_calls",
        "turns",
        "prompt_tokens",
        "completion_tokens",
        "total_tokens",
        "cached_tokens",
        "duration_ms",
    )
    totals = {field: 0 for field in total_fields}
    token_coverage: dict[str, int] = {}
    verification_required = verification_passed = 0
    profile: dict[str, object] | None = None
    for scenario in scenarios:
        scenario_id = str(scenario["id"])
        result_path = artifacts / f"{scenario_id}.result.json"
        events_path = artifacts / f"{scenario_id}.events.jsonl"
        agent = contracts.load_result(result_path, scenario_id)
        model = contracts.object_value(agent["model"], f"result for {scenario_id} model")
        current_profile = {
            field: model[field] for field in sorted(contracts.MODEL_ID_FIELDS)
        }
        if profile is None:
            profile = current_profile
        else:
            contracts.require(
                current_profile == profile,
                "all evaluation artifacts must use the same provider, adapter, and model",
            )
        tools = contracts.load_events(events_path, scenario_id, agent)
        score = score_scenario(scenario, agent, tools, result_path, events_path)
        scores.append(score)
        scenario_metrics = contracts.object_value(score["metrics"], f"score for {scenario_id}")
        for field in totals:
            totals[field] += int(scenario_metrics[field])
        coverage = str(scenario_metrics["token_usage_coverage"])
        token_coverage[coverage] = token_coverage.get(coverage, 0) + 1
        oracle = contracts.object_value(scenario["oracle"], "scenario oracle")
        if oracle["verification_status"] != "not_required":
            verification_required += 1
            verification_passed += agent["verification_status"] == "passed"

    passed, count = sum(score["status"] == "passed" for score in scores), len(scores)
    metrics = {
        "task_success": {"passed": passed, "total": count, "rate": rate(passed, count)},
        "verification": {
            "passed": verification_passed,
            "required": verification_required,
            "rate": rate(verification_passed, verification_required),
        },
        "tool_calls": {
            "total": totals["tool_calls"],
            "mean": totals["tool_calls"] / count,
        },
        "turns": {"total": totals["turns"], "mean": totals["turns"] / count},
        "tokens": {
            "prompt": totals["prompt_tokens"],
            "completion": totals["completion_tokens"],
            "total": totals["total_tokens"],
            "cached": totals["cached_tokens"],
            "cache_hit_rate": rate(totals["cached_tokens"], totals["prompt_tokens"]),
            "usage_coverage": dict(sorted(token_coverage.items())),
        },
        "duration_ms": {
            "total": totals["duration_ms"],
            "mean": totals["duration_ms"] / count,
        },
    }
    return {
        "schema_version": 1,
        "operation": "eval_regression",
        "mode": "offline",
        "scope": "seed_contract_regression",
        "suite": suite,
        "profile": profile,
        "manifest_sha256": contracts.sha256(manifest),
        "evidence": {
            "manifest_schema": "validated",
            "artifact_replay": "scored",
            "live_model_evaluation": "not_run",
        },
        "status": "passed" if passed == count else "failed",
        "metrics": metrics,
        "scenarios": scores,
    }


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replay seed contract scenarios offline; this is not a model-quality benchmark."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    collect = commands.add_parser("collect", help="Sanitize one result and event trace.")
    collect.add_argument("--manifest", type=Path, default=contracts.DEFAULT_MANIFEST)
    collect.add_argument("--scenario", required=True)
    collect.add_argument("--result", type=Path, required=True)
    collect.add_argument("--events", type=Path, required=True)
    collect.add_argument("--artifacts", type=Path, required=True)
    collect.add_argument("--force", action="store_true")
    score = commands.add_parser("score", help="Score a complete offline artifact set.")
    score.add_argument("--manifest", type=Path, default=contracts.DEFAULT_MANIFEST)
    score.add_argument("--artifacts", type=Path, required=True)
    score.add_argument("--output", type=Path)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report = (
            collect_offline(
                args.manifest,
                args.scenario,
                args.result,
                args.events,
                args.artifacts,
                args.force,
            )
            if args.command == "collect"
            else evaluate_offline(args.manifest, args.artifacts)
        )
        content = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        if args.command == "score" and args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(content, encoding="utf-8")
        else:
            sys.stdout.write(content)
    except (contracts.EvalError, OSError) as error:
        print(f"eval-regression: {error}", file=sys.stderr)
        return 2
    return 0 if args.command == "collect" or report["status"] == "passed" else 1
