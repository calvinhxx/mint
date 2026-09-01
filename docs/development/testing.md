# 测试

[← 返回文档树](../../README.md)

## 日常检查

开发时运行：

```bash
cmake --preset vcpkg-dev
cmake --build --preset vcpkg-dev
cmake --build --preset vcpkg-dev --target format-check
ctest --preset vcpkg-dev
```

发布前运行：

```bash
bash scripts/release-check.sh
```

`release-check.sh` 会检查版本、格式、Debug、Release、ASan/UBSan、全部 CTest、离线 CLI 和本机发布包，不访问模型接口。

## 测试分组

使用 CTest 标签运行一类测试，例如：

```bash
ctest --preset vcpkg-dev -L architecture
```

| 标签 | 内容 |
|---|---|
| `unit` | 纯逻辑与小型组件 |
| `integration` | Agent、工具、命令和持久化协作 |
| `contract` | 协议、策略、恢复和稳定输出 |
| `architecture` | 依赖方向、模块目录和终端边界 |
| `localization` | 中英文资源、占位符和类型化 ID |
| `style` | 双语注释规则 |
| `provider` | 三种 adapter 与离线回归 |
| `release` | 版本和发布证据脚本 |

测试源码按 `unit`、`integration`、`contract` 和 `support` 组织。架构契约还会阻止 CLI、tools 和 infrastructure 的实现重新堆回聚合目录。

## 平台矩阵

| 系统 | x64 测试 / Release | ARM64 测试 / Release |
|---|---|---|
| Windows | `vcpkg-windows` / `vcpkg-windows-release` | `vcpkg-windows-arm64` / `vcpkg-windows-arm64-release` |
| macOS | `vcpkg-osx-x64` / `vcpkg-osx-x64-release` | `vcpkg-osx` / `vcpkg-osx-release` |
| Linux | `vcpkg-linux` / `vcpkg-linux-release` | `vcpkg-linux-arm64` / `vcpkg-linux-arm64-release` |

这些 preset 都是原生构建。ARM64 应在 ARM64 主机运行；矩阵配置以 `.github/build-matrix.json` 为准。

## 如何理解结果

- 本机通过只证明当前系统、架构和源码。
- Release 构建不包含 GoogleTest，不能代替 Debug 测试。
- 包验收只检查安装产物，不能代替 Sanitizer。
- 离线 provider 测试只证明协议解析，不证明真实服务可用。
- 跳过的平台条件测试不算对应平台已经验收。

打包、六平台工作流和真实模型证据见[发布](releasing.md)。
