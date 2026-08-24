# v1.2.0 验收记录

验收日期：2026-08-24

参考平台：macOS arm64，AppleClang 17

验收对象：`aiagent v1.2.0`

## 结论

| 范围 | 结果 |
|---|---|
| 工程结构、编译、格式与静态告警门 | PASS |
| 确定性单元/集成/安全/契约测试 | PASS |
| ASan + UBSan | PASS |
| CLI 与显式 policy 离线闭环 | PASS |
| 外部 Chat Completions provider 的 v1.2 隔离复验 | PASS（当前配置端点） |

全部 v1.2 门禁通过。真实模型响应报告的模型为 `openai/gpt-oss-120b`；该结果只证明本次 `config.json` 对应端点，不代表所有 Chat Completions provider。

## 工程门禁

Debug + warnings-as-errors：

```bash
cmake -S . -B build/v1_2-dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DAIAGENT_WARNINGS_AS_ERRORS=ON
cmake --build build/v1_2-dev --target format-check
cmake --build build/v1_2-dev --parallel 4
ctest --test-dir build/v1_2-dev --output-on-failure
```

结果：

```text
4/4 tests passed
- aiagent_tests                 unit;integration
- aiagent_v1_2_tests            v1.2;contract
- aiagent_cli_version           v1.2;smoke
- v1_fixture_lifecycle          acceptance
```

Sanitizer：

```bash
cmake -S . -B build/v1_2-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DAIAGENT_WARNINGS_AS_ERRORS=ON \
  -DAIAGENT_ENABLE_SANITIZERS=ON
cmake --build build/v1_2-sanitize --parallel 4
ctest --test-dir build/v1_2-sanitize --output-on-failure
```

结果同样为 `4/4 passed`，没有 AddressSanitizer 或 UndefinedBehaviorSanitizer 报告。

## v1.2 契约证据

自动测试覆盖：

- policy 严格 schema、未知字段、路径、预算、recipe 和 fingerprint；
- recipe 模式不向模型暴露自由 argv，模型无法覆盖固定命令；
- 非 verification recipe 即使成功，也不能解除最新写入验证门禁；
- changeset 的 create/replace/delete/move、精确字段、预校验、审批拒绝和中途失败回滚；
- ChangeJournal created/modified/deleted 状态及 schema v1 到 v2 迁移；
- session schema v3 的 pending/in-flight 一致性；
- 只读 in-flight 自动重放；副作用 in-flight 默认阻断，显式授权后才重试；
- session schema v2 到 v3 迁移，以及残缺 v3 拒绝；
- 模型 retry/latency/usage 元数据与 Agent 聚合摘要；
- Seatbelt 网络、路径、运行时文件和工作区外写入边界；
- 故障 fixture 在修复前真实失败、确定性修复后真实通过。

## CLI 验证

```bash
./build/v1_2-dev/aiagent --version
```

```text
aiagent 1.2.0
```

```bash
./build/v1_2-dev/aiagent \
  --demo \
  --policy policy.example.json \
  "只读检查项目入口并给出一句话结论"
```

该路径成功显示 policy fingerprint、写路径、固定 `build/test` recipes、`macos-seatbelt`、验证门禁和任务预算；离线模型完成 list/search/read 循环，终态为 `completed/not_required`。

## 真实模型隔离复验

仓库内 fixture：[`tests/fixtures/v1_broken_project`](../tests/fixtures/v1_broken_project)

能力 policy：[`policy.v1_2.json`](../tests/fixtures/v1_broken_project/policy.v1_2.json)

fixture 被复制到 `/private/tmp` 的新目录，仓库基线没有被模型修改。真实密钥只由受保护的本地 `config.json` 读取，没有进入任务、事件或验收文档。

最终 Harness 结果：

```json
{
  "status": "completed",
  "completed": true,
  "turns": 12,
  "duration_ms": 278348,
  "verification_status": "passed",
  "execution": {
    "tool_calls": 11,
    "successful_tool_calls": 11,
    "tool_errors": 0,
    "file_changes": 2,
    "recipe_calls": 6,
    "verification_commands": 2,
    "commands_passed": 5,
    "commands_failed": 1,
    "last_file_change_call": 8,
    "last_command_call": 11,
    "last_command_outcome": "passed",
    "last_command_verification_eligible": true
  }
}
```

证据链：

1. 模型先读取工程结构、实现和测试，没有写入。
2. 固定 recipe 序列的前三条退出码为 `configure=0`、`build=0`、`test=8`，建立真实失败基线。
3. `apply_changeset` 恰好调用一次，同时替换 `src/calculator.cpp` 并创建 `FIX_REPORT.md`。
4. 源码净变化仅为 `return left - right;` 改成 `return left + right;`。
5. 写后 recipe 序列为 `configure=0`、`build=0`、`test=0`；普通 build 成功时门禁仍为 `not_run`，直到 verification test 通过。
6. 六条 recipe 事件均为 `sandboxed=true`、`sandbox_backend=macos-seatbelt`。
7. CMakeLists、README、公开头、测试和 policy 与 fixture 原件逐字一致。
8. 独立于 Agent 再运行 CTest，结果为 `1/1 passed`。
9. session 为 schema v3、`status=completed`、`in_flight_tool_call=null`；session 与 events 文件权限均为 `0600`。

模型可观测数据也贯穿最终结果：12 次模型调用、17 次 HTTP 尝试、5 次自动重试、32,671 total tokens、1,280 cached tokens。多次 provider 瞬时失败没有破坏工具顺序或恢复语义。

### 复现命令

先把 fixture 复制到临时目录，避免修改验收基线，然后运行：

```bash
./build/v1_2-dev/aiagent \
  --config /absolute/path/to/config.json \
  --root /private/tmp/<isolated>/project \
  --policy /private/tmp/<isolated>/project/policy.v1_2.json \
  --events-jsonl /private/tmp/<isolated>/events.jsonl \
  --session /private/tmp/<isolated>/session.json \
  --json \
  "先用 configure/build/test recipes 建立失败证据；再用一次 apply_changeset 同时修复 src/calculator.cpp 并创建 FIX_REPORT.md；最新写入后重新 build，并以 verification test recipe 通过后结束。"
```

本次通过条件：

1. 写入路径恰好为 `src/calculator.cpp` 与 `FIX_REPORT.md`；
2. 模型不能改变 configure/build/test 的 argv；
3. 初始 test 失败，修复后的最新 test 成功；
4. 最终 `completed=true` 且 `verification_status=passed`；
5. `recipe_calls` 与 `verification_commands` 计数正确；
6. session 为 schema v3，终态 `in_flight_tool_call=null`；
7. 所有命令事件均为 `sandboxed=true`、`sandbox_backend=macos-seatbelt`；
8. 独立运行 `ctest --test-dir build --output-on-failure` 为 `1/1 passed`。

八项条件本次全部满足。
