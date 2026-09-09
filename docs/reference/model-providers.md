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

模板在源码树的 `configs/providers` 中，安装包中位于 `share/mint/providers`。它们供 CLI 和显式 `--config` 使用；桌面设置当前不加载在线模型目录，而是统一保存一份自定义模型配置。Models.dev 目录留待后续版本集成。

Cursor 和 GitHub Copilot 是客户端，不提供可直接交给 mint 的通用模型协议。需要相同模型时，应使用对应上游供应商的 API Key。

## 地址与认证

- 内置供应商可以省略 `endpoint`，mint 会按 adapter 选择官方地址。
- 代理或自定义服务必须填写完整 `endpoint`，并明确填写 `provider`。
- 远程地址必须使用 HTTPS；HTTP 只允许本机回环地址。
- `api_key_env` 保存环境变量名，密钥值只从环境读取。

旧字段 `api_url` 和内联 `api_key` 在 `1.0.x` 仍兼容。CLI 会提示迁移；新配置不要继续使用。

桌面设置页使用一个通用模型表单，可配置 Chat Completions、Responses 或 Anthropic Messages 协议以及 Base URL、模型 ID 和 API Key。GUI 和 CLI 都通过 `ModelConfigService` 读写同一份用户配置：`model.json` 保存模型字段，`credentials.json` 保存密钥。密钥不会读回表单，只显示已保存掩码。models.dev 模型目录暂未接入。

运行任务时，密钥只作为鉴权信息发往配置中的 endpoint，不进入模型消息、工具结果、`model.json`、QSettings、命令参数、任务记录或诊断日志。远程请求必须使用 HTTPS，本机回环可用 HTTP。Mint 的工具子进程使用环境白名单，不会继承模型密钥。文件仅允许当前系统用户访问，内容不加密，不会触发 Keychain 或 Credential Manager 弹窗；也不会把设置同步到云端。需要操作系统级加密时，仍可使用 `api_key_env` 配合外部密钥管理工具。

日常只需在设置页保存，或使用下面的 `mint config` 命令；不必分别编辑两个文件。`model.json` 是模型参数入口，`credentials.json` 是服务内部维护的密钥存储，不是第二套模型配置。`mint config show --json` 的 `config` 字段给出实际入口路径。

两个文件都位于用户状态目录（`--state-dir` 可覆盖）：

- macOS：`~/Library/Application Support/mint/`
- Linux：`$XDG_STATE_HOME/mint/`；未设置时为 `~/.local/state/mint/`
- Windows：`%LOCALAPPDATA%\mint\`

旧版本保存在 Keychain 或 Credential Manager 中的密钥不会自动迁移，以免升级时再次触发系统授权弹窗。升级后在 Mint 设置页重新保存一次；确认新任务可用后，可以手动删除旧的 `Mint model API key` 项。

## 共享读写入口

GUI 保存后，下一次 CLI 或 GUI 任务会直接读取该配置；CLI 修改后，GUI 自动刷新公开字段。GUI 保存尚未完成时不能启动或恢复另一任务，避免读到保存前的配置。正在运行的任务保留启动时的配置，不会中途换模型或密钥。语言、外观、工作区和任务组织仍属于本机 GUI 偏好，不混入模型配置。

```bash
mint config show --json                         # 查看共享配置，不输出密钥
mint config set --stdin-json --json < update.json
```

`set` 的标准输入格式如下。`expected_revision` 必须取自最近一次 `show` 的结果；仅首次创建时为空。字段变更与密钥操作在同一次保存中提交。

```json
{
  "expected_revision": "复制 show 返回的 revision",
  "adapter": "chat_completions",
  "base_url": "https://api.example.com/v1",
  "model": "your-model"
}
```

可选 `api_key` 表示保存新密钥，`remove_api_key: true` 表示移除，二者不能同时使用。含密钥的输入应由可信的本地入口传入，不要放进项目或 shell 参数。未提供密钥时，仅在协议和 endpoint 不变的情况下保留原密钥；更换服务不会把旧密钥发往新地址。

保存使用跨进程锁和版本检查；过期的 `expected_revision` 返回 `conflict`（退出码 2），不会覆盖更新。新密钥先写入私有存储，`model.json` 原子替换后才生效；旧密钥在提交前仍可使用。不要手工修改 `credentials.json` 的引用关系。

### 旧配置与临时覆盖

项目中的 `config.json` 不再是默认入口。查看配置、切换工作区或启动任务都不会自动导入它；共享配置缺失时显示未配置，而不是悄悄恢复旧模型或密钥。首次迁移旧文件需明确执行：

```bash
mint config import --config /path/to/old.json
```

已有共享配置时，普通导入返回 `conflict`（退出码 2）。如需用已选定的旧配置替换共享配置，先用 `mint config show --json` 取得当前 `revision`，再显式执行：

```bash
mint config import --config /path/to/legacy.json \
  --replace --expected-revision '<当前 revision>'
```

两个替换参数必须同时提供；版本过期时返回冲突，不覆盖并发更新。导入保留高级请求参数，把内联密钥或匹配旧 provider/endpoint 的私有密钥转入共享存储，不借用其他模型的已保存密钥；原文件不修改、不删除。确认新任务可用、旧文件没有需要保留的差异后，可归档或删除旧文件，不再维护两套配置。

升级兼容仅处理 GUI 原先明确选择的 `QSettings model/config` 指针。无共享配置时导入一次；已有共享配置时显示两份配置供用户明确选择导入旧 GUI 配置或保留共享配置，在解决前不启动真实模型任务。成功导入、明确保留共享配置或成功保存新默认后才清除旧指针；失败和并发冲突会保留来源，不会静默退回另一模型。新 GUI 不再生成 `custom-*.json`。

显式 `--config /path/to/provider.json` 仅覆盖本次运行，不修改共享默认配置。该覆盖文件不会自动借用其他 endpoint 的已保存密钥。`--api-key-env NAME` 仍可覆盖本次任务的认证方式。

离线查看最终配置：

```bash
mint provider
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

自动预算会区分已核验的模型窗口与未知端点：官方 DeepSeek Chat Completions 端点的 `deepseek-v4-flash` / `deepseek-v4-pro` 使用[官方文档中的 1M 上下文窗口](https://api-docs.deepseek.com/quick_start/pricing/)，来源标记为 `model_profile`；未知型号、别名和代理端点继续使用 8000 的兼容预算。显式 `max_request_tokens` 优先，较小的速率响应头仍会收紧预算。模型窗口不会放宽任务本身的 `max_context_bytes` 限制，也不等同于每分钟 Token 额度。

任务 policy 的 `max_total_tokens` 控制整个任务的累计 usage，并随 checkpoint 恢复。它依赖供应商实际返回的 usage，不是金额保证；缺少 usage 时，结果会标记为 best effort 或 unavailable。

## 真实兼容性测试

```bash
mint provider test
```

这条命令固定发送两轮请求：第一轮要求调用回显工具，第二轮返回工具结果。它不读取工作区，不执行命令，失败后不重试，但会消耗模型额度。

缓存统计覆盖全部调用时，报告中的 `cache_hit_rate` 等于累计缓存输入 tokens 除以累计输入 tokens；任一调用缺少 usage 或缓存字段时为 `null`，`cache_usage_reports` 表示确有缓存统计的调用数。供应商明确报告零命中才显示 0%，旧检查点缺少统计覆盖信息时也按未知处理。mint 不会为了测试缓存额外发送请求；单次结果为 0% 不代表协议不兼容。

正式发布的真实模型证据见[发布流程](../development/releasing.md#真实模型证据)。
