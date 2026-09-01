# 开始使用 mint

[← 返回 README 文档树](../../README.md)

## 安装

当前源码版本是 `v1.0.0`。新的预编译包尚未发布，请先从源码构建。

需要 CMake 3.24+、Ninja、C++20 编译器和 vcpkg。先设置 vcpkg 路径：

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
~~~

Windows PowerShell 使用：

~~~powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
~~~

Linux 上如果要让 Agent 运行构建或测试，还需要安装 Bubblewrap。Ubuntu / Debian 示例：

~~~bash
sudo apt-get install bubblewrap
~~~

然后构建当前机器的 Release 版本：

~~~bash
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
~~~

生成的程序位于 `build/vcpkg-release/mint`，Windows 下为 `mint.exe`。把该目录加入 `PATH`；需要指定系统和架构时，使用[构建矩阵](../development/testing.md#平台矩阵)中的 preset。

## 先跑离线演示

~~~bash
mint init --root .
mint run --root . --demo "总结这个目录"
~~~

`--demo` 使用确定性本地模型，不需要 API Key，也不会写文件或运行项目命令。

## 配置真实模型

仓库带有不含密钥的配置模板，位于 `configs/providers`。安装包发布后，同一组模板会放在 `share/mint/providers`。

| 服务 | 配置文件 | API Key 环境变量 |
|---|---|---|
| OpenAI / Codex | `openai-codex.json` | `OPENAI_API_KEY` |
| Anthropic / Claude | `claude-messages.json` | `ANTHROPIC_API_KEY` |
| Google / Gemini | `gemini-chat.json` | `GEMINI_API_KEY` |
| xAI / Grok | `grok-responses.json` | `XAI_API_KEY` |
| Moonshot / Kimi | `kimi-chat.json` | `MOONSHOT_API_KEY` |
| Groq Chat Completions | `groq-chat.json` | `GROQ_API_KEY` |
| OpenAI Responses | `openai-responses.json` | `OPENAI_API_KEY` |
| DeepSeek Chat Completions | `deepseek-chat.json` | `DEEPSEEK_API_KEY` |
| 自定义兼容接口 | `custom-chat.json` | `MINT_MODEL_API_KEY` |

以 Groq 为例：

~~~bash
export GROQ_API_KEY='你的密钥'
cp configs/providers/groq-chat.json config.json
mint provider --config config.json
~~~

Windows PowerShell 设置密钥：

~~~powershell
$env:GROQ_API_KEY = '你的密钥'
~~~

`mint provider` 只解析配置并显示最终 endpoint、adapter、能力和 token 参数，不会请求模型，也不会输出密钥。内置服务只写 `provider` 即可使用默认 endpoint；旧配置中的官方根地址也会自动补全。代理或自定义接口用 `endpoint` 写完整请求地址，并明确写 `provider`；旧字段 `api_url` 继续兼容。只有自定义兼容接口允许覆盖 `capabilities`。远程接口必须使用 HTTPS，明文 HTTP 只允许 `localhost`、`127.0.0.0/8` 或 `::1` 本机回环地址。

密钥应只通过模板中的 `api_key_env` 指定环境变量名。旧字段 `api_key` 在 `1.0.x` 仍可运行，但 CLI 会提示迁移；不要把真实密钥写进配置文件或提交到仓库。

Codex 模型使用 OpenAI API Key。Cursor 和 GitHub Copilot 是编程客户端，不是独立的模型 HTTP 协议；它们的产品凭据不能当成 mint 的模型 Key。需要同款模型时，配置对应的 OpenAI、Anthropic 或 Google 等上游 Key。模板中的模型 ID 会随供应商更新，可按账号实际可用型号修改 `model`。

长任务还要给单次请求留出 Token 预算。`max_request_tokens` 设为 `0` 时，首轮按 8000 tokens 控制；成功响应带有 `x-ratelimit-limit-tokens` 后，mint 会采用更低的服务端上限。Agent 先扣除输出上限、工具定义和默认 256 tokens 的安全余量，再按保守的 2 bytes/token 估算压缩历史。不同模型的 tokenizer 不同，这只能降低 413 风险，不能保证完全避免；仍有问题时可调低 `request_token_estimate_bytes_per_token` 或 `max_request_tokens`。一分钟内连续请求耗尽额度仍可能返回 429。

`mint init` 新建的任务 policy 还会设置 `max_total_tokens: 100000`，按 provider 实际返回的 usage 控制整个任务；旧 policy 保持原值，需要时可手动加入该字段，`0` 表示关闭。mint 在每轮响应后检查累计值，因此最后一次请求可能越过目标值，但越界后不会再执行工具或发送下一次请求。它不是金额保证；若接口不返回 usage，结果会明确标记只能按已报告值尽力停止，mint 不会把缺失值当成零成本。

想先确认真实接口能否完成工具调用，可以运行：

~~~bash
mint provider test --config config.json
~~~

这条命令会消耗额度。它只发送两轮固定内容：第一轮要求调用一个回显工具，第二轮把工具结果交还模型。它不读取工作区、不执行命令，也不输出密钥或模型原文；每轮输出上限不超过 1024 tokens，并且失败后不重试。报告中的 `cache_hit_rate` 按累计缓存输入 tokens ÷ 累计输入 tokens 计算，不会为测缓存增加请求。

确认配置后运行：

~~~bash
mint run --root . "修复失败的测试，改完重新跑测试"
~~~

`config.json` 不要提交。所有可复制模板都以 `configs/providers` 为唯一来源，发布包中对应 `share/mint/providers`。

## 日常命令

~~~bash
mint init --root .        # 初始化权限和固定命令
mint run --root . "任务"  # 开始新任务
mint resume --root .      # 继续中断的任务
mint status --root .      # 查看任务状态
~~~

模型不能执行任意 shell，只能选择 `init` 登记的命令。文件访问受项目根目录和任务 policy 限制，任务记录保存在项目之外。

## 诊断与安全

mint 默认把不含正文和密钥的轮转 JSONL 日志写到系统状态目录的 `logs` 子目录。使用 `--log-file-level debug` 增加 provider、模型、工具和命令名称，使用 `--log-file-level off` 关闭磁盘日志。

~~~bash
mint status --root . --log-file-level debug
jq . "$HOME/Library/Application Support/mint/logs"/mint-*.jsonl
jq 'select(.event == "model.request.completed") | .fields' "$HOME/Library/Application Support/mint/logs"/mint-*.jsonl
~~~

日志不会记录 API Key、模型正文、命令输出、diff 或文件内容。三个平台默认要求命令沙箱；权限、日志路径、恢复方式和 `--unsafe-no-command-sandbox` 的风险统一见[安全、权限与任务恢复](../guides/safety-and-recovery.md)。
