"""Versioned contracts and validators for offline evaluation artifacts."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Mapping


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "evals" / "scenarios.json"
MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_EVENT_BYTES = 64 * 1024 * 1024
MAX_EVENT_LINE_BYTES = 256 * 1024
MAX_EVENTS = 100_000
IDENTIFIER = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
TOOL_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_.-]{0,127}$")
TIMESTAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")

SCENARIO_FIELDS = {"id", "category", "task", "workspace", "oracle"}
ORACLE_FIELDS = {
    "status",
    "verification_status",
    "changed_files",
    "required_tools",
    "forbidden_tools",
    "max_turns",
    "max_tool_calls",
    "max_tool_errors",
}
AGENT_FIELDS = {
    "completed",
    "status",
    "stop_reason",
    "turns",
    "duration_ms",
    "verification_status",
    "execution",
    "model",
    "changes",
}
EXECUTION_FIELDS = {"tool_calls", "tool_errors"}
MODEL_ID_FIELDS = {"adapter", "model", "provider"}
MODEL_FIELDS = {
    *MODEL_ID_FIELDS,
    "prompt_tokens",
    "completion_tokens",
    "total_tokens",
    "cached_tokens",
    "cache_hit_rate",
    "token_budget",
}
TOOLS = {
    "apply_changeset",
    "apply_patch",
    "list_files",
    "read_file",
    "run_command",
    "run_recipe",
    "search_text",
    "workspace_changes",
}
EVENT_TYPES = {
    "approval_requested",
    "approval_resolved",
    "context_compacted",
    "model_completed",
    "model_progress",
    "model_requested",
    "task_finished",
    "task_started",
    "token_budget_exhausted",
    "tool_completed",
    "tool_started",
    "verification_blocked",
    "verification_ready",
}
STATUSES = {
    "budget_exhausted",
    "cancelled",
    "completed",
    "error",
    "failed",
    "max_turns",
    "timed_out",
}
VERIFICATION = {
    "cancelled",
    "denied",
    "failed",
    "not_required",
    "not_run",
    "passed",
    "timed_out",
}
STOP_REASONS = {
    "max_turns_exhausted",
    "max_total_tokens_exhausted",
    "total_budget_exhausted",
    "user_cancelled",
    "verified_final_answer_missing",
    "workspace_integrity_failed",
}


class EvalError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvalError(message)


def object_value(value: object, label: str) -> Mapping[str, object]:
    require(isinstance(value, Mapping), f"{label} must be an object")
    return value


def exact_fields(value: Mapping[str, object], fields: set[str], label: str) -> None:
    missing, extra = fields - set(value), set(value) - fields
    require(not missing, f"{label} is missing fields: {', '.join(sorted(missing))}")
    require(not extra, f"{label} has unsupported fields: {', '.join(sorted(extra))}")


def non_negative_int(value: object, label: str) -> int:
    require(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0,
        f"{label} must be a non-negative integer",
    )
    return value


def text(value: object, label: str, maximum: int = 4096) -> str:
    require(
        isinstance(value, str) and 0 < len(value) <= maximum and "\0" not in value,
        f"{label} must be a bounded non-empty string",
    )
    return value


def enum(value: object, choices: set[str], label: str) -> str:
    result = text(value, label)
    require(result in choices, f"{label} is invalid")
    return result


def relative_path(value: object, label: str, allow_root: bool = False) -> str:
    path = text(value, label, 512)
    if allow_root and path == ".":
        return path
    posix, windows = PurePosixPath(path), PureWindowsPath(path)
    parts = (*posix.parts, *windows.parts)
    require(
        not posix.is_absolute()
        and not windows.drive
        and not windows.root
        and all(part not in {"", ".", ".."} for part in parts)
        and posix.as_posix() == path,
        f"{label} must be a normalized relative path",
    )
    return path


def string_list(
    value: object, label: str, *, choices: set[str] | None = None, paths: bool = False
) -> list[str]:
    require(isinstance(value, list), f"{label} must be an array")
    result = [
        relative_path(item, f"{label}[{index}]")
        if paths
        else text(item, f"{label}[{index}]", 128)
        for index, item in enumerate(value)
    ]
    require(len(result) == len(set(result)), f"{label} contains duplicates")
    require(choices is None or set(result) <= choices, f"{label} contains unsupported values")
    return result


def load_json(path: Path, label: str) -> Mapping[str, object]:
    try:
        require(path.stat().st_size <= MAX_JSON_BYTES, f"{label} is too large")
        return object_value(json.loads(path.read_text(encoding="utf-8")), label)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvalError(f"cannot read {label}: {path}") from error


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(64 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise EvalError(f"cannot hash artifact: {path}") from error
    return digest.hexdigest()


def load_manifest(path: Path) -> tuple[str, str, list[Mapping[str, object]]]:
    document = load_json(path, "evaluation manifest")
    exact_fields(document, {"schema_version", "suite", "description", "scenarios"}, "manifest")
    require(document.get("schema_version") == 1, "manifest schema is unsupported")
    suite = text(document.get("suite"), "manifest suite", 64)
    description = text(document.get("description"), "manifest description", 512)
    require(IDENTIFIER.fullmatch(suite) is not None, "manifest suite is invalid")
    scenarios = document.get("scenarios")
    require(
        isinstance(scenarios, list) and scenarios,
        "manifest scenarios must be a non-empty array",
    )

    seen: set[str] = set()
    for index, raw in enumerate(scenarios):
        label = f"scenario[{index}]"
        scenario = object_value(raw, label)
        exact_fields(scenario, SCENARIO_FIELDS, label)
        scenario_id = text(scenario.get("id"), f"{label} id", 64)
        category = text(scenario.get("category"), f"{label} category", 32)
        require(IDENTIFIER.fullmatch(scenario_id) is not None, f"{label} id is invalid")
        require(IDENTIFIER.fullmatch(category) is not None, f"{label} category is invalid")
        require(scenario_id not in seen, f"duplicate scenario id: {scenario_id}")
        seen.add(scenario_id)
        text(scenario.get("task"), f"{label} task")
        relative_path(scenario.get("workspace"), f"{label} workspace", allow_root=True)
        oracle = object_value(scenario.get("oracle"), f"{label} oracle")
        exact_fields(oracle, ORACLE_FIELDS, f"{label} oracle")
        enum(oracle.get("status"), STATUSES, f"{label} status")
        enum(oracle.get("verification_status"), VERIFICATION, f"{label} verification")
        string_list(oracle.get("changed_files"), f"{label} changed files", paths=True)
        required = string_list(
            oracle.get("required_tools"), f"{label} required tools", choices=TOOLS
        )
        forbidden = string_list(
            oracle.get("forbidden_tools"), f"{label} forbidden tools", choices=TOOLS
        )
        require(not set(required) & set(forbidden), f"{label} tool rules overlap")
        max_turns = non_negative_int(oracle.get("max_turns"), f"{label} max turns")
        max_tools = non_negative_int(oracle.get("max_tool_calls"), f"{label} max tools")
        require(max_turns > 0 and max_tools > 0, f"{label} budgets must be positive")
        non_negative_int(oracle.get("max_tool_errors"), f"{label} max tool errors")
    return suite, description, scenarios


def integer_fields(value: Mapping[str, object], fields: set[str], label: str) -> dict[str, int]:
    return {field: non_negative_int(value.get(field), f"{label} {field}") for field in fields}


def validate_execution(value: object, label: str) -> Mapping[str, object]:
    execution = object_value(value, label)
    exact_fields(execution, EXECUTION_FIELDS, label)
    count = integer_fields(execution, EXECUTION_FIELDS, label)
    require(count["tool_errors"] <= count["tool_calls"], f"{label} counters are inconsistent")
    return execution


def validate_model(value: object, label: str) -> Mapping[str, object]:
    model = object_value(value, label)
    base_fields = MODEL_FIELDS - {"token_budget"}
    exact_fields(model, MODEL_FIELDS if "token_budget" in model else base_fields, label)
    count = integer_fields(model, base_fields - MODEL_ID_FIELDS - {"cache_hit_rate"}, label)
    for field in MODEL_ID_FIELDS:
        text(model.get(field), f"{label} {field}", 256)
    require(
        count["prompt_tokens"] + count["completion_tokens"] == count["total_tokens"],
        f"{label} total tokens are inconsistent",
    )
    require(
        count["cached_tokens"] <= count["prompt_tokens"],
        f"{label} cached tokens are inconsistent",
    )
    observed_rate = model.get("cache_hit_rate")
    expected_rate = (
        count["cached_tokens"] / count["prompt_tokens"] if count["prompt_tokens"] else None
    )
    require(
        (observed_rate is None and expected_rate is None)
        or (
            isinstance(observed_rate, (int, float))
            and not isinstance(observed_rate, bool)
            and abs(float(observed_rate) - expected_rate) <= 1e-12
        ),
        f"{label} cache hit rate is inconsistent",
    )
    if "token_budget" in model:
        validate_token_budget(model["token_budget"], count["total_tokens"], label)
    return model


def validate_token_budget(value: object, total_tokens: int, label: str) -> None:
    budget = object_value(value, f"{label} token budget")
    exact_fields(
        budget,
        {
            "max_total_tokens",
            "reported_total_tokens",
            "usage_coverage",
            "enforcement",
            "exhausted",
        },
        f"{label} token budget",
    )
    maximum = non_negative_int(
        budget.get("max_total_tokens"), f"{label} token budget maximum"
    )
    reported = non_negative_int(
        budget.get("reported_total_tokens"), f"{label} token budget reported total"
    )
    coverage = enum(
        budget.get("usage_coverage"),
        {"complete", "not_started", "partial", "unavailable"},
        f"{label} token budget coverage",
    )
    enforcement = enum(
        budget.get("enforcement"),
        {"best_effort", "disabled", "not_started", "reported_usage", "unavailable"},
        f"{label} token budget enforcement",
    )
    exhausted = budget.get("exhausted")
    require(isinstance(exhausted, bool), f"{label} token budget exhausted must be boolean")
    require(reported == total_tokens, f"{label} token budget total is inconsistent")
    expected_enforcement = (
        "disabled"
        if maximum == 0
        else "not_started"
        if coverage == "not_started"
        else "unavailable"
        if coverage == "unavailable"
        else "reported_usage"
        if coverage == "complete"
        else "best_effort"
    )
    require(
        coverage not in {"not_started", "unavailable"} or reported == 0,
        f"{label} token budget coverage is inconsistent",
    )
    require(
        enforcement == expected_enforcement,
        f"{label} token budget enforcement is inconsistent",
    )
    require(
        exhausted == (maximum != 0 and reported >= maximum),
        f"{label} token budget exhaustion is inconsistent",
    )


def load_result(path: Path, scenario_id: str) -> Mapping[str, object]:
    document = load_json(path, f"result for {scenario_id}")
    exact_fields(
        document,
        {"schema_version", "scenario_id", "agent"},
        f"result for {scenario_id}",
    )
    require(
        document.get("schema_version") == 1,
        f"result for {scenario_id} has unsupported schema",
    )
    require(document.get("scenario_id") == scenario_id, f"result for {scenario_id} has wrong id")
    agent = object_value(document.get("agent"), f"result for {scenario_id} agent")
    exact_fields(agent, AGENT_FIELDS, f"result for {scenario_id} agent")
    completed = agent.get("completed")
    status = enum(agent.get("status"), STATUSES, f"result for {scenario_id} status")
    require(
        isinstance(completed, bool) and completed == (status == "completed"),
        f"result for {scenario_id} completion is inconsistent",
    )
    stop_reason = agent.get("stop_reason")
    require(
        stop_reason is None or stop_reason in STOP_REASONS,
        f"result for {scenario_id} stop reason is invalid",
    )
    require(
        (completed and stop_reason is None) or (not completed and stop_reason is not None),
        f"result for {scenario_id} stop reason is inconsistent",
    )
    non_negative_int(agent.get("turns"), f"result for {scenario_id} turns")
    non_negative_int(agent.get("duration_ms"), f"result for {scenario_id} duration")
    enum(
        agent.get("verification_status"),
        VERIFICATION,
        f"result for {scenario_id} verification",
    )
    validate_execution(agent.get("execution"), f"result for {scenario_id} execution")
    model = validate_model(agent.get("model"), f"result for {scenario_id} model")
    if status == "budget_exhausted":
        budget = object_value(model.get("token_budget"), f"result for {scenario_id} token budget")
        require(
            stop_reason == "max_total_tokens_exhausted" and budget.get("exhausted") is True,
            f"result for {scenario_id} exhausted status is inconsistent",
        )
    changes = object_value(agent.get("changes"), f"result for {scenario_id} changes")
    exact_fields(changes, {"files"}, f"result for {scenario_id} changes")
    changed = string_list(
        changes.get("files"), f"result for {scenario_id} changed files", paths=True
    )
    require(changed == sorted(changed), f"result for {scenario_id} changed files must be sorted")
    return agent


def load_events(path: Path, scenario_id: str, agent: Mapping[str, object]) -> list[str]:
    try:
        require(path.stat().st_size <= MAX_EVENT_BYTES, f"events for {scenario_id} are too large")
        lines = path.read_bytes().splitlines()
    except OSError as error:
        raise EvalError(f"cannot read events for {scenario_id}: {path}") from error
    require(0 < len(lines) <= MAX_EVENTS, f"events for {scenario_id} have invalid record count")
    tool_names: list[str] = []
    open_tools: dict[str, str] = {}
    event_types: list[str] = []
    finish: Mapping[str, object] | None = None
    model_completions = token_budget_events = 0
    for sequence, raw in enumerate(lines, start=1):
        require(
            len(raw) <= MAX_EVENT_LINE_BYTES,
            f"event {sequence} for {scenario_id} is too large",
        )
        try:
            event = object_value(json.loads(raw), f"event {sequence} for {scenario_id}")
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise EvalError(f"event {sequence} for {scenario_id} is invalid JSON") from error
        label = f"event {sequence} for {scenario_id}"
        exact_fields(event, {"schema_version", "seq", "timestamp", "type", "data"}, label)
        require(
            event.get("schema_version") == 1 and event.get("seq") == sequence,
            f"{label} sequence is not contiguous",
        )
        timestamp = text(event.get("timestamp"), f"{label} timestamp", 32)
        require(TIMESTAMP.fullmatch(timestamp) is not None, f"{label} timestamp is invalid")
        event_type = enum(event.get("type"), EVENT_TYPES, f"{label} type")
        data = object_value(event.get("data"), f"{label} data")
        event_types.append(event_type)
        if event_type == "model_completed":
            non_negative_int(data.get("turn"), f"{label} turn")
            model_completions += 1
        elif event_type in {"tool_started", "tool_completed"}:
            call_id = text(data.get("tool_call_id"), f"{label} tool id", 256)
            name = text(data.get("name"), f"{label} tool name", 128)
            require(TOOL_NAME.fullmatch(name) is not None, f"{label} tool name is invalid")
            if event_type == "tool_started":
                require(call_id not in open_tools, f"{label} repeats an open tool id")
                open_tools[call_id] = name
                tool_names.append(name)
            else:
                require(
                    open_tools.pop(call_id, None) == name,
                    f"{label} has no matching tool start",
                )
        elif event_type == "token_budget_exhausted":
            token_budget_events += 1
            validate_budget_event(data, agent, label, scenario_id)
        elif event_type == "task_finished":
            finish = data
    require(
        event_types[0] == "task_started" and event_types[-1] == "task_finished",
        f"events for {scenario_id} have invalid task boundaries",
    )
    require(
        event_types.count("task_started") == 1 and event_types.count("task_finished") == 1,
        f"events for {scenario_id} have duplicate task boundaries",
    )
    require(not open_tools, f"events for {scenario_id} contain unfinished tool calls")
    execution = object_value(agent.get("execution"), f"result for {scenario_id} execution")
    require(
        len(tool_names) == execution.get("tool_calls"),
        f"events for {scenario_id} disagree with result tool calls",
    )
    require(
        model_completions == agent.get("turns"),
        f"events for {scenario_id} disagree with result turns",
    )
    require(
        token_budget_events == (1 if agent.get("status") == "budget_exhausted" else 0),
        f"events for {scenario_id} disagree with budget-exhausted status",
    )
    require(finish is not None, f"events for {scenario_id} have no finish record")
    expected_finish = {
        "status": agent.get("status"),
        "turns": agent.get("turns"),
        "duration_ms": agent.get("duration_ms"),
        "verification_status": agent.get("verification_status"),
        "tool_calls": execution.get("tool_calls"),
    }
    require(
        all(finish.get(key) == value for key, value in expected_finish.items()),
        f"events for {scenario_id} disagree with result summary",
    )
    return tool_names


def validate_budget_event(
    data: Mapping[str, object],
    agent: Mapping[str, object],
    label: str,
    scenario_id: str,
) -> None:
    exact_fields(
        data,
        {
            "max_total_tokens",
            "reported_total_tokens",
            "model_calls",
            "usage_reports",
            "pending_tool_call_count",
        },
        label,
    )
    budget = object_value(agent.get("model"), f"result for {scenario_id} model").get(
        "token_budget"
    )
    require(
        data.get("max_total_tokens") == budget.get("max_total_tokens")
        and data.get("reported_total_tokens") == budget.get("reported_total_tokens")
        and data.get("model_calls") == agent.get("turns"),
        f"{label} disagrees with result token budget",
    )
    non_negative_int(data.get("usage_reports"), f"{label} usage reports")
    non_negative_int(data.get("pending_tool_call_count"), f"{label} pending tools")
