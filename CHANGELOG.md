# Changelog

## 1.0.0 - Unreleased

- 提供 `init`、`run`、`resume` 和 `status` CLI，以及可嵌入的 C++ Agent 内核。
- 统一 Chat Completions、Responses 与 Anthropic Messages，内置主流 provider 配置模板。
- 将文件写入、固定命令、验证和恢复纳入显式 policy 与变更事务。
- 在 macOS、Linux 和 Windows 上提供命令隔离，并保留清晰的已知边界。
- 支持本地任务状态、结构化诊断日志、离线回归和六平台构建配置。
- 增加任务级累计 Token 上限、usage 覆盖说明和跨 checkpoint 统计。
- 严格执行工具参数契约，并提示内联密钥迁移。
- 默认保护常见凭据路径，关闭命令子进程 stdin，并为托管 recipe 设置有限资源预算。
- 将高级策略执行收敛为显式 `mint exec` 子命令，托管 JSON 结果返回事件文件路径。
- 按 domain、ports、application、tools、runtime 与 infrastructure 划分核心模块，并使用分层公共头文件。
- 桌面端使用用户私有的本地凭据文件保存 API Key，不再触发系统凭据弹窗。
- GUI 与 CLI 共用用户状态目录中的 `model.json` 和 `credentials.json`，新增 `mint config show`、`set` 和 `import`；保存支持版本冲突检测，运行中的任务保留启动时配置。
- 项目中的 `config.json` 不再自动读取或导入；旧文件需显式导入，`--config` 仅覆盖本次运行。
- CMake 预设收敛为 `dev` 和 `release`，替代 `vcpkg-dev`、`vcpkg-release` 等旧入口；GUI 默认通过 `FetchContent` 构建 FluentQt，并提供 `mint_gui_app` macOS 开发 App 目标。
- 桌面端最低使用 FluentQt 1.8.1，适配集合控件点击契约，修复主题辅助功能切换、抽屉重开和输入框重复边框，并保留键盘导航。
