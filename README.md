# aiagent

一个 C++20 本地 Coding Agent Harness。默认只读；写路径、固定命令和验证门禁都由用户明确授权。

当前版本：`v1.4.0`（macOS arm64 参考实现）。

## 快速开始

需要 CMake 3.24+、C++20 编译器和 libcurl。

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

./build/aiagent init --root .
./build/aiagent run --root . --demo "这个项目从哪里启动？"
./build/aiagent status --root .
```

离线 `--demo` 不需要 API Key。真实模型使用：

```bash
cp config.example.json config.json
# 编辑 config.json
./build/aiagent run --root . "修复失败测试，验证通过后总结"
```

默认示例使用 Chat Completions；Responses API 可复制 `config.responses.example.json`。旧版
`config.json` 不需要迁移，新增的 `adapter` 和 `stream` 都有兼容默认值。

真实模型任务中断后运行 `./build/aiagent resume --root .`；默认恢复最近一个可恢复任务，也可用 `--task ID` 指定。离线 demo 始终强制只读，且不会恢复成真实模型任务。

`init` 是显式信任动作：它识别 CMake、Cargo 或 npm 工程，生成可检查的写路径和固定 recipes。项目 profile、任务 policy 快照、session 与 events 保存在工作区外；仓库文件不会在运行时静默扩大旧任务权限。

## 核心边界

- `apply_patch` 精确修改单文件；`apply_changeset` 原子提交多文件事务，失败回滚。
- 模型只能选择 policy 中的 recipe 名称，不能临时改写 program/argv/cwd。
- 最新写入后，必须由最新成功的 verification recipe 解除验证门禁。
- schema v3 checkpoint 区分 pending 与 in-flight；有副作用的模糊操作默认不重放。
- Chat Completions / Responses 共用一个模型端口；流式文本只显示在终端，JSONL 不记录正文。
- macOS 命令默认进入 Seatbelt；没有受支持安全后端时，CLI 默认拒绝命令执行。

高级兼容模式仍支持显式 `--policy`、`--session` 和原始 capability flags，详见：

```bash
./build/aiagent --help
```

## 文档

- [工程结构与设计约束](docs/ARCHITECTURE.md)
- [核心模块与完成进度](docs/PROGRESS.md)
- [v1.4 验收记录](docs/V1_4_ACCEPTANCE.md)
- [v1.2 外部模型验收记录](docs/V1_2_ACCEPTANCE.md)

安全边界：这是受控执行 Harness，不是容器。macOS 已有参考安全后端；Linux/Windows 原生命令隔离尚未完成。外部模型证据只代表验收时使用的具体 provider 配置。
