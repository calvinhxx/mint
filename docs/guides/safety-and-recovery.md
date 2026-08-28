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
| Windows | 默认拒绝；尚无文件与网络沙箱 |

Linux 缺少 `bwrap` 时会直接拒绝命令，不会自动退回到无沙箱模式。非标准安装位置可以用 `MINT_BWRAP_PATH=/absolute/path/to/bwrap` 指定。

Windows 已使用 `CreateProcessW` 直接启动程序，不经过 shell；只继承标准输入输出句柄，过滤环境变量，并用 Job Object 控制整棵进程树的结束、CPU、内存和进程数。它仍能访问用户有权访问的文件和网络，因此只有显式传入 `--unsafe-no-command-sandbox` 才会启用。macOS / Linux 传入同一参数也会关闭各自的 OS 沙箱。

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
| 写文件 | 不自动重试，先检查现场 |
| 构建或测试命令 | 不自动重试，避免重复副作用 |

只有用户明确允许后，才会重试结果不确定的写操作或命令。checkpoint 和事件记录保存在项目目录之外。

## 当前边界

- macOS 和 Linux 已在 x64 / ARM64 原生 CI 完成适用测试；
- Linux Bubblewrap 后端已在两种架构完成沙箱边界验收；
- Windows 受控进程和 Job Object 已实现，但文件与网络隔离仍缺；
- POSIX 进程树总资源和工作区总磁盘配额仍待补充。

这些限制属于当前实现范围，不应由提示词代替。
