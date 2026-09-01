# 架构说明

[← 返回 README 文档树](../../README.md)

mint 是一个轻量的通用 Agent 内核。模型只能返回文字或工具请求，所有文件和进程操作都由本地代码执行。

## 一次任务怎么运行

~~~bash
./build/vcpkg-release/mint run --root . "修复失败的测试，改完重新跑测试"
~~~

这条命令对应下面的调用链：

~~~mermaid
flowchart TD
    main["main.cpp<br/>解析参数"] --> command["run_agent_command<br/>组装一次任务"]
    command --> store["ProjectStore<br/>创建或恢复任务"]
    command --> agent["Agent::run"]
    agent --> loop["run_agent_loop"]
    loop --> model["ModelClient<br/>请求模型"]
    model --> reply{"返回类型"}
    reply -- "工具请求" --> registry["ToolRegistry<br/>检查并执行"]
    registry --> local["文件工具或固定命令"]
    registry --> transaction["ChangeTransactionStore<br/>多文件事务日志"]
    local --> loop
    transaction -.->|事务 ID| store
    reply -- "最终回答" --> gate{"最新修改已验证？"}
    gate -- "否" --> loop
    gate -- "是或无需验证" --> result["输出结果并保存状态"]
    loop -.->|每轮保存| store
~~~

主要步骤是：

1. CLI 读取参数、项目配置和任务状态；
2. `run_agent_command` 创建模型客户端、工具注册表和任务控制器；
3. Agent 把任务和工具定义发给模型；
4. 模型请求工具时，`ToolRegistry` 先检查权限，再执行本地操作；命令前后的源码变化也进入统一账本；
5. 工具结果回到 Agent，下一轮模型请求基于真实结果继续；
6. 模型给出最终回答后，Agent 检查最新文件改动是否已经通过验证。

模型第一轮直接回答且没有待验证改动时，任务只需要一次 HTTP 请求。Agent Loop 是“按需重复”，不是固定轮数，也不是持续聊天会话。

## 代码分层

| 目录 | 职责 |
|---|---|
| `src/cli` | 参数解析、任务组装、终端输入输出 |
| `src/application/agent` | Agent Loop、上下文控制和 checkpoint 恢复 |
| `src/application/project_service.cpp` | 项目类型识别和初始策略生成 |
| `src/domain` | 权限、任务状态和变更记录等规则数据 |
| `include/mint/ports` | 模型、工具、会话、事件和停止信号的稳定接口 |
| `src/tools` | 工具定义、权限检查、文件操作和变更集 |
| `src/infrastructure/model` | Provider 配置、HTTP、协议转换和响应解析 |
| `src/infrastructure/command` | 进程启动、资源限制和平台沙箱 |
| `src/infrastructure/persistence` | 项目、会话、事件和变更事务存储 |
| `src/infrastructure/logging` | 结构化诊断日志 |
| `src/infrastructure/filesystem` | 输出路径校验和私有文件权限 |
| `src/runtime` | 审批结果、路径包含、超时、取消和终端文本等跨层运行时规则 |

`application` 只依赖规则、运行时工具和 ports，不包含 `tools` 或 `infrastructure` 的具体头文件。`ToolRegistry`、`ModelProviderClient`、`SessionStore`、`EventLog` 和 `TaskControl` 实现这些接口，由 CLI 统一组装。

目录边界同时也是编译边界。`mint::agent`、`mint::project`、`mint::model`、`mint::command`、`mint::persistence`、`mint::logging` 和 `mint::filesystem` 都能单独构建。`mint::application` 与 `mint::infrastructure` 只是兼容聚合入口，不再把实现编进一个大库。CLI 和 tools 直接声明实际依赖，新增模型代码不会触发命令模块重编译。

~~~mermaid
flowchart LR
    cli["CLI / composition root"] --> agent["mint::agent"]
    cli --> project["mint::project"]
    cli --> tools["mint::tools"]
    cli --> model["mint::model"]
    cli --> persistence["mint::persistence"]
    cli --> logging["mint::logging"]
    agent --> ports["mint::ports"]
    tools --> ports
    tools --> command["mint::command"]
    tools --> persistence
    model --> ports
    command --> logging
    model --> logging
    persistence --> filesystem["mint::filesystem"]
    logging --> filesystem
    ports --> domain["mint::domain"]
~~~

## 本地记录怎么分层

| 记录 | 用途 | 生命周期 |
|---|---|---|
| CLI stdout / stderr | 当前终端交互和机器协议 | 不保证持久化 |
| `events.jsonl` | 任务步骤、审批和验证证据 | 跟随任务保存 |
| `session.json` | 模型上下文、检查点和最终结果 | 跟随任务保存 |
| `logs/mint-*.jsonl` | 本地运行诊断 | 轮转并自动清理 |

诊断日志由 `src/infrastructure/logging` 统一写入。普通终端使用 warn 级别的 console logger，文件使用 info 级别的 rotating logger；显式 `--log-level` 会同时覆盖两者。每个进程创建独立文件，避免并发任务竞争同一个轮转文件。机器交互模式关闭 console logger，让 stderr 只承载 `mint_interaction` 控制消息。

磁盘记录是单行 JSON，包含 UTC 时间、级别、事件名、进程和线程 ID，以及按事件 schema 校验后的字段。未知事件直接丢弃，未知字段或类型不匹配的值只增加省略计数；字符串按 UTF-8 边界截断，单条记录最大 16 KiB。logger 不接受请求正文、模型内容、工具参数、命令输出或 diff。默认 file sink 创建失败时任务继续，显式 `--log-dir` 不可用时直接报错，避免用户误以为日志已经保存。

## 模型接口怎么适配

`provider` 表示服务方，`adapter` 表示请求协议。两者分开后，不需要为每家供应商复制一套协议实现；供应商目录只提供默认 endpoint、认证方式和能力差异。

Agent 依赖 `include/mint/ports/model_client.hpp` 中的 `ModelClient`，不知道 HTTP 和供应商实现。

~~~mermaid
flowchart LR
    config["config.json"] --> profile["ProviderProfile<br/>服务、协议、能力"]
    profile --> protocol["model_protocol<br/>统一消息转请求 JSON"]
    protocol --> http["model_http_transport<br/>libcurl / SSE"]
    http --> parse["统一 ModelReply"]
    parse --> agent["Agent Loop"]
~~~

概念上只有 OpenAI 系与 Anthropic 系两个协议族；OpenAI 系内部有两种不同的 wire shape，因此代码落成三个 adapter：

| adapter | 常见服务 |
|---|---|
| `chat_completions` | Groq、DeepSeek、Gemini、Kimi 和兼容代理 |
| `responses` | OpenAI / Codex、xAI / Grok |
| `anthropic_messages` | Anthropic / Claude |

解析规则只有三条：

1. 内置 provider 可省略 endpoint，由能力目录选择 adapter 对应的官方地址；
2. 旧配置中的官方根地址会自动补全，完整 endpoint 保持原样；
3. 代理和其他地址使用完整 `endpoint`，按显式 `provider` 或 `custom` 处理。

能力目录集中在 `model_provider_profile.cpp`，决定工具调用、`tool_choice`、流式 usage、推理续传和 token 上限字段。DeepSeek V4 默认思考模式会保留并回传 `reasoning_content`、补齐工具消息的空 `content`，同时省略它不接受的显式 `tool_choice`；协议层不按域名写分支。能力目录描述的是 API 方言，不承诺某个模型始终可用。

三种 adapter 最终都写入同一个 `ModelUsage`。OpenAI 风格的 `cached_tokens`、DeepSeek 的 `prompt_cache_hit_tokens` 和 Anthropic 的 `cache_read_input_tokens` 会归一为缓存输入 tokens；命中率统一按累计缓存输入 ÷ 累计输入计算。输入为零时命中率是 `null`，不会用额外请求探测缓存。

每轮请求前，Agent 会向 `ModelClient` 查询请求预算。`max_request_tokens` 为 `0` 时先使用 8000 tokens，成功响应带有 `x-ratelimit-limit-tokens` 后采用更低的服务端上限。输出上限、工具定义和安全余量会先被扣除，剩余空间才交给上下文压缩器。默认估算按保守的 2 bytes/token；它适配不同 tokenizer 时只能降低超限概率，配置可用 `request_token_estimate_bytes_per_token` 继续收紧。

传输层只允许远程 HTTPS 和本机回环 HTTP。HTTP 正文、SSE 缓冲、模型文本、推理内容和工具调用分别受限，避免异常或恶意接口让本地进程无界分配内存；高级配置可以用 `response_limits` 继续收紧默认值。

`mint provider --config ...` 只离线解析配置，不读取密钥。`mint provider test --config ...` 才会读取密钥并发出请求；它绕过 Agent Loop 和工作区工具，用固定的两轮握手检查 function call、参数解析和工具结果续接。

## 关键文件

| 文件 | 负责什么 |
|---|---|
| `include/mint/ports` | Agent 使用的模型、工具、会话、事件和停止接口 |
| `include/mint/ports/model_client.hpp` | 统一模型调用接口 |
| `src/cli/agent_command.cpp` | 组装一次 CLI 任务 |
| `src/cli/agent_event_router.cpp` | 把 Agent 事件分发到任务记录与诊断日志 |
| `src/application/agent/agent_loop.cpp` | 启动、推进和结束 Agent Loop |
| `src/application/agent/agent_cycle.cpp` | 模型调用、工具执行和验证门禁 |
| `src/application/agent/agent_checkpoint.cpp` | checkpoint 校验与恢复 |
| `src/application/agent/agent_execution.cpp` | 工具与验证执行统计 |
| `src/application/agent/agent_model_summary.cpp` | 模型调用、Token 和缓存统计 |
| `src/application/agent/agent_reporting.cpp` | 最终状态、终端摘要和机器结果 |
| `src/tools/tool_registry.cpp` | 工具路由、权限检查和变更账本 |
| `src/tools/tool_registry_command.cpp` | 命令前后快照、变化归档和失败关闭 |
| `src/tools/path_identity.cpp` | 跨平台路径、大小写别名和目录项身份 |
| `src/runtime/path.cpp` | 已规范化绝对路径的跨平台词法包含判断 |
| `src/tools/workspace_change_tracker.cpp` | 命令前后的工作区快照与变化分类 |
| `src/infrastructure/model/model_protocol.cpp` | 三种模型协议的稳定分派入口 |
| `src/infrastructure/model/model_protocol_request.cpp` | canonical 消息与工具定义转换为 provider 请求 |
| `src/infrastructure/model/model_protocol_response.cpp` | provider 响应、工具调用与用量归一化 |
| `src/infrastructure/model/model_protocol_stream.cpp` | Chat Completions 与 Responses 的 SSE 增量解码 |
| `src/infrastructure/model/model_protocol_anthropic.cpp` | Anthropic Messages 的流式状态机 |
| `src/infrastructure/command/command_runner.cpp` | 固定命令、审批和结果组装 |
| `src/infrastructure/command/command_sandbox.cpp` | 程序解析和平台沙箱策略 |
| `src/infrastructure/persistence/project_store.cpp` | 项目与任务状态存储 |

## 从哪里开始读源码

先顺着一次任务的调用链读：

~~~text
main.cpp
  -> command_line.cpp
  -> agent_command.cpp
  -> Agent::run
  -> agent/agent_loop.cpp
  -> agent/agent_cycle.cpp
  -> ToolRegistry / ModelClient
~~~

恢复逻辑从 `agent/agent_checkpoint.cpp` 进入。普通工具根据 `in_flight_tool_call` 判断是否可重试；`apply_changeset` 再交给 `change_transaction.cpp` 对照事务日志和 checkpoint 中的事务 ID。

阅读循环时，在 `src/application/agent/agent_run.hpp` 中先关注四个状态：`conversation_` 是模型上下文，`pending_calls_` 是尚未执行的工具请求，`in_flight_call_` 是已经开始但结果未确认的请求，`result_` 保存轮数、工具记录、验证状态和最终回答。

最新修改通过 verification recipe 后，循环会在整批工具结果都返回后提示模型收尾。它只减少无意义的继续调用；是否允许结束仍由验证门判断，后续命令失败时任务依然不能完成。
