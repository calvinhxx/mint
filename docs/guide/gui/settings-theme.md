# 设置与主题

[导读目录](../README.md) · [桌面客户端](README.md)

模型设置表单、输入样式、主题预览和配色。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/gui/ui/theme](../../../src/gui/ui/theme) | 统一主题应用、外观和布局尺寸。 |

## 文件说明

### src/gui/ui/components

| 文件 | 用途 |
|---|---|
| [settings_panel.cpp](../../../src/gui/ui/components/settings_panel.cpp) | 创建可滚动的模型、语言和主题设置页，校验协议、模型以及 HTTPS 或回环 HTTP 地址。保留未保存草稿，显示凭据掩码和版本冲突，提供旧配置迁移选择。 |
| [settings_panel.hpp](../../../src/gui/ui/components/settings_panel.hpp) | 声明设置面板状态、模型表单草稿和操作信号，包括保存、移除密钥、重载及旧配置选择。模型请求和共享配置写入由界面之外的模块处理。 |
| [theme_choice_button.cpp](../../../src/gui/ui/components/theme_choice_button.cpp) | 绘制系统、浅色和深色三种主题的缩略预览，包含选中、悬停和键盘焦点反馈；系统模式用浅深两半表示跟随系统。 |
| [theme_choice_button.hpp](../../../src/gui/ui/components/theme_choice_button.hpp) | 带主题模式的可选按钮，响应 FluentQt 主题刷新，为设置页提供统一的预览尺寸。 |
| [underline_free_input.cpp](../../../src/gui/ui/components/underline_free_input.cpp) | 为单行、密码和多行输入绘制完整边框，移除 Fluent 下划线样式。处理焦点和禁用状态、密码框尾部操作区的位置与尺寸，以及透明输入表面。 |
| [underline_free_input.hpp](../../../src/gui/ui/components/underline_free_input.hpp) | 声明 Mint 风格的三类输入控件适配器，保留 FluentQt 的文本行为，提供密码框尾部操作和多行输入表面开关。 |

### src/gui/ui/theme

| 文件 | 用途 |
|---|---|
| [layout_metrics.hpp](../../../src/gui/ui/theme/layout_metrics.hpp) | 集中定义间距、标题栏、侧栏、分隔把手、会话、输入框、时间线、抽屉和主题预览的布局尺寸，供组件和委托共用。 |
| [mint_theme.cpp](../../../src/gui/ui/theme/mint_theme.cpp) | 定义 Mint 浅色和深色语义调色板，应用到 FluentQt 与 QApplication，并生成窗口及 Markdown 样式。解析跟随系统模式，广播主题刷新通知。 |
| [mint_theme.hpp](../../../src/gui/ui/theme/mint_theme.hpp) | 声明主题色角色、浅深配色选择、系统主题解析和安装入口。组件按语义取色，主题由这个模块管理。 |

[导读目录](../README.md) · [桌面客户端](README.md)

上一页：[会话展示与输入](conversation.md) · 下一页：[窗口、页面与资源](startup-resources.md)
