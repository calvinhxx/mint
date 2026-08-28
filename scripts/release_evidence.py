"""Validate the sanitized live evidence required by a mint release."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Mapping, Sequence


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
    "response_id",
    "secret",
    "unified_diff",
}


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


def _ordered_file_digest(paths: Sequence[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        try:
            content = path.read_bytes()
        except OSError as error:
            raise EvidenceError(f"cannot hash {path}") from error
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def _provider_matrix(root: Path) -> tuple[list[str], str]:
    manifest_path = root / "configs" / "provider-regression.json"
    manifest = _load_json(manifest_path, "provider manifest")
    profiles = _array(manifest.get("profiles"), "provider manifest profiles")
    ids: list[str] = []
    paths: list[Path] = []
    for index, value in enumerate(profiles):
        profile = _object(value, f"provider manifest profile {index}")
        _exact_fields(profile, {"id", "config"}, f"provider manifest profile {index}")
        profile_id, config_name = profile["id"], profile["config"]
        _require(isinstance(profile_id, str), f"provider manifest profile {index} has no id")
        _require(isinstance(config_name, str), f"provider manifest profile {index} has no config")
        config_path = (manifest_path.parent / config_name).resolve()
        try:
            config_path.relative_to(manifest_path.parent.resolve())
        except ValueError as error:
            raise EvidenceError("provider config escapes the manifest directory") from error
        _require(config_path.is_file(), f"provider config does not exist: {config_name}")
        ids.append(profile_id)
        paths.append(config_path)
    return ids, _ordered_file_digest([manifest_path, *paths])


def _fixture_digest(root: Path) -> str:
    fixture = root / "tests" / "fixtures" / "v1_broken_project"
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
            "policy.v1_2.json",
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


def release_source_digest(root: Path) -> str:
    """Hash tracked release inputs, excluding metadata written after live validation."""
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"], check=False, capture_output=True
        )
    except OSError as error:
        raise EvidenceError("cannot start git while hashing release sources") from error
    _require(result.returncode == 0, "cannot list tracked release sources")

    digest = hashlib.sha256()
    included = 0
    for name in sorted(os.fsdecode(value) for value in result.stdout.split(b"\0") if value):
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
) -> None:
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
    for item in profiles:
        profile = _object(item, label)
        _require(profile.get("status") == "passed", f"{label} profile did not pass")
        acceptance = _object(profile.get("acceptance"), f"{label} acceptance")
        _require(acceptance.get("requests") == 2, f"{label} handshake is incomplete")
        _require(
            acceptance.get("checks")
            == {
                "function_call": True,
                "arguments_round_trip": True,
                "tool_result_continuation": True,
            },
            f"{label} tool checks failed",
        )


def _validate_fixture(
    document: Mapping[str, object], root: Path, version: str, source_digest: str
) -> None:
    label = "fixture evidence"
    _exact_fields(
        document,
        {
            "schema_version",
            "operation",
            "mode",
            "generated_at_utc",
            "mint_version",
            "source_sha256",
            "config_sha256",
            "fixture_sha256",
            "profile",
            "status",
            "baseline",
            "agent",
            "independent_verification",
        },
        label,
    )
    _validate_common(document, "fixture_regression", version, source_digest, label)
    config = root / "configs" / "providers" / "openai-responses.json"
    _require(document["config_sha256"] == _file_sha256(config), f"{label} config is stale")
    _require(document["fixture_sha256"] == _fixture_digest(root), f"{label} fixture is stale")

    profile = _object(document["profile"], f"{label} profile")
    _require(
        profile.get("provider") == "openai"
        and profile.get("adapter") == "responses"
        and profile.get("stream") is True,
        f"{label} profile is invalid",
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

    agent = _object(document["agent"], f"{label} agent")
    _require(
        agent.get("completed") is True and agent.get("status") == "completed",
        f"{label} agent did not complete",
    )
    _require(agent.get("verification_status") == "passed", f"{label} verification did not pass")
    _require(
        agent.get("changes") == {"files": ["FIX_REPORT.md", "src/calculator.cpp"]},
        f"{label} changed files outside the fixture policy",
    )
    execution = _object(agent.get("execution"), f"{label} execution")
    _require(execution.get("verification_commands", 0) > 0, f"{label} ran no verification command")
    model = _object(agent.get("model"), f"{label} model")
    _require(
        model.get("provider") == "openai" and model.get("adapter") == "responses",
        f"{label} adapter is invalid",
    )
    _require(model.get("streamed_calls", 0) > 0, f"{label} has no SSE evidence")


def validate_release_evidence(
    root: Path, version: str, evidence_dir: Path | None = None
) -> Path:
    evidence_dir = evidence_dir or root / "release" / "evidence" / f"v{version}"
    provider = _load_json(evidence_dir / EVIDENCE_FILENAMES[0], "provider evidence")
    fixture = _load_json(evidence_dir / EVIDENCE_FILENAMES[1], "fixture evidence")
    source_digest = release_source_digest(root)
    _validate_provider(provider, root, version, source_digest)
    _validate_fixture(fixture, root, version, source_digest)
    return evidence_dir
