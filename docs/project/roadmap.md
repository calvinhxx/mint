# 当前状态

[← 返回文档树](../../README.md)

源码基线是 `v1.0.0`。CLI 主流程已经完成，正式 Release 还缺跨平台与真实模型证据。

| 范围 | 状态 |
|---|---|
| Agent Loop、文件工具、固定命令、验证门禁 | 已完成 |
| checkpoint、changeset 事务、任务恢复 | 已完成 |
| 中英文 CLI、JSONL 日志、机器输出 | 已完成 |
| Chat Completions、Responses、Anthropic Messages | 离线测试完成 |
| Windows、macOS、Linux 的 x64 / ARM64 CI 矩阵 | 已完成 |
| 本机构建、测试和发布包验收 | 已完成 |
| FluentQt 桌面源码预览 | macOS 构建、原生渲染与离线任务链路已通过 |
| 一个真实 provider 的正式握手与修复 fixture | 待完成 |
| 六平台正式 Release 证据 | 待完成 |

## v1.0.0 收尾

1. 在同一提交上通过本地全量检查；
2. 选择一个 provider，完成一次受控握手和一次修复 fixture；
3. 生成六平台测试与发布包证据；
4. 更新 Changelog，创建 `v1.0.0` tag 和 GitHub Release。

具体命令见[发布](../development/releasing.md)。

## 已知边界

- macOS 和 Linux 的命令沙箱不提供完整宿主读取白名单；
- Windows 没有单文件大小硬限制；
- POSIX 资源限制和工作区磁盘限制不是文件系统原生 quota；
- Windows 和 macOS 包尚未签名，macOS 尚未 notarize；
- Linux 预编译包以 Ubuntu 24.04 为兼容基线；
- provider 模板与离线协议测试不等于线上服务验证；
- 桌面界面尚未完成 Windows、Linux 原生视觉验收，也未进入发布包；

持续聊天和多 Agent 编排不在当前产品范围。桌面预览只呈现现有任务、审批和证据语义，不扩展 Agent 能力。

## v1.0.0 之后

先根据真实用户和回归结果修复可靠性问题。桌面界面完成跨平台原生验收后再考虑打包；没有使用证据前，不扩展多 Agent 或更多工作流抽象。
