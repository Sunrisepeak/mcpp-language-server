# 设置与命令行

[English](../30-settings.md) | **简体中文**

mcppls 的每一个可配置行为都只有一处定义：`src/config/settings.cppm` 里注册表的一行（0.0.6 计划 §9
T1）。下面这张表——VS Code 设置、命令行选项，以及会影响行为的环境变量——是从这份注册表生成的
（`mcppls settings --format markdown --lang zh-CN`），`tests/test_settings.cpp` 把它、英文版和
`editors/vscode/package.json` 都与注册表互相校验，三者不会走样。

**优先级。** 命令行 > `initializationOptions` > 默认值；之后的 `workspace/didChangeConfiguration`
会更新 `initializationOptions`（或更早一次 `didChangeConfiguration`）设置的值，但绝不会更新命令行设置
的值。取值超出该设置自己的取值范围（比如一个未知的枚举值）时从不生效——回落到默认值，并记为一个问题
（`mcppls report` 的 `settings.problems`；`mcppls settings` 本身不会带问题，因为它只打印注册表）。

**生效方式。** 一个已经在运行的设置改变之后如何生效：`重启` 需要重启 mcppls（下面需要重启的设置，
VS Code 扩展已经会这样做）；`重新加载模型` 只重新加载项目模型，不需要重启；`立即生效` 两者都不需要——
下次被读取时就是它生效的时候。

**改名后的设置照常能用。** 注册表给一个设置登记了旧名时，用旧名（不管是点号写法还是
`initializationOptions`/`didChangeConfiguration` 里的写法）依然有效；下面这些设置目前还没有改过名，
所以都没有登记旧名。

`initializationOptions` 和 `didChangeConfiguration` 都同时接受嵌套对象
（`{"semanticTokens": {"modules": false}}`）和点号写法的键（`{"semanticTokens.modules": false}`），
外面套不套一层 `mcppls` 都可以。

<!-- settings:begin -->
### 项目与构建工具

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.buildTool` | `offline`, `online`, `off` | `offline` | `--build-tool` | 重新加载模型 | 项目构建工具的运行方式。`offline`：不联网运行——如果构建工具因此无法在不下载东西的情况下描述构建，状态栏会说明缺什么，并提议在终端里运行它。`online`：允许联网，超时时间从一分钟延长到十分钟。`off`：从不运行构建工具；仍会探测构建系统、仍读取它已有的产物（要连探测也关掉，见 `buildDiscovery`）。 |
| `mcppls.toolEnvironment` | `auto`, `editor` | `auto` | `--tool-environment` | 重启 | 构建工具在哪个环境中启动。`auto` 会在后台读取一次你登录 shell 的环境（仅限 POSIX 系统）——从桌面项或 Dock 图标启动的编辑器不带任何 shell 配置，没有这个选项，它找到的构建工具可能就不是你终端里找到的那个。在 Windows 上，编辑器的环境本就和终端一致。`editor` 始终使用编辑器进程自身的环境。 |
| `mcppls.producerTimeout` | 非负整数（秒） | `0` | `--producer-timeout` | 重新加载模型 | 构建工具描述项目最多可以花多长时间。默认 `0` 使用设计本身的限制（离线一分钟，`buildTool` 为 `online` 时十分钟）；调短可以观察限制是否生效，构建确实慢就调长。 |
| `mcppls.untrusted` | `true`, `false` | `false` | `--untrusted` | 重启 | 不运行任何构建工具，也不运行编译器；一个不受信任的工作区也等同于 `buildDiscovery` 为 `off`。 |
| `mcppls.discoverCompilers` | `true`, `false` | `true` | `--no-discover` | 重新加载模型 | 为构建描述没有覆盖到的源码在本机查找编译器。关闭后，这类源码改用语义工具包。 |
| `mcppls.buildDiscovery` | `auto`, `off` | `auto` | `--build-discovery` | 重新加载模型 | 是否探测项目的构建系统（0.0.6 计划 §3.7 B-7）。`off`：不隐式读取或执行任何东西——只用明确配置的 `database`，否则扫描源码。`buildTool` 管的是探测到的构建工具能不能*执行*；这个开关管的是要不要去探测它。 |
| `mcppls.buildDiscovery.providers` | `mcpp`, `cmake`, `xmake`, `meson`, `compile-commands`（逗号分隔） | `mcpp`, `cmake`, `xmake`, `meson`, `compile-commands` | `--build-discovery-providers` | 重新加载模型 | `buildDiscovery` 可以使用哪些构建系统提供者；从中去掉某个提供者即停用它的探测（例如只想用已有的 CMake 构建目录，不要 xmake）。 |
| `mcppls.buildDiscovery.askBeforeDownload` | `true`, `false` | `true` | — | 立即生效 | 当构建工具需要下载才能完成描述项目时，客户端可以提议去获取它。关闭后，状态栏说明需要下载，但不会再询问。 |

### 引擎

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.engine` | `clangd`, `none` | `clangd` | `--engine` | 重启 | 核心引擎。无论如何，mcppls 自己的模块引擎都会运行；`none` 表示只提供模块相关功能。 |
| `mcppls.compiler` | 字符串 | （空） | `--compiler` | 重新加载模型 | 为模块语义使用这个编译器，而不是检测到的那个：可以是绝对路径、`PATH` 上的名字，或 `kit`（强制使用内置的语义工具包）。空表示自动检测。 |
| `mcppls.semanticKit` | `auto`, `off` | `auto` | `--semantic-kit` | 重新加载模型 | 内置的标准库工具包是否可以被使用：`auto` 在没有找到编译器时使用；`off` 从不使用（没有编译器时只剩模块相关功能）。 |
| `mcppls.requestTimeout` | 非负整数（秒） | `60` | `--request-timeout` | 重启 | 一个引擎请求最多等待多久，超时后不经该引擎就给出答复。用户在等的请求（悬停、跳转、补全等）总共最多等 30 秒，clangd 启动或准备模块期间也算在内，之后由 mcppls 自己的引擎答复。 |
| `MCPPLS_ENGINE_ARGUMENTS` | 字符串 | （空） | — | 重启 | 追加到 clangd 自身命令行末尾的额外参数，用于排查问题（例如 `-j=8 --background-index-priority=background`）。 |

### 编辑器体验

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.semanticTokens.modules` | `true`, `false` | `true` | — | 重启 | 用服务端的语义 token 给 `import`、`module`、`export` 和模块名上色。关闭后只用语法文件的颜色。 |
| `mcppls.semanticTokens.moduleType` | `true`, `false` | `false` | — | 重启 | 客户端声明自己认得自定义的 `module` 语义 token 类型和 `partition` 修饰符（设计 2026-09-25 K/§7）；除本仓库的 VS Code 扩展外都关闭，因为没有别的客户端会声明它。不是 package.json 里的设置：VS Code 扩展自己贡献了这个 token 类型，因此固定声明为开。 |
| `mcppls.completion.triggerOnSpace` | `true`, `false` | `true` | — | 重启 | 在 `import` 或 `export import` 后输入空格时立即弹出模块列表；其他位置的空格不会发给服务端。什么都不说的客户端只有在自证是 VS Code 或其分支时才会得到这个行为；其他客户端需要用 `initializationOptions.completion.triggerOnSpace: true` 主动开启。 |
| `mcppls.index.primeImplementationUnits` | `auto`, `off` | `auto` | `--prime-implementation-units` | 立即生效 | mcppls 在后台把一个模块的实现单元打开给 clangd（0.0.6 计划 §2.6、§9 T5），这样跳到定义能到达只存在于实现单元里的定义。`off`：只有客户端自己打开的文件才会被索引。 |
| `mcppls.detectConflicts` | `true`, `false` | `true` | — | 立即生效 | 在此工作区中提议关闭另一个 C++ 扩展的语言功能（只提议一次），之后又有冲突扩展启用时会提示。仅限 VS Code：其他客户端不会在多个语言服务端之间做取舍。 |

### 诊断与日志

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.logLevel` | `debug`, `info`, `warning`, `error` | `info` | `--log-level` | 重启 | 服务端自身的日志级别。 |
| `mcppls.disableWorkaround` | `WA-CLANGD-<n>`（可重复） | （无） | `--disable-workaround`（可重复） | 重启 | 关掉一个针对 clangd 缺陷登记的规避措施（`WA-CLANGD-<n>`，见 SKILL.md 的上游缺陷登记），用来确认它是否还有必要；可重复。`mcppls report` 在 `engines[].details.workarounds` 下列出所有登记过的规避措施。 |
| `mcppls.trace.server` | `off`, `messages`, `verbose` | `off` | — | 立即生效 | 把 LSP 通信记录到 C++ Modules 输出通道（Trace 级别）；`verbose` 还会打开服务端的 debug 日志（通过附加 `--log-level debug`，Debug 级别）。要看到它们，需把该输出通道的日志级别调到对应级别。 |
| `MCPPLS_LOG_LEVEL` | `debug`, `info`, `warning`, `error` | （空） | — | 重启 | 覆盖 VS Code 扩展启动服务端时使用的日志级别，优先于 `trace.server`。 |

### AI 评审

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.ai.enabled` | `true`, `false` | `false` | — | 立即生效 | 显示 AI 时代的功能：Review Changes 用 mcppls 的规则对照 `HEAD` 审查工作区的变更，并把发现连同证据以问题的形式展示。除非这里开启并且另行配置了模型来源，否则不会向任何模型发送内容。 |

### 路径

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.database` | 路径 | （空） | `--database` | 重新加载模型 | 工作区自己的 S1 构建数据库，相对于其根目录，用它代替探测。 |
| `mcppls.mcpp` | 路径 | （空） | `--mcpp` | 重新加载模型 | mcpp 项目所用的 `mcpp` 可执行文件；空表示在 `PATH` 上查找。 |
| `mcppls.payload` | 路径 | （空） | `--payload` | 重启 | 包含 clangd 和语义工具包的 payload 目录；下面的 `clangd` 和 `kit` 可以分别覆盖其中一项。 |
| `mcppls.clangd` | 路径 | （空） | `--clangd` | 重启 | clangd 可执行文件，覆盖 payload 自带的那一份。 |
| `mcppls.kit` | 路径 | （空） | `--kit` | 重启 | 语义工具包目录，覆盖 payload 自带的那一份。 |
| `MCPPLS_CACHE_DIR` | 路径 | （空） | — | 重启 | 覆盖 mcppls 原本在用户缓存目录下选定的整个缓存目录（工作区模型、工具链探测结果、日志、诊断包）。 |
<!-- settings:end -->

## 命令

| 命令 | 作用 |
|---|---|
| C++ Modules: Collect Diagnostic Report | 以 JSON 形式打开一份 bug 报告需要的一切：模型及其来源、计划、各引擎、请求统计、最近的事件、最近的外部运行记录，以及日志尾部 |
| C++ Modules: Run the Build Tool in a Terminal | 用于构建描述需要下载依赖的情况。优先使用用户自己的终端，因为手动设置的代理只在那里生效 |
| C++ Modules: Show Module Graph | 项目的模块及其相互导入关系 |
| C++ Modules: Select Context | 切换该文件所使用的那一套构建数据库 |
| C++ Modules: Restart Language Server / Show Logs | 重启语言服务端、查看日志 |
| C++ Modules: Turn Off Other C++ Language Features / Restore Other C++ Language Features | 关闭 C/C++ 扩展的 IntelliSense（调试器照常可用）和 clangd 扩展，可选当前工作区或全局；恢复时原样放回之前的设置 |
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
mcppls settings [--format markdown|json] [--lang en|zh-CN]  上面这张表，或它的机器可读形式
mcppls print-environment <marker>               在两个标记之间打印本进程的环境
mcppls version
```

上面的每一个全局选项（`mcppls --help`）都是某个注册设置的命令行写法；它的作用、默认值和生效方式见上表。

`print-environment` 是给服务端自己用的：`mcppls.toolEnvironment` 为 `auto` 时，服务端让登录 shell 运行的就是这个命令。
