# aiagent

一个 C++20 本地 Coding Agent Harness。模型只能在你明确授予的范围内读文件、提交文本变更和运行固定命令；修改后的最终回答还可以由测试结果强制门禁。

当前版本：`v1.2.0`（macOS arm64 参考实现）。

## 快速开始

需要 CMake 3.24+、C++20 编译器和 libcurl。

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/aiagent --demo "这个项目从哪里启动？"
```

离线演示使用固定脚本，不需要 API Key。

## 使用真实模型

```bash
cp config.example.json config.json
# 编辑 config.json
./build/aiagent --root . "说明这个项目的入口和核心模块"
```

接口需兼容 Chat Completions function tools。`config.json` 已被 Git 忽略，并在所有 Agent 文件工具和命令沙箱中受保护。

编码任务推荐使用显式 policy，而不是每次拼一长串参数：

```bash
cp policy.example.json policy.json
# 按目标仓库修改 write_paths 与 recipes
./build/aiagent \
  --config config.json \
  --root /path/to/project \
  --policy /path/to/policy.json \
  "修复失败测试，验证通过后总结改动"
```

Policy 固定写路径、命令参数、验证命令和运行预算；它不会自动从仓库加载，必须通过 `--policy` 明确采用。需要人工复核多文件事务时，再加 `--approve-each-changeset`。

## v1.2 核心能力

- 默认只读；写入、命令和路径范围均为显式授权。
- `apply_patch` 处理单文件精确替换，`apply_changeset` 原子提交创建、替换、删除和移动，失败自动回滚。
- Policy recipes 固定 program/argv/cwd/timeout；模型不能临时改写命令。
- 最新写入之后，只有标记为 verification 的最新成功 recipe 才能解除验证门禁。
- schema v3 检查点记录 pending 与 in-flight 工具；恢复时只读操作可重放，副作用默认阻断，需显式 `--retry-inflight`。
- JSONL 事件与最终 JSON 包含调用、重试、耗时、token、命令和变更摘要，不记录密钥或大段敏感正文。
- macOS 命令默认进入 Seatbelt；无受支持安全后端时 CLI 默认拒绝命令执行。

查看完整 CLI：

```bash
./build/aiagent --help
./build/aiagent --version
```

## 文档

- [工程结构与设计约束](docs/ARCHITECTURE.md)
- [核心模块与完成进度](docs/PROGRESS.md)
- [v1.2 验收记录](docs/V1_2_ACCEPTANCE.md)
- [v1.0 历史验收记录](docs/V1_ACCEPTANCE.md)

安全边界：这是受控执行 Harness，不是容器或完整 OS 沙箱。v1.2 的命令沙箱、确定性测试和当前配置 provider 的隔离闭环已在 macOS arm64 验收；该结果不代表所有 provider，Linux/Windows 原生安全后端尚未完成。
