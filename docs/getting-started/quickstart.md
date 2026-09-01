# 开始使用

[← 返回文档树](../../README.md)

## 构建

需要：

- CMake 3.24+
- Ninja
- C++20 编译器
- vcpkg

macOS 或 Linux：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
```

Windows PowerShell：

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
```

程序位于 `build/vcpkg-release/mint`，Windows 下为 `mint.exe`。Linux 上运行项目命令还需要 Bubblewrap，例如 Ubuntu / Debian 使用 `sudo apt-get install bubblewrap`。

## 离线试用

```bash
./build/vcpkg-release/mint init --root .
./build/vcpkg-release/mint run --root . --demo "总结这个目录"
```

`--demo` 使用本地确定性模型，不需要密钥，也不会写文件或运行项目命令。

## 配置模型

仓库的 `configs/providers` 目录包含无密钥模板。以 DeepSeek 为例：

```bash
export DEEPSEEK_API_KEY='your-api-key'
cp configs/providers/deepseek-chat.json config.json

./build/vcpkg-release/mint provider --config config.json
./build/vcpkg-release/mint run --root . "总结当前项目"
```

`mint provider` 只检查配置，不发送请求。密钥应放在模板指定的环境变量中，不要写进 `config.json`。

供应商列表、兼容协议、endpoint 规则和 Token 预算见[模型配置](../reference/model-providers.md)。真实兼容性测试会消耗额度，运行前先阅读其中的说明。

## 常用命令

```bash
mint init --root .        # 创建项目策略
mint run --root . "任务"  # 开始任务
mint resume --root .      # 继续中断的任务
mint status --root .      # 查看任务状态
mint provider             # 离线检查模型配置
```

使用 `mint exec --help` 查看面向自动化的高级入口。

## 语言和日志

界面默认跟随系统语言，也可以明确指定：

```bash
mint --lang en --help
mint --lang zh-CN --help
```

诊断日志默认写入系统状态目录。需要更多字段时使用 `--log-file-level debug`，完全关闭落盘使用 `--log-file-level off`。日志内容和任务数据的区别见[安全与恢复](../guides/safety-and-recovery.md#本地数据)。
