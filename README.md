# aiagent

一个运行在本地项目里的命令行编程助手：读代码、改文件、运行构建和测试。

## 快速开始

需要 CMake 3.24+、C++20 编译器和 libcurl。

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

./build/aiagent init --root .
./build/aiagent run --root . --demo "这个项目从哪里启动？"
```

`--demo` 不需要 API Key，也不会修改文件。

## 使用模型

```bash
cp config.example.json config.json
# 填写 api_key、base_url 和 model
./build/aiagent run --root . "修复失败的测试，然后告诉我改了什么"
```

Responses API 使用 `config.responses.example.json`。不要提交包含密钥的 `config.json`。

## 常用命令

```bash
./build/aiagent run --root . "任务" # 开始任务
./build/aiagent resume --root .    # 继续中断的任务
./build/aiagent status --root .    # 查看任务状态
./build/aiagent --help             # 查看全部选项
```

## 安全原则

- 默认只读；写入和命令执行需要明确开启。
- 模型只能运行项目初始化时记录的命令，不能自由执行 shell。
- 多文件修改失败时自动恢复；也可以要求测试通过后才结束任务。
- 任务记录保存在项目之外，API Key 不会提供给模型工具。

## 文档

- [架构图与代码导读](docs/ARCHITECTURE.md)
- [当前完成进度](docs/PROGRESS.md)

目前完整支持 macOS arm64；Linux 和 Windows 的安全命令执行仍在开发中。
