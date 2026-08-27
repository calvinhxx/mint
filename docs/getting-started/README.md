# 开始使用

[文档首页](../README.md) / 开始使用

## 准备环境

需要 CMake 3.24+、Ninja、C++20 编译器和 vcpkg。

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
~~~

生成的程序位于 `build/vcpkg-release/mint`。

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

下一步阅读：[mint 如何运行一次任务](../concepts/architecture.md)。
