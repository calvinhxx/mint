# 项目状态

[← 返回 README 文档树](../../README.md)

当前源码基线是 `v1.0.0`。mint 已具备可用的本地 Agent 主流程：模型循环、受限文件工具、固定命令、三平台命令隔离、任务恢复、验证门禁、provider 适配和本地诊断日志。

## 已有能力

- Windows、macOS、Linux 的 x64 / ARM64 原生构建配置；
- Chat Completions、Responses、Anthropic Messages 和 SSE 统一模型接口；
- 默认关闭写入，命令不经过 shell，并受 policy 与操作系统沙箱约束；
- checkpoint、changeset 事务和中断恢复；
- Debug、Sanitizer、契约测试、离线 provider 回归与六平台发布配置。

## v1.0.0 已完成

这个基线包含完整 CLI 主流程，不包含 GUI 或多 Agent 编排：

### 用户能力

- 固定命令造成的源码修改进入统一变更账本；越界、权限、路径别名、二进制和不可安全表示的变化会直接失败关闭；
- HTTP、SSE、模型文本、推理内容和工具调用都有独立资源上限；远程地址必须使用 HTTPS；
- Codex、Claude、Gemini、Grok、Kimi、Groq 和 DeepSeek 共用三种协议适配器，配置模板不含密钥；
- 三种协议统一统计输入、输出、缓存 tokens 和加权缓存命中率；
- 任务 policy 可限制累计 Token，预算与已报告 usage 会跨 checkpoint 恢复；
- 面向人的终端输出会转义控制字符，JSON / JSONL 机器协议保持原始结构；
- 发布包包含完整文档树、安全策略和唯一一组 provider 模板；
- 日常 CI 只跑一条快速 Debug 门禁，六平台测试和发布包留给候选版与 Tag；
- Anthropic 流式工具调用在 Clang、GCC 和 MSVC 的严格告警模式下均可构建。
- 工具 Schema 同时由运行时代码执行，未知字段和错误参数组合不会被静默忽略；
- 6 个种子 Agent 场景支持脱敏采集、离线回放和成功率、验证率、轮数、工具数、Token、缓存与耗时统计。

### 工程结构

- Agent 只依赖模型、工具、会话、事件和停止信号接口，具体实现由 CLI 组装；
- application 与 infrastructure 按 Agent、模型、命令、持久化、日志和文件安全拆成独立目录与 CMake target；
- 模型统计、执行统计和最终报告分离，集成测试也按 Agent、模型和命令职责拆开；
- 架构测试会阻止 application 反向依赖具体适配器，也会阻止实现重新堆回聚合目录；
- 公共头文件、测试目录和 CMake target 都按职责命名，不再保留未发布旧版本的兼容壳与版本标签。

## 发布验证

新的 `v1.0.0` Release 尚未发布，也没有已提交的真实 provider 证据。发布前必须在同一份源码上通过本地 Debug 与 Sanitizer 全量测试、一个真实 provider 的受控握手与修复 fixture，以及六个平台的测试、打包和校验。真实请求只保留脱敏统计，不保存提示词、模型回答、密钥或原始响应。

## 已知边界

- Windows 尚无单文件大小硬限制；
- macOS 与 Linux 的命令沙箱以阻止越界写入、网络和敏感路径读取为主，不提供完整宿主读取白名单；
- Windows 和 macOS 包尚未签名，macOS 尚未 notarize；
- Linux 预编译包以 Ubuntu 24.04 为兼容基线；
- `v1.0.0` 尚未选定正式真实回归的 provider；模板和离线协议测试不等于线上服务验证；
- 种子评测集尚未在固定真实模型配置上完整运行，不能当作模型质量基准；
- 持续聊天、GUI 和多 Agent 编排不在当前产品范围。

历史变化见 [Changelog](../../CHANGELOG.md)，验证方法与证据边界见[构建与测试](../development/testing.md)。
