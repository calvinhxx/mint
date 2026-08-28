#!/usr/bin/env python3
"""Run the committed provider compatibility matrix without persisting raw replies."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Mapping, Sequence

from release_evidence import EvidenceError, release_source_digest


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "configs" / "provider-regression.json"
PROFILE_ID = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
SAFE_REPORT_SCHEMA = {
    "provider": None,
    "source": None,
    "adapter": None,
    "endpoint": None,
    "model": None,
    "stream": None,
    "authentication": None,
    "api_key_env": None,
    "capabilities": {
        "function_tools": None,
        "streaming": None,
        "stream_usage": None,
        "stateless_reasoning_replay": None,
        "token_limit_parameter": None,
        "explicit_tool_choice": None,
        "chat_reasoning_replay": None,
        "requires_tool_call_content": None,
    },
    "limits": {"max_completion_tokens": None, "max_attempts_per_request": None},
    "acceptance": {
        "requests": None,
        "attempts": None,
        "retries": None,
        "duration_ms": None,
        "streamed_requests": None,
        "stream_events": None,
        "streamed_bytes": None,
        "reported_provider": None,
        "reported_adapter": None,
        "reported_model": None,
        "usage": {
            "reported_requests": None,
            "prompt_tokens": None,
            "completion_tokens": None,
            "total_tokens": None,
            "cached_tokens": None,
        },
        "checks": {
            "function_call": None,
            "arguments_round_trip": None,
            "tool_result_continuation": None,
        },
    },
}


class RegressionError(RuntimeError):
    pass


@dataclass(frozen=True)
class Profile:
    id: str
    config: Path


def load_manifest(path: Path) -> list[Profile]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RegressionError(f"cannot read provider manifest: {path}") from error

    if document.get("schema_version") != 1 or not isinstance(document.get("profiles"), list):
        raise RegressionError("provider manifest must use schema_version 1 and contain profiles")

    root = path.resolve().parent
    profiles: list[Profile] = []
    ids: set[str] = set()
    paths: set[Path] = set()
    for item in document["profiles"]:
        if not isinstance(item, dict) or set(item) != {"id", "config"}:
            raise RegressionError("each provider profile must contain only id and config")
        profile_id = item["id"]
        config_name = item["config"]
        if not isinstance(profile_id, str) or PROFILE_ID.fullmatch(profile_id) is None:
            raise RegressionError(f"invalid provider profile id: {profile_id!r}")
        if not isinstance(config_name, str) or not config_name:
            raise RegressionError(f"invalid config path for provider profile: {profile_id}")

        config = (root / config_name).resolve()
        try:
            config.relative_to(root)
        except ValueError as error:
            raise RegressionError(f"provider config escapes manifest directory: {config_name}") from error
        if not config.is_file():
            raise RegressionError(f"provider config does not exist: {config_name}")
        if profile_id in ids or config in paths:
            raise RegressionError(f"duplicate provider profile: {profile_id}")
        ids.add(profile_id)
        paths.add(config)
        profiles.append(Profile(profile_id, config))

    if not profiles:
        raise RegressionError("provider manifest has no profiles")
    return profiles


def run_json(command: Sequence[str]) -> tuple[int, dict]:
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True)
    except OSError as error:
        raise RegressionError(f"cannot start mint executable: {command[0]}") from error
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError:
        document = {}
    return result.returncode, document if isinstance(document, dict) else {}


def run_provider(executable: Path, profile: Profile, live: bool = False) -> tuple[int, dict]:
    command = [str(executable), "provider"]
    if live:
        command.append("test")
    command.extend(
        ["--config", str(profile.config), "--json", "--log-level", "off"]
    )
    return run_json(command)


def matrix_digest(manifest: Path, profiles: Sequence[Profile]) -> str:
    digest = hashlib.sha256()
    for path in (manifest, *(profile.config for profile in profiles)):
        content = path.read_bytes()
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def mint_version(executable: Path) -> str:
    try:
        result = subprocess.run(
            [str(executable), "--version"], check=False, capture_output=True, text=True
        )
    except OSError as error:
        raise RegressionError(f"cannot start mint executable: {executable}") from error
    match = re.fullmatch(r"mint ([0-9]+\.[0-9]+\.[0-9]+)\s*", result.stdout)
    if result.returncode != 0 or match is None:
        raise RegressionError("mint --version returned an unexpected result")
    return match.group(1)


def allowlisted(document: Mapping[str, object], schema: Mapping[str, object]) -> dict:
    result = {}
    for field, nested_schema in schema.items():
        if field not in document:
            continue
        value = document[field]
        if nested_schema is None:
            if value is None or isinstance(value, (bool, int, float, str)):
                result[field] = value
        elif isinstance(value, Mapping):
            result[field] = allowlisted(value, nested_schema)
    return result


def safe_report(profile_id: str, report: Mapping[str, object], status: str) -> dict:
    return {"id": profile_id, "status": status, **allowlisted(report, SAFE_REPORT_SCHEMA)}


def missing_credentials(
    reports: Sequence[Mapping[str, object]], environment: Mapping[str, str]
) -> list[str]:
    required = {
        value
        for report in reports
        if isinstance((value := report.get("api_key_env")), str) and value
    }
    return sorted(name for name in required if not environment.get(name))


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
        raise RegressionError(f"refusing to replace existing evidence file: {path}") from error
    except OSError as error:
        path.unlink(missing_ok=True)
        raise RegressionError(f"cannot write evidence file: {path}") from error


def execute(args: argparse.Namespace) -> int:
    if args.output is not None and args.output.exists():
        raise RegressionError(f"refusing to replace existing evidence file: {args.output}")
    profiles = load_manifest(args.manifest)
    version = mint_version(args.mint)
    inspections: list[dict] = []
    for profile in profiles:
        exit_code, report = run_provider(args.mint, profile)
        if exit_code != 0 or report.get("operation") != "inspect":
            raise RegressionError(f"offline inspection failed for {profile.id}")
        inspections.append(report)

    result = {
        "schema_version": 1,
        "operation": "provider_regression",
        "mode": "live" if args.live else "inspect",
        "generated_at_utc": datetime.now(timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z"),
        "mint_version": version,
        "matrix_sha256": matrix_digest(args.manifest, profiles),
        "status": "passed",
        "profiles": [
            safe_report(profile.id, report, "inspected")
            for profile, report in zip(profiles, inspections)
        ],
    }
    if not args.live:
        write_report(args.output, result)
        return 0

    missing = missing_credentials(inspections, os.environ)
    if missing:
        result["status"] = "blocked"
        result["missing_environment"] = missing
        write_report(args.output, result)
        return 2

    try:
        result["source_sha256"] = release_source_digest(ROOT)
    except EvidenceError as error:
        raise RegressionError(str(error)) from error

    live_reports: list[dict] = []
    for index, (profile, inspection) in enumerate(zip(profiles, inspections)):
        exit_code, report = run_provider(args.mint, profile, live=True)
        if exit_code != 0 or report.get("status") != "passed":
            failure = safe_report(profile.id, inspection, "failed")
            failure["exit_code"] = exit_code
            live_reports.append(failure)
            live_reports.extend(
                safe_report(pending.id, pending_report, "not_run")
                for pending, pending_report in zip(
                    profiles[index + 1 :], inspections[index + 1 :]
                )
            )
            result["status"] = "failed"
            break
        live_reports.append(safe_report(profile.id, report, "passed"))

    result["profiles"] = live_reports
    write_report(args.output, result)
    return 0 if result["status"] == "passed" else 1


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mint", type=Path, required=True, help="path to the mint executable")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--live", action="store_true", help="send the compatibility requests")
    parser.add_argument("--output", type=Path, help="new JSON evidence file; never overwritten")
    args = parser.parse_args(argv)
    if args.live and args.output is None:
        parser.error("--live requires --output so the sanitized result is retained")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    try:
        return execute(parse_args(argv))
    except RegressionError as error:
        print(f"provider regression: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
