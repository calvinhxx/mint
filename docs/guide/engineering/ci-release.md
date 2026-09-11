# CI 与发布

[导读目录](../README.md) · [工程设施](README.md)

GitHub 工作流、六平台矩阵、provider 验收和发布证据。

## 源码目录

| 目录 | 用途 |
|---|---|
| [.github](../../../.github) | GitHub CI、依赖更新和发布自动化，以及 Linux CI 沙箱准备。 |
| [.github/actions](../../../.github/actions) | 供多个工作流复用的仓库内 composite action。 |
| [.github/actions/setup-native](../../../.github/actions/setup-native) | 安装和选择各平台原生编译器、构建工具与系统依赖。 |
| [.github/apparmor](../../../.github/apparmor) | Linux CI 使用 Bubblewrap 时需要的 AppArmor 策略。 |
| [.github/scripts](../../../.github/scripts) | 校验构建矩阵、工作流、源码空白和 Release 资产的辅助脚本。 |
| [.github/workflows](../../../.github/workflows) | 快速 CI、六平台完整测试及版本发布工作流。 |
| [scripts](../../../scripts) | 开发者执行 provider 回归、修复 fixture、版本与发布证据校验的入口。 |

## 文件说明

### .github/actions/setup-native

| 文件 | 用途 |
|---|---|
| [action.yml](../../../.github/actions/setup-native/action.yml) | 为原生构建准备环境：检查 runner 架构，配置 vcpkg 二进制缓存，加载 MSVC 环境或 Linux 系统依赖，并在受限 Ubuntu 上安装 bubblewrap 专用的 AppArmor 配置。 |

### .github/apparmor

| 文件 | 用途 |
|---|---|
| [mint-bwrap](../../../.github/apparmor/mint-bwrap) | 允许 /usr/bin/bwrap 创建 user namespace，供 Ubuntu CI 运行命令沙箱测试；宿主机的全局 AppArmor 限制仍然生效。 |

### .github

| 文件 | 用途 |
|---|---|
| [build-matrix.json](../../../.github/build-matrix.json) | 记录 Windows、macOS、Linux 各 x64/ARM64 原生 runner 的 triplet、并行数和系统包。fast 只选 Linux x64，full/release 选择六个平台。 |
| [dependabot.yml](../../../.github/dependabot.yml) | 每周检查 GitHub Actions 依赖更新，并限制同时打开的更新 PR 数量。 |

### .github/scripts

| 文件 | 用途 |
|---|---|
| [check-committed-whitespace.py](../../../.github/scripts/check-committed-whitespace.py) | 检查已提交代码中的空白错误。PR 使用 merge-base 范围，push 使用前后提交，新分支使用 HEAD，因此工作树干净也不会漏检提交中的问题。 |
| [select-build-matrix.py](../../../.github/scripts/select-build-matrix.py) | 从共享矩阵中选择 fast、full 或 release 层级，输出 GitHub Actions 所需的 JSON；结构非法或执行集合为空时失败。 |
| [setup-msvc.ps1](../../../.github/scripts/setup-msvc.ps1) | 通过 vswhere 查找含对应 C++ 工具链的 Visual Studio，再调用 vcvarsall 导出 x64/ARM64 原生构建环境，检查 cl、link 和 rc 是否可用。 |
| [validate-build-matrix.py](../../../.github/scripts/validate-build-matrix.py) | 检查六个平台的定义、runner 架构、triplet、测试层级和 dev/release 预设是否一致。重复场景、缺失平台或错误的构建/测试选项都会使检查失败。 |
| [validate-workflows.py](../../../.github/scripts/validate-workflows.py) | 静态检查工作流和复合 Action：外部 uses 必须固定到完整提交 SHA，本地引用的工作流或 Action 必须存在。 |
| [verify-release-assets.py](../../../.github/scripts/verify-release-assets.py) | 按版本和 release 矩阵检查六个平台的归档及 SHA-256 文件。缺件、多余文件、符号链接、校验值错误或文件名不匹配都会使检查失败。 |

### .github/workflows

| 文件 | 用途 |
|---|---|
| [_native-build.yml](../../../.github/workflows/_native-build.yml) | 共享构建工作流，负责选择矩阵、执行 dev 构建和 CTest，以及可选的 Release 打包验收。只有 fast 的 Linux x64 会额外安装固定版本的 Qt，单独构建 GUI 并运行 desktop offscreen 测试。 |
| [_validate.yml](../../../.github/workflows/_validate.yml) | 共享校验工作流，检查提交空白、版本、矩阵和 Action 引用。版本 tag 还必须通过正式发布证据检查，且对应提交须属于默认分支。 |
| [ci.yml](../../../.github/workflows/ci.yml) | 在 main push 和 PR 上执行源码校验，以及 Linux x64 fast 的原生和 GUI 检查，结果汇总为 CI Gate；同一引用的过时运行会被取消。 |
| [full-tests.yml](../../../.github/workflows/full-tests.yml) | 手动运行六个平台的 Debug 构建和测试，结果汇总为 Full Tests Gate。此层级不执行仅在 fast 中启用的 Qt GUI 构建步骤。 |
| [release.yml](../../../.github/workflows/release.yml) | 手动运行或创建版本 tag 时，构建六个平台的候选包，运行 macOS ARM64 Sanitizer 和资产校验。只有 tag 会在全部检查通过后发布，且不会替换已公开版本的资产。 |

### scripts

| 文件 | 用途 |
|---|---|
| [fixture-regression.py](../../../scripts/fixture-regression.py) | 默认离线检查登记的 provider 和故障样例。显式启用 live 后才复制样例、确认失败基线、运行受限修复任务、检查允许的变更，再独立重建并测试，最后输出绑定源码的脱敏证据。 |
| [provider-regression.py](../../../scripts/provider-regression.py) | 默认离线逐项检查登记的 provider。显式指定 live 和 profile 后才执行选定的握手；缺凭据时记录为阻塞，执行失败时记录为失败，并保存带源码摘要、不可覆盖的脱敏报告。 |
| [provider_matrix.py](../../../scripts/provider_matrix.py) | 解析已提交的 provider 清单，要求配置路径留在清单目录内且不重复。支持按 ID 或原配置路径查询，并生成清单及模板内容的稳定摘要。 |
| [release-check.sh](../../../scripts/release-check.sh) | 依次执行本机发布前检查：版本和工作流校验、dev 构建、格式检查和测试、Release 打包验收、独立 Sanitizer 构建，以及临时状态中的离线 init→run。 |
| [release_evidence.py](../../../scripts/release_evidence.py) | 统一写入新证据文件并拒绝覆盖；检查脱敏字段、请求、用量、工具链及源码和配置摘要，要求同一 provider 完成握手和修复验收。 |
| [validate-version.py](../../../scripts/validate-version.py) | 核对 CMake、vcpkg、运行时和 Changelog 的版本。校验 tag 时，还要求已发布文案、日期、当前版本链接，以及绑定当前源码的正式证据。 |

[导读目录](../README.md) · [工程设施](README.md)

上一页：[构建与打包](build.md) · 下一页：[文档、配置模板与消息](docs-config.md)
