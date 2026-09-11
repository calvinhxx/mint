# 变更与恢复

[导读目录](../README.md) · [核心模块](README.md)

变更账本、多文件事务、命令前后快照和崩溃恢复。

## 源码目录

| 目录 | 用途 |
|---|---|
| [src/core/tools/changes](../../../src/core/tools/changes) | 变更账本、多文件事务、命令前后快照和恢复处理。 |

## 文件说明

### include/mint/domain

| 文件 | 用途 |
|---|---|
| [change_journal.hpp](../../../include/mint/domain/change_journal.hpp) | 声明按路径合并创建、修改、删除操作的变更账本，提供受限 diff 快照及可恢复的完整状态接口。 |

### src/core/domain

| 文件 | 用途 |
|---|---|
| [change_journal.cpp](../../../src/core/domain/change_journal.cpp) | 合并同一路径的多次变更并消除净零变化，生成带上下文且受字节限制的 unified diff，序列化并校验恢复账本状态。 |

### src/core/tools/changes

| 文件 | 用途 |
|---|---|
| [change_set.cpp](../../../src/core/tools/changes/change_set.cpp) | 将多文件创建、精确替换、删除和移动预验证成变更计划，生成审批 diff，记录原始/目标状态，执行提交并在失败时尝试恢复原文件。 |
| [change_transaction.cpp](../../../src/core/tools/changes/change_transaction.cpp) | 序列化持久变更事务，在重启恢复时比对当前文件、原状态、目标状态和检查点事务 ID，决定回滚或确认提交并清理事务。 |
| [change_transaction.hpp](../../../src/core/tools/changes/change_transaction.hpp) | 定义事务用文件状态集合，以及事务文档生成、唯一 ID 和恢复原始内容的内部接口。 |
| [tool_registry_state.cpp](../../../src/core/tools/changes/tool_registry_state.cpp) | 保存和恢复工具注册表的变更账本，同时保留命令越权、文件无法审计和追踪失败等风险记录。 |
| [workspace_change_tracker.cpp](../../../src/core/tools/changes/workspace_change_tracker.cpp) | 在条目/字节限额内采集工作区文件、目录、链接与权限快照，比较命令前后变化，区分可审计文本变更与不支持的条目变化。 |
| [workspace_change_tracker.hpp](../../../src/core/tools/changes/workspace_change_tracker.hpp) | 定义工作区快照、路径过滤器、采集限额及变化报告，供命令执行前后核对文件状态。 |

[导读目录](../README.md) · [核心模块](README.md)

上一页：[工作区工具](tools.md) · 下一页：[配置与存储](config-storage.md)
