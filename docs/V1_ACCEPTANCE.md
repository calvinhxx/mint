# v1.0.0 验收记录

验收日期：2026-08-24

参考平台：macOS arm64，Darwin 24.6.0，AppleClang 17

验收对象：`aiagent v1.0.0`

## 结论

`PASS`。

最终二进制在隔离的故障 C++ 工程中，由真实模型完成了仓库检查、失败基线、精确修复、报告创建、重建和最新 CTest 验证。Harness 最终结果为：

```json
{
  "status": "completed",
  "completed": true,
  "turns": 16,
  "verification_status": "passed",
  "execution": {
    "tool_calls": 15,
    "file_changes": 2,
    "command_calls": 8,
    "commands_failed": 1,
    "commands_passed": 7,
    "last_file_change_call": 14,
    "last_command_call": 15,
    "last_command_outcome": "passed"
  }
}
```

`last_command_call > last_file_change_call`，因此最终验证晚于报告创建和源码修复，不是复用旧的成功结果。

## 验收工程

仓库内固定样例：[`tests/fixtures/v1_broken_project`](../tests/fixtures/v1_broken_project)

初始缺陷位于 `src/calculator.cpp`：

```cpp
return left - right;
```

测试契约要求 `add(2, 3) == 5` 和 `add(-2, 3) == 1`。确定性生命周期测试先证明初始 CTest 失败，再证明精确修复后通过。

真实模型验收使用 `/private/tmp` 下的新副本，不修改仓库内 fixture。运行能力被限制为：

- `apply_patch` 写入：只允许 `src/calculator.cpp` 和 `FIX_REPORT.md`；
- 命令：只允许 `cmake` 和 `ctest`；
- 命令沙箱：强制 `macos-seatbelt`；
- 验证：强制 `--require-verification`；
- 上下文：256 KiB；
- 总时间：600 秒；
- 最大模型轮数：24。

核心调用形态如下，真实密钥只由本地 `config.json` 读取，没有写入日志或验收文档：

```bash
./build/v1/aiagent \
  --config config.json \
  --root /private/tmp/<isolated>/project \
  --allow-write \
  --allow-write-path src/calculator.cpp \
  --allow-write-path FIX_REPORT.md \
  --allow-command cmake \
  --allow-command ctest \
  --require-verification \
  --max-turns 24 \
  --max-context-bytes 262144 \
  --max-seconds 600 \
  --events-jsonl /private/tmp/<isolated>/events.jsonl \
  --session /private/tmp/<isolated>/session.json \
  --json "<acceptance task>"
```

## 真实模型证据链

1. 模型读取文件结构、README、实现和测试契约。
2. 沙箱内 CTest 返回 exit code `8`，形成真实失败基线。
3. `apply_patch` 只把 `left - right` 改为 `left + right`；源码格式保持不变。
4. `apply_patch` 创建 `FIX_REPORT.md`，记录根因与验证命令。
5. 报告创建之后再次运行受控命令，最后一条命令 exit code 为 `0`。
6. Harness 输出的 changed files 恰好为：

```text
FIX_REPORT.md
src/calculator.cpp
```

7. 最终净 diff 中实现变化恰好是：

```diff
-    return left - right;
+    return left + right;
```

8. 8 条命令事件全部包含：

```json
{
  "sandboxed": true,
  "sandbox_backend": "macos-seatbelt"
}
```

命令退出码序列包含一个失败 `8`，最后为成功 `0`。

## 独立复核

Agent 结束后，在隔离副本之外再次执行：

```bash
ctest --test-dir build --output-on-failure
```

结果：`1/1 passed`，`100% tests passed`。

逐字比较确认以下文件与原 fixture 完全一致：

- `README.md`
- `CMakeLists.txt`
- `include/calculator.hpp`
- `tests/calculator_tests.cpp`

事件与会话文件权限均为 `0600`。最终 session 为 schema v2，并记录：

```json
{
  "allowed_write_paths": ["src/calculator.cpp", "FIX_REPORT.md"],
  "command_sandboxed": true,
  "command_sandbox_backend": "macos-seatbelt",
  "max_context_bytes": 262144
}
```

## 自动化回归

常规 Debug 构建：

```text
2/2 tests passed
- aiagent_tests
- v1_fixture_lifecycle
```

ASan + UBSan 构建：

```text
2/2 tests passed
- aiagent_tests
- v1_fixture_lifecycle
```

`aiagent_tests` 包含：

- 根目录、路径穿越、符号链接、忽略目录和配置保护；
- UTF-8、精确替换、创建、原子写入、净 diff 和写路径 allowlist；
- 命令白名单、无 shell、环境过滤、审批、超时、取消和进程组终止；
- Seatbelt 工作区内写入、工作区外写入阻断、受保护文件读取阻断和网络阻断；
- 最新验证门禁、失败后继续修复、拒绝/取消/超时状态；
- 脱敏 JSONL、纯 JSON 结果、schema v2 检查点和 pending call 恢复；
- 64 KiB 上下文硬上限下的 128 KiB 工具结果压缩；
- 本地 HTTP 服务器连续两次 503 后第三次成功的重试退避。

## 不属于 v1 PASS 的声明

- macOS Seatbelt 是当前参考后端，`sandbox-exec` 已被 Apple 标记为 deprecated；
- Linux 安全命令后端和 Windows 命令执行尚未完成；
- 检查点保证最后稳定点恢复，不承诺事务级 exactly-once；
- 流式响应、provider adapter、删除/移动、多文件事务和 GUI 属于 v1 后工作。

这些边界不会削弱本次定义内的 v1 主线闭环，但禁止把 v1 描述成跨平台容器或完整 IDE 产品。
