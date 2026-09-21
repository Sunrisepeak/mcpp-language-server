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

安装 **C++ Modules**（`sunrisepeak.mcppls`）。它带了服务端、一个锁定版本的 clangd 和语义工具包；不用另外装什么，也不用配置。设置和命令在 [30-settings.md](30-settings.md) 里；扩展自己的说明页是 [editors/vscode/README.md](../../editors/vscode/README.md)。

**和 mcpp 扩展一起用。** `mcpp-community.mcpp-vscode`（也就是 mcpp）与本扩展是两个独立的扩展，建议同时安装：

| | 负责 |
|---|---|
| **mcpp** | 构建、工具链、项目操作 |
| **C++ Modules**（本项目） | C++ 模块语义，驱动自己锁定版本的 clangd |

**和其他 C++ 扩展一起用。** Microsoft 的 C/C++ 扩展和官方 clangd 扩展都想当同一批文件的语言服务端。mcppls 第一次运行时会提示一次，问要不要把它们在这个工作区里的语言功能关掉；这个提示由 `mcppls.detectConflicts` 控制。

## Claude Code

一个插件把 `mcppls serve` 注册为 C、C++ 以及各种 module 扩展名（`.cppm`、`.ccm`、`.cxxm`、`.c++m`、`.ixx`、`.mpp`、`.mxx`）的语言服务端。它在项目里替代官方 clangd 插件，而不是和它并行运行。参见 [editors/claude-code/mcppls-lsp/README.md](../../editors/claude-code/mcppls-lsp/README.md)。

## GitHub Copilot CLI

`editors/copilot-cli/` 里有一份 LSP 配置和一份 MCP 配置，可以直接放进它的配置文件里。

## Zed

[`editors/zed/`](../../editors/zed/README.md) 里的扩展会在 PATH 上找到 `mcppls` 并启动它。`mcpp run -p devtools -- extension --editor zed --install` 会构建好扩展和服务端，只留一步：命令面板 → "zed: install dev extension" → `editors/zed`。加上 `--link` 可以让工具自己把这一步也做了。参见 [editors/zed/README.md](../../editors/zed/README.md)。

Zed 自带 C/C++ 的 clangd，两个都跑在同一个文件上，就成了两个引擎回答同一个问题，所以要把 mcppls 排在前面：`"languages": {"C++": {"language_servers": ["mcppls", "!clangd"]}}`。mcppls 会自己启动 clangd，并带上一份 clangd 本来不会有的模块数据库。

## CLion

[`editors/clion/`](../../editors/clion/README.md) 里的插件通过 IntelliJ 平台的 LSP API 注册 mcppls。

`mcpp run --features clion -p devtools -- extension --editor clion --install` 会构建它、把服务端放到位，并把插件解包进每一个 CLion 的插件目录；然后重启 CLion。这个插件还没有在真正跑起来的 CLion 里验证过，详见它的 README。

## 其他 LSP 客户端

`mcppls serve` 通过 stdio 提供 LSP。服务端需要能找到一个 payload——参见 [00-install.md](00-install.md)。

## 其他 MCP 客户端

`mcppls mcp` 通过 stdio 提供 MCP，开放语义工具。同一台机器上的多个客户端可以通过 `mcppls mcp --daemon` 共享一个工作区。参见 [40-agents.md](40-agents.md)。
