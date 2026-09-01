# 安全与恢复

[← 返回 README 文档树](../../README.md)

mint 不把 shell 或文件句柄交给模型。模型只能从本次任务提供的工具中选择动作，本地代码决定是否执行。

## 文件权限

每个文件请求都会检查：

- 路径必须留在项目根目录内；
- 写入位置必须被任务 policy 允许；
- 大小写别名按文件系统身份判断，硬链接不会继承另一个目录项的精确写授权；
- 符号链接不能绕出项目；
- 配置、密钥和任务存档等保护路径不可访问；
- `.env`、`.env.*`（公开模板除外）、常见私钥扩展名以及 `.ssh`、`.aws`、`.kube` 等凭据目录默认不可访问；
- 多文件修改必须先整体通过预检查。

相关实现集中在 `src/tools/tool_registry.cpp`、`path_identity.cpp`、`workspace_tools.cpp` 和 `change_set.cpp`。

## 命令权限

模型不能提交一段任意 shell。项目初始化后，命令以 recipe 保存为程序、参数和工作目录，例如：

~~~text
configure -> cmake -S . -B build/mint-managed
build     -> cmake --build build/mint-managed
test      -> ctest --test-dir build/mint-managed --output-on-failure
~~~

运行时只能选择已经登记且被 policy 允许的 recipe。`CommandRunner` 还负责超时、输出限制、资源上限和平台沙箱。

| 系统 | 默认命令保护 |
|---|---|
| macOS | Seatbelt：工作区外不可写，网络不可用；HOME 中未授权路径不可读，其他部分宿主路径仍可能可读 |
| Linux | Bubblewrap：工作区可写，HOME、运行时目录和保护路径被隐藏；其余宿主路径通常只读可见，网络隔离 |
| Windows | AppContainer：工作区、已登记程序和显式只读路径可用；受保护路径与网络不可用 |

macOS 和 Linux 的默认目标是阻止越界写入、网络访问和已知敏感路径读取，不是完整的宿主读取白名单。不要把它们当作虚拟机或机密数据执行环境。Windows 的读取授权更窄，但 AppContainer 仍可见部分公共系统资源。

Linux 缺少 `bwrap` 时会直接拒绝命令，不会自动退回到无沙箱模式。非标准安装位置可以用 `MINT_BWRAP_PATH=/absolute/path/to/bwrap` 指定。

Windows 使用 `CreateProcessW` 直接启动程序，不经过 shell。每个 `CommandRunner` 拥有独立的无网络 capability AppContainer；DACL 只向工作区、已登记程序、程序同目录运行文件和 policy 声明的只读路径授权，受保护路径保持不可读。Job Object 再限制进程树、CPU、内存和进程数。

工具链确实需要读取工作区外文件时，在 policy 顶层声明绝对路径：

~~~json
{
  "schema_version": 1,
  "command_read_paths": ["/opt/toolchain"]
}
~~~

Windows 可写成 `C:\toolchains\llvm`。路径必须已经存在，不能位于工作区内、包含整个工作区或与保护路径重叠。授权只增加读取和执行权限，不增加写权限；关闭 OS 沙箱时也不能使用这个字段。

AppContainer 不是虚拟机，仍可见部分 Windows 公共系统资源。日常任务不允许关闭项目命令的 OS 隔离；高级 `mint exec` 接口中的 `--unsafe-no-command-sandbox` 只供完全信任工作区和命令的本地操作者显式使用。

## 命令资源上限

资源上限写在任务 policy 的 `tool_limits.command_resources` 中：

~~~json
{
  "tool_limits": {
    "command_resources": {
      "cpu_seconds": 60,
      "memory_bytes": 1073741824,
      "max_processes": 128,
      "file_size_bytes": 67108864,
      "workspace_disk_bytes": 4294967296
    }
  }
}
~~~

数值为 `0` 表示不启用该项；旧 policy 没有这些字段时保持原行为。`mint init` 为识别出的 CMake、Cargo 和 npm 项目默认设置 300 秒 CPU、256 个进程和 16 GiB 工作区文件上限；不含可执行 recipe 的只读项目不设置无意义的命令限额。

- `max_processes` 计算一次命令的完整进程树。Windows 使用 Job Object；macOS 和 Linux 由 mint 跟踪子孙进程，超限后终止整棵树。
- `workspace_disk_bytes` 统计工作区内普通文件的逻辑大小，不跟随符号链接。命令启动前、运行中和结束后都会检查。
- POSIX 的 CPU 和单文件大小仍由进程级 `rlimit` 执行；Linux 内存使用 `RLIMIT_AS`，macOS 由 mint 监控命令主进程。
- Windows Job Object 不提供单文件大小限制，因此 Windows policy 的 `file_size_bytes` 必须为 `0`。

工作区磁盘限制采用 100 毫秒巡检，不是文件系统原生 quota。快速写入可能短暂超过目标值，超限文件也不会自动回滚；命令结果会明确返回 `resource_limit: "workspace_disk"`。

## 修改后的验证

启用验证门禁后：

1. 文件一旦修改，已有验证结果立即失效；
2. 必须在最新修改之后运行允许的验证命令；
3. 命令返回成功，任务才能正常完成；
4. 再次修改文件后，需要重新验证。

因此“模型说已经修好”不算完成证据，实际命令结果才算。

固定命令也不能绕过这套规则。mint 会在命令前后检查工作区：文本变化进入同一个变更账本，修改后运行的这条命令本身不能同时充当验证。显式配置了 `write_paths` 时，命令改到范围外会立即失败关闭；二进制、超大文件、符号链接、权限或目录结构等无法生成可靠文本 diff 的变化会标记为不可审计，并阻止任务继续执行命令。受保护的普通文件存在额外硬链接时，mint 会在启动命令前直接拒绝执行，避免别名藏在不扫描的构建目录。mint 不会自动撤销命令已经产生的副作用，失败结果会列出受影响路径，用户应检查并手动清理。`build`、`dist`、`target`、`node_modules` 等构建产物目录不进入源码账本。

大型源码树可以通过 `tool_limits.workspace_snapshot_entries`、`workspace_snapshot_bytes` 和 `workspace_snapshot_text_bytes` 调整命令快照预算。超过预算时命令不会启动；提高预算会增加扫描时间和内存占用。

## 模型连接与终端输出

远程模型地址必须使用 HTTPS。明文 HTTP 只允许严格的本机回环地址：`localhost`、`127.0.0.0/8` 和 `::1`，并强制绕过环境代理。模型响应正文、SSE 行与事件、文本、推理内容和工具调用都有独立上限；默认值适合普通任务，需要进一步收紧时可在模型配置的 `response_limits` 对象中覆盖对应字段。

模型密钥应通过 `api_key_env` 引用环境变量。内联 `api_key` 在 `1.0.x` 只为兼容旧配置而保留，使用时会显示迁移警告；provider 检查和诊断日志都不会输出密钥值。当前使用的模型配置会单独加入保护路径；工作区里的 `.env` 和常见凭据文件也不会进入模型上下文。

新建任务 policy 默认设置 `max_total_tokens: 100000`。mint 按 provider 报告的 usage 跨轮、跨恢复累计；检查发生在响应返回后，因此单次响应可能越过目标值。越界后待执行工具和下一次模型请求都会被阻断，任务以 `budget_exhausted` 结束。没有 usage 的调用无法换算成可靠 Token 数，因此机器结果会标记为 `best_effort` 或 `unavailable`。这个限制不能代替供应商后台的消费上限，也不代表固定金额。

模型文本、工具摘要、diff 和错误消息写入人类终端前会转义控制字符、双向文本控制符和无效 UTF-8，避免内容改标题、清屏或写剪贴板。JSON / JSONL 机器协议不做文字改写，由 JSON 编码负责保持消息边界。

## 中断恢复

Agent 会在工具执行前后保存 checkpoint。恢复时按动作是否有副作用处理：

| 中断时的动作 | 默认处理 |
|---|---|
| 读取、搜索等只读工具 | 可以安全重试 |
| `apply_changeset` 多文件修改 | 根据事务日志自动回滚或确认，然后安全重试未完成的调用 |
| `apply_patch` 单文件修改 | 不自动重试，先检查现场 |
| 构建或测试命令 | 不自动重试，避免重复副作用 |

changeset 在写第一个文件前保存事务日志，内容包括每个文件修改前后的状态。工具结果写入 checkpoint 后，session schema v5 会同时记录事务 ID 和命令工作区完整性状态，随后才删除事务日志。恢复时：

- checkpoint 没有该事务 ID：说明写入结果还未确认，mint 先把所有文件恢复到修改前，再重试 changeset；
- checkpoint 已有该事务 ID：说明结果已经确认，mint 保留修改并清理日志；
- 任一文件既不等于修改前，也不等于修改后：说明外部程序改过文件，mint 不覆盖现场，也不删除日志。

每个任务还有一把进程锁，两个 mint 进程不能同时修改同一任务。托管任务的事务日志、锁、checkpoint 和事件记录都在项目目录之外，并且不会暴露给模型工具。

`apply_patch` 和命令没有这套事务协议。它们的结果不确定时，只有用户明确允许后才会重试。

## 本地记录与敏感信息

诊断日志默认位于状态目录的 `logs` 子目录。POSIX 使用 `0700` 目录和 `0600` 文件，轮转重新打开文件时会再次限制权限；Windows 使用受保护 DACL，只授权当前用户和 SYSTEM。mint 不会收紧用户已有共享目录的权限，而是停用默认文件日志或拒绝显式 `--log-dir`。日志只保存运行状态和统计字段，不保存凭据、模型正文、最终回答、命令参数、命令输出、diff 或文件内容；日志文件可以用于排错，但仍可能包含 provider、模型名和受控程序名，分享前应先检查。

`events.jsonl` 和 `session.json` 不是普通诊断日志。事件记录需要保存审批与执行证据，session 需要保存模型消息、工具结果和恢复状态，因此都按私密任务数据处理，只存放在工作区之外并限制为当前用户可读写。不要把整个任务目录作为公开问题附件上传。

使用 `--log-file-level off` 可以关闭诊断日志落盘。机器交互模式仍写本地文件，但不会把诊断行混入 stderr 控制协议。

## 当前边界

- 仓库提供 Windows、macOS、Linux 的 x64 / ARM64 原生 CI 配置；每次发布仍需保留对应运行证据；
- Linux Bubblewrap 和 Windows AppContainer 在两种架构验收工作区写入、越界写入、受保护读取和网络四个边界；
- macOS 与 Linux 不提供完整的宿主读取隔离，未隐藏的工作区外路径仍可能只读可见；
- POSIX 的 CPU 和内存上限不是整棵进程树的汇总值；
- 工作区磁盘上限是巡检式限制，不是文件系统硬配额；
- Windows 仍不支持 `file_size_bytes` 单文件限制。

这些限制属于当前实现范围，不应由提示词代替。
