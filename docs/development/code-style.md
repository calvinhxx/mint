# 代码风格

[← 返回 README 文档树](../../README.md)

本页是仓库长期使用的代码风格规则。代码审查和自动检查都以这里为准。

## 模块

- 顶层目录按依赖方向划分，复杂模块内部再按功能划分；不要为单个文件增加目录。
- 公共接口放在 `include/mint`，实现和私有头文件放在对应的 `src` 模块。
- application 依赖 ports，不直接依赖具体 provider、文件系统或进程实现。
- 协议名、事件名等稳定标识集中定义，业务代码不重复写字符串常量。
- 只有独立职责或独立依赖边界才值得增加 CMake target；不要创建整层 facade。

目录和依赖规则由 `architecture_module_layout`、`architecture_application_boundary` 等契约测试检查。

## 注释

代码应尽量通过命名和结构表达意图。注释只解释原因、边界、兼容性或不明显的约束，不复述代码。

自然语言注释必须同时提供英文和简体中文，英文在前，两种语言表达同一含义：

~~~cpp
// EN: Zero disables the task-level cumulative token budget.
// ZH-CN: 值为零时，不启用任务级累计 token 预算。
~~~

Doxygen 注释使用相同标记：

~~~cpp
/**
 * EN: Returns true when candidate is root or one of its descendants.
 * ZH-CN: 当 candidate 是 root 本身或其后代时返回 true。
 */
~~~

规则覆盖 C/C++、测试、CMake、Python、Shell 和工作流配置。以下机器可读标记不需要翻译：

- `// namespace ...`；
- shebang、SPDX 标识；
- clang-format、NOLINT、IWYU、ShellCheck 等工具指令；
- 协议字段、类型名和代码标识符。

不要为了补齐双语而增加无价值注释。没有必要说明的代码保持无注释即可。

## 自动检查

`comment_language_contract` 会检查每个自然语言注释块是否同时包含 `EN:` 和 `ZH-CN:`：

~~~bash
python3 tests/scripts/comment_language_tests.py .
ctest --preset vcpkg-dev -R comment_language_contract
~~~

提交前还应运行格式检查和相关测试，完整命令见[构建与测试](testing.md)。
