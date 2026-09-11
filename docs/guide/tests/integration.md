# 集成测试

[导读目录](../README.md) · [测试](README.md)

Agent、工具、模型、命令与存储共同运行时的行为。

## 源码目录

| 目录 | 用途 |
|---|---|
| [tests/integration](../../../tests/integration) | 跨 Agent、工具、模型、命令和存储的实际行为回归。 |

## 文件说明

### tests/integration

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../tests/integration/CMakeLists.txt) | 将 Agent、事务、命令、工作区和运行文件的集成测试汇入 mint_tests，链接真实功能模块及合成模型、HTTP、命令测试支撑，并注册 integration 用例。 |
| [agent_config_paths_tests.cpp](../../../tests/integration/agent_config_paths_tests.cpp) | 检查保护列表只包含共享配置文件和私有暂存目录，不会误屏蔽它们的公共父目录；显式覆盖的配置文件也必须受保护。 |
| [agent_context_tests.cpp](../../../tests/integration/agent_context_tests.cpp) | 测试请求预算、上下文裁剪和稳定前缀，检查后续用户约束、失败证据、签名续接及工具调用配对得到保留。恢复时从完整 checkpoint 重建上下文，并阻止重复读取项目材料的循环。 |
| [agent_loop_tests.cpp](../../../tests/integration/agent_loop_tests.cpp) | 用确定性模型测试只读任务完成、安全工具错误回传、取消与协议故障的区别，以及 JSONL/机器结果和运行命令取消的基本生命周期。 |
| [agent_session_tests.cpp](../../../tests/integration/agent_session_tests.cpp) | 检查从稳定 checkpoint 恢复 pending 工具时只执行一次，并保留历史用量、变更和验证状态。能力变更与有歧义的副作用重放会被拒绝；明确重试被允许，并兼容旧 schema。 |
| [agent_test_models.hpp](../../../tests/integration/agent_test_models.hpp) | 提供确定性只读模型、规范工具回复构造器，以及验证就绪状态和最新工具结果的查询，供多个 Agent 集成测试复用，不依赖线上模型。 |
| [agent_token_budget_tests.cpp](../../../tests/integration/agent_token_budget_tests.cpp) | 检查累计 Token 上限的精确边界：达到预算后不再执行待定工具，但仍接受最终回答。还覆盖溢出饱和、缺失 usage 时的真实状态，以及 checkpoint 中累计用量的继承。 |
| [agent_verification_tests.cpp](../../../tests/integration/agent_verification_tests.cpp) | 检查写入或补丁之后必须通过合格的验证命令，验证失败后需要修复，由最新命令决定验证状态；审批被拒绝不能算作验证。最后一轮验证成功后，仍要给模型生成最终回答的机会。 |
| [change_transaction_tests.cpp](../../../tests/integration/change_transaction_tests.cpp) | 测试崩溃后回滚完整或部分文件写入、保留 checkpoint 已确认的写入、拒绝外部重写、任务锁排他和日志路径别名防护，也覆盖在途 changeset 的安全重放。 |
| [command_change_tracking_tests.cpp](../../../tests/integration/command_change_tracking_tests.cpp) | 检查命令造成的源码变更可审计、受 policy 限制，且命令不能自证验证通过。覆盖快照预算、大小写及软硬链接别名、二进制、权限和目录变化、非法 UTF-8 名称及事务记录。 |
| [command_runner_tests.cpp](../../../tests/integration/command_runner_tests.cpp) | 测试授权命令/recipe、环境和句柄隔离、超时取消、资源预算与 OS 沙箱。覆盖监测异常后的进程终止、回收和管道关闭，沙箱与直接启动各自独立，以及 Windows 脚本拒绝。 |
| [model_client_tests.cpp](../../../tests/integration/model_client_tests.cpp) | 测试模型配置加载和错误约束，用可控失败和时间参数检查请求重试、退避及 Retry-After 行为。 |
| [runtime_files_tests.cpp](../../../tests/integration/runtime_files_tests.cpp) | 检查事件日志恢复后序号继续递增，崩溃留下的半行与后续记录分开。日志/session 路径不能是目录或软硬链接，拒绝时也不能覆盖原目标。 |
| [terminal_output_tests.cpp](../../../tests/integration/terminal_output_tests.cpp) | 检查人工终端中的模型最终回答和流式片段会转义控制字符，同时 JSON/机器结果保留原始语义，防止恶意输出操控终端。 |
| [tool_registry_tests.cpp](../../../tests/integration/tool_registry_tests.cpp) | 测试文件列举、读取、搜索工具及已发布的参数契约，检查运行预算、路径写入授权和补丁前置条件，也覆盖批变更的原子执行与失败回滚。 |
| [workspace_scan_tests.cpp](../../../tests/integration/workspace_scan_tests.cpp) | 测试遍历剪枝，以及总量、字节和取消限制，检查空保护项不会屏蔽根目录。覆盖隐藏/空目录计数、搜索行号、跳过二进制文件，以及硬链接和父路径别名的保护规则。 |

[导读目录](../README.md) · [测试](README.md)

上一页：[单元测试](unit.md) · 下一页：[契约测试](contracts.md)
