# 核心模块

[导读目录](../README.md)

不依赖 Qt 的 Agent 执行库，按功能查接口和实现。

## 按主题阅读

| 文档 | 内容 |
|---|---|
| [Agent 循环](agent.md) | 任务开始、模型调用、工具队列、检查点和结果输出；也包括项目类型探测。 |
| [数据、策略与运行控制](domain-runtime.md) | 消息与预算类型、任务策略、审批、取消和终端文本转义。 |
| [模型接入](model.md) | 配置解析、供应商协议、HTTP / SSE 和重试。 |
| [工作区工具](tools.md) | 工具注册、参数检查、读取、搜索和文件编辑。 |
| [变更与恢复](changes.md) | 变更账本、多文件事务、命令前后快照和崩溃恢复。 |
| [配置与存储](config-storage.md) | GUI / CLI 共享配置，以及项目、任务和检查点的保存。 |
| [命令与沙箱](commands.md) | 命令校验、审批、资源限制和各平台的进程、沙箱实现。 |
| [文件、路径与平台接口](platform.md) | 原生文件操作、路径转换、系统查询和私有目录权限。 |
| [日志与本地化](logging-localization.md) | 诊断日志、任务事件以及中英文消息。 |

## 源码目录

| 目录 | 用途 |
|---|---|
| [include](../../../include) | 对外提供的 C++ 头文件；实现与私有接口留在 src。 |
| [include/mint](../../../include/mint) | 按职责组织 mint 公共 C++ API。 |
| [include/mint/application](../../../include/mint/application) | Agent 运行与项目策略探测的用例入口。 |
| [include/mint/domain](../../../include/mint/domain) | 跨模块共享的数据、授权策略、预算和变更账本类型。 |
| [include/mint/infrastructure](../../../include/mint/infrastructure) | 模型客户端、配置、命令、存储和日志组件的公共接口。 |
| [include/mint/localization](../../../include/mint/localization) | 中英文消息与类型化占位符的公共接口。 |
| [include/mint/platform](../../../include/mint/platform) | 跨平台文件、路径和系统查询接口，使用标准库类型。 |
| [include/mint/ports](../../../include/mint/ports) | Agent 所需的模型、工具、停止信号、会话存储和事件接口。 |
| [include/mint/runtime](../../../include/mint/runtime) | 任务取消、时间预算、审批、路径规则和终端转义。 |
| [include/mint/tools](../../../include/mint/tools) | 工作区工具注册器及其授权选项。 |
| [src/core](../../../src/core) | 不依赖 Qt 的 Agent、工具、策略、模型、存储和系统实现。 |
| [src/core/infrastructure](../../../src/core/infrastructure) | 命令、文件系统、日志、模型、存储和配置的具体实现。 |

## 文件说明

### include/mint

| 文件 | 用途 |
|---|---|
| [version.hpp](../../../include/mint/version.hpp) | 集中定义产品版本及会话、工作区变更和策略的持久化 schema 版本号，供读取兼容性检查使用。 |

### src/core

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/CMakeLists.txt) | 按依赖顺序加入核心各层，建立仅含接口的 mint::ports 与可供嵌入者使用的聚合目标 mint::core；当前 CLI 直接链接各功能库。 |

### src/core/application

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/application/CMakeLists.txt) | 分别构建 Agent 用例库 mint::agent 与项目策略探测库 mint::project；Agent 依赖领域模型、端口和运行辅助层，而不直接链接具体模型或工具实现。 |

### src/core/infrastructure

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/infrastructure/CMakeLists.txt) | 按职责建立文件系统、日志、命令、模型、持久化和共享设置库；按系统选择原生文件权限、进程与沙箱实现，并隔离 curl、spdlog 和系统库依赖。 |

[导读目录](../README.md)

上一页：[项目进度](../../project/roadmap.md) · 下一页：[Agent 循环](agent.md)
