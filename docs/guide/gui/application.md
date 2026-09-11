# 桌面状态与控制器

[导读目录](../README.md) · [桌面客户端](README.md)

任务运行状态、工作区选择、模型设置和偏好控制。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/gui/application](../../../src/gui/application) | 不依赖 Qt 的桌面状态、控制器、运行对象和基础设施端口。 |
| [src/gui/application/preferences](../../../src/gui/application/preferences) | 界面偏好数据、仓库接口和偏好变更控制器。 |
| [src/gui/application/workspaces](../../../src/gui/application/workspaces) | 工作区列表、当前选择和路径有效性检查。 |

## 文件说明

### src/gui/application

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/gui/application/CMakeLists.txt) | 将任务、工作区、模型设置和偏好控制器编译为应用层静态库。这个库不依赖 Qt，只链接公共本地化模块。 |
| [agent_runtime.hpp](../../../src/gui/application/agent_runtime.hpp) | GUI 应用层的任务执行接口，包含任务列表、历史读取、启动、恢复、审批、取消，以及结果和事件回调。任务运行期间仍可查询和浏览。 |
| [desktop_types.hpp](../../../src/gui/application/desktop_types.hpp) | 定义 GUI 各层传递的任务摘要、事件、审批、历史、结果和运行请求等数据。限制保留的事件数量，用量报告完整时才计算缓存命中率。 |
| [model_settings_controller.cpp](../../../src/gui/application/model_settings_controller.cpp) | 处理模型设置的查询、保存、密钥移除和旧配置冲突选择，记录异步操作状态与错误。保存成功后取消本次覆盖配置，将新的共享配置快照发给界面。 |
| [model_settings_controller.hpp](../../../src/gui/application/model_settings_controller.hpp) | 声明模型设置页的状态和操作接口，包含加载、保存、凭据状态及迁移冲突的变化通知，并判断是否可以启动新任务。 |
| [model_settings_runtime.hpp](../../../src/gui/application/model_settings_runtime.hpp) | 模型设置的存储接口及传输类型。界面只接收不含密钥值的供应商摘要；接口约定三种协议适配器、带版本的更新请求，以及旧配置冲突的解决回调。 |
| [task_organization.cpp](../../../src/gui/application/task_organization.cpp) | 按工作区管理任务置顶与归档集合：归档会撤销置顶，刷新任务时移除已不存在的记录，并在持久化成功后更新内存状态。 |
| [task_organization.hpp](../../../src/gui/application/task_organization.hpp) | 声明置顶、归档的数据结构和存储接口，以及按项目管理这些状态的 TaskOrganizer。任务执行和历史存储由其他模块负责。 |
| [task_run.cpp](../../../src/gui/application/task_run.cpp) | 管理一次运行的独立快照，处理就绪、审批等待、结果和失败状态。去掉恢复时重放的旧事件，只保留最近事件。达到轮数上限、取消或超时后，判断任务能否恢复。 |
| [task_run.hpp](../../../src/gui/application/task_run.hpp) | TaskRun 保存当前或最近一次运行的根目录、任务身份、日志、审批、结果和错误。工作区控制器切换浏览对象时，仍保留这份运行状态。 |
| [task_workspace_controller.cpp](../../../src/gui/application/task_workspace_controller.cpp) | 处理工作区导航、历史读取、任务组织和单任务执行，保存各工作区的选择书签。后台运行事件写入独立快照，任务完成或报错时不改变用户正在浏览的会话。 |
| [task_workspace_controller.hpp](../../../src/gui/application/task_workspace_controller.hpp) | 声明工作区状态、变化通知和操作入口。当前浏览会话与全局运行身份分别保存；同时只持有一个运行快照，并限制第二个任务的启动或恢复。 |

### src/gui/application/preferences

| 文件 | 用途 |
|---|---|
| [gui_preferences.hpp](../../../src/gui/application/preferences/gui_preferences.hpp) | 定义 GUI 可保存的语言与主题偏好，以及 system、light、dark 主题模式的稳定字符串编码和解析规则。 |
| [preferences_controller.cpp](../../../src/gui/application/preferences/preferences_controller.cpp) | 合并环境语言、已保存偏好和本次启动的覆盖值。用户切换语言或主题时，保存选择并通知界面刷新。 |
| [preferences_controller.hpp](../../../src/gui/application/preferences/preferences_controller.hpp) | 偏好控制器接口，包含初始化、语言和主题切换，以及偏好变化订阅。具体设置存储通过独立接口访问。 |
| [preferences_repository.hpp](../../../src/gui/application/preferences/preferences_repository.hpp) | GUI 偏好存储接口，不依赖 QSettings。加载结果可为空，保存操作返回是否成功。 |

### src/gui/application/workspaces

| 文件 | 用途 |
|---|---|
| [workspace_catalog.cpp](../../../src/gui/application/workspaces/workspace_catalog.cpp) | 规范化工作区路径并去重，检查目录是否可用、是否与私有状态目录隔离。恢复上次工作区，保存工作区列表、最近选择和归档任务数量。 |
| [workspace_catalog.hpp](../../../src/gui/application/workspaces/workspace_catalog.hpp) | 工作区目录册及其存储接口，包含启动恢复、目录校验、工作区增删和任务数量更新，供导航控制器调用。 |

[导读目录](../README.md) · [桌面客户端](README.md)

上一页：[桌面客户端](README.md) · 下一页：[后台运行与设置存储](runtime.md)
