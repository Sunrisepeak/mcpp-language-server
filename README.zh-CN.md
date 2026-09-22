# mcppls - mcpp language server

> C++ 模块的语言服务器：不管用哪个编译器，模块语义都是同一套，编辑器、Agent 和 CI 共用

[English](README.md) | **简体中文**

[文档](docs/zh-CN/README.md) · [安装](docs/zh-CN/00-install.md) · [设置](docs/zh-CN/30-settings.md) ·
[Agent 与 CI](docs/zh-CN/40-agents.md) · [规范](docs/specs/README.md) ·
[Releases](https://github.com/Sunrisepeak/mcpp-language-server/releases)

C++20 把模块写进了标准，但剩下的事各家工具链各做各的：BMI 格式互不兼容，依赖扫描的方式不同，
`import std` 从哪来也不一样。结果是同一份能编译通过的模块代码，换一个编译器，编辑器便几乎给不出任何信息。

mcppls 把各种构建方式——[mcpp](https://github.com/mcpp-community/mcpp)、CMake、单独一份
`compile_commands.json`，甚至什么都没有——统一整理成一份模块描述，交给内置的固定版本 clangd，
再补上 clangd 自己处理不了的模块相关请求。Agent 和 CI 通过 MCP 和命令行获得同样的结果。

## 功能

- **各编译器通用**：GCC、Clang、MinGW、clang-cl、MSVC 都支持，并使用各自工具链的 `std`
- **无需配置**：自动识别构建方式、探测工具链，状态栏里直接显示当前用的是什么
- **补齐 clangd 缺的能力**：按模块名跳转、`import` 补全、模块依赖图、模块相关的诊断
- **不受外部程序拖累**：构建工具默认离线运行，在独立的进程组中执行，超时后整组结束
- **故障不扩散**：某个模块出错时由占位单元代替，不影响整个工程
- **给 Agent 和 CI 用**：跨模块查引用和调用方、改完代码后校验、有依据的代码审查
- **开箱即用**：服务端、固定版本的 clangd 和语义工具包打成一个 payload，机器上没装编译器也能 `import std`

## 安装

### 让 Agent 安装

把下面这段提示词发给 Agent，由它选择合适的方式完成安装：

```
Read .agents/skills/mcppls-usage/SKILL.md and docs/00-install.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then install mcppls for the editor I use — from the latest release, or built from source
if there is none for my platform — and check that it works on my C++ modules project.
```

### 从 release 安装

在 [release 页面](https://github.com/Sunrisepeak/mcpp-language-server/releases) 下载对应的文件：

| 编辑器 | 文件 | 安装方式 |
|---|---|---|
| VS Code | [Marketplace](https://marketplace.visualstudio.com/items?itemName=sunrisepeak.mcppls)，或 `mcppls-<平台>.vsix` | 搜索 *C++ Modules (mcppls)*，或 `code --install-extension mcppls-<平台>.vsix` |
| Zed、CLion | `mcppls-zed-<版本>.tar.gz`、`mcppls-clion-<版本>.zip` | 见[安装文档](docs/zh-CN/00-install.md) |
| 其他支持 LSP 的编辑器 | `payload-<平台>.tar.gz` | 解压后把 `payload/bin/` 加到 `PATH`，用 `mcppls serve` 启动 |

### 从源码构建

**1. 用 [xlings](https://github.com/openxlings/xlings) 安装 mcpp**（每台机器只需一次）：

```bash
curl -fsSL https://d2learn.org/xlings-install.sh | bash         # Linux / macOS
irm https://d2learn.org/xlings-install.ps1.txt | iex            # Windows（PowerShell）

xlings install mcpp -y -g                                       # 装完后重新打开终端
```

**2. 构建并安装到编辑器：**

```bash
mcpp build
mcpp run -p devtools -- extension --editor vscode --install
```

| 编辑器 | 安装 | 卸载 |
|---|---|---|
| VS Code | `extension --editor vscode --install` | `uninstall --editor vscode` |
| Zed | `extension --editor zed --install`，然后在 Zed 命令面板里运行 *zed: install dev extension*（加 `--link` 可以省掉这一步） | `uninstall --editor zed` |
| CLion | `mcpp run --features clion -p devtools -- extension --editor clion --install`，完成后重启 CLion | `uninstall --editor clion` |

Agent 和其他客户端怎么接入，见[编辑器与 Agent](docs/zh-CN/10-editors.md)。

## 支持程度

工程能提供的信息越多，mcppls 能做的就越多：

| | 工程类型 | 模块信息从哪来 | 能得到什么 |
|---|---|---|---|
| **L1** | **mcpp** | 离线调用 `mcpp emit build-database` | 完整信息：每个编译单元的角色、编译参数，以及工具链自带的 `std` |
| **L2** | **CMake**（`FILE_SET CXX_MODULES`） | 构建目录里的数据库；没有的话，mcppls 在单独的目录里自己配置一份 | CMake 生成的信息，`@modmap` 文件已展开 |
| **L3** | 只有 **`compile_commands.json`** | 这份数据库，再加上源码扫描 | 每个文件的编译参数，模块关系靠扫描补齐 |
| **L4** | **只有源码**，或者没有编译器 | 源码扫描加内置的语义工具包 | 模块照样能解析，`import std` 可用，诊断基于 libc++ |

信息不够时逐级降级，当前在哪一级会直接显示出来。不受信任的工作区一律按 L4 处理，不会运行任何构建工具或编译器。

## 文档

- [docs/zh-CN/](docs/zh-CN/README.md)：安装、编辑器、工程类型、设置、Agent 与 CI、常见问题（英文原文在 [docs/](docs/README.md)）
- [docs/specs/](docs/specs/README.md)：五份规范，各自独立发版（英文）
- [.agents/docs/design.md](.agents/docs/design.md)：设计文档（英文）

**用 Agent 配置 mcppls**，把这段提示词发给它：

```
Read .agents/skills/mcppls-usage/SKILL.md and the docs/ directory of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then set up mcppls for my C++ modules project and show me what it can answer.
```

## 参与贡献

构建、测试和一致性测试见 [CONTRIBUTING.md](CONTRIBUTING.md)（英文），全部开发命令见
[开发工具](docs/zh-CN/93-devtools.md)。

欢迎用 Agent 参与开发，流程和必须遵守的规则写在
[.agents/skills/mcppls-contributing/SKILL.md](.agents/skills/mcppls-contributing/SKILL.md) 里：

```
Read .agents/skills/mcppls-contributing/SKILL.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then follow it to help me submit a contribution.
```

## 许可证

Apache-2.0，见 [LICENSE](LICENSE)；payload 中附带的第三方组件见 [NOTICE](NOTICE)。
