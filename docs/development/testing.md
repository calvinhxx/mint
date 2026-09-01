# 构建与测试

[← 返回 README 文档树](../../README.md)

## 本地全量检查

需要 CMake 3.24+、Ninja、C++20 编译器、Python 3.10+ 和 vcpkg。

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
bash scripts/release-check.sh
~~~

这条命令依次检查版本与配置、代码格式、Debug、Release、ASan/UBSan、全部 CTest、离线 CLI 和本机发布包。它不访问模型接口。

只验证正在开发的代码时，运行：

~~~bash
cmake --preset vcpkg-dev
cmake --build --preset vcpkg-dev
cmake --build --preset vcpkg-dev --target format-check
ctest --preset vcpkg-dev
~~~

只运行某类测试可以使用 CTest 标签，例如 `ctest --preset vcpkg-dev -L unit`。当前主要标签是 `unit`、`integration`、`contract`、`acceptance`、`architecture`、`provider` 和 `release`。

测试源码按 `unit`、`integration` 和 `contract` 组织，不再跟随历史版本号建目录。临时工作区、回环 HTTP 服务和命令子进程放在 `tests/support`，可复用的故障工程放在 `tests/fixtures`。`architecture` 标签还会检查终端输出边界、application 对 ports 的依赖方向，以及源码目录和 CMake target 是否保持模块化。

## 平台矩阵

测试 preset 带 GoogleTest，Release preset 只生成产品包：

| 系统 | x64 测试 / Release | ARM64 测试 / Release |
|---|---|---|
| Windows | `vcpkg-windows` / `vcpkg-windows-release` | `vcpkg-windows-arm64` / `vcpkg-windows-arm64-release` |
| macOS | `vcpkg-osx-x64` / `vcpkg-osx-x64-release` | `vcpkg-osx` / `vcpkg-osx-release` |
| Linux | `vcpkg-linux` / `vcpkg-linux-release` | `vcpkg-linux-arm64` / `vcpkg-linux-arm64-release` |

这些 preset 都是原生构建：ARM64 preset 应在 ARM64 主机运行。CI 的平台、runner 和 vcpkg triplet 由 `.github/build-matrix.json` 统一描述，并由脚本检查它与 CMake preset 是否一致。

普通 PR 和 main push 只进入 `CI`，运行 Linux x64 Debug、CTest 与格式检查。需要检查所有系统时，手动运行 `Full Tests`。发布候选和 tag 进入 `Release`，运行六平台 Debug 测试、Release 构建与包验收，并增加 macOS Sanitizer；只有 tag 会校验正式发布证据并创建 GitHub Release。

~~~bash
gh workflow run full-tests.yml --ref main
~~~

## 发布包

本机构建并检查当前平台的包：

~~~bash
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
cpack --config build/vcpkg-release/CPackConfig.cmake
cmake -DMINT_BUILD_DIR=build/vcpkg-release -P cmake/VerifyPackage.cmake
~~~

包位于 `build/vcpkg-release/packages`。验收会解包运行 `mint --version`，检查 provider 模板、README 文档树、项目与依赖许可证，并执行无密钥 provider 配置检查。

正式 tag 前可以手动生成六平台候选包：

~~~bash
gh workflow run release.yml --ref main
~~~

该任务上传六个归档和对应 SHA-256，但不会创建 GitHub Release。版本 tag 必须与 CMake、vcpkg、运行时版本、Changelog 和用户文档一致，发布证据也必须绑定同一版本和源码。

## Provider 验收

下面两项不访问网络：

~~~bash
./build/vcpkg-dev/mint provider --config configs/providers/deepseek-chat.json
python3 scripts/provider-regression.py \
  --mint build/vcpkg-dev/mint \
  --manifest configs/provider-regression.json
~~~

真实握手和修复 fixture 会消耗额度，只在明确选择 profile 时运行：

~~~bash
./build/vcpkg-release/mint provider test --config configs/providers/deepseek-chat.json

python3 scripts/fixture-regression.py \
  --mint build/vcpkg-release/mint \
  --config configs/providers/deepseek-chat.json \
  --fixture tests/fixtures/broken_cpp_project \
  --live \
  --output build/fixture-regression.json
~~~

正式发布只要求一个 live profile 同时通过握手和 fixture。其他 provider 仍执行离线协议检查，不能因此声称都经过真实服务验证。脱敏后的正式证据保存在 `release/evidence/<version>`；原始响应、API Key 和临时 fixture 不进入仓库。

握手固定发送两次请求；生成正式证据时应把所选 profile 的 `max_retries` 设为 `0`，fixture 也使用该 profile 的请求上限。缓存命中率直接来自必要请求的累计 usage，不单独发送缓存探针；单次结果为 0% 不代表协议不兼容。

## 结果边界

- 本地通过只证明当前主机和当前源码；六平台支持以对应 runner 的原生构建与测试为准。
- Release preset 不包含 GoogleTest，包验收也不能代替 Debug 与 Sanitizer 测试。
- macOS 使用 Seatbelt，Linux 使用 Bubblewrap，Windows 使用 AppContainer；三者限制越界写入和网络，但 macOS/Linux 仍可能只读访问部分宿主路径，不能当作虚拟机。
- Windows 尚无单文件大小硬限制；POSIX 资源统计和工作区磁盘限制也不是文件系统原生 quota。
- Windows 和 macOS 包尚未签名，macOS 尚未 notarize。
- 三种 adapter 都有本地协议与回环服务测试；某个 provider 的真实握手不能替代其他 provider 的线上证据。当前 `v1.0.0` 尚无已提交的真实发布证据。
