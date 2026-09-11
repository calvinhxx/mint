# 契约测试

[导读目录](../README.md) · [测试](README.md)

策略恢复、项目流程、模型协议和用量统计。

## 源码目录

| 目录 | 用途 |
|---|---|
| [tests/contract](../../../tests/contract) | 验证外部模型、策略与恢复、用量统计、项目工作流等系统契约。 |

## 文件说明

### tests/contract

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../tests/contract/CMakeLists.txt) | 分别创建策略恢复、项目流程、provider 协议/目录/用量/配置的契约测试目标，链接各自需要的模块和本机测试服务器，并设置对应的 CTest 标签。 |
| [model_usage_tests.cpp](../../../tests/contract/model_usage_tests.cpp) | 检查缓存命中率以 prompt tokens 为分母，单次与累计统计一致。用量或缓存覆盖缺失时必须保持未知，不能显示成 0%；旧 checkpoint 也不能凭空补出统计覆盖。 |
| [policy_and_recovery_tests.cpp](../../../tests/contract/policy_and_recovery_tests.cpp) | 验证修改任务必须完成指定的验证 recipe 才能结束，checkpoint 中只读的在途调用可以自动重放。测试使用合成模型和命令，结果可重复。 |
| [project_workflow_tests.cpp](../../../tests/contract/project_workflow_tests.cpp) | 测试项目识别、策略建议、托管任务状态和恢复记录的持久化，以及每次托管操作前对旧版 Windows 私有状态权限的安全迁移。 |
| [provider_acceptance_tests.cpp](../../../tests/contract/provider_acceptance_tests.cpp) | 用脚本模型检查两次请求的工具握手契约、协议不匹配时的拒绝行为和结果脱敏。还检查缺少缓存分母时不输出伪命中率；测试不调用真实 provider。 |
| [provider_catalog_tests.cpp](../../../tests/contract/provider_catalog_tests.cpp) | 测试 DeepSeek usage/cache 字段归一化、Anthropic 工具结果/流/签名思考内容和认证头，以及 Gemini 工具调用中 thought signature 的保留。 |
| [provider_profile_tests.cpp](../../../tests/contract/provider_profile_tests.cpp) | 测试模板、adapter/endpoint 选择、官方型号预算、显式限制优先级和回环明文例外。检查矛盾或不安全配置会被拒绝，CLI 离线检查与显式握手被正确区分，且凭据不泄漏。 |
| [provider_protocol_tests.cpp](../../../tests/contract/provider_protocol_tests.cpp) | 用本机 HTTP/SSE 服务测试 Chat/Responses 编解码、工具往返、流分片、大小/索引上限和预算反馈。还测试断流、限流、指定失败重试、代理隔离和 CLI 握手脱敏。 |

[导读目录](../README.md) · [测试](README.md)

上一页：[集成测试](integration.md) · 下一页：[桌面测试](gui.md)
