# 命令与沙箱

[导读目录](../README.md) · [核心模块](README.md)

命令校验、审批、资源限制和各平台的进程、沙箱实现。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/infrastructure/command](../../../src/core/infrastructure/command) | 固定命令校验、审批、进程请求、资源监控和沙箱组装。 |
| [src/core/infrastructure/command/native](../../../src/core/infrastructure/command/native) | Windows / POSIX 进程和各平台沙箱实现，原生 SDK 类型仅在这一层使用。 |

## 文件说明

### include/mint/infrastructure

| 文件 | 用途 |
|---|---|
| [command_runner.hpp](../../../include/mint/infrastructure/command_runner.hpp) | 声明受控命令运行器及工具定义、执行接口。配置包括程序白名单或固定配方、审批回调、工作目录、输出与资源限额和沙箱设置。 |

### src/core/infrastructure/command

| 文件 | 用途 |
|---|---|
| [command_process.hpp](../../../src/core/infrastructure/command/command_process.hpp) | 定义跨平台的进程请求、结果和执行接口，请求包含 argv、工作目录、环境覆盖、资源限额、取消控制与原生沙箱引用。 |
| [command_resource_monitor.cpp](../../../src/core/infrastructure/command/command_resource_monitor.cpp) | 在命令运行期间遍历工作区并累计文件大小，用于判断磁盘占用是否越限，处理遍历过程中消失的条目。 |
| [command_resource_monitor.hpp](../../../src/core/infrastructure/command/command_resource_monitor.hpp) | 声明工作区磁盘限额检查，供不同平台的进程监控循环复用。 |
| [command_runner.cpp](../../../src/core/infrastructure/command/command_runner.cpp) | 解析工具 JSON，校验程序白名单、配方、参数和目录。执行前检查取消与审批，再调用进程和沙箱接口，输出统一的命令结果。 |
| [command_sandbox.cpp](../../../src/core/infrastructure/command/command_sandbox.cpp) | 实现跨平台共用的程序授权检查，拒绝通用 shell/脚本启动器，并把可执行文件查找及必需沙箱构造交给原生后端。 |
| [command_sandbox.hpp](../../../src/core/infrastructure/command/command_sandbox.hpp) | 定义沙箱启动包装信息及原生上下文引用，声明程序解析、沙箱配置构建和沙箱执行入口。 |
| [command_sandbox_backend.hpp](../../../src/core/infrastructure/command/command_sandbox_backend.hpp) | 声明原生后端必须实现的可执行文件校验、查找和沙箱配置构建协议，供公共命令层调用。 |

### src/core/infrastructure/command/native

| 文件 | 用途 |
|---|---|
| [command_appcontainer_windows.cpp](../../../src/core/infrastructure/command/native/command_appcontainer_windows.cpp) | 创建并清理 Windows AppContainer 配置，按需授予工作区/工具只读权限并保护拒绝路径，记录原 ACL 以供退出时恢复。 |
| [command_appcontainer_windows.hpp](../../../src/core/infrastructure/command/native/command_appcontainer_windows.hpp) | 声明持有 AppContainer SID、临时目录及 ACL 恢复状态的 RAII 对象，供 Windows 进程启动器附加隔离属性。 |
| [command_process_posix.cpp](../../../src/core/infrastructure/command/native/command_process_posix.cpp) | 实现 POSIX fork/exec、环境过滤、输出管道与进程树监控，结合资源限制、取消和超时终止子进程并收集退出结果。 |
| [command_process_tree_posix.cpp](../../../src/core/infrastructure/command/native/command_process_tree_posix.cpp) | 在 Linux/macOS 获取进程父子关系并维护后代集合，检测进程数量和向整棵命令进程树发信号，兼顾 PID 身份变化。 |
| [command_process_tree_posix.hpp](../../../src/core/infrastructure/command/native/command_process_tree_posix.hpp) | 声明进程树监控器，封装刷新后代数量与批量发信号，隐藏原生跟踪状态。 |
| [command_process_windows.cpp](../../../src/core/infrastructure/command/native/command_process_windows.cpp) | 实现 Windows 参数引用、环境过滤、CreateProcess 与受限句柄继承，使用 Job Object 管理资源、进程树和退出，并排空输出。 |
| [command_sandbox_linux.cpp](../../../src/core/infrastructure/command/native/command_sandbox_linux.cpp) | 生成 bubblewrap 沙箱参数，隔离网络与进程空间、限定工作区写入、遮蔽用户/临时目录及敏感路径，并补入明确的工具只读路径。 |
| [command_sandbox_macos.cpp](../../../src/core/infrastructure/command/native/command_sandbox_macos.cpp) | 生成 sandbox-exec 策略并为命令创建受控临时目录，限制读写与网络；执行结束按目录身份校验清理目标，避免误删替换后的路径。 |
| [command_sandbox_posix.cpp](../../../src/core/infrastructure/command/native/command_sandbox_posix.cpp) | 实现 POSIX 可执行文件查找与执行权限检查，规范化额外只读路径，并拒绝与工作区/拒绝路径冲突的范围。 |
| [command_sandbox_posix.hpp](../../../src/core/infrastructure/command/native/command_sandbox_posix.hpp) | 声明 POSIX 后端共用的可执行性判断与额外只读路径规范化函数。 |
| [command_sandbox_unsupported.cpp](../../../src/core/infrastructure/command/native/command_sandbox_unsupported.cpp) | 在没有对应沙箱后端的系统上返回失败；非沙箱执行仍使用公共进程接口。 |
| [command_sandbox_windows.cpp](../../../src/core/infrastructure/command/native/command_sandbox_windows.cpp) | 查找并规范化 Windows 可执行文件，拒绝批处理等间接启动格式，构建 AppContainer 上下文后交给 Windows 进程执行器。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[配置与存储](config-storage.md) · 下一页：[文件、路径与平台接口](platform.md)
