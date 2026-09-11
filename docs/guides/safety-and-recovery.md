# 安全与恢复

[← 返回文档树](../../README.md)

mint 信任本地操作者和任务 policy，不信任模型输出或工作区内容。

## 文件

- 文件访问限制在项目根目录内；读取支持根内绝对路径，写入只接受相对路径。
- 写入默认关闭；启用后仍受 `write_paths` 限制。
- `.git`、当前模型配置和任务状态不可读写；`.codex`、`.agents`、`.husky` 禁止写入，命令也不能读取。
- 按已知文件名屏蔽 `.env`、私钥和常见凭据文件；这不是内容级密钥扫描。
- 文件工具拒绝有多个硬链接的普通文件；启动前的扫描发现此类文件时，命令也会被拒绝。使用硬链接副本的源码树需先复制成独立文件。
- 符号链接、大小写别名和越界路径在实际访问前检查。

项目中的 `config.json` 不再自动读取或导入。日常配置通过 GUI「设置 → 模型」或 `mint config` 管理，临时覆盖须显式指定 `--config`；配置与密钥的存储方式见[本地数据](#本地数据)。

## 命令

模型不能提交 shell 文本，只能选择 policy 中登记的 recipe。mint 直接启动程序，构建工具产生的子进程仍受平台沙箱约束。

| 平台 | 后端 | 主要边界 |
|---|---|---|
| macOS | Seatbelt | 阻止越界写入、网络和敏感路径读取 |
| Linux | Bubblewrap | 工作区可写，其余宿主路径只读或隐藏，网络关闭 |
| Windows | AppContainer + Job Object | 按路径授权读取和写入，网络关闭，限制进程树 |

Linux 找不到 `bwrap` 时会拒绝运行命令。非标准安装位置可通过 `MINT_BWRAP_PATH=/absolute/path/to/bwrap` 指定。

日常 `run` 和 `resume` 不能关闭命令沙箱。`--unsafe-no-command-sandbox` 只属于高级 `mint exec` 接口，适用于完全信任工作区和命令的本地操作者。

工具链确实需要读取工作区外目录时，在 policy 顶层声明绝对路径：

```json
{
  "schema_version": 1,
  "command_read_paths": ["/opt/toolchain"]
}
```

该字段只增加读取和执行权限，不增加写权限。

## 资源限制

文件列表和搜索受 `tool_limits.workspace_snapshot_entries` 的遍历总量限制，隐藏目录和空目录也计入，达到上限时返回 `truncated: true`。搜索还限制候选文件数、单文件字节数和命中数；长扫描响应任务取消与超时。

命令限制位于 `tool_limits.command_resources`：

```json
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
```

值为 `0` 表示关闭对应限制。Windows 不支持 `file_size_bytes`；工作区磁盘限制通过定时扫描实现，不是文件系统 quota。

## 验证

启用验证门禁后：

1. 文件修改会使旧验证失效；
2. 必须在最新修改之后运行登记的验证命令；
3. 命令成功后任务才能完成；
4. 再次修改文件后需要重新验证。

命令造成的源码变化也进入同一变更账本。越界、二进制、超大文件或无法安全表示的变化会使工作区标记为不可审计，并阻止后续命令。mint 不会自动撤销命令已经产生的副作用。

## 恢复

Agent 在工具执行前后保存 checkpoint。中断后的默认处理取决于副作用：

| 动作 | 默认处理 |
|---|---|
| 读取、搜索 | 可安全重试 |
| `apply_changeset` | 根据事务日志回滚或确认后继续 |
| `apply_patch` | 不自动重试，先检查现场 |
| 构建或测试命令 | 不自动重试，避免重复副作用 |

changeset 在写入前记录所有文件状态。恢复时只有文件仍等于事务记录的“修改前”或“修改后”状态，mint 才会自动处理；发现外部修改时会保留现场并等待人工决定。

每个任务有独立进程锁，两个 mint 进程不能同时修改同一任务。

## 本地数据

诊断日志默认位于系统状态目录的 `logs` 子目录，只保存事件名、状态和统计字段，不保存密钥、模型正文、命令输出、diff 或文件内容。

GUI 和 CLI 共用 Mint 用户状态目录中的 `model.json`，保存模型参数和认证引用（密钥引用或 `api_key_env` 环境变量名）；`credentials.json` 单独保存通过设置页或 `mint config` 写入的 API Key。它们与 QSettings 中的语言、外观等偏好分开。父目录和文件仅允许当前系统用户访问，内容不加密。`mint config show --json` 可查看实际配置路径，不输出密钥。不要手工修改密钥引用，读写与迁移入口见[模型配置](../reference/model-providers.md#共享读写入口)。

CLI 在任务启动时读取配置及其对应的密钥快照，正在运行的任务不受后续保存影响。GUI 不另行维护 profile 或注入密钥。密钥只用于配置中模型 endpoint 的鉴权；远程请求必须使用 HTTPS，本机回环可用 HTTP。密钥不进入模型消息、工具子进程环境、`model.json`、QSettings、命令参数、任务状态或诊断日志。使用 `api_key_env` 时，配置只保存环境变量名，密钥值由外部环境提供。

`events.jsonl` 和 `session.json` 不同：它们需要保存审批证据、模型消息和工具结果，因此属于私密任务数据。不要把整个任务目录上传到公开 Issue。

关闭诊断日志：

```bash
mint status --root . --log-file-level off
```

## 边界

本地操作者需保证任务期间没有其他进程恶意替换目录结构；文件工具的路径检查不提供完整的并发文件系统隔离。

这些沙箱不是虚拟机。macOS 和 Linux 仍可能只读访问部分宿主路径；POSIX 的 CPU 和内存限制也不是完整进程树的汇总值。Windows 的单文件大小与工作区磁盘限制见[资源限制](#资源限制)。
