# 工作区工具

[导读目录](../README.md) · [核心模块](README.md)

工具注册、参数检查、读取、搜索和文件编辑。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/tools](../../../src/core/tools) | Agent 的工作区工具，分别处理协议分发、操作执行和事务。 |
| [src/core/tools/registry](../../../src/core/tools/registry) | 工具名、参数契约、Schema、能力清单及执行路由。 |
| [src/core/tools/workspace](../../../src/core/tools/workspace) | 文件读取搜索、受控编辑、路径身份、敏感路径和有界目录扫描。 |

## 文件说明

### include/mint/ports

| 文件 | 用途 |
|---|---|
| [tool_runtime.hpp](../../../include/mint/ports/tool_runtime.hpp) | 声明工具执行和工作区账本两个端口，包含工具能力、变更快照、恢复与持久事务协调接口，并将二者组合为 AgentTools。 |

### include/mint/tools

| 文件 | 用途 |
|---|---|
| [tool_registry.hpp](../../../include/mint/tools/tool_registry.hpp) | 声明 AgentTools 的工作区实现及授权选项，聚合文件工具、命令审批、路径保护、变更账本和持久事务恢复能力。 |

### src/core/tools

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/tools/CMakeLists.txt) | 构建工作区工具库 mint::tools，汇集注册表、文件操作、变更跟踪和事务实现，私有连接命令、持久化与平台库。 |

### src/core/tools/registry

| 文件 | 用途 |
|---|---|
| [tool_arguments.cpp](../../../src/core/tools/registry/tool_arguments.cpp) | 统一检查工具参数对象、允许字段和必需字符串，并把错误结果及可能含非法 UTF-8 的 JSON 安全序列化。 |
| [tool_arguments.hpp](../../../src/core/tools/registry/tool_arguments.hpp) | 声明工具参数校验、标准错误结果和 JSON 输出辅助函数，供各具体工具复用。 |
| [tool_catalog.cpp](../../../src/core/tools/registry/tool_catalog.cpp) | 生成只读和写入工具的 JSON schema，按权限选择可见工具，同时生成不会包含整段补丁内容的调用摘要。 |
| [tool_catalog.hpp](../../../src/core/tools/registry/tool_catalog.hpp) | 声明工作区工具定义生成与工具调用摘要接口，分离 schema/展示元数据和实际执行。 |
| [tool_contract.hpp](../../../src/core/tools/registry/tool_contract.hpp) | 定义工具目录深度、搜索大小、变更数量、预览和恢复上限，供 schema 与执行逻辑共用。 |
| [tool_names.hpp](../../../src/core/tools/registry/tool_names.hpp) | 集中定义八个内置工具协议名称，供注册、分派和状态判断使用。 |
| [tool_registry.cpp](../../../src/core/tools/registry/tool_registry.cpp) | 初始化工作区、授权范围、保护路径、命令运行器和变更存储；暴露能力/工具定义，统一检查取消并按名称分派工具调用。 |
| [tool_registry_command.cpp](../../../src/core/tools/registry/tool_registry_command.cpp) | 在命令执行前后采集和核对工作区快照，将可审计文件变化写入账本；把越权、无法审计或追踪失败标为完整性风险。 |
| [tool_registry_command.hpp](../../../src/core/tools/registry/tool_registry_command.hpp) | 定义命令执行前工作区基线的存储状态和追踪失败代码，供注册表与快照实现共用。 |

### src/core/tools/workspace

| 文件 | 用途 |
|---|---|
| [file_editor.cpp](../../../src/core/tools/workspace/file_editor.cpp) | 实现单文件 apply_patch 的创建和唯一精确字符串替换，校验授权、链接、UTF-8 和大小，安全写入后记录账本；不解析通用 unified diff。 |
| [file_support.cpp](../../../src/core/tools/workspace/file_support.cpp) | 实现 UTF-8/NUL 与硬链接检查、路径显示及安全文件替换/删除；使用独占临时文件并只清理本次拥有的条目。 |
| [file_support.hpp](../../../src/core/tools/workspace/file_support.hpp) | 声明文本/硬链接检查、工作区相对路径显示和文件安全替换/删除辅助接口。 |
| [path_identity.cpp](../../../src/core/tools/workspace/path_identity.cpp) | 结合原生文件 ID、父路径规范化和真实目录项拼写区分对象身份与目录项身份，使链接及大小写别名不能绕过范围检查。 |
| [path_identity.hpp](../../../src/core/tools/workspace/path_identity.hpp) | 声明对象身份、目录项身份和路径包含判断。普通访问可跟随链接目标，快照则将符号链接与目标分别识别。 |
| [search_tools.cpp](../../../src/core/tools/workspace/search_tools.cpp) | 实现 search_text 的受限文本遍历和命中收集，过滤忽略/敏感/受保护路径，限制文件大小、扫描量、命中数和展示行长度。 |
| [sensitive_path.cpp](../../../src/core/tools/workspace/sensitive_path.cpp) | 识别常见凭据目录、环境文件和保留写路径，保留公开模板例外，并提供有界扫描结果给工具和命令保护策略。 |
| [sensitive_path.hpp](../../../src/core/tools/workspace/sensitive_path.hpp) | 声明敏感路径、保留写路径判断和扫描接口，返回路径清单及是否因限额截断。 |
| [workspace_support.cpp](../../../src/core/tools/workspace/workspace_support.cpp) | 维护构建/版本控制等忽略目录规则并识别其大小写别名，提供文本匹配和搜索行截短实现。 |
| [workspace_support.hpp](../../../src/core/tools/workspace/workspace_support.hpp) | 声明忽略目录/路径组件检查、文本子串匹配和展示行截短函数。 |
| [workspace_tools.cpp](../../../src/core/tools/workspace/workspace_tools.cpp) | 实现工作区内路径解析、分深度列举文件及按偏移读取文本块，检查保护/敏感路径、文件类型、NUL 二进制标记与容量限制并返回分页信息。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[模型接入](model.md) · 下一页：[变更与恢复](changes.md)
