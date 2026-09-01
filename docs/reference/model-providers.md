# 模型配置

[← 返回文档树](../../README.md)

mint 把“供应商”和“请求协议”分开处理。供应商决定默认地址、认证方式和能力；adapter 负责请求与响应格式。

## 协议

| adapter | 常见服务 |
|---|---|
| `chat_completions` | DeepSeek、Groq、Gemini、Kimi 和兼容接口 |
| `responses` | OpenAI / Codex、xAI / Grok |
| `anthropic_messages` | Anthropic / Claude |

协议转换统一输出 `ModelReply`，Agent Loop 不依赖具体供应商。

## 内置模板

| 服务 | 模板 | 密钥环境变量 |
|---|---|---|
| OpenAI / Codex | `openai-codex.json` | `OPENAI_API_KEY` |
| OpenAI Responses | `openai-responses.json` | `OPENAI_API_KEY` |
| Anthropic / Claude | `claude-messages.json` | `ANTHROPIC_API_KEY` |
| Google / Gemini | `gemini-chat.json` | `GEMINI_API_KEY` |
| xAI / Grok | `grok-responses.json` | `XAI_API_KEY` |
| Moonshot / Kimi | `kimi-chat.json` | `MOONSHOT_API_KEY` |
| Groq | `groq-chat.json` | `GROQ_API_KEY` |
| DeepSeek | `deepseek-chat.json` | `DEEPSEEK_API_KEY` |
| 自定义兼容接口 | `custom-chat.json` | `MINT_MODEL_API_KEY` |

模板在源码树的 `configs/providers` 中，安装包中位于 `share/mint/providers`。

Cursor 和 GitHub Copilot 是客户端，不提供可直接交给 mint 的通用模型协议。需要相同模型时，应使用对应上游供应商的 API Key。

## 地址与认证

- 内置供应商可以省略 `endpoint`，mint 会按 adapter 选择官方地址。
- 代理或自定义服务必须填写完整 `endpoint`，并明确填写 `provider`。
- 远程地址必须使用 HTTPS；HTTP 只允许本机回环地址。
- `api_key_env` 保存环境变量名，密钥值只从环境读取。

旧字段 `api_url` 和内联 `api_key` 在 `1.0.x` 仍兼容。CLI 会提示迁移；新配置不要继续使用。

离线查看最终配置：

```bash
mint provider --config config.json
```

输出包含 endpoint、adapter、能力和请求限制，不包含密钥值。

## Token 预算

模型配置中的主要字段：

| 字段 | 作用 |
|---|---|
| `max_request_tokens` | 单次请求的输入与输出总预算；`0` 使用自动上限 |
| `max_completion_tokens` | 单次响应输出上限 |
| `request_token_safety_margin` | 为协议开销保留的余量 |
| `request_token_estimate_bytes_per_token` | 历史压缩时使用的保守估算 |

任务 policy 的 `max_total_tokens` 控制整个任务的累计 usage，并随 checkpoint 恢复。它依赖供应商实际返回的 usage，不是金额保证；缺少 usage 时，结果会标记为 best effort 或 unavailable。

## 真实兼容性测试

```bash
mint provider test --config config.json
```

这条命令固定发送两轮请求：第一轮要求调用回显工具，第二轮返回工具结果。它不读取工作区，不执行命令，失败后不重试，但会消耗模型额度。

报告中的 `cache_hit_rate` 等于累计缓存输入 tokens 除以累计输入 tokens。mint 不会为了测试缓存额外发送请求；单次结果为 0% 不代表协议不兼容。

正式发布的真实模型证据见[发布流程](../development/releasing.md#真实模型证据)。
