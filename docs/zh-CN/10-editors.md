# 编辑器与 Agent

[English](../10-editors.md) | **简体中文**

这里的扩展都可以从源码构建，由同一个工具完成：

```bash
mcpp run -p devtools -- extension --editor vscode|zed|clion|all   # 构建（默认 all）
mcpp run -p devtools -- extension --editor vscode|zed|clion --install   # 并安装
mcpp run -p devtools -- uninstall --editor vscode|zed|clion|all   # 卸载
```

打包需要什么——Node 和 Rust——都写在 `mcpp.toml` 的 `[xlings.workspace]` 里，工具启动前 `mcpp run` 会先准备好。Gradle 和它要用的 JDK 放在 `clion` feature 后面，只有构建那个插件的人才需要下载它们：`mcpp run --features clion -p devtools -- extension --editor clion`。Zed 和 CLion 都没有装插件的命令行，所以 `--install` 在磁盘上留下的东西和它们自己的 UI 装出来的完全一样：对 Zed，它停在最后一步之前，告诉你在命令面板里执行哪个操作（推荐这样装，加 `--link` 则由它自己完成）；对 CLion，它按 *Install Plugin from Disk* 的方式解包插件。两者都会先从 PATH 启动 `mcppls`，找不到就用 `--install` 放在 `<user data>/mcppls/payload` 的那个服务端。

## VS Code

安装 **C++ Modules**（`sunrisepeak.mcpp-language-server`）。它带了服务端、一个锁定版本的 clangd 和语义工具包；不用另外装什么，也不用配置。设置和命令在 [30-settings.md](30-settings.md) 里；扩展自己的说明页是 [editors/vscode/README.md](../../editors/vscode/README.md)。

**和 mcpp 扩展一起用。** `mcpp-community.mcpp-vscode`（也就是 mcpp）与本扩展是两个独立的扩展，建议同时安装：

| | 负责 |
|---|---|
| **mcpp** | 构建、工具链、项目操作 |
| **C++ Modules**（本项目） | C++ 模块语义，驱动自己锁定版本的 clangd |

**和其他 C++ 扩展一起用。** Microsoft 的 C/C++ 扩展和官方 clangd 扩展都想当同一批文件的语言服务端。mcppls 第一次运行时会提示一次，问要不要把它们在这个工作区里的语言功能关掉；这个提示由 `mcppls.detectConflicts` 控制。之后任何时候都可以用 *Turn Off Other C++ Language Features* 在当前工作区或全局关掉它们，用 *Restore Other C++ Language Features* 恢复；C/C++ 扩展的调试器照常可用。之后又有冲突扩展启用时，会有一条提示说明。扩展没有办法禁用别的扩展：mcppls 只改它们自己的设置，而且只在你选择之后才改。

**和 Copilot 一起用时的补全。** VS Code 1.125 及以后的版本，在内联补全（VS Code 已内置 GitHub Copilot）显示灰字时不打开补全列表，其他时候也要等你停下输入才打开（`editor.quickSuggestions` 的默认值 `{"other": "offWhenInlineCompletions"}`）。对 C 和 C++ 文件，插件改设为 `{"other": "on"}`：列表随输入打开，灰字同时显示在旁边。这个值会盖过为所有语言设置的 `editor.quickSuggestions`；把它写在 `"[cpp]"` 和 `"[c]"` 下就能保留你的设置（日志里会说明一次）。

**语法高亮。** `import`、`module`、`export` 和模块名有两层上色：扩展自带的语法文件（打开即生效，边输入边上色），以及服务端的语义 token（模块名的 token 类型是 `module`，主题默认按命名空间上色，也可以在 `editor.semanticTokenColorCustomizations` 里单独指定颜色）。VS Code 自带的 C++ 语法不给 `import` 上色。

**在一个工作区里关掉它。** `mcppls.enable`（默认 `true`，按工作区生效）可以让 mcppls 保持关闭；**C++ Modules: Turn Off in This Workspace** 和 **Turn On in This Workspace** 会设置它，状态项显示 “C++ Modules: off in this workspace”。其他编辑器没有这样的设置；下面各个编辑器的小节和它们的 README 说明了在那边怎么做。

**无法恢复时。** mcppls 无法自行恢复时，会先写出一个诊断包，再弹出一条通知，提供 **Report Issue…**、**Restart Server**、**Reset This Workspace's Cache**、**Turn Off in This Workspace** 和 **Show Logs**；见 [50-troubleshooting.md](50-troubleshooting.md#mcppls-无法自行恢复时)。

**缓存，就在状态栏上。** 那一个状态栏项同时承载缓存：悬停看三段式只读卡片——项目（状态圆点、模块与单元数、真实的准备进度）、缓存（每类一条条形图的表格、对预算的大小、上次清理）、动作（清理缓存、打开日志与报告、复制 Agent 提示词）加仓库链接。点击打开缓存枢纽——分四组（概览、清理、诊断、反馈）的菜单；“明细”下钻最大模块，“打开目录…”下钻报告点名的三个目录。**清理模块缓存** 只清理没有引擎占用的东西，不重启、不重编；它右侧的眼睛按钮先做预演。反馈组复制 Agent 排障提示词——给本地 AI agent 的任务书：已核实的事实、每条带“正常长什么样”的只读检查、输出契约，以及疑似 bug 分支（你同意后由 agent 自己起草 issue、呈给你过目，任何东西发出前都先给你看；`mcppls cache --prompt agent` 在任何地方打印同一段文字）。编辑器文案跟随显示语言——英文与简体中文；服务端日志、CLI 与提示词保持英文。其它编辑器用 `workspace/executeCommand` `mcppls.sweepCache` 或 CLI；卡片与枢纽是 VS Code 专有。

## Claude Code

一个插件把 `mcppls serve` 注册为 C、C++ 以及各种 module 扩展名（`.cppm`、`.ccm`、`.cxxm`、`.c++m`、`.ixx`、`.mpp`、`.mxx`）的语言服务端。它在项目里替代官方 clangd 插件，而不是和它并行运行。参见 [editors/claude-code/mcppls-lsp/README.md](../../editors/claude-code/mcppls-lsp/README.md)。

## GitHub Copilot CLI

`editors/copilot-cli/` 里有一份 LSP 配置和一份 MCP 配置，可以直接放进它的配置文件里。

## Zed

[`editors/zed/`](../../editors/zed/README.md) 里的扩展会在 PATH 上找到 `mcppls` 并启动它。安装分两步：

1. `mcpp run -p devtools -- extension --editor zed --install` 会构建好扩展和服务端，只留一步：命令面板 → "zed: install dev extension" → `editors/zed`。加上 `--link` 可以让工具自己把这一步也做了。
2. 在 Zed 的设置里（`zed: open settings`）把 mcppls 排在前面，并关掉 clangd：

   ```json
   { "languages": { "C++": { "language_servers": ["mcppls", "!clangd"] } } }
   ```

Zed 自带 C/C++ 的 clangd，不做第 2 步它会和 mcppls 同时跑在同一个文件上：两个引擎回答同一个问题，诊断也有两份。mcppls 仍然能正常回答（CI 会用 Zed 的默认设置在真实的 Zed 里打开一个项目来检查这一点），但它会自己启动 clangd，并带上一份 clangd 本来不会有的模块数据库，所以 Zed 自带的那个没有任何帮助。参见 [editors/zed/README.md](../../editors/zed/README.md)。

要在某个项目里关掉 mcppls，就在项目的 `.zed/settings.json` 里用 `!` 指名这个服务端（并把 clangd 再列回去，因为项目的列表会替换你自己的）；见 [editors/zed/README.md](../../editors/zed/README.md#keeping-mcppls-off-for-one-project)。

## Neovim

[`editors/nvim/`](../../editors/nvim/README.md) 里的插件（Neovim 0.10 及以上）会找到 `mcppls`——PATH 上的，或者 `--install` 放在用户数据目录下的 payload——然后通过 Neovim 自带的 LSP 客户端，为 C 和 C++ buffer 启动它。把 `editors/nvim` 加进 runtimepath，调用 `require('mcppls').setup()` 即可；0.11 及以上也可以用 `vim.lsp.enable('mcppls')`。插件提供 `:McpplsStatus`、`:McpplsRestart`、`:McpplsReload`、`:McpplsResetCache`（重置这个工作区的缓存并重新准备）四个命令和一个状态栏组件。不要再为 C/C++ 另外启动 clangd：如果有第二个 C++ 服务器挂到同一个 buffer 上，插件会提示一次；设置 `disable_conflicting = true` 后插件会替你停掉它。模块关键字和模块名来自服务端的语义 token（`@lsp.type.keyword`、`@lsp.type.module`；`semantic_tokens_modules = false` 可关闭）。它没有按项目的开关；README 里给了两种在某个项目里关掉它的办法（[editors/nvim/README.md](../../editors/nvim/README.md#keeping-mcppls-off-for-one-project)）。

## CLion

[`editors/clion/`](../../editors/clion/README.md) 里的插件通过 IntelliJ 平台的 LSP API 注册 mcppls。

`mcpp run --features clion -p devtools -- extension --editor clion --install` 会构建它、把服务端放到位，并把插件解包进每一个 CLion 的插件目录；然后重启 CLion。支持 CLion 2025.2 及以上；插件基于 CLion 2026.2.3 构建，并在这个版本里测试。

一个文件只由一个引擎回答。CLion 自己建模的项目（已加载的 CMake、compilation database 或 Makefile 工作区，或者根目录有 `CMakeLists.txt`）由 CLion 自带的 C/C++ 引擎回答，mcppls 不启动；其余项目（mcpp、xmake、普通文件夹）由 mcppls 回答。Settings | Tools | mcppls 里有一个选项"Also for projects CLion models"，打开后 mcppls 也用于前一类项目；两个引擎会同时回答，插件对每个项目提示一次。要在某个项目里无论如何都关掉 mcppls，就在 Settings | Plugins 里只为这个项目禁用插件（[editors/clion/README.md](../../editors/clion/README.md#keeping-mcppls-off-for-one-project)）。

## 其他 LSP 客户端

`mcppls serve` 通过 stdio 提供 LSP。服务端需要能找到一个 payload——参见 [00-install.md](00-install.md)。

## 其他 MCP 客户端

`mcppls mcp` 通过 stdio 提供 MCP，开放语义工具。同一台机器上的多个客户端可以通过 `mcppls mcp --daemon` 共享一个工作区。参见 [40-agents.md](40-agents.md)。
