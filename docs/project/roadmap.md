# 项目进度

[← 返回文档树](../../README.md)

当前处于 v1.0.0 发布前收尾。版本号已设为 `1.0.0`，[Changelog](../../CHANGELOG.md) 仍为 `Unreleased`。以下状态按 2026-09-11 的本地源码核对；未核验远端发布和 CI 报告。

| 范围 | 状态 |
|---|---|
| 单任务运行 | 文件工具、固定命令、审批、验证、检查点和事务恢复已有实现与回归测试 |
| 模型接入 | 三种协议适配器及离线测试已实现；GUI / CLI 共用模型配置和密钥存储 |
| 桌面客户端 | 源码预览，运行任务时可浏览其他会话；尚未纳入发布包 |
| 自动化检查 | 已配置六平台 CLI 测试与打包，以及 Linux x64 GUI 离屏测试；需核对候选提交的运行结果 |
| 发布证据 | 本地尚无 `release/evidence/` 归档，真实 provider 和六平台候选版本验收仍需收齐 |

## 发布前剩余

1. 对同一候选源码完成本地检查、六平台 CLI 测试和包验收。
2. 选择一个 provider，生成对应的真实握手和修复 fixture 报告，并归档脱敏证据。
3. 核对版本、更新发布文案，再创建 tag 和 Release。

操作步骤只在[发布](../development/releasing.md)中维护；测试范围见[测试](../development/testing.md)。

## 后续重点

- 先处理实际任务中暴露的中断、恢复、模型兼容和界面交互问题。
- 完成 Windows、Linux 桌面原生交互验收后，再推进 GUI 打包和正式分发签名。当前 Windows、macOS 包未签名，macOS 未 notarize；Linux 包以 Ubuntu 24.04 为兼容基线。

当前聚焦单任务 Agent，持续聊天和多 Agent 编排不在计划内。附件输入、MCP 客户端和长期语义记忆尚未提供。沙箱与资源限制见[安全与恢复](../guides/safety-and-recovery.md#边界)。

[源码导读](../guide/README.md)
