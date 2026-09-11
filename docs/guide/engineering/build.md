# 构建与打包

[导读目录](../README.md) · [工程设施](README.md)

CMake 入口、依赖、编译选项、测试注册规则和安装包。

## 源码目录

| 目录 | 用途 |
|---|---|
| [cmake](../../../cmake) | 可复用的构建规则：依赖、编译选项、消息生成、测试和打包。 |
| [cmake/triplets](../../../cmake/triplets) | vcpkg 的 Windows / macOS 架构、库类型及部署目标配置。 |

## 文件说明

### 仓库根目录

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../CMakeLists.txt) | 声明版本和构建开关，查找依赖，加入源码、测试、开发工具与打包规则。GUI 可选，默认不构建。 |
| [CMakePresets.json](../../../CMakePresets.json) | 定义 dev / release 及对应的 build / test preset，使用 vcpkg 工具链。默认构建 CLI，分别用于 Debug 测试和 Release 打包。 |
| [vcpkg.json](../../../vcpkg.json) | 固定依赖版本基线与 MIT 元信息；默认依赖 curl、nlohmann-json、spdlog，tests feature 额外引入 GoogleTest。 |

### cmake

| 文件 | 用途 |
|---|---|
| [BuildMacOSDevelopmentApp.cmake](../../../cmake/BuildMacOSDevelopmentApp.cmake) | 把当前 GUI、CLI 和 plist 复制到独立暂存 App，依次完成本机 ad-hoc 签名和验签，再替换开发包。输入和输出均不允许是符号链接，也不复制外部 Qt SDK。 |
| [MintDeveloperTools.cmake](../../../cmake/MintDeveloperTools.cmake) | 查找 clang-format，为项目 C/C++ 源码创建格式化和只检查格式的目标；找不到工具时明确禁用这些开发目标。 |
| [MintDevelopmentAppInfo.plist.in](../../../cmake/MintDevelopmentAppInfo.plist.in) | 定义 Mint 本机开发 App 的可执行文件名、Bundle ID、版本、高分辨率支持和可选最低 macOS 版本，由 CMake 填充。 |
| [MintGuiDependencies.cmake](../../../cmake/MintGuiDependencies.cmake) | 配置 FluentQt 的获取方式：默认用 FetchContent 固定到 v1.8.1，也可显式选择本地源码或已安装的 SDK。检查最低版本，关闭上游示例、测试和安装，复用当前 Qt 构建。 |
| [MintLocalization.cmake](../../../cmake/MintLocalization.cmake) | 从双语消息目录生成类型化的 Message/Placeholder 标识和编译期目录片段。检查标识冲突，并分块生成内容，避免单个字符串字面量过大。 |
| [MintMacOSDevelopmentApp.cmake](../../../cmake/MintMacOSDevelopmentApp.cmake) | 注册依赖当前 GUI 和 CLI 的 mint_gui_app 目标，生成带版本的 plist，再调用安全复制与签名脚本。每次显式执行目标都会更新开发 App，避免内嵌 CLI 过期。 |
| [MintPackaging.cmake](../../../cmake/MintPackaging.cmake) | 配置 CLI 的 CPack 归档名称、平台架构和 ZIP/TGZ 格式，安装所需 DLL、配置模板、语言目录、文档和许可证，并生成 SHA-256。目前不打包 GUI。 |
| [MintPlatformDefaults.cmake](../../../cmake/MintPlatformDefaults.cmake) | 未显式指定时，将最低 macOS 部署版本设为 13.0，供主工程和自定义 vcpkg triplet 共用。 |
| [MintSanitizers.cmake](../../../cmake/MintSanitizers.cmake) | 为指定目标启用 GCC/Clang 的 ASan、UBSan、帧指针和不可恢复 UB 检查。不支持的编译器会报错，独立第三方库不会自动插桩。 |
| [MintTargetOptions.cmake](../../../cmake/MintTargetOptions.cmake) | 为自有目标设置 C++20、公共头文件、编译警告和可选的 warnings-as-errors，也设置 MSVC/Windows 编译约束、Apple 链接告警及 Sanitizer 参数。 |
| [MintTesting.cmake](../../../cmake/MintTesting.cmake) | 创建 GoogleTest 可执行文件，设置依赖、标签和中文测试环境，并注册到 CTest。测试由源码扫描登记，因此不适用当前平台的用例需要在函数体内显式跳过。 |
| [VerifyPackage.cmake](../../../cmake/VerifyPackage.cmake) | 解包并验算发布归档，检查 CLI、模板、语言、文档和第三方许可。随后执行版本、双语帮助和无密钥 provider 检查，并核对 macOS 部署版本元数据。 |

### cmake/triplets

| 文件 | 用途 |
|---|---|
| [arm64-osx.cmake](../../../cmake/triplets/arm64-osx.cmake) | 定义 macOS ARM64 的 vcpkg 静态库 triplet，与主工程共用最低部署版本和目标架构。 |
| [arm64-windows.cmake](../../../cmake/triplets/arm64-windows.cmake) | 定义 Windows ARM64 的动态库/动态 CRT triplet，并为 spdlog 启用宽字符文件路径支持。 |
| [x64-osx.cmake](../../../cmake/triplets/x64-osx.cmake) | 定义 macOS x64 的 vcpkg 静态库 triplet，与主工程共用最低部署版本和目标架构。 |
| [x64-windows.cmake](../../../cmake/triplets/x64-windows.cmake) | 定义 Windows x64 的动态库/动态 CRT triplet，并为 spdlog 启用宽字符文件路径支持。 |

### src

| 文件 | 用途 |
|---|---|
| [CMakeLists.txt](../../../src/CMakeLists.txt) | 加入 core，再根据构建开关加入 CLI 和 GUI。 |

[导读目录](../README.md) · [工程设施](README.md)

上一页：[工程设施](README.md) · 下一页：[CI 与发布](ci-release.md)
