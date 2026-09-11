# 桌面测试

[导读目录](../README.md) · [测试](README.md)

控制器、Qt 控件、QProcess、共享配置和交互。

## 源码目录

| 目录 | 用途 |
|---|---|
| [tests/desktop](../../../tests/desktop) | 桌面控制器、协议、真实 QProcess、共享配置、控件和交互回归。 |

## 文件说明

### tests/desktop

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../tests/desktop/CMakeLists.txt) | 组织 Qt offscreen 控件、控制器、进程测试和独立共享配置套件，构建模拟子进程故障的 fixture，并注入当前 CLI 路径，避免测到旧程序。 |
| [desktop_branding_tests.cpp](../../../tests/desktop/desktop_branding_tests.cpp) | 将 Orbit Tail 品牌资产渲染为紧凑控件和启动图标所需的尺寸，检查资源存在，并且在不同大小下都有有效可见内容。 |
| [desktop_component_test_support.hpp](../../../tests/desktop/desktop_component_test_support.hpp) | 提供可手动触发回调的 Agent 和模型设置 fake，以及内存偏好仓储。记录运行、恢复和保存请求，供组件测试使用，不启动真实模型。 |
| [desktop_controller_tests.cpp](../../../tests/desktop/desktop_controller_tests.cpp) | 测试工作区恢复、后台运行与当前浏览选择的隔离、审批归属、过期历史请求丢弃和批事件去重。还测试同步通知重入、配置保存限制，以及组织操作只保护运行中任务的规则。 |
| [desktop_conversation_tests.cpp](../../../tests/desktop/desktop_conversation_tests.cpp) | 测试会话只有一个滚动容器，输入区悬浮，以及跳到最新和追加事件时的阅读位置。证据链接的按下和释放必须匹配，打开同窗抽屉时还要保留控件原有的点击契约。 |
| [desktop_custom_model_tests.cpp](../../../tests/desktop/desktop_custom_model_tests.cpp) | 测试共享配置异步保存/删除、单次 override、旧指针迁移冲突、CAS、原子替换监听和错误恢复，也覆盖回调中销毁对象或更换订阅、队列顺序及临时密钥清理。 |
| [desktop_input_tests.cpp](../../../tests/desktop/desktop_input_tests.cpp) | 检查输入框没有重复下划线或边框，明暗主题的边缘表现一致，密码框保留原生编辑和掩码行为。聚焦输入区析构前必须断开子控件回调，防止悬空调用。 |
| [desktop_model_settings_ui_tests.cpp](../../../tests/desktop/desktop_model_settings_ui_tests.cpp) | 测试共享模型编辑表单、未保存草稿与外部变更的冲突，以及保存失败后不切换配置。保存期间不能新建或恢复真实任务；旧 GUI 配置发生冲突时，界面必须展示两个来源并要求明确选择。 |
| [desktop_model_tests.cpp](../../../tests/desktop/desktop_model_tests.cpp) | 检查持久化的可恢复任务不会显示为实时运行，任务行能稳定更新，只展开当前项目任务，空根不会生成伪侧栏行。也检查时间线追加和面向用户的活动文案。 |
| [desktop_preferences_tests.cpp](../../../tests/desktop/desktop_preferences_tests.cpp) | 测试主题、语言、工作区顺序和 Unicode 路径的持久化，以及旧模型指针仅用于迁移并按条件清理。默认仓储不能回退读取组织/系统域，不同状态目录必须保持隔离。 |
| [desktop_process_tests.cpp](../../../tests/desktop/desktop_process_tests.cpp) | 用离线 CLI 和合成子进程测试终态、非零退出、崩溃、长日志分帧和尾行排空。覆盖首回调中销毁对象或解绑、Qt 信号生命周期、旧帧隔离、审批通道故障和异步历史刷新。 |
| [desktop_protocol_tests.cpp](../../../tests/desktop/desktop_protocol_tests.cpp) | 检查 CLI 参数和结果保留原生 Unicode 路径，JSON 事件、审批和结果转换为桌面类型，无关或不完整对象被拒绝，缺失的缓存统计保持未知。 |
| [desktop_settings_tests.cpp](../../../tests/desktop/desktop_settings_tests.cpp) | 测试中性主题和输入调色板、主题与语言持久化、设置抽屉点击隔离和反向关闭动画，也覆盖辅助功能触发、快速切换及窄英文窗口布局。 |
| [desktop_shared_config_integration_tests.cpp](../../../tests/desktop/desktop_shared_config_integration_tests.cpp) | 在空子进程环境和临时状态中，交叉执行 GUI 保存与 CLI show/set/provider inspect，检查配置版本和凭据一致。保存显式 override 后，新进程应使用共享默认配置，原 override 保持不变。 |
| [desktop_sidebar_interaction_tests.cpp](../../../tests/desktop/desktop_sidebar_interaction_tests.cpp) | 测试工作区展开、任务选择和行按钮的命中区，并检查 Qt 选择通知不会重复派发导航。还覆盖后台审批、行控件复用、滚动恢复、键盘/菜单、受控动画和分割线光标范围。 |
| [desktop_startup_tests.cpp](../../../tests/desktop/desktop_startup_tests.cpp) | 检查 root/state/config 参数缺值、空值或误用下一 flag 时会被拒绝，Qt 路径转换保留 Unicode。symlink/.. 按文件系统语义解析，不能隐藏本应拒绝的状态叶目录符号链接。 |
| [desktop_task_organization_tests.cpp](../../../tests/desktop/desktop_task_organization_tests.cpp) | 测试置顶、归档和恢复可以持久化且可逆，保存或加载失败不污染当前状态。不同项目的组织信息互相隔离，模型按置顶排序并过滤归档任务。 |
| [desktop_test_main.cpp](../../../tests/desktop/desktop_test_main.cpp) | 初始化 offscreen QApplication、FluentQt/应用资源和默认主题，再启动 GoogleTest，统一管理控件测试的 Qt 生命周期。 |
| [desktop_timeline_tests.cpp](../../../tests/desktop/desktop_timeline_tests.cpp) | 检查批事件通过成段插入更新现有时间线，保留窗口有界且按整批移除和追加，不重置整段会话。 |
| [desktop_workspace_tests.cpp](../../../tests/desktop/desktop_workspace_tests.cpp) | 测试空工作区提示和添加入口、指针导航、可访问名称、大量会话下的侧栏稳定性、运行指示，以及后台浏览时逐会话保留草稿。也测试异步、单实例的目录选择器。 |

[导读目录](../README.md) · [测试](README.md)

上一页：[契约测试](contracts.md) · 下一页：[样例与测试辅助](fixtures.md)
