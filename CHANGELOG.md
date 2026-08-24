# Changelog

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
