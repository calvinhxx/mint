# v1.3.0 验收记录

验收日期：2026-08-24

参考平台：macOS arm64，AppleClang 17

验收对象：`aiagent v1.3.0`

## 结论

| 范围 | 结果 |
|---|---|
| Debug、warnings-as-errors 与 clang-format | PASS |
| 单元、集成、安全和 v1.2/v1.3 contract tests | PASS |
| managed CLI 端到端工作流 | PASS |
| ASan + UBSan | PASS |
| v1.2 兼容 CLI | PASS |
| 新的外部 provider 复验 | 未执行；沿用并明确引用 v1.2 的既有证据 |

v1.3 的项目初始化、任务隔离、状态查询、恢复选择和模型传输进度已经形成确定性闭环。外部模型质量与 provider 兼容性没有因本次本地门禁而被扩大声明。

## 工程门禁

```bash
cmake -S . -B build/v1_3-dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DAIAGENT_WARNINGS_AS_ERRORS=ON
cmake --build build/v1_3-dev --target format-check
cmake --build build/v1_3-dev --parallel 4
ctest --test-dir build/v1_3-dev --output-on-failure
```

常规结果：

```text
6/6 tests passed
- aiagent_tests                  unit;integration
- aiagent_v1_2_tests             v1.2;contract
- aiagent_v1_3_tests             v1.3;contract
- aiagent_cli_version            v1.3;smoke
- v1_fixture_lifecycle           acceptance
- aiagent_v1_3_cli_workflow      v1.3;acceptance
```

Sanitizer 使用相同配置并增加 `-DAIAGENT_ENABLE_SANITIZERS=ON`，结果同样为 `6/6 passed`，没有 AddressSanitizer 或 UndefinedBehaviorSanitizer 报告。

## v1.3 契约证据

独立 `aiagent_v1_3_tests` 覆盖：

- CMake、Cargo、npm build/test scripts 和未知工程的能力建议；
- 不跟随 symlink build manifest，也不把 symlink/特殊文件加入建议写范围；
- state/workspace 互相包含时拒绝；已有公共目录拒绝且原权限不被修改；状态层级 symlink 拒绝；
- project profile、project policy、task metadata 和 task policy 的私有权限；
- task 创建时冻结 policy fingerprint；
- `init --force` 更新项目 policy，但不修改旧 task policy；
- task 文本 UTF-8、metadata workspace 绑定，以及 session schema v3 到 status/resumable 的映射；
- model/demo task mode；中断的 demo 即使存在 session 也不进入可恢复集合；
- task id 路径穿越和 task policy symlink 替换拒绝。

既有模型 retry 测试新增 `ModelProgress` 序列断言：

```text
attempt_started(1/3)
retry_scheduled(HTTP 429)
attempt_started(2/3)
retry_scheduled(HTTP 503)
attempt_started(3/3)
request_succeeded(HTTP 200)
```

事件只记录 attempt、HTTP status、delay 和 elapsed time，不包含 prompt、密钥或响应正文。

## managed CLI 端到端

自动验收执行以下主链：

```bash
aiagent init   --root <fixture> --state-dir <external-state> --json
aiagent status --root <fixture> --state-dir <external-state> --json
aiagent run    --root <fixture> --state-dir <external-state> --demo --json "inspect this fixture"
aiagent status --root <fixture> --state-dir <external-state> --json
aiagent resume --root <fixture> --state-dir <external-state> --task <completed-id> --json
```

通过条件：

1. CMake 工程被识别，初始化后任务列表为空；
2. demo `run` 强制只读并返回 `completed=true`、稳定 task id 和工作区外 task directory；
3. `status` 精确列出该任务的 `mode=demo`、completed/non-resumable 终态；
4. 对 completed task 的 `resume` 明确失败；
5. 重复 `init` 需要 `--force`，而 forced init 保留已有任务；
6. `status --demo` 等无关参数不被静默忽略；
7. 工作区内部 `--state-dir` 明确拒绝；
8. 旧 `aiagent --demo ...` 仍完成，且结果不混入 managed task 字段。
9. 模型配置等 checkpoint 前错误仍返回 task id，`status --task` 可查询 created 任务。

九项条件全部满足。

## 迁移边界

- v1.2 的 `--policy/--session/--events-jsonl/--resume` 和 capability flags 保持兼容。
- v1.3 不自动搬运旧 session；旧任务继续使用兼容 CLI，新的 managed task 从 `init` 开始。
- project/task metadata 使用 schema v1，Agent checkpoint 继续使用 session schema v3。
- managed task 的 policy 是创建时快照，不随项目 policy 变化。

## 外部模型证据

本次没有再次把 fixture 发给外部 provider。最近一次真实模型隔离闭环仍见 [`V1_2_ACCEPTANCE.md`](V1_2_ACCEPTANCE.md)：当前配置端点完成 12 轮、11 次工具调用、一次 changeset、失败基线和写后 verification。v1.3 对模型 HTTP 路径的功能变化是无敏感正文的进度回调与动态 User-Agent；provider 能力仍需按具体端点分别验收。
