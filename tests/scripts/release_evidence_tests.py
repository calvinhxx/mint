#!/usr/bin/env python3

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import release_evidence  # noqa: E402


def reports(source_digest: str) -> tuple[dict, dict]:
    ids, matrix_digest = release_evidence._provider_matrix(ROOT)
    live_profile_id = "groq-chat"

    identities = {
        "openai-codex": {
            "provider": "openai",
            "adapter": "responses",
            "endpoint": "https://api.openai.com/v1/responses",
            "model": "gpt-5.3-codex",
            "stream": True,
            "api_key_env": "OPENAI_API_KEY",
            "token_limit_parameter": "max_output_tokens",
            "stream_usage": False,
            "stateless_reasoning_replay": True,
            "explicit_tool_choice": True,
            "chat_reasoning_replay": False,
            "requires_tool_call_content": False,
        },
        "openai-responses": {
            "provider": "openai",
            "adapter": "responses",
            "endpoint": "https://api.openai.com/v1/responses",
            "model": "gpt-5.4-mini-2026-03-17",
            "stream": True,
            "api_key_env": "OPENAI_API_KEY",
            "token_limit_parameter": "max_output_tokens",
            "stream_usage": False,
            "stateless_reasoning_replay": True,
            "explicit_tool_choice": True,
            "chat_reasoning_replay": False,
            "requires_tool_call_content": False,
        },
        "claude-messages": {
            "provider": "anthropic",
            "adapter": "anthropic_messages",
            "endpoint": "https://api.anthropic.com/v1/messages",
            "model": "claude-sonnet-5",
            "stream": True,
            "api_key_env": "ANTHROPIC_API_KEY",
            "token_limit_parameter": "max_tokens",
            "stream_usage": True,
            "stateless_reasoning_replay": False,
            "explicit_tool_choice": True,
            "chat_reasoning_replay": False,
            "requires_tool_call_content": False,
        },
        "gemini-chat": {
            "provider": "google",
            "adapter": "chat_completions",
            "endpoint": "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions",
            "model": "gemini-3.7-flash",
            "stream": True,
            "api_key_env": "GEMINI_API_KEY",
            "token_limit_parameter": "max_completion_tokens",
            "stream_usage": True,
            "stateless_reasoning_replay": False,
            "explicit_tool_choice": True,
            "chat_reasoning_replay": False,
            "requires_tool_call_content": False,
        },
        "grok-responses": {
            "provider": "xai",
            "adapter": "responses",
            "endpoint": "https://api.x.ai/v1/responses",
            "model": "grok-4.6",
            "stream": True,
            "api_key_env": "XAI_API_KEY",
            "token_limit_parameter": "max_output_tokens",
            "stream_usage": False,
            "stateless_reasoning_replay": True,
            "explicit_tool_choice": True,
            "chat_reasoning_replay": False,
            "requires_tool_call_content": False,
        },
        "kimi-chat": {
            "provider": "moonshot",
            "adapter": "chat_completions",
            "endpoint": "https://api.moonshot.ai/v1/chat/completions",
            "model": "kimi-k3",
            "stream": True,
            "api_key_env": "MOONSHOT_API_KEY",
            "token_limit_parameter": "max_completion_tokens",
            "stream_usage": True,
            "stateless_reasoning_replay": False,
            "explicit_tool_choice": True,
            "chat_reasoning_replay": True,
            "requires_tool_call_content": True,
        },
        "groq-chat": {
            "provider": "groq",
            "adapter": "chat_completions",
            "endpoint": "https://api.groq.com/openai/v1/chat/completions",
            "model": "openai/gpt-oss-20b",
            "stream": False,
            "api_key_env": "GROQ_API_KEY",
            "token_limit_parameter": "max_completion_tokens",
            "stream_usage": True,
            "stateless_reasoning_replay": False,
            "explicit_tool_choice": True,
            "chat_reasoning_replay": False,
            "requires_tool_call_content": False,
        },
        "deepseek-chat": {
            "provider": "deepseek",
            "adapter": "chat_completions",
            "endpoint": "https://api.deepseek.com/chat/completions",
            "model": "deepseek-v4-flash",
            "stream": True,
            "api_key_env": "DEEPSEEK_API_KEY",
            "token_limit_parameter": "max_tokens",
            "stream_usage": True,
            "stateless_reasoning_replay": False,
            "explicit_tool_choice": False,
            "chat_reasoning_replay": True,
            "requires_tool_call_content": True,
        },
    }

    def provider_profile(profile_id: str) -> dict:
        identity = identities[profile_id]
        passed = profile_id == live_profile_id
        report = {
            "id": profile_id,
            "provider": identity["provider"],
            "source": "config",
            "adapter": identity["adapter"],
            "endpoint": identity["endpoint"],
            "model": identity["model"],
            "stream": identity["stream"],
            "authentication": "environment",
            "api_key_env": identity["api_key_env"],
            "capabilities": {
                "function_tools": True,
                "streaming": True,
                "stream_usage": identity["stream_usage"],
                "stateless_reasoning_replay": identity["stateless_reasoning_replay"],
                "token_limit_parameter": identity["token_limit_parameter"],
                "explicit_tool_choice": identity["explicit_tool_choice"],
                "chat_reasoning_replay": identity["chat_reasoning_replay"],
                "requires_tool_call_content": identity["requires_tool_call_content"],
            },
            "limits": {
                "max_request_tokens": 8000,
                "max_request_tokens_source": "response_header" if passed else "automatic",
                "response_header_max_request_tokens": 8000 if passed else None,
                "request_token_safety_margin": 256,
                "request_token_estimate_bytes_per_token": 2,
                "max_completion_tokens": 1024 if passed else 2048,
                "max_attempts_per_request": 1 if passed else 3,
            },
            "status": "passed" if passed else "inspected",
        }
        if passed:
            report["acceptance"] = {
                "requests": 2,
                "attempts": 2,
                "retries": 0,
                "duration_ms": 30,
                "streamed_requests": 0,
                "stream_events": 0,
                "streamed_bytes": 0,
                "reported_provider": "groq",
                "reported_adapter": "chat_completions",
                "reported_model": "openai/gpt-oss-20b",
                "usage": {
                    "reported_requests": 2,
                    "prompt_tokens": 100,
                    "completion_tokens": 20,
                    "total_tokens": 120,
                    "cached_tokens": 0,
                    "cache_hit_rate": 0.0,
                },
                "checks": {
                    "function_call": True,
                    "arguments_round_trip": True,
                    "tool_result_continuation": True,
                },
            }
        return report

    provider = {
        "schema_version": 1,
        "operation": "provider_regression",
        "mode": "live",
        "generated_at_utc": "2026-08-29T00:00:00Z",
        "mint_version": "1.5.0",
        "source_sha256": source_digest,
        "matrix_sha256": matrix_digest,
        "status": "passed",
        "profiles": [provider_profile(profile_id) for profile_id in ids],
    }
    config = ROOT / "configs" / "providers" / "groq-chat.json"
    fixture = {
        "schema_version": 1,
        "operation": "fixture_regression",
        "mode": "live",
        "generated_at_utc": "2026-08-29T00:01:00Z",
        "mint_version": "1.5.0",
        "source_sha256": source_digest,
        "profile_id": live_profile_id,
        "config_sha256": release_evidence._file_sha256(config),
        "fixture_sha256": release_evidence._fixture_digest(ROOT),
        "profile": {
            "provider": "groq",
            "adapter": "chat_completions",
            "endpoint": "https://api.groq.com/openai/v1/chat/completions",
            "model": "openai/gpt-oss-20b",
            "stream": False,
            "api_key_env": "GROQ_API_KEY",
        },
        "limits": {
            "max_turns": 16,
            "max_context_bytes": 24576,
            "max_context_estimated_tokens": 12288,
            "max_request_tokens": 8000,
            "max_request_tokens_source": "response_header",
            "response_header_max_request_tokens": 8000,
            "request_token_safety_margin": 256,
            "request_token_estimate_bytes_per_token": 2,
            "max_completion_tokens_per_request": 1024,
            "max_attempts_per_request": 3,
            "max_seconds": 600,
        },
        "status": "passed",
        "command_sandbox_preflight": {"status": "passed", "backend": "macos-seatbelt"},
        "baseline": {"configure": "passed", "build": "passed", "test": "failed_as_expected"},
        "agent": {
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
                "provider": "groq",
                "adapter": "chat_completions",
                "model": "openai/gpt-oss-20b",
                "streamed_calls": 0,
                "stream_events": 0,
                "streamed_bytes": 0,
                "duration_ms": 40,
                "max_request_tokens": 8000,
                "max_request_tokens_source": "response_header",
                "response_header_max_request_tokens": 8000,
                "request_token_estimate_bytes_per_token": 2,
            },
            "changes": {"files": ["FIX_REPORT.md", "src/calculator.cpp"]},
        },
        "independent_verification": {"configure": "passed", "build": "passed", "test": "passed"},
    }
    return provider, fixture


class ReleaseEvidenceTests(unittest.TestCase):
    def test_live_evidence_requires_committed_functional_sources(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src.cpp").write_text("one\n", encoding="utf-8")
            (root / "CHANGELOG.md").write_text("draft\n", encoding="utf-8")
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "-c",
                    "user.name=mint test",
                    "-c",
                    "user.email=mint@example.invalid",
                    "commit",
                    "-qm",
                    "fixture",
                ],
                check=True,
            )

            release_evidence.require_release_source_tree(root)
            (root / "CHANGELOG.md").write_text("dated\n", encoding="utf-8")
            release_evidence.require_release_source_tree(root)
            (root / "untracked.cpp").write_text("two\n", encoding="utf-8")
            with self.assertRaisesRegex(
                release_evidence.EvidenceError, "committed functional sources"
            ):
                release_evidence.require_release_source_tree(root)

    def test_source_digest_ignores_only_release_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "release" / "evidence" / "v1.0.0").mkdir(parents=True)
            (root / "src.cpp").write_text("one\n", encoding="utf-8")
            (root / "keep.cpp").write_text("keep\n", encoding="utf-8")
            (root / "CHANGELOG.md").write_text("draft\n", encoding="utf-8")
            evidence = root / "release" / "evidence" / "v1.0.0" / "provider-regression.json"
            evidence.write_text("{}\n", encoding="utf-8")
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)

            before = release_evidence.release_source_digest(root)
            (root / "CHANGELOG.md").write_text("dated\n", encoding="utf-8")
            evidence.write_text('{"status":"passed"}\n', encoding="utf-8")
            self.assertEqual(release_evidence.release_source_digest(root), before)
            (root / "src.cpp").write_text("two\n", encoding="utf-8")
            self.assertNotEqual(release_evidence.release_source_digest(root), before)

            (root / "src.cpp").unlink()
            deleted = release_evidence.release_source_digest(root)
            subprocess.run(["git", "-C", str(root), "add", "-u"], check=True)
            self.assertEqual(release_evidence.release_source_digest(root), deleted)

    def test_complete_live_reports_pass(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)
        provider, fixture = reports(source_digest)
        with tempfile.TemporaryDirectory() as directory:
            evidence_dir = Path(directory)
            (evidence_dir / "provider-regression.json").write_text(
                json.dumps(provider), encoding="utf-8"
            )
            (evidence_dir / "fixture-regression.json").write_text(
                json.dumps(fixture), encoding="utf-8"
            )
            self.assertEqual(
                release_evidence.validate_release_evidence(ROOT, "1.5.0", evidence_dir),
                evidence_dir,
            )

    def test_non_live_failed_or_stale_reports_fail(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)
        provider, fixture = reports(source_digest)
        for field, value, message in (
            ("mode", "inspect", "not a live result"),
            ("status", "failed", "did not pass"),
            ("source_sha256", "0" * 64, "source digest is stale"),
        ):
            invalid = copy.deepcopy(provider)
            invalid[field] = value
            with self.assertRaisesRegex(release_evidence.EvidenceError, message):
                release_evidence._validate_provider(invalid, ROOT, "1.5.0", source_digest)

        fixture["independent_verification"]["test"] = "failed"
        live_profiles = release_evidence._validate_provider(
            provider, ROOT, "1.5.0", source_digest
        )
        with self.assertRaisesRegex(release_evidence.EvidenceError, "verification failed"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )

        provider, fixture = reports(source_digest)
        provider["profiles"][0].pop("provider")
        with self.assertRaisesRegex(release_evidence.EvidenceError, "missing fields: provider"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, fixture = reports(source_digest)
        live_profiles = release_evidence._validate_provider(
            provider, ROOT, "1.5.0", source_digest
        )
        fixture["agent"]["model"].pop("provider")
        with self.assertRaisesRegex(release_evidence.EvidenceError, "missing fields: provider"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )

        _, fixture = reports(source_digest)
        fixture["limits"]["max_turns"] = 13
        with self.assertRaisesRegex(release_evidence.EvidenceError, "turn limit is invalid"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )

        _, fixture = reports(source_digest)
        fixture["command_sandbox_preflight"]["backend"] = "unknown"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "sandbox preflight failed"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )

        _, fixture = reports(source_digest)
        fixture["agent"]["stop_reason"] = "max_turns_exhausted"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "stopped unexpectedly"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )

        _, fixture = reports(source_digest)
        fixture["agent"]["execution"]["last_command_outcome"] = "failed"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "did not verify"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )

    def test_raw_secret_and_incomplete_tool_evidence_fail(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)
        provider, fixture = reports(source_digest)
        provider["profiles"][1]["acceptance"]["raw_response"] = "private"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "unsupported fields"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        valid_provider, _ = reports(source_digest)
        live_profiles = release_evidence._validate_provider(
            valid_provider, ROOT, "1.5.0", source_digest
        )
        fixture["agent"]["secret"] = "private"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "unsupported fields"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )

        for container, field in (
            (None, "stdout"),
            ("execution", "stderr"),
            ("model", "request_body"),
        ):
            _, fixture = reports(source_digest)
            target = fixture["agent"] if container is None else fixture["agent"][container]
            target[field] = "private"
            with self.assertRaisesRegex(release_evidence.EvidenceError, "unsupported fields"):
                release_evidence._validate_fixture(
                    fixture, ROOT, "1.5.0", source_digest, live_profiles
                )

        provider, _ = reports(source_digest)
        provider["profiles"][1]["acceptance"]["checks"]["function_call"] = False
        with self.assertRaisesRegex(release_evidence.EvidenceError, "tool checks failed"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

    def test_live_provider_handshake_requires_two_attempts_without_retries(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)

        provider, _ = reports(source_digest)
        live_profile = next(
            profile for profile in provider["profiles"] if profile["status"] == "passed"
        )
        live_profile["limits"]["max_attempts_per_request"] = 2
        with self.assertRaisesRegex(release_evidence.EvidenceError, "must disable retries"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, _ = reports(source_digest)
        live_profile = next(
            profile for profile in provider["profiles"] if profile["status"] == "passed"
        )
        live_profile["acceptance"]["attempts"] = 3
        live_profile["acceptance"]["retries"] = 1
        with self.assertRaisesRegex(release_evidence.EvidenceError, "zero retries"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

    def test_nested_provider_fields_require_the_complete_typed_schema(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)

        for container, field in (
            (("profiles", 0), "source"),
            (("profiles", 0, "capabilities"), "streaming"),
            (("profiles", 1, "acceptance"), "usage"),
            (("profiles", 1, "acceptance", "usage"), "total_tokens"),
        ):
            provider, _ = reports(source_digest)
            target = provider
            for part in container:
                target = target[part]
            target.pop(field)
            with self.assertRaisesRegex(release_evidence.EvidenceError, "missing fields"):
                release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, _ = reports(source_digest)
        provider["profiles"][1]["acceptance"]["usage"]["prompt_tokens"] = "sk-secret"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "non-negative integer"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, _ = reports(source_digest)
        provider["profiles"][1]["acceptance"]["usage"]["cache_hit_rate"] = 0.5
        with self.assertRaisesRegex(release_evidence.EvidenceError, "hit rate is inconsistent"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, _ = reports(source_digest)
        provider["profiles"][1]["capabilities"]["function_tools"] = {"note": "secret"}
        with self.assertRaisesRegex(release_evidence.EvidenceError, "must be a boolean"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, _ = reports(source_digest)
        provider["profiles"][1]["acceptance"]["reported_provider"] = "openai"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "identity does not match"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, _ = reports(source_digest)
        provider["profiles"][1]["acceptance"]["reported_model"] = "secret-model"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "identity does not match"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

    def test_release_requires_one_live_profile_and_a_matching_fixture(self) -> None:
        source_digest = release_evidence.release_source_digest(ROOT)
        provider, fixture = reports(source_digest)
        for profile in provider["profiles"]:
            profile["status"] = "inspected"
            profile.pop("acceptance", None)
        with self.assertRaisesRegex(release_evidence.EvidenceError, "no live profile"):
            release_evidence._validate_provider(provider, ROOT, "1.5.0", source_digest)

        provider, fixture = reports(source_digest)
        live_profiles = release_evidence._validate_provider(
            provider, ROOT, "1.5.0", source_digest
        )
        fixture["profile_id"] = "openai-responses"
        with self.assertRaisesRegex(release_evidence.EvidenceError, "no live handshake"):
            release_evidence._validate_fixture(
                fixture, ROOT, "1.5.0", source_digest, live_profiles
            )


if __name__ == "__main__":
    unittest.main()
