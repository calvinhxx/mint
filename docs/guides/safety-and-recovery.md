# 安全与恢复

[← 返回 README 文档树](../../README.md)

mint 不把 shell 或文件句柄交给模型。模型只能从本次任务提供的工具中选择动作，本地代码决定是否执行。

## 文件权限

每个文件请求都会检查：

- 路径必须留在项目根目录内；
- 写入位置必须被任务 policy 允许；
- 符号链接不能绕出项目；
- 配置、密钥和任务存档等保护路径不可访问；
- 多文件修改必须先整体通过预检查。

相关实现位于 [`tool_registry.cpp`](../../src/tools/tool_registry.cpp)、[`workspace_tools.cpp`](../../src/tools/workspace_tools.cpp) 和 [`change_set.cpp`](../../src/tools/change_set.cpp)。

## 命令权限

模型不能提交一段任意 shell。项目初始化后，命令以 recipe 保存为程序、参数和工作目录，例如：

~~~text
configure -> cmake -S . -B build
build     -> cmake --build build
test      -> ctest --test-dir build --output-on-failure
~~~

运行时只能选择已经登记且被 policy 允许的 recipe。`CommandRunner` 还负责超时、输出限制、资源上限和平台沙箱。

| 系统 | 默认命令保护 |
|---|---|
| macOS | Seatbelt：限制宿主写入、敏感文件和网络 |
| Linux | Bubblewrap：工作区可写，其余宿主路径只读或隐藏；使用独立网络和运行时目录 |
| Windows | AppContainer：工作区和已登记程序可用；受保护路径与网络不可用 |

Linux 缺少 `bwrap` 时会直接拒绝命令，不会自动退回到无沙箱模式。非标准安装位置可以用 `MINT_BWRAP_PATH=/absolute/path/to/bwrap` 指定。

Windows 使用 `CreateProcessW` 直接启动程序，不经过 shell。每个 `CommandRunner` 拥有独立的无网络 capability AppContainer；DACL 只向工作区、已登记程序及其同目录运行文件授权，受保护路径保持不可读。Job Object 再限制进程树、CPU、内存和进程数。

AppContainer 不是虚拟机，仍可见部分 Windows 公共系统资源。如果工具还依赖工作区和程序同目录之外的用户文件，命令会以无权访问失败。`--unsafe-no-command-sandbox` 会关闭当前平台的 OS 隔离，只应由信任工作区和命令的本地操作者显式使用。

## 命令资源上限

资源上限写在任务 policy 的 `tool_limits.command_resources` 中：

~~~json
{
  "tool_limits": {
    "command_resources": {
      "cpu_seconds": 60,
      "memory_bytes": 1073741824,
      "max_processes": 128,
      "file_size_bytes": 67108864
    }
  }
}
~~~

数值为 `0` 表示不启用该项；旧 policy 没有这些字段时保持原行为。Linux 使用进程级 `rlimit`；macOS 的内存由 mint 监控命令主进程，其余项目使用 `rlimit`。Windows 的 CPU、内存和进程数限制作用于整个 Job Object，也就是命令进程树。

Windows Job Object 不提供单文件大小限制，因此 Windows policy 的 `file_size_bytes` 必须为 `0`。POSIX 的限制仍不等于整棵进程树总量；工作区总磁盘配额也尚未实现。可识别的超限原因会写入命令结果，Linux 内存分配或进程创建被内核拒绝时也可能只得到非零退出码。

## 修改后的验证

启用验证门禁后：

1. 文件一旦修改，已有验证结果立即失效；
2. 必须在最新修改之后运行允许的验证命令；
3. 命令返回成功，任务才能正常完成；
4. 再次修改文件后，需要重新验证。

因此“模型说已经修好”不算完成证据，实际命令结果才算。

## 中断恢复

Agent 会在工具执行前后保存 checkpoint。恢复时按动作是否有副作用处理：

| 中断时的动作 | 默认处理 |
|---|---|
| 读取、搜索等只读工具 | 可以安全重试 |
| `apply_changeset` 多文件修改 | 根据事务日志自动回滚或确认，然后安全重试未完成的调用 |
| `apply_patch` 单文件修改 | 不自动重试，先检查现场 |
| 构建或测试命令 | 不自动重试，避免重复副作用 |

changeset 在写第一个文件前保存事务日志，内容包括每个文件修改前后的状态。工具结果写入 checkpoint 后，session schema v4 会同时记录事务 ID，随后才删除日志。恢复时：

- checkpoint 没有该事务 ID：说明写入结果还未确认，mint 先把所有文件恢复到修改前，再重试 changeset；
- checkpoint 已有该事务 ID：说明结果已经确认，mint 保留修改并清理日志；
- 任一文件既不等于修改前，也不等于修改后：说明外部程序改过文件，mint 不覆盖现场，也不删除日志。

每个任务还有一把进程锁，两个 mint 进程不能同时修改同一任务。托管任务的事务日志、锁、checkpoint 和事件记录都在项目目录之外，并且不会暴露给模型工具。

`apply_patch` 和命令没有这套事务协议。它们的结果不确定时，只有用户明确允许后才会重试。

## 当前边界

- 三个系统都在 x64 / ARM64 原生 CI 运行适用测试；
- Linux Bubblewrap 和 Windows AppContainer 在两种架构验收工作区写入、越界写入、受保护读取和网络四个边界；
- Windows 对工作区外的自定义工具链仍缺可配置的只读路径；
- POSIX 进程树总资源和工作区总磁盘配额仍待补充。

这些限制属于当前实现范围，不应由提示词代替。
