# 开始使用 mint

[← 返回 README 文档树](../../README.md)

## 准备环境

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

生成的程序位于 `build/vcpkg-release/mint`，Windows 下为 `mint.exe`。需要指定系统和架构时，使用[构建矩阵](../development/testing.md#平台构建矩阵)中的 preset。

## 先跑离线演示

~~~bash
./build/vcpkg-release/mint init --root .
./build/vcpkg-release/mint run --root . --demo "总结这个目录"
~~~

`--demo` 使用确定性本地模型，不需要 API Key，也不会写文件或运行项目命令。

## 配置真实模型

Chat Completions：

~~~bash
cp config.example.json config.json
~~~

Responses API：

~~~bash
cp config.responses.example.json config.json
~~~

在 `config.json` 中填写 `api_key`、`api_url` 和 `model`，不要提交这个文件。然后运行：

~~~bash
./build/vcpkg-release/mint run --root . "修复失败的测试，改完重新跑测试"
~~~

## 日常命令

~~~bash
./build/vcpkg-release/mint init --root .        # 初始化权限和固定命令
./build/vcpkg-release/mint run --root . "任务"  # 开始新任务
./build/vcpkg-release/mint resume --root .      # 继续中断的任务
./build/vcpkg-release/mint status --root .      # 查看任务状态
~~~

模型不能执行任意 shell，只能选择 `init` 登记的命令。文件访问受项目根目录和任务 policy 限制，任务记录保存在项目之外。

macOS 和 Linux 默认要求操作系统沙箱。Linux 会隐藏用户目录和任务配置，只把当前工作区映射为宿主机可写路径，并断开宿主网络。缺少 Bubblewrap 时，mint 会拒绝运行项目命令。

Windows 已能无 shell 地运行登记过的程序，并限制继承句柄、环境、进程树、CPU 和内存；但它还不能隔离文件和网络。默认仍会拒绝项目命令。只有明确接受该风险时才使用：

~~~powershell
.\build\vcpkg-release\mint.exe run --root . --unsafe-no-command-sandbox "运行测试"
~~~
