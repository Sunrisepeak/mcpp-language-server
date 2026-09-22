# 开发 mcppls：mcpp、mcppls 与 mcppls-devtools

[English](../93-devtools.md) | **简体中文**

这个仓库对贡献者的所有要求，都走这三条命令。不用记脚本路径，也不用装解释器：这里的每个程序都是用 mcpp 在 openkal 上构建出的 C++，Linux、macOS、Windows 用的是同一份源码。

| 命令 | 回答什么问题 | 示例 |
|---|---|---|
| `mcpp` | 构建、测试、运行，以及环境 | `mcpp build`、`mcpp test --workspace`、`mcpp run -p devtools -- …` |
| `mcppls` | 用户排查自己机器上的问题时需要的东西 | `mcppls cache`、`mcppls check FILE`、`mcppls report`、`mcppls print-environment` |
| `mcppls-devtools` | 只有参与本仓库开发的人才需要的东西 | 打包、发布、仓库检查、基准测试 |

**新命令该归到哪一个。** 排查自己问题的用户会跑它，就归服务端；只有参与本仓库开发的人才会跑它，就归 devtools；mcpp 已经能做的事，两边都不归。devtools 从不包装 mcpp 的动词，也从不重复服务端的诊断功能。

## 快速上手

```bash
mcpp build                                                   # 服务端
mcpp test && mcpp test --workspace                           # 服务端的测试，然后是所有成员的测试
mcpp run -p devtools -- extension --editor vscode --install  # payload、VS Code 扩展，并安装
```

`mcpp run -p devtools -- <command>` 会先准备好这条命令需要的东西（Node、Rust、Gradle——见[环境](#环境)），然后再执行它。构建出的二进制文件也可以直接运行，CI 就是这么做的；这时候所需工具必须已经在 PATH 里，缺哪个工具，报错里会点出声明它的地方。

## 仓库结构

根目录的 `mcpp.toml` 是一个以服务端为根的工作区。它按**依赖集合**拆分：mcpp 会把一个包里的每个模块都链接进这个包的每个可执行文件，所以某个工具需要、但服务端不能带上的东西，就放进服务端不依赖的那个成员里。

| 成员 | 是什么 | 依赖 |
|---|---|---|
| `.`（根目录） | `mcppls`，以及它自己的测试和生成程序：`mcppls-conformance`、`mcppls-lspgen`、两个 mock | base, platform |
| `modules/base` | 错误处理、路径、文本、globbing、sha256、URI、日志、产品版本号 | openkal |
| `modules/platform` | 文件、进程、环境变量、目录、回环 socket——唯一直接对接 openkal 的包 | base |
| `modules/pack` | 校验下载、归档、payload 和 kit 的组装、发布检查 | base, platform, tinyhttps, libarchive |
| `tools/devtools` | `mcppls-devtools` | base, platform, pack |
| `tools/model-gateway` | `mcppls-model`，参考实现的 model gateway | openkal, tinyhttps |

`mcpp build -p <member>` 和 `mcpp test -p <member>` 只作用于一个成员；`--workspace` 作用于全部成员（根包用不带参数的命令就能作用到）。`modules/os/*` 存放六个平台常量，是唯一允许出现平台差异的地方（`mcppls-devtools check os-surface`）。

## 按任务分类的命令

### 开发

| 任务 | 命令 |
|---|---|
| 构建服务端 | `mcpp build` |
| 为另一个平台构建 | `mcpp build --target aarch64-macos` / `--target x86_64-windows-gnu` |
| 从 LSP meta model 重新生成 `src/lsp` | `mcpp run mcppls-lspgen -- generate --meta-model vendor/lsp-metamodel/metaModel-3.18.json --out src/lsp` |
| 一次性改遍产品版本号 | `mcpp run -p devtools -- version --set 0.0.2` |

### 测试

| 任务 | 命令 |
|---|---|
| 单元测试 | `mcpp test`、`mcpp test --workspace` |
| 仓库不变量检查 | `mcpp run -p devtools -- check all` |
| 一致性测试用例 | `mcppls-conformance run --server … --payload … --fixture conformance/fixtures/<name>`（[conformance/README.md](../../conformance/README.md)） |
| 审查测试用例（精确率、召回率） | `mcpp run -p devtools -- bench review --server … --payload … --mock-model …` |
| Agent 任务基准测试，基线与参考结果 | `mcpp run -p devtools -- bench tasks validate` |
| 规范文档的 schema | `python3 docs/specs/tools/validate.py`——唯一还留着的 Python，见[下文](#不是-c-的部分) |
| 真实项目压力测试：多套 fixture × 客户端画像的矩阵 | `mcpp run -p devtools -- stress --payload … --fixture module-faults --client vscode --client neovim` |

### 调试

| 任务 | 命令 |
|---|---|
| 模块缓存里有什么，以及怎么清 | `mcppls cache [--modules] [--format json]`、`mcppls cache --clean <name>` |
| 服务端怎么理解某个文件 | `mcppls check FILE` |
| bug report 需要的一切信息 | `mcppls report` |
| 多次运行的启动耗时，可选对照预算值 | `mcpp run -p devtools -- measure summary DIR [--max-cold S] [--max-warm S]` |

### 打包与发布

| 任务 | 命令 |
|---|---|
| 本机对应的 payload | `mcpp run -p devtools -- payload` |
| 另一个平台的 payload | `mcpp run -p devtools -- payload --platform win32-x64 --server PATH` |
| 检查一个 payload | `mcpp run -p devtools -- payload --verify DIR` |
| 单独构建语义工具包 | `mcpp run -p devtools -- kit --platform linux-x64 --out DIR` |
| 编辑器扩展 | `mcpp run -p devtools -- extension [--editor vscode\|zed\|clion\|all] [--install [--link]]` |
| 安装一个已有的插件（来自某次 release） | `... -- extension --editor zed\|clion --install --plugin PATH --payload DIR` |
| 移除已安装的扩展 | `mcpp run -p devtools -- uninstall --editor vscode\|zed\|clion\|all` |
| 检查暂存好的 release | `mcppls-devtools release check --version V --dir release [--render]` |
| 一次 release 的 xlings 产物 | `mcppls-devtools release xlings --payloads DIR --out DIR` |

## 环境

devtools 自己不装任何东西。它要运行的工具都声明在根目录的 `mcpp.toml` 里，`mcpp run -p devtools` 会先把这些准备好：

| 工具 | 给谁用 | 声明位置 |
|---|---|---|
| Node（npm、npx） | VS Code 扩展 | `[xlings.workspace]`，`when = "dev"` |
| Rust（cargo、rustup） | Zed 扩展 | `[xlings.workspace]`，`when = "dev"` |
| Gradle 及其 JDK | CLion 插件 | `[feature-xlings.clion]`：`mcpp run -p devtools --features clion -- extension --editor clion` |
| cmake、ninja | 语义工具包里 libc++ 的 configure 步骤 | 由宿主机提供（CI 会安装） |

`when = "dev"` 表示这些工具只为 `mcpp run` 准备，`mcpp build` 或 `mcpp test` 不会用到，普通使用者也用不到。

## CI 与本地使用同样的命令

CI 里的每一步，调用的都是 `mcpp` 或 `mcppls-devtools`；同样的参数，在本地也能跑。这个 workflow（[.github/workflows/ci.yml](../../.github/workflows/ci.yml)）在每种宿主机上构建一次 devtools，把二进制文件交给后面的 job 用，包括没有工具链的 release job。

## 不是 C++ 的部分

| 内容 | 原因 | 到什么时候为止 |
|---|---|---|
| `editors/vscode`（TypeScript）、`editors/clion`（Kotlin）、`editors/zed`（Rust → WebAssembly） | 扩展的实现语言由编辑器决定 | 永久如此；devtools 负责驱动它们的构建 |
| `docs/specs/tools/validate.py` | JSON Schema 2020-12 校验；mcpp 索引里还没有对应的 C++ 校验器 | 直到有了这样的校验器，或者规范能用 draft-07 说清同样的东西 |

除此之外的脚本，`mcppls-devtools check scripts` 一律判失败；完整清单和每一项的理由在 `tools/devtools/scripts.allow` 里。

## 新增一个命令

- 逻辑放进一个库模块——打包相关的数据放 `modules/pack`，其余放 `tools/devtools/src`——旁边配一个测试（`mcpp test -p pack`、`mcpp test -p devtools`）。命令本身只负责解析参数、调用这个模块。
- `tools/devtools/src/` 下每个命令一个模块，导出一个 `cmdline::App`；`main.cpp` 加一行就能接上它。
- 给 CI 或 Agent 读取的东西都要有 `--json`。报错用一句话说清该怎么做。
- 文件、进程和环境变量都走 `mcppls.platform`，不直接用 C 标准库或操作系统接口；平台差异只能落在那六个常量里。
- 同一次改动里把上面的表也更新掉（`mcppls-devtools check docs` 会拿它们跟 `--help` 的输出做比对）。
