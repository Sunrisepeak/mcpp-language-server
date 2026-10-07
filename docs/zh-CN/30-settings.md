# 设置与命令行

[English](../30-settings.md) | **简体中文**

mcppls 的每一个可配置行为都只有一处定义：`src/config/settings.cppm` 里注册表的一行。下面这张表——VS Code 设置、命令行选项，以及会影响行为的环境变量——是从这份注册表生成的
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
`initializationOptions`/`didChangeConfiguration` 里的写法）依然有效。0.0.9 改了两个名字：
一个本身是普通值的设置不能同时是其他设置的父键（否则 VS Code 会把子设置显示成 `undefined`），所以
`mcppls.engine` 现在是 `mcppls.engine.name`，`mcppls.buildDiscovery` 现在是 `mcppls.buildDiscovery.mode`。
旧名在所有编辑器里继续有效；VS Code 扩展会提示一次是否把你自己的设置改成新名，点了才改。

**`null` 等于未设置。** `initializationOptions` 里的 `null` 被跳过；`didChangeConfiguration` 里的 `null`
让该设置恢复默认值。

`initializationOptions` 和 `didChangeConfiguration` 都同时接受嵌套对象
（`{"semanticTokens": {"modules": false}}`）和点号写法的键（`{"semanticTokens.modules": false}`），
外面套不套一层 `mcppls` 都可以。

<!-- settings:begin -->
### 项目与构建工具

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.buildTool` | `offline`, `online`, `off` | `offline` | `--build-tool` | 重新加载模型 | 项目构建工具的运行方式。`offline`：不联网运行——如果构建工具因此无法在不下载东西的情况下描述构建，状态栏会说明缺什么，并提议在终端里运行它。`online`：允许联网，超时时间从一分钟延长到十分钟。`off`：从不运行构建工具；仍会探测构建系统、仍读取它已有的产物（要连探测也关掉，见 `buildDiscovery.mode`）。 |
| `mcppls.toolEnvironment` | `auto`, `editor` | `auto` | `--tool-environment` | 重启 | 构建工具在哪个环境中启动。`auto` 会在后台读取一次你登录 shell 的环境（仅限 POSIX 系统）——从桌面项或 Dock 图标启动的编辑器不带任何 shell 配置，没有这个选项，它找到的构建工具可能就不是你终端里找到的那个。在 Windows 上，编辑器的环境本就和终端一致。`editor` 始终使用编辑器进程自身的环境。 |
| `mcppls.producerTimeout` | 非负整数（秒） | `0` | `--producer-timeout` | 重新加载模型 | 构建工具描述项目最多可以花多长时间。默认 `0` 使用设计本身的限制：离线时第一次 5 分钟，之后是上一次用时的三倍，在 1 到 10 分钟之间；`buildTool` 为 `online` 时十分钟。调短可以观察限制是否生效，构建用时起伏大时可以定一个值。 |
| `mcppls.untrusted` | `true`, `false` | `false` | `--untrusted` | 重启 | 不运行任何构建工具，也不运行编译器；一个不受信任的工作区也等同于 `buildDiscovery.mode` 为 `off`。 |
| `mcppls.discoverCompilers` | `true`, `false` | `true` | `--no-discover` | 重新加载模型 | 为构建描述没有覆盖到的源码在本机查找编译器。关闭后，这类源码改用语义工具包。 |
| `mcppls.buildDiscovery.mode` | `auto`, `off` | `auto` | `--build-discovery` | 重新加载模型 | 是否探测项目的构建系统。`off`：不隐式读取或执行任何东西——只用明确配置的 `database`，否则扫描源码。`buildTool` 管的是探测到的构建工具能不能*执行*；这个开关管的是要不要去探测它。 |
| `mcppls.buildDiscovery.providers` | `mcpp`, `cmake`, `xmake`, `meson`, `compile-commands`（逗号分隔） | `mcpp`, `cmake`, `xmake`, `meson`, `compile-commands` | `--build-discovery-providers` | 重新加载模型 | `buildDiscovery.mode` 可以使用哪些构建系统提供者；从中去掉某个提供者即停用它的探测（例如只想用已有的 CMake 构建目录，不要 xmake）。 |
| `mcppls.buildDiscovery.askBeforeDownload` | `true`, `false` | `true` | — | 立即生效 | 当构建工具需要下载才能完成描述项目时，客户端可以提议去获取它。关闭后，状态栏说明需要下载，但不会再询问。 |

### 引擎

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.engine.name` | `clangd`, `none` | `clangd` | `--engine` | 重启 | 核心引擎。无论如何，mcppls 自己的模块引擎都会运行；`none` 表示只提供模块相关功能。 |
| `mcppls.engine.workers` | 字符串 | `auto` | `--engine-workers` | 重启 | clangd 同时构建的文件数（它的 `-j`），由模块预建、索引和你正在编辑的文件共用；总会留一个给你正在等的东西。`auto`：硬件线程数减一，介于二到八之间，且不超过内存 GB 数的一半。写数字则按数字。 |
| `mcppls.compiler` | 字符串 | （空） | `--compiler` | 重新加载模型 | 为模块语义使用这个编译器，而不是检测到的那个：可以是绝对路径、`PATH` 上的名字，或 `kit`（强制使用内置的语义工具包）。空表示自动检测。 |
| `mcppls.semanticKit` | `auto`, `off` | `auto` | `--semantic-kit` | 重新加载模型 | 内置的标准库工具包是否可以被使用：`auto` 在没有找到编译器时使用；`off` 从不使用（没有编译器时只剩模块相关功能）。 |
| `mcppls.requestTimeout` | 非负整数（秒） | `60` | `--request-timeout` | 重启 | 一个引擎请求最多等待多久，超时后不经该引擎就给出答复。用户在等的请求（悬停、跳转、补全等）总共最多等 30 秒，clangd 启动或准备模块期间也算在内，之后由 mcppls 自己的引擎答复。 |
| `MCPPLS_ENGINE_ARGUMENTS` | 字符串 | （空） | — | 重启 | 追加到 clangd 自身命令行末尾的额外参数，用于排查问题（例如 `-j=8 --background-index-priority=background`）。 |

### 编辑器体验

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.semanticTokens.modules` | `true`, `false` | `true` | — | 重启 | 用服务端的语义 token 给 `import`、`module`、`export` 和模块名上色。关闭后只用语法文件的颜色。 |
| `mcppls.semanticTokens.moduleType` | `true`, `false` | `false` | — | 重启 | 客户端声明自己认得自定义的 `module` 语义 token 类型和 `partition` 修饰符；除本仓库的 VS Code 扩展外都关闭，因为没有别的客户端会声明它。不是 package.json 里的设置：VS Code 扩展自己贡献了这个 token 类型，因此固定声明为开。 |
| `mcppls.completion.triggerOnSpace` | `true`, `false` | `true` | — | 重启 | 在 `import` 或 `export import` 后输入空格时立即弹出模块列表；其他位置的空格不会发给服务端。什么都不说的客户端只有在自证是 VS Code 或其分支时才会得到这个行为；其他客户端需要用 `initializationOptions.completion.triggerOnSpace: true` 主动开启。 |
| `mcppls.format.fallbackStyle` | 字符串 | `auto` | `--format-fallback-style` | 重启 | 没有项目 .clang-format 时使用的格式样式。auto 在引擎支持时为 mcpp 项目使用固定的 mcpp 样式，其他情况沿用 clangd 默认。命名样式覆盖 auto；mcpp 可让其他项目主动启用，但要求引擎支持。项目 .clang-format 始终优先。 |
| `mcppls.index.primeImplementationUnits` | `auto`, `off` | `auto` | `--prime-implementation-units` | 重启 | 在后台让 clangd 逐个构建模块的实现单元（每次少量），这样即使实现文件从没打开过，跳到定义也能到达只在实现单元里的定义。clangd 自己的后台索引看不到模块单元的导入（WA-CLANGD-008）。`off`：只有一次跳转请求所搜索的单元和你打开的文件会为此被索引。 |
| `mcppls.enable` | `true`, `false` | `true` | — | 立即生效 | 为此工作区启动 mcppls。在某个工作区的设置里设为 `false`，扩展在那里保持安装但不启动——用于它暂时还服务不了的项目，而不必卸载；状态栏上的条目可以重新打开它。 |
| `mcppls.detectConflicts` | `true`, `false` | `true` | — | 立即生效 | 在此工作区中提议关闭另一个 C++ 扩展的语言功能（只提议一次），之后又有冲突扩展启用时会提示。仅限 VS Code：其他客户端不会在多个语言服务端之间做取舍。 |

### 诊断与日志

| 设置 | 取值 | 默认值 | 命令行 | 生效方式 | 作用 |
|---|---|---|---|---|---|
| `mcppls.logLevel` | `debug`, `info`, `warning`, `error` | `info` | `--log-level` | 重启 | 服务端自身的日志级别。 |
| `mcppls.disableWorkaround` | `WA-CLANGD-<n>`（可重复） | （无） | `--disable-workaround`（可重复） | 重启 | 关掉一个针对 clangd 缺陷登记的规避措施（`WA-CLANGD-<n>`；上游缺陷登记在 issue #24），用来确认它是否还有必要；可重复。`mcppls report` 在 `engines[].details.workarounds` 下列出所有登记过的规避措施。 |
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
| `mcppls.cache.maxBytes` | ? | ? | `--cache-max-bytes` | 重启 | 单个工作区的模块缓存上限。接近或超出时会报告——状态栏与缓存菜单显示 near/over；回到预算内是自动清扫的事（删副本与死实例目录，已发布的 BMI 永不删除），所有工作区加在一起的上限由 cache.totalBytes 负责。删净可删仍超限时只报告不硬删。`unlimited` 关闭预算，`0` 表示可清理的一律不留。 |
| `mcppls.cache.totalBytes` | ? | ? | `--cache-total-bytes` | 重启 | 所有工作区模块缓存的总上限。只有没有实例打开的工作区按最久未用的先后让出副本；已发布的模块本体不会被删。 |
| `mcppls.cache.instanceGrace` | 非负整数（秒） | `86400` | `--cache-instance-grace` | 重启 | 一个不自述的实例目录（0.0.9 及更早版本的遗留）在删除前保留多久：默认 86400 秒，即 24 小时。会自述的目录按它自己的心跳判断。 |
| `mcppls.cache.showInStatusBar` | `auto`, `always`, `never` | `auto` | — | 立即生效 | 状态栏是否显示缓存大小。`auto` 只在缓存接近或超过预算时显示；`always` 与 `never` 如字面。无论如何，其余数字看悬停卡片与菜单。 |
| `mcppls.statusBar.maxLength` | ? | ? | — | 立即生效 | 状态栏项最多占多少字符（24-60；`$(图标)` 记 2）：装不下的进悬停卡片；模块状态永远优先于缓存显示。 |
| `mcppls.kit` | 路径 | （空） | `--kit` | 重启 | 语义工具包目录，覆盖 payload 自带的那一份。 |
| `MCPPLS_CACHE_DIR` | 路径 | （空） | — | 重启 | 覆盖 mcppls 原本在用户缓存目录下选定的整个缓存目录（工作区模型、工具链探测结果、日志、诊断包）。 |
<!-- settings:end -->

有一个编辑器设置由 VS Code 插件为 C 和 C++ 给出取值，而不是由服务端读取：`[cpp]` 和 `[c]` 下的 `editor.quickSuggestions` 为 `{"other": "on", "comments": "off", "strings": "off"}`，这样补全列表随输入打开。VS Code 从 1.125 起自己的默认值是 `{"other": "offWhenInlineCompletions"}`，在 Copilot 这类内联补全显示灰字时不打开列表（`WA-VSCODE-002`）。你写在 `"[cpp]"` 或 `"[c]"` 下的值优先；为所有语言设置的值不会生效，插件日志里会说明一次。

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
