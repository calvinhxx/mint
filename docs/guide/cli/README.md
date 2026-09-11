# 命令行入口

[导读目录](../README.md)

参数解析、命令分发，以及模型、工具和任务的组装。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/cli](../../../src/cli) | 解析命令行参数，组装并调用核心模块。 |
| [src/cli/agent](../../../src/cli/agent) | 任务启动与恢复、策略合并、审批、流式输出和事件路由。 |
| [src/cli/config](../../../src/cli/config) | 共享模型配置的查看、保存和显式导入命令。 |
| [src/cli/platform](../../../src/cli/platform) | POSIX main 与 Windows wmain / 参数编码适配。 |
| [src/cli/project](../../../src/cli/project) | 项目初始化与托管任务状态展示。 |
| [src/cli/provider](../../../src/cli/provider) | provider 配置检查、两次请求的握手验收，以及密钥环境变量处理。 |
| [src/cli/support](../../../src/cli/support) | 可注入终端、诊断日志配置和通用进程入口声明。 |

## 文件说明

### src/cli

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/cli/CMakeLists.txt) | 将 CLI 适配逻辑构建为 mint_cli，链接所需核心模块，按平台选择 main / wmain 入口并产出 mint。 |
| [command_line.cpp](../../../src/cli/command_line.cpp) | 解析子命令、路径、授权、预算和输出参数，配置语言，识别机器输出模式并生成帮助信息。 |
| [command_line.hpp](../../../src/cli/command_line.hpp) | 定义 CommandLine 和各子命令 action，集中保存 CLI 参数与默认值，声明解析和帮助入口。 |
| [main.cpp](../../../src/cli/main.cpp) | CLI 通用入口：初始化语言和日志，分发帮助、版本、配置、provider 或 Agent 命令，统一文本 / JSON 异常输出。 |

### src/cli/agent

| 文件 | 用途 |
|---|---|
| [agent_command.cpp](../../../src/cli/agent/agent_command.cpp) | 为 init / status / run / resume / exec 准备任务上下文，创建模型和工具，接入取消信号与取消文件，再运行 Agent 并输出结果。 |
| [agent_command.hpp](../../../src/cli/agent/agent_command.hpp) | 声明 CLI 的任务执行入口，接收命令参数、托管任务路径和终端通道。 |
| [agent_command_config.cpp](../../../src/cli/agent/agent_command_config.cpp) | 合并并校验 policy 与执行参数，检查状态文件冲突，确定受保护路径，打开运行文件并生成 ToolRegistry 配置。 |
| [agent_command_internal.hpp](../../../src/cli/agent/agent_command_internal.hpp) | 定义任务启动时共用的 RuntimeFiles，以及策略、路径、审批、模型创建和运行输出的内部接口。 |
| [agent_command_io.cpp](../../../src/cli/agent/agent_command_io.cpp) | 适配文本 / JSONL 审批、模型流式进度、问题输入、退出码和运行摘要，并从共享配置创建真实或 demo 模型。 |
| [agent_event_router.cpp](../../../src/cli/agent/agent_event_router.cpp) | 把 AgentEventSink 事件写入任务事件日志，并将任务开始 / 结束摘要同步到诊断日志。 |
| [agent_event_router.hpp](../../../src/cli/agent/agent_event_router.hpp) | 声明 AgentEventRouter，用它实现 Agent 的事件端口，转接具体日志存储。 |

### src/cli/config

| 文件 | 用途 |
|---|---|
| [model_config_command.cpp](../../../src/cli/config/model_config_command.cpp) | 实现 mint config show / set / import，读取受限大小的标准输入 JSON、处理版本冲突并输出不含密钥的配置快照。 |
| [model_config_command.hpp](../../../src/cli/config/model_config_command.hpp) | 声明共享模型配置子命令入口，连接参数解析、终端和 ModelConfigService。 |

### src/cli/platform

| 文件 | 用途 |
|---|---|
| [entry_posix.cpp](../../../src/cli/platform/entry_posix.cpp) | POSIX 原生 main，只将 argc / argv 交给通用 run_cli。 |
| [entry_windows.cpp](../../../src/cli/platform/entry_windows.cpp) | Windows 原生 wmain，将宽字符命令行转换为 UTF-8 后交给 run_cli，并处理转换失败。 |

### src/cli/project

| 文件 | 用途 |
|---|---|
| [project_commands.cpp](../../../src/cli/project/project_commands.cpp) | 实现项目初始化和任务状态展示：生成建议 policy、持久化项目登记，按任务 ID 或列表输出摘要。 |
| [project_commands.hpp](../../../src/cli/project/project_commands.hpp) | 声明 init / status 命令入口，将项目展示与 Agent 运行分开。 |

### src/cli/provider

| 文件 | 用途 |
|---|---|
| [provider_acceptance.cpp](../../../src/cli/provider/provider_acceptance.cpp) | 执行固定两次请求的 provider 验收：先请求工具调用，再回传合成结果，检查最终回执、流式行为和用量统计。 |
| [provider_acceptance.hpp](../../../src/cli/provider/provider_acceptance.hpp) | 声明依赖 ModelClient 端口的 provider 握手验收，可用真实客户端或测试替身调用。 |
| [provider_command.cpp](../../../src/cli/provider/provider_command.cpp) | 实现 provider inspect / test：读取共享或显式配置，输出脱敏的端点、能力和预算，必要时执行正式握手。 |
| [provider_command.hpp](../../../src/cli/provider/provider_command.hpp) | 声明 provider 子命令入口，CLI 在这里分发参数，协议验收另有实现。 |
| [provider_credentials.hpp](../../../src/cli/provider/provider_credentials.hpp) | 处理显式密钥环境变量覆盖并清除被替代的内联密钥，提供内联密钥弃用提示。 |

### src/cli/support

| 文件 | 用途 |
|---|---|
| [console.cpp](../../../src/cli/support/console.cpp) | 将 Console 接到标准输入、输出和错误流，提供读取、刷新和可测试的流访问。 |
| [console.hpp](../../../src/cli/support/console.hpp) | 定义 Console 的文本和错误输出接口。输入、输出流可注入，测试时可替换真实终端。 |
| [diagnostic_logging.cpp](../../../src/cli/support/diagnostic_logging.cpp) | 确定本地诊断日志目录与级别，检查符号链接和工作区重叠，配置日志并报告初始化失败。 |
| [diagnostic_logging.hpp](../../../src/cli/support/diagnostic_logging.hpp) | 声明从 CLI 参数配置诊断日志的适配入口，返回日志启用状态供错误展示使用。 |
| [entrypoint.hpp](../../../src/cli/support/entrypoint.hpp) | 声明各平台共用的 run_cli；Windows 参数转换在原生入口完成。 |

[导读目录](../README.md)

上一页：[日志与本地化](../core/logging-localization.md) · 下一页：[桌面客户端](../gui/README.md)
