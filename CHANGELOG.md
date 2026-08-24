# Changelog

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
