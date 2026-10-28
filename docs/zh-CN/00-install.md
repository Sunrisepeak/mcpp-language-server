# 安装

[English](../00-install.md) | **简体中文**

> release 发布在 GitHub release 页面，VS Code 扩展同时也上架了 VS Code Marketplace 和 Open VSX。xlings 索引上暂时还没有。Android 上的 Termux 运行的是 `linux-arm64` 版本（见[下文](#android-上的-termux)）。

## VS Code：从 Marketplace 安装

在扩展视图里搜索 **mcppls** 或 **C++ Modules Language Server**，或者运行 `code --install-extension sunrisepeak.mcpp-language-server`（[Marketplace 页面](https://marketplace.visualstudio.com/items?itemName=sunrisepeak.mcpp-language-server））。VS Code 会自动选对应平台的版本——`linux-x64`、`linux-arm64`、`darwin-arm64` 或 `win32-x64`；其他平台暂时没有。`darwin-arm64` 需要 macOS 12 及以上；Linux 上各架构支持哪些系统，见[支持的 Linux 系统](#支持的-linux-系统)。

## Cursor、VSCodium、Windsurf 等兼容 VS Code 的编辑器：从 Open VSX 安装

这些编辑器从 [Open VSX](https://open-vsx.org/extension/sunrisepeak/mcpp-language-server) 安装扩展：在它们的扩展视图里搜索 **mcppls** 即可。这是同一个扩展，由同一个 release 构建，支持的平台也一样。release 发布时就会上架 Open VSX，VS Code Marketplace 稍后跟上。

## VS Code：从 release 安装

从 [release 页面][releases] 下载 `mcppls-<platform>.vsix`，然后安装：

- **在编辑器里**：Extensions 视图 → `…` 菜单 → *Install from VSIX…*
- **在命令行**：`code --install-extension mcppls-linux-x64.vsix`

VSIX 里带了所需的一切：服务端、一个锁定版本的 clangd，以及语义工具包。打开一个 C++ 项目，状态栏会显示它找到了什么。选和自己机器对应的文件——`linux-x64`、`linux-arm64`、`darwin-arm64` 或 `win32-x64`——因为每个文件带的是各自平台的 clangd 和工具包。每个文件里的服务端是同一个文件 `mcppls.okb`（服务端在 x86_64 与 arm64 上的映像），由 VSIX 一并携带的本平台运行器 `okrun` 启动。

这个扩展是 `sunrisepeak.mcpp-language-server`，和 **mcpp**（`mcpp-community.mcpp-vscode`）是两个不同的扩展；mcpp 负责构建、工具链和项目操作。两者都值得装，参见 [10-editors.md](10-editors.md)。

## 其他编辑器：从 release 安装

release 页面上有两类文件供 VS Code 以外的编辑器使用：

- **payload**，`payload-<platform>.tar.gz`：服务端、一个锁定版本的 clangd 和语义工具包，与 VSIX 里带的相同。服务端是 `payload/bin/mcppls.okb`，由本平台的运行器启动：`payload/bin/okrun payload/bin/mcppls.okb serve` 通过 stdio 提供 LSP，`… mcp` 提供 MCP；它在旁边找到 clangd 和工具包。把它解压到用户数据目录下的 `mcppls/payload`——Linux 上是 `~/.local/share/mcppls/payload`（设置了 `$XDG_DATA_HOME` 时以它为准），macOS 上是 `~/Library/Application Support/mcppls/payload`，Windows 上是 `%LOCALAPPDATA%\mcppls\payload`——Zed、CLion 和 Neovim 插件不需要任何配置就会启动它。
- **作为命令的服务端** `mcppls`：`mcppls-<version>-openkal.zip` 是一个包，服务 Linux、macOS 与 Windows 上的 x86_64 和 arm64。解压后把其中的 `bin/` 加进 `PATH`；它带着这些系统各自的运行器，不需要再装任何东西。也可以取本系统的单文件 `mcppls-<version>-<system>-<arch>`（`linux-x86_64`、`linux-aarch64`、`macos-aarch64`、`windows-x86_64.exe`），以 `mcppls` 之名放到 `PATH` 上：一个可执行文件，不用解压。插件先启动 `PATH` 上的 `mcppls`，再找已安装的 payload。给出 `--payload <dir>` 时它用该 payload 的 clangd 和工具包；没有时用 `PATH` 上的 clangd 和 xlings 安装的工具包。

Zed 和 CLion 的插件是同一个 release 页面上的独立压缩包；[10-editors.md](10-editors.md) 里两个都有。Neovim 插件就是本仓库的 [`editors/nvim`](../../editors/nvim/README.md)。

release 里的每个文件都列在它的 `MANIFEST.md` 里，说明是什么、怎么装；`SHA256SUMS` 覆盖所有文件。

[releases]: https://github.com/Sunrisepeak/mcpp-language-server/releases

## 支持的 Linux 系统

服务端和它的运行器不需要系统提供任何东西，在同架构的任何 Linux 上都能运行。能支持到哪些系统，取决于 payload 里带的 clangd：

| 平台 | clangd | 需要 | 能运行的系统 |
|---|---|---|---|
| `linux-x64` | mcppls-clangd 23.1.0，在 Ubuntu 20.04 上构建，libstdc++ 与 zlib 已静态链接 | glibc 2.31 | Ubuntu 20.04、Debian 11、RHEL 和 Rocky Linux 9、openEuler 22.03 LTS 及以上 |
| `linux-arm64` | 同一引擎，以相同方式为 arm64 构建 | glibc 2.31 | 同上各系统的 arm64 版本 |

更老的系统——Ubuntu 18.04、Debian 10、RHEL 和 Rocky Linux 8、Amazon Linux 2——上，自带的 clangd 无法启动。这时 mcppls 仍提供它自己的模块级功能（模块跳转、import 补全、模块诊断），状态栏会说明原因；也可以用 `mcppls.clangd` 指定一个能在该系统上运行的 clangd。Alpine 等基于 musl 的发行版，两种架构的 clangd 都无法运行。

### Android 上的 Termux

装了 Termux 的 Android 手机或平板运行的是 `linux-arm64` 版本，没有单独的安装包。Termux 的 PRoot（很多方案在它下面运行 Linux 系统）是受支持的环境：

- **服务端**就是 `linux-arm64` payload 里的那个：把 `payload-linux-arm64.tar.gz` 解压到插件查找的位置（做法同[其他编辑器：从 release 安装](#其他编辑器从-release-安装)），再由你在那里运行的编辑器（比如 Neovim）启动它；或者把 `mcppls-<version>-linux-aarch64` 以 `mcppls` 之名放到 `PATH` 上；或者在 Termux 里运行的兼容 VS Code 的编辑器中安装 `mcppls-linux-arm64.vsix`。
- **clangd** 和任何 `linux-arm64` 系统上的一样，是 glibc 程序，所以上表的要求同样适用：CI 是在 Termux 里用 `proot-distro` 装的 Debian 中运行它的。clangd 在这台机器上无法运行（glibc 低于要求）时状态是 `engine-incompatible`，被沙箱拒绝启动时是 `engine-start-failed` 并写明原因；两种情况下 mcppls 都仍提供模块级功能。
- **mcppls 为 PRoot 做了什么。** 它会检测自己运行在哪种沙箱里（`mcppls report` 里显示为 `server.sandbox`，在这里是 `proot`），并按 PRoot 的应答方式启动程序。PRoot 有两个行为曾让它无法工作：Termux 的 PRoot 拒绝“通过目录句柄启动程序”的那个调用，所以服务端启动 clangd 时会说“not supported”；PRoot 的快速路径还会在 `openat` 之后留下被改写的寄存器，读文件就报 “outside every preopened directory”（issue #32）。不需要任何配置。

## 从源码构建

服务端基于 [mcpplibs.os](https://github.com/mcpplibs/os)（其下为 [openkal](https://github.com/mcpplibs/openkal)）用 C++23 modules 写成，用 [mcpp](https://github.com/mcpp-community/mcpp) 2026.10.27.1 或更新版本构建。工作区唯一的平台 openkal-platform 0.17.3 说明了每个目标的 C 库和 C++ 运行时，因此没有成员声明它们：

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
| 服务端的 ok 映像，每种架构一份 | `mcpp build --release --target x86_64-openkal`（或 `aarch64-openkal`）；`mcpp run --release --target x86_64-openkal mcppls -- serve` 经平台的运行器在 Linux、macOS 与 Windows 上运行它 |
| 每个 payload 携带的 okb：两种架构的映像在一个文件里 | `mcpp pack --release --target x86_64-openkal --pkg-format okb` |
| 一个包服务所有系统和架构 | `mcpp pack --release --target x86_64-openkal --mode self-contained --pkg-format zip`（本系统能构建的运行器；CI 在每个系统上各打一份，再用 `mcpp pack --merge` 合并） |
| 本系统的单文件 | `mcpp pack --release --target x86_64-openkal --pkg-format single-file` |
| 构建程序作为 ok 映像运行 | `mcpp build --host-platform openkal-platform@0.17.3` |

**零配置。** 打包需要什么，都写在 `mcpp.toml` 的 `[xlings.workspace]` 里——VS Code 扩展需要 Node，Zed 需要 Rust——工具启动前 `mcpp run` 会先把这些准备好。payload 本身（clangd、语义工具包）由工具用 C++ 组装，不涉及任何解释器。所有贡献者会用到的命令都在 [93-devtools.md](93-devtools.md) 里。Gradle 和它要用的 JDK 放在 `clion` feature 后面（`mcpp run --features clion ...`），这样不构建那个插件的人就不用下载它们。没有什么需要手动安装，工具也不会背着你装东西：需要什么，只写在这一份 manifest 里。

VS Code 有装卸扩展的命令行；Zed 和 CLion 没有。对这两个编辑器，`--install` 在磁盘上留下的内容与它们自己的界面安装的结果完全相同：对 Zed，它停在最后一步之前，告诉你在命令面板里执行哪个操作（推荐这样装），加 `--link` 则由它自己完成这一步；对 CLion，它按 *Install Plugin from Disk* 的方式解包插件。两者都会先从 PATH 启动 `mcppls`，找不到就用 `--install` 放在 `<user data>/mcppls/payload` 的那个 payload 的服务端（Linux 上是 `$XDG_DATA_HOME` 或 `~/.local/share`，macOS 上是 `~/Library/Application Support`，Windows 上是 `%LOCALAPPDATA%`）；用 `uninstall` 卸载 Zed 和 CLion 中的最后一个时，这个服务端也会一起删除。

第一次运行会从 libc++ 源码编译语义工具包，这一步比较慢；之后会缓存在 `.payload-cache` 里。具体步骤是什么、生成的 payload 目录结构是什么样，见 [packaging/README.md](../../packaging/README.md)。

想让扩展直接使用某个 payload 而不用重新打包，把它放到 `editors/vscode/payload`，或者设置 `MCPPLS_PAYLOAD`。

## 装的都是什么、装在哪

| 组成 | 是什么 | 为什么锁定版本 |
|---|---|---|
| `mcppls.okb` 与 `okrun` | 服务端（两种架构的映像），以及启动它的本系统平台运行器 | — |
| clangd 23.1.0 | mcppls 用它做引擎，处理除模块级请求之外的一切 | 不同 clangd 版本的模块支持不一样；mcppls 生成的参数是按这个版本对齐的 |
| `mcppls-kit` | 一个语义工具包：libc++ 编译成 modules，附带一份模块清单 | 编译器给不了 `import std` 的机器上，靠它也能解析 |

服务端先找 `--payload` 指定的 payload，找不到再找包含自己映像或可执行文件的那个。`--clangd` 和 `--kit` 可以替换其中一部分；还缺的部分会去 payload 之外找（clangd 找 `PATH` 上的，kit 找 xlings 安装它的位置）。
