# 开始使用

[← 返回文档树](../../README.md)

## 构建

需要：

- CMake 3.24+
- Ninja
- C++20 编译器
- vcpkg

macOS 或 Linux：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset release
cmake --build --preset release
```

Windows PowerShell：

使用 MSVC 时，在 Visual Studio 的 Developer PowerShell 或对应架构的 Native Tools 命令行中构建。

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
cmake --preset release
cmake --build --preset release
```

程序位于 `build/release/mint`，Windows 下为 `mint.exe`。Linux 上运行项目命令还需要 Bubblewrap，例如 Ubuntu / Debian 使用 `sudo apt-get install bubblewrap`。

依赖版本固定在 `vcpkg.json`。复用旧 vcpkg 时，若提示找不到基线或依赖版本，先在 vcpkg 目录执行 `git pull --ff-only` 并重新运行 bootstrap；也可使用新克隆的 vcpkg。更换 `VCPKG_ROOT` 后，配置时加 `--fresh`。

## 离线试用

```bash
./build/release/mint init --root .
./build/release/mint run --root . --demo "总结这个目录"
```

`--demo` 使用本地确定性模型，不需要密钥，也不会写文件或运行项目命令。

## 桌面界面

桌面界面是可选的源码预览。默认 `dev`、`release` 只构建 CLI；开启 GUI 还需安装 Qt 6.5+ 桌面开发套件（含 Qt SVG）。CMake 使用上游推荐的 `FetchContent` 自动获取并编译 [FluentQt](https://github.com/calvinhxx/Fluent-Qt) v1.8.1，首次配置需要访问 GitHub。

### 构建与运行

在 Mint 仓库根目录执行，保持前文的 `VCPKG_ROOT` 设置。`CMAKE_PREFIX_PATH` 填包含 `lib/cmake/Qt6` 的桌面 kit 目录，例如 macOS 的 `/Users/你的用户名/Qt/6.9.3/macos`，不要填 Qt 安装器根目录：

```bash
cmake --preset release -B build/gui -DMINT_BUILD_GUI=ON \
  -DCMAKE_PREFIX_PATH="/path/to/Qt/6.x/kit"
cmake --build build/gui --target mint_gui --parallel 4
./build/gui/mint init --root .        # 仅首次初始化项目
./build/gui/mint-gui --root . --demo
```

FluentQt 与 Mint 共用编译器、Qt、架构和 macOS 最低系统版本，不构建其 Gallery、测试或 Python 绑定。依赖源码位于 `build/gui/_deps`；清空 `build` 后重跑上述命令即可重新获取和构建。开发测试的额外依赖见[测试](../development/testing.md)。

Windows 在匹配 Qt kit 的编译器环境中运行；MSVC 用户使用 Developer PowerShell。将配置命令合为一行，或把 Bash 的续行符 `\` 换成 PowerShell 的反引号。程序后缀为 `.exe`；启动前将 Qt kit 的 `bin` 加入 `PATH`，使系统能找到 Qt DLL。

更换 Qt kit、编译器或架构时使用新的构建目录，或在完整配置命令中加入 `--fresh`；仅修改 `CMAKE_PREFIX_PATH` 不会清除已有的 `Qt6_DIR` 等缓存路径。

### macOS App

可双击的本机开发 App 由构建系统生成：

```bash
cmake --build build/gui --target mint_gui_app --parallel 4
open build/gui/Mint.app --args --root "$PWD" --demo
```

该目标会同步同次构建的 GUI 与 CLI。删除 App 或清空 `build` 后重新执行即可。它使用本机 Qt，并只对自身文件作临时签名；不是可复制到其他电脑的独立发布包。

### 可选：本地 FluentQt 源码或 SDK

已有 FluentQt 1.8.1+ 源码或需要离线构建时，在配置命令中追加 `-DMINT_FLUENTQT_SOURCE_DIR=/path/to/Fluent-Qt` 即可跳过自动获取。将自己的源码放在 `build` 外，避免清空构建目录时一起删除。

使用已安装 SDK 时显式开启 `MINT_USE_SYSTEM_FLUENTQT`，并使用独立构建目录：

```bash
cmake --preset release -B build/gui-sdk -DMINT_BUILD_GUI=ON \
  -DMINT_USE_SYSTEM_FLUENTQT=ON \
  -DCMAKE_PREFIX_PATH="/path/to/Qt/6.x/kit" \
  -DFluentQt_DIR="/path/to/fluentqt/lib/cmake/FluentQt"
cmake --build build/gui-sdk --target mint_gui --parallel 4
```

SDK 需为 FluentQt 1.8.1+，并使用相同 Qt 主版本、兼容的 Qt 版本、架构和编译器。macOS 上 Mint 默认 deployment target 为 13.0；若 SDK 面向 macOS 15，配置时必须追加 `-DMINT_MACOS_DEPLOYMENT_TARGET=15.0`（并要求运行系统达到该版本），或重新构建面向 13.0 的 SDK。否则会出现 `was built for newer macOS version`，并被严格链接检查判为失败。

SDK 模式与 `MINT_FLUENTQT_SOURCE_DIR` 不可同时使用。FluentQt 以静态库接入，修改 SDK 源码后需要先重新构建并安装 SDK，再重新配置 Mint；源码接入则会在 Mint 构建中自动重编。

GUI 不复制 Agent 实现：桌面控制器通过结构化运行时端口调用 `mint` CLI，因此权限、任务恢复、验证和日志仍以 CLI 为事实源。`mint_gui` 会先构建它依赖的 CLI；运行时默认查找同目录的 `mint`，也可以通过 `--mint-cli /path/to/mint` 指定。`--demo` 只影响新建任务，已有任务历史会照常显示。

右键任务可置顶或归档；任务栏的更多菜单可切换到已归档任务。该状态按项目保存在本机，不会改动任务记录。

真实模型默认读取用户状态目录中的共享配置，与桌面设置共用，不随工作区改变。项目里的旧 `config.json` 不再自动读取或导入；`--config /path/to/provider.json` 仅覆盖本次运行。

## 配置模型

日常使用只需在 GUI 的「设置 → 模型」中保存一次，CLI 随即共用；无需在每个项目创建 `config.json`。`mint config show --json` 可查看当前配置及实际文件路径，不输出密钥。

仅使用 CLI 时，可首次导入仓库 `configs/providers` 中的无密钥模板。以 DeepSeek 为例：

```bash
export DEEPSEEK_API_KEY='your-api-key'
./build/release/mint config import --config configs/providers/deepseek-chat.json

./build/release/mint provider
./build/release/mint run --root . "总结当前项目"
```

`mint provider` 只检查配置，不发送请求，也不会导入旧文件。已有共享配置时，`config import` 返回冲突并保留现有设置；后续修改用 GUI 或 `mint config set --stdin-json`。模板的密钥放在指定环境变量中，不要写进项目文件。

供应商列表、兼容协议、endpoint 规则和 Token 预算见[模型配置](../reference/model-providers.md)。真实兼容性测试会消耗额度，运行前先阅读其中的说明。

## 常用命令

```bash
mint init --root .        # 创建项目策略
mint run --root . "任务"  # 开始任务
mint resume --root .      # 继续中断的任务
mint status --root .      # 查看任务状态
mint provider             # 离线检查模型配置
```

使用 `mint exec --help` 查看面向自动化的高级入口。

## 语言和日志

CLI 默认跟随系统语言，也可以明确指定：

```bash
mint --lang en --help
mint --lang zh-CN --help
```

桌面界面的设置按钮位于标题栏。模型区使用一套通用配置：选择兼容协议，填写 Base URL、模型 ID 和可选 API Key，保存后 GUI 和 CLI 的新任务共用。`mint config show --json` 可查看同一配置，`mint config set --stdin-json` 可修改；GUI 会刷新外部变更，有未保存编辑时保留草稿并检查版本冲突。API Key 只显示掩码，不写入项目配置、QSettings、命令参数、任务记录或日志。models.dev 目录尚未接入桌面界面。语言选择会保存在本机；启动时传入 `--lang` 只覆盖本次运行。

侧栏的文件夹按钮可添加工作区。每个工作区保留自己的任务列表、置顶和归档状态；侧栏只展开当前工作区，避免任务较多时拖慢切换。

未指定 `--root` 时，GUI 恢复当前设置目录中上次使用的有效工作区；首次启动先显示添加工作区入口，不会把启动进程的当前目录自动加入侧栏。从终端打开当前项目请显式使用 `--root .`。已删除的目录、文件系统根目录，以及与 Mint 设置目录相互包含的目录不能作为工作区。

GUI 同时执行一个任务。执行期间可以切换工作区、阅读其他会话和编辑草稿；返回原会话会恢复它的草稿或实时进度。后台结果不会替换正在阅读的会话。达到轮数或时间上限时会显示可继续状态，保留已有任务记录。

诊断日志默认写入系统状态目录。需要更多字段时使用 `--log-file-level debug`，完全关闭落盘使用 `--log-file-level off`。日志内容和任务数据的区别见[安全与恢复](../guides/safety-and-recovery.md#本地数据)。
