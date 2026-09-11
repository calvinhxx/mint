# 窗口、页面与资源

[导读目录](../README.md) · [桌面客户端](README.md)

启动、窗口和页面组装，标题栏、品牌图形及资源加载。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/gui/app](../../../src/gui/app) | 桌面启动入口，解析参数、组装对象、应用主题并显示窗口。 |
| [src/gui/ui/pages](../../../src/gui/ui/pages) | 任务工作区页面，处理用户操作、控制器通知、草稿与刷新。 |
| [src/gui/ui/resources](../../../src/gui/ui/resources) | 编译进程序的 Qt 资源清单与图形资源。 |
| [src/gui/ui/resources/branding](../../../src/gui/ui/resources/branding) | Mint 北极狐品牌图形及底板版本。 |
| [src/gui/ui/shell](../../../src/gui/ui/shell) | 应用主窗口、标题栏、工作区页面和设置面板的容器。 |
| [src/gui/ui/support](../../../src/gui/ui/support) | GUI 资源初始化、本地化与状态文字的小型展示辅助。 |

## 文件说明

### src/gui/app

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/gui/app/CMakeLists.txt) | 构建 mint-gui 可执行文件，将供应商示例配置复制到构建产物，链接 GUI 各模块，并确保对应的 mint CLI 已构建。macOS 下还会注册开发用应用包。 |
| [gui_main.cpp](../../../src/gui/app/gui_main.cpp) | 桌面程序入口。解析启动参数，准备私有状态目录，创建 QSettings 仓库、CLI 运行适配器和控制器，再启动 FluentQt 窗口。也提供离线截图与界面质量报告入口。 |
| [gui_options.cpp](../../../src/gui/app/gui_options.cpp) | 解析工作区、CLI 路径、状态目录、模型覆盖配置、语言、主题及截图参数。将相对路径固定到启动时的位置，保留状态目录末级的符号链接，留给安全策略检查。 |
| [gui_options.hpp](../../../src/gui/app/gui_options.hpp) | 声明 GUI 启动的路径、外观、演示和诊断选项，以及解析失败和调用路径解析的入口。 |

### src/gui/ui/components

| 文件 | 用途 |
|---|---|
| [fox_mark.cpp](../../../src/gui/ui/components/fox_mark.cpp) | 从 Qt 资源加载透明底北极狐 SVG，按标题栏品牌标记尺寸抗锯齿绘制，并允许鼠标事件穿过装饰图案。 |
| [fox_mark.hpp](../../../src/gui/ui/components/fox_mark.hpp) | 声明 Mint 狐狸标记控件，保存 SVG 渲染器，并提供固定的布局尺寸。 |
| [mint_title_bar.cpp](../../../src/gui/ui/components/mint_title_bar.cpp) | 排列狐狸标记、Mint 名称、当前任务标题、恢复入口和设置按钮；随侧栏宽度调整品牌区边界，并对长标题做省略。 |
| [mint_title_bar.hpp](../../../src/gui/ui/components/mint_title_bar.hpp) | 声明标题栏接收的任务状态、导航边界，以及设置和恢复任务的操作信号，供窗口壳协调。 |
| [title_bar_surface.cpp](../../../src/gui/ui/components/title_bar_surface.cpp) | 绘制标题栏主内容区的主题底色和底部分隔线。鼠标事件穿过装饰层，不妨碍窗口拖动。 |
| [title_bar_surface.hpp](../../../src/gui/ui/components/title_bar_surface.hpp) | 声明标题栏背景装饰控件，与标题栏内容分开，供窗口壳按侧栏边界调整绘制区域。 |

### src/gui/ui/pages

| 文件 | 用途 |
|---|---|
| [task_workspace_page.cpp](../../../src/gui/ui/pages/task_workspace_page.cpp) | 组装侧栏、会话、输入框、证据和设置抽屉，将组件操作连接到三个控制器。按工作区和任务保存内存草稿，允许运行时导航，限制第二个任务发送，以及模型配置未决时的执行。 |
| [task_workspace_page.hpp](../../../src/gui/ui/pages/task_workspace_page.hpp) | 声明主工作区页面的组件和协调状态，包含控制器引用、抽屉、异步目录选择、待新建任务、草稿缓存及标题栏通知。 |

### src/gui/ui/resources/branding

| 文件 | 用途 |
|---|---|
| [mint-arctic-fox-mark.svg](../../../src/gui/ui/resources/branding/mint-arctic-fox-mark.svg) | 64×64 透明背景的北极狐头与环绕尾巴矢量标记，用于标题栏中的 Mint 品牌图案。 |
| [mint-arctic-fox-tile.svg](../../../src/gui/ui/resources/branding/mint-arctic-fox-tile.svg) | 256×256 深色圆角方形底的北极狐应用图标，由 QApplication 和 MintWindow 作为窗口或应用图标加载。 |

### src/gui/ui/resources

| 文件 | 用途 |
|---|---|
| [mint_gui.qrc](../../../src/gui/ui/resources/mint_gui.qrc) | 将狐狸标记和应用图标注册到 /mint/branding 的 Qt 资源路径。桌面程序从资源中加载图片，无需在运行时读取外部图片文件。 |

### src/gui/ui/shell

| 文件 | 用途 |
|---|---|
| [mint_window.cpp](../../../src/gui/ui/shell/mint_window.cpp) | 创建 FluentQt 原生窗口壳，组装标题栏和工作区页面，协调主题、语言、系统配色和窗口拖动排除区。关闭窗口时请求取消当前任务。 |
| [mint_window.hpp](../../../src/gui/ui/shell/mint_window.hpp) | 声明桌面顶层窗口如何处理关闭、尺寸、主题和标题栏变化，保存页面与标题栏组件。Agent 执行逻辑由其他模块负责。 |

### src/gui/ui/support

| 文件 | 用途 |
|---|---|
| [gui_resources.cpp](../../../src/gui/ui/support/gui_resources.cpp) | 调用 Q_INIT_RESOURCE，显式注册静态库中的 Mint 图片资源，供运行时通过资源路径加载品牌图案。 |
| [gui_resources.hpp](../../../src/gui/ui/support/gui_resources.hpp) | 声明 GUI 资源初始化函数，供应用启动时调用。Qt 资源注册细节由实现文件处理。 |
| [localized_text.hpp](../../../src/gui/ui/support/localized_text.hpp) | 把公共本地化消息及具名参数转换为 QString，供界面组件共用相同的消息目录与格式化规则。 |
| [status_text.hpp](../../../src/gui/ui/support/status_text.hpp) | 将运行、完成、失败、轮数上限、超时、预算耗尽、取消和等待等状态码映射为界面文字，并将可恢复的历史状态统一显示为可恢复。 |

[导读目录](../README.md) · [桌面客户端](README.md)

上一页：[设置与主题](settings-theme.md) · 下一页：[工程设施](../engineering/README.md)
