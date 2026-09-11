# 测试

[导读目录](../README.md)

先按测试对象选页面；测试注册、架构检查和脚本自身的测试列在本页。

## 按主题阅读

| 文档 | 内容 |
|---|---|
| [单元测试](unit.md) | 参数、配置、日志、路径、平台文件和运行设置。 |
| [集成测试](integration.md) | Agent、工具、模型、命令与存储共同运行时的行为。 |
| [契约测试](contracts.md) | 策略恢复、项目流程、模型协议和用量统计。 |
| [桌面测试](gui.md) | 控制器、Qt 控件、QProcess、共享配置和交互。 |
| [样例与测试辅助](fixtures.md) | 故障修复样例、合成子进程、本机 HTTP 服务和临时工作区。 |

## 源码目录

| 目录 | 用途 |
|---|---|
| [tests](../../../tests) | 单元、跨模块、契约、桌面、脚本测试及其 fixture 和辅助进程。 |
| [tests/cmake](../../../tests/cmake) | 架构 / 终端 / 平台边界检查，以及各类 CTest 注册和 App 打包合成验收。 |
| [tests/scripts](../../../tests/scripts) | 对 CI、版本、消息目录、发布证据和回归脚本自身的测试。 |

## 文件说明

### tests

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../tests/CMakeLists.txt) | 查找 GoogleTest 和 Python，组织 support、unit、integration、contract 及可选 desktop 测试。注册架构、CLI、脚本和工作流检查，并设置默认测试语言。 |

### tests/cmake

| 文件 | 用途 |
|---|---|
| [RegisterArchitectureTests.cmake](../../../tests/cmake/RegisterArchitectureTests.cmake) | 将应用层、控制台、平台、模块目录和桌面依赖检查脚本注册到 CTest；Apple 平台还会注册开发 App 的合成构建契约测试。 |
| [RegisterCliTests.cmake](../../../tests/cmake/RegisterCliTests.cmake) | 注册双语目录、注释规则、CLI 版本与帮助、日志输出边界、共享模型配置跨进程回归和离线 provider 冒烟检查。 |
| [RegisterWorkflowTests.cmake](../../../tests/cmake/RegisterWorkflowTests.cmake) | 注册 provider/fixture/release/CI 脚本单测、离线配置巡检、故障样例先失败后通过的测试，以及托管项目 CLI 生命周期验收。 |
| [VerifyApplicationBoundary.cmake](../../../tests/cmake/VerifyApplicationBoundary.cmake) | 同时检查 include 和目标链接，要求 Agent/Project 应用层只依赖 domain、ports、runtime 和 localization，不能直接依赖基础设施实现。 |
| [VerifyConsoleBoundary.cmake](../../../tests/cmake/VerifyConsoleBoundary.cmake) | 扫描生产代码中的标准流和 stdio 输出，要求直接进程输入输出只能出现在 Console 适配器中，防止业务代码绕过注入或污染机器通道。 |
| [VerifyDesktopBoundary.cmake](../../../tests/cmake/VerifyDesktopBoundary.cmake) | 检查 core/CLI 不依赖 Qt，GUI 应用层不泄漏 Qt/JSON/适配器，UI 不直接操作 QProcess 或基础设施，同时检查类型化应用接口和目标链接方向。 |
| [VerifyDiagnosticCli.cmake](../../../tests/cmake/VerifyDiagnosticCli.cmake) | 启动离线 CLI，检查早期错误不污染 JSONL 交互 stderr，禁用文件日志时不访问日志路径，日志目录不包围工作区，以及没有默认用户目录时仍能使用显式 Unicode 日志目录。 |
| [VerifyMacOSDevelopmentApp.cmake](../../../tests/cmake/VerifyMacOSDevelopmentApp.cmake) | 用合成程序和假 codesign 测试带空格路径的 App 创建、CLI 更新、删除后重建及签名范围。检查输出别名会被拒绝，且外部目录、原程序和 Qt SDK 不被改写。 |
| [VerifyModuleLayout.cmake](../../../tests/cmake/VerifyModuleLayout.cmake) | 检查功能目录和独立库目标是否存在；禁止未分组源码堆积、整层兼容 facade，以及 tools/CLI 对 core 总入口的依赖。 |
| [VerifyPlatformBoundary.cmake](../../../tests/cmake/VerifyPlatformBoundary.cmake) | 扫描生产头文件和源码，要求平台条件宏和原生系统头只出现在明确的 platform 实现或 command/filesystem native 后端中，防止跨平台业务代码出现平台分支。 |

### tests/scripts

| 文件 | 用途 |
|---|---|
| [ci_workflow_tests.py](../../../tests/scripts/ci_workflow_tests.py) | 静态检查矩阵层级、现存预设、Release 独立 Sanitizer、仅在 Linux fast 中执行的 GUI 检查和固定 Qt 安装。另测试提交空白的检查范围和发布资产集合，防止重新使用旧构建入口。 |
| [comment_language_tests.py](../../../tests/scripts/comment_language_tests.py) | 扫描 C/C++、脚本、CMake 和工作流中的自然语言注释，要求同块包含 EN 和 ZH-CN。区分字符串、命名空间注释和机器指令，以减少误报。 |
| [fixture_regression_tests.py](../../../tests/scripts/fixture_regression_tests.py) | 用 mock 和临时工程测试故障修复验收脚本的权限限制、失败基线与独立验证的执行顺序、源码变化观察、预算和 macOS 预检。也检查失败报告脱敏、无凭据时停止，以及证据不可覆盖。 |
| [localization_catalog_tests.py](../../../tests/scripts/localization_catalog_tests.py) | 检查双语目录的形状、键和占位符一致，且 C++ 标识不冲突。生产代码必须使用生成的类型化消息 ID，不能遗漏翻译或硬编码面向用户的文案。 |
| [model_config_cli_tests.py](../../../tests/scripts/model_config_cli_tests.py) | 用临时状态、合成凭据和本机流式 HTTP 服务测试 CLI 共享配置及运行快照。覆盖默认 demo 工具、不隐式加载工作区配置、无用户目录时的显式覆盖，以及双语帮助。 |
| [provider_regression_tests.py](../../../tests/scripts/provider_regression_tests.py) | 测试 provider 清单路径约束、live 模式显式选择、缺凭据时确定性阻塞、结果字段脱敏、证据不可覆盖及报告与源码绑定；不发送真实 provider 请求。 |
| [release_evidence_tests.py](../../../tests/scripts/release_evidence_tests.py) | 用合成报告检查正式证据对应已提交的功能源码，且 provider/fixture 匹配。拒绝过期、非 live、失败、重试超额、字段不完整或含原始秘密的记录，缺失的缓存统计保持未知。 |
| [validate_version_tests.py](../../../tests/scripts/validate_version_tests.py) | 检查带日期的正式版本能通过校验；对 tag 而言，Unreleased、开发中标记、缺少当前版本链接或 Changelog 节都会使校验失败。 |

[导读目录](../README.md)

上一页：[文档、配置模板与消息](../engineering/docs-config.md) · 下一页：[单元测试](unit.md)
