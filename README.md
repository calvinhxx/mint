# mint

mint 是一个轻量的通用 AI Agent 工具。它提供模型循环、工具调用、权限控制、任务恢复和结果验证，可以直接使用 CLI，也可以作为 C++ 内核扩展。

当前自带文件读取、搜索、编辑和固定命令工具，适合本地工程与目录自动化。

## 快速试用

需要 CMake 3.24+、Ninja、C++20 编译器和 vcpkg。

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release

./build/vcpkg-release/mint init --root .
./build/vcpkg-release/mint run --root . --demo "总结这个目录"
~~~

`--demo` 不需要 API Key，不会修改文件或运行项目命令。

## 文档

所有文档都从这里进入：

- 入门
  - [开始使用](docs/getting-started/quickstart.md)：安装、配置模型和日常命令。
- 原理
  - [工作原理与代码结构](docs/concepts/architecture.md)：一次任务如何经过模型、工具和验证。
- 指南
  - [安全、权限与任务恢复](docs/guides/safety-and-recovery.md)：文件边界、固定命令和中断处理。
- 开发
  - [构建、测试与验收](docs/development/testing.md)：本地验证命令和证据边界。
- 项目
  - [当前进度与路线图](docs/project/roadmap.md)：已完成能力、缺口和下一步。

第一次使用只需阅读“开始使用”；需要理解源码时，再阅读“工作原理与代码结构”。

目前完整验证的平台是 macOS arm64。Linux 和 Windows 的安全命令后端尚未完成。
