# 文档、配置模板与消息

[导读目录](../README.md) · [工程设施](README.md)

项目文档、模型配置示例和中英文消息目录。

## 源码目录

| 目录 | 用途 |
|---|---|
| [configs](../../../configs) | 可提交的模型配置模板与 provider 回归矩阵。 |
| [configs/providers](../../../configs/providers) | 各 provider 的协议、模型与预算模板，线上验收结果另行记录。 |
| [docs](../..) | 用户与开发文档。 |
| [docs/concepts](../../concepts) | 运行流程、模块分工和依赖方向的概念说明。 |
| [docs/development](../../development) | 代码规范、本地验证和正式发布流程。 |
| [docs/getting-started](../../getting-started) | 从构建、初始化到配置模型和运行任务的操作手册。 |
| [docs/guides](../../guides) | 权限、安全边界和故障恢复的专题说明。 |
| [docs/project](../../project) | 功能进度、发布缺口、已知限制和后续安排。 |
| [docs/reference](../../reference) | 模型配置字段、协议能力、预算和用量的参考资料。 |
| [locales](../../../locales) | 中英文消息数据源；消息索引在构建时生成。 |

## 文件说明

### configs

| 文件 | 用途 |
|---|---|
| [provider-regression.json](../../../configs/provider-regression.json) | 登记 provider 回归检查及对应配置模板，不记录服务是否通过线上验收。 |

### configs/providers

| 文件 | 用途 |
|---|---|
| [claude-messages.json](../../../configs/providers/claude-messages.json) | Anthropic Messages 协议配置模板，声明 provider、模型、流式输出与回答预算。 |
| [custom-chat.json](../../../configs/providers/custom-chat.json) | 自定义 Chat Completions 兼容端点模板，显式声明工具调用、流式输出和 Token 参数等能力。 |
| [deepseek-chat.json](../../../configs/providers/deepseek-chat.json) | DeepSeek 的 Chat Completions 配置模板，启用流式输出并将自动重试设为零。 |
| [gemini-chat.json](../../../configs/providers/gemini-chat.json) | Gemini 的 Chat Completions 兼容接口模板，声明 provider、模型、流式输出与回答预算。 |
| [grok-responses.json](../../../configs/providers/grok-responses.json) | Grok 的 Responses 协议配置模板，声明 provider、模型、流式输出与回答预算。 |
| [groq-chat.json](../../../configs/providers/groq-chat.json) | Groq 的 Chat Completions 模板，使用非流式请求并显式限制请求 Token 预算。 |
| [kimi-chat.json](../../../configs/providers/kimi-chat.json) | Kimi / Moonshot 的 Chat Completions 配置模板，声明模型、流式输出与回答预算。 |
| [openai-codex.json](../../../configs/providers/openai-codex.json) | 通过 Responses API 使用 Codex 模型的配置模板，不包含 Codex 桌面应用集成。 |
| [openai-responses.json](../../../configs/providers/openai-responses.json) | OpenAI Responses 协议的模型、流式输出与回答预算模板。 |

### docs/concepts

| 文件 | 用途 |
|---|---|
| [architecture.md](../../concepts/architecture.md) | 说明任务循环、模块分工和依赖方向，以及 GUI 子进程、平台接口和本地状态文件。 |

### docs/development

| 文件 | 用途 |
|---|---|
| [code-style.md](../../development/code-style.md) | 约定代码格式、模块划分、平台代码放置方式和双语注释，附自动检查方法。 |
| [releasing.md](../../development/releasing.md) | 规定候选包、六平台测试、真实 provider 握手、修复 fixture、证据归档与正式 tag 的发布流程。 |
| [testing.md](../../development/testing.md) | 说明开发构建、测试分类、Sanitizer、GUI 离线测试和原生交互验收方法。 |

### docs/getting-started

| 文件 | 用途 |
|---|---|
| [quickstart.md](../../getting-started/quickstart.md) | 用户操作手册：构建 CLI / GUI，初始化项目，配置模型，运行、恢复和查看任务。 |

### docs/guides

| 文件 | 用途 |
|---|---|
| [safety-and-recovery.md](../../guides/safety-and-recovery.md) | 说明 policy、写入授权、固定命令、验证门禁、事务恢复、凭据保护与已知隔离限制。 |

### docs/project

| 文件 | 用途 |
|---|---|
| [roadmap.md](../../project/roadmap.md) | 维护当前阶段、发布缺口和后续重点。 |

### docs/reference

| 文件 | 用途 |
|---|---|
| [model-providers.md](../../reference/model-providers.md) | 说明共享模型配置、密钥优先级、三类协议、provider 能力、Token 限额和 usage / cache 统计。 |

### locales

| 文件 | 用途 |
|---|---|
| [en.json](../../../locales/en.json) | 英文消息目录，按稳定消息 ID 提供 CLI、核心错误和桌面文字；构建时生成类型化消息索引。 |
| [zh-CN.json](../../../locales/zh-CN.json) | 简体中文消息目录，与英文目录共享消息 ID 和占位符契约。 |

[导读目录](../README.md) · [工程设施](README.md)

上一页：[CI 与发布](ci-release.md) · 下一页：[测试](../tests/README.md)
