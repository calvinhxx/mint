<div align="right">
  <strong>简体中文</strong> · <a href="README.en.md">English</a>
</div>

# mint

mint 是一个轻量的本地 AI Agent。模型选择下一步，mint 在本机完成文件读取、搜索、修改和验证。

模型没有直接的 shell 或文件系统权限。写入默认关闭，项目命令只能从用户登记的列表中选择；修改后必须通过真实构建或测试，任务才能完成。

> [!NOTE]
> 预编译包尚未发布，当前请从源码构建。项目只提供 CLI 和 C++ Agent 内核，不包含 GUI 或多 Agent 编排。

## 试用

需要 CMake 3.24+、Ninja、C++20 编译器和 [vcpkg](https://github.com/microsoft/vcpkg)。

```bash
git clone https://github.com/calvinhxx/mint.git
cd mint

export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release

./build/vcpkg-release/mint init --root .
./build/vcpkg-release/mint run --root . --demo "总结这个项目"
```

`--demo` 不需要 API Key，不会修改文件或运行项目命令。接入真实模型和 Windows 构建方式见[开始使用](docs/getting-started/quickstart.md)。

## 文档

- 使用
  - [开始使用](docs/getting-started/quickstart.md)：构建、配置和日常命令。
  - [模型配置](docs/reference/model-providers.md)：供应商、协议、密钥和 Token 预算。
  - [安全与恢复](docs/guides/safety-and-recovery.md)：文件边界、命令沙箱和任务恢复。
- 理解
  - [架构](docs/concepts/architecture.md)：执行流程、模块和依赖方向。
- 开发
  - [代码风格](docs/development/code-style.md)：命名、双语注释和自动检查。
  - [测试](docs/development/testing.md)：本地检查和平台矩阵。
  - [发布](docs/development/releasing.md)：打包、真实模型证据和发布门禁。
- 项目
  - [当前状态](docs/project/roadmap.md)：完成度、已知边界和下一步。
  - [更新记录](CHANGELOG.md)
  - [安全漏洞报告](SECURITY.md)
