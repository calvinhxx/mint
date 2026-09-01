<div align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</div>

# mint

mint is a lightweight local AI agent. The model chooses the next step; mint reads, searches, changes, and verifies files on your machine.

The model has no direct shell or filesystem access. Writes are disabled by default, project commands must be registered by the user, and changed code must pass a real build or test before a task can complete.

> [!NOTE]
> Prebuilt packages are not published yet; build from source for now. The project provides a CLI and C++ agent core, without a GUI or multi-agent orchestration.

## Try it

You need CMake 3.24+, Ninja, a C++20 compiler, and [vcpkg](https://github.com/microsoft/vcpkg).

```bash
git clone https://github.com/calvinhxx/mint.git
cd mint

export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release

./build/vcpkg-release/mint init --root .
./build/vcpkg-release/mint run --root . --demo "Summarize this project"
```

`--demo` needs no API key and cannot change files or run project commands. See [Getting started](docs/getting-started/quickstart.md) for real model configuration and Windows builds.

## Documentation

Detailed guides are currently written in Simplified Chinese.

- Use mint
  - [Getting started](docs/getting-started/quickstart.md): build, configure, and use the CLI.
  - [Model configuration](docs/reference/model-providers.md): providers, protocols, credentials, and token budgets.
  - [Safety and recovery](docs/guides/safety-and-recovery.md): file boundaries, command sandboxes, and task recovery.
- Understand mint
  - [Architecture](docs/concepts/architecture.md): execution flow, modules, and dependency direction.
- Develop
  - [Code style](docs/development/code-style.md): naming, bilingual comments, and automated checks.
  - [Testing](docs/development/testing.md): local checks and the platform matrix.
  - [Releasing](docs/development/releasing.md): packages, live model evidence, and release gates.
- Project
  - [Current status](docs/project/roadmap.md): completion, known limits, and next steps.
  - [Changelog](CHANGELOG.md)
  - [Security reporting](SECURITY.md)
