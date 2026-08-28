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
| `tests/CMakeLists.txt` | 选择测试源码并注册验收流程 |

库目标同时提供 `mint::domain`、`mint::runtime`、`mint::infrastructure`、`mint::tools`、`mint::application` 和 `mint::core` 别名。新增源码时，只修改所属目录的 `CMakeLists.txt`。

## 平台构建矩阵

六个 preset 都在对应架构的原生系统上配置、编译并运行适用的 CTest：

| 系统 | x64 | ARM64 |
|---|---|---|
| Windows | `vcpkg-windows` | `vcpkg-windows-arm64` |
| macOS | `vcpkg-osx-x64` | `vcpkg-osx` |
| Linux | `vcpkg-linux` | `vcpkg-linux-arm64` |

替换下面的 `PRESET` 即可本地复现：

~~~bash
cmake --preset PRESET
cmake --build --preset PRESET
ctest --preset PRESET
~~~

这些 preset 面向同架构原生构建，例如 `vcpkg-linux-arm64` 应在 ARM64 Linux 上运行。CI 从 [`.github/build-matrix.json`](../../.github/build-matrix.json) 读取 runner、vcpkg triplet、CMake preset 和平台运行时依赖；校验脚本会检查它们是否一致。

2026-08-28，Windows、macOS、Linux 的 x64 / ARM64 六条原生流水线均会构建并运行适用测试。Linux 两条流水线验收 Bubblewrap；Windows 两条流水线验收无 shell 启动、argv、环境与句柄过滤、超时、取消、恢复和 Job Object 资源限制。

这里的“支持”表示代码能够在目标系统编译并运行该系统适用的自动测试。macOS 命令使用 Seatbelt；Linux 使用 Bubblewrap，并由矩阵安装 `bubblewrap` 包。Windows 没有文件与网络沙箱，测试会确认安全模式拒绝启动，不把受控进程后端写成安全沙箱。

Linux 沙箱测试会真实检查四件事：工作区内可以写、工作区外不能写、受保护文件不能读、命令不能访问宿主网络。Bubblewrap 不可用时不会静默降级；只有用户显式传入 `--unsafe-no-command-sandbox` 才能关闭这层保护。Ubuntu hosted runner 默认用 AppArmor 限制 user namespace，CI 因此只为 `/usr/bin/bwrap` 加载临时 `userns` profile，不关闭系统级限制。

资源限制测试会真实触发 CPU、内存和单文件大小上限。macOS 的内存用父进程监控，Linux 使用 `RLIMIT_AS`；Sanitizer 构建不启用内存上限。Windows 另外创建子进程验证 Job Object 的进程树上限。Windows 不支持 `file_size_bytes`，测试确认非零配置会被拒绝。

## 本地全量验证

需要 CMake 3.24+、Ninja、C++20 编译器和 vcpkg。

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
bash scripts/release-check.sh
~~~

这条命令检查版本号、构建矩阵、代码格式、Debug、Release、ASan/UBSan、全部 CTest 和离线 CLI。GitHub Actions 除了执行这条 macOS ARM64 深度门禁，还会运行上面的六组合构建矩阵。

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

2026-08-28 在 macOS arm64、AppleClang 17 上的结果：

- Debug：51/51 tests passed；
- ASan + UBSan：51/51 tests passed；
- Release：构建通过，且未安装 GoogleTest；
- clang-format：通过；
- GitHub Actions 六平台与发布门禁：[运行 33139754990](https://github.com/calvinhxx/mint/actions/runs/33139754990)，全部通过。

51 个测试包括 11 个单元测试、24 个集成测试、12 个契约测试、2 个 CLI smoke 和 2 个独立验收流程。CTest 使用 GoogleTest discovery，因此每个场景可以单独筛选和报告。

## 测试覆盖到哪里

| 证据 | 主要检查 | 不能证明 |
|---|---|---|
| 单元与契约测试 | policy、工具、checkpoint、协议转换、输出边界 | 真实服务始终兼容 |
| 本地回环 HTTP 服务 | Chat / Responses、SSE、重试、两轮工具闭环 | 所有 provider 都可用 |
| 故障 fixture | 失败基线、修改、写后重新验证 | 任意项目都能自动修好 |
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
- 六组合矩阵覆盖原生构建和适用测试，Linux Bubblewrap 后端在 x64 / ARM64 验收；
- Windows 受控进程后端不等于文件与网络沙箱，安全模式仍会拒绝命令；
- 本地测试证明确定性行为，不替代真实 provider 回归；
- checkpoint 保证从稳定点恢复，不承诺跨进程 exactly-once。
