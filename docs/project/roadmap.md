# 项目进度

[← 返回 README 文档树](../../README.md)

更新时间：2026-08-29

## 当前状态

| 项目 | 状态 |
|---|---|
| 代码版本 | `1.5.0`（开发中） |
| 产品定位 | 轻量的通用 AI Agent 工具与 C++ 内核 |
| 形态 | 本地 CLI；一次 `run` 处理一个任务 |
| 命令 | `init / run / resume / status / provider / provider test` |
| 模型接口 | OpenAI、Groq、DeepSeek 与 custom profile；Chat Completions、Responses、可选 SSE |
| 构建矩阵 | Windows / macOS / Linux × x64 / ARM64 |
| 安装包 | v1.5 六平台 Release 归档与 SHA-256 演练通过；尚未打正式 tag |
| 平台验收 | 六组合原生 CI；三个系统都默认启用 OS 命令隔离 |
| 本地测试 | Debug 和 Sanitizer CTest 72/72 |
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
- provider 能力目录、官方端点识别和代理显式配置；请求按 profile 选择 token 字段、stream usage、tool choice 和推理续传；
- DeepSeek V4 默认思考模式兼容：流式与非流式工具调用都会回传 `reasoning_content`、补齐工具消息的空 `content`，并省略不受支持的显式 `tool_choice`；
- OpenAI Responses、Groq Chat、DeepSeek Chat 和 custom Chat 四份无密钥回归配置，以及离线 `mint provider` 检查命令；
- 显式 `mint provider test` 真实兼容性握手：固定两轮请求验证 function call、参数和工具结果续接，限制重试与输出额度，只报告脱敏统计；
- 声明式 provider 回归批次：一次检查 OpenAI Responses、Groq Chat 和 DeepSeek Chat，live 模式先校验全部密钥，再保存不可覆盖的脱敏证据；
- OpenAI Responses + SSE 隔离修复脚本：默认只做离线检查，显式 live 后复制故障 fixture、确认失败基线、执行受限修复、独立复测并保存脱敏证据；
- release tag 证据门禁：真实 provider 与修复结果必须匹配当前版本、配置、fixture 和功能源码，缺失、失败、离线或包含原始字段的结果都会被拒绝；
- 跨平台回环 HTTP 验收：Windows、macOS、Linux 共用 Chat 重试、Responses SSE、Agent 工具循环和 provider test 契约，不因 Windows 跳过网络路径；
- 支持从环境变量读取 API Key，检查命令不会读取或输出密钥，endpoint 查询参数也不会出现在报告中；
- vcpkg 依赖、spdlog 诊断日志和 GoogleTest 单元测试；
- Windows、macOS、Linux 的 x64 / ARM64 CMake preset、原生 CI runner 和矩阵一致性校验，六组合均已通过；
- Linux Bubblewrap 安全命令后端，隔离宿主写入、用户目录、运行时套接字、网络和继承文件描述符；
- Windows AppContainer 命令后端，隔离越界写入、受保护文件和网络，并使用 `CreateProcessW`、继承句柄白名单、过滤环境和 Job Object；
- policy 可声明工作区外的只读工具链路径，Windows AppContainer 会按路径补充只读 DACL，macOS / Linux 会在隐藏目录中重新暴露该路径；
- policy 可配置命令资源上限：三平台限制进程树总数并巡检工作区磁盘用量；POSIX 另支持 CPU、内存和单文件大小，Windows Job Object 支持 CPU 和内存；
- 本地与 GitHub Actions 共用一条发布检查，覆盖版本、格式、Debug、Release、Sanitizer、CTest 和离线 CLI；
- 标准 CMake 安装规则和 CPack 归档，包含 CLI、provider 模板、Windows 运行时 DLL、项目与依赖许可证；测试与 Release 使用独立 preset，六平台都会解包运行版本冒烟，正式 tag 才上传 GitHub Release；
- v1.5 六平台发布演练已生成并汇总验证 6 个 Release 归档和 6 个 SHA-256，二进制架构与 Windows / macOS / Linux 的 x64 / ARM64 目标一致；
- v1.4 真实 Chat Completions 回归，包含工具调用、限流等待、修改、验证和独立复测；
- 文本输出、JSON 输出和诊断信息分流。

## 证据范围

| 证据 | 已覆盖 | 不代表 |
|---|---|---|
| 单元和协议测试 | 权限、消息转换、恢复等确定性行为 | 真实服务始终兼容 |
| 本地假模型服务 | HTTP、SSE、重试和同一条 provider 验收 CLI | 所有真实 provider 均可用 |
| 固定 provider 配置 | 配置字段、能力选择和请求 JSON 不漂移 | 对应模型已发起真实请求 |
| 真实模型验收 | 当时的端点和配置完成了任务 | 其他端点或未来版本也通过 |

v1.4 已用当前 `config.json` 完成一次隔离的 Chat Completions 修复任务。v1.5 已把真实服务握手收敛为可重复命令，但尚未使用外部额度执行；Responses 和 SSE 仍只有本地协议与回环服务证据。具体结果见 [测试与验收](../development/testing.md)。

## 仍缺少

| 方向 | 缺口 |
|---|---|
| 资源限制 | Windows 不支持单文件大小限制；POSIX CPU / 内存不是进程树汇总；工作区磁盘限制是巡检而非文件系统硬配额 |
| 恢复 | changeset 已可自动恢复；单文件写和命令仍按副作用工具处理，需要用户确认不确定结果 |
| 模型兼容 | 已有统一验收命令；Responses、SSE 和更多 provider 尚未留下本版本真实运行记录 |
| 发布体验 | Windows / macOS 归档尚未签名；Linux 预编译包以 Ubuntu 24.04 为兼容基线 |
| 交互 | 没有持续聊天、多任务 GUI 和多 Agent 编排 |

## 下一步

1. 有模型额度时，运行固定 provider 回归批次，一次完成 OpenAI Responses、Groq Chat 和 DeepSeek Chat 握手并保存脱敏结果；
2. OpenAI Responses 握手通过后，运行 `scripts/fixture-regression.py --live` 完成隔离修复并保存脱敏证据；
3. 上述兼容性证据通过离线发布校验后，为 v1.5.0 定稿 Changelog 并打 tag，验收六个平台的正式下载包；
4. 根据真实构建负载决定是否增加 Linux cgroup 等硬资源后端；内核稳定后再做持续聊天和 GUI。

历史版本主要变化：v1.0 建立本地工具，v1.1 增加 policy 和 recipe，v1.2 增加恢复与验证，v1.3 补齐日常 CLI，v1.4 收口模型协议，v1.5 补齐六平台构建、三平台命令隔离、资源限制、多文件事务恢复和 provider profile。
