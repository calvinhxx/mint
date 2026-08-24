# 工程结构与设计约束

## 目标

`aiagent` 是一个可嵌入的本地 Coding Agent Harness。工程结构围绕四个约束组织：

1. Agent 决策与外部副作用分离；
2. 所有能力先授权，再暴露给模型；
3. 文件修改、命令结果和恢复状态可验证；
4. CLI 只负责组装，不承载领域逻辑。

## 目录

```text
include/aiagent/
  application/       Agent Loop、结果摘要、项目能力建议
  domain/            模型端口、TaskPolicy、ChangeJournal
  infrastructure/    模型网关、命令执行、配置、事件、项目/任务存储
  runtime/           取消与墙钟预算
  tools/             模型工具注册表与能力边界
  *.hpp              v1.0 公共头兼容转发层
src/
  application/       用例编排、checkpoint/recovery、验证门禁
  cli/               参数解析与 composition root
  domain/            纯策略和状态模型
  infrastructure/    HTTP、进程、JSONL 与持久化适配器
  runtime/           跨模块运行控制
  tools/             读工具、patch、transactional changeset
tests/
  aiagent_tests.cpp   既有单元/集成/安全回归
  v1_2/              v1.2 独立契约测试
  v1_3/              项目识别、状态隔离与 task policy 快照契约
  fixtures/          可重复的端到端故障工程
cmake/               编译告警、Sanitizer、格式化规则
```

## 依赖方向

```text
cli
 └─ application
     ├─ domain
     ├─ runtime
     ├─ tools
     └─ infrastructure

tools ───────────────► domain + runtime + infrastructure(command runner)
infrastructure ──────► domain + runtime
domain / runtime 不依赖 application、tools 或 cli
```

CMake 按上述边界生成 `aiagent_domain`、`aiagent_runtime`、`aiagent_infrastructure`、`aiagent_tools` 和 `aiagent_application`。嵌入方可链接稳定聚合目标 `aiagent_core`，无需知道内部拆分。

## 核心模块

| 模块 | 责任 | 不负责 |
|---|---|---|
| `Agent` | 模型/工具循环、上下文预算、验证门禁、检查点恢复、最终结果 | HTTP、文件系统写入、进程启动 |
| `ProjectService` | 识别支持的工程并生成最小 policy 建议 | 持久化、信任决定、执行命令 |
| `ProjectStore` | 工作区外 profile、task id、policy 快照和状态发现 | Agent 执行、跨进程锁、任务调度 |
| `ModelClient` | 模型端口与标准 reply/usage/metadata | provider 配置加载 |
| `ChatCompletionsClient` | HTTP 请求、错误分类、退避重试、响应解析、传输进度 | Agent 策略 |
| `TaskPolicy` | 严格解析写范围、固定 recipe 和预算，生成能力指纹 | 自动信任仓库文件 |
| `ToolRegistry` | 只暴露获授权工具，执行路径/参数/保护规则 | 自由 shell |
| `ChangeSet` | 多文件预校验、diff 审批、事务提交、失败回滚 | 跨进程/跨机器事务 |
| `CommandRunner` | 固定 argv、cwd、环境、超时、输出与 OS 沙箱 | shell 语法解释 |
| `ChangeJournal` | 记录会话初始态到当前态的净 diff | Git 提交或历史重写 |
| `SessionStore` | 权限受限的原子 checkpoint 持久化 | 分布式锁 |
| `EventLog` | 脱敏、顺序 JSONL 运行事件 | 完整 prompt/文件内容归档 |
| `TaskControl` | SIGINT 与总墙钟预算传播 | 业务终态判断 |

## 关键状态契约

### 项目与任务契约

- `init` 是采用工程建议的显式用户动作；运行时不会从仓库自动加载能力文件。
- CMake、Cargo 和带 build/test scripts 的 npm 工程获得固定 recipe 建议；未知工程与无受支持脚本的 npm 工程保持只读。
- 默认状态目录位于工作区外：macOS 使用 `~/Library/Application Support/aiagent`，Linux 使用 `$XDG_STATE_HOME/aiagent` 或 `~/.local/state/aiagent`，Windows 使用 `%LOCALAPPDATA%/aiagent`。
- `--state-dir` 可以覆盖默认位置，但它与工作区不能互相包含；已有目录必须已经是当前用户私有目录，aiagent 不会替用户修改公共目录权限。
- 每个项目使用稳定 workspace key；每个 task 独立保存 `task.json`、`policy.json`、`session.json` 和 `events.jsonl`。
- task metadata 区分 `model` 与 `demo`；managed demo 强制只读且不可恢复，避免中断后切换成真实 provider 或初始化无用的命令后端。
- task 创建时复制项目 policy；后续 `init --force` 只影响新任务，恢复旧任务仍使用原快照。
- 新状态目录限制为当前用户访问；build manifest、状态层级、task 目录、metadata 和 policy 不接受符号链接替换。

### 能力契约

- 默认只读；写和命令能力不会隐式开启。
- 兼容工作流中的 `--policy` 必须由用户明确指定；日常工作流只使用外部 ProjectStore 中的 project/task policy。
- Recipe 的 program、argv、cwd、timeout 和 verification 标记在任务开始前固定。
- Policy fingerprint 只用于恢复时检测能力漂移，不是密码学签名或信任来源。
- 模型配置、policy、session、events 和仓库元数据进入保护路径，不可被模型读取或修改。

### 写入契约

- `apply_patch` 只允许新建或唯一精确替换。
- `apply_changeset` 先验证全部操作，再审批，再提交。
- create/replace/delete/move 使用不同的精确字段集合；未知或多余字段均拒绝。
- 任何提交步骤失败都会按原始存在性和内容恢复所有已触及路径。
- ChangeJournal schema v2 同时表达 created、modified 和 deleted。

### 验证契约

- 每次成功写入都会使旧验证失效。
- 原始 command allowlist 兼容模式中的命令可作为验证；policy 模式只有 `verification=true` 的 recipe 可作为验证。
- 开启验证门禁后，最新写入之后的最新 eligible command 必须成功，模型文字不能覆盖该证据。

### 恢复契约

- session schema v3 在执行工具前持久化 `pending_tool_calls` 和 `in_flight_tool_call`。
- 工具结果入历史并保存后才清除 in-flight 标记。
- 崩溃后只读工具可以安全自动重放；有副作用工具默认停止，只有用户检查状态后显式使用 `--retry-inflight` 才会重试。
- schema v2 可迁移到 v3；残缺的 v3 快照不做猜测性修复。

### 传输进度契约

- Chat Completions 每次 attempt 开始、retry 安排、成功和终止失败都会产生 `ModelProgress`。
- 进度只包含 attempt、HTTP status、等待和耗时，不包含密钥、prompt、响应正文或 provider 错误正文。
- 文本 CLI 实时写 stderr；managed task 同时记录 `model_progress` JSONL 事件；`--json` 的 stdout 始终只保留最终机器结果。

## 代码风格

- C++20，4 空格缩进，100 列目标；以 `.clang-format` 和 `.editorconfig` 为准。
- 公共 API 位于 `include/aiagent/<layer>/`；实现细节留在 `src/`。
- 新代码使用 canonical 分层头；根级公共头只作为 v1.0 源码兼容转发层。
- 资源使用 RAII；预期输入错误返回结构化工具失败或抛出带上下文的标准异常。
- 不拼接 shell 命令，不把 secret 放入模型消息、工具结果或事件日志。
- 修改能力契约时，先更新状态/Schema 和失败语义，再补测试与 CLI 文档。

本地质量门：

```bash
cmake -S . -B build/dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAIAGENT_WARNINGS_AS_ERRORS=ON
cmake --build build/dev --target format-check
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

高风险修改还需启用 `-DAIAGENT_ENABLE_SANITIZERS=ON`，并运行故障 fixture 与真实模型隔离验收。
