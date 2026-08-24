# v1.4.0 验收记录

验收日期：2026-08-24

参考平台：macOS arm64，AppleClang 17

## 结论

| 范围 | 结果 |
|---|---|
| Debug、warnings-as-errors、clang-format | PASS |
| 常规 CTest | 7/7 PASS |
| ASan + UBSan CTest | 7/7 PASS |
| Chat Completions 向后兼容 | PASS |
| Chat / Responses 分片 SSE 契约 | PASS |
| Responses 两轮 CLI 工具闭环 | PASS |
| 流式 429 自动重试 | PASS |
| 新的真实外部 provider 复验 | 未执行 |

v1.4 完成了“多协议模型网关 + 可选流式传输”闭环。所有新增 provider 证据来自确定性纯协议测试和本地回环 HTTP 服务，不扩大为真实外部 provider 兼容声明。

## 对外契约

旧配置不需要修改：缺少新字段时等价于：

```json
{
  "adapter": "chat_completions",
  "stream": false
}
```

Responses 配置使用：

```json
{
  "adapter": "responses",
  "api_url": "https://api.openai.com/v1/responses",
  "api_key": "",
  "model": "your-model-id",
  "stream": true
}
```

公共类型 `ChatCompletionsConfig` 和 `ChatCompletionsClient` 作为 v1.3 源码兼容别名保留；新代码使用 `ModelProviderConfig`、`ModelProviderClient` 和 `ModelAdapter`。

## 协议证据

独立 `aiagent_v1_4_tests` 覆盖：

- v1.3 配置默认值、Responses 显式选择和错误字段诊断；
- Chat `messages/tools/max_completion_tokens` 与 Responses `input/tools/max_output_tokens` 翻译；
- Responses function tool 扁平化、`call_id`、`function_call_output` 和 usage 字段归一化；
- 未压缩历史保留 Responses 原始 output items，包括 reasoning continuation 数据；
- Chat 工具名称和 JSON 参数跨多个 SSE chunk 拼接；
- Responses `response.output_text.delta`、`response.function_call_arguments.delta` 和 `response.completed`；
- 任意网络分片与 CRLF，缺少完成事件、失败状态和无效 adapter 的拒绝路径；
- HTTP 429 的 SSE 错误正文不会抢先变成解析失败，而是进入既有退避重试。

协议映射按验收时的官方 OpenAI 文档实现：[Create a model response](https://developers.openai.com/api/reference/typescript/resources/beta/subresources/responses/methods/create) 和 [Responses streaming events](https://platform.openai.com/docs/api-reference/responses-streaming/response/function_call_arguments)。这些链接是实现依据，不是外部服务验收证据。

## CLI 端到端

验收测试启动真实 `aiagent` 子进程，并由本地服务返回两轮 Responses SSE：

```text
user task
  → response.function_call_arguments.delta
  → response.completed(function_call read_file)
  → Harness 读取真实 fixture README.md
  → 下一请求携带 function_call + function_call_output
  → response.output_text.delta
  → response.completed(final message)
  → 单个 --json 最终结果
```

通过条件：

1. CLI 两轮完成，执行一次 `read_file`；
2. 第二个 HTTP 请求保留 `call_id` 并包含真实工具结果；
3. 最终 JSON 的 adapter 为 `responses`，`streamed_calls=2`；
4. `--json` stdout 没有混入文本增量；
5. 流式正文不进入 JSONL progress，只有事件数和字节数。

## 工程门禁

常规配置：

```bash
cmake -S . -B build/v1_4-dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DAIAGENT_WARNINGS_AS_ERRORS=ON
cmake --build build/v1_4-dev --target format-check
cmake --build build/v1_4-dev --parallel
ctest --test-dir build/v1_4-dev --output-on-failure
```

结果：`7/7 tests passed`。

Sanitizer 配置增加 `-DAIAGENT_ENABLE_SANITIZERS=ON`，结果同样为 `7/7 tests passed`，没有 AddressSanitizer 或 UndefinedBehaviorSanitizer 报告。

## 边界

- 本次没有读取或发送本地真实 API Key，也没有调用外部模型。
- `stream=true` 只改变传输和终端呈现，不改变工具授权、写路径、recipe 或验证门禁。
- Responses 请求显式 `store=false`；需要连续推理的 output items 保存在受保护的本地 session 中。
- 文本终端会展示增量预览，任务最终答案仍以统一 `AgentResult` 为准。
- Linux/Windows 的原生命令安全后端仍未完成；v1.4 不改变这一平台边界。
