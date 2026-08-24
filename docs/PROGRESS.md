# AI Agent 主线进度

更新时间：2026-08-24

## 结论

- 当前版本：`v1.3.0`。
- 主线状态：v1.3 实现和自动验收完成；尚未创建 release tag。
- 工程门禁：Debug + `-Werror`、format-check、CTest、ASan + UBSan 全部通过。
- 参考平台：macOS arm64，AppleClang 17，命令后端 `macos-seatbelt`。
- 外部模型：v1.2 已用当前本地 Chat Completions 配置完成隔离修复闭环；v1.3 没有把该旧结果算作新的 provider 复验。

产品定义仍是本地 Coding Agent Harness，不包含 GUI、多 Agent 编排或完整容器隔离。

## v1.3 主线闭环

```text
用户显式 init
   ↓
工程识别 → 工作区外 ProjectStore → 项目 policy
                                      ↓ 创建快照
run / resume → task id + task policy + session + events
                                      ↓
仓库理解 → 固定 recipe → transactional changeset → verification
                                      ↓
status + 净 diff + 可恢复终态 + 模型传输进度
```

## 核心模块与完成度

百分比只表示 v1.3 已声明范围，不代表长期产品的最终形态。

| 核心模块 | 完成度 | v1.3 已完成 |
|---|---:|---|
| 工程结构与风格 | 100% | application/domain/infrastructure/runtime/tools/cli 分层；分层 CMake targets；稳定 `aiagent_core`；clang-format、EditorConfig、`-Werror`、Sanitizer 门禁 |
| 日常 CLI | 100% | `init/run/resume/status`；交互或参数任务输入；最近 model task 恢复；demo 强制只读且不可恢复；指定 task 查询；文本/JSON 输出；旧 CLI 兼容 |
| ProjectService | 100% | 识别 CMake、Cargo、npm；只建议已存在且非 symlink 的写路径；生成固定 build/test recipes；未知工程和无脚本 npm 默认只读 |
| ProjectStore | 100% | 工作区外私有状态；稳定项目 key；profile、project policy、task metadata；独立 task policy/session/events；任务列表与可恢复状态发现 |
| 权限与信任 | 100% | `init` 才采用建议；state/workspace 互不包含；新目录 0700、文件 0600；已有公共目录拒绝且不改权限；manifest/状态/task symlink 拒绝；`init --force` 不改旧 task policy 快照 |
| 配置与密钥保护 | 100% | 独立 `config.json`；配置、policy、session、events、project/task metadata 均进入保护路径；不把密钥传给模型工具 |
| 模型网关 | 100% | Chat Completions function tools；超时、取消、408/429/5xx 退避；usage/response/model/HTTP 元数据；尝试、重试、成功和失败实时事件 |
| Agent Loop 与上下文 | 100% | 多轮 assistant/tool 回填；多工具；轮数/时间/上下文预算；大结果压缩；完成、超时、取消和验证终态 |
| TaskPolicy / Recipes | 100% | 严格 schema；写路径；固定 program/argv/cwd/timeout；verification 标记；预算；能力指纹；恢复时完全匹配 |
| 文件工具与边界 | 100% | list/read/search；canonical root；越界、symlink、敏感目录、二进制、UTF-8、数量和大小限制 |
| Patch / ChangeSet | 100% | 单文件精确 patch；1–16 个 create/replace/delete/move；预校验；有界 diff 审批；提交失败回滚 |
| Command Runner | 100%（macOS） | 无 shell；程序解析；固定 recipe；cwd、过滤环境、输出、进程组、超时/取消；Seatbelt 禁网及文件边界 |
| 验证控制器 | 100% | 每次写入使旧验证失效；只有 eligible verification recipe 成功才允许最终完成 |
| ChangeJournal / Diff | 100% | schema v2 created/modified/deleted；move 记为删除+创建；多次修改折叠为会话净 diff；恢复检测漂移 |
| Session / Recovery | 100% | schema v3 pending + durable in-flight barrier；只读自动重放；副作用默认阻断；显式 `--retry-inflight`；schema v2 迁移 |
| Events / Machine Result | 100% | 脱敏 JSONL；模型传输、上下文、工具、验证和终态事件；最终 JSON 汇总 task id、重试、tokens、命令和变更 |
| 测试与验收资产 | 100% | 单元/集成/安全回归；v1.2/v1.3 contract suites；CLI smoke；故障 fixture；managed CLI 端到端；常规与 ASan/UBSan 6/6 |

## 版本增量

### v1.1：任务级能力策略

- 显式 policy 替代日常长串 capability flags，同时保留兼容模式。
- 模型只选择 recipe 名称，不能控制 argv。
- 多文件操作升级为一次可预览、可回滚 changeset。

### v1.2：可靠恢复与可观测性

- checkpoint schema v3 补上“工具已开始但结果尚未持久化”的崩溃窗口。
- 有副作用的模糊 in-flight 操作默认不重放。
- verification recipe、模型 usage/重试元数据、分层工程和独立 contract suite 完成。

### v1.3：项目与任务工作流

- 日常入口收敛为 `init/run/resume/status`，不再要求用户手工管理多个运行时文件。
- 项目配置和所有任务状态移到工作区外，任务创建时冻结 policy 快照。
- CMake/Cargo/npm 获得显式、可检查的能力建议，未知项目保持只读。
- task id、状态查询、最近可恢复任务和旧 CLI 兼容进入端到端门禁。
- HTTP 尝试与自动重试不再静默等待，终端和 JSONL 同时给出进度。

## 验证状态

| 门禁 | 结果 |
|---|---|
| CMake Debug + AppleClang 17 + warnings-as-errors | PASS |
| clang-format check | PASS |
| 常规 CTest | 6/6 PASS |
| ASan + UBSan CTest | 6/6 PASS |
| CLI `--version` | `aiagent 1.3.0` |
| v1.3 `init → run --demo → status` | PASS；外部 task state、completed 终态和不可恢复拒绝均已验证 |
| 旧 `aiagent --demo ...` 调用 | PASS；不生成 managed task 字段 |
| v1.2 外部模型隔离 fixture | PASS；12 轮、11 次工具、初始 test 失败、最终 verification 通过 |

详细证据见 [`V1_3_ACCEPTANCE.md`](V1_3_ACCEPTANCE.md) 和 [`V1_2_ACCEPTANCE.md`](V1_2_ACCEPTANCE.md)。

## 平台边界

| 平台 | v1.3 状态 |
|---|---|
| macOS arm64 | 完整参考实现与自动验收 |
| Linux | 核心与状态存储可移植；没有正式原生命令安全后端，安全命令模式默认拒绝 |
| Windows | 状态路径已有实现；受控命令执行与正式验收矩阵未完成 |

Seatbelt 是受控执行后端，不等于容器；`sandbox-exec` 也已被 Apple 标记为 deprecated。

## 下一主线

1. Linux namespace/seccomp 或容器后端，以及 Windows Job Object/AppContainer；
2. streaming 与 Responses/provider adapters，并建立多 provider contract tests；
3. CPU、内存、进程数和磁盘配额；
4. changeset 跨进程事务日志和可重放 trace；
5. 独立产品层的 GUI 与多任务管理，不侵入 Harness 核心。
