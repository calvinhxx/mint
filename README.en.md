<div align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</div>

# mint

mint is a lightweight local AI agent. The model chooses the next step; mint reads, searches, changes, and verifies files on your machine.

The model has no direct shell or filesystem access. Writes are disabled by default, project commands must be registered by the user, and changed code must pass a real build or test before a task can complete.

> [!NOTE]
> Prebuilt packages are not published yet; build from source for now. The CLI is the stable entry point. A FluentQt desktop preview is available from source but is not included in release packages. Multi-agent orchestration is out of scope.

## Try it

You need CMake 3.24+, Ninja, a C++20 compiler, and [vcpkg](https://github.com/microsoft/vcpkg).

### CLI

The default `release` and `dev` presets build only the CLI.

```bash
git clone https://github.com/calvinhxx/mint.git
cd mint

export VCPKG_ROOT=/path/to/vcpkg
cmake --preset release
cmake --build --preset release

./build/release/mint init --root .
./build/release/mint run --root . --demo "Summarize this project"
```

`--demo` needs no API key and cannot change files or run project commands. See [Getting started](docs/getting-started/quickstart.md) for real model configuration and Windows builds.

### GUI

Also install a Qt 6.5+ desktop development kit, including Qt SVG. CMake fetches and builds FluentQt v1.8.1 automatically. From the repository root, keep `VCPKG_ROOT` set and replace the path below with your Qt kit:

```bash
cmake --preset release -B build/gui -DMINT_BUILD_GUI=ON \
  -DCMAKE_PREFIX_PATH="/path/to/Qt/6.x/kit"
cmake --build build/gui --target mint_gui --parallel 4
./build/gui/mint init --root .        # Initialize this project once
./build/gui/mint-gui --root . --demo
```

On macOS, `cmake --build build/gui --target mint_gui_app --parallel 4` creates the double-clickable `build/gui/Mint.app`. After removing `build`, rerun these commands. See [Desktop builds](docs/getting-started/quickstart.md#桌面界面) for Windows, local source, and SDK options.

## Documentation

Detailed guides are currently written in Simplified Chinese.

- Use mint
  - [Getting started](docs/getting-started/quickstart.md): build, configure, and use the CLI.
  - [Desktop preview](docs/getting-started/quickstart.md#桌面界面): build and try the FluentQt GUI offline.
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
