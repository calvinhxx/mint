# Agent 循环

[导读目录](../README.md) · [核心模块](README.md)

任务开始、模型调用、工具队列、检查点和结果输出；也包括项目类型探测。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/application](../../../src/core/application) | Agent 任务流程与项目策略探测。 |
| [src/core/application/agent](../../../src/core/application/agent) | 单任务循环，按上下文、模型请求、工具执行、检查点与结果报告拆分。 |

## 文件说明

### include/mint/application

| 文件 | 用途 |
|---|---|
| [agent.hpp](../../../include/mint/application/agent.hpp) | 定义 Agent 的运行入口和结果结构，包括轮数、预算、执行统计、模型用量、变更与验证结果。模型、工具、停止信号、事件和会话服务均可注入。 |
| [project_service.hpp](../../../include/mint/application/project_service.hpp) | 声明项目探测接口，返回项目类型、建议任务策略和判定所依据的文件，不保存项目状态。 |

### src/core/application/agent

| 文件 | 用途 |
|---|---|
| [agent.cpp](../../../src/core/application/agent/agent.cpp) | 校验 Agent 选项和服务组合，保存外部依赖；run() 生成系统提示后将一次任务交给内部运行循环。 |
| [agent_checkpoint.cpp](../../../src/core/application/agent/agent_checkpoint.cpp) | 生成检查点，恢复前检查版本、任务能力和预算是否一致，再恢复消息、统计和待执行工具，并处理未完成的工具调用与变更事务。 |
| [agent_checkpoint.hpp](../../../src/core/application/agent/agent_checkpoint.hpp) | 声明恢复后的会话状态及检查点生成、恢复接口，将文档转换与运行循环分开。 |
| [agent_context.cpp](../../../src/core/application/agent/agent_context.cpp) | 按完整工具调用组压缩上下文，保留有用的工具结果，供应商状态保持原样。优先在已有请求内容后追加，预算不足时才重建，并统计前缀复用。 |
| [agent_context.hpp](../../../src/core/application/agent/agent_context.hpp) | 定义上下文压缩结果和 ContextWindow 的请求状态，分别记录完整历史大小、发送大小、丢弃组数与前缀复用指标。 |
| [agent_conversation.cpp](../../../src/core/application/agent/agent_conversation.cpp) | 实现会话消息容器的开始、恢复和追加，按 system/user/assistant/tool 角色保存历史，并为工具结果绑定调用 ID。 |
| [agent_conversation.hpp](../../../src/core/application/agent/agent_conversation.hpp) | 声明内部 Conversation 容器，统一消息结构的创建与追加入口，供检查点及模型请求读取完整历史。 |
| [agent_cycle.cpp](../../../src/core/application/agent/agent_cycle.cpp) | 处理一轮模型预算计算、上下文准备、回复和逐项工具执行。工具执行前后保存检查点，有待验证的变更时阻止任务直接完成。 |
| [agent_execution.cpp](../../../src/core/application/agent/agent_execution.cpp) | 从工具结果累计文件和命令执行统计，按最后变更与验证命令顺序判断验证状态，并序列化/恢复执行摘要和工具调用。 |
| [agent_execution.hpp](../../../src/core/application/agent/agent_execution.hpp) | 声明执行统计累积、验证状态计算及工具调用/执行摘要的 JSON 转换，供循环与检查点共用。 |
| [agent_loop.cpp](../../../src/core/application/agent/agent_loop.cpp) | 驱动一次任务的初始化或恢复、停止和预算检查、顺序工具执行、模型调用与结束处理，并保存检查点、发送任务事件。 |
| [agent_loop.hpp](../../../src/core/application/agent/agent_loop.hpp) | 声明内部 run_agent_loop 入口，接收抽象服务、选项、用户请求与系统提示，返回统一 AgentResult。 |
| [agent_model_summary.cpp](../../../src/core/application/agent/agent_model_summary.cpp) | 统一模型调用、重试、流式和 token 统计格式并累加，区分未报告用量和已知缓存值，生成预算执行状态，支持从摘要恢复。 |
| [agent_model_summary.hpp](../../../src/core/application/agent/agent_model_summary.hpp) | 声明模型用量、调用元数据、累计预算与摘要的转换和输出函数，供循环、事件和检查点共用。 |
| [agent_prompt.cpp](../../../src/core/application/agent/agent_prompt.cpp) | 根据实际工具能力组装系统提示，说明工作区范围、证据复用、文件内容不可信、写入/命令授权及完成前验证要求。 |
| [agent_reporting.cpp](../../../src/core/application/agent/agent_reporting.cpp) | 从工具账本收集最终变更，格式化执行/模型摘要，生成最终结果 JSON，并限制事件中保留的工具参数、结果和错误摘要。 |
| [agent_reporting.hpp](../../../src/core/application/agent/agent_reporting.hpp) | 声明最终状态收集、文本报告和工具事件摘要接口，负责展示及事件字段的安全裁剪。 |
| [agent_run.hpp](../../../src/core/application/agent/agent_run.hpp) | 定义一次运行的状态和操作，保存完整会话、本轮请求、工具队列、在途调用、统计及恢复标记；实现分布在 loop 与 cycle。 |

### src/core/application

| 文件 | 用途 |
|---|---|
| [project_service.cpp](../../../src/core/application/project_service.cpp) | 通过 CMakeLists.txt、Cargo.toml 或 package.json 识别项目，生成可写路径、固定构建/测试配方和运行预算建议；未知项目回退为只读。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[核心模块](README.md) · 下一页：[数据、策略与运行控制](domain-runtime.md)
