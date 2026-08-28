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
4. 模型请求工具时，`ToolRegistry` 先检查权限，再执行本地操作；
5. 工具结果回到 Agent，下一轮模型请求基于真实结果继续；
6. 模型给出最终回答后，Agent 检查最新文件改动是否已经通过验证。

模型第一轮直接回答且没有待验证改动时，任务只需要一次 HTTP 请求。Agent Loop 是“按需重复”，不是固定轮数，也不是持续聊天会话。

## 代码分层

| 目录 | 职责 |
|---|---|
| `src/cli` | 参数解析、任务组装、终端输入输出 |
| `src/application` | Agent Loop、上下文控制和项目初始化流程 |
| `src/domain` | 权限、任务状态和变更记录等规则数据 |
| `src/tools` | 工具定义、权限检查、文件操作和变更集 |
| `src/infrastructure` | HTTP、进程、配置、日志和任务存储 |
| `src/runtime` | 超时和取消 |

依赖方向以 application 为中心：application 使用 domain 规则，通过 tools 和 infrastructure 接触文件、进程与网络。CLI 只负责把这些对象组装起来。

## 模型接口怎么适配

`provider` 表示服务方，`adapter` 表示请求协议。两者分开后，代理地址仍可使用 Groq 或 OpenAI 的协议规则，自建接口也可以单独声明能力。

~~~mermaid
flowchart LR
    config["config.json"] --> profile["ProviderProfile<br/>服务、协议、能力"]
    profile --> protocol["model_protocol<br/>统一消息转请求 JSON"]
    protocol --> http["model_http_transport<br/>libcurl / SSE"]
    http --> parse["统一 ModelReply"]
    parse --> agent["Agent Loop"]
~~~

解析规则只有三条：

1. `api.openai.com`、`api.groq.com` 和 `api.deepseek.com` 会按完整主机名识别；
2. 代理地址在配置中明确写 `provider`；
3. 其他地址按 `custom` 处理，可以显式填写 `capabilities`。

能力目录集中在 `model_provider_profile.cpp`，目前决定工具调用、流式输出、流式 usage、无状态推理续传和 token 上限字段。它描述的是 API 方言，不承诺某个模型始终可用。

`mint provider --config ...` 只离线解析配置，不读取密钥。`mint provider test --config ...` 才会读取密钥并发出请求；它绕过 Agent Loop 和工作区工具，用固定的两轮握手检查 function call、参数解析和工具结果续接。

## 关键文件

| 文件 | 负责什么 |
|---|---|
| [`src/cli/agent_command.cpp`](../../src/cli/agent_command.cpp) | 一次命令的主流程 |
| [`src/cli/agent_command_config.cpp`](../../src/cli/agent_command_config.cpp) | policy、路径和工具配置 |
| [`src/cli/agent_command_io.cpp`](../../src/cli/agent_command_io.cpp) | 审批、模型进度和运行信息 |
| [`src/cli/provider_command.cpp`](../../src/cli/provider_command.cpp) | provider 配置检查与显式验收入口 |
| [`src/cli/provider_acceptance.cpp`](../../src/cli/provider_acceptance.cpp) | 固定两轮工具握手、结果校验和脱敏统计 |
| [`src/application/agent.cpp`](../../src/application/agent.cpp) | 校验 Agent 参数并进入循环 |
| [`src/application/agent_loop.cpp`](../../src/application/agent_loop.cpp) | 循环推进、任务开始和结束 |
| [`src/application/agent_cycle.cpp`](../../src/application/agent_cycle.cpp) | 模型调用、工具执行和验证门禁 |
| [`src/application/agent_checkpoint.cpp`](../../src/application/agent_checkpoint.cpp) | checkpoint schema 校验与恢复 |
| [`src/tools/tool_registry.cpp`](../../src/tools/tool_registry.cpp) | 工具路由和执行前检查 |
| [`src/tools/change_transaction.cpp`](../../src/tools/change_transaction.cpp) | changeset 事务格式、回滚和 checkpoint 确认 |
| [`src/infrastructure/model_client.cpp`](../../src/infrastructure/model_client.cpp) | 模型客户端公共门面和配置校验 |
| [`src/infrastructure/model_provider_profile.cpp`](../../src/infrastructure/model_provider_profile.cpp) | provider 目录、端点识别、能力与凭据来源 |
| [`src/infrastructure/model_request.cpp`](../../src/infrastructure/model_request.cpp) | 请求重试、进度事件和结果元数据 |
| [`src/infrastructure/model_http_transport.cpp`](../../src/infrastructure/model_http_transport.cpp) | 单次 libcurl 请求、响应头和 SSE 数据接收 |
| [`src/infrastructure/model_protocol.cpp`](../../src/infrastructure/model_protocol.cpp) | Chat Completions / Responses 格式转换 |
| [`src/infrastructure/command_runner.cpp`](../../src/infrastructure/command_runner.cpp) | 固定命令契约、审批和结果组装 |
| [`src/infrastructure/command_sandbox.cpp`](../../src/infrastructure/command_sandbox.cpp) | 程序解析、危险启动器拦截和 OS 沙箱策略 |
| [`src/infrastructure/command_process.cpp`](../../src/infrastructure/command_process.cpp) | POSIX 子进程、超时、取消和资源检查编排 |
| [`src/infrastructure/command_process_tree.cpp`](../../src/infrastructure/command_process_tree.cpp) | macOS / Linux 子孙进程发现、计数和整树终止 |
| [`src/infrastructure/command_resource_monitor.cpp`](../../src/infrastructure/command_resource_monitor.cpp) | 三平台共用的工作区磁盘用量检查 |
| [`src/infrastructure/command_process_windows.cpp`](../../src/infrastructure/command_process_windows.cpp) | Windows `CreateProcessW`、AppContainer 启动属性、句柄隔离和 Job Object |
| [`src/infrastructure/command_appcontainer_windows.cpp`](../../src/infrastructure/command_appcontainer_windows.cpp) | Windows AppContainer profile、路径 DACL 和生命周期 |
| [`src/infrastructure/session_store.cpp`](../../src/infrastructure/session_store.cpp) | checkpoint 读写 |
| [`src/infrastructure/change_transaction_store.cpp`](../../src/infrastructure/change_transaction_store.cpp) | 事务日志原子写入和跨进程锁 |
| [`src/tools/workspace_tools.cpp`](../../src/tools/workspace_tools.cpp) | 工作区文件读取与修改 |
| [`src/tools/change_set.cpp`](../../src/tools/change_set.cpp) | 多文件变更、预检和回滚 |
| [`src/infrastructure/diagnostic_log.cpp`](../../src/infrastructure/diagnostic_log.cpp) | 不进入 stdout 的诊断日志 |

## 从哪里开始读源码

先顺着一次任务的调用链读：

~~~text
main.cpp
  -> command_line.cpp
  -> agent_command.cpp
  -> Agent::run
  -> agent_loop.cpp
  -> agent_cycle.cpp
  -> ToolRegistry / ModelClient
~~~

恢复逻辑从 `agent_checkpoint.cpp` 进入。普通工具根据 `in_flight_tool_call` 判断是否可重试；`apply_changeset` 再交给 `change_transaction.cpp` 对照事务日志和 checkpoint 中的事务 ID。

阅读循环时，在 `agent_run.hpp` 中先关注四个状态：`messages_` 是模型上下文，`pending_calls_` 是尚未执行的工具请求，`in_flight_call_` 是已经开始但结果未确认的请求，`result_` 保存轮数、工具记录、验证状态和最终回答。
