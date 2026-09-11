# 工程设施

[导读目录](../README.md)

仓库配置、构建、发布脚本和项目参考文档。

## 按主题阅读

| 文档 | 内容 |
|---|---|
| [构建与打包](build.md) | CMake 入口、依赖、编译选项、测试注册规则和安装包。 |
| [CI 与发布](ci-release.md) | GitHub 工作流、六平台矩阵、provider 验收和发布证据。 |
| [文档、配置模板与消息](docs-config.md) | 项目文档、模型配置示例和中英文消息目录。 |

## 本地生成的目录

`build/` 保存构建结果、CMake 缓存、下载的依赖源码、测试临时目录和本机验证记录，例如 `build/gui`、`build/release`、`build/platform-sanitize`。其中的 FluentQt / vcpkg 源码属于依赖，不列入导读。

`.git/` 保存版本数据库；`.deps/`、`vcpkg_installed/`、`.cache/`、`cmake-build-*` 等本地生成目录由忽略规则管理，也不列入索引。

## 源码目录

| 目录 | 用途 |
|---|---|
| [.vscode](../../../.vscode) | VS Code 的编译数据库接入配置。 |
| [src](../../../src) | 产品实现，分为核心运行库、CLI 和可选 GUI。 |

## 文件说明

### 仓库根目录

| 文件 | 用途 |
|---|---|
| [.clang-format](../../../.clang-format) | 统一 C++ 的缩进、换行、括号和 include 排序；format 与 format-check 使用这份规则。 |
| [.clangd](../../../.clangd) | 指定 build/dev 为 clangd 的编译数据库目录，提供头文件路径和编译选项。 |
| [.editorconfig](../../../.editorconfig) | 统一编辑器的 UTF-8、LF、文件末尾换行、缩进及尾部空白规则。 |
| [.gitignore](../../../.gitignore) | 忽略构建结果、依赖缓存、本地 preset、临时会话和用户模型配置，防止提交到仓库。 |
| [CHANGELOG.md](../../../CHANGELOG.md) | 按版本记录对外行为变化、配置迁移和构建入口调整。 |
| [LICENSE](../../../LICENSE) | 项目采用的 MIT 开源许可证，规定使用、修改和分发条件。 |
| [README.en.md](../../../README.en.md) | 英文项目入口，说明定位、CLI / GUI 构建、离线试用与文档导航。 |
| [README.md](../../../README.md) | 中文项目入口，说明本地 Agent 定位、权限边界、CLI / GUI 构建方式与文档导航。 |
| [SECURITY.md](../../../SECURITY.md) | 说明漏洞私密报告方式、支持版本，以及路径保护、固定命令和各平台沙箱的安全边界。 |

### .vscode

| 文件 | 用途 |
|---|---|
| [c_cpp_properties.json](../../../.vscode/c_cpp_properties.json) | 让 VS Code C/C++ 扩展读取 build/dev/compile_commands.json，使用 CMake 的编译配置。 |

[导读目录](../README.md)

上一页：[窗口、页面与资源](../gui/startup-resources.md) · 下一页：[构建与打包](build.md)
