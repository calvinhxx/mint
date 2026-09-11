# 会话展示与输入

[导读目录](../README.md) · [桌面客户端](README.md)

时间线、结果、输入框、审批和证据面板。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/gui/ui/presentation](../../../src/gui/ui/presentation) | 把原始任务事件转换成用户可读的标题、摘要和证据展示内容。 |

## 文件说明

### src/gui/ui/components

| 文件 | 用途 |
|---|---|
| [approval_panel.cpp](../../../src/gui/ui/components/approval_panel.cpp) | 显示命令或文件变更的审批卡片，提供查看证据、拒绝和批准操作。变更 diff 被截断时禁用批准，只有完整展示变更后才能确认。 |
| [approval_panel.hpp](../../../src/gui/ui/components/approval_panel.hpp) | 声明审批内容和证据文本的展示状态，通过批准、拒绝及打开证据信号将用户操作交给上层。命令执行和文件写入在控件之外处理。 |
| [conversation_transcript.cpp](../../../src/gui/ui/components/conversation_transcript.cpp) | 用单个列表视口显示运行时间线和结果。追加事件时跟随末尾或保留阅读位置，为输入框留出空间，并处理证据链接的按下与释放。 |
| [conversation_transcript.hpp](../../../src/gui/ui/components/conversation_transcript.hpp) | 声明会话时间线的状态渲染、滚动跟随、跳到最新和证据选择接口，连接列表模型与绘制委托。 |
| [evidence_panel.cpp](../../../src/gui/ui/components/evidence_panel.cpp) | 创建右侧证据面板，以只读文本显示命令参数、工具记录或 diff，并提供本地化标题与关闭操作。 |
| [evidence_panel.hpp](../../../src/gui/ui/components/evidence_panel.hpp) | 声明证据内容及关闭信号，供页面在同一个只读面板中显示不同来源的检查材料。 |
| [run_conversation.cpp](../../../src/gui/ui/components/run_conversation.cpp) | 在时间线上叠放审批和错误提示，仅在浏览当前运行任务时显示审批。动态调整底部留白，并决定何时显示跳到最新按钮。 |
| [run_conversation.hpp](../../../src/gui/ui/components/run_conversation.hpp) | 声明会话主区域的状态渲染和滚动接口，组合时间线、审批卡片和提示，并将证据与审批操作转发给页面。 |
| [task_composer.cpp](../../../src/gui/ui/components/task_composer.cpp) | 创建行数可扩展的任务输入区及发送、停止按钮，按工作区、模型配置和运行名额决定是否允许操作。维护焦点外框和草稿，在析构前断开焦点回调。 |
| [task_composer.hpp](../../../src/gui/ui/components/task_composer.hpp) | 分别提供输入草稿、焦点、当前会话运行状态和新任务可用性的接口，通过提交、取消及高度变化信号与页面连接。 |

### src/gui/ui/delegates

| 文件 | 用途 |
|---|---|
| [run_timeline_delegate.cpp](../../../src/gui/ui/delegates/run_timeline_delegate.cpp) | 绘制时间线活动点、标题、详情、证据操作，以及 Markdown 结果卡片和用量摘要。禁用 Markdown 内嵌 HTML，根据内容计算结果高度和证据点击区域。 |
| [run_timeline_delegate.hpp](../../../src/gui/ui/delegates/run_timeline_delegate.hpp) | 声明时间线各类列表行的绘制和尺寸计算接口，将证据操作区的几何计算提供给会话控件，用于点击命中判断。 |

### src/gui/ui/models

| 文件 | 用途 |
|---|---|
| [run_timeline_model.cpp](../../../src/gui/ui/models/run_timeline_model.cpp) | 将任务事件、结果、空态和底部留白转为 Qt 列表行。按序号增量追加事件并淘汰旧事件，生成本地化用量及缓存报告，提供证据和纯文本导出。 |
| [run_timeline_model.hpp](../../../src/gui/ui/models/run_timeline_model.hpp) | 声明时间线行类别和数据角色，保存事件及结果快照，提供追加、留白、翻译和证据读取接口。 |

### src/gui/ui/presentation

| 文件 | 用途 |
|---|---|
| [run_event_presentation.cpp](../../../src/gui/ui/presentation/run_event_presentation.cpp) | 将模型、工具、审批、验证和任务结束事件转换为中文或英文标题、轮次或目标详情，以及语义色调。视图使用转换后的内容，不直接显示原始协议类型。 |
| [run_event_presentation.hpp](../../../src/gui/ui/presentation/run_event_presentation.hpp) | 声明运行事件的展示数据，以及中性、注意、成功、严重四类色调，为时间线模型提供独立的呈现转换接口。 |

[导读目录](../README.md) · [桌面客户端](README.md)

上一页：[工作区与会话导航](navigation.md) · 下一页：[设置与主题](settings-theme.md)
