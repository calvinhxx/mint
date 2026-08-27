# mint

mint 是一个轻量的通用 AI Agent 工具。它提供模型循环、工具调用、权限控制、任务恢复和结果验证，既可以直接使用 CLI，也可以作为 C++ 内核扩展。

当前自带文件读取、搜索、编辑和固定命令工具，适合本地工程与目录自动化。一次 `run` 只处理一个任务：模型决定下一步，本地工具执行操作。

## 编译

需要 CMake 3.24+、Ninja、C++20 编译器和 vcpkg。

~~~bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
~~~

## 先试一下

~~~bash
./build/vcpkg-release/mint init --root .
./build/vcpkg-release/mint run --root . --demo "总结这个目录"
~~~

`--demo` 不需要 API Key，并且不会修改文件或运行项目命令。

## 配置模型

~~~bash
cp config.example.json config.json
# 在 config.json 中填写 api_key、api_url 和 model
./build/vcpkg-release/mint run --root . "修复失败的测试，改完重新跑测试"
~~~

Responses API 可从 `config.responses.example.json` 开始配置。不要提交包含密钥的 `config.json`。

## 常用命令

~~~bash
./build/vcpkg-release/mint init --root .        # 初始化项目权限和命令
./build/vcpkg-release/mint run --root . "任务"  # 开始新任务
./build/vcpkg-release/mint resume --root .      # 继续中断的任务
./build/vcpkg-release/mint status --root .      # 查看任务状态
~~~

模型不能执行任意 shell，只能选择 `init` 登记的命令。文件访问受项目根目录和任务权限限制，任务记录保存在项目之外；需要其他能力时，可以新增工具或模型适配器，不必修改 Agent Loop。

目前完整验证的平台是 macOS arm64。Linux 和 Windows 的安全命令执行后端尚未完成。

工作原理见 [架构说明](docs/ARCHITECTURE.md)，当前完成度见 [项目进度](docs/PROGRESS.md)，其他文档见 [docs](docs/README.md)。
