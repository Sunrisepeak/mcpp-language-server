# 设置与命令行

[English](../30-settings.md) | **简体中文**

## VS Code 设置

| 设置 | 取值 | 作用 |
|---|---|---|
| `mcppls.buildTool` | `offline`（默认）, `online`, `off` | 项目构建工具的运行方式。`offline`：不联网运行——如果构建工具因此无法在不下载东西的情况下描述构建，状态栏会说明缺什么，并提议在你的终端里运行它。`online`：允许联网，超时时间从一分钟延长到十分钟。`off`：从不运行构建工具，使用缓存的描述或扫描到的源码 |
| `mcppls.toolEnvironment` | `auto`（默认）, `editor` | 构建工具在哪个环境中启动。`auto` 会在后台读取一次你登录 shell 的环境（仅限 POSIX 系统）——从桌面项或 Dock 图标启动的编辑器不带任何 shell 配置，没有这个选项，它找到的构建工具可能就不是你终端里找到的那个。在 Windows 上，编辑器的环境本就和终端一致。`editor` 始终使用编辑器进程自身的环境 |
| `mcppls.compiler` | 编译器驱动的路径，或 `kit` | 为模块语义使用这个编译器，而不是检测到的那个。`kit` 强制使用内置的语义工具包 |
| `mcppls.semanticKit` | `auto`（默认）, `off` | 内置工具包是否可以被使用 |
| `mcppls.engine` | `clangd`（默认）, `none` | 核心引擎。无论如何，mcppls 自己的模块引擎都会运行；`none` 表示只提供模块相关功能 |
| `mcppls.ai.enabled` | `false`（默认） | 是否启用变更审查里依赖模型的那部分。关闭时服务端不发起任何模型调用 |
| `mcppls.detectConflicts` | `true`（默认） | 在此工作区中提议关闭另一个 C++ 扩展的语言功能（只提议一次） |
| `mcppls.trace.server` | `off`（默认）, `messages`, `verbose` | 把 LSP 通信记录到 C++ Modules 输出通道（Trace 级别）；`verbose` 还会打开服务端的 debug 日志（Debug 级别）。要看到它们，需把该输出通道的日志级别调到对应级别 |

## 命令

| 命令 | 作用 |
|---|---|
| C++ Modules: Collect Diagnostic Report | 以 JSON 形式打开一份 bug 报告需要的一切：模型及其来源、计划、各引擎、请求统计、最近的事件、最近的外部运行记录，以及日志尾部 |
| C++ Modules: Run the Build Tool in a Terminal | 用于构建描述需要下载依赖的情况。优先使用用户自己的终端，因为手动设置的代理只在那里生效 |
| C++ Modules: Show Module Graph | 项目的模块及其相互导入关系 |
| C++ Modules: Select Context | 切换该文件所使用的那一套构建数据库 |
| C++ Modules: Restart Language Server / Show Logs | 重启语言服务端、查看日志 |
| C++ Modules: Review Changes / Clear Review | 对工作区变更做审查，以诊断形式发布（依赖模型的规则需要开启 `mcppls.ai.enabled`） |

## 命令行

```
mcppls [serve] [--payload DIR] [--clangd PATH] [--kit DIR] [--engine clangd|none] [--untrusted]
mcppls mcp [--root DIR] [--daemon]              通过 stdio 提供 MCP，即 Agent 用的工具
mcppls query symbol|refs|calls|outline|module|context ...
mcppls diagnostics <file>... | verify [--changed] | impact | review [--base REV] [--format sarif]
mcppls daemon run|start|status|stop             共享工作区的守护进程
mcppls check <file>                             模型、语义配置、模块诊断，然后运行 clangd --check
mcppls report [--root DIR] [--settle SECONDS]   bug 报告需要的全部信息，JSON 格式
mcppls model [--root DIR] [--export s1|compile-commands|engine]
mcppls print-environment <marker>               在两个标记之间打印本进程的环境
mcppls version
```

对每个子命令都生效的选项：

| 选项 | 默认值 | 作用 |
|---|---|---|
| `--build-tool offline\|online\|off` | `offline` | `mcppls.buildTool` 的命令行写法 |
| `--tool-environment auto\|editor` | `auto` | `mcppls.toolEnvironment` 的命令行写法 |
| `--producer-timeout SECONDS` | 60，联网时 600 | 构建工具描述项目最多可以花多长时间。构建确实慢就调长；想观察超时限制是否生效就调短 |
| `--request-timeout SECONDS` | 60 | 一个引擎请求最多等待多久，超时后不经该引擎就给出答复 |
| `--untrusted` | — | 不运行任何构建工具，也不运行编译器 |
| `--no-discover` | — | 不查找编译器；零散源码使用语义工具包 |
| `--log-level debug\|info\|warning\|error` | `info` | |

`print-environment` 是给服务端自己用的：`mcppls.toolEnvironment` 为 `auto` 时，服务端让登录 shell 运行的就是这个命令。
