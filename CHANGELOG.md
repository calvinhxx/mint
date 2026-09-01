# Changelog

## 1.0.0 - Unreleased

- 提供 `init`、`run`、`resume` 和 `status` CLI，以及可嵌入的 C++ Agent 内核。
- 统一 Chat Completions、Responses 与 Anthropic Messages，内置主流 provider 配置模板。
- 将文件写入、固定命令、验证和恢复纳入显式 policy 与变更事务。
- 在 macOS、Linux 和 Windows 上提供命令隔离，并保留清晰的已知边界。
- 支持本地任务状态、结构化诊断日志、离线回归和六平台构建配置。
- 增加任务级累计 Token 上限、usage 覆盖说明和跨 checkpoint 统计。
- 严格执行工具参数契约，提示内联密钥迁移，并提供脱敏 Agent 评测回放。
- 按 domain、ports、application、tools、runtime 与 infrastructure 划分核心模块，并使用分层公共头文件。
