# mcppls - mcpp language server

> 一个 C++ 模块语言服务器 —— 在任何编译器上给出同一套模块语义，编辑器、编码代理与 CI 都用同一份

[English](README.md) | **简体中文**

[文档](docs/README.md) · [安装](docs/00-install.md) · [设置](docs/30-settings.md) ·
[面向代理](docs/40-agents.md) · [规范](docs/specs/README.md) ·
[Releases](https://github.com/Sunrisepeak/mcpp-language-server/releases)

C++20 将 named modules 纳入标准，其余部分各工具链并未一致：BMI 格式不兼容，依赖扫描各异，
`import std` 的来源也不同——一份能正常构建的模块代码，换个编译器，编辑器就什么也给不出。

mcppls 把任何构建——[mcpp](https://github.com/mcpp-community/mcpp)、CMake、一份裸的
`compile_commands.json`，甚至什么都没有——归一成同一种模块描述，用它驱动锁定版本的 clangd，
并补上 clangd 不回答的模块级请求。编码代理与 CI 通过 MCP 和命令行拿到同样的答案。

## 能力

- **任何编译器上的模块语义** —— GCC、Clang、MinGW、clang-cl 与 MSVC，各用自己的 `std`
- **零配置** —— 自动识别构建方式、探测工具链，状态里写明当前所用的是什么
- **clangd 没有的** —— 模块名跳转、`import` 补全、模块图、模块诊断
- **不被自己启动的程序拖住** —— 构建工具离线运行，独立进程单元，到期即结束
- **故障留在原地** —— 损坏的模块得到占位单元，不拖垮整个工程
- **面向代理与 CI** —— 跨导入的引用与调用者、编辑后校验、带证据的审查
- **一个 payload** —— 服务端、锁定的 clangd 与语义工具包一起交付，没有编译器也能 `import std`

## 安装

### 让代理安装

把这段发给编码代理，它会按下面的某一种方式替你装好：

```
Read .agents/skills/mcppls-usage/SKILL.md and docs/00-install.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then install mcppls for the editor I use — from the latest release, or built from source
if there is none for my platform — and check that it works on my C++ modules project.
```

### 从 release 安装

从 [release 页面](https://github.com/Sunrisepeak/mcpp-language-server/releases) 下载：

| 编辑器 | 文件 | 安装 |
|---|---|---|
| VS Code | `mcppls-<平台>.vsix` | `code --install-extension mcppls-<平台>.vsix` |
| Zed、CLion | `mcppls-zed-<版本>.tar.gz`、`mcppls-clion-<版本>.zip` | 见 [docs/00-install.md](docs/00-install.md) |
| 其他 LSP 客户端 | `payload-<平台>.tar.gz` | 解压，把 `bin/` 加入 `PATH`，运行 `mcppls serve` |

### 从源码构建

**1. 通过 [xlings](https://github.com/openxlings/xlings) 安装 mcpp**（每台机器一次）：

```bash
curl -fsSL https://d2learn.org/xlings-install.sh | bash         # Linux / macOS
irm https://d2learn.org/xlings-install.ps1.txt | iex            # Windows（PowerShell）

xlings install mcpp -y -g                                       # 然后新开一个终端
```

**2. 构建服务端并装进编辑器：**

```bash
mcpp build
mcpp run -p devtools -- extension --editor vscode --install
```

| 编辑器 | 安装 | 卸载 |
|---|---|---|
| VS Code | `extension --editor vscode --install` | `uninstall --editor vscode` |
| Zed | `extension --editor zed --install`，再在 Zed 命令面板执行 *zed: install dev extension*（或加 `--link`） | `uninstall --editor zed` |
| CLion | `mcpp run --features clion -p devtools -- extension --editor clion --install`，然后重启 CLion | `uninstall --editor clion` |

编码代理与其他客户端见 [docs/10-editors.md](docs/10-editors.md)。

### 让代理安装

把这段发给编码代理，由它替你完成上面的步骤：

```
Read .agents/skills/mcppls-usage/SKILL.md and docs/00-install.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then install mcppls for the editor I use — from the latest release, or built from source
if there is none for my platform — and check that it works on my C++ modules project.
```

## 它知道多少

| | 工程 | 模块描述来自 | 你得到 |
|---|---|---|---|
| **L1** | **mcpp** | `mcpp emit build-database`，离线运行 | 每个单元、它的角色与参数，以及该工具链的 `std` |
| **L2** | **CMake**（`FILE_SET CXX_MODULES`） | 构建目录的数据库，或 mcppls 自己的私有配置 | 生成器的答案，`@modmap` 已展开 |
| **L3** | 只有 **`compile_commands.json`** | 数据库加扫描 | 每个文件的参数，模块角色由扫描补全 |
| **L4** | **只有源码**，或没有编译器 | 扫描加内置语义工具包 | 模块照样解析，`import std` 可用，诊断来自 libc++ |

逐级降级，并始终说明当前在哪一级。不受信任的工作区就是 L4：不运行任何构建工具和编译器。

## 文档

- [docs/](docs/README.md) —— 安装、编辑器、工程类型、设置、代理与 CI、排障
- [docs/specs/](docs/specs/README.md) —— 五份规范，独立版本
- [.agents/docs/design.md](.agents/docs/design.md) —— 设计

**用编码代理上手**，把这段发给它：

```
Read .agents/skills/mcppls-usage/SKILL.md and the docs/ directory of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then set up mcppls for my C++ modules project and show me what it can answer.
```

## 参与贡献

构建、测试与一致性夹具见 [CONTRIBUTING.md](CONTRIBUTING.md)，所有贡献者命令见
[docs/93-devtools.md](docs/93-devtools.md)。

欢迎借助编码代理完成的贡献；工作流程与不可妥协的规则见
[.agents/skills/mcppls-contributing/SKILL.md](.agents/skills/mcppls-contributing/SKILL.md)：

```
Read .agents/skills/mcppls-contributing/SKILL.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then follow it to help me submit a contribution.
```

## 许可

Apache-2.0。见 [LICENSE](LICENSE)，payload 捆绑内容见 [NOTICE](NOTICE)。
