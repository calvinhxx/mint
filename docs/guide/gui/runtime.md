# 后台运行与设置存储

[导读目录](../README.md) · [桌面客户端](README.md)

CLI 子进程、事件跟读、历史查询、QSettings 和共享配置的异步操作。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/gui/infrastructure](../../../src/gui/infrastructure) | 通过 QProcess、文件读取和 QSettings 实现桌面 application 端口。 |
| [src/gui/infrastructure/cli](../../../src/gui/infrastructure/cli) | CLI 参数与 JSONL 协议适配、单任务进程、事件流和历史读取。 |
| [src/gui/infrastructure/settings](../../../src/gui/infrastructure/settings) | QSettings 偏好存储，以及共享模型配置的异步操作和外部变更通知。 |
| [src/gui/support](../../../src/gui/support) | Qt 字符串与标准库路径的转换，供 GUI 各层共用。 |

## 文件说明

### src/gui/infrastructure

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/gui/infrastructure/CMakeLists.txt) | 分别构建 CLI 进程适配器和设置存储适配器。前者链接 Qt 并发及 JSON 解析，后者链接共享 ModelConfigService，两者保持独立。 |

### src/gui/infrastructure/cli

| 文件 | 用途 |
|---|---|
| [mint_cli_command_builder.cpp](../../../src/gui/infrastructure/cli/mint_cli_command_builder.cpp) | 将桌面运行请求转换为 mint status、run、resume 的参数列表，传入工作区、状态目录、语言、配置、JSONL 交互及取消文件。直接传递参数，不拼接 shell 命令。 |
| [mint_cli_command_builder.hpp](../../../src/gui/infrastructure/cli/mint_cli_command_builder.hpp) | 声明类型化的 CLI 参数构建接口。运行请求的参数编码与 QProcess 生命周期分别处理。 |
| [mint_cli_process.cpp](../../../src/gui/infrastructure/cli/mint_cli_process.cpp) | 用独立 QProcess 启动 mint 任务和状态查询，异步读取历史及事件，并传递审批决定和取消信号。丢弃过期的浏览结果，根据结构化任务结果判断任务是可恢复地结束，还是发生了进程故障。 |
| [mint_cli_process.hpp](../../../src/gui/infrastructure/cli/mint_cli_process.hpp) | AgentRuntime 的 Qt 进程实现，持有唯一的运行进程、事件流、输出缓冲和查询代数。状态查询与历史读取不占用任务运行名额。 |
| [mint_cli_protocol.cpp](../../../src/gui/infrastructure/cli/mint_cli_protocol.cpp) | 解析 CLI 状态 JSON、运行事件、最终结果与 stderr 交互记录，转换为 GUI 数据对象。读取结构化错误，并序列化一次审批决定。 |
| [mint_cli_protocol.hpp](../../../src/gui/infrastructure/cli/mint_cli_protocol.hpp) | 声明 GUI 与 CLI 交换的状态、task_ready 和审批消息结构，以及各类 JSON 协议的解析入口。 |
| [run_event_stream.cpp](../../../src/gui/infrastructure/cli/run_event_stream.cpp) | 增量读取 events.jsonl，将文件读取和 JSON 解析交给 QtConcurrent。按 64 KiB 切片、每批 128 条记录处理，丢弃过期批次。进程结束后，读完尾部事件再通知完成。 |
| [run_event_stream.hpp](../../../src/gui/infrastructure/cli/run_event_stream.hpp) | 声明事件日志跟读器的打开、排空和重置接口，保存读取偏移、未完整的记录、定时器及代数，并将事件分批交给运行适配器。 |
| [task_history_reader.cpp](../../../src/gui/infrastructure/cli/task_history_reader.cpp) | 从指定任务目录读取 events.jsonl 和 session.json，保留最近的历史事件，并记录损坏事件或无效结果快照以供界面提示。 |
| [task_history_reader.hpp](../../../src/gui/infrastructure/cli/task_history_reader.hpp) | 独立的同步历史读取接口，供进程适配器在后台线程加载任务历史。文件读取由这个模块负责，界面只接收结果。 |

### src/gui/infrastructure/settings

| 文件 | 用途 |
|---|---|
| [qsettings_preferences_repository.cpp](../../../src/gui/infrastructure/settings/qsettings_preferences_repository.cpp) | 在应用自身的 QSettings 域内保存语言和主题。读取旧版模型配置指针，只在它仍等于启动时的旧值时清除，以免覆盖另一个旧版窗口的新选择。 |
| [qsettings_preferences_repository.hpp](../../../src/gui/infrastructure/settings/qsettings_preferences_repository.hpp) | PreferencesRepository 的存储实现，支持默认设置或指定 INI 文件，也提供旧版模型配置指针的读取和条件清理接口。 |
| [qsettings_task_organization_repository.cpp](../../../src/gui/infrastructure/settings/qsettings_task_organization_repository.cpp) | 按项目作用域的 SHA-256 摘要分区保存置顶与归档任务 ID，写入前排序并同步。关闭组织级和系统级设置回退，防止其他状态域的记录混入。 |
| [qsettings_task_organization_repository.hpp](../../../src/gui/infrastructure/settings/qsettings_task_organization_repository.hpp) | 任务组织仓库的 QSettings 实现，可使用应用默认存储或测试指定的 INI 文件。序列化格式由仓库处理，控制器通过接口访问。 |
| [qsettings_workspace_catalog_repository.cpp](../../../src/gui/infrastructure/settings/qsettings_workspace_catalog_repository.cpp) | 用带版本的 QSettings 键保存工作区路径列表、上次活动目录和归档数量。校验计数字段，通过共用的 Qt 路径转换保留本机路径。 |
| [qsettings_workspace_catalog_repository.hpp](../../../src/gui/infrastructure/settings/qsettings_workspace_catalog_repository.hpp) | WorkspaceCatalogRepository 的 QSettings 实现，支持默认应用设置或显式指定的 INI 文件，供工作区目录册加载与保存。 |
| [shared_model_settings_operations.cpp](../../../src/gui/infrastructure/settings/shared_model_settings_operations.cpp) | 调用 ModelConfigService 查询共享配置、提交带版本的更新或迁移旧配置，返回脱敏摘要及冲突结果，并清理请求中的草稿密钥。这些存储操作不持有 Qt 对象。 |
| [shared_model_settings_operations.hpp](../../../src/gui/infrastructure/settings/shared_model_settings_operations.hpp) | 声明后台模型设置操作及其结果，包含查询、修改和迁移选择。操作对象管理草稿密钥的生命周期，结果只返回界面需要的摘要和失败类别。 |
| [shared_model_settings_runtime.cpp](../../../src/gui/infrastructure/settings/shared_model_settings_runtime.cpp) | 用 QtConcurrent 串行调度共享配置操作，合并过期查询，保留保存结果。监听配置目录变化并刷新，旧指针清理成功后才完成迁移，同时保护回调的生命周期。 |
| [shared_model_settings_runtime.hpp](../../../src/gui/infrastructure/settings/shared_model_settings_runtime.hpp) | ModelSettingsRuntime 的 Qt 异步实现，使用文件监视，保存待查询、待修改、订阅代数和重试定时器。配置仍由共享 ModelConfigService 持久化。 |

### src/gui/support

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/gui/support/CMakeLists.txt) | 构建 Qt 路径转换静态库，供 GUI 各模块共用 QString 与平台路径转换。只依赖 QtCore 和公共 platform 模块。 |
| [qt_path.cpp](../../../src/gui/support/qt_path.cpp) | 通过 mint::platform 的 UTF-8 路径 API，在 QString 与 std::filesystem::path 之间双向转换。转换时显式传入长度，保留本机路径分隔语义。 |
| [qt_path.hpp](../../../src/gui/support/qt_path.hpp) | 声明 GUI 共用的两项 Qt 路径转换函数。窗口、模型和存储适配器都调用它们，无需各自编写平台编码分支。 |

[导读目录](../README.md) · [桌面客户端](README.md)

上一页：[桌面状态与控制器](application.md) · 下一页：[工作区与会话导航](navigation.md)
