# 测试

[← 返回文档树](../../README.md)

## 日常检查

开发时运行：

```bash
cmake --preset dev
cmake --build --preset dev
cmake --build --preset dev --target format-check
ctest --preset dev
```

## 按模块构建

CMake target 就是模块边界。指定 target 时，它依赖的模块会自动更新，其他无关模块不会重编：

```bash
cmake --build --preset dev --target mint_model
cmake --build --preset dev --target mint_tools
cmake --build --preset dev --target mint_agent
cmake --build --preset dev --target mint
```

使用 `cmake --build --preset dev --target help` 查看全部 target。需要清理后重编当前配置中的全部内容时运行：

```bash
cmake --build --preset dev --clean-first
```

GUI 的 Qt、FluentQt 和平台要求见[桌面界面](../getting-started/quickstart.md#桌面界面)。README 使用不含测试的 Release 配置；桌面开发另用 `dev` 和独立目录，并安装 Python 3.10+ 及 Qt Test 模块：

```bash
cmake --preset dev -B build/gui-tests \
  -DMINT_BUILD_GUI=ON \
  -DCMAKE_PREFIX_PATH="/path/to/Qt/6.x/kit"
cmake --build build/gui-tests --target mint_gui mint_desktop_tests mint_desktop_custom_model_tests --parallel 4
ctest --test-dir build/gui-tests --output-on-failure -L desktop
```

Sanitizer 同样是开发配置的临时开关：

```bash
cmake --preset dev -B build/sanitize -DMINT_ENABLE_SANITIZERS=ON
cmake --build build/sanitize --parallel 4
ctest --test-dir build/sanitize --output-on-failure --parallel 4
```

发布前运行：

```bash
bash scripts/release-check.sh
```

`release-check.sh` 会检查版本、格式、Debug、Release、ASan/UBSan、全部 CTest、离线 CLI 和本机发布包，不访问模型接口。

## 测试分组

使用 CTest 标签运行一类测试，例如：

```bash
ctest --preset dev -L architecture
```

| 标签 | 内容 |
|---|---|
| `unit` | 纯逻辑与小型组件 |
| `integration` | Agent、工具、命令和持久化协作 |
| `contract` | 协议、策略、恢复和稳定输出 |
| `architecture` | 依赖方向、模块目录和终端边界 |
| `localization` | 中英文资源、占位符和类型化 ID |
| `style` | 双语注释规则 |
| `provider` | 三种 adapter 与离线回归 |
| `release` | 版本和发布证据脚本 |
| `desktop` | 桌面控制器、类型化协议、视图模型和 CLI 进程适配 |

测试源码按 `unit`、`integration`、`contract`、`desktop` 和 `support` 组织。Agent 的上下文、写入验证、会话恢复和 Token 预算分别放在 `agent_context_tests.cpp`、`agent_verification_tests.cpp`、`agent_session_tests.cpp`、`agent_token_budget_tests.cpp`；桌面按模型、工作区、会话画布和设置交互归组。专用 fake 留在所属场景，只有复用夹具才共享；文件拆分不改变 CTest 名称和标签。架构契约继续检查模块目录和依赖方向。

## 桌面交互与截图

完成前文桌面构建后，用离线 demo 导出固定尺寸截图：

```bash
./build/gui-tests/mint-gui --root . --demo \
  --state-dir /path/to/test-state \
  --window-size 1440x900 --screenshot /tmp/mint-gui.png
```

桌面集成测试使用确定性 demo 模型和临时状态目录，不访问模型服务。截图模式只做离屏渲染，不显示或抢占窗口；`--window-size` 用于固定验收尺寸，`--screenshot-panel evidence|settings` 可在截图前打开对应抽屉。它用于本机视觉检查，不替代 Windows、macOS、Linux 各自的人工验收。

交互验收时去掉 `--screenshot`，使用临时工作区和 `--state-dir /path/to/test-state` 隔离任务、密钥、主题与侧栏偏好，避免修改日常会话。至少实际检查工作区展开/收起、会话切换、行尾按钮、抽屉反复开关、浅深主题切换与窄窗口滚动。`--demo` 的离线运行不能证明真实模型接口可用。

可追加 `--quality-report /tmp/mint-inspector.json`，在首次布局稳定后输出 FluentQt Inspector 报告。该报告只检查当时已显示的控件；应分别检查主界面和设置抽屉，不把零告警等同于交互验收。输入框的高 DPI 回归可运行：

```bash
QT_SCALE_FACTOR=2 ./build/gui-tests/tests/mint_desktop_tests --gtest_filter='DesktopInputs.*'
```

## 平台矩阵

| 系统 | x64 triplet | ARM64 triplet |
|---|---|---|
| Windows | `x64-windows` | `arm64-windows` |
| macOS | `x64-osx` | `arm64-osx` |
| Linux | `x64-linux` | `arm64-linux` |

本地和 CI 都使用 `dev`、`release`。CI 只从 `.github/build-matrix.json` 传入 triplet 和并行度；ARM64 构建在 ARM64 主机运行。

每次 PR 和主分支推送的 `fast` 流程还会在 Linux x64 的 CLI 检查后，用独立的 `build/gui` 构建 GUI 和两个桌面测试目标，并通过 `QT_QPA_PLATFORM=offscreen` 运行全部 `desktop` 标签测试；未发现测试也会失败。该步骤使用固定的 Qt 6.8.3，仅下载并缓存[官方仓库](https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_683/qt6_683/qt.qt6.683.linux_gcc_64/)中的 `qtbase`、`qtsvg`、`icu` 档案。安装入口固定到 [install-qt-action v4.3.1](https://github.com/jurplel/install-qt-action/releases/tag/v4.3.1) 的提交。

默认 presets、`full` 和 `release` 矩阵仍只构建 CLI。Linux 离屏门禁覆盖桌面代码的编译和自动化行为；其他系统的桌面构建、原生光标和窗口交互仍需各平台验收。

## 如何理解结果

- 本机通过只证明当前系统、架构和源码。
- Release 构建不包含 GoogleTest，不能代替 Debug 测试。
- 包验收只检查安装产物，不能代替 Sanitizer。
- 离线 provider 测试只证明协议解析，不证明真实服务可用。
- 跳过的平台条件测试不算对应平台已经验收。

打包、六平台工作流和真实模型证据见[发布](releasing.md)。
