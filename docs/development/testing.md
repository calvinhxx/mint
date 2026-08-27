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

## 本地全量验证

需要 CMake 3.24+、Ninja、C++20 编译器和 vcpkg。

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
bash scripts/release-check.sh
~~~

这条命令检查版本号、代码格式、Debug、Release、ASan/UBSan、全部 CTest 和离线 CLI。
GitHub Actions 也执行同一条命令。

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

2026-08-27 在 macOS arm64、AppleClang 17 上的结果：

- Debug：50/50 tests passed；
- ASan + UBSan：50/50 tests passed；
- Release：构建通过，且未安装 GoogleTest；
- clang-format：通过；
- [GitHub Actions 发布门禁](https://github.com/calvinhxx/mint/actions/runs/33058098444)：通过。

50 个测试包括 11 个单元测试、23 个集成测试、12 个契约测试、2 个 CLI smoke 和 2 个独立验收流程。CTest 使用 GoogleTest discovery，因此每个场景可以单独筛选和报告。

## 测试覆盖到哪里

| 证据 | 主要检查 | 不能证明 |
|---|---|---|
| 单元与契约测试 | policy、工具、checkpoint、协议转换、输出边界 | 真实服务始终兼容 |
| 本地回环 HTTP 服务 | Chat / Responses、SSE、重试、两轮工具闭环 | 所有 provider 都可用 |
| 故障 fixture | 失败基线、修改、写后重新验证 | 任意项目都能自动修好 |
| 真实模型记录 | 当时端点和配置完成了隔离任务 | 当前版本或其他端点也通过 |

当前 fixture 位于 [`tests/fixtures/v1_broken_project`](../../tests/fixtures/v1_broken_project)。它先让 `calculator::add` 测试失败，再验证把减法修正为加法后 CTest 通过。

## 历史真实模型记录

这些记录没有在 v1.4 重新执行，只用于保留已有证据。

### v1.0

- 在 `/private/tmp` 的 fixture 副本中运行，不修改仓库基线；
- 只允许修改 `src/calculator.cpp` 和 `FIX_REPORT.md`；
- 只允许执行登记的 CMake、CTest 命令，并强制使用 macOS Seatbelt；
- 初始 CTest exit code 为 8，修改后的最后一次 CTest exit code 为 0；
- 共 16 轮、15 次工具调用、2 个文件变化；Agent 结束后独立 CTest 为 1/1 passed。

### v1.2

- 响应报告的模型为 `openai/gpt-oss-120b`，只代表当时 `config.json` 指向的端点；
- 共 12 轮、11 次工具调用，使用一次 changeset 同时修复源码并创建报告；
- 固定 recipe 先得到 `configure=0 / build=0 / test=8`，写后得到 `configure=0 / build=0 / test=0`；
- 普通 build 不能解除验证门禁，只有标记为 verification 的 test 可以；
- 最终 session 为 schema v3，独立 CTest 为 1/1 passed。

## 当前边界

- v1.4 没有读取真实 API Key，也没有调用外部模型；
- 完整验证平台仍是 macOS arm64；
- Linux 和 Windows 尚无正式的安全命令后端；
- 本地测试证明确定性行为，不替代真实 provider 回归；
- checkpoint 保证从稳定点恢复，不承诺跨进程 exactly-once。
