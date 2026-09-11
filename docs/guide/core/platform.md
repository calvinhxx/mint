# 文件、路径与平台接口

[导读目录](../README.md) · [核心模块](README.md)

原生文件操作、路径转换、系统查询和私有目录权限。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/infrastructure/filesystem](../../../src/core/infrastructure/filesystem) | 输出路径校验与私有目录 / 文件的权限策略。 |
| [src/core/infrastructure/filesystem/native](../../../src/core/infrastructure/filesystem/native) | POSIX 所有者 / 权限和 Windows ACL 的私有路径后端。 |
| [src/core/platform](../../../src/core/platform) | 底层文件、路径和系统接口，在这里处理平台差异；业务权限由上层决定。 |

## 文件说明

### include/mint/platform

| 文件 | 用途 |
|---|---|
| [file.hpp](../../../include/mint/platform/file.hpp) | 声明不暴露平台句柄的底层文件操作：安全打开普通文件、锁竞争、直接写入、同步替换、独占创建及文件身份比较；私有权限策略由上层处理。 |
| [path.hpp](../../../include/mint/platform/path.hpp) | 声明 UTF-8 与原生路径互转、通用分隔符输出和按宿主平台惯例比较路径组件的统一入口。 |
| [system.hpp](../../../include/mint/platform/system.hpp) | 声明 UTC 时间转换、进程 ID、界面语言及用户状态目录基址查询，隔离系统信息获取差异。 |

### include/mint/runtime

| 文件 | 用途 |
|---|---|
| [path.hpp](../../../include/mint/runtime/path.hpp) | 声明工作区路径包含检查，只比较路径组件，不解析符号链接；访问文件时还需校验文件身份。 |

### src/core/infrastructure/filesystem/native

| 文件 | 用途 |
|---|---|
| [private_path_native.hpp](../../../src/core/infrastructure/filesystem/native/private_path_native.hpp) | 声明私有文件和目录的权限接口及失败原因，公共策略层无需使用 POSIX 权限位或 Windows ACL 类型。 |
| [private_path_posix.cpp](../../../src/core/infrastructure/filesystem/native/private_path_posix.cpp) | 用 mkdir、lstat、fchmod 和 fstat 创建或验证 POSIX 私有目录/文件，检查当前用户所有权和组/其他用户访问权限。 |
| [private_path_windows.cpp](../../../src/core/infrastructure/filesystem/native/private_path_windows.cpp) | 构造并核对仅当前用户与 SYSTEM 可访问的受保护 ACL，创建私有目录并通过独立安全句柄调整/验证已打开文件权限。 |

### src/core/infrastructure/filesystem

| 文件 | 用途 |
|---|---|
| [output_path.cpp](../../../src/core/infrastructure/filesystem/output_path.cpp) | 检查输出文件名、末尾符号链接和硬链接策略。只规范化父目录，保留目标文件名，避免跟随不安全的输出条目。 |
| [output_path.hpp](../../../src/core/infrastructure/filesystem/output_path.hpp) | 声明输出路径校验接口和允许/拒绝硬链接的策略枚举，供会话、事件、日志与设置存储共用。 |
| [private_path.cpp](../../../src/core/infrastructure/filesystem/private_path.cpp) | 检查私有存储路径，拒绝符号链接，创建或验证目录，并调用原生权限实现。把各类失败转换成本地化错误。 |
| [private_path.hpp](../../../src/core/infrastructure/filesystem/private_path.hpp) | 声明私有目录创建/校验、已有目录迁移策略和打开文件权限检查接口，供各持久化适配器使用。 |

### src/core/platform

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/core/platform/CMakeLists.txt) | 构建独立 mint::platform，按 Windows/POSIX 选择文件实现，并纳入路径编码和系统查询实现。 |
| [file_posix.cpp](../../../src/core/platform/file_posix.cpp) | 以 open/fstat/flock/fsync 等实现普通文件、单硬链和末尾链接检查；用非阻塞打开避免 FIFO 卡死，并提供同步替换、独占创建及 inode 身份比较。 |
| [file_windows.cpp](../../../src/core/platform/file_windows.cpp) | 以 CreateFileW、LockFileEx、WriteFile 和 MoveFileExW 实现文件入口，管理 HANDLE 到 CRT 流的所有权转移，并按原生文件 ID 比较目录项。 |
| [path.cpp](../../../src/core/platform/path.cpp) | 实现 UTF-8 路径输入输出；路径组件比较在 Windows 使用原生忽略大小写规则，其他系统保持组件原值比较。 |
| [system.cpp](../../../src/core/platform/system.cpp) | 集中实现各系统的 UTC 转换、PID 和用户状态目录基址；另在 Windows 查询用户界面语言，其他系统的语言选择由本地化环境规则处理。 |

### src/core/runtime

| 文件 | 用途 |
|---|---|
| [path.cpp](../../../src/core/runtime/path.cpp) | 逐组件判断绝对候选路径是否位于绝对根路径下，拒绝空路径和相对路径；本身不做文件系统规范化。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[命令与沙箱](commands.md) · 下一页：[日志与本地化](logging-localization.md)
