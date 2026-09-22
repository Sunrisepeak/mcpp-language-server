# 工程类型

[English](../20-projects.md) | **简体中文**

mcppls 不需要你描述自己的构建方式。它会自己去找，然后在状态栏里告诉你找到了什么。下面说明每类工程里"找到"指什么，以及找不到时你会得到什么。

状态栏里的 `L1`..`L4` 是 *tier*：工程是怎么被描述的——L1 是构建数据库（mcpp 的 `emit build-database`，或者你自己提供的），L2 是 CMake 自己的数据库，L3 是单独一份 `compile_commands.json`（包括版本太旧、无法 emit 构建数据库的 mcpp 留下的那份），L4 是只有源码（不受信任的工作区永远是 L4，不管磁盘上还有什么）。它和 `mcppls check`、`cxxModules/status` 里的 `level` 不是同一个数：`level` 是 [S1](../specs/s1-build-database.md) 自己的 1..4，表示数据库*文档*结构化得有多完整；手写的 level 3 数据库和 mcpp 的构建数据库都是 L1，没有 `FILE_SET CXX_MODULES` 的 CMake 工程即使 level 1 也是 L2。状态栏和编辑器插件只显示 `L<tier>`，把两者区分开。模型不会被更差 tier 的模型替换：如果构建工具之后给出的描述不如手上的完整，就保留手上的模型，并在状态里说明它可能已过期。

## mcpp

mcppls 直接向 mcpp 要构建描述：

```
mcpp emit build-database --format json
```

mcpp 给出的文档里列出了每个翻译单元、它的模块角色、它的参数，以及它用的标准库模块——不需要构建任何东西，也不写入你的项目。这是最好的情况：构建描述直接来自掌管构建的工具本身。

有两点要注意：

- **这一步是离线的。** 获取构建描述是查询工程信息，而不是替用户执行任务，所以服务端以 `MCPP_OFFLINE` 发起查询。如果项目依赖的东西机器上还没有，mcpp 会说明这一点，状态栏会提供一个选项，让你在自己的终端里跑构建工具——你的代理和凭证都在那里。`mcppls.buildTool` 见 [30-settings.md](30-settings.md)。
- **老版本的 mcpp** 没有 `emit build-database`，但这不是终点：如果机器上别处装有更新的 mcpp（xlings 的包目录、mcpp 自己的 registry 目录），mcppls 会改问*那一个*，同样只读、离线，只用来描述工程——工程本身仍用它固定的 mcpp 构建。发生这种情况时状态会说"described by mcpp X (the project pins Y)"。只有机器上没有任何 mcpp 能回答时，才会让固定的那个做配置（`mcpp build --configure-only`），这一步会写入项目；这时状态会说明，解决办法是升级 mcpp。
- **过期的数据库**——条目指向的文件已经不在了（构建曾经写过的 `target/` 后来被删掉，或者从别的机器提交进来的 `compile_commands.json`）——只使用仍然存在的部分；状态会注明它已过期，而不是直接失败。构建时才生成的模块（依赖包自己的 `std`、代码生成器的输出），mcppls 会先去构建通常留下它们的地方找——工程自己的构建目录，以及 mcpp 的 build-database 缓存（删掉 `target/` 后它仍在）——找不到时才用空的占位单元。

## CMake

如果构建目录里有 `build_database.json`（CMake 4.4+ 配 Ninja、`FILE_SET CXX_MODULES`），mcppls 就读它。否则它读 `compile_commands.json`，并展开生成器写出的 `@modmap` 文件。

如果连构建目录都没有，而且是受信任的工作区，mcppls 会**自己配置一个**，放在它自己的缓存目录下——绝不会放进你的项目。第一次这样配置可能会下载项目声明的依赖（`FetchContent`、`ExternalProject`）；之后每一次都会加上 `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON`。

## compile_commands.json

任何 `compile_commands.json` 都能用，不管是谁生成的。mcppls 会扫描里面列出的源文件，补上数据库里没有的模块角色，并探测它提到的编译器，获取它们的模块信息。

## 没有构建系统

源文件会被扫描，模块角色从它们的声明里推断出来，同时在机器上找编译器。这是兜底方案，也是为什么打开一个装满 `.cppm` 文件的目录能得到有用结果，而不是一无所获。

## 也没有编译器

这时内置的**语义工具包**接管：libc++ 编译成 modules，附带一份说明每个模块位置的清单。`import std` 能解析，你自己代码里的模块也能解析。诊断信息来自一个和你实际构建所用不同的标准库，所以可能会有出入——状态栏会说明当前的语义来自语义工具包，而不是构建工具链。

## 不受信任的工作区

不会运行任何构建工具，也不会运行编译器——VS Code 的工作区信任机制优先于一切。语义由语义工具包提供，状态会说明原因。

## 状态栏告诉你什么

| 显示 | 含义 |
|---|---|
| 一个编译器和标准库 | 模型来自你的构建；用的是那个工具链的 `std` |
| 一个语义工具包 | 没找到可用的编译器，或者工作区不受信任 |
| "may be stale" | 构建工具这次没能给出答案；用的还是上次加载的模型 |
| "needs a download" | 离线规划需要本机没有的依赖；状态栏提供了解决的操作 |
