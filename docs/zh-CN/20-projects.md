# 工程类型

[English](../20-projects.md) | **简体中文**

mcppls 不需要你描述自己的构建方式。它会自己去找，然后在状态栏里告诉你找到了什么。下面说明每类工程里"找到"指什么，以及找不到时你会得到什么。

状态栏里的 `L1`..`L4` 是 *tier*：工程是怎么被描述的——L1 是构建数据库（mcpp 的 `emit build-database`，或者你自己提供的），L2 是 CMake 自己的数据库，L3 是单独一份 `compile_commands.json`（包括版本太旧、无法 emit 构建数据库的 mcpp 留下的那份，以及 xmake、meson 写出的那份），L4 是只有源码（不受信任的工作区永远是 L4，不管磁盘上还有什么）。它和 `mcppls check`、`cxxModules/status` 里的 `level` 不是同一个数：`level` 是 [S1](../specs/s1-build-database.md) 自己的 1..4，表示数据库*文档*结构化得有多完整；手写的 level 3 数据库和 mcpp 的构建数据库都是 L1，没有 `FILE_SET CXX_MODULES` 的 CMake 工程即使 level 1 也是 L2。状态栏和编辑器插件只显示 `L<tier>`，把两者区分开。模型不会被更差 tier 的模型替换：如果构建工具之后给出的描述不如手上的完整，就保留手上的模型，并在状态里说明它可能已过期。

## mcpp

mcppls 直接向 mcpp 要构建描述：

```
mcpp emit build-database --format json
```

mcpp 给出的文档里列出了每个翻译单元、它的模块角色、它的参数，以及它用的标准库模块——不需要构建任何东西，也不写入你的项目。这是最好的情况：构建描述直接来自掌管构建的工具本身。

有两点要注意：

- **这一步是离线的。** 获取构建描述是查询工程信息，而不是替用户执行任务，所以服务端以 `MCPP_OFFLINE` 发起查询。如果项目依赖的东西机器上还没有，mcpp 会说明这一点，而且不需要等你做任何决定：
  - 工程立即按源码提供服务（L4），mcpp 能描述的部分照常使用；
  - 右下角的通知提供 **Download and Continue**（这一次允许构建工具联网）、**Run in Terminal**（你的代理和凭证在那里）和 **Don't Ask Again**。它可以一直不点；每个工作区、每组缺失的东西只问一次；
  - 构建描述会在 30 秒、1 分钟、2 分钟后、之后每 5 分钟离线重试一次，`mcpp.toml` 或 `mcpp.lock` 一变化就立即重试——所以你在自己的终端里构建了，工程会自己升级，那个询问也就不再适用。

  `mcppls.buildTool` 和 `mcppls.buildDiscovery.askBeforeDownload` 见 [30-settings.md](30-settings.md)。
- **构建规则生成的文件。** 规则包（比如 `mcpp:plugins` 的 `rules-qt`）在构建时把 `.ui`、`.qrc`、`.ts` 变成头文件和源文件。mcpp 描述构建时不运行这些步骤，所以表单的 `ui_*.h` 在它的描述里还不存在。mcppls 会去掉规则的输入（它们不是 C++），构建写过这些文件时从你工程自己的 `target/` 读取，否则说明缺了哪些，并提供 **Build in Terminal**；一旦构建写出了它们，包含它们的文件不用重启就有语义。
- **老版本的 mcpp** 没有 `emit build-database`，但这不是终点：如果机器上别处装有更新的 mcpp（xlings 的包目录、mcpp 自己的 registry 目录），mcppls 会改问*那一个*，同样只读、离线，只用来描述工程——工程本身仍用它固定的 mcpp 构建。发生这种情况时状态会说"described by mcpp X (the project pins Y)"。只有机器上没有任何 mcpp 能回答时，才会让固定的那个做配置（`mcpp build --configure-only`），这一步会写入项目；这时状态会说明，解决办法是升级 mcpp。
- **过期的数据库**——条目指向的文件已经不在了（构建曾经写过的 `target/` 后来被删掉，或者从别的机器提交进来的 `compile_commands.json`）——只使用仍然存在的部分；状态会注明它已过期，而不是直接失败。构建时才生成的模块（依赖包自己的 `std`、代码生成器的输出），mcppls 会先去构建通常留下它们的地方找——工程自己的构建目录，以及 mcpp 的 build-database 缓存（删掉 `target/` 后它仍在）——找不到时才用空的占位单元。

## CMake

如果构建目录里有 `build_database.json`（CMake 4.4+ 配 Ninja、`FILE_SET CXX_MODULES`），mcppls 就读它。否则它读 `compile_commands.json`，并展开生成器写出的 `@modmap` 文件。

构建目录会在 `build*/`、`out/build/*`、`cmake-build-*`，以及 `CMakePresets.json`（或 `CMakeUserPresets.json`）第一个配置预设的 `binaryDir` 里找。

如果连构建目录都没有，而且是受信任的工作区，mcppls 会**自己配置一个**，放在它自己的缓存目录下——绝不会放进你的项目——并沿用那个预设的生成器、工具链文件和缓存变量，这样描述的就是你实际会得到的构建。这次配置是**断网的**（`-DFETCHCONTENT_FULLY_DISCONNECTED=ON`），第一次也一样：机器上没有的 `FetchContent` 依赖会让它停下，你会得到和上面 mcpp 一样的非阻塞询问（**Download and Continue** 会在那个私有目录里联网配置一次）。

## xmake

有 `xmake.lua` 就是 xmake 工程。根目录或 `.vscode/` 里已有的 `compile_commands.json`（xmake 的 VS Code 插件写在那里）会直接读取。否则 mcppls 用 xmake 自己的命令 `xmake project -k compile_commands` 获取一份，这个命令不编译任何东西——但它会配置并扫描模块，所以 mcppls 把 xmake 的配置目录和构建目录指向自己的缓存（`XMAKE_CONFIGDIR`、`--builddir`），你的工程保持不变。它离线运行（`--policies=package.fetch_only,network.mode:private`）：没有安装的包会让它停下，并给出和上面一样的询问。第一次描述要几秒钟（实测约 6–8 秒，大部分是 xmake 在探测工具链）；这期间工程按源码提供服务。模块角色靠扫描得到，所以 xmake 工程是 L3。

## meson

有 `meson.build` 就是 meson 工程。已有的构建目录（`builddir/`、`build/`，或任何带 `meson-private/` 的目录）会读取其中的 `compile_commands.json`。否则 mcppls 用 `--wrap-mode=nodownload` 把 `meson setup` 跑进自己的缓存；需要下载的子项目会让它停下，并给出同样的询问。和 xmake 一样是 L3。

## 关闭探测

`mcppls.buildDiscovery = off` 让 mcppls 完全不探测构建系统：不隐式读取或运行任何东西，只使用你用 `mcppls.database` 指定的数据库，否则扫描源码（L4）。`mcppls.buildDiscovery.providers` 则只去掉个别构建系统——比如只读现有的 CMake 构建目录、永远不运行 xmake。`mcppls.buildTool = off` 是更窄的开关：仍然探测构建系统、读取它们已有的输出，只是从不运行。见 [30-settings.md](30-settings.md)。

## compile_commands.json

任何 `compile_commands.json` 都能用，不管是谁生成的。mcppls 会扫描里面列出的源文件，补上数据库里没有的模块角色，并探测它提到的编译器，获取它们的模块信息。

## 没有构建系统

源文件会被扫描，模块角色从它们的声明里推断出来，同时在机器上找编译器。这是兜底方案，也是为什么打开一个装满 `.cppm` 文件的目录能得到有用结果，而不是一无所获。

## 也没有编译器

这时内置的**语义工具包**接管：libc++ 编译成 modules，附带一份说明每个模块位置的清单。`import std` 能解析，你自己代码里的模块也能解析。诊断信息来自一个和你实际构建所用不同的标准库，所以可能会有出入——状态栏会说明当前的语义来自语义工具包，而不是构建工具链。

## 用哪个 C++ 标准，以及 C++26

标准以构建为准：一个单元的命令里写的 `-std=`（或 `/std:`）是什么，mcppls 就交给 clangd 什么；`/std:c++latest` 即 C++26。另有两条规则：

- **同一上下文中的模块单元用同一个标准。** 模块的 BMI 只能在构建它时所用的标准下导入——`std` 按 C++23 构建时，C++26 文件里的 `import std` 会直接失败（"C++26 was disabled in precompiled file"）。因此同一上下文里导入、提供或属于某个模块的单元，统一按其中最新的标准来读，不涉及模块的普通单元保留自己的标准；有单元被提升时日志会说明，报告中的 `plan.languageStandard`、`plan.standardsSeen`、`plan.standardsRaised` 给出具体情况，状态中的 profile 也会写明所用标准。
- **没有任何构建描述的源文件，按读取它们的编译器所支持的最新标准来读**：语义工具包（clang 23、libc++ 23）、GCC 14 及以上、Clang 17 及以上为 C++26（Clang 20 之前写作 `c++2c`）；更老的编译器为 C++23，即支持 `import std` 的最低标准。

C++26 能用到什么，取决于 clangd 23.1：包索引（pack indexing）、`= delete("reason")`、占位变量 `_`、`static_assert` 自定义消息、`#embed`、可变参数友元，以及 clang 23 已实现的其余特性；标准库部分取决于你构建所用的标准库（语义工具包为 libc++ 23）。**契约（P2900）和反射（P2996）clang 23 尚未实现**：使用它们的代码即使 GCC 能编译，clangd 里也会报错；VS Code 仍会为 `contract_assert`、`pre`、`post` 着色。

## 不受信任的工作区

不会运行任何构建工具，也不会运行编译器——VS Code 的工作区信任机制优先于一切。语义由语义工具包提供，状态会说明原因。

## 状态栏告诉你什么

| 显示 | 含义 |
|---|---|
| 一个编译器和标准库 | 模型来自你的构建；用的是那个工具链的 `std` |
| 一个语义工具包 | 没找到可用的编译器，或者工作区不受信任 |
| "may be stale" | 构建工具这次没能给出答案；用的还是上次加载的模型 |
| "needs a download" | 离线规划需要本机没有的依赖；这期间工程按源码提供服务，并提供解决的操作（或者在终端里构建：它会自己升级） |
| "files the build generates … do not exist yet" | 规则的输出（比如 Qt 表单的头文件）还没构建出来；构建一次，包含它的文件就有语义 |
| "implementation unit(s) cannot be read" | 某个实现单元编译不了（通常是缺头文件），所以跳到定义到不了其中的定义 |
