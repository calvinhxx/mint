# 单元测试

[导读目录](../README.md) · [测试](README.md)

参数、配置、日志、路径、平台文件和运行设置。

## 源码目录

| 目录 | 用途 |
|---|---|
| [tests/unit](../../../tests/unit) | 参数、终端、本地化、配置服务、路径和平台文件原语等单元测试。 |

## 文件说明

### tests/unit

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../tests/unit/CMakeLists.txt) | 组织 CLI、控制台、日志、协议、语言和运行设置单测，另外创建共享模型配置与平台文件测试套件，分别管理模块依赖和验证范围。 |
| [command_line_tests.cpp](../../../tests/unit/command_line_tests.cpp) | 测试 CLI 子命令和选项组合、JSONL 交互条件、取消文件约束、环境变量凭据入口、日志参数必填及任务 Token 上限，并检查有歧义的隐式 exec 调用被拒绝。 |
| [console_tests.cpp](../../../tests/unit/console_tests.cpp) | 检查可注入控制台分离标准输出和错误输出，正确处理 EOF。安全渲染应保留普通文本，转义终端控制、双向字符和非法 UTF-8。 |
| [diagnostic_log_tests.cpp](../../../tests/unit/diagnostic_log_tests.cpp) | 测试结构化日志级别、字段白名单、UTF-8 截断、私有轮换和有界保留。覆盖目录/软链接权限拒绝，检查日志失败不破坏主流程、交互 stderr 不被污染。 |
| [interaction_protocol_tests.cpp](../../../tests/unit/interaction_protocol_tests.cpp) | 测试版本化 JSONL 控制行、审批证据和决定解析。无效或关闭的通道必须安全失败，取消与拒绝须区分；人工预览转义控制字符，机器预览保持一行合法 JSON。 |
| [localization_tests.cpp](../../../tests/unit/localization_tests.cpp) | 测试中英文目录和常见语言别名的选择、未知语言不改变当前设置，以及类型化占位符按目标语言正确替换。 |
| [model_config_command_tests.cpp](../../../tests/unit/model_config_command_tests.cpp) | 测试 config show/set/import 的参数和版本条件、共享默认/显式覆盖、一次迁移，以及 CAS 拒绝旧版本。检查错误和机器输出不泄漏密钥，配置文件不被执行产物覆盖。 |
| [model_config_service_tests.cpp](../../../tests/unit/model_config_service_tests.cpp) | 测试私有共享配置与凭据绑定、原子保存/锁/CAS、旧配置迁移和运行快照。覆盖切换端点时不转移密钥、并发修改、非法输入，以及软硬链接和权限攻击。 |
| [path_tests.cpp](../../../tests/unit/path_tests.cpp) | 测试纯路径工具的父子包含关系、前缀碰撞、绝对路径规范化和保守大小写语义，检查相似的路径文本不会被误判为目录包含。 |
| [platform_file_tests.cpp](../../../tests/unit/platform_file_tests.cpp) | 测试原生文件入口的独占创建、稳定句柄锁、持久化原子替换和临时文件不覆盖。检查目录/FIFO/软硬链接会被拒绝，并核对文件系统大小写身份。 |
| [platform_tests.cpp](../../../tests/unit/platform_tests.cpp) | 测试统一平台入口的 Unicode 原生路径往返、空/相对路径转换、UTC 时间和稳定的正数进程 ID，使调用方无需使用平台宏。 |
| [runtime_settings_tests.cpp](../../../tests/unit/runtime_settings_tests.cpp) | 测试各运行预算的默认值、部分覆盖和完整 JSON 往返。未知字段和越界数值会被拒绝，防止配置错误在没有提示的情况下改变执行限制。 |

[导读目录](../README.md) · [测试](README.md)

上一页：[测试](README.md) · 下一页：[集成测试](integration.md)
