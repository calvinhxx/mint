# mint 文档

从“开始使用”进入即可。其他分支分别解释原理、安全、开发验证和项目计划。

## 文档树

~~~text
docs/
├── README.md
├── getting-started/
│   └── README.md                 安装、配置和日常命令
├── concepts/
│   └── architecture.md           Agent Loop 与代码结构
├── guides/
│   └── safety-and-recovery.md    权限、验证和中断恢复
├── development/
│   └── testing.md                构建、测试与证据边界
└── project/
    └── roadmap.md                当前完成度与下一步
~~~

## 怎么读

| 目的 | 阅读顺序 |
|---|---|
| 第一次使用 | [开始使用](getting-started/README.md) → [安全与恢复](guides/safety-and-recovery.md) |
| 理解源码 | [架构说明](concepts/architecture.md) → [安全与恢复](guides/safety-and-recovery.md) → [测试与验收](development/testing.md) |
| 查看进度 | [项目路线图](project/roadmap.md) |
