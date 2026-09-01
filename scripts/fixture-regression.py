#!/usr/bin/env python3
"""Run the v1 repair fixture in isolation and retain only sanitized evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Mapping, Sequence

from provider_matrix import ProviderMatrixError, load_provider_matrix, profile_by_config
from release_evidence import EvidenceError, release_source_digest, require_release_source_tree


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "configs" / "provider-regression.json"
DEFAULT_CONFIG = ROOT / "configs" / "providers" / "openai-responses.json"
DEFAULT_FIXTURE = ROOT / "tests" / "fixtures" / "v1_broken_project"
EXPECTED_SOURCE_CHANGES = {"FIX_REPORT.md", "src/calculator.cpp"}
IGNORED_WORKSPACE_DIRECTORIES = {"build"}
PROCESS_STATUSES = {"failed", "invalid_json", "start_failed", "timed_out"}
MODEL_PROGRESS_KINDS = {
    "attempt_started",
    "stream_started",
    "stream_completed",
    "retry_scheduled",
    "request_succeeded",
    "request_failed",
}
MAX_EVENT_LOG_BYTES = 64 * 1024 * 1024
EXPECTED_TASK_LIMITS = {
    "max_turns": 16,
    "max_context_bytes": 24 * 1024,
    "max_seconds": 600,
}
SAFE_AGENT_STOP_REASONS = {
    "max_turns_exhausted",
    "total_budget_exhausted",
    "user_cancelled",
    "verified_final_answer_missing",
    "workspace_integrity_failed",
}
WORKSPACE_RISK_STATUSES = {"policy_violation", "unauditable"}
TASK = (
    "Complete this exact fixture acceptance sequence without exploring unrelated files. "
    "The regression runner already proved the failing baseline; do not repeat it or run commands "
    "before the requested edits. "
    "First read README.md and src/calculator.cpp. Then use apply_patch on src/calculator.cpp "
    "with operation replace to change the exact text `return left - right;` to "
    "`return left + right;`. Use apply_patch on FIX_REPORT.md with operation create and write a "
    "short root-cause and verification summary. After both edits, run the configure, build, and "
    "test recipes in that order. When the test recipe passes, call no more tools and immediately "
    "return a concise final answer. Do not change any other file or weaken the tests."
)


class FixtureRegressionError(RuntimeError):
    def __init__(self, stage: str, message: str, details: Mapping[str, object] | None = None):
        super().__init__(message)
        self.stage = stage
        self.details = dict(details or {})


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


def run_command(command: Sequence[str], stage: str, timeout: int = 180) -> CommandResult:
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise FixtureRegressionError(
            stage,
            f"command timed out after {timeout} seconds",
            {"process_status": "timed_out", "timeout_seconds": timeout},
        ) from error
    except OSError as error:
        raise FixtureRegressionError(
            stage, f"cannot start command: {command[0]}", {"process_status": "start_failed"}
        ) from error
    return CommandResult(result.returncode, result.stdout, result.stderr)


def parse_json_output(result: CommandResult, stage: str) -> dict:
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise FixtureRegressionError(stage, "command did not return a JSON object") from error
    if not isinstance(document, dict):
        raise FixtureRegressionError(stage, "command did not return a JSON object")
    return document


def mint_version(executable: Path) -> str:
    result = run_command([str(executable), "--version"], "mint_version")
    match = re.fullmatch(r"mint ([0-9]+\.[0-9]+\.[0-9]+)\s*", result.stdout)
    if result.returncode != 0 or match is None:
        raise FixtureRegressionError("mint_version", "mint --version returned an unexpected result")
    return match.group(1)


def inspect_provider(executable: Path, config: Path) -> dict:
    result = run_command(
        [
            str(executable),
            "provider",
            "--config",
            str(config),
            "--json",
            "--log-level",
            "off",
        ],
        "provider_inspection",
    )
    document = parse_json_output(result, "provider_inspection")
    if result.returncode != 0 or document.get("operation") != "inspect":
        raise FixtureRegressionError("provider_inspection", "offline provider inspection failed")
    capabilities = document.get("capabilities")
    if not isinstance(capabilities, dict) or capabilities.get("function_tools") is not True:
        raise FixtureRegressionError(
            "provider_inspection", "fixture regression requires function tool support"
        )
    if (
        document.get("authentication") != "environment"
        or not isinstance(document.get("api_key_env"), str)
        or not document.get("api_key_env")
    ):
        raise FixtureRegressionError(
            "provider_inspection", "fixture regression requires environment-based authentication"
        )
    return document


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_inventory(root: Path, stage: str = "fixture_validation") -> dict[str, str]:
    inventory: dict[str, str] = {}
    try:
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(root)
            if relative.parts and relative.parts[0] in IGNORED_WORKSPACE_DIRECTORIES:
                continue
            if path.is_symlink():
                raise FixtureRegressionError(stage, "fixture must not contain symlinks")
            if path.is_file():
                inventory[relative.as_posix()] = sha256_file(path)
    except OSError as error:
        raise FixtureRegressionError(stage, "cannot inspect fixture source inventory") from error
    return inventory


def inventory_digest(inventory: Mapping[str, str]) -> str:
    digest = hashlib.sha256()
    for name, content_digest in sorted(inventory.items()):
        encoded_name = name.encode("utf-8")
        digest.update(len(encoded_name).to_bytes(8, "big"))
        digest.update(encoded_name)
        digest.update(bytes.fromhex(content_digest))
    return digest.hexdigest()


def validate_fixture(fixture: Path) -> dict[str, str]:
    required = {
        "CMakeLists.txt",
        "README.md",
        "include/calculator.hpp",
        "policy.v1_2.json",
        "src/calculator.cpp",
        "tests/calculator_tests.cpp",
    }
    inventory = source_inventory(fixture)
    if set(inventory) != required:
        raise FixtureRegressionError("fixture_validation", "fixture source inventory is unexpected")

    try:
        policy = json.loads((fixture / "policy.v1_2.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FixtureRegressionError("fixture_validation", "fixture policy is invalid") from error
    if not isinstance(policy, dict):
        raise FixtureRegressionError("fixture_validation", "fixture policy is invalid")
    write_paths = policy.get("write_paths")
    recipes = policy.get("recipes")
    expected_recipes = {
        "configure": ("cmake", ["-S", ".", "-B", "build"], False),
        "build": ("cmake", ["--build", "build", "--clean-first"], False),
        "test": ("ctest", ["--test-dir", "build", "--output-on-failure"], True),
    }
    recipe_contract = {
        recipe.get("name"): (
            recipe.get("program"),
            recipe.get("args"),
            recipe.get("verification", False),
        )
        for recipe in recipes
        if isinstance(recipe, dict) and isinstance(recipe.get("name"), str)
    } if isinstance(recipes, list) else {}
    if (
        policy.get("schema_version") != 1
        or not isinstance(write_paths, list)
        or not all(isinstance(path, str) for path in write_paths)
        or set(write_paths) != EXPECTED_SOURCE_CHANGES
        or len(recipe_contract) != 3
        or recipe_contract != expected_recipes
        or policy.get("require_verification") is not True
        or any(policy.get(field) != value for field, value in EXPECTED_TASK_LIMITS.items())
    ):
        raise FixtureRegressionError("fixture_validation", "fixture policy boundary is unexpected")
    return inventory


def safe_profile_report(document: Mapping[str, object]) -> dict:
    return {
        "provider": document.get("provider"),
        "adapter": document.get("adapter"),
        "endpoint": document.get("endpoint"),
        "model": document.get("model"),
        "stream": document.get("stream"),
        "api_key_env": document.get("api_key_env"),
    }


def numeric_fields(document: Mapping[str, object], fields: Sequence[str]) -> dict:
    return {
        field: value
        for field in fields
        if isinstance((value := document.get(field)), int) and not isinstance(value, bool)
    }


def optional_integer(document: Mapping[str, object], field: str) -> int | None:
    value = document.get(field)
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def fixture_limits(inspection: Mapping[str, object]) -> dict[str, object]:
    request_limits = inspection.get("limits")
    if not isinstance(request_limits, Mapping):
        raise FixtureRegressionError(
            "provider_inspection", "provider inspection did not report request limits"
        )
    max_completion_tokens = optional_integer(request_limits, "max_completion_tokens")
    max_attempts = optional_integer(request_limits, "max_attempts_per_request")
    max_request_tokens = optional_integer(request_limits, "max_request_tokens")
    safety_margin = optional_integer(request_limits, "request_token_safety_margin")
    estimate_bytes_per_token = optional_integer(
        request_limits, "request_token_estimate_bytes_per_token"
    )
    request_limit_source = request_limits.get("max_request_tokens_source")
    if (
        max_completion_tokens is None
        or max_completion_tokens <= 0
        or max_attempts is None
        or max_attempts <= 0
        or max_request_tokens is None
        or max_request_tokens <= 0
        or safety_margin is None
        or safety_margin < 0
        or estimate_bytes_per_token is None
        or not 1 <= estimate_bytes_per_token <= 8
        or request_limit_source not in {"automatic", "config"}
    ):
        raise FixtureRegressionError(
            "provider_inspection", "provider inspection reported invalid request limits"
        )
    max_context_bytes = EXPECTED_TASK_LIMITS["max_context_bytes"]
    return {
        **EXPECTED_TASK_LIMITS,
        "max_context_estimated_tokens": (
            max_context_bytes + estimate_bytes_per_token - 1
        )
        // estimate_bytes_per_token,
        "max_request_tokens": max_request_tokens,
        "max_request_tokens_source": request_limit_source,
        "response_header_max_request_tokens": None,
        "request_token_safety_margin": safety_margin,
        "request_token_estimate_bytes_per_token": estimate_bytes_per_token,
        "max_completion_tokens_per_request": max_completion_tokens,
        "max_attempts_per_request": max_attempts,
    }


def safe_agent_report(
    document: Mapping[str, object], changed_files: Sequence[str] | None
) -> dict:
    execution = document.get("execution")
    model = document.get("model")
    safe_execution = numeric_fields(
        execution if isinstance(execution, Mapping) else {},
        (
            "tool_calls",
            "successful_tool_calls",
            "tool_errors",
            "file_changes",
            "command_calls",
            "recipe_calls",
            "verification_commands",
            "commands_passed",
            "commands_failed",
            "commands_timed_out",
            "commands_cancelled",
            "commands_denied",
            "last_file_change_call",
            "last_command_call",
        ),
    )
    if isinstance(execution, Mapping):
        last_outcome = execution.get("last_command_outcome")
        safe_outcomes = {"not_run", "passed", "failed", "timed_out", "cancelled", "denied"}
        if last_outcome in safe_outcomes:
            safe_execution["last_command_outcome"] = last_outcome
        verification_eligible = execution.get("last_command_verification_eligible")
        if isinstance(verification_eligible, bool):
            safe_execution["last_command_verification_eligible"] = verification_eligible
    safe_model = numeric_fields(
        model if isinstance(model, Mapping) else {},
        (
            "calls",
            "attempts",
            "retries",
            "usage_reports",
            "prompt_tokens",
            "completion_tokens",
            "total_tokens",
            "cached_tokens",
            "streamed_calls",
            "stream_events",
            "streamed_bytes",
            "duration_ms",
            "max_request_tokens",
            "response_header_max_request_tokens",
            "request_token_estimate_bytes_per_token",
        ),
    )
    if isinstance(model, Mapping):
        if "cache_hit_rate" in model:
            cache_hit_rate = model.get("cache_hit_rate")
            if cache_hit_rate is None or (
                isinstance(cache_hit_rate, (int, float))
                and not isinstance(cache_hit_rate, bool)
                and 0.0 <= float(cache_hit_rate) <= 1.0
            ):
                safe_model["cache_hit_rate"] = cache_hit_rate
        for field in ("adapter", "provider", "model", "max_request_tokens_source"):
            if isinstance(model.get(field), str):
                safe_model[field] = model[field]
        if model.get("response_header_max_request_tokens") is None:
            safe_model["response_header_max_request_tokens"] = None
    changes = (
        {"files": list(changed_files)} if changed_files is not None else {"observed": False}
    )
    safe_stop_reason = document.get("stop_reason")
    return {
        "completed": document.get("completed") is True,
        "status": document.get("status") if isinstance(document.get("status"), str) else None,
        "stop_reason": (
            safe_stop_reason if safe_stop_reason in SAFE_AGENT_STOP_REASONS else None
        ),
        "turns": optional_integer(document, "turns"),
        "duration_ms": optional_integer(document, "duration_ms"),
        "verification_status": (
            document.get("verification_status")
            if isinstance(document.get("verification_status"), str)
            else None
        ),
        "execution": safe_execution,
        "model": safe_model,
        "changes": changes,
    }


def safe_workspace_risks(document: Mapping[str, object]) -> list[dict[str, str]]:
    changes = document.get("changes")
    details = changes.get("details") if isinstance(changes, Mapping) else None
    if not isinstance(details, list):
        return []

    risks: list[dict[str, str]] = []
    for item in details:
        if not isinstance(item, Mapping):
            continue
        path = item.get("path")
        status = item.get("status")
        if (
            not isinstance(path, str)
            or not 0 < len(path) <= 512
            or "\0" in path
            or status not in WORKSPACE_RISK_STATUSES
        ):
            continue
        if path != "<workspace>":
            posix_path = PurePosixPath(path)
            windows_path = PureWindowsPath(path)
            path_parts = (*posix_path.parts, *windows_path.parts)
            if (
                posix_path.is_absolute()
                or windows_path.drive
                or windows_path.root
                or any(part in {"", ".", ".."} for part in path_parts)
            ):
                continue
        risks.append({"path": path, "status": status})
        if len(risks) == 16:
            break
    return risks


def safe_agent_failure(
    document: Mapping[str, object], events_path: Path, configured_max_attempts: int
) -> dict[str, object] | None:
    if document.get("completed") is not False:
        return None

    if (
        document.get("status") == "failed"
        and document.get("stop_reason") == "workspace_integrity_failed"
    ):
        result: dict[str, object] = {"category": "workspace_integrity_failed"}
        risks = safe_workspace_risks(document)
        if risks:
            result["workspace_risks"] = risks
        return result

    if document.get("status") != "error":
        return None

    result: dict[str, object] = {"category": "agent_error"}
    last_progress: dict[str, object] | None = None
    try:
        if events_path.stat().st_size > MAX_EVENT_LOG_BYTES:
            return result
        with events_path.open("rb") as events:
            for raw_line in events:
                try:
                    event = json.loads(raw_line)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
                if not isinstance(event, Mapping):
                    continue
                if event.get("type") != "model_progress":
                    continue
                schema_version = event.get("schema_version")
                data = event.get("data")
                if (
                    schema_version != 1
                    or isinstance(schema_version, bool)
                    or not isinstance(data, Mapping)
                    or data.get("kind") not in MODEL_PROGRESS_KINDS
                ):
                    continue
                last_progress = {
                    field: data.get(field)
                    for field in ("kind", "attempt", "max_attempts", "http_status")
                }
    except OSError:
        return result

    if last_progress is None or last_progress.get("kind") != "request_failed":
        return result

    result["category"] = "model_request_failed"
    http_status = last_progress.get("http_status")
    if (
        isinstance(http_status, int)
        and not isinstance(http_status, bool)
        and 100 <= http_status <= 599
    ):
        result["http_status"] = http_status

    attempt = last_progress.get("attempt")
    max_attempts = last_progress.get("max_attempts")
    if (
        isinstance(configured_max_attempts, int)
        and not isinstance(configured_max_attempts, bool)
        and configured_max_attempts > 0
        and isinstance(attempt, int)
        and not isinstance(attempt, bool)
        and isinstance(max_attempts, int)
        and not isinstance(max_attempts, bool)
        and 1 <= attempt <= max_attempts <= configured_max_attempts
    ):
        result.update(
            attempts=attempt,
            retries=attempt - 1,
            max_attempts=max_attempts,
        )
    return result


def effective_agent_limits(agent: Mapping[str, object]) -> dict[str, object]:
    model = agent.get("model")
    if not isinstance(model, Mapping):
        raise FixtureRegressionError("agent_result", "agent result has no model limits")
    maximum = optional_integer(model, "max_request_tokens")
    learned_value = model.get("response_header_max_request_tokens")
    learned = optional_integer(model, "response_header_max_request_tokens")
    estimate = optional_integer(model, "request_token_estimate_bytes_per_token")
    source = model.get("max_request_tokens_source")
    if (
        maximum is None
        or maximum <= 0
        or source not in {"automatic", "config", "response_header"}
        or (learned_value is not None and (learned is None or learned <= 0))
        or estimate is None
        or not 1 <= estimate <= 8
        or (source == "response_header" and (learned is None or maximum != learned))
    ):
        raise FixtureRegressionError(
            "agent_result", "agent result reported invalid effective request limits"
        )
    return {
        "max_request_tokens": maximum,
        "max_request_tokens_source": source,
        "response_header_max_request_tokens": learned,
        "request_token_estimate_bytes_per_token": estimate,
    }


def safe_failure_details(error: FixtureRegressionError) -> dict:
    result: dict[str, object] = {"stage": error.stage}
    process_status = error.details.get("process_status")
    if process_status in PROCESS_STATUSES:
        result["process_status"] = process_status
    for field in ("exit_code", "timeout_seconds"):
        value = error.details.get(field)
        if isinstance(value, int) and not isinstance(value, bool):
            result[field] = value
    boundary = error.details.get("source_boundary")
    if isinstance(boundary, Mapping):
        expected = sorted(EXPECTED_SOURCE_CHANGES)
        observed = boundary.get("observed_files")
        report_status = boundary.get("report_status")
        if (
            isinstance(observed, list)
            and all(isinstance(path, str) for path in observed)
            and report_status in {"empty", "invalid_utf8", "missing", "present", "unreadable"}
        ):
            observed_set = set(observed)
            result["source_boundary"] = {
                "expected_files": expected,
                "observed_files": observed,
                "missing_files": sorted(EXPECTED_SOURCE_CHANGES - observed_set),
                "unexpected_files": sorted(observed_set - EXPECTED_SOURCE_CHANGES),
                "report_status": report_status,
            }
    return result


def require_success(result: CommandResult, stage: str) -> None:
    if result.returncode != 0:
        raise FixtureRegressionError(stage, "command failed", {"exit_code": result.returncode})


def configure_build(project: Path, build: Path, prefix: str) -> None:
    configure = run_command(
        ["cmake", "-S", str(project), "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug"],
        f"{prefix}_configure",
    )
    require_success(configure, f"{prefix}_configure")
    build_result = run_command(
        ["cmake", "--build", str(build), "--config", "Debug", "--clean-first"],
        f"{prefix}_build",
    )
    require_success(build_result, f"{prefix}_build")


def run_baseline(project: Path) -> dict:
    build = project / "build"
    configure_build(project, build, "baseline")
    test = run_command(
        ["ctest", "--test-dir", str(build), "-C", "Debug", "--output-on-failure"],
        "baseline_test",
    )
    output = test.stdout + test.stderr
    if test.returncode == 0 or "add(2, 3) should be 5" not in output:
        raise FixtureRegressionError(
            "baseline_test", "fixture did not produce the expected failing test"
        )
    return {"configure": "passed", "build": "passed", "test": "failed_as_expected"}


def command_sandbox_preflight() -> dict[str, str] | None:
    if sys.platform != "darwin":
        return None
    result = run_command(
        [
            "/usr/bin/sandbox-exec",
            "-p",
            "(version 1)(allow default)",
            "/usr/bin/true",
        ],
        "command_sandbox_preflight",
        timeout=10,
    )
    if result.returncode != 0:
        raise FixtureRegressionError(
            "command_sandbox_preflight",
            "macOS Seatbelt cannot start in the current execution environment",
            {"process_status": "failed", "exit_code": result.returncode},
        )
    return {"status": "passed", "backend": "macos-seatbelt"}


def run_agent(executable: Path, config: Path, project: Path, runtime: Path) -> tuple[int, dict]:
    runtime.mkdir(parents=True)
    command = [
        str(executable),
        "--config",
        str(config),
        "--policy",
        str(project / "policy.v1_2.json"),
        "--root",
        str(project),
        "--session",
        str(runtime / "session.json"),
        "--events-jsonl",
        str(runtime / "events.jsonl"),
        "--json",
        "--log-level",
        "off",
        TASK,
    ]
    result = run_command(command, "agent", timeout=660)
    try:
        document = parse_json_output(result, "agent")
    except FixtureRegressionError as error:
        raise FixtureRegressionError(
            "agent",
            str(error),
            {"process_status": "invalid_json", "exit_code": result.returncode},
        ) from error
    return result.returncode, document


def changed_sources(before: Mapping[str, str], project: Path) -> list[str]:
    after = source_inventory(project, "source_observation")
    names = set(before) | set(after)
    return sorted(name for name in names if before.get(name) != after.get(name))


def validate_source_boundary(changed: Sequence[str], project: Path) -> None:
    observed = set(changed)
    report = project / "FIX_REPORT.md"
    if not report.is_file():
        report_status = "missing"
    else:
        try:
            report_status = "present" if report.read_text(encoding="utf-8").strip() else "empty"
        except UnicodeError:
            report_status = "invalid_utf8"
        except OSError:
            report_status = "unreadable"
    details = {
        "source_boundary": {
            "observed_files": list(changed),
            "report_status": report_status,
        }
    }
    if observed != EXPECTED_SOURCE_CHANGES or report_status != "present":
        raise FixtureRegressionError(
            "source_boundary", "repair did not satisfy the expected source boundary", details
        )


def validate_agent_result(
    document: Mapping[str, object],
    changed_files: Sequence[str],
    inspection: Mapping[str, object],
) -> None:
    model = document.get("model")
    execution = document.get("execution")
    reported_changes = document.get("changes")
    reported_files = (
        reported_changes.get("files") if isinstance(reported_changes, Mapping) else None
    )
    streamed_calls = (
        optional_integer(model, "streamed_calls") if isinstance(model, Mapping) else None
    )
    verification_commands = (
        optional_integer(execution, "verification_commands")
        if isinstance(execution, Mapping)
        else None
    )
    if (
        document.get("completed") is not True
        or document.get("status") != "completed"
        or document.get("verification_status") != "passed"
        or not isinstance(model, Mapping)
        or model.get("provider") != inspection.get("provider")
        or model.get("adapter") != inspection.get("adapter")
        or streamed_calls is None
        or ((streamed_calls > 0) != (inspection.get("stream") is True))
        or not isinstance(execution, Mapping)
        or verification_commands is None
        or verification_commands < 1
        or execution.get("last_command_outcome") != "passed"
        or execution.get("last_command_verification_eligible") is not True
        or not isinstance(reported_files, list)
        or not all(isinstance(path, str) for path in reported_files)
        or set(reported_files) != set(changed_files)
    ):
        raise FixtureRegressionError("agent_result", "agent result failed the repair contract")


def independent_verification(project: Path, build: Path) -> dict:
    configure_build(project, build, "verification")
    test = run_command(
        ["ctest", "--test-dir", str(build), "-C", "Debug", "--output-on-failure"],
        "verification_test",
    )
    require_success(test, "verification_test")
    return {"configure": "passed", "build": "passed", "test": "passed"}


def write_report(path: Path | None, report: Mapping[str, object]) -> None:
    content = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if path is None:
        sys.stdout.write(content)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("x", encoding="utf-8") as output:
            output.write(content)
    except FileExistsError as error:
        raise FixtureRegressionError(
            "evidence", f"refusing to replace evidence file: {path}"
        ) from error
    except OSError as error:
        path.unlink(missing_ok=True)
        raise FixtureRegressionError("evidence", f"cannot write evidence file: {path}") from error


def base_report(
    args: argparse.Namespace,
    profile_id: str,
    inspection: Mapping[str, object],
    inventory: Mapping[str, str],
) -> dict:
    return {
        "schema_version": 1,
        "operation": "fixture_regression",
        "mode": "live" if args.live else "inspect",
        "generated_at_utc": datetime.now(timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z"),
        "mint_version": mint_version(args.mint),
        "profile_id": profile_id,
        "config_sha256": sha256_file(args.config),
        "fixture_sha256": inventory_digest(inventory),
        "profile": safe_profile_report(inspection),
        "limits": fixture_limits(inspection),
        "status": "passed",
    }


def execute(args: argparse.Namespace) -> int:
    if args.output is not None and args.output.exists():
        raise FixtureRegressionError(
            "evidence", f"refusing to replace evidence file: {args.output}"
        )
    try:
        profile = profile_by_config(load_provider_matrix(DEFAULT_MANIFEST), args.config)
    except ProviderMatrixError as error:
        raise FixtureRegressionError("provider_profile", str(error)) from error
    inventory = validate_fixture(args.fixture)
    inspection = inspect_provider(args.mint, args.config)
    report = base_report(args, profile.id, inspection, inventory)
    if not args.live:
        write_report(args.output, report)
        return 0

    key_name = inspection["api_key_env"]
    if not os.environ.get(key_name):
        report["status"] = "blocked"
        report["missing_environment"] = [key_name]
        write_report(args.output, report)
        return 2

    try:
        require_release_source_tree(ROOT)
        report["source_sha256"] = release_source_digest(ROOT)
    except EvidenceError as error:
        raise FixtureRegressionError("source_digest", str(error)) from error

    try:
        sandbox_preflight = command_sandbox_preflight()
        if sandbox_preflight is not None:
            report["command_sandbox_preflight"] = sandbox_preflight
        with tempfile.TemporaryDirectory(prefix="mint-fixture-regression-") as directory:
            temporary = Path(directory)
            project = temporary / "project"
            runtime = temporary / "runtime"
            shutil.copytree(args.fixture, project)
            report["baseline"] = run_baseline(project)
            agent_exit, agent_result = run_agent(args.mint, args.config, project, runtime)
            if agent_exit != 0:
                report["agent_process"] = {"status": "failed", "exit_code": agent_exit}
                failure = safe_agent_failure(
                    agent_result,
                    runtime / "events.jsonl",
                    report["limits"]["max_attempts_per_request"],
                )
                if failure is not None:
                    report["agent_process"]["failure"] = failure
            report["agent"] = safe_agent_report(agent_result, None)
            changed = changed_sources(inventory, project)
            report["agent"] = safe_agent_report(agent_result, changed)
            if agent_exit == 0:
                report["limits"].update(effective_agent_limits(report["agent"]))
            validate_source_boundary(changed, project)
            report["independent_verification"] = independent_verification(
                project, temporary / "verification-build"
            )
            if agent_exit != 0:
                raise FixtureRegressionError(
                    "agent",
                    "mint returned a non-zero exit code",
                    {"process_status": "failed", "exit_code": agent_exit},
                )
            validate_agent_result(agent_result, changed, inspection)
    except FixtureRegressionError as error:
        report["status"] = "failed"
        report["failed_stage"] = error.stage
        report["failure"] = safe_failure_details(error)
        report.setdefault(
            "independent_verification", {"status": "not_run", "reason": error.stage}
        )
        write_report(args.output, report)
        return 1

    write_report(args.output, report)
    return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mint", type=Path, required=True, help="path to the mint executable")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    parser.add_argument("--live", action="store_true", help="allow the external model run")
    parser.add_argument("--output", type=Path, help="new JSON evidence file; never overwritten")
    args = parser.parse_args(argv)
    if args.live and args.output is None:
        parser.error("--live requires --output so sanitized evidence is retained")
    return args


def main() -> int:
    try:
        return execute(parse_args())
    except FixtureRegressionError as error:
        print(f"fixture regression failed at {error.stage}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
