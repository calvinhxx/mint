# 样例与测试辅助

[导读目录](../README.md) · [测试](README.md)

故障修复样例、合成子进程、本机 HTTP 服务和临时工作区。

## 源码目录

| 目录 | 用途 |
|---|---|
| [tests/fixtures](../../../tests/fixtures) | 用于完整任务验收的可复制小项目。 |
| [tests/fixtures/broken_cpp_project](../../../tests/fixtures/broken_cpp_project) | 带有计算缺陷的 C++ 项目，用于检查失败基线、Agent 修复和修复后的验证。 |
| [tests/fixtures/broken_cpp_project/include](../../../tests/fixtures/broken_cpp_project/include) | 修复 fixture 的计算器接口，属于被测示例项目。 |
| [tests/fixtures/broken_cpp_project/src](../../../tests/fixtures/broken_cpp_project/src) | 修复 fixture 的故障实现；预置错误供 Agent 修复。 |
| [tests/fixtures/broken_cpp_project/tests](../../../tests/fixtures/broken_cpp_project/tests) | 规定 fixture 正确结果的断言，用来验收修复。 |
| [tests/support](../../../tests/support) | 临时工作区、可控 HTTP 服务、合成命令与桌面子进程等测试设施。 |

## 文件说明

### tests/fixtures/broken_cpp_project

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../tests/fixtures/broken_cpp_project/CMakeLists.txt) | 构建独立的 C++20 calculator 测试程序并注册 CTest，提供一个最小失败工程，可用于测试配置、编译和修复流程。 |
| [README.md](../../../tests/fixtures/broken_cpp_project/README.md) | 向受测 Agent 说明故意失败样例的验收步骤：先证明失败，只修实现并写 FIX_REPORT，再配置、构建和测试。这是 fixture 提供的任务材料。 |
| [policy.json](../../../tests/fixtures/broken_cpp_project/policy.json) | 将样例写权限限定为 calculator.cpp 和 FIX_REPORT.md，登记固定的 configure/build/test 命令及验证要求，并限制轮次、上下文和执行时长。 |

### tests/fixtures/broken_cpp_project/include

| 文件 | 用途 |
|---|---|
| [calculator.hpp](../../../tests/fixtures/broken_cpp_project/include/calculator.hpp) | 声明样例的 add 接口，作为错误实现和保持不变的验收测试共用的最小公共头文件。 |

### tests/fixtures/broken_cpp_project/src

| 文件 | 用途 |
|---|---|
| [calculator.cpp](../../../tests/fixtures/broken_cpp_project/src/calculator.cpp) | 将 add 故意实现为 left - right，使未修复工程的测试失败。这是验收夹具预置的缺陷，不属于 Mint 业务代码的待修 bug。 |

### tests/fixtures/broken_cpp_project/tests

| 文件 | 用途 |
|---|---|
| [calculator_tests.cpp](../../../tests/fixtures/broken_cpp_project/tests/calculator_tests.cpp) | 断言正数和混合符号加法的结果，错误时输出明确原因。受测 Agent 只能修实现，不能削弱这两个验收断言。 |

### tests/support

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../tests/support/CMakeLists.txt) | 构建命令辅助程序逻辑和本机脚本 HTTP 服务的测试支撑库，供不同测试目标复用；Windows 还需链接 Winsock。 |
| [command_helper.cpp](../../../tests/support/command_helper.cpp) | 为受测子进程提供确定性行为，包括输出、失败、睡眠、资源消耗，探测环境、输入和句柄，修改文件和链接，探测网络、派生子进程和模拟工作区消失，用于真实进程及沙箱故障回归。 |
| [command_helper.hpp](../../../tests/support/command_helper.hpp) | 声明命令测试辅助入口，使测试程序能在特殊参数下切换为可控子进程，无需依赖系统 shell 脚本。 |
| [desktop_process_fixture.cpp](../../../tests/support/desktop_process_fixture.cpp) | 模拟桌面 CLI 子进程的终态、非零退出、崩溃、审批输入关闭、交互积压和长 JSONL 日志，用于触发事件循环与回调析构相关问题。 |
| [scripted_http_server.cpp](../../../tests/support/scripted_http_server.cpp) | 实现只绑定本机回环的跨平台测试服务器，按顺序返回预设 HTTP/SSE 响应，按需分片并记录请求。负责 socket/thread 生命周期，也支持允许断连的故障场景。 |
| [scripted_http_server.hpp](../../../tests/support/scripted_http_server.hpp) | 声明脚本 HTTP 响应结构和回环服务器接口。测试可设置状态码、头、正文、分片大小及客户端断连，并在结束后断言实际请求。 |
| [test_executable.hpp](../../../tests/support/test_executable.hpp) | 声明查询当前测试可执行文件路径的接口，供集成测试把自身作为授权命令启动，不依赖 PATH 中不确定的程序。 |
| [test_workspace.hpp](../../../tests/support/test_workspace.hpp) | 提供唯一且自动清理的临时工作区、可靠文本读写和常用断言辅助，隔离各测试的文件并减少重复代码。 |

### tests

| 文件 | 用途 |
|---|---|
| [verify_fixture_lifecycle.cmake](../../../tests/verify_fixture_lifecycle.cmake) | 复制故障工程，实际配置、构建并确认测试按预期失败；随后只把减法改成加法，重建并确认测试通过。这项检查证明样例能够被修复，不验证模型能力。 |
| [verify_project_workflow.cmake](../../../tests/verify_project_workflow.cmake) | 用离线 demo 串联 init/status/run/resume/force-init/exec，检查状态必须位于工作区外、完成任务不可恢复、强制初始化保留历史，以及前置步骤失败后仍有可查询的任务身份。 |

[导读目录](../README.md) · [测试](README.md)

上一页：[桌面测试](gui.md)
