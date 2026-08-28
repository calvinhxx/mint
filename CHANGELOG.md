# Changelog

## 1.5.0 - Unreleased

- Added native Windows, macOS, and Linux build/test lanes for x64 and ARM64, backed by matching vcpkg presets and a checked matrix catalog.
- Validated all six native platform lanes, including Windows x64 and ARM64 command-sandbox boundary tests.
- Added a Linux command sandbox backed by Bubblewrap: the workspace is the only writable host path, the user home and runtime directories are masked, protected files stay hidden, capabilities are dropped, and the host network is isolated.
- Closed inherited file descriptors before approved commands start, preventing an otherwise isolated child from reusing process resources opened by mint.
- Added policy-configurable CPU, memory, process-count, and per-file size limits for POSIX commands, with resource-limit evidence retained through context compaction.
- Added a Windows AppContainer command backend with no network capabilities, DACL-scoped workspace and executable access, protected-path denial, strict argv and handle isolation, and Job Object resource limits.
- Added session schema v4 and a per-task changeset journal with an exclusive process lock, deterministic crash rollback, checkpoint acknowledgement, and fail-closed external-change detection.
- Added centralized OpenAI, Groq, DeepSeek, and custom provider profiles with endpoint detection, capability-driven request fields, and explicit proxy configuration.
- Added secret-free fixed provider configurations and an offline `mint provider` command that reports the resolved adapter and capabilities without reading or printing API keys.
- Added an explicit, spend-bounded `mint provider test` handshake that validates a two-request tool round trip and emits only sanitized compatibility metrics.
- Added a manifest-driven provider regression batch that preflights every credential before live requests and writes non-overwriting, allowlisted evidence for the three official profiles.
- Fixed DeepSeek V4 thinking-mode tool continuation by replaying `reasoning_content`, keeping assistant tool-call content non-null, and omitting unsupported explicit `tool_choice`.
- Runs Chat retry, Responses SSE, tool continuation, and sanitized provider acceptance through the same loopback HTTP fixture on Windows, macOS, and Linux instead of skipping the Windows transport path.
- Added API key environment-variable loading and redacted endpoint query parameters from configuration reports.
- Added standard CMake installation and versioned CPack archives for six platform/architecture combinations, including provider templates, checksums, runtime dependencies, and license files.
- Separated test and Release presets for every platform so published archives omit GoogleTest and vcpkg build helpers.
- Added an opt-in workflow dispatch that builds, uploads, and cross-checks all six Release archives without creating a tag or GitHub Release.
- Added package extraction and launch checks to every platform lane; matching version tags publish only after the full matrix and quality gate pass.
- Fixed Windows file replacement and multi-config fixture handling exposed by the native test lanes.
- Kept the macOS ARM64 quality gate for version, format, Debug, Release, sanitizer, full CTest, and offline CLI checks.
- Added a security policy, immutable workflow action pins, a pin-validation gate, and weekly Dependabot updates.

## 1.4.0 - 2026-08-27

- Renamed the project, executable, C++ namespace, public headers, CMake targets, and state paths to `mint`; positioned it as a lightweight general AI agent toolkit.
- Added a unified model-provider layer for Chat Completions and Responses, including optional SSE streaming, normalized tool calls, usage statistics, and server-directed retries.
- Preserved the v1.3 Chat Completions configuration and public aliases while adding explicit provider adapters.
- Fixed context compaction so failed tool evidence cannot be rewritten as a successful result.
- Split the Agent loop, model transport/retry path, command execution, tool routing, file editing, and CLI composition into smaller responsibility-focused modules without changing checkpoint compatibility.
- Added a vcpkg manifest and CMake presets for Debug, Release, and ASan/UBSan builds; GoogleTest is installed only through the test feature.
- Reorganized CMake by source layer, moved shared target and test behavior into focused modules, and added a core-only `add_subdirectory` path through `mint::core`.
- Added a macOS CI gate that runs the same version, format, Debug, Release, sanitizer, test, and offline CLI checks used locally.
- Added an internal spdlog diagnostics facade with structured task, model, tool, and command events that remain separate from machine-readable stdout.
- Replaced the hand-written test harness with independently discoverable GoogleTest unit, integration, and contract cases.
- Added an injectable CLI Console boundary and an architecture test that prevents direct process I/O from spreading into production modules.
- Consolidated duplicate versioned acceptance notes and source-reading guidance into the current testing and architecture docs.
- Revalidated the current Chat Completions path against the isolated repair fixture, including server-directed rate-limit retries and independent post-run verification.

## 1.3.0 - 2026-08-24

- Added the `init`, `run`, `resume`, and `status` project workflow while preserving the legacy CLI; managed demos are read-only and non-resumable.
- Added explicit CMake, Cargo, and npm policy suggestions with unsupported projects defaulting to read-only.
- Added private project and task state outside the workspace, including immutable per-task policy snapshots.
- Added task IDs, resumable-task discovery, completed-task rejection, and machine-readable status output.
- Added real-time model attempt, retry-delay, success, and failure progress to the terminal and JSONL events.
- Added v1.3 project-store contracts and an end-to-end managed CLI acceptance gate.

## 1.2.0 - 2026-08-24

- Reorganized the project into application, domain, infrastructure, runtime, tools, and CLI layers.
- Added explicit task-policy files with fixed command recipes and verification-only recipes.
- Added transactional multi-file create/replace/delete/move changesets with preview approval and rollback.
- Added session schema v3 with durable in-flight tool recovery rules and schema v2 migration.
- Added model retry, latency, response, usage, and aggregate telemetry.
- Added format, warnings-as-errors, sanitizer, v1.2 contract, and fixture acceptance gates.
- Validated the current configured Chat Completions provider on the isolated policy/changeset/verification fixture.

## 1.0.0 - 2026-08-24

- Completed the minimal local coding loop: inspect, patch, run allowlisted commands, verify, checkpoint, and report a bounded diff.
