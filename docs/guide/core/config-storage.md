# 配置与存储

[导读目录](../README.md) · [核心模块](README.md)

GUI / CLI 共享配置，以及项目、任务和检查点的保存。

CLI 与 GUI 都通过 `ModelConfigService` 读写用户状态目录中的 `model.json`、`credentials.json` 和锁文件。`--config` 只覆盖本次运行，工作区旧 `config.json` 不会自动导入。任务启动后使用当时的配置快照。

GUI 的外观、工作区和任务组织偏好保存在 QSettings 中，与模型密钥分开。托管项目和任务数据也放在工作区之外，每个任务保存 `policy.json`、`session.json`、`events.jsonl` 及变更事务文件。

字段、路径优先级和预算设置见 [模型配置参考](../../reference/model-providers.md)。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/infrastructure/persistence](../../../src/core/infrastructure/persistence) | 项目、任务、检查点、事件和变更事务的文件存储。 |
| [src/core/infrastructure/settings](../../../src/core/infrastructure/settings) | CLI / GUI 共用的模型配置和凭据服务，分别处理字段、密钥、文件与事务。 |

## 文件说明

### include/mint/infrastructure

| 文件 | 用途 |
|---|---|
| [change_transaction_store.hpp](../../../include/mint/infrastructure/change_transaction_store.hpp) | 声明变更事务存储，持有跨进程锁，提供事务文档的读写和清除操作。事务及锁文件路径由会话路径派生。 |
| [model_config_service.hpp](../../../include/mint/infrastructure/model_config_service.hpp) | 定义 CLI/GUI 共用的模型与凭据持久化服务，提供脱敏检查、按版本更新、显式旧配置导入和运行时加载，并区分共享配置与单次覆盖。 |
| [project_store.hpp](../../../include/mint/infrastructure/project_store.hpp) | 定义外部状态目录中的项目与托管任务存储接口，统一项目策略、任务路径、任务摘要、创建/列举及最近可恢复任务查询。 |
| [session_store.hpp](../../../include/mint/infrastructure/session_store.hpp) | 声明 SessionRepository 的文件实现，向 Agent 提供检查点存在性查询、JSON 加载与保存接口。 |

### include/mint/ports

| 文件 | 用途 |
|---|---|
| [session_repository.hpp](../../../include/mint/ports/session_repository.hpp) | 定义检查点存储端口，仅暴露存在、加载和保存操作，隔离 Agent 与实际文件存储。 |

### src/core/infrastructure/persistence

| 文件 | 用途 |
|---|---|
| [change_transaction_store.cpp](../../../src/core/infrastructure/persistence/change_transaction_store.cpp) | 为会话变更事务取得非阻塞跨进程锁，复用会话 JSON 存储读写事务文档，并校验清除和路径派生操作。 |
| [project_store.cpp](../../../src/core/infrastructure/persistence/project_store.cpp) | 按规范化工作区映射外部项目状态目录，保存项目策略及每任务元数据/策略副本，校验文件和任务 ID，汇总并筛选可恢复任务。 |
| [session_store.cpp](../../../src/core/infrastructure/persistence/session_store.cpp) | 有界读取和序列化 JSON 检查点，将内容写入带时间戳/序号的临时文件，设置属主读写权限后重命名到目标，失败时清理临时文件。 |

### src/core/infrastructure/settings

| 文件 | 用途 |
|---|---|
| [model_config_service.cpp](../../../src/core/infrastructure/settings/model_config_service.cpp) | 按需解析共享配置路径，处理加锁、检查、更新和迁移。显式覆盖配置可独立加载，只在需要本地凭据绑定时读取对应存储。 |
| [model_settings_credentials.cpp](../../../src/core/infrastructure/settings/model_settings_credentials.cpp) | 验证凭据文档和引用，以 adapter/endpoint 绑定选择密钥，兼容旧供应商作用域键，并清理不再使用的托管键及内联密钥字段。 |
| [model_settings_credentials.hpp](../../../src/core/infrastructure/settings/model_settings_credentials.hpp) | 声明凭据引用、已校验文档和绑定键选择接口。密钥查询返回借用视图，减少展示与事务处理中产生的密钥副本。 |
| [model_settings_files.cpp](../../../src/core/infrastructure/settings/model_settings_files.cpp) | 读写私有配置，检查读取大小，计算版本摘要，加跨进程锁并擦除敏感缓冲区。写入时在受保护 staging 目录创建临时文件，同步后替换目标。 |
| [model_settings_files.hpp](../../../src/core/infrastructure/settings/model_settings_files.hpp) | 定义自动擦除临时密钥的守卫、持有 JSON 与版本的文档及锁对象，声明配置存储读写和唯一引用生成入口。 |
| [model_settings_profile.cpp](../../../src/core/infrastructure/settings/model_settings_profile.cpp) | 规范化共享配置端点和更新输入，复用模型配置解析器生成类型化配置，并构造不含密钥/敏感 URL 后缀的展示快照。 |
| [model_settings_profile.hpp](../../../src/core/infrastructure/settings/model_settings_profile.hpp) | 声明共享设置的字段校验、端点规范化、覆盖配置解析及脱敏快照构造函数。 |
| [model_settings_transactions.cpp](../../../src/core/infrastructure/settings/model_settings_transactions.cpp) | 在已持锁状态下检查 expected_revision 并提交更新/旧配置导入；先保存新凭据再切换模型引用，随后清理旧键，保持原导入文件不变。 |
| [model_settings_transactions.hpp](../../../src/core/infrastructure/settings/model_settings_transactions.hpp) | 声明共享配置更新和迁移接口；调用方须先持有存储锁，写入前校验当前版本。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[变更与恢复](changes.md) · 下一页：[命令与沙箱](commands.md)
