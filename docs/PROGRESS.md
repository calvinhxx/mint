# AI Agent 主线进度

更新时间：2026-08-24

## 结论

- 当前版本：`v1.2.0`。
- 主线状态：`v1.2 实现完成`；尚未创建 Git commit/tag。
- 工程门禁：Debug + `-Werror`、format-check、CTest、ASan + UBSan 全部通过。
- 参考平台：macOS arm64，AppleClang 17，命令后端 `macos-seatbelt`。
- 外部模型：v1.2 已使用本地配置的 Chat Completions provider 完成隔离 fixture 闭环；模型报告为 `openai/gpt-oss-120b`。该结果只证明当前配置端点，不外推到所有 provider。

v1.2 的产品定义仍是本地 Coding Agent Harness，不包含 GUI、多 Agent 编排或完整容器隔离。

## v1.2 主线能力

```text
显式 policy
   ↓
理解仓库 → 固定 recipe 建立失败证据 → transactional changeset
   ↓                                           ↓
有界上下文/事件/检查点 ← 最新写入后的 verification recipe
   ↓
机器结果 + 净 diff + 可恢复终态
```

## 核心模块与完成度

百分比只表示 v1.2 已声明范围，不表示长期产品的最终形态。

| 核心模块 | 完成度 | v1.2 已完成 |
|---|---:|---|
| 工程结构与风格 | 100% | application/domain/infrastructure/runtime/tools/cli 分层；CMake 分层 targets；稳定 `aiagent_core`；兼容转发头；clang-format、EditorConfig、`-Werror`、Sanitizer 门禁 |
| CLI / Composition Root | 100% | `--policy`、changeset 审批、session resume、in-flight 风险确认、JSON 输出、预算和 `--version`；业务逻辑留在库层 |
| 配置与密钥保护 | 100% | 独立 `config.json`；模型配置、policy、session、events 与仓库元数据均受工具和命令沙箱保护；不自动信任仓库 policy |
| 模型网关 | 100% | Chat Completions function tools；超时、取消、408/429/5xx 重试；usage、response id、adapter/model、HTTP 尝试与耗时元数据 |
| Agent Loop 与上下文 | 100% | 多轮 assistant/tool 回填；多工具；硬轮数/时间/上下文预算；大结果压缩；明确完成、超时、取消和验证终态 |
| TaskPolicy / Recipes | 100% | 严格 schema；未知字段拒绝；写路径、固定 program/argv/cwd/timeout、verification 标记和预算；稳定能力指纹；恢复时完全匹配 |
| 文件工具与能力边界 | 100% | list/read/search；canonical root；越界、符号链接、敏感目录、二进制、UTF-8、数量和大小限制 |
| Patch / ChangeSet | 100% | 单文件精确 patch；1–16 个 create/replace/delete/move；全量预校验；有界 diff 审批；提交失败完整回滚；精确操作字段契约 |
| Command Runner | 100%（macOS） | 无 shell；程序解析/白名单；固定 recipe；cwd、过滤环境、输出、进程组、超时/取消；Seatbelt 禁网与文件边界 |
| 验证控制器 | 100% | 每次写入使旧验证失效；policy 模式只有 `verification=true` recipe eligible；失败/超时/取消/拒绝均不能伪装通过 |
| ChangeJournal / Diff | 100% | schema v2 记录 created/modified/deleted；move 表达为删除+创建；多次修改折叠为会话净 diff；恢复时校验外部漂移 |
| Session / Recovery | 100% | schema v3 pending + durable in-flight barrier；只读自动重放；副作用默认阻断并要求显式 `--retry-inflight`；schema v2 迁移；残缺 v3 拒绝 |
| Events / Machine Result | 100% | 顺序脱敏 JSONL；模型、上下文、工具、验证和终态事件；最终 JSON 汇总调用、重试、tokens、耗时、命令和 changed files |
| 测试与验收资产 | 100% | 单元/集成/安全测试、v1.2 独立契约测试、CLI smoke、故障 fixture 生命周期、常规与 ASan/UBSan 4/4，以及真实模型隔离修复闭环 |

## 相比 v1.0 的主要增量

### v1.1：任务级能力策略

- 长串 CLI capability flags 被可审计的显式 policy 替代，同时保留兼容模式。
- 模型只选择 recipe 名称，不能控制 argv。
- 多文件操作从多个独立 patch 提升为一次可预览、可回滚 changeset。
- 写路径、recipes、policy fingerprint 和审批策略进入 session capability contract。

### v1.2：可靠恢复与可观测性

- checkpoint 从 schema v2 升为 v3，补上工具“开始执行但结果尚未持久化”的崩溃窗口标记。
- 默认不重放有副作用的模糊 in-flight 操作；风险必须由用户显式接受。
- 验证证据区分普通 recipe 与 verification recipe。
- 模型调用元数据和聚合 usage 贯穿事件、checkpoint、CLI 摘要和最终 JSON。
- 工程从平铺源码重组为分层库，新增独立 v1.2 contract suite 与统一质量门。

## 验证状态

| 门禁 | 结果 |
|---|---|
| CMake Debug + AppleClang 17 + warnings-as-errors | PASS |
| clang-format check | PASS |
| 常规 CTest：unit/integration + v1.2 contract + CLI smoke + fixture acceptance | 4/4 PASS |
| ASan + UBSan CTest | 4/4 PASS |
| CLI `--version` | `aiagent 1.2.0` |
| CLI `--demo --policy policy.example.json` | PASS，policy/recipes/Seatbelt/只读循环可见 |
| v1.2 外部模型隔离 fixture | PASS：12 轮、11 次工具、1 次 changeset、初始 test 失败、最终 verification test 通过 |

详细命令和边界见 [`V1_2_ACCEPTANCE.md`](V1_2_ACCEPTANCE.md)。

## 平台边界

| 平台 | v1.2 状态 |
|---|---|
| macOS arm64 | 完整参考实现与自动验收 |
| Linux | 核心可移植；没有正式原生安全命令后端，CLI 默认拒绝安全命令模式 |
| Windows | 尚未形成受控命令执行与正式验收矩阵 |

Seatbelt 是受控执行后端，不等于容器；`sandbox-exec` 也已被 Apple 标记为 deprecated。

## v1.3 候选主线

1. Linux namespace/seccomp 或容器后端，Windows Job Object/AppContainer；
2. streaming 与 Responses/provider adapters，并建立多 provider contract tests；
3. CPU、内存、进程数和磁盘配额；
4. 可重放模型 trace、持久 task id 与 OpenTelemetry；
5. changeset crash recovery 的事务日志，而不只是默认阻断/人工重试；
6. 作为独立产品层的 GUI 与多任务管理，不侵入 Harness 核心。
