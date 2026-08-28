# 开始使用 mint

[← 返回 README 文档树](../../README.md)

## 安装

从 v1.5.0 开始，[Releases](https://github.com/calvinhxx/mint/releases) 提供 Windows、macOS、Linux 的 x64 和 ARM64 预编译包。下载与机器匹配的 `.zip` 或 `.tar.gz`，可用同名 `.sha256` 文件校验下载内容。

解压后，`bin` 里是程序，`share/mint/providers` 里是模型配置模板。把 `bin` 加入 `PATH` 后，下文可以直接使用 `mint`。

macOS 包的最低版本是 13.0；Linux 包在 Ubuntu 24.04 构建，较旧发行版建议源码构建。Windows 和 macOS 包目前没有商业代码签名；遇到系统来源警告时可改用源码构建，不要关闭全局安全策略。

### 从源码构建

需要 CMake 3.24+、Ninja、C++20 编译器和 vcpkg。先设置 vcpkg 路径：

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
~~~

Windows PowerShell 使用：

~~~powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
~~~

Linux 上如果要让 Agent 运行构建或测试，还需要安装 Bubblewrap。Ubuntu / Debian 示例：

~~~bash
sudo apt-get install bubblewrap
~~~

然后构建当前机器的 Release 版本：

~~~bash
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
~~~

生成的程序位于 `build/vcpkg-release/mint`，Windows 下为 `mint.exe`。把该目录加入 `PATH`；需要指定系统和架构时，使用[构建矩阵](../development/testing.md#平台构建矩阵)中的 preset。

## 先跑离线演示

~~~bash
mint init --root .
mint run --root . --demo "总结这个目录"
~~~

`--demo` 使用确定性本地模型，不需要 API Key，也不会写文件或运行项目命令。

## 配置真实模型

仓库和发布包都带了四份可直接检查的配置。源码目录是 `configs/providers`，发布包目录是 `share/mint/providers`。

| 服务 | 配置文件 | API Key 环境变量 |
|---|---|---|
| Groq Chat Completions | `groq-chat.json` | `GROQ_API_KEY` |
| OpenAI Responses | `openai-responses.json` | `OPENAI_API_KEY` |
| DeepSeek Chat Completions | `deepseek-chat.json` | `DEEPSEEK_API_KEY` |
| 自定义兼容接口 | `custom-chat.json` | `MINT_MODEL_API_KEY` |

以 Groq 为例：

~~~bash
export GROQ_API_KEY='你的密钥'
cp share/mint/providers/groq-chat.json config.json  # 发布包
# 源码目录使用：cp configs/providers/groq-chat.json config.json
mint provider --config config.json
~~~

Windows PowerShell 设置密钥：

~~~powershell
$env:GROQ_API_KEY = '你的密钥'
~~~

`mint provider` 只解析配置并显示 adapter、能力和 token 参数，不会请求模型，也不会输出密钥。使用代理时在配置中明确写 `provider`；只有自定义兼容接口允许覆盖 `capabilities`。

想先确认真实接口能否完成工具调用，可以运行：

~~~bash
mint provider test --config config.json
~~~

这条命令会消耗额度。它只发送两轮固定内容：第一轮要求调用一个回显工具，第二轮把工具结果交还模型。它不读取工作区、不执行命令，也不输出密钥或模型原文；每轮输出上限不超过 1024 tokens，每轮最多尝试两次。

确认配置后运行：

~~~bash
mint run --root . "修复失败的测试，改完重新跑测试"
~~~

`config.json` 不要提交。根目录的 `config.example.json` 和 `config.responses.example.json` 分别是 Groq 与 OpenAI 的简写入口。

## 日常命令

~~~bash
mint init --root .        # 初始化权限和固定命令
mint run --root . "任务"  # 开始新任务
mint resume --root .      # 继续中断的任务
mint status --root .      # 查看任务状态
~~~

模型不能执行任意 shell，只能选择 `init` 登记的命令。文件访问受项目根目录和任务 policy 限制，任务记录保存在项目之外。

三个平台都默认要求操作系统隔离。Linux 会隐藏用户目录和任务配置，只把工作区映射为宿主机可写路径，并断开宿主网络。缺少 Bubblewrap 时，mint 会拒绝运行项目命令。

Windows 会在 AppContainer 中直接启动登记程序，不经过 shell。命令可访问工作区和已授权的程序，不能读受保护文件、越界写入或使用网络。工具链需要工作区外文件时，可在 policy 中增加 `command_read_paths`；格式和边界见[安全与恢复](../guides/safety-and-recovery.md#命令权限)。

`--unsafe-no-command-sandbox` 会关闭当前平台的这层保护，不是日常运行所需的参数。
