# 模型接入

[导读目录](../README.md) · [核心模块](README.md)

配置解析、供应商协议、HTTP / SSE 和重试。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/infrastructure/model](../../../src/core/infrastructure/model) | 模型配置、provider 能力、三类协议、HTTP / SSE、请求预算与重试。 |

## 文件说明

### include/mint/infrastructure

| 文件 | 用途 |
|---|---|
| [config.hpp](../../../include/mint/infrastructure/config.hpp) | 声明模型配置的文件读取和 JSON 解析函数，独立配置与共享配置服务共用这些字段校验。 |
| [model_provider_client.hpp](../../../include/mint/infrastructure/model_provider_client.hpp) | 定义供应商/协议类型、能力配置、超时重试与响应限额、流式回调和具体模型客户端；同时声明无网络的演示客户端。 |

### include/mint/ports

| 文件 | 用途 |
|---|---|
| [model_client.hpp](../../../include/mint/ports/model_client.hpp) | 定义模型端口：查询当前工具集下的请求预算，并用会话消息和工具定义取得统一 ModelReply。 |

### src/core/infrastructure/model

| 文件 | 用途 |
|---|---|
| [config.cpp](../../../src/core/infrastructure/model/config.cpp) | 从文件或 JSON 解析模型配置，校验供应商、协议、端点、能力及各类上限；普通字段先校验，后续失败时擦除已取得的密钥副本。 |
| [demo_model_client.cpp](../../../src/core/infrastructure/model/demo_model_client.cpp) | 按固定步骤返回工具调用和最终回复，用于演示文件探查到回答的流程；不请求供应商，也不模拟真实模型能力。 |
| [model_client.cpp](../../../src/core/infrastructure/model/model_client.cpp) | 实现 ModelProviderClient 的配置初始化，按显式配置、已知模型窗口和响应头计算请求预算；构建并检查请求大小后调用请求执行层。 |
| [model_http_transport.cpp](../../../src/core/infrastructure/model/model_http_transport.cpp) | 封装单次 libcurl 请求、认证头、超时和取消回调，采集 HTTP/速率头，并把有界响应字节交给普通响应缓存或 SSE 解码器。 |
| [model_http_transport.hpp](../../../src/core/infrastructure/model/model_http_transport.hpp) | 定义单次 HTTP 尝试的状态、响应体/流解码器、限流信息及错误载体，并声明传输运行时初始化与请求入口。 |
| [model_protocol.cpp](../../../src/core/infrastructure/model/model_protocol.cpp) | 按所选 adapter 调用三类供应商协议的请求构造与响应解析函数，是协议模块的入口。 |
| [model_protocol.hpp](../../../src/core/infrastructure/model/model_protocol.hpp) | 声明协议无关的请求构建、回复解析及增量 SSE 解码器接口，对外隐藏具体协议累积状态。 |
| [model_protocol_anthropic.cpp](../../../src/core/infrastructure/model/model_protocol_anthropic.cpp) | 累积 Anthropic 的 message/content-block 流事件，拼接文本、工具参数和思考状态，校验字节/数量上限并构造最终响应。 |
| [model_protocol_anthropic.hpp](../../../src/core/infrastructure/model/model_protocol_anthropic.hpp) | 声明 Anthropic 流事件累积器，提供事件投递、结束聚合和流式字节统计接口。 |
| [model_protocol_common.cpp](../../../src/core/infrastructure/model/model_protocol_common.cpp) | 实现协议间共用的输出预算、参数 JSON 解析、文本提取、供应商状态读取与错误说明处理。 |
| [model_protocol_internal.hpp](../../../src/core/infrastructure/model/model_protocol_internal.hpp) | 声明协议实现内部的输出预算器、供应商状态字段和三类请求/响应转换函数，供各适配文件共用。 |
| [model_protocol_request.cpp](../../../src/core/infrastructure/model/model_protocol_request.cpp) | 把统一会话和工具定义转换为 Chat Completions、Responses 或 Anthropic Messages 请求，按能力保留推理状态、重排工具结果并选择输出 token 字段。 |
| [model_protocol_response.cpp](../../../src/core/infrastructure/model/model_protocol_response.cpp) | 把三类非流式响应转换成文本、工具调用、用量和供应商续传状态，校验参数与输出限额；分别记录未报告缓存数据和已知零命中。 |
| [model_protocol_stream.cpp](../../../src/core/infrastructure/model/model_protocol_stream.cpp) | 解析分块 SSE 的行/事件边界，累积 Chat Completions 与 Responses 增量并分派 Anthropic 事件，处理终止、错误和各类响应大小限制。 |
| [model_provider_profile.cpp](../../../src/core/infrastructure/model/model_provider_profile.cpp) | 维护供应商端点与能力目录，严格解析 URL、选择协议及已知模型窗口，规范化认证来源并解析环境变量凭据。 |
| [model_provider_profile.hpp](../../../src/core/infrastructure/model/model_provider_profile.hpp) | 声明供应商/协议名称解析、端点规范化、明文环回端点判断及凭据校验/解析的内部入口。 |
| [model_request.cpp](../../../src/core/infrastructure/model/model_request.cpp) | 处理一次模型调用的传输、重试退避和限流等待，以及特定供应商的工具生成错误。成功后转换响应格式，并发送调用进度与元数据。 |
| [model_request.hpp](../../../src/core/infrastructure/model/model_request.hpp) | 声明模型调用接口，接收序列化后的请求体，负责重试、传输和成功响应处理；预算逻辑由客户端处理。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[数据、策略与运行控制](domain-runtime.md) · 下一页：[工作区工具](tools.md)
