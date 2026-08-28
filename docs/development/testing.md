# 测试与验收

[← 返回 README 文档树](../../README.md)

这份文档只记录当前代码怎么验证，以及这些结果能说明什么。

## CMake 结构

根 `CMakeLists.txt` 只声明选项、查找依赖和进入子目录。每个目标的源码与依赖写在对应目录中：

| 位置 | 职责 |
|---|---|
| `src/*/CMakeLists.txt` | 定义 domain、runtime、infrastructure、tools、application 和 CLI 目标 |
| `cmake/MintTargetOptions.cmake` | C++20、头文件路径、警告选项 |
| `cmake/MintSanitizers.cmake` | ASan 和 UBSan |
| `cmake/MintDeveloperTools.cmake` | `format` 与 `format-check` |
| `cmake/MintTesting.cmake` | GoogleTest 目标和 CTest 标签 |
| `cmake/MintPackaging.cmake` | 安装目录、运行时依赖和 CPack 包名 |
| `cmake/VerifyPackage.cmake` | 解包、校验和、文件布局与版本冒烟 |
| `cmake/triplets` | 让 mint 与 macOS 静态依赖使用同一个最低系统版本 |
| `tests/CMakeLists.txt` | 选择测试源码并注册验收流程 |

库目标同时提供 `mint::domain`、`mint::runtime`、`mint::infrastructure`、`mint::tools`、`mint::application` 和 `mint::core` 别名。新增源码时，只修改所属目录的 `CMakeLists.txt`。

## 平台构建矩阵

每个平台各有测试和 Release preset。两者使用独立构建目录，正式包不会继承 GoogleTest 等测试依赖：

| 系统 | x64 测试 / Release | ARM64 测试 / Release |
|---|---|---|
| Windows | `vcpkg-windows` / `vcpkg-windows-release` | `vcpkg-windows-arm64` / `vcpkg-windows-arm64-release` |
| macOS | `vcpkg-osx-x64` / `vcpkg-osx-x64-release` | `vcpkg-osx` / `vcpkg-osx-release` |
| Linux | `vcpkg-linux` / `vcpkg-linux-release` | `vcpkg-linux-arm64` / `vcpkg-linux-arm64-release` |

选择表里的测试 preset，即可本地复现构建和 CTest：

~~~bash
cmake --preset PRESET
cmake --build --preset PRESET
ctest --preset PRESET
~~~

Release preset 只用于生成正式包，不注册 CTest。所有 preset 都面向同架构原生构建，例如 `vcpkg-linux-arm64` 应在 ARM64 Linux 上运行。CI 从 [`.github/build-matrix.json`](../../.github/build-matrix.json) 读取 runner、vcpkg triplet、测试 preset、Release preset 和平台运行时依赖；校验脚本会检查它们是否一致，并拒绝 Release preset 启用测试 feature。

2026-08-28，Windows、macOS、Linux 的 x64 / ARM64 六条原生流水线均会构建并运行适用测试。Linux 两条流水线验收 Bubblewrap；Windows 两条流水线验收 AppContainer、无 shell 启动、argv、环境与句柄过滤、超时、取消、恢复和 Job Object 资源限制。

这里的“支持”表示代码能够在目标系统编译并运行该系统适用的自动测试。macOS 命令使用 Seatbelt；Linux 使用 Bubblewrap，并由矩阵安装 `bubblewrap` 包；Windows 使用无网络 capability 的 AppContainer 和 DACL 授权。

沙箱测试会真实检查五件事：工作区内可以写、工作区外不能写、受保护文件不能读、命令不能访问宿主网络、显式授权的外部路径可读但不可写。Bubblewrap 不可用时不会静默降级；只有用户显式传入 `--unsafe-no-command-sandbox` 才能关闭这层保护。Ubuntu hosted runner 默认用 AppArmor 限制 user namespace，CI 因此只为 `/usr/bin/bwrap` 加载临时 `userns` profile，不关闭系统级限制。

资源限制测试会真实触发 CPU、内存、进程树数量、单文件大小和工作区磁盘上限，并检查工作区已经超限时不会启动新命令。macOS 的内存用父进程监控，Linux 使用 `RLIMIT_AS`；Sanitizer 构建不启用内存上限。Windows 不支持 `file_size_bytes`，测试确认非零配置会被拒绝。

## 本地全量验证

需要 CMake 3.24+、Ninja、C++20 编译器、Python 3.10+ 和 vcpkg。Python 只用于测试与发版脚本。

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
bash scripts/release-check.sh
~~~

这条命令检查版本号、构建矩阵、代码格式、Debug、Release、ASan/UBSan、全部 CTest、离线 CLI 和 Release 包。GitHub Actions 除了执行这条 macOS ARM64 深度门禁，还会运行上面的六组合构建矩阵。

## 安装与发布包

本机 Release 包可单独生成并验收：

~~~bash
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
cpack --config build/vcpkg-release/CPackConfig.cmake
cmake -DMINT_BUILD_DIR=build/vcpkg-release -P cmake/VerifyPackage.cmake
~~~

产物位于 `build/vcpkg-release/packages`。Windows 使用 `.zip`，macOS / Linux 使用 `.tar.gz`；每个包都有使用 LF 换行的 `.sha256`，可在任意平台校验。六平台 CI 都会解包运行 `mint --version`，并检查 provider 模板、项目许可证和依赖许可证。

macOS 使用 13.0 deployment target。Linux 包由 Ubuntu 24.04 runner 构建，不承诺兼容更旧的 glibc。Windows 和 macOS 归档当前未做商业证书签名或 notarization。

正式打 tag 前，可以在主分支手动跑一次六平台候选包：

~~~bash
gh workflow run CI --ref main -f release_packages=true
~~~

这次运行会用六个独立 Release preset 构建、解包验收并上传保留 7 天的 Actions artifacts，再由 Ubuntu 汇总检查文件数量和六份 SHA-256；它不会创建 GitHub Release。普通 push 和 PR 不上传这些包。

在主分支提交上推送与 CMake、vcpkg manifest 和 Changelog 日期一致的 `vMAJOR.MINOR.PATCH` tag 后，CI 会执行同一条打包路径。tag 还必须包含当前版本的两份真实回归证据；全部门禁和包验收通过后才公开 GitHub Release。

只排查某一种构建时，可以单独运行 preset：

~~~bash
export VCPKG_ROOT=/path/to/vcpkg

cmake --preset vcpkg-dev
cmake --build --preset vcpkg-dev
cmake --build build/vcpkg-dev --target format-check
ctest --preset vcpkg-dev

cmake --preset vcpkg-release
cmake --build --preset vcpkg-release

cmake --preset vcpkg-sanitize
cmake --build --preset vcpkg-sanitize
ctest --preset vcpkg-sanitize
~~~

2026-08-29 在 macOS arm64、AppleClang 17 上的结果：

- Debug：72/72 tests passed；
- ASan + UBSan：72/72 tests passed；
- Release：构建通过，且未安装 GoogleTest；
- Release 包：macOS ARM64 归档、SHA-256、文件布局和解包运行通过；
- clang-format：通过；
- GitHub Actions 会运行六平台矩阵和发布门禁；最新结果见 [Actions](https://github.com/calvinhxx/mint/actions)。

72 个测试包括 14 个单元测试、30 个集成测试、21 个契约测试、3 个 CLI smoke 和 4 个独立验收流程。CTest 使用 GoogleTest discovery，因此每个场景可以单独筛选和报告。

changeset 恢复测试会真实写两个文件并重建 `ToolRegistry`，覆盖：完整写入后崩溃、只完成部分文件、checkpoint 已确认但日志未清、外部改写、两个进程争用同一任务，以及 Agent 自动回滚并重放 in-flight changeset。测试同时检查 schema v2/v3 可迁移到 v4。

provider 契约测试会加载 `configs/providers` 下四份固定配置，检查端点识别、代理声明、能力冲突、token 字段、流式 usage、Responses 推理续传、DeepSeek Chat 的 `reasoning_content` 续传和报告脱敏。下面的命令不访问网络：

~~~bash
./build/vcpkg-dev/mint provider --config configs/providers/groq-chat.json --json
~~~

需要检查真实服务时，显式运行：

~~~bash
export GROQ_API_KEY='你的密钥'
./build/vcpkg-dev/mint provider test --config configs/providers/groq-chat.json --json
~~~

`provider test` 发出两个逻辑请求，检查唯一 function call、参数、call id、工具结果续接、流式状态和 usage。它不读取工作区；成功报告只保留协议、耗时、重试和 token 统计。输出上限固定为配置值与 1024 的较小者，每个逻辑请求最多尝试两次。仓库测试用一份跨平台回环 HTTP 服务运行 Chat 重试、Responses SSE、Agent 工具循环和同一条 provider 验收路径；六条平台流水线都会执行，不消耗外部额度。

发版前用同一条命令检查三份官方配置：

~~~bash
# 默认只做离线检查
python3 scripts/provider-regression.py --mint ./build/vcpkg-dev/mint

# 显式 --live 才会请求服务；结果文件不会覆盖已有文件
python3 scripts/provider-regression.py \
  --mint ./build/vcpkg-dev/mint \
  --live \
  --output build/provider-regression-v1.5.json
~~~

批次由 `configs/provider-regression.json` 声明。live 模式会先确认三份配置对应的 API Key 环境变量均已设置，避免执行到一半才发现缺少密钥。证据只保存白名单中的 profile、协议和统计字段，并记录配置矩阵的 SHA-256；原始响应、错误正文、密钥和本机配置路径不会写入文件。

三份握手通过后，用同一份 OpenAI Responses + SSE 配置运行隔离修复：

~~~bash
# 默认只检查配置、fixture 和 policy，不请求模型
python3 scripts/fixture-regression.py --mint ./build/vcpkg-dev/mint

# 显式 --live 才复制故障工程并请求模型；证据文件不会被覆盖
python3 scripts/fixture-regression.py \
  --mint ./build/vcpkg-dev/mint \
  --live \
  --output build/openai-responses-fixture-v1.5.json
~~~

脚本先在临时副本中确认 CTest 按预期失败，再按 fixture policy 启动 Agent。模型只能修改 `src/calculator.cpp` 和 `FIX_REPORT.md`，只能运行三条固定 recipe，并且最后必须通过 verification recipe。Agent 结束后，脚本会用另一个全新构建目录独立复测。JSON 证据只保留 profile、内容摘要、轮次、工具、Token、改动文件和验证状态；模型回答、diff、response id、事件原文和密钥不会写入。

两份结果的 `status` 都为 `passed` 后，把它们加入当前版本的发布证据，再运行一次离线校验：

~~~bash
mkdir -p release/evidence/v1.5.0
cp build/provider-regression-v1.5.json \
  release/evidence/v1.5.0/provider-regression.json
cp build/openai-responses-fixture-v1.5.json \
  release/evidence/v1.5.0/fixture-regression.json
python3 scripts/validate-version.py --release-evidence
~~~

证据会绑定生成它的功能源码、provider 配置和 fixture。之后只允许修改 Changelog、这份测试记录、Roadmap 和证据文件；其他功能改动会使校验失败并要求重新运行真实回归。正式 tag 会自动执行相同检查，缺少证据、离线结果、失败结果或包含原始响应字段的文件都不能发布。

## 测试覆盖到哪里

| 证据 | 主要检查 | 不能证明 |
|---|---|---|
| 单元与契约测试 | policy、工具、checkpoint、协议转换、输出边界 | 真实服务始终兼容 |
| 本地回环 HTTP 服务 | Chat / Responses、SSE、重试、脱敏的两轮 provider 验收 | 所有真实 provider 都可用 |
| 固定 provider 配置 | profile 解析和请求形状保持稳定 | 服务在线、密钥有效或模型可用 |
| 故障 fixture | 失败基线、修改、写后重新验证 | 任意项目都能自动修好 |
| 六平台包验收 | 归档可解开，CLI 可启动，运行时文件齐全 | 用户机器的所有系统策略都相同 |
| 真实模型记录 | 当时端点和配置完成了隔离任务 | 当前版本或其他端点也通过 |

当前 fixture 位于 [`tests/fixtures/v1_broken_project`](../../tests/fixtures/v1_broken_project)。它先让 `calculator::add` 测试失败，再验证把减法修正为加法后 CTest 通过。

## 真实模型记录

### v1.4

- 2026-08-27 在 `/private/tmp` 的 fixture 副本中运行，仓库基线没有修改；
- 模型响应报告为 `openai/gpt-oss-120b`，adapter 为 `chat_completions`，未开启 SSE；
- 初始独立 CTest 为 0/1，`calculator::add` 把加法写成了减法；
- Agent 共 9 轮、8 次工具调用，只修改 `src/calculator.cpp`；
- 3 次固定 recipe 全部成功，verification 状态为 `passed`；
- 2 次 429 按服务端时间等待后重试成功；
- 最终独立新建构建目录复测为 1/1 passed；
- 共使用 17,955 tokens，其中输入 17,177、输出 778、缓存 1,280。

### v1.2

- 响应报告的模型为 `openai/gpt-oss-120b`，只代表当时 `config.json` 指向的端点；
- 共 12 轮、11 次工具调用，使用一次 changeset 同时修复源码并创建报告；
- 固定 recipe 先得到 `configure=0 / build=0 / test=8`，写后得到 `configure=0 / build=0 / test=0`；
- 普通 build 不能解除验证门禁，只有标记为 verification 的 test 可以；
- 最终 session 为 schema v3，独立 CTest 为 1/1 passed。

### v1.0

- 在 `/private/tmp` 的 fixture 副本中运行，不修改仓库基线；
- 只允许修改 `src/calculator.cpp` 和 `FIX_REPORT.md`；
- 只允许执行登记的 CMake、CTest 命令，并强制使用 macOS Seatbelt；
- 初始 CTest exit code 为 8，修改后的最后一次 CTest exit code 为 0；
- 共 16 轮、15 次工具调用、2 个文件变化；Agent 结束后独立 CTest 为 1/1 passed。

## 当前边界

- v1.4 的真实外部证据只覆盖 Chat Completions 非流式配置；
- 六组合矩阵覆盖原生构建和适用测试，Linux Bubblewrap 和 Windows AppContainer 后端在 x64 / ARM64 验收；
- Windows AppContainer 不是虚拟机；工作区外的工具链必须通过 `command_read_paths` 显式授权；
- 工作区磁盘限制按普通文件逻辑大小巡检，不是文件系统原生 quota；
- 本地测试证明确定性行为，不替代真实 provider 回归；
- OpenAI、Groq、DeepSeek 和 custom 固定配置已有统一验收入口，但尚未记录本版本的真实外部运行；
- OpenAI Responses + SSE 隔离修复已有可重复脚本，但尚未消耗外部额度执行；
- session 管理的 `apply_changeset` 通过事务日志避免跨进程重复提交；`apply_patch`、命令和任意扩展工具不承诺 exactly-once。
