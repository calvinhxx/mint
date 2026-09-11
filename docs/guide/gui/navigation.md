# 工作区与会话导航

[导读目录](../README.md) · [桌面客户端](README.md)

侧栏列表、选择、菜单、动画和分栏光标。

## 文件说明

### src/gui/ui/components

| 文件 | 用途 |
|---|---|
| [task_sidebar.cpp](../../../src/gui/ui/components/task_sidebar.cpp) | 组合工作区和会话导航，处理鼠标、键盘、无障碍选择、展开收起及上下文菜单。保存各工作区的滚动位置，显示运行或等待审批标记；状态刷新时保持选择和悬停稳定。 |
| [task_sidebar.hpp](../../../src/gui/ui/components/task_sidebar.hpp) | 声明侧栏导航、置顶、归档和工作区操作信号，保存展开、滚动、行操作及菜单的局部交互状态。业务决定由控制器处理。 |
| [workspace_split_view.cpp](../../../src/gui/ui/components/workspace_split_view.cpp) | 适配 FluentQt 分栏的指针命中行为：仅在分隔把手范围或拖动中显示调整光标，进入面板后恢复，并绘制悬停分界线。 |
| [workspace_split_view.hpp](../../../src/gui/ui/components/workspace_split_view.hpp) | 声明工作区分栏的事件处理接口。处理子面板的进入、离开事件，以及拖动、尺寸和启用状态变化，并同步光标。 |

### src/gui/ui/delegates

| 文件 | 用途 |
|---|---|
| [task_item_delegate.cpp](../../../src/gui/ui/delegates/task_item_delegate.cpp) | 绘制工作区折叠箭头、任务选中背景、状态图标和省略后的长标题，固定预留操作槽位。悬停和箭头动画遵循减少动效策略。 |
| [task_item_delegate.hpp](../../../src/gui/ui/delegates/task_item_delegate.hpp) | 声明侧栏行的视觉状态和几何规则，以及运行行、操作行、悬停和展开动画接口。各行由委托绘制，无需为每条任务创建控件。 |

### src/gui/ui/models

| 文件 | 用途 |
|---|---|
| [task_list_model.cpp](../../../src/gui/ui/models/task_list_model.cpp) | 将工作区和当前工作区的任务转为扁平导航列表，过滤归档任务，按置顶状态排序，并为运行任务预留位置。提供无障碍文本、路径名和行索引，通过增删行更新保持选择稳定。 |
| [task_list_model.hpp](../../../src/gui/ui/models/task_list_model.hpp) | 声明侧栏列表的工作区和任务数据角色，包含展开、可移除、置顶、归档、运行和待审批状态，并提供按任务或工作区定位行的接口。 |

[导读目录](../README.md) · [桌面客户端](README.md)

上一页：[后台运行与设置存储](runtime.md) · 下一页：[会话展示与输入](conversation.md)
