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
from pathlib import Path
from typing import Mapping, Sequence


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "configs" / "providers" / "openai-responses.json"
DEFAULT_FIXTURE = ROOT / "tests" / "fixtures" / "v1_broken_project"
EXPECTED_SOURCE_CHANGES = {"FIX_REPORT.md", "src/calculator.cpp"}
IGNORED_WORKSPACE_DIRECTORIES = {"build"}
TASK = (
    "Repair the intentional bug in this fixture. Follow README.md and the task policy: "
    "diagnose with the registered recipes, change only authorized files, create FIX_REPORT.md, "
    "and finish only after the verification recipe passes."
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
        raise FixtureRegressionError(stage, f"command timed out after {timeout} seconds") from error
    except OSError as error:
        raise FixtureRegressionError(stage, f"cannot start command: {command[0]}") from error
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
    if document.get("provider") != "openai" or document.get("adapter") != "responses":
        raise FixtureRegressionError(
            "provider_inspection", "fixture regression requires an OpenAI Responses profile"
        )
    if document.get("stream") is not True:
        raise FixtureRegressionError(
            "provider_inspection", "fixture regression requires SSE streaming"
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


def source_inventory(root: Path) -> dict[str, str]:
    inventory: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if relative.parts and relative.parts[0] in IGNORED_WORKSPACE_DIRECTORIES:
            continue
        if path.is_symlink():
            raise FixtureRegressionError("fixture_validation", "fixture must not contain symlinks")
        if path.is_file():
            inventory[relative.as_posix()] = sha256_file(path)
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
        "build": ("cmake", ["--build", "build"], False),
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
        or policy.get("max_turns") != 24
        or policy.get("max_context_bytes") != 131072
        or policy.get("max_seconds") != 600
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


def safe_agent_report(document: Mapping[str, object], changed_files: Sequence[str]) -> dict:
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
        ),
    )
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
        ),
    )
    if isinstance(model, Mapping):
        for field in ("adapter", "provider", "model"):
            if isinstance(model.get(field), str):
                safe_model[field] = model[field]
    return {
        "completed": document.get("completed") is True,
        "status": document.get("status") if isinstance(document.get("status"), str) else None,
        "turns": optional_integer(document, "turns"),
        "duration_ms": optional_integer(document, "duration_ms"),
        "verification_status": (
            document.get("verification_status")
            if isinstance(document.get("verification_status"), str)
            else None
        ),
        "execution": safe_execution,
        "model": safe_model,
        "changes": {"files": list(changed_files)},
    }


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
    return result.returncode, parse_json_output(result, "agent")


def changed_sources(before: Mapping[str, str], project: Path) -> list[str]:
    after = source_inventory(project)
    names = set(before) | set(after)
    changed = sorted(name for name in names if before.get(name) != after.get(name))
    if set(changed) != EXPECTED_SOURCE_CHANGES:
        raise FixtureRegressionError(
            "source_boundary", "repair changed an unexpected source inventory"
        )
    if not (project / "FIX_REPORT.md").is_file() or not (project / "FIX_REPORT.md").read_text(
        encoding="utf-8"
    ).strip():
        raise FixtureRegressionError("source_boundary", "repair report is missing or empty")
    return changed


def validate_agent_result(document: Mapping[str, object], changed_files: Sequence[str]) -> None:
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
        or model.get("provider") != "openai"
        or model.get("adapter") != "responses"
        or streamed_calls is None
        or streamed_calls < 1
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
        "config_sha256": sha256_file(args.config),
        "fixture_sha256": inventory_digest(inventory),
        "profile": safe_profile_report(inspection),
        "status": "passed",
    }


def execute(args: argparse.Namespace) -> int:
    if args.output is not None and args.output.exists():
        raise FixtureRegressionError(
            "evidence", f"refusing to replace evidence file: {args.output}"
        )
    inventory = validate_fixture(args.fixture)
    inspection = inspect_provider(args.mint, args.config)
    report = base_report(args, inspection, inventory)
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
        with tempfile.TemporaryDirectory(prefix="mint-fixture-regression-") as directory:
            temporary = Path(directory)
            project = temporary / "project"
            shutil.copytree(args.fixture, project)
            report["baseline"] = run_baseline(project)
            agent_exit, agent_result = run_agent(
                args.mint, args.config, project, temporary / "runtime"
            )
            changed = changed_sources(inventory, project)
            report["agent"] = safe_agent_report(agent_result, changed)
            if agent_exit != 0:
                raise FixtureRegressionError(
                    "agent", "mint returned a non-zero exit code", {"exit_code": agent_exit}
                )
            validate_agent_result(agent_result, changed)
            report["independent_verification"] = independent_verification(
                project, temporary / "verification-build"
            )
    except FixtureRegressionError as error:
        report["status"] = "failed"
        report["failed_stage"] = error.stage
        report.update(error.details)
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
