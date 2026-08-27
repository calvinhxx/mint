# mint

mint 是一个轻量的通用 AI Agent 工具。它提供模型循环、工具调用、权限控制、任务恢复和结果验证，可以直接使用 CLI，也可以作为 C++ 内核扩展。

当前自带文件读取、搜索、编辑和固定命令工具，适合本地工程与目录自动化。

## 编译并试用

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

- [开始使用](docs/getting-started/README.md)：配置模型和日常命令。
- [工作原理](docs/concepts/architecture.md)：一次任务如何经过模型、工具和验证。
- [完整文档树](docs/README.md)：安全、测试和项目路线图。

目前完整验证的平台是 macOS arm64。Linux 和 Windows 的安全命令后端尚未完成。
