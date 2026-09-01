"""Validate the sanitized live evidence required by a mint release."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Mapping, Sequence

from provider_matrix import (
    ProviderMatrixError,
    ProviderProfile,
    load_provider_matrix,
    provider_matrix_digest,
    profile_by_id,
)


EVIDENCE_FILENAMES = ("provider-regression.json", "fixture-regression.json")
RELEASE_METADATA = {
    "CHANGELOG.md",
    "docs/development/testing.md",
    "docs/project/roadmap.md",
}
EVIDENCE_PREFIX = "release/evidence/"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
TIMESTAMP = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$")
UNSAFE_FIELDS = {
    "answer",
    "api_key",
    "config",
    "debug",
    "debug_prompt",
    "diff",
    "error",
    "events",
    "last_response_id",
    "private_dialect",
    "raw",
    "raw_response",
    "request_body",
    "response_body",
    "response_id",
    "secret",
    "session",
    "stderr",
    "stdout",
    "prompt",
    "unified_diff",
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
EXECUTION_FIELDS = {
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
    "last_command_outcome",
    "last_command_verification_eligible",
}
MODEL_FIELDS = {
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
    "adapter",
    "provider",
    "model",
    "max_request_tokens",
    "max_request_tokens_source",
    "response_header_max_request_tokens",
    "request_token_estimate_bytes_per_token",
}
OPTIONAL_MODEL_FIELDS = {"cache_hit_rate"}
PROVIDER_PROFILE_FIELDS = {
    "id",
    "status",
    "provider",
    "source",
    "adapter",
    "endpoint",
    "model",
    "stream",
    "authentication",
    "api_key_env",
    "capabilities",
    "limits",
    "acceptance",
}
CAPABILITY_FIELDS = {
    "function_tools",
    "streaming",
    "stream_usage",
    "stateless_reasoning_replay",
    "token_limit_parameter",
    "explicit_tool_choice",
    "chat_reasoning_replay",
    "requires_tool_call_content",
}
REQUEST_LIMIT_FIELDS = {
    "max_request_tokens",
    "max_request_tokens_source",
    "response_header_max_request_tokens",
    "request_token_safety_margin",
    "request_token_estimate_bytes_per_token",
    "max_completion_tokens",
    "max_attempts_per_request",
}
ACCEPTANCE_FIELDS = {
    "requests",
    "attempts",
    "retries",
    "duration_ms",
    "streamed_requests",
    "stream_events",
    "streamed_bytes",
    "reported_provider",
    "reported_adapter",
    "reported_model",
    "usage",
    "checks",
}
USAGE_FIELDS = {
    "reported_requests",
    "prompt_tokens",
    "completion_tokens",
    "total_tokens",
    "cached_tokens",
}
OPTIONAL_USAGE_FIELDS = {"cache_hit_rate"}
CHECK_FIELDS = {"function_call", "arguments_round_trip", "tool_result_continuation"}
BOOLEAN_CAPABILITY_FIELDS = CAPABILITY_FIELDS - {"token_limit_parameter"}
ACCEPTANCE_COUNTER_FIELDS = {
    "requests",
    "attempts",
    "retries",
    "duration_ms",
    "streamed_requests",
    "stream_events",
    "streamed_bytes",
}
PROVIDER_NAMES = {
    "custom",
    "openai",
    "anthropic",
    "google",
    "xai",
    "moonshot",
    "groq",
    "deepseek",
}
ADAPTER_NAMES = {"chat_completions", "responses", "anthropic_messages"}
PROFILE_SOURCES = {"config", "endpoint", "compatibility_default"}
TOKEN_LIMIT_PARAMETERS = {"max_completion_tokens", "max_tokens", "max_output_tokens"}
REQUEST_LIMIT_SOURCES = {"automatic", "config", "response_header"}
ENVIRONMENT_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,127}$")


class EvidenceError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def _object(value: object, label: str) -> Mapping[str, object]:
    _require(isinstance(value, Mapping), f"{label} must be an object")
    return value


def _array(value: object, label: str) -> Sequence[object]:
    _require(isinstance(value, list), f"{label} must be an array")
    return value


def _exact_fields(document: Mapping[str, object], expected: set[str], label: str) -> None:
    missing = expected - set(document)
    extra = set(document) - expected
    _require(not missing, f"{label} is missing fields: {', '.join(sorted(missing))}")
    _require(not extra, f"{label} has unsupported fields: {', '.join(sorted(extra))}")


def _allowed_fields(document: Mapping[str, object], allowed: set[str], label: str) -> None:
    extra = set(document) - allowed
    _require(not extra, f"{label} has unsupported fields: {', '.join(sorted(extra))}")


def _required_fields(document: Mapping[str, object], required: set[str], label: str) -> None:
    missing = required - set(document)
    _require(not missing, f"{label} is missing fields: {', '.join(sorted(missing))}")


def _non_negative_integer(value: object, label: str) -> int:
    _require(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0,
        f"{label} must be a non-negative integer",
    )
    return value


def _positive_integer(value: object, label: str) -> int:
    result = _non_negative_integer(value, label)
    _require(result > 0, f"{label} must be a positive integer")
    return result


def _boolean(value: object, label: str) -> bool:
    _require(isinstance(value, bool), f"{label} must be a boolean")
    return value


def _non_empty_string(value: object, label: str) -> str:
    _require(isinstance(value, str) and bool(value), f"{label} must be a non-empty string")
    return value


def _enum_string(value: object, allowed: set[str], label: str) -> str:
    result = _non_empty_string(value, label)
    _require(result in allowed, f"{label} is invalid")
    return result


def _optional_positive_integer(value: object, label: str) -> int | None:
    if value is None:
        return None
    return _positive_integer(value, label)


def _validate_cache_hit_rate(document: Mapping[str, object], label: str) -> None:
    if "cache_hit_rate" not in document:
        return
    prompt_tokens = _non_negative_integer(document.get("prompt_tokens"), f"{label} prompt tokens")
    cached_tokens = _non_negative_integer(document.get("cached_tokens"), f"{label} cached tokens")
    _require(cached_tokens <= prompt_tokens, f"{label} cached tokens exceed prompt tokens")
    rate = document.get("cache_hit_rate")
    if prompt_tokens == 0:
        _require(rate is None, f"{label} cache hit rate must be null without prompt tokens")
        return
    _require(
        isinstance(rate, (int, float)) and not isinstance(rate, bool),
        f"{label} cache hit rate must be numeric",
    )
    expected = cached_tokens / prompt_tokens
    _require(
        0.0 <= float(rate) <= 1.0 and abs(float(rate) - expected) <= 1e-12,
        f"{label} cache hit rate is inconsistent",
    )


def _validate_request_limits(
    limits: Mapping[str, object], label: str, completion_field: str, attempts_field: str
) -> None:
    maximum = _positive_integer(limits.get("max_request_tokens"), f"{label} maximum")
    source = _enum_string(
        limits.get("max_request_tokens_source"), REQUEST_LIMIT_SOURCES, f"{label} source"
    )
    response_header = _optional_positive_integer(
        limits.get("response_header_max_request_tokens"), f"{label} response header maximum"
    )
    safety_margin = _non_negative_integer(
        limits.get("request_token_safety_margin"), f"{label} safety margin"
    )
    estimate = _positive_integer(
        limits.get("request_token_estimate_bytes_per_token"),
        f"{label} estimate bytes per token",
    )
    _require(estimate <= 8, f"{label} estimate bytes per token is invalid")
    completion = _positive_integer(limits.get(completion_field), f"{label} completion maximum")
    _positive_integer(limits.get(attempts_field), f"{label} attempt maximum")
    _require(
        safety_margin < maximum and completion < maximum - safety_margin,
        f"{label} cannot reserve the entire request budget",
    )
    if source == "response_header":
        _require(
            response_header == maximum,
            f"{label} response-header source does not match its effective maximum",
        )


def _load_json(path: Path, label: str) -> Mapping[str, object]:
    try:
        return _object(json.loads(path.read_text(encoding="utf-8")), label)
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read {label}: {path}") from error


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(64 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise EvidenceError(f"cannot hash {path}") from error
    return digest.hexdigest()


def _provider_profiles(root: Path) -> tuple[Path, list[ProviderProfile]]:
    manifest_path = root / "configs" / "provider-regression.json"
    try:
        return manifest_path, load_provider_matrix(manifest_path)
    except ProviderMatrixError as error:
        raise EvidenceError(str(error)) from error


def _provider_matrix(root: Path) -> tuple[list[str], str]:
    manifest, profiles = _provider_profiles(root)
    try:
        digest = provider_matrix_digest(manifest, profiles)
    except ProviderMatrixError as error:
        raise EvidenceError(str(error)) from error
    return [profile.id for profile in profiles], digest


def _fixture_digest(root: Path) -> str:
    fixture = root / "tests" / "fixtures" / "broken_cpp_project"
    inventory: dict[str, str] = {}
    for path in sorted(fixture.rglob("*")):
        relative = path.relative_to(fixture)
        if relative.parts and relative.parts[0] == "build":
            continue
        _require(not path.is_symlink(), "release fixture must not contain symlinks")
        if path.is_file():
            inventory[relative.as_posix()] = _file_sha256(path)
    _require(
        set(inventory)
        == {
            "CMakeLists.txt",
            "README.md",
            "include/calculator.hpp",
            "policy.json",
            "src/calculator.cpp",
            "tests/calculator_tests.cpp",
        },
        "release fixture inventory is unexpected",
    )
    digest = hashlib.sha256()
    for name, content_digest in sorted(inventory.items()):
        encoded_name = name.encode("utf-8")
        digest.update(len(encoded_name).to_bytes(8, "big"))
        digest.update(encoded_name)
        digest.update(bytes.fromhex(content_digest))
    return digest.hexdigest()


def _fixture_policy_limits(root: Path) -> dict[str, int]:
    policy = _load_json(
        root / "tests" / "fixtures" / "broken_cpp_project" / "policy.json",
        "release fixture policy",
    )
    limits: dict[str, int] = {}
    for field in ("max_turns", "max_context_bytes", "max_seconds"):
        value = policy.get(field)
        _require(
            isinstance(value, int) and not isinstance(value, bool) and value > 0,
            f"release fixture policy {field} is invalid",
        )
        limits[field] = value
    return limits


def require_release_source_tree(root: Path) -> None:
    """Require all functional release inputs to be committed before live validation."""
    commands = (
        ["git", "-C", str(root), "diff", "--name-only", "-z"],
        ["git", "-C", str(root), "diff", "--cached", "--name-only", "-z"],
        ["git", "-C", str(root), "ls-files", "--others", "--exclude-standard", "-z"],
    )
    changed: set[str] = set()
    for command in commands:
        try:
            result = subprocess.run(command, check=False, capture_output=True)
        except OSError as error:
            raise EvidenceError("cannot inspect git state for live release evidence") from error
        _require(result.returncode == 0, "cannot inspect git state for live release evidence")
        changed.update(os.fsdecode(value) for value in result.stdout.split(b"\0") if value)

    functional = sorted(
        name
        for name in changed
        if name not in RELEASE_METADATA and not name.startswith(EVIDENCE_PREFIX)
    )
    _require(
        not functional,
        "live release evidence requires committed functional sources: "
        + ", ".join(functional[:8]),
    )


def release_source_digest(root: Path) -> str:
    """Hash tracked release inputs, excluding metadata written after live validation."""
    try:
        tracked_result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"], check=False, capture_output=True
        )
        deleted_result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--deleted", "-z"],
            check=False,
            capture_output=True,
        )
    except OSError as error:
        raise EvidenceError("cannot start git while hashing release sources") from error
    _require(
        tracked_result.returncode == 0 and deleted_result.returncode == 0,
        "cannot list tracked release sources",
    )

    deleted = {
        os.fsdecode(value) for value in deleted_result.stdout.split(b"\0") if value
    }

    digest = hashlib.sha256()
    included = 0
    tracked = (
        os.fsdecode(value) for value in tracked_result.stdout.split(b"\0") if value
    )
    for name in sorted(tracked):
        if name in deleted:
            continue
        if name in RELEASE_METADATA or name.startswith(EVIDENCE_PREFIX):
            continue
        path = root / name
        _require(
            path.is_file() and not path.is_symlink(),
            f"tracked release source is invalid: {name}",
        )
        try:
            content = path.read_bytes()
        except OSError as error:
            raise EvidenceError(f"cannot read tracked release source: {name}") from error
        for value in (name.encode("utf-8"), content):
            digest.update(len(value).to_bytes(8, "big"))
            digest.update(value)
        included += 1
    _require(included > 0, "release source inventory is empty")
    return digest.hexdigest()


def _reject_unsafe_fields(value: object, label: str) -> None:
    if isinstance(value, Mapping):
        unsafe = UNSAFE_FIELDS & set(value)
        _require(not unsafe, f"{label} has unsupported fields: {', '.join(sorted(unsafe))}")
        for nested in value.values():
            _reject_unsafe_fields(nested, label)
    elif isinstance(value, list):
        for nested in value:
            _reject_unsafe_fields(nested, label)


def _validate_common(
    document: Mapping[str, object], operation: str, version: str, source_digest: str, label: str
) -> None:
    _require(document.get("schema_version") == 1, f"{label} schema is unsupported")
    _require(document.get("operation") == operation, f"{label} operation is invalid")
    _require(document.get("mode") == "live", f"{label} is not a live result")
    timestamp = document.get("generated_at_utc")
    _require(
        isinstance(timestamp, str) and TIMESTAMP.fullmatch(timestamp),
        f"{label} timestamp is invalid",
    )
    _require(document.get("mint_version") == version, f"{label} version mismatch")
    source = document.get("source_sha256")
    _require(
        isinstance(source, str) and SHA256.fullmatch(source),
        f"{label} source digest is invalid",
    )
    _require(source == source_digest, f"{label} source digest is stale")
    _require(document.get("status") == "passed", f"{label} did not pass")
    _reject_unsafe_fields(document, label)


def _validate_provider(
    document: Mapping[str, object], root: Path, version: str, source_digest: str
) -> dict[str, Mapping[str, object]]:
    label = "provider evidence"
    _exact_fields(
        document,
        {
            "schema_version",
            "operation",
            "mode",
            "generated_at_utc",
            "mint_version",
            "source_sha256",
            "matrix_sha256",
            "status",
            "profiles",
        },
        label,
    )
    _validate_common(document, "provider_regression", version, source_digest, label)
    expected_ids, matrix_digest = _provider_matrix(root)
    _require(document["matrix_sha256"] == matrix_digest, f"{label} provider matrix is stale")
    profiles = _array(document["profiles"], f"{label} profiles")
    reported_ids = [_object(item, label).get("id") for item in profiles]
    _require(reported_ids == expected_ids, f"{label} profile list is invalid")
    live_profiles: dict[str, Mapping[str, object]] = {}
    for item in profiles:
        profile = _object(item, label)
        _allowed_fields(profile, PROVIDER_PROFILE_FIELDS, f"{label} profile")
        status = profile.get("status")
        _require(status in {"inspected", "passed"}, f"{label} profile status is invalid")
        expected_profile_fields = (
            PROVIDER_PROFILE_FIELDS
            if status == "passed"
            else PROVIDER_PROFILE_FIELDS - {"acceptance"}
        )
        _exact_fields(profile, expected_profile_fields, f"{label} profile")
        _non_empty_string(profile.get("id"), f"{label} profile id")
        _enum_string(profile.get("provider"), PROVIDER_NAMES, f"{label} profile provider")
        _enum_string(profile.get("source"), PROFILE_SOURCES, f"{label} profile source")
        _enum_string(profile.get("adapter"), ADAPTER_NAMES, f"{label} profile adapter")
        _non_empty_string(profile.get("endpoint"), f"{label} profile endpoint")
        _non_empty_string(profile.get("model"), f"{label} profile model")
        _boolean(profile.get("stream"), f"{label} profile stream")
        _require(
            profile.get("authentication") == "environment",
            f"{label} profile authentication is invalid",
        )
        api_key_env = _non_empty_string(
            profile.get("api_key_env"), f"{label} profile api key environment"
        )
        _require(
            ENVIRONMENT_NAME.fullmatch(api_key_env) is not None,
            f"{label} profile api key environment is invalid",
        )
        capabilities = _object(profile.get("capabilities"), f"{label} capabilities")
        _exact_fields(capabilities, CAPABILITY_FIELDS, f"{label} capabilities")
        for field in BOOLEAN_CAPABILITY_FIELDS:
            _boolean(capabilities.get(field), f"{label} capability {field}")
        _enum_string(
            capabilities.get("token_limit_parameter"),
            TOKEN_LIMIT_PARAMETERS,
            f"{label} capability token limit parameter",
        )
        limits = _object(profile.get("limits"), f"{label} request limits")
        _exact_fields(limits, REQUEST_LIMIT_FIELDS, f"{label} request limits")
        _validate_request_limits(
            limits, f"{label} request limits", "max_completion_tokens", "max_attempts_per_request"
        )
        if status == "inspected":
            continue
        _require(
            limits.get("max_attempts_per_request") == 1,
            f"{label} live profile must disable retries",
        )
        acceptance = _object(profile.get("acceptance"), f"{label} acceptance")
        _exact_fields(acceptance, ACCEPTANCE_FIELDS, f"{label} acceptance")
        for field in ACCEPTANCE_COUNTER_FIELDS:
            _non_negative_integer(acceptance.get(field), f"{label} acceptance {field}")
        _require(acceptance.get("requests") == 2, f"{label} handshake is incomplete")
        _require(
            acceptance.get("attempts") == 2 and acceptance.get("retries") == 0,
            f"{label} handshake must use two attempts and zero retries",
        )
        expected_streamed = acceptance.get("requests") if profile.get("stream") else 0
        _require(
            acceptance.get("streamed_requests") == expected_streamed,
            f"{label} streamed request count is inconsistent",
        )
        reported_provider = _non_empty_string(
            acceptance.get("reported_provider"), f"{label} reported provider"
        )
        reported_adapter = _non_empty_string(
            acceptance.get("reported_adapter"), f"{label} reported adapter"
        )
        reported_model = _non_empty_string(
            acceptance.get("reported_model"), f"{label} reported model"
        )
        _require(
            reported_provider == profile.get("provider")
            and reported_adapter == profile.get("adapter")
            and reported_model == profile.get("model"),
            f"{label} reported identity does not match its profile",
        )
        checks = _object(acceptance.get("checks"), f"{label} checks")
        _exact_fields(checks, CHECK_FIELDS, f"{label} checks")
        for field in CHECK_FIELDS:
            _boolean(checks.get(field), f"{label} check {field}")
        usage = _object(acceptance.get("usage"), f"{label} usage")
        _required_fields(usage, USAGE_FIELDS, f"{label} usage")
        _allowed_fields(usage, USAGE_FIELDS | OPTIONAL_USAGE_FIELDS, f"{label} usage")
        for field in USAGE_FIELDS:
            _non_negative_integer(usage.get(field), f"{label} usage {field}")
        _validate_cache_hit_rate(usage, f"{label} usage")
        _require(
            usage.get("reported_requests") <= acceptance.get("requests")
            and usage.get("cached_tokens") <= usage.get("prompt_tokens"),
            f"{label} usage counts are inconsistent",
        )
        _require(
            checks
            == {
                "function_call": True,
                "arguments_round_trip": True,
                "tool_result_continuation": True,
            },
            f"{label} tool checks failed",
        )
        live_profiles[str(profile["id"])] = profile
    _require(live_profiles, f"{label} has no live profile")
    return live_profiles


def _validate_fixture(
    document: Mapping[str, object],
    root: Path,
    version: str,
    source_digest: str,
    live_profiles: Mapping[str, Mapping[str, object]],
) -> None:
    label = "fixture evidence"
    fields = {
        "schema_version",
        "operation",
        "mode",
        "generated_at_utc",
        "mint_version",
        "source_sha256",
        "profile_id",
        "config_sha256",
        "fixture_sha256",
        "profile",
        "limits",
        "status",
        "baseline",
        "agent",
        "independent_verification",
    }
    if "command_sandbox_preflight" in document:
        fields.add("command_sandbox_preflight")
    _exact_fields(
        document,
        fields,
        label,
    )
    _validate_common(document, "fixture_regression", version, source_digest, label)
    profile_id = document["profile_id"]
    _require(isinstance(profile_id, str), f"{label} profile id is invalid")
    _require(profile_id in live_profiles, f"{label} profile has no live handshake")
    _, committed_profiles = _provider_profiles(root)
    try:
        committed_profile = profile_by_id(committed_profiles, profile_id)
    except ProviderMatrixError as error:
        raise EvidenceError(str(error)) from error
    _require(
        document["config_sha256"] == _file_sha256(committed_profile.config),
        f"{label} config is stale",
    )
    _require(document["fixture_sha256"] == _fixture_digest(root), f"{label} fixture is stale")

    limits = _object(document["limits"], f"{label} limits")
    _exact_fields(
        limits,
        {
            "max_turns",
            "max_context_bytes",
            "max_context_estimated_tokens",
            "max_request_tokens",
            "max_request_tokens_source",
            "response_header_max_request_tokens",
            "request_token_safety_margin",
            "request_token_estimate_bytes_per_token",
            "max_completion_tokens_per_request",
            "max_attempts_per_request",
            "max_seconds",
        },
        f"{label} limits",
    )
    _validate_request_limits(
        limits,
        f"{label} request limits",
        "max_completion_tokens_per_request",
        "max_attempts_per_request",
    )
    policy_limits = _fixture_policy_limits(root)
    _require(
        limits.get("max_turns") == policy_limits["max_turns"],
        f"{label} turn limit is invalid",
    )
    _require(
        limits.get("max_context_bytes") == policy_limits["max_context_bytes"],
        f"{label} context limit is invalid",
    )
    _require(
        limits.get("max_context_estimated_tokens")
        == (
            policy_limits["max_context_bytes"]
            + limits.get("request_token_estimate_bytes_per_token", 0)
            - 1
        )
        // max(limits.get("request_token_estimate_bytes_per_token", 0), 1),
        f"{label} context token estimate is invalid",
    )
    _require(
        limits.get("max_seconds") == policy_limits["max_seconds"],
        f"{label} time limit is invalid",
    )
    profile = _object(document["profile"], f"{label} profile")
    _exact_fields(
        profile,
        {"provider", "adapter", "endpoint", "model", "stream", "api_key_env"},
        f"{label} profile",
    )
    _enum_string(profile.get("provider"), PROVIDER_NAMES, f"{label} profile provider")
    _enum_string(profile.get("adapter"), ADAPTER_NAMES, f"{label} profile adapter")
    for field in ("endpoint", "model", "api_key_env"):
        _non_empty_string(profile.get(field), f"{label} profile {field}")
    _boolean(profile.get("stream"), f"{label} profile stream")
    live_profile = live_profiles[profile_id]
    identity_fields = ("provider", "adapter", "endpoint", "model", "stream", "api_key_env")
    _require(
        all(profile.get(field) == live_profile.get(field) for field in identity_fields),
        f"{label} profile does not match the live handshake",
    )
    live_limits = _object(live_profile.get("limits"), f"{label} live request limits")
    for field in ("request_token_safety_margin", "request_token_estimate_bytes_per_token"):
        _require(
            limits.get(field) == live_limits.get(field),
            f"{label} request configuration does not match the live handshake",
        )
    _require(
        document["baseline"]
        == {"configure": "passed", "build": "passed", "test": "failed_as_expected"},
        f"{label} did not prove the failing baseline",
    )
    _require(
        document["independent_verification"]
        == {"configure": "passed", "build": "passed", "test": "passed"},
        f"{label} independent verification failed",
    )
    if "command_sandbox_preflight" in document:
        sandbox = _object(
            document["command_sandbox_preflight"], f"{label} command sandbox preflight"
        )
        _exact_fields(
            sandbox, {"status", "backend"}, f"{label} command sandbox preflight"
        )
        _require(
            sandbox == {"status": "passed", "backend": "macos-seatbelt"},
            f"{label} command sandbox preflight failed",
        )

    agent = _object(document["agent"], f"{label} agent")
    _exact_fields(agent, AGENT_FIELDS, f"{label} agent")
    _require(
        agent.get("completed") is True and agent.get("status") == "completed",
        f"{label} agent did not complete",
    )
    _require(agent.get("stop_reason") is None, f"{label} agent stopped unexpectedly")
    _require(agent.get("verification_status") == "passed", f"{label} verification did not pass")
    _non_negative_integer(agent.get("turns"), f"{label} agent turns")
    _non_negative_integer(agent.get("duration_ms"), f"{label} agent duration")
    changes = _object(agent.get("changes"), f"{label} changes")
    _exact_fields(changes, {"files"}, f"{label} changes")
    changed_files = _array(changes.get("files"), f"{label} changed files")
    _require(
        all(isinstance(path, str) for path in changed_files),
        f"{label} changed files are invalid",
    )
    _require(
        changes == {"files": ["FIX_REPORT.md", "src/calculator.cpp"]},
        f"{label} changed files outside the fixture policy",
    )
    execution = _object(agent.get("execution"), f"{label} execution")
    _exact_fields(execution, EXECUTION_FIELDS, f"{label} execution")
    for field, value in execution.items():
        if field == "last_command_outcome":
            _enum_string(
                value,
                {"not_run", "passed", "failed", "timed_out", "cancelled", "denied"},
                f"{label} execution {field}",
            )
        elif field == "last_command_verification_eligible":
            _boolean(value, f"{label} execution {field}")
        else:
            _non_negative_integer(value, f"{label} execution {field}")
    _require(execution.get("verification_commands", 0) > 0, f"{label} ran no verification command")
    _require(
        0 < execution.get("last_file_change_call", 0)
        < execution.get("last_command_call", 0)
        <= execution.get("tool_calls", 0),
        f"{label} final command ordering is invalid",
    )
    _require(
        execution.get("last_command_outcome") == "passed"
        and execution.get("last_command_verification_eligible") is True,
        f"{label} final command did not verify the latest changes",
    )
    model = _object(agent.get("model"), f"{label} model")
    _required_fields(model, MODEL_FIELDS, f"{label} model")
    _allowed_fields(model, MODEL_FIELDS | OPTIONAL_MODEL_FIELDS, f"{label} model")
    for field, value in model.items():
        if field in {"adapter", "provider", "model", "max_request_tokens_source"}:
            _require(isinstance(value, str) and bool(value), f"{label} model {field} is invalid")
        elif field == "response_header_max_request_tokens" and value is None:
            continue
        elif field == "cache_hit_rate":
            continue
        else:
            _non_negative_integer(value, f"{label} model {field}")
    _validate_cache_hit_rate(model, f"{label} model")
    for field in ("provider", "adapter", "model"):
        _require(field in model, f"{label} model {field} is missing")
    acceptance = _object(live_profile.get("acceptance"), f"{label} live acceptance")
    accepted_models = {profile.get("model"), acceptance.get("reported_model")}
    _require(
        model.get("provider") == profile.get("provider")
        and model.get("adapter") == profile.get("adapter")
        and model.get("model") in accepted_models,
        f"{label} agent profile is invalid",
    )
    for field in (
        "max_request_tokens",
        "max_request_tokens_source",
        "response_header_max_request_tokens",
        "request_token_estimate_bytes_per_token",
    ):
        _require(
            model.get(field) == limits.get(field),
            f"{label} agent request budget does not match its runtime limits",
        )
    _validate_request_limits(
        {
            "max_request_tokens": model.get("max_request_tokens"),
            "max_request_tokens_source": model.get("max_request_tokens_source"),
            "response_header_max_request_tokens": model.get(
                "response_header_max_request_tokens"
            ),
            "request_token_safety_margin": limits.get("request_token_safety_margin"),
            "request_token_estimate_bytes_per_token": model.get(
                "request_token_estimate_bytes_per_token"
            ),
            "max_completion_tokens_per_request": limits.get(
                "max_completion_tokens_per_request"
            ),
            "max_attempts_per_request": limits.get("max_attempts_per_request"),
        },
        f"{label} agent request limits",
        "max_completion_tokens_per_request",
        "max_attempts_per_request",
    )
    streamed = model.get("streamed_calls", 0)
    _require(isinstance(streamed, int), f"{label} streamed call count is invalid")
    _require(
        (streamed > 0) == (profile.get("stream") is True),
        f"{label} streaming evidence does not match the profile",
    )


def validate_release_evidence(
    root: Path, version: str, evidence_dir: Path | None = None
) -> Path:
    evidence_dir = evidence_dir or root / "release" / "evidence" / f"v{version}"
    provider = _load_json(evidence_dir / EVIDENCE_FILENAMES[0], "provider evidence")
    fixture = _load_json(evidence_dir / EVIDENCE_FILENAMES[1], "fixture evidence")
    source_digest = release_source_digest(root)
    live_profiles = _validate_provider(provider, root, version, source_digest)
    _validate_fixture(fixture, root, version, source_digest, live_profiles)
    return evidence_dir
