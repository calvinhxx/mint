# 安全与恢复

[← 返回 README 文档树](../../README.md)

mint 不把 shell 或文件句柄交给模型。模型只能从本次任务提供的工具中选择动作，本地代码决定是否执行。

## 文件权限

每个文件请求都会检查：

- 路径必须留在项目根目录内；
- 写入位置必须被任务 policy 允许；
- 符号链接不能绕出项目；
- 配置、密钥和任务存档等保护路径不可访问；
- 多文件修改必须先整体通过预检查。

相关实现位于 [`tool_registry.cpp`](../../src/tools/tool_registry.cpp)、[`workspace_tools.cpp`](../../src/tools/workspace_tools.cpp) 和 [`change_set.cpp`](../../src/tools/change_set.cpp)。

## 命令权限

模型不能提交一段任意 shell。项目初始化后，命令以 recipe 保存为程序、参数和工作目录，例如：

~~~text
configure -> cmake -S . -B build
build     -> cmake --build build
test      -> ctest --test-dir build --output-on-failure
~~~

运行时只能选择已经登记且被 policy 允许的 recipe。`CommandRunner` 还负责超时、输出限制和平台沙箱。

## 修改后的验证

启用验证门禁后：

1. 文件一旦修改，已有验证结果立即失效；
2. 必须在最新修改之后运行允许的验证命令；
3. 命令返回成功，任务才能正常完成；
4. 再次修改文件后，需要重新验证。

因此“模型说已经修好”不算完成证据，实际命令结果才算。

## 中断恢复

Agent 会在工具执行前后保存 checkpoint。恢复时按动作是否有副作用处理：

| 中断时的动作 | 默认处理 |
|---|---|
| 读取、搜索等只读工具 | 可以安全重试 |
| 写文件 | 不自动重试，先检查现场 |
| 构建或测试命令 | 不自动重试，避免重复副作用 |

只有用户明确允许后，才会重试结果不确定的写操作或命令。checkpoint 和事件记录保存在项目目录之外。

## 当前边界

- macOS arm64 是目前完整验证的平台；
- macOS Seatbelt 提供额外限制，但不是容器；
- Linux 还没有正式命令沙箱，Windows 还不能安全运行项目命令；
- CPU、内存、进程数和磁盘配额仍待补充。

这些限制属于当前实现范围，不应由提示词代替。
