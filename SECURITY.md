# 安全策略

## 支持范围

| 版本 | 状态 |
|---|---|
| `main` | 开发中，接收安全修复 |
| 最新 GitHub Release | 接收安全修复 |
| 更早版本 | 不再维护 |

## 报告漏洞

请使用仓库 **Security → Report a vulnerability** 私下报告安全问题。不要在公开 Issue 中发布漏洞细节、利用代码、API Key 或真实项目数据。

报告中请包含受影响版本、操作系统与架构、复现步骤、预期影响，以及不含敏感信息的最小示例。普通功能缺陷仍使用公开 Issue。

## 安全边界

mint 信任本地操作者及其 policy，不信任模型输出和工作区文件内容。

- 模型不能提交任意 shell 文本，只能选择用户已经登记的 recipe；构建工具仍可能启动自己的子进程。
- macOS 使用 allow-default、显式拒绝敏感访问的 Seatbelt profile；Linux 使用 Bubblewrap。两者都不是容器或虚拟机。
- Windows 尚未开放安全命令执行。
- `--unsafe-no-command-sandbox` 会按用户选择关闭 OS 命令沙箱。

默认配置下能够绕过路径、policy、验证、恢复或敏感信息保护的行为属于安全问题。仅仅因为用户扩大了根目录、登记了有副作用的 recipe，或显式启用了 unsafe 模式而产生的预期访问，不自动视为漏洞；执行超出授权范围仍属于安全问题。
