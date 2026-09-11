# 数据、策略与运行控制

[导读目录](../README.md) · [核心模块](README.md)

消息与预算类型、任务策略、审批、取消和终端文本转义。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/domain](../../../src/core/domain) | 策略加载与校验、预算规则和变更账本，使用 JSON 和少量文件 I/O。 |
| [src/core/runtime](../../../src/core/runtime) | 取消和时间预算、路径包含关系、终端安全转义的实现。 |

## 文件说明

### include/mint/domain

| 文件 | 用途 |
|---|---|
| [model.hpp](../../../include/mint/domain/model.hpp) | 定义各层共用的 JSON、工具调用、模型回复、用量和请求上限数据，以及 token 估算、缓存命中率和预算来源规则。 |
| [runtime_settings.hpp](../../../include/mint/domain/runtime_settings.hpp) | 定义运行设置、默认值和允许范围，涵盖任务、工具与命令资源，并声明校验、JSON 解析和序列化函数。 |
| [task_policy.hpp](../../../include/mint/domain/task_policy.hpp) | 定义固定命令配方和任务授权策略，包括可写路径、额外只读路径、验证要求、资源预算及策略指纹，并声明策略加载接口。 |

### include/mint/ports

| 文件 | 用途 |
|---|---|
| [stop_token.hpp](../../../include/mint/ports/stop_token.hpp) | 定义停止信号的最小查询端口，以停止原因字符串向 Agent 传递取消或超时状态。 |

### include/mint/runtime

| 文件 | 用途 |
|---|---|
| [command_approval.hpp](../../../include/mint/runtime/command_approval.hpp) | 定义审批请求和结果来源，分别记录用户拒绝、任务取消、非法响应和交互关闭。 |
| [task_control.hpp](../../../include/mint/runtime/task_control.hpp) | 声明共享任务取消与时间预算控制器，兼容原子取消标志和外部信号标志，并实现 StopToken。 |
| [terminal_text.hpp](../../../include/mint/runtime/terminal_text.hpp) | 声明终端文本转义函数，正文版本保留换行，字段版本限制为单行，防止控制字符和双向排版字符直接生效。 |

### src/core/domain

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/domain/CMakeLists.txt) | 构建领域库 mint::domain，汇集变更账本、运行设置与任务策略，公开 JSON 类型依赖并私有链接本地化。 |
| [runtime_settings.cpp](../../../src/core/domain/runtime_settings.cpp) | 解析工具与命令资源设置，拒绝未知字段和越界值，将类型化设置输出为 JSON。 |
| [task_policy.cpp](../../../src/core/domain/task_policy.cpp) | 有界读取和解析策略文档，校验写路径、固定命令配方与各类预算，拒绝额外字段，并计算用于恢复一致性检查的稳定指纹。 |

### src/core/runtime

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/runtime/CMakeLists.txt) | 构建 mint::runtime，对外依赖 ports，内部使用 platform 的路径组件规则。 |
| [task_control.cpp](../../../src/core/runtime/task_control.cpp) | 实现任务取消、外部信号检查、总时间预算到期判断，以及已用/剩余时间和停止原因查询。 |
| [terminal_text.cpp](../../../src/core/runtime/terminal_text.cpp) | 解码 UTF-8 并把控制字节、非法编码和双向控制字符显式转义；单行字段版本同时转义换行和制表符。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[Agent 循环](agent.md) · 下一页：[模型接入](model.md)
