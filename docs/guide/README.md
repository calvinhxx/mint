# 源码导读

按模块查找目录和文件用途。同一功能的公共头文件与实现放在一起。

项目介绍见[README](../../README.md)，依赖关系和读码顺序见[架构](../concepts/architecture.md)，当前阶段见[项目进度](../project/roadmap.md)。

## 文档树

- [核心模块](core/README.md)
  - [Agent 循环](core/agent.md)
  - [数据、策略与运行控制](core/domain-runtime.md)
  - [模型接入](core/model.md)
  - [工作区工具](core/tools.md)
  - [变更与恢复](core/changes.md)
  - [配置与存储](core/config-storage.md)
  - [命令与沙箱](core/commands.md)
  - [文件、路径与平台接口](core/platform.md)
  - [日志与本地化](core/logging-localization.md)
- [命令行入口](cli/README.md)
- [桌面客户端](gui/README.md)
  - [桌面状态与控制器](gui/application.md)
  - [后台运行与设置存储](gui/runtime.md)
  - [工作区与会话导航](gui/navigation.md)
  - [会话展示与输入](gui/conversation.md)
  - [设置与主题](gui/settings-theme.md)
  - [窗口、页面与资源](gui/startup-resources.md)
- [工程设施](engineering/README.md)
  - [构建与打包](engineering/build.md)
  - [CI 与发布](engineering/ci-release.md)
  - [文档、配置模板与消息](engineering/docs-config.md)
- [测试](tests/README.md)
  - [单元测试](tests/unit.md)
  - [集成测试](tests/integration.md)
  - [契约测试](tests/contracts.md)
  - [桌面测试](tests/gui.md)
  - [样例与测试辅助](tests/fixtures.md)
