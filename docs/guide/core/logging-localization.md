# 日志与本地化

[导读目录](../README.md) · [核心模块](README.md)

诊断日志、任务事件以及中英文消息。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/infrastructure/logging](../../../src/core/infrastructure/logging) | 脱敏诊断事件、终端日志和限额滚动文件日志。 |
| [src/core/localization](../../../src/core/localization) | 语言选择与消息格式化实现，以及构建时使用的消息头文件模板。 |

## 文件说明

### include/mint/infrastructure

| 文件 | 用途 |
|---|---|
| [diagnostic_log.hpp](../../../include/mint/infrastructure/diagnostic_log.hpp) | 定义诊断日志级别、控制台与本地滚动日志配置、保留限额及状态查询、事件输出、刷新和关闭接口。 |
| [event_log.hpp](../../../include/mint/infrastructure/event_log.hpp) | 声明 AgentEventSink 的 JSONL 文件实现，管理输出文件、事件序号及并发写入互斥；用于可回放的任务事件流。 |

### include/mint/localization

| 文件 | 用途 |
|---|---|
| [localization.hpp](../../../include/mint/localization/localization.hpp) | 提供中英文语言选择、类型化消息与占位符格式化接口，以及退出作用域时恢复语言的 ScopedLanguage。 |

### include/mint/ports

| 文件 | 用途 |
|---|---|
| [agent_event_sink.hpp](../../../include/mint/ports/agent_event_sink.hpp) | 定义 Agent 输出事件名称及 JSON 数据的抽象端口，使运行循环不依赖日志文件或 GUI。 |

### src/core/infrastructure/logging

| 文件 | 用途 |
|---|---|
| [diagnostic_log.cpp](../../../src/core/infrastructure/logging/diagnostic_log.cpp) | 管理线程安全的 spdlog 控制台、文件后端和当前配置，按级别输出筛选后的诊断事件。日志失败不影响业务执行。 |
| [diagnostic_log_file.cpp](../../../src/core/infrastructure/logging/diagnostic_log_file.cpp) | 在受控私有目录为当前进程创建滚动日志，按文件名、保留时间及总大小清理本项目日志，并返回文件输出故障状态。 |
| [diagnostic_log_internal.hpp](../../../src/core/infrastructure/logging/diagnostic_log_internal.hpp) | 声明日志后端、事件筛选、时间与 PID 格式、文件 logger 创建接口，供日志输出、字段处理和文件管理共用。 |
| [diagnostic_log_schema.cpp](../../../src/core/infrastructure/logging/diagnostic_log_schema.cpp) | 集中定义允许记录的事件和字段类型，对文本/程序名做限长与安全处理，过滤未授权字段并生成带时间、进程和线程信息的日志记录。 |

### src/core/infrastructure/persistence

| 文件 | 用途 |
|---|---|
| [event_log.cpp](../../../src/core/infrastructure/persistence/event_log.cpp) | 输出带时间戳和递增序号的任务 JSONL 事件；追加时检查既有日志并处理崩溃留下的不完整末行，保证后续事件可继续读取。 |

### src/core/localization

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/localization/CMakeLists.txt) | 把中英文 JSON 词典嵌入生成头，生成类型化消息/占位符 ID，再构建本地化库并暴露生成的头文件目录。 |
| [catalogs.hpp.in](../../../src/core/localization/catalogs.hpp.in) | 嵌入词典的 CMake 模板，将中英文词典分块生成为字符串数组，运行时无需依赖外部词典文件。 |
| [localization.cpp](../../../src/core/localization/localization.cpp) | 解析内嵌词典，按环境或显式设置选择语言，查找消息、替换占位符并实现作用域语言恢复。 |
| [message_ids.hpp.in](../../../src/core/localization/message_ids.hpp.in) | 生成 Message、Placeholder 枚举及对应词典键名，调用方使用编译期可检查的标识符。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[文件、路径与平台接口](platform.md) · 下一页：[命令行入口](../cli/README.md)
