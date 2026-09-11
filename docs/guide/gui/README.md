# 桌面客户端

[导读目录](../README.md)

界面状态、后台运行与导航，以及桌面模块的分工。

## 按主题阅读

| 文档 | 内容 |
|---|---|
| [桌面状态与控制器](application.md) | 任务运行状态、工作区选择、模型设置和偏好控制。 |
| [后台运行与设置存储](runtime.md) | CLI 子进程、事件跟读、历史查询、QSettings 和共享配置的异步操作。 |
| [工作区与会话导航](navigation.md) | 侧栏列表、选择、菜单、动画和分栏光标。 |
| [会话展示与输入](conversation.md) | 时间线、结果、输入框、审批和证据面板。 |
| [设置与主题](settings-theme.md) | 模型设置表单、输入样式、主题预览和配色。 |
| [窗口、页面与资源](startup-resources.md) | 启动、窗口和页面组装，标题栏、品牌图形及资源加载。 |

## 运行与导航

一个 GUI runtime 持有一个 `TaskRun` 和一个运行用的 `QProcess`。运行期间可以切换工作区、浏览历史任务；运行中的任务和正在浏览的页面分别保存状态。历史读取、状态查询和事件解析异步执行。

单个 GUI runtime 目前没有多任务运行队列或多 Agent 调度器。独立启动的 CLI / GUI 进程各自运行，程序没有覆盖整台机器的全局 Core 单例。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/gui](../../../src/gui) | FluentQt 桌面客户端，复用 CLI 执行任务并使用核心共享配置服务。 |
| [src/gui/ui](../../../src/gui/ui) | FluentQt / Qt Widgets 界面、展示模型和用户交互。 |
| [src/gui/ui/components](../../../src/gui/ui/components) | 侧栏、输入框、审批、证据、设置、会话内容和分隔条等可组合控件。 |
| [src/gui/ui/delegates](../../../src/gui/ui/delegates) | Qt item delegate，负责任务行和运行时间线的绘制及局部交互。 |
| [src/gui/ui/models](../../../src/gui/ui/models) | 用 QAbstractListModel 把任务、工作区状态和事件转换成列表行。 |

## 文件说明

### src/gui

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/gui/CMakeLists.txt) | GUI 的构建入口，依次加入 support、application、infrastructure、ui、app 子模块。 |

### src/gui/ui

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/gui/ui/CMakeLists.txt) | 将 FluentQt 组件、Qt 列表模型和绘制委托、工作区页面、窗口壳、主题及资源编译为 GUI 展示库，启用 AUTOMOC 与 AUTORCC。 |

[导读目录](../README.md)

上一页：[命令行入口](../cli/README.md) · 下一页：[桌面状态与控制器](application.md)
