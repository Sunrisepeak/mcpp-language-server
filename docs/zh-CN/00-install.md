# 安装

[English](../00-install.md) | **简体中文**

> release 发布在 GitHub release 页面，VS Code 扩展同时也上架了 VS Code Marketplace。Open VSX 和 xlings 索引上暂时都还没有。

## VS Code：从 Marketplace 安装

在扩展视图里搜索 **C++ Modules Language Server**，或者运行 `code --install-extension sunrisepeak.mcpp-language-server`（[Marketplace 页面](https://marketplace.visualstudio.com/items?itemName=sunrisepeak.mcpp-language-server））。VS Code 会自动选对应平台的版本——`linux-x64`、`linux-arm64`、`darwin-arm64` 或 `win32-x64`；其他平台暂时没有。Linux 上各架构支持哪些系统，见[支持的 Linux 系统](#支持的-linux-系统)。

## VS Code：从 release 安装

从 [release 页面][releases] 下载 `mcppls-<platform>.vsix`，然后安装：

- **在编辑器里**：Extensions 视图 → `…` 菜单 → *Install from VSIX…*
- **在命令行**：`code --install-extension mcppls-linux-x64.vsix`

VSIX 里带了所需的一切：服务端、一个锁定版本的 clangd，以及语义工具包。打开一个 C++ 项目，状态栏会显示它找到了什么。选和自己机器对应的文件——`linux-x64`、`linux-arm64`、`darwin-arm64` 或 `win32-x64`——因为每个文件带的是各自平台的 payload。

这个扩展是 `sunrisepeak.mcpp-language-server`，和 **mcpp**（`mcpp-community.mcpp-vscode`）是两个不同的扩展；mcpp 负责构建、工具链和项目操作。两者都值得装，参见 [10-editors.md](10-editors.md)。

## 其他编辑器：从 release 安装

下载 `payload-<platform>.tar.gz`，解压后把其中的 `payload/bin/` 加进 `PATH`。这样就有了 `mcppls`，它通过 stdio 提供 LSP（`mcppls serve`）和 MCP（`mcppls mcp`）。Zed 和 CLion 的插件是同一个 release 页面上的独立压缩包；[10-editors.md](10-editors.md) 里两个都有。Neovim 插件就是本仓库的 [`editors/nvim`](../../editors/nvim/README.md)。

release 里的每个文件都列在它的 `MANIFEST.md` 里，说明是什么、怎么装；`SHA256SUMS` 覆盖所有文件。

[releases]: https://github.com/Sunrisepeak/mcpp-language-server/releases

## 支持的 Linux 系统

服务端是静态链接的可执行文件，在同架构的任何 Linux 上都能运行。能支持到哪些系统，取决于 payload 里带的 clangd：

| 平台 | clangd | 需要 | 能运行的系统 |
|---|---|---|---|
| `linux-x64` | clangd/clangd 发布的 23.1.0，libstdc++ 已静态链接 | glibc 2.18 | 目前所有基于 glibc 的发行版 |
| `linux-arm64` | LLVM 官方发布的 23.1.0 Linux arm64 版本（clangd/clangd 不发布 arm64 Linux 版本） | glibc 2.34、GCC 12 的 libstdc++（`GLIBCXX_3.4.30`）、zlib | Ubuntu 22.04 及以上、Debian 12 及以上、openEuler 24.03 LTS 及以上（已测试）；Fedora 36 及以上（按其软件包版本推断） |

比这更老的 arm64 系统——Ubuntu 20.04、Debian 11、RHEL 和 Rocky Linux 8 与 9、Amazon Linux 2023、openEuler 22.03——上，自带的 clangd 无法启动。这时 mcppls 仍提供它自己的模块级功能（模块跳转、import 补全、模块诊断），状态栏会说明原因。Alpine 等基于 musl 的发行版，两种架构的 clangd 都无法运行。

## 从源码构建

服务端基于 [openkal](https://github.com/mcpplibs/openkal) 用 C++23 modules 写成，用 [mcpp](https://github.com/mcpp-community/mcpp) 构建：

```bash
mcpp build                                        # 服务端
mcpp run -p devtools -- extension --install       # payload、扩展，装进 VS Code
```

第二条命令会组装 payload（服务端、一个精简版 clangd 和语义工具包）、构建扩展并安装，过程中会报出每一步的名字和耗时。缺少依赖时，它会说明缺什么、如何获取。

| 场景 | 命令 |
|---|---|
| release 构建 | `mcpp run --release -p devtools -- extension --editor vscode --install` |
| Zed | `mcpp run -p devtools -- extension --editor zed --install`，然后在 Zed 的命令面板执行 *zed: install dev extension*（或者加上 `--link`） |
| CLion | `mcpp run --features clion -p devtools -- extension --editor clion --install`，然后重启 CLion |
| 三者都打包但不安装 | `mcpp run -p devtools -- extension` |
| 移除已安装的内容 | `mcpp run -p devtools -- uninstall --editor vscode\|zed\|clion\|all` |
| 只要 payload，给其他编辑器用 | `mcpp run -p devtools -- payload` |
| 复用已经构建好的 clangd 或 kit | `... -- payload --clangd DIR --kit DIR` |
| 其他平台的服务端 | `mcpp build --target aarch64-macos` / `--target x86_64-windows-gnu` / `--target aarch64-linux-musl` |

**零配置。** 打包需要什么，都写在 `mcpp.toml` 的 `[xlings.workspace]` 里——VS Code 扩展需要 Node，Zed 需要 Rust——工具启动前 `mcpp run` 会先把这些准备好。payload 本身（clangd、语义工具包）由工具用 C++ 组装，不涉及任何解释器。所有贡献者会用到的命令都在 [93-devtools.md](93-devtools.md) 里。Gradle 和它要用的 JDK 放在 `clion` feature 后面（`mcpp run --features clion ...`），这样不构建那个插件的人就不用下载它们。没有什么需要手动安装，工具也不会背着你装东西：需要什么，只写在这一份 manifest 里。

VS Code 有装卸扩展的命令行；Zed 和 CLion 没有。对这两个编辑器，`--install` 在磁盘上留下的内容与它们自己的界面安装的结果完全相同：对 Zed，它停在最后一步之前，告诉你在命令面板里执行哪个操作（推荐这样装），加 `--link` 则由它自己完成这一步；对 CLion，它按 *Install Plugin from Disk* 的方式解包插件。两者都会先从 PATH 启动 `mcppls`，找不到就用 `--install` 放在 `<user data>/mcppls/payload` 的那个服务端（Linux 上是 `$XDG_DATA_HOME` 或 `~/.local/share`，macOS 上是 `~/Library/Application Support`，Windows 上是 `%LOCALAPPDATA%`）；用 `uninstall` 卸载 Zed 和 CLion 中的最后一个时，这个服务端也会一起删除。

第一次运行会从 libc++ 源码编译语义工具包，这一步比较慢；之后会缓存在 `.payload-cache` 里。具体步骤是什么、生成的 payload 目录结构是什么样，见 [packaging/README.md](../../packaging/README.md)。

想让扩展直接使用某个 payload 而不用重新打包，把它放到 `editors/vscode/payload`，或者设置 `MCPPLS_PAYLOAD`。

## 装的都是什么、装在哪

| 组成 | 是什么 | 为什么锁定版本 |
|---|---|---|
| `mcppls` | 服务端 | — |
| clangd 23.1.0 | mcppls 用它做引擎，处理除模块级请求之外的一切 | 不同 clangd 版本的模块支持不一样；mcppls 生成的参数是按这个版本对齐的 |
| `mcppls-kit` | 一个语义工具包：libc++ 编译成 modules，附带一份模块清单 | 编译器给不了 `import std` 的机器上，靠它也能解析 |

服务端先找 `--payload` 指定的 payload，找不到再找包含自己可执行文件的那个。`--clangd` 和 `--kit` 可以替换其中一部分；还缺的部分会去 payload 之外找（clangd 找 `PATH` 上的，kit 找 xlings 安装它的位置）。
