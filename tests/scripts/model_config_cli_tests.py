#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import unittest


MINT: Path
OLD_KEY = "synthetic-loopback-model-key-before"
NEW_KEY = "synthetic-loopback-model-key-after"
OLD_MODEL = "loopback-model-before"
NEW_MODEL = "loopback-model-after"
CHALLENGE = "mint-provider-acceptance-v1"
RECEIPT = "MINT_PROVIDER_ACCEPTANCE_OK"


class LoopbackProvider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), StreamingHandler)
        self.requests: list[tuple[dict, str, str]] = []
        self.failures: list[str] = []
        self.first_request = threading.Event()
        self.continue_first_request = threading.Event()
        self.records_lock = threading.Lock()


class StreamingHandler(BaseHTTPRequestHandler):
    server: LoopbackProvider

    def log_message(self, _format: str, *args: object) -> None:
        pass

    def do_POST(self) -> None:
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 256 * 1024:
                raise ValueError("Unexpected request body length")
            request = json.loads(self.rfile.read(size))
            with self.server.records_lock:
                self.server.requests.append(
                    (request, self.headers.get("Authorization", ""), self.path)
                )
                request_index = len(self.server.requests)
            if request.get("stream") is not True:
                raise ValueError("The shared default must use streaming")
            continuation = any(
                message.get("role") == "tool" for message in request["messages"]
            )
            delta = (
                {"role": "assistant", "content": RECEIPT}
                if continuation
                else {
                    "role": "assistant",
                    "tool_calls": [
                        {
                            "index": 0,
                            "id": "call_mint_loopback",
                            "type": "function",
                            "function": {
                                "name": "mint_acceptance_echo",
                                "arguments": json.dumps({"challenge": CHALLENGE}),
                            },
                        }
                    ],
                }
            )

            def chunk(choices: list[dict], **extra: object) -> dict:
                return {
                    "id": f"loopback-{request_index}",
                    "object": "chat.completion.chunk",
                    "model": request["model"],
                    "choices": choices,
                    **extra,
                }

            chunks = [
                chunk([{"index": 0, "delta": delta, "finish_reason": None}]),
                chunk(
                    [
                        {
                            "index": 0,
                            "delta": {},
                            "finish_reason": "stop" if continuation else "tool_calls",
                        }
                    ]
                ),
                chunk(
                    [],
                    usage={"prompt_tokens": 12, "completion_tokens": 6, "total_tokens": 18},
                ),
            ]
            body = "".join(f"data: {json.dumps(value)}\n\n" for value in chunks)
            head = body.encode("utf-8")
            tail = b"data: [DONE]\n\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(head) + len(tail)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(head)
            self.wfile.flush()
            if request_index == 1:
                # EN: Hold the first stream open while another CLI replaces the defaults.
                # ZH-CN: 在另一个 CLI 替换默认配置期间，保持首轮流式响应未完成。
                self.server.first_request.set()
                if not self.server.continue_first_request.wait(15):
                    raise TimeoutError("The concurrent configuration update did not complete")
            self.wfile.write(tail)
            self.wfile.flush()
        except Exception as error:
            with self.server.records_lock:
                self.server.failures.append(type(error).__name__)
            self.close_connection = True


class ModelConfigCliIntegrationTest(unittest.TestCase):
    def test_default_demo_can_list_and_search_its_current_workspace(self) -> None:
        for state_location in ("sibling", "workspace", "parent"):
            with self.subTest(state_location=state_location), tempfile.TemporaryDirectory(
                prefix="mint-default-workspace-"
            ) as directory:
                base = Path(directory)
                workspace = base / "workspace"
                workspace.mkdir()
                (workspace / "README.md").write_text("# Agent fixture\n", encoding="utf-8")
                state = {"sibling": base / "state", "workspace": workspace,
                         "parent": base}[state_location]
                common = ["--root", ".", "--state-dir", str(state), "--json",
                          "--lang", "en", "--log-file-level", "off"]
                mode = "exec"
                if state_location == "sibling":
                    mode = "run"
                    initialized = subprocess.run(
                        [str(MINT), "init", *common], cwd=workspace,
                        text=True, capture_output=True, timeout=10, check=False,
                    )
                    self.assertEqual(initialized.returncode, 0, initialized.stderr)
                result = subprocess.run(
                    [str(MINT), mode, *common, "--demo", "Summarize this project"],
                    cwd=workspace, text=True, capture_output=True, timeout=10, check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertTrue(report["completed"])
                self.assertEqual(report["model"]["adapter"], "demo")
                # EN: Demo finishes after three tools even when list/search fail.
                # ZH-CN: 即使列表和搜索失败，演示也会在三次工具调用后结束。
                self.assertEqual(report["execution"]["tool_calls"], 3)
                self.assertEqual(report["execution"]["successful_tool_calls"], 3)
                self.assertEqual(report["execution"]["tool_errors"], 0)

    def test_legacy_workspace_files_are_explicit_inputs_not_defaults(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mint-config-entry-") as directory:
            root = Path(directory)
            state = root / "state"
            workspaces = [root / "first", root / "second"]
            originals: list[bytes] = []
            for index, workspace in enumerate(workspaces):
                workspace.mkdir()
                source = workspace / "config.json"
                source.write_text(json.dumps({
                    "provider": "custom",
                    "endpoint": "https://legacy.example.test/v1/chat/completions",
                    "model": f"legacy-{index}",
                    "api_key": f"synthetic-legacy-{index}-key",
                    "max_retries": 3,
                }), encoding="utf-8")
                originals.append(source.read_bytes())

            def invoke(workspace: Path, *command: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [str(MINT), *command, "--state-dir", str(state), "--json",
                     "--lang", "en", "--log-file-level", "off"],
                    cwd=workspace, text=True, capture_output=True, timeout=10, check=False,
                )

            for workspace in workspaces:
                shown = invoke(workspace, "config", "show")
                self.assertEqual(shown.returncode, 0, shown.stderr)
                self.assertFalse(json.loads(shown.stdout)["configured"])
                self.assertNotEqual(invoke(workspace, "provider").returncode, 0)
                self.assertNotEqual(invoke(workspace, "exec", "inspect").returncode, 0)
                self.assertFalse((state / "model.json").exists())
                self.assertFalse((state / "credentials.json").exists())

            # EN: An explicit override is read-only and never becomes the shared default.
            # ZH-CN: 显式覆盖只读，不会变成共享默认配置。
            override = invoke(workspaces[0], "provider", "--config", "config.json")
            self.assertEqual(override.returncode, 0, override.stderr)
            self.assertEqual(json.loads(override.stdout)["model"], "legacy-0")
            self.assertNotIn("synthetic-legacy-0-key", override.stdout + override.stderr)
            self.assertFalse((state / "model.json").exists())

            imported = invoke(workspaces[0], "config", "import", "--config", "config.json")
            self.assertEqual(imported.returncode, 0, imported.stderr)
            self.assertNotIn("synthetic-legacy-0-key", imported.stdout + imported.stderr)
            committed = {
                name: (state / name).read_bytes()
                for name in ("model.json", "credentials.json")
            }
            for command in (("config", "show"), ("provider",), ("provider", "test")):
                # EN: Reject before provider requests; --help keeps regressions offline too.
                # ZH-CN: 在供应商请求前拒绝；--help 也确保回归失败时不联网。
                rejected = invoke(workspaces[0], *command, "--config", "", "--help")
                self.assertNotEqual(rejected.returncode, 0)
            self.assertNotIn(b"synthetic-legacy-0-key", committed["model.json"])
            for workspace in workspaces:
                inspected = invoke(workspace, "provider")
                self.assertEqual(inspected.returncode, 0, inspected.stderr)
                self.assertEqual(json.loads(inspected.stdout)["model"], "legacy-0")
                self.assertNotIn("synthetic-legacy-0-key", inspected.stdout + inspected.stderr)
            conflict = invoke(workspaces[1], "config", "import", "--config", "config.json")
            self.assertEqual(conflict.returncode, 2)
            self.assertEqual(json.loads(conflict.stdout)["code"], "conflict")
            for name, content in committed.items():
                self.assertEqual((state / name).read_bytes(), content)
            for workspace, original in zip(workspaces, originals):
                self.assertEqual((workspace / "config.json").read_bytes(), original)

            # EN: Removing the authoritative model must not resurrect any workspace key.
            # ZH-CN: 删除共享模型入口后，不得恢复任何工作区密钥。
            (state / "model.json").unlink()
            shown = invoke(workspaces[1], "config", "show")
            self.assertEqual(shown.returncode, 0, shown.stderr)
            self.assertFalse(json.loads(shown.stdout)["configured"])
            self.assertNotEqual(invoke(workspaces[1], "provider").returncode, 0)
            self.assertFalse((state / "model.json").exists())
            self.assertEqual((state / "credentials.json").read_bytes(),
                             committed["credentials.json"])

    def test_explicit_config_runs_without_home_or_shared_storage(self) -> None:
        requests: list[dict] = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, _format: str, *args: object) -> None:
                pass

            def do_POST(self) -> None:
                requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                body = json.dumps({
                    "id": "explicit-config-loopback", "model": "explicit-model",
                    "choices": [{"message": {"role": "assistant", "content": "Explicit OK"},
                                 "finish_reason": "stop"}],
                }).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        with tempfile.TemporaryDirectory(prefix="mint-explicit-no-home-") as directory:
            workspace = Path(directory)
            blocked_state = workspace / "blocked-state"
            blocked_state.write_text("unchanged sentinel", encoding="utf-8")
            profile = workspace / "override.json"
            environment = os.environ.copy()
            for name in ("HOME", "LOCALAPPDATA", "APPDATA", "USERPROFILE", "XDG_STATE_HOME"):
                environment.pop(name, None)
            with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as provider:
                thread = threading.Thread(target=provider.serve_forever, daemon=True)
                thread.start()
                try:
                    profile.write_text(json.dumps({
                        "provider": "custom",
                        "endpoint": f"http://127.0.0.1:{provider.server_port}/v1/chat/completions",
                        "model": "explicit-model", "api_key": "synthetic-explicit-key",
                        "stream": False, "max_retries": 0,
                    }), encoding="utf-8")
                    for state_args in ([], ["--state-dir", str(blocked_state)]):
                        with self.subTest(state_args=state_args):
                            common = ["--config", str(profile), "--json",
                                      "--log-file-level", "off", *state_args]
                            inspected = subprocess.run(
                                [str(MINT), "provider", *common], cwd=workspace, env=environment,
                                text=True, capture_output=True, timeout=10, check=False,
                            )
                            self.assertEqual(inspected.returncode, 0, inspected.stdout + inspected.stderr)
                            result = subprocess.run(
                                [str(MINT), "exec", "--root", ".", *common, "Reply with Explicit OK"],
                                cwd=workspace, env=environment, text=True, capture_output=True,
                                timeout=10, check=False,
                            )
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            report = json.loads(result.stdout)
                            self.assertTrue(report["completed"])
                            self.assertEqual(report["answer"], "Explicit OK")
                            self.assertNotIn("synthetic-explicit-key", result.stdout + result.stderr)
                    self.assertEqual(len(requests), 2)
                    self.assertEqual(blocked_state.read_text(encoding="utf-8"), "unchanged sentinel")
                finally:
                    provider.shutdown()
                    thread.join(timeout=5)

    def test_help_describes_shared_defaults_in_both_languages(self) -> None:
        for locale in ("en", "zh-CN"):
            result = subprocess.run(
                [str(MINT), "--lang", locale, "--help"],
                text=True, capture_output=True, timeout=10, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("model.json", result.stdout)
            self.assertNotIn("./config.json", result.stdout)

    def test_shared_credentials_and_inflight_runtime_snapshot(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mint-model-config-cli-") as directory:
            root = Path(directory)
            state = root / "state"
            logs = root / "logs"
            workspace = root / "workspace"
            workspace.mkdir()
            environment = {
                key: value
                for key, value in os.environ.items()
                if not key.startswith("MINT_")
                and not key.endswith("_API_KEY")
                and not key.upper().endswith("_PROXY")
            }
            environment["NO_PROXY"] = "127.0.0.1,localhost"
            outputs: list[str] = []

            def arguments(*command: str) -> list[str]:
                return [
                    str(MINT),
                    *command,
                    "--state-dir", str(state),
                    "--log-dir", str(logs),
                    "--log-level", "trace",
                    "--log-file-level", "trace",
                    "--lang", "en",
                    "--json",
                ]

            def invoke(*command: str, document: dict | None = None) -> dict:
                result = subprocess.run(
                    arguments(*command),
                    input=None if document is None else json.dumps(document),
                    text=True,
                    capture_output=True,
                    cwd=workspace,
                    env=environment,
                    timeout=20,
                    check=False,
                )
                outputs.extend([result.stdout, result.stderr])
                self.assertEqual(result.returncode, 0, "The CLI command failed")
                return json.loads(result.stdout)

            with LoopbackProvider() as provider:
                thread = threading.Thread(target=provider.serve_forever, daemon=True)
                thread.start()
                process: subprocess.Popen[str] | None = None
                try:
                    def settings(revision: str, model: str, key: str) -> dict:
                        return {
                            "expected_revision": revision,
                            "adapter": "chat_completions",
                            "base_url": f"http://127.0.0.1:{provider.server_port}/v1",
                            "model": model,
                            "api_key": key,
                        }

                    first = invoke(
                        "config", "set", "--stdin-json",
                        document=settings("", OLD_MODEL, OLD_KEY),
                    )
                    self.assertTrue(first["saved_api_key"])
                    self.assertTrue(first["credential_available"])
                    process = subprocess.Popen(
                        arguments("provider", "test"),
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        cwd=workspace,
                        env=environment,
                    )
                    self.assertTrue(provider.first_request.wait(15), "No loopback request arrived")
                    second = invoke(
                        "config", "set", "--stdin-json",
                        document=settings(first["revision"], NEW_MODEL, NEW_KEY),
                    )
                    self.assertNotEqual(first["revision"], second["revision"])
                    provider.continue_first_request.set()
                    stdout, stderr = process.communicate(timeout=20)
                    outputs.extend([stdout, stderr])
                    self.assertEqual(process.returncode, 0, "The in-flight provider probe failed")
                    in_flight = json.loads(stdout)
                    self.assertEqual(in_flight["status"], "passed")
                    self.assertEqual(in_flight["model"], OLD_MODEL)
                    self.assertEqual(in_flight["acceptance"]["reported_model"], OLD_MODEL)
                    self.assertEqual(in_flight["acceptance"]["requests"], 2)
                    self.assertEqual(in_flight["acceptance"]["streamed_requests"], 2)
                    next_run = invoke("provider", "test")
                    self.assertEqual(next_run["status"], "passed")
                    self.assertEqual(next_run["model"], NEW_MODEL)
                    self.assertEqual(next_run["acceptance"]["reported_model"], NEW_MODEL)
                    self.assertEqual(next_run["acceptance"]["streamed_requests"], 2)
                    self.assertEqual(provider.failures, [])
                    self.assertEqual(len(provider.requests), 4)
                    for index, (request, authorization, path) in enumerate(provider.requests):
                        expected_model = OLD_MODEL if index < 2 else NEW_MODEL
                        expected_key = OLD_KEY if index < 2 else NEW_KEY
                        self.assertEqual(request["model"], expected_model)
                        self.assertTrue(
                            authorization == f"Bearer {expected_key}",
                            "A provider request used the wrong private credential",
                        )
                        self.assertEqual(path, "/v1/chat/completions")
                        self.assertTrue(request["stream"])
                    log_files = list(logs.glob("*.jsonl"))
                    self.assertTrue(log_files, "The trace logging assertion must inspect real logs")
                    outputs.extend(path.read_text(encoding="utf-8") for path in log_files)
                    public_text = "\n".join(outputs)
                    self.assertNotIn(OLD_KEY, public_text)
                    self.assertNotIn(NEW_KEY, public_text)
                    config_text = (state / "model.json").read_text(encoding="utf-8")
                    self.assertNotIn(OLD_KEY, config_text)
                    self.assertNotIn(NEW_KEY, config_text)
                    if os.name != "nt":
                        self.assertEqual((state / "credentials.json").stat().st_mode & 0o777, 0o600)
                finally:
                    provider.continue_first_request.set()
                    if process is not None and process.poll() is None:
                        process.kill()
                        process.communicate(timeout=5)
                    provider.shutdown()
                    thread.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--mint", type=Path, required=True)
    options = parser.parse_args()
    MINT = options.mint.resolve(strict=True)
    unittest.main(argv=[__file__])
