# 发布

[← 返回文档树](../../README.md)

发布证据必须来自同一份源码。构建成功、真实模型握手和六平台测试是三类不同证据，不能互相替代。

## 本机候选包

```bash
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
cpack --config build/vcpkg-release/CPackConfig.cmake
cmake -DMINT_BUILD_DIR=build/vcpkg-release -P cmake/VerifyPackage.cmake
```

归档和 SHA-256 位于 `build/vcpkg-release/packages`。验收会运行 `mint --version` 和中英文帮助，检查 provider、locale、双语 README、文档树与许可证，并执行无密钥配置检查。

## 六平台验证

普通 PR 和 main push 运行 Linux x64 快速门禁。完整测试和候选包使用独立工作流：

```bash
gh workflow run full-tests.yml --ref main
gh workflow run release.yml --ref main
```

`Full Tests` 运行六个平台的 Debug 测试。`Release` 构建六个平台的 Release 包，并在 macOS 增加 Sanitizer。手动运行不会创建 GitHub Release；只有版本 tag 会进入正式发布路径。

## 真实模型证据

正式版本只要求一个 provider 同时通过握手和修复 fixture。先复制对应模板到工作区外或 `config.json`，将 `max_retries` 设为 `0`，再运行：

```bash
./build/vcpkg-release/mint provider test --config config.json

python3 scripts/fixture-regression.py \
  --mint build/vcpkg-release/mint \
  --config config.json \
  --fixture tests/fixtures/broken_cpp_project \
  --live \
  --output build/fixture-regression.json
```

握手固定使用两次请求，fixture 使用完成任务所需的请求。不要无提示重试，也不要为了缓存命中率额外调用模型。

脱敏后的正式证据保存在 `release/evidence/<version>`。原始响应、提示词、API Key 和临时 fixture 不进入仓库。一个 provider 的真实结果不能代表其他 provider 已完成线上验证。

## Agent 评测

`evals/scenarios.json` 包含读取、搜索、修改、验证和工具边界的种子场景。它用于回归，不是模型排行榜。

运行场景后保留 `mint run --json` 的结果，再收集脱敏事件：

```bash
python3 scripts/eval-regression.py collect \
  --scenario read-project-overview \
  --result build/evals/raw-result.json \
  --events /path/to/events.jsonl \
  --artifacts build/evals/artifacts

python3 scripts/eval-regression.py score \
  --artifacts build/evals/artifacts \
  --output build/evals/report.json
```

同一报告只接受相同 provider、adapter 和 model 的产物。离线采集与评分通过，不代表真实模型已经运行。

## Tag 前检查

1. `scripts/release-check.sh` 通过；
2. 一个 provider 的握手和 fixture 证据已脱敏并绑定当前源码；
3. 六平台测试、Release 包和校验值通过；
4. CMake、vcpkg、运行时版本、Changelog 和用户文档一致；
5. 当前版本不再标记为 `Unreleased` 或“尚未发布”。

版本校验脚本会在 tag 流程中拒绝缺失证据或仍带预发布文案的仓库。
