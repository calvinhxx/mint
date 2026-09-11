# 发布

[← 返回文档树](../../README.md)

以下命令在仓库根目录执行。先提交发布所需的代码、构建文件、模型模板和 fixture，再构建和验证。构建成功、真实模型握手和六平台测试分别验收。

## 本机候选包

```bash
cmake --preset release
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake
cmake -DMINT_BUILD_DIR=build/release -P cmake/VerifyPackage.cmake
```

归档和 SHA-256 位于 `build/release/packages`。验收会运行 `mint --version` 和中英文帮助，检查 provider、locale、双语 README、文档树与许可证，并执行无密钥配置检查。

## 六平台验证

普通 PR 和 main push 运行 Linux x64 快速门禁。在已推送的候选分支上运行完整测试和候选包工作流：

```bash
release_ref="$(git branch --show-current)"
gh workflow run full-tests.yml --ref "$release_ref"
gh workflow run release.yml --ref "$release_ref"
```

核对两次运行的 `headSha` 均为待发布提交。`Full Tests` 运行六个平台的 Debug 测试；`Release` 构建六个平台的 Release 包，并在 macOS 运行 Sanitizer。手动运行不会创建 GitHub Release，版本 tag 才会触发发布。

## 真实模型证据

正式版本要求同一个 provider 通过握手和修复 fixture。下面只调用 `openai-responses`，使用[登记模板](../../configs/providers/openai-responses.json)。先在此模板中设置 `"max_retries": 0` 并提交，再重新构建；通过环境变量 `OPENAI_API_KEY` 提供密钥，模板里只保留变量名。

确认本次真实请求的调用范围和费用后运行：

```bash
python3 scripts/provider-regression.py \
  --mint build/release/mint \
  --profile openai-responses \
  --live \
  --output build/live-evidence/provider-regression.json

python3 scripts/fixture-regression.py \
  --mint build/release/mint \
  --config configs/providers/openai-responses.json \
  --fixture tests/fixtures/broken_cpp_project \
  --live \
  --output build/live-evidence/fixture-regression.json
```

握手使用两次请求，其余 profile 只做离线检查；fixture 按任务需要调用模型。脚本拒绝覆盖已有报告，失败后先检查原因，再决定是否重新采集。

两份报告都通过后，检查脱敏内容，将它们移至 `release/evidence/v<version>/`，保留上述文件名，再运行：

```bash
python3 scripts/validate-version.py --release-evidence
```

校验会核对版本、源码摘要、登记模板和 fixture。修改这些输入后须重新构建、采集证据；证据文件及[脚本列明的发布元数据](../../scripts/release_evidence.py)除外。`fixture-regression.py` 要求配置路径指向登记模板，不能使用复制出的 `config.json`。单独运行 `mint provider test` 也不会生成正式回归报告。

原始响应、提示词、API Key 和临时 fixture 不进入仓库。一个 provider 的通过结果只覆盖该 provider。

## Tag 前检查

1. `scripts/release-check.sh` 通过；
2. 一个 provider 的握手和 fixture 证据已脱敏并绑定当前源码；
3. 六平台测试、Release 包和校验值通过；
4. CMake、vcpkg、运行时版本、Changelog 和用户文档一致；
5. 当前版本不再标记为 `Unreleased` 或“尚未发布”。

版本校验脚本会在 tag 流程中拒绝缺失证据或仍带预发布文案的仓库。
