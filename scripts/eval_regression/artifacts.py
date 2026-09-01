"""Sanitize existing mint results into deterministic offline artifacts."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path

from . import contracts


def sanitize_result(path: Path, scenario_id: str) -> dict[str, object]:
    raw = contracts.load_json(path, f"raw result for {scenario_id}")
    contracts.require(
        raw.get("schema_version") == 1,
        f"raw result for {scenario_id} has unsupported schema",
    )
    agent: dict[str, object] = {}
    for field in contracts.AGENT_FIELDS - {"execution", "model", "changes"}:
        contracts.require(field in raw, f"raw result for {scenario_id} is missing {field}")
        agent[field] = raw[field]

    raw_execution = contracts.object_value(
        raw.get("execution"), f"raw result for {scenario_id} execution"
    )
    contracts.require(
        contracts.EXECUTION_FIELDS <= set(raw_execution),
        f"raw result for {scenario_id} execution is incomplete",
    )
    agent["execution"] = {
        field: raw_execution[field] for field in contracts.EXECUTION_FIELDS
    }

    raw_model = contracts.object_value(
        raw.get("model"), f"raw result for {scenario_id} model"
    )
    required_model = contracts.MODEL_FIELDS - {"token_budget"}
    contracts.require(
        required_model <= set(raw_model),
        f"raw result for {scenario_id} model is incomplete",
    )
    agent["model"] = {
        field: raw_model[field] for field in contracts.MODEL_FIELDS if field in raw_model
    }

    raw_changes = contracts.object_value(
        raw.get("changes"), f"raw result for {scenario_id} changes"
    )
    changed_files = contracts.string_list(
        raw_changes.get("files"),
        f"raw result for {scenario_id} changed files",
        paths=True,
    )
    agent["changes"] = {"files": sorted(changed_files)}
    return {"schema_version": 1, "scenario_id": scenario_id, "agent": agent}


def sanitize_events(path: Path, scenario_id: str) -> list[dict[str, object]]:
    try:
        contracts.require(
            path.stat().st_size <= contracts.MAX_EVENT_BYTES,
            f"raw events for {scenario_id} are too large",
        )
        lines = path.read_bytes().splitlines()
    except OSError as error:
        raise contracts.EvalError(f"cannot read raw events for {scenario_id}: {path}") from error
    contracts.require(
        0 < len(lines) <= contracts.MAX_EVENTS,
        f"raw events for {scenario_id} have invalid record count",
    )
    fields_by_type = {
        "model_completed": ("turn",),
        "tool_started": ("turn", "tool_call_id", "name"),
        "tool_completed": ("turn", "tool_call_id", "name"),
        "task_finished": (
            "status",
            "turns",
            "duration_ms",
            "verification_status",
            "tool_calls",
        ),
        "token_budget_exhausted": (
            "max_total_tokens",
            "reported_total_tokens",
            "model_calls",
            "usage_reports",
            "pending_tool_call_count",
        ),
    }
    safe: list[dict[str, object]] = []
    for sequence, raw_line in enumerate(lines, start=1):
        contracts.require(
            len(raw_line) <= contracts.MAX_EVENT_LINE_BYTES,
            f"raw event {sequence} for {scenario_id} is too large",
        )
        try:
            event = contracts.object_value(
                json.loads(raw_line), f"raw event {sequence} for {scenario_id}"
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise contracts.EvalError(
                f"raw event {sequence} for {scenario_id} is invalid JSON"
            ) from error
        label = f"raw event {sequence} for {scenario_id}"
        contracts.exact_fields(
            event, {"schema_version", "seq", "timestamp", "type", "data"}, label
        )
        contracts.require(
            event.get("schema_version") == 1 and event.get("seq") == sequence,
            f"{label} sequence is not contiguous",
        )
        timestamp = contracts.text(event.get("timestamp"), f"{label} timestamp", 32)
        contracts.require(
            contracts.TIMESTAMP.fullmatch(timestamp) is not None,
            f"{label} timestamp is invalid",
        )
        event_type = contracts.enum(event.get("type"), contracts.EVENT_TYPES, f"{label} type")
        data = contracts.object_value(event.get("data"), f"{label} data")
        retained = fields_by_type.get(event_type, ())
        contracts.require(set(retained) <= set(data), f"{label} is missing required data")
        safe.append(
            {
                "schema_version": 1,
                "seq": sequence,
                "timestamp": timestamp,
                "type": event_type,
                "data": {field: data[field] for field in retained},
            }
        )
    return safe


def collect_offline(
    manifest: Path,
    scenario_id: str,
    raw_result: Path,
    raw_events: Path,
    artifacts: Path,
    force: bool,
) -> dict[str, object]:
    suite, _description, scenarios = contracts.load_manifest(manifest)
    contracts.require(
        scenario_id in {str(scenario["id"]) for scenario in scenarios},
        f"scenario is not declared in manifest: {scenario_id}",
    )
    result = sanitize_result(raw_result, scenario_id)
    events = sanitize_events(raw_events, scenario_id)
    result_content = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    events_content = "".join(
        json.dumps(event, ensure_ascii=False) + "\n" for event in events
    )
    validate_artifacts(result_content, events_content, scenario_id)

    artifacts.mkdir(parents=True, exist_ok=True)
    result_path = artifacts / f"{scenario_id}.result.json"
    events_path = artifacts / f"{scenario_id}.events.jsonl"
    already_exists = os.path.lexists(result_path) or os.path.lexists(events_path)
    if not force:
        contracts.require(
            not already_exists,
            f"artifacts already exist for {scenario_id}; pass --force to replace them",
        )
    try:
        if force:
            with tempfile.TemporaryDirectory(
                prefix=".mint-eval-", dir=artifacts
            ) as directory:
                temporary = Path(directory)
                temporary_result = temporary / result_path.name
                temporary_events = temporary / events_path.name
                temporary_result.write_text(result_content, encoding="utf-8")
                temporary_events.write_text(events_content, encoding="utf-8")
                os.replace(temporary_result, result_path)
                os.replace(temporary_events, events_path)
        else:
            with result_path.open("x", encoding="utf-8") as output:
                output.write(result_content)
            with events_path.open("x", encoding="utf-8") as output:
                output.write(events_content)
    except OSError as error:
        raise contracts.EvalError(
            f"cannot write artifacts for {scenario_id}: {artifacts}"
        ) from error
    return {
        "schema_version": 1,
        "operation": "eval_collect",
        "mode": "offline",
        "scope": "seed_contract_regression",
        "suite": suite,
        "scenario_id": scenario_id,
        "result": str(result_path),
        "events": str(events_path),
        "overwritten": already_exists,
    }


def validate_artifacts(result: str, events: str, scenario_id: str) -> None:
    with tempfile.TemporaryDirectory(prefix="mint-eval-") as directory:
        temporary = Path(directory)
        result_path = temporary / "result.json"
        events_path = temporary / "events.jsonl"
        result_path.write_text(result, encoding="utf-8")
        events_path.write_text(events, encoding="utf-8")
        agent = contracts.load_result(result_path, scenario_id)
        contracts.load_events(events_path, scenario_id, agent)
