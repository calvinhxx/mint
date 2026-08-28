# 项目进度

[← 返回 README 文档树](../../README.md)

更新时间：2026-08-28

## 当前状态

| 项目 | 状态 |
|---|---|
| 代码版本 | `1.5.0`（开发中） |
| 产品定位 | 轻量的通用 AI Agent 工具与 C++ 内核 |
| 形态 | 本地 CLI；一次 `run` 处理一个任务 |
| 命令 | `init / run / resume / status` |
| 模型接口 | Chat Completions、Responses、可选 SSE |
| 构建矩阵 | Windows / macOS / Linux × x64 / ARM64 |
| 平台验收 | 六组合原生 CI；三个系统都默认启用 OS 命令隔离 |
| 本地测试 | Debug 和 Sanitizer CTest 57/57 |
| 持续集成 | 六组合原生构建矩阵；macOS ARM64 深度门禁 |
| 最近真实模型验收 | v1.4 Chat Completions 隔离修复任务 |

主流程已经覆盖项目初始化、读取、修改、验证、状态查询和中断恢复。当前重点是稳定 CLI 和可扩展内核，不包含 IDE 或 GUI。

## 已完成

- 项目识别和 policy / recipe 初始化；
- 多轮模型调用与工具结果回填；
- 受限的文件读取、搜索、修改和多文件变更；
- 固定构建、测试命令及验证门禁；
- checkpoint、事件日志和中断恢复；
- session schema v4 与多文件事务日志：进程在 changeset 中途退出时自动回滚或确认，恢复后不会重复提交；
- 循环生命周期、单轮执行和 checkpoint 恢复分离，session schema 保持兼容；
- Chat Completions / Responses 统一内部格式；
- 普通响应、SSE、限流重试和请求统计；
- HTTP 传输、重试编排和模型协议相互独立，成功 SSE 不重复缓存完整响应体；
- vcpkg 依赖、spdlog 诊断日志和 GoogleTest 单元测试；
- Windows、macOS、Linux 的 x64 / ARM64 CMake preset、原生 CI runner 和矩阵一致性校验，六组合均已通过；
- Linux Bubblewrap 安全命令后端，隔离宿主写入、用户目录、运行时套接字、网络和继承文件描述符；
- Windows AppContainer 命令后端，隔离越界写入、受保护文件和网络，并使用 `CreateProcessW`、继承句柄白名单、过滤环境和 Job Object；
- policy 可配置命令资源上限：POSIX 支持 CPU、内存、进程数和单文件大小，Windows Job Object 支持进程树 CPU、内存和进程数；
- 本地与 GitHub Actions 共用一条发布检查，覆盖版本、格式、Debug、Release、Sanitizer、CTest 和离线 CLI；
- v1.4 真实 Chat Completions 回归，包含工具调用、限流等待、修改、验证和独立复测；
- 文本输出、JSON 输出和诊断信息分流。

## 证据范围

| 证据 | 已覆盖 | 不代表 |
|---|---|---|
| 单元和协议测试 | 权限、消息转换、恢复等确定性行为 | 真实服务始终兼容 |
| 本地假模型服务 | HTTP、SSE、重试和工具闭环 | 所有 provider 均可用 |
| 真实模型验收 | 当时的端点和配置完成了任务 | 其他端点或未来版本也通过 |

v1.4 已用当前 `config.json` 完成一次隔离的 Chat Completions 修复任务。Responses 和 SSE 仍只有本地协议与回环服务证据。具体结果见 [测试与验收](../development/testing.md)。

## 仍缺少

| 方向 | 缺口 |
|---|---|
| 平台运行时 | Windows 还不能在 policy 中声明工作区外的自定义只读工具链路径 |
| 资源限制 | 还缺 POSIX 进程树总量、跨平台工作区磁盘配额；Windows 不支持单文件大小限制 |
| 恢复 | changeset 已可自动恢复；单文件写和命令仍按副作用工具处理，需要用户确认不确定结果 |
| 模型兼容 | Responses、SSE 和更多 provider 尚未做真实回归 |
| 交互 | 没有持续聊天、多任务 GUI 和多 Agent 编排 |

## 下一步

1. 增加 provider 能力识别和固定回归配置；
2. 增加 Windows 外部只读工具链路径，补 POSIX 进程树与跨平台磁盘配额；
3. 内核稳定后再增加持续聊天和 GUI。

历史版本主要变化：v1.0 建立本地工具，v1.1 增加 policy 和 recipe，v1.2 增加恢复与验证，v1.3 补齐日常 CLI，v1.4 收口模型协议，v1.5 补齐六平台构建、三平台命令隔离、资源限制和多文件事务恢复。
