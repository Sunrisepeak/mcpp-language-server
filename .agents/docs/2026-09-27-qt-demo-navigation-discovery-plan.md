# mcppls 优化方案：qt-demo 链式报错、跳到声明而不是实现、构建工具无感探测

状态：方案第 3 版（对照 mcpp 2026.9.27.1 复核；问题 2 找到根因——clangd 的后台索引不为模块单元准备依赖，方案随之重排；
D1、D2、D5 已定，D4′ 待再确认）· 2026-09-27 · 基于 mcppls `main` 2e80b85（0.0.5）、mcpp `main` b439fd97（2026.9.27.1，
本机从源码构建：xlings 索引与 GitHub Release 尚未发布该版本）

依据：

- qt-demo（`/home/speak/test/mcpp/qt-demo`，mcpp 2026.9.26.2 + `mcpp:plugins` 0.15.2 `rules-qt`，gcc 16.1.0，Qt 6.11.1）：
  用户三次 VS Code 会话的日志（07:10、12:03、16:29，`~/.config/Code/logs/*/window*/exthost/sunrisepeak.mcpp-language-server/`），
  `mcppls check`、`mcpp emit build-database` 的原始输出、引擎数据库；以及在副本上验证修法的端到端运行（§1.4）；
- 跳转实测：自写 LSP 客户端（每秒请求一次 `textDocument/definition`，记录结果的变化）跑 mcppls 0.0.5 + payload 中的
  clangd 23.1.0，项目为 hello 系列、本仓库、GalTranslPP（`/home/speak/workspace/scode/GalTranslPP`）；
  clangd 行为对照 llvm-project 23.1.0 源码；
- 构建工具实测：cmake 4.4.2、xmake v3.1.1（含其 Lua 源码）在本机的离线行为和耗时；
- mcpp 2026.9.27.1（#719：#704–#716、`prepare.cppm` 拆成 16 个实现单元 + 实现分区 `:state`）：qt-demo 的 emit 复测；
  mcpp 自身源码作为大型"接口 + 实现单元 + 分区"项目做跳转实测（引擎数据库 507 条）。

编号：问题 1 为 Q1-*，问题 2 为 N-*（navigation），问题 3 为 B-*（build discovery），需要 mcpp / rules-qt 处理的为 M-*。

## 0. 摘要

### 0.1 三个结论

1. **qt-demo：`ui_mainwindow.h` 是持续存在的根因，两边都要修，mcppls 这边能独立解决。** mcpp 的 emit 数据库把生成物的
   `-I` 指向 emit 私有目录（`~/.mcpp/cache/build-database/<hash>/target/.build-mcpp/out/qt`），那里没有 `ui_mainwindow.h`；
   同时把 `.ui/.qrc/.ts` 写成了 C++ 翻译单元（M-1、M-2）。16:29 的会话里 std 已经不再误判，**剩下的就是这一条**。
   已验证修法：把私有目录换成项目自己的 `target/.build-mcpp/out/qt` 并剔除非 C++ 条目后，`main.cpp` 的
   `clangd --check` 退出码 0、零错误（Q1-2 + Q1-3，升为 P0）。另外 12:15 那次会话里 mcppls 把 std 的
   "找不到提供者" 误判成 "编不过"，把整个项目切到了 libc++（Q1-1）。**mcpp 2026.9.27.1 复测：M-1、M-2 原样存在**
   （`.ui/.qrc/.ts` 仍是翻译单元，`-I` 仍指向私有目录；emit 0.25 s，不写项目）。M-2 按 mcpp 规范是有意为之
   （emit 不写工程、不运行 action，R2.1/R2.5），所以向 mcpp 提的要求改为"在 S1 里标明生成物"，mcppls 的 Q1-3 长期保留。
2. **跳到声明：根因找到了，比第 2 版估计的严重。** clangd 23.1.0 的**后台索引编译模块单元时不准备模块依赖**
   （`BackgroundIndex::index()` 不经过 ModulesBuilder，源码可证；日志里每个模块单元都是 `Failed to compile …, index may be
   incomplete`，hello13 的 16 个项目单元全部如此，`math.cpp` 索引出 0 个符号）。实现单元里的定义能不能进索引，全看 clang 的
   错误恢复能保住多少：签名只用内建类型、限定名能解析的，常常碰巧对得上（所以最小 hello 基本正常）；签名里有模块里的类型
   （`PrepareState&`、`const std::string&`）、或成员函数属于分区里的类，就对不上——**索引里声明和定义变成两个符号**
   （`workspace/symbol` 能同时看到 `state.cppm:495` 与 `graph.cpp:61` 两条）。实测：**mcpp 2026.9.27.1 自身**拆出来的
   `prepare` 模块，从 `driver.cpp` 点 `phase0_…`、`phase4b_…`，从 `cmd_build.cppm` 点 `prepare_build`，420 s 内三处全部停在
   声明。打开过的文件走前台路径（会构建模块依赖），所以"打开过一次就好了"。**修法在 mcpp 源码上验证了三处中的两处**：让 clangd
   临时打开 `manifest.cpp`、`graph.cpp` 再关闭，5 s 内到 `.cpp`，关闭后 120 s 内一直正确（N-7）。第三处 `prepare_build` 另有原因：
   定义文件一直开着，hover 也知道两者是同一个函数，但索引里根本没有这个符号（`workspace/symbol` 为空），N-7 修不了，列为待查 O-1。GalTranslPP 的实现 `.cpp` 全是模块单元，同一机制适用，
   预计同样受影响；Windows 上的确认仍需 V-W。第 2 版里的 (a) 磁盘改动、(b) 解析不了、(d) `.ixx` 不被优先索引都还成立，
   但它们是这个根因之上的附加情况。
3. **构建工具探测收敛成 `BuildSystemProvider`，并且可以关掉。** 只看文件的探测 → 读已有产物 → 离线、私有目录、有时限地
   问构建工具 → 需要下载时问一次，同意后后台联网，期间 L4 顶上，拿到后无感切换。xmake 用的正是专门的
   `xmake project -k compile_commands`，**不编译**；但它会隐式配置并做**模块依赖扫描**，默认写进项目的 `build/`，
   所以要私有 `--builddir`；加上 `--policies=package.fetch_only,network.mode:private` 后不再联网更新仓库（已验证）。
   新增设置 `mcppls.buildDiscovery`（`auto` / `off`）和按提供者的开关（B-7）。对照 mcpp 2026.9.27.1：emit 已声明自己的
   副作用（`effects`: `read-project`、`write-global-cache`、`exec-build-script`），缺依赖有稳定代码
   `MCPP_OFFLINE_DOWNLOAD_REQUIRED`（mcppls 已按代码识别）；但**部分回答里某个成员需要下载时，mcppls 只报
   `producer-partial`，不会进入询问流程**（B-8，新发现）。

### 0.2 已定的决策

| # | 决定 |
|---|---|
| D1 | CMake 首次配置改为离线优先（修订 BD7）：不问就不联网 |
| D2 | 联网获取按工作区一次性同意；提供"不再询问"；不改全局设置 |
| D5 | 首个模型的等待 10 s → 2.5 s，之后 L4 先上、后台继续 |
| D4′ | **待再确认**：原提案"tier 1/2 只按文件回退"做不到（§7 R1），改为"只在确认是 std 自身编译失败时整体切换，并同时移除工具链的 std 单元" |

### 0.3 条目总表

| 主题 | # | 修什么 | 证据 | 归属 | 优先级 | 估算 |
|---|---|---|---|---|---|---|
| **qt-demo** | Q1-3 | 生成物目录：emit 私有目录缺文件时，只读地改用项目自己 `target/` 下的同一相对路径；都没有就报 `generated-files-missing`，按钮"在终端运行 mcpp build" | 副本端到端：`clangd --check` 退出码 0 | mcppls | **P0** | 1.5 天 |
| | Q1-2 | 引擎数据库剔除非 C/C++ 条目，计入 "left out" 并记日志 | 同上；clangd `'linker' input unused`、`Couldn't build compiler invocation` | mcppls | P0 | 0.5 天 |
| | Q1-1 | std 的 `unresolved` 不触发 kit 回退；回退只认 std 单元自身的编译失败（D4′） | 12:15:52.944 加入 `std.cc`，78 ms 后被判失败 | mcppls | P0 | 1 天 |
| | Q1-4 | 数据库代际：clangd 读到新一代之前报的模块失败只记日志 | 同上 | mcppls | P0 | 1 天（与 Q1-1 合做） |
| | M-1 | emit 不应把规则输入（`.ui/.qrc/.ts`）写成翻译单元（已提 mcpp#724 §1） | emit 原始输出 | mcpp | — | — |
| | M-2 | emit 的生成物路径指向从未填充的私有目录（规范 R2.1/R2.5 有意不运行 action）：请 mcpp 在 S1 里标明哪些 `-I` 目录与单元是生成物、由哪个规则动作产生，必要时附上 `mcpp build` 会放到的工程内路径 | 2026.9.27.1 复测仍在；已提 mcpp#724 §2（含可选的"只跑无副作用生成器"模式） | mcpp / rules-qt | — | — |
| | M-3 | 离线 emit 缺依赖时整份失败 → 返回能规划的部分 + 缺什么（S2 `producer-partial` 已有形状） | 07:10 会话只拿到 inferred | mcpp | — | — |
| **跳转** | N-7 | **模块感知的索引补全**：mcppls 让 clangd 以前台路径（带模块依赖）逐个打开再关闭实现单元，补上后台索引建错的定义；三级队列——按需（definition 只拿到声明时，N-2）、相关（打开的文件所在模块与直接导入模块的实现单元，N-5）、其余（空闲时全部）；clangd 每次重启后重做 | mcpp 源码：5 s 到 `.cpp`，关闭后 120 s 内稳定；hello B 场景 2 s | mcppls | **P0** | 3 天 |
| | N-1 | 未打开的源文件在磁盘上变了：送进 N-7 的"相关"级队列 | 修前 125 s 不恢复 | mcppls | P0 | 0.5 天（并入 N-7） |
| | N-6 | 上游：后台索引为模块单元准备依赖（`BackgroundIndex::index()` 接入 ModulesBuilder）；登记到 #24，附 hello13 最小复现；另做一个探索：让 mcppls 的 module-hints 路径指向 clangd 自己构建的 BMI，后台索引即可成功（hello13：16/17 单元编译成功、9 个定义全部正确），但在 mcpp 源码上粗糙的链接方式让 clangd 12 s 内崩溃，需要设计 | §2.3 | 上游 / 探索 | P2 | 1 天（登记 + 复现）；探索另计 |
| | N-3 | 解析不了的实现单元进状态栏（`implementation-unreadable`） | hello5、GalTranslPP（Linux） | mcppls | P1 | 1 天 |
| | N-4 | 夹具：分区里声明、签名含类类型的函数（最小复现），磁盘改动、新文件、坏实现单元 | §5 | mcppls | P1 | 1 天 |
| | V-W | GalTranslPP 的 Windows CI 跳转实测（修前 / 修后） | §2.4 | 验证 | P1 | 0.5 天 + CI 约 1 h |
| **构建探测** | B-1 | `BuildSystemProvider` 接口与注册表，现有 mcpp / CMake / compdb / inferred 平移 | §3.2 | mcppls | P1 | 3 天 |
| | B-2 | 需要下载时的询问 + 一次性联网获取 + L4 顶替 + 无感切换 | §3.5 | mcppls + 插件 | P1 | 3 天 |
| | B-3 | CMake 首次配置离线优先（D1）；读 `CMakePresets.json` | 实测 5.5 s 失败、消息可识别 | mcppls | P1 | 1.5 天 |
| | B-4 | 首个模型等待 2.5 s（D5） | BD5 | mcppls | P1 | 0.5 天 |
| | B-5 | xmake 提供者 | 实测：离线配方有效、不写项目 | mcppls | P1 | 2 天 |
| | B-7 | 无感探测的开关：`mcppls.buildDiscovery`、按提供者启停、是否询问下载 | 用户要求 | mcppls + 插件 | P1 | 1 天 |
| | B-8 | mcpp 的部分回答里带 `MCPP_OFFLINE_DOWNLOAD_REQUIRED` 时：用已规划的部分，同时进入询问流程；`MCPP_BUILD_DATABASE_HOST_TOOL_DEFERRED`（note）并入 `generated-files-missing` 的说明 | `src/project/mcpp.cpp:312` 只看 error、只报 partial | mcppls | P1 | 0.5 天 |
| | B-6 | meson 提供者（`--wrap-mode=nodownload`） | 未实测 | mcppls | P3 | 1 天 |

mcppls 侧合计约 **24–26 人日**（不含 B-6 与 N-6 的探索）。

### 0.4 版本划分建议

| 版本 | 内容 | 估算 |
|---|---|---|
| **0.0.6 "准"** | Q1-1～Q1-4、N-7（含 N-1/N-2/N-5）、N-3、N-4、N-6 登记、V-W；同时向 mcpp 提 M-1/M-2/M-3 | 约 12 天 |
| **0.0.7 "顺"** | B-1、B-7、B-4、B-2、B-8、B-3、B-5 | 约 11.5 天 |
| 之后 | B-6（meson） | 约 1 天 |

## 1. 问题 1：qt-demo 的链式报错

### 1.1 现场

项目：`[build] sources = ["src/*.cpp", "ui/*.ui", "res/*.qrc", "i18n/*.ts"]`，`build.mcpp` 调 `mcpp::rules::qt::compile`
（moc、uic、rcc、lupdate/lrelease），`src/main.cpp` 第 2 行 `#include "ui_mainwindow.h"`，之后 `import std;`。

三次会话：

| 会话 | 模型 | 现象 |
|---|---|---|
| 07:10 | 先 inferred（`producer-needs-download`：`xim:qt-base@6.11.1` 未安装），07:11:40 起 mcpp 模型 | `ui_mainwindow.h` 找不到；`.ui/.qrc/.ts` 索引失败 |
| 12:03 | mcpp 模型（缓存），12:15 重读后加入 std 单元 | 同上，**加上** std 误判 → 整项目切 libc++ kit → 重启 |
| 16:29 | mcpp 模型，一开始就有 2 个 std 单元 | 只剩 `ui_mainwindow.h` 找不到（`IncludeCleaner` 刷屏）和三个非 C++ 条目；没有 kit 回退 |

12:03 会话的关键行：

```
12:15:49.833 engine database changed: 6 compiled otherwise ...; restarting clangd in 2 s
12:15:52.944 engine database: 10 entries (2 standard library units) — 2 added std.cc, std.compat.cc
12:15:53.022 warning: argument unused during compilation: '-I …' '-O0' …          （非 C++ 条目）
12:15:53.022 Failed to build module std; due to Don't get the module unit for module std
12:15:53.022 clangd could not build the standard library module …; reading the project with the semantic kit
12:15:53.830 engine database changed: 2 added std.compat.cppm, std.cppm, 2 removed std.cc, std.compat.cc, 6 compiled otherwise
12:15:58.764 clangd could not scan …/gcc/16.1.0/include/c++/16.1.0/bits/std.cc: fatal error: 'bits/stdc++.h' file not found
12:16:01.833 restarting clangd: a module's unit left the engine database (std)
```

### 1.2 链条

```
M-2 生成物 -I 指向空目录 ──► main.cpp 致命失败（ui_mainwindow.h）──► main.cpp 无语义、无法跳转（每次会话都有）
M-1 .ui/.qrc/.ts 成了翻译单元 ──► 三条扫描、索引失败，日志刷屏（每次会话都有）
数据库刚加入 std.cc，clangd 仍在用旧一代 ──► "Don't get the module unit for module std"（只在 12:15 出现）
      └─► mcppls 判为 std 编不过（Q1-1）──► 整项目切 kit ──► gcc 的 std.cc 用 kit 参数 → bits/stdc++.h 找不到 ──► 再重启
```

### 1.3 责任划分

| 环节 | 归属 | 理由 |
|---|---|---|
| `.ui/.qrc/.ts` 成为 TU（`g++ … -c ui/mainwindow.ui -o …/mainwindow.ui.o`） | **mcpp**（M-1） | S1 的翻译单元是 C/C++ 编译命令；`rules-qt` 已用 `device_extensions` 声明了这些扩展名 |
| 生成物目录为空（私有目录里只有两个 0 字节文件，没有 `ui_mainwindow.h`） | **mcpp / rules-qt**（M-2） | emit 在私有目录（`$MCPP_HOME/cache/build-database/<key>`）规划，按规范不写工程（R2.1）、不运行 action（R2.5，2026.9.27.1 起连宿主工具也不构建，记 note `MCPP_BUILD_DATABASE_HOST_TOOL_DEFERRED`），所以 uic 的产物永远不会出现在那里；项目自己的 `target/.build-mcpp/out/qt/ui_mainwindow.h` 是存在的。能要求 mcpp 的是**标明**这些路径是生成物，而不是去生成 |
| 缺依赖时只能拿到 inferred | **mcpp**（M-3，改进） | 离线时整份失败 |
| std 误判 → 整项目 kit | **mcppls**（Q1-1、Q1-4） | `src/engine/clangd.cpp:2001` 的 `stdFailed && kind != FailureKind::other` 把 `unresolved`（`src/engine/clangd/process.cpp:220`）也算作失败 |
| 非 C++ 条目进了引擎数据库 | **mcppls**（Q1-2，防御） | `is_cxx_source_name`（`src/project/scan.cpp:500`）已有扩展名表 |
| 生成头文件缺失没有提示，也不去找已构建好的 | **mcppls**（Q1-3） | 设计 P7；设计 2.1 对生成模块已经"去构建留下的地方找"，头文件目录没有覆盖 |

### 1.4 mcppls 的修法

**Q1-3 生成物目录（P0，已端到端验证）。**

1. 数据库里的 `-I` 目录或源文件落在 mcpp 的 build-database 私有根下（`~/.mcpp/cache/build-database/<hash>/target/…`，
   `src/project/generated.cpp:70` 已认识这个根）时，取 `target/` 之后的相对路径，去 `<项目根>/target/` 下找；
   存在且含有被引用的文件就**只读地**替换。与设计 2.1 对生成模块的做法一致，扩展到头文件目录和生成的源文件。
2. 都没有（从未构建）→ issue `generated-files-missing`："`ui_mainwindow.h` 由构建规则生成，尚不存在；运行一次
   `mcpp build` 后可用"，按钮"在终端运行"。只挂在引用它的文件上，只报一次。项目构建后 watch 到 `target/` 下对应文件出现，
   重新计划。
3. 不受信任的工作区不做 1（与设计 2.1 的 untrusted 规则一致）。
4. 这是临时对策：它依赖 mcpp 私有目录与 `target/` 的对应关系。M-2 落地后（S1 里标明生成物及其动作）改为按声明处理；
   对应关系对不上时只报 issue，不猜。

验证：在 qt-demo 副本上，把 emit 输出中的私有目录换成副本自己的 `target/.build-mcpp`、去掉 `.ui/.qrc/.ts` 三条，用
`mcppls --database` 读取：`database 5 entries, 2 standard library units`，`clangd exit 0`，无任何错误
（修前同样的 `check` 退出码 1，`ui_mainwindow.h` 找不到）。

**Q1-2 剔除非 C/C++ 条目。** 进入引擎数据库前按扩展名（`is_cxx_source_name` 加上 `.c`、`.m`、`.mm`）和 S1 的语言字段过滤，
剔除的计入 `left out`，日志写明 "3 entries are not C or C++ (mainwindow.ui, …); left out"。mcppls 自己的模块索引也不扫它们。

**Q1-1 std 回退只认"std 单元自身的编译失败"（D4′）。**

- `unresolved`（找不到提供者）只说明 clangd 眼里的数据库没有 std 的单元。处理：核对 clangd 当前读到的那一代数据库里有没有
  std 的提供者。没有 → 代际竞态，等它读到新一代（Q1-4）；有 → 按 `module-unresolved` 报在导入它的文件上，不切 kit。
- 只有 `compile` 类失败、且失败的源文件就是 std 的单元时才切 kit。切换时**同时从数据库移除工具链的 std 单元**
  （12:15:58 的 `bits/stdc++.h` 就是 gcc 的 `std.cc` 被套上 kit 参数造成的），issue 里写明原因，并提供"重试工具链的 std"。
- 不做"按文件回退"，原因见 §7 R1。

**Q1-4 数据库代际。** 每次写引擎数据库记一个代号；clangd 重启或重读后才算"读到了这一代"。之前报出的模块失败、扫描失败，
记日志但不触发回退、不建事故、不算入重启预算。

### 1.5 `windows.h`

qt-demo 在 Linux 上三次会话都没有出现 `windows.h`。GalTranslPP 的 `Tool.ixx`、`Tool.cpp` 等在全局模块片段里
`#include <Windows.h>`，这类 Windows 项目在 Linux 上打开必然报这个错，属于预期（它们的依赖也只装在 Windows 上）；
mcppls 应做的是 N-3：说清楚"这些单元读不了、为什么"，而不是让错误连锁。如果 qt-demo 在 Windows 上也见到
`windows.h`，仍需要那边的问题包（`mcppls.exportBundle`）才能判断。

## 2. 问题 2：点击函数跳到声明而不是实现

### 2.1 行业惯例

| 工具 | 默认点击 / F12 | 声明 | 在定义上再点 |
|---|---|---|---|
| LSP | `textDocument/definition`：定义（函数体） | `textDocument/declaration` | — |
| VS Code | Ctrl+Click、F12 = Go to Definition；Go to Declaration 单独命令；Ctrl+F12 = Go to Implementation（虚函数覆写） | | |
| clangd | 索引里有定义就给定义，否则给声明 | 给声明 | 在定义上 → 声明 |
| Visual Studio | F12 到 `.cpp` 里的函数体 | Ctrl+F12 | |
| Qt Creator | F2 Follow Symbol：到定义；在定义上 → 声明 | | 切换 |

模块项目的约定：**`.cppm`/`.ixx` 相当于头文件，实现单元相当于源文件；Ctrl+Click 到实现，Go to Declaration 到接口，
在函数体上再点回到接口。** mcppls 的 `mcpp-split` 夹具已按此约定；mcppls 自己的引擎只回答模块名位置
（`src/engine/native/index.cpp:187`），函数的定义完全取决于 clangd 的索引。问题在索引什么时候不全。

### 2.2 实测（Linux）

| 场景 | 结果 | 结论 |
|---|---|---|
| 最小 hello（llvm@22 / gcc@16 / L4）；分区成员、全局模块片段、`hello::add` 限定写法 | 3.4–4.5 s 首次应答即到 `.cpp`；`.cppm` 声明 ↔ `.cpp` 定义互相切换 | 正常（签名简单，碰巧对得上，见 §2.3） |
| 本仓库冷启动，只开 `src/project/model.cpp` | 15 s 内超时（clangd 在建模块）；21–26 s 起四个函数全部到 `.cpp`，之后 7 分钟稳定 | 正常 |
| A：在打开着的 `math.cpp` 里写实现，保存后关闭 | 2 s 内到 `math.cpp`，关闭后仍正确 | 正常 |
| **B：未打开的 `types.cpp` 在磁盘上加了实现**（git pull、agent、外部编辑） | **一直是声明，125 s 不恢复** | 缺陷 → N-1 |
| C：新建 `extra.cpp` | 约 8 s 后可达（模型重载把它加进数据库） | 可接受 |
| **D：`greet.cpp` 包含不存在的头文件** | `bump` 始终是声明；同模块其他实现单元正常 | 缺陷 → N-3 |
| B 之后让 clangd 临时打开再关闭 `types.cpp` | 2 s 后到 `types.cpp`，80 s 内一直正确 | N-1 修法有效 |
| GalTranslPP（Linux），`ApiPool.cpp` 里的 `wide2Ascii`、`Tool.ixx` 里的声明 | 150 s 内一直为空；`Tool.cpp` 报 `bit7z/bitarchivereader.hpp` 找不到，其余单元同类（`Windows.h`、vcpkg 头文件） | 只能说明 D；Windows 需实测 |
| **mcpp 2026.9.27.1 源码**，只开 `driver.cpp`、`cmd_build.cppm`（冷启动，引擎数据库 507 条） | `phase0_…`→`state.cppm:490`、`phase4b_…`→`state.cppm:495`、`prepare_build`→`prepare.cppm:723`，420 s 不变；分片早已生成（`manifest.cpp.*.idx`、`graph.cpp.*.idx` 都在） | **缺陷 → N-7**；不是"还没轮到" |
| 同上，热启动，再打开 `manifest.cpp` | 立即到 `manifest.cpp:47`；`workspace/symbol` 对 `phase4b_graph_worklist` 返回**两条**：`state.cppm:495` 与 `graph.cpp:61` | 索引里声明与定义是两个符号 |
| 同上，打开 `manifest.cpp`、`graph.cpp` 再关闭 | 5 s 内两处都到 `.cpp`；关闭后 30/60/90/120 s 仍正确 | **N-7 修法有效** |
| 同上，`prepare_build`（主接口 `prepare.cppm` 里导出、带默认参数；定义在一直打开着的 `driver.cpp`） | 从调用处、从声明处都只到 `prepare.cppm:723`（100 s 不变）；`driver.cpp` 0 条诊断；hover 在定义处显示 "provided by prepare.cppm"；`workspace/symbol` 查 `prepare_build` **为空** | **另一个缺陷，O-1**：前台 AST 知道两者是同一个函数，但动态索引没记下这个符号 |
| hello8/9/11/13：分区里声明 `phase_a(State&)`、`phase_p(const Point&)`、`phase_s(const std::string&)`、`Box::f(const std::string&)`，实现单元里定义 | 全部停在声明；同一分区的 `plain_c(int)`、`Box::g(int)`（定义在无显式 import 的文件里）到 `.cpp`；加了 `:state` 分区后，原本正常的 `Point::sum`、`twice` 也开始停在声明 | 结果取决于错误恢复，不稳定 |
| hello13 的 clangd 日志（`--log-level debug`） | 16 个项目单元的后台索引全部 `Failed to compile …, index may be incomplete`；`math.cpp` 0 个符号 | 根因的直接证据 |

B 的根因：clangd 的后台索引只在启动时（按摘要检查过期分片）和编译命令变化时重建文件；`didChangeWatchedFiles` 在
clangd 里只用于编译数据库。mcppls 在 `Workspace::handle_watched_files`（`src/orchestrator/workspace.cpp:1827`）转发了
事件，但 clangd 不会因此重建索引。打开的文件走动态索引，所以"在编辑器里改"是好的，"在别处改"是坏的；agent 改代码越多，
B 越常见。

D 的根因：`#include` 找不到是致命错误，clang 停止解析，该翻译单元不产生符号。

### 2.3 根因：后台索引不为模块单元准备依赖

clangd 23.1.0 的 `BackgroundIndex::index()`（`clang-tools-extra/clangd/index/Background.cpp:254` 起）：
`buildCompilerInvocation(Inputs, IgnoreDiags)` → `prepareCompilerInstance(CI, /*Preamble=*/nullptr, …)` →
`createStaticIndexingAction`。整个过程没有 ModulesBuilder：前台打开文件时 clangd 会先构建该文件导入的模块的 BMI 并把
`-fmodule-file=` 交给编译器，后台索引不做这一步。于是每个模块单元在后台都是"找不到导入的模块"地编译
（`HadErrors = hasUncompilableErrorOccurred()` → 日志 `Failed to compile …, index may be incomplete`）。

后果按 clang 的错误恢复分三种：

- 定义的签名和限定名都不依赖导入的东西（`int plain_c(int)` 这类）：生成的 USR 与声明一致，碰巧能跳；
- 签名里有来自模块的类型、或所属的类来自模块：这些类型成了错误类型，定义生成了另一个 USR，**索引里声明和定义是两个符号**；
- 整个实现单元解析不下去：0 个符号（hello13 的 `math.cpp`）。

所以第 2 版"最小 hello 正常"的结论只对"签名简单、不用分区"的写法成立；拆分区、用自定义类型当参数（mcpp 的 `prepare`、
GalTranslPP 的 `NormalJsonTranslator:TransAgent`、`DictionaryGenerator:ReviewAgent` 都是这种写法）就会稳定地跳到声明。
打开的文件走前台路径（有 ModulesBuilder），它的动态索引是对的，而且实测关闭后仍保留（至少 120 s；clangd 重启后丢失）。

mcppls 自己的 WA-CLANGD-004（module hints）给每条命令加了 `-fmodule-file=<名>=<module-hints>/<名>.pcm`，设计上"路径从不写"，
只为让 clangd 找到提供者。关掉它（`--disable-workaround WA-CLANGD-004`）结果不变，说明它不是原因；但它提供了一个上游修复之外的
思路（N-6 探索）：如果这些路径真的指向 clangd 前台构建出的 BMI（`<cdb>/.cache/clangd/modules/<源>-<hash>/<hash>/<名>.pcm`，
文件名与 hint 同名），后台索引就能加载模块。hello13 上手工链接后 16/17 个单元编译成功、9 个定义全部正确；但在 mcpp 源码上把
271 个 BMI（多轮、多种命令留下的）粗暴链接过去，clangd 12 s 内崩溃——要做只能链接"当前这一代命令"构建出的那一份，且要处理
BMI 过期，风险高，放在上游修复之后再评估。

上游：clangd/clangd#2569（模块内跨文件引用、重命名不可用）是同一类症状，但没有指出原因；N-6 把"后台索引缺 ModulesBuilder"
连同 hello13 的最小复现登记到 #24，并补充到上游。

### 2.4 冷启动：模块项目没有"优先索引"

clangd 23.1.0（`clang-tools-extra/clangd/index/Background.cpp:178`）：

```cpp
void BackgroundIndex::boostRelated(llvm::StringRef Path) {
  if (isHeaderFile(Path))
    Queue.boost(filenameWithoutExtension(Path), IndexBoostedFile);
}
```

`isHeaderFile`（`SourceCode.cpp:1240`）要求扩展名的驱动类型"只有预编译阶段"。`.cppm`/`.ccm` 是 `TY_CXXModule`
（有编译阶段），`.ixx` 在 `clang/lib/Driver/Types.cpp` 里没有登记（`TY_INVALID`），两者都不算头文件。结果：打开 `Tool.ixx`
或导入它的文件，`Tool.cpp` 不会被提前索引，只能等后台队列按顺序轮到。小项目几秒就排完（hello、本仓库都没感觉），
GalTranslPP 这种 227 个单元、每个实现单元都带重型全局模块片段的项目，窗口会长到几分钟，期间 Ctrl+Click 都落在 `.ixx`。
它会放大 §2.3 的问题：本来打开一次就能补上的定义，也要等到用户真的打开那个实现单元。

### 2.5 V-W：GalTranslPP 的 Windows 实测（待同意）

复用 Sunrisepeak/GalTranslPP 的临时 PR #1（`ci/mcppls-issue23-probe`，windows-2025，上次一轮约 40 分钟）：探针脚本
增加 definition 时间线——选 20 个"在 `.ixx` 声明、在 `.cpp` 定义"的调用点，从打开起每 5 s 请求一次，记录首次到 `.cpp`
的时间和一直停在 `.ixx` 的调用点；跑两轮：0.0.5 与带 N-7 的构建。调用点里要包含分区 `NormalJsonTranslator:TransAgent`、
`DictionaryGenerator:ReviewAgent` 里声明的成员。**这会向该仓库的临时分支推送提交并触发 CI，需要你同意。**

### 2.6 方案

**N-7 模块感知的索引补全（P0，替代第 2 版的 N-5、吸收 N-1 与 N-2）。** 既然后台索引建不对模块单元、前台打开能建对且关闭后保留，
mcppls 就替 clangd 把实现单元"过一遍前台"：用已有的后台文档机制（`background_`，prime 单元用的那套）让 clangd `didOpen`
（磁盘文本）→ 等该文件 idle（`textDocument/clangd.fileStatus`，上限 10 s）→ `didClose`。一个队列，三级优先：

| 级别 | 什么进队列 | 何时 |
|---|---|---|
| 按需（原 N-2） | definition 只回答一个位置、落在接口单元（含分区）里、且与 declaration 的回答相同时，该声明所在模块的实现单元 | 请求到来时；限时 1.5 s 后重问一次，仍是声明就原样返回（有 N-3 标记时附原因） |
| 相关（原 N-5、N-1） | 编辑器打开的文件所在模块、及它直接导入的模块的实现单元（不递归，单次最多 16 个）；watch 到磁盘变化、编辑器没打开、摘要确实变了的源文件 | 打开文件、磁盘变化时 |
| 其余 | 数据库里其余的模块实现单元（`module X;`、实现分区） | clangd 空闲、没有前台请求时，逐个 |

- 并发最多 2；已经过一遍且摘要未变的跳过；解析失败（致命错误）的记给 N-3，不重试直到它的输入变化。
- clangd 重启（计划、恢复、崩溃）后动态索引丢失，"相关"级立即重做，"其余"级在空闲时重做。
- 单次 watch 事件超过 20 个文件（git checkout、rebase；D3）时，这些文件只进"其余"级，不插队。
- 成本：每个单元一次前台 AST 构建（mcpp 源码上两个实现单元一起 5 s）。"其余"级只针对实现单元：mcpp 源码的引擎数据库
  507 条里，接口单元 181、实现单元 16（`prepare` 拆出来的）、导入模块的普通单元 310，所以要补的只有 16 个；实现单元多的项目
  （GalTranslPP 每个 `.ixx` 配一个 `.cpp`）按 V-W 的实测定并发与上限。
- 范围：N-7 修的是**跳到定义**。同一根因也让 310 个导入模块的普通单元在后台索引里残缺，跨文件的 Find References、
  Call Hierarchy、Rename 只能看到打开过的文件（clangd/clangd#2569 的症状）。把这些单元也过一遍前台代价大，不在 N-7 内，
  留给 N-6（上游修复或 BMI 方案）。
- 不改编译命令去"骗" clangd 重建（会重建 BMI）；不为了索引重启 clangd。
- 上游修好（N-6）或 N-6 的 BMI 方案成熟后，"其余"级可以关掉，按需与相关两级仍保留（它们也解决 `.ixx`/`.cppm` 不被优先索引的问题）。

**N-3 解析不了的实现单元进状态（P1）。** clangd 报出的致命错误（`fatal error: … file not found`、模块扫描失败）落在实现单元上时，
记为 `implementation-unreadable`：状态栏写"1 个实现单元无法读取：`greet.cpp`（`ui_generated.h` 找不到），其中的定义不可跳转"；
是生成物时与 Q1-3 合并。

**文档。** 用户文档加一段约定：Ctrl+Click / F12 到实现，Go to Declaration 到接口，Peek 同时看两边；"全 `.cppm`"写法的项目，
定义本来就在接口里。

## 3. 问题 3：构建工具无感探测的抽象层

### 3.1 现状与差距

| 现状（0.0.5） | 位置 | 差距 |
|---|---|---|
| `detect_project` 固定顺序：配置的数据库 > `mcpp.toml` > `CMakeLists.txt` > `compile_commands.json` > inferred | `src/project/detect.cpp` | 每加一个工具要改多处 switch；不认 xmake、meson |
| mcpp：离线 emit（`MCPP_OFFLINE=1`），需要下载时只有 "Run in Terminal" | `src/project/mcpp.cpp:327`，`src/orchestrator/workspace.cpp:1524` | 没有"同意后由 mcppls 后台联网"；联网只能把 `mcppls.buildTool` 设成 `online`（永久、全局） |
| CMake：有构建目录就读；否则在私有目录配置，首次允许下载（BD7） | `src/project/cmake.cpp:163` | 与 D1 冲突；不读 `CMakePresets.json`，私有配置可能选到和用户不同的编译器 |
| 没有模型时等 producer 10 s（BD5） | `src/orchestrator/workspace.cpp` | D5：2.5 s |
| 离线只靠 `MCPP_OFFLINE` 这个约定的环境变量 | `modules/platform/src/toolrun.cpp:81` | 每个工具需要自己的离线配方 |
| 无感探测没有总开关；`mcppls.buildTool = off` 只管"不执行"，已有产物照读 | — | B-7 |

### 3.2 抽象：`BuildSystemProvider`

```cpp
// 只看文件、不执行：< 50 ms。多个提供者都认领时取 confidence 最高的（mcpp.toml 与 CMakeLists.txt 并存等）。
struct Claim { std::string provider; int confidence; std::string manifest; std::vector<std::string> markers; };

enum class Outcome { ok, partial, needs_download, failed, timed_out };
struct Answer {
    Outcome outcome;
    std::optional<spec::Database> database;    // ok / partial
    std::vector<std::string> missing;          // needs_download：缺什么（包名、FetchContent 名）
    std::string terminalCommand;               // 用户在终端里自己跑的等价命令
    std::string reason;                        // failed：构建工具自己的诊断
};

class BuildSystemProvider {
public:
    virtual std::string_view id() const = 0;                                   // "mcpp" "cmake" "xmake" "meson" "compile-commands"
    virtual std::optional<Claim> detect(std::string_view root) const = 0;      // 纯文件系统
    virtual std::optional<Answer> existing(const Claim&) const = 0;            // 读已有产物，不执行
    virtual Answer describe(const Claim&, const DescribeContext&) const = 0;   // 执行构建工具；context 说明是否离线
    virtual std::vector<std::string> watch_inputs(const Claim&) const = 0;     // 触发重新描述的文件
    virtual std::vector<std::string> fingerprint(const Claim&) const = 0;      // 模型缓存的输入
};
```

`DescribeContext` 沿用 `ProviderContext`（trusted、runner、offline、soft/hard 时限、`onSlow`），另加 `privateDirectory`
（每个工作区、每个提供者一份，在 mcppls 缓存目录下）。inferred 不是提供者，是所有提供者都不可用时的兜底。

**副作用契约（每个提供者都要满足，conformance 的 `workspace-unchanged` 覆盖全部）：**

1. 不写工作区，所有状态在 `privateDirectory`。
2. 离线配方由提供者给出（环境变量、参数），`toolrun` 统一记录 offline、网络观察、耗时、stderr 尾部。
3. 失败分类：`needs_download`（可以询问）、`failed`（构建脚本错，报构建工具原话）、`timed_out`。
4. 不交互：离线阶段绝不从标准输入读确认。

### 3.3 各工具的实现要点

| 提供者 | detect | existing（不执行） | describe 离线 | 同意后联网 | 实测 |
|---|---|---|---|---|---|
| **mcpp** | `mcpp.toml` | — | `mcpp emit build-database --format json`，`MCPP_OFFLINE=1` | 同一命令去掉离线 | qt-demo：2026.9.26.2 0.6 s，2026.9.27.1 0.25 s |
| **CMake** | `CMakeLists.txt`（+ `CMakePresets.json`） | `build*/`、`out/build/*`、`cmake-build-*`、preset 的 `binaryDir` 里的 `build_database.json` / `compile_commands.json` | 私有目录配置，`-DFETCHCONTENT_FULLY_DISCONNECTED=ON`；有 preset 时跟随其 generator、toolchainFile、cacheVariables | 私有目录去掉 `FULLY_DISCONNECTED` 重新配置，之后回到离线 | 缺 fmt：5.5 s 失败，消息 `FETCHCONTENT_FULLY_DISCONNECTED … requires the source directory … populated`，项目目录无变化 |
| **xmake** | `xmake.lua` | 项目根或 `.vscode/` 下的 `compile_commands.json` | 见 3.4 | 去掉 `fetch_only` 与 `network.mode:private`，加 `-y` | 见 3.4 |
| **meson** | `meson.build` | `builddir/`、`build/` 下的 `compile_commands.json` | 私有目录 `meson setup --wrap-mode=nodownload` | 去掉 `nodownload` | 未测 |
| **compile-commands** | `compile_commands.json` / `build/compile_commands.json` | 就是它 | — | — | — |

CMake 跟随 preset 很重要：私有配置不跟随 preset 时，可能选到另一个编译器，给出的语义和用户实际构建的对不上。

### 3.4 mcpp 2026.9.27.1 对照

| mcpp 的事实 | 来源 | 对本方案的影响 |
|---|---|---|
| emit 不写工程、在 `$MCPP_HOME/cache/build-database/<key>` 规划；运行构建程序，但不运行 action；不构建宿主工具，库中没有的记 note `MCPP_BUILD_DATABASE_HOST_TOOL_DEFERRED`（#707） | mcpp `docs/specs/build-database.md` R2.1–R2.5 | 副作用契约对 mcpp 天然成立；生成物缺失是常态而非异常（Q1-3、M-2）；被推迟的宿主工具产生的文件也属于"生成物缺失"，B-8 把这条 note 并进 `generated-files-missing` 的说明 |
| 信封声明副作用：`effects` = `read-project`、`write-global-cache`、`exec-build-script`（`--protocol-version` 同样声明，R2.6） | qt-demo 实测信封 | 提供者契约直接读它：出现 `write-project` 视为违约（记事故、不用该结果）；`exec-build-script` 表示会运行项目自己的 `build.mcpp`，只在受信任工作区执行（与现状一致） |
| 缺依赖的稳定代码 `MCPP_OFFLINE_DOWNLOAD_REQUIRED`（e2e 735） | mcpp 测试 | mcppls 已按代码识别（`src/spec/discovery.cpp:91`），旧版本的文字匹配保留作兼容 |
| 按成员规划，失败成员给 `error` 诊断、其余成员照常给 `data`（2026.9.26.2 起，#699） | CHANGELOG | **缺口 B-8**：mcppls 只在"整份失败"时识别需要下载；部分回答里某成员的错误是 `MCPP_OFFLINE_DOWNLOAD_REQUIRED` 时只报 `producer-partial`（`src/project/mcpp.cpp:312`），不会询问。B-8：用已规划的部分，同时按 §3.6 询问 |
| 工作空间成员继承根的 `[xlings.workspace]`（#713、#714） | CHANGELOG | GalTranslPP 这种工作空间，成员现在也会因为根声明的包未安装而需要下载，走部分回答 + B-8 |
| 离线时"记录为已安装但载荷已删除"的包被拒绝并点名（#712、#716） | CHANGELOG | 同属 `needs_download`；询问文字用 mcpp 自己的消息 |

### 3.5 xmake：专门的 CDB 命令不编译，但仍需两处隔离

`xmake project -k compile_commands <outdir>` 是 xmake 专门的编译数据库生成命令，**不编译**。实测它做两件事：
隐式配置（工具链探测、flag 检查，冷 5.4 s、热 3.9 s）和 **C++ 模块依赖扫描**（对每个模块文件调用 g++ 做扫描，结果写进
`build/.gens/…/rules/bmi/cache/scans/*.json` 和 `build/.deps/…/*.d`）。所以：

1. **不加隔离会写项目**：只跑 `xmake project -k compile_commands` 时，项目里多出 `build/`（`.gens`、`.deps`，已实测）；
   不设 `XMAKE_CONFIGDIR` 时配置写进项目的 `.xmake/`（xmake 的默认行为，本次实测都设了私有目录）。
   `xmake project` 没有 builddir 参数，builddir 只能由 `xmake f` 设定，所以首次需要两步：
   ```
   XMAKE_CONFIGDIR=<private>/config xmake f -c --confirm=no \
       --policies=package.fetch_only,network.mode:private --builddir=<private>/build
   XMAKE_CONFIGDIR=<private>/config xmake project -k compile_commands <private>/out
   ```
   之后只跑第二条（配置已缓存在私有目录）。实测两步 5–7 s、只跑第二条 3.9 s，项目目录无变化。
2. **离线要显式关网络**：只加 `package.fetch_only` 时，本机仓库未拉取过，xmake 先 `updating repositories`（联网 11 s、写
   `~/.xmake`）。xmake 源码（`modules/private/action/require/impl/repository.lua` 的 `pulled()`）在
   `network.mode` 策略为 `private` 时跳过仓库更新；加上 `network.mode:private` 后实测不再联网，缺包时 5.8 s 内报
   `The packages(xxhash) not found`，归为 `needs_download`。
3. 生成的命令带 `-fmodule-mapper=/tmp/.xmake…/*.mapper.txt`（gcc），这些是 BMI 参数，mcppls 的规范化（P3）本就会去掉；
   模块角色由扫描恢复，层级为 L3。

耗时超过 2.5 s 的时限属于正常，按 D5 先上 L4、拿到后切换。

### 3.6 状态机与交互

```
打开工作区
  │ buildDiscovery = off？ → 只用 mcppls.database（若配置），否则 L4；结束
  │ 各提供者 detect（纯文件，< 50 ms），按 buildDiscovery.providers 过滤
  ├─ 有模型缓存 → 立即用（BD4），后台离线确认
  ├─ existing 有产物 → 立即用
  └─ 离线 describe，最多等 2.5 s（D5）
        ├─ 在时限内成功 → 用它
        ├─ 超时限 → L4 先上（状态栏"正在读取构建描述（cmake，4 s）"），继续等到 hard 上限，成功后切换
        ├─ failed → L4 + 构建工具原话（现有 model-fallback）
        ├─ needs_download → L4 + 询问（askBeforeDownload = false 时只进状态栏）
        └─ partial 且其中有成员 needs_download → 用已规划的部分 + 询问（B-8）

询问（非模态通知；每个工作区只问一次，除非缺的东西变了）：
  "qt-demo 需要下载依赖才能拿到完整的构建信息（xim:qt-base@6.11.1）。现在先按源码扫描提供基础功能。"
  [下载并继续]  [在终端运行]  [不再询问]
     ├─ 下载并继续 → 后台联网 describe（Network::allowed，hard 10 min，workDoneProgress，可取消）
     │     ├─ 成功 → 一次计划内 clangd 重启（不计入重启预算，RD6），打开的文档保持；之后回到离线
     │     └─ 失败/取消 → 保持 L4，issue 是构建工具原话，按钮"在终端运行"
     ├─ 在终端运行 → 现有 mcppls.runBuildToolInTerminal；watch 到输入变化后离线重试
     └─ 不再询问 → 记在工作区状态；状态栏仍显示 producer-needs-download
```

要点：

- **同意是一次性的、只对这个工作区（D2）**；不改 `mcppls.buildTool`。
- **L4 的质量**：qt-demo 这种依赖大型 SDK 的项目，L4 拿不到 Qt 头文件，"临时可用"只对模块本身成立；M-3（离线时返回部分数据库）
  落地后，等待期间就有已安装依赖的全部语义（mcppls 已支持 `producer-partial`）。
- **RD1 的例外写明**：RD1 是为"producer 慢但会成功"设的。按 D5，超过 2.5 s 就 L4 先上，拿到构建模型后切换一次；
  离线失败或需要下载时同样 L4 先上。RD1 改写为"构建模型到达后只切换一次，切换不计入重启预算"。
- **CMake 联网获取的代价**：FetchContent 下载到私有目录，与用户自己的构建目录各下一份；在询问文字里写明。

### 3.7 开关（B-7）

| 设置 / 参数 | 取值 | 作用 |
|---|---|---|
| `mcppls.buildDiscovery` / `--build-discovery` | `auto`（默认）、`off` | `off`：不探测构建系统、不读已有产物、不执行、不询问；只用显式配置的 `mcppls.database`，否则 L4 |
| `mcppls.buildDiscovery.providers` | 数组，默认 `["mcpp", "cmake", "xmake", "meson", "compile-commands"]` | 从中去掉某个提供者即停用它（例如只想用 CMake 已有的构建目录、不要 xmake） |
| `mcppls.buildDiscovery.askBeforeDownload` | `true`（默认）、`false` | `false`：需要下载时只在状态栏说明，不弹询问 |
| `mcppls.buildTool`（已有） | `offline`（默认）、`online`、`off` | 不变：管"构建工具怎么执行"。`off` 仍会探测、仍读已有产物，只是不执行；与 `buildDiscovery = off` 的区别写进设置说明 |

不受信任的工作区等同 `buildDiscovery = off`（与现在一致）。设置变化时重新加载模型（`extension.ts` 已对 `mcppls.buildTool`
这样处理）。服务端参数、VS Code 设置、Zed/CLion 的 initializationOptions 三处同步；`cxxModules/status` 的 `project`
字段报告当前取值（S3 需加字段，见 §7 R6）。

### 3.8 编辑器插件侧

- VS Code：`cxxModules/status` 里出现 `producer-needs-download` 且带 `askOnline: true` 时弹通知（每工作区一次），按钮调用
  服务端命令 `mcppls.describeOnline`（参数为工作区根）。进度走 LSP 的 `window/workDoneProgress`，不依赖插件。
- Zed、CLion：没有通知按钮时，issue 文本里给出等价命令；`mcppls.describeOnline` 也能从命令面板触发。

## 4. 与现有设计的关系

| 设计条目 | 变化 |
|---|---|
| BD5 | 10 s → 2.5 s（D5） |
| BD7 | 删除：CMake 首次配置也离线（D1） |
| RD1 | 改写为"构建模型到达后只切换一次，不计入重启预算" |
| P1（故障只影响它所在的地方） | Q1-1/Q1-4 修掉一个违反点 |
| 设计 2.1 "生成模块去构建留下的地方找" | 扩展到生成的头文件目录和源文件（Q1-3），长期保留 |
| 设计 2（引擎抽象：clangd 负责 C++ 语义） | 新增：mcppls 负责让 clangd 的索引覆盖模块实现单元（N-7），直到上游后台索引支持模块 |
| WA-CLANGD-004（module hints） | 不变；登记"hint 路径指向真实 BMI"作为 N-6 的探索方向 |
| 新增 | `BuildSystemProvider` 与副作用契约；`mcppls.buildDiscovery` |

## 5. 验证计划

| 夹具 / 检查 | 覆盖 | 类型 |
|---|---|---|
| `mcpp-rules-generated`（新） | mock mcpp 给出 M-1/M-2 形状的 S1：非 C++ 条目剔除且计入 left out；生成物目录缺失时 `generated-files-missing`；项目 `target/` 有生成物时被只读使用、`main.cpp` 零诊断；不回退 kit；工作区不变 | conformance（mock） |
| `std-generation-race`（新） | 数据库加入 std 单元后到 clangd 重读前报的 `unresolved` 不触发 `std-fallback-kit` | conformance（mock） |
| `mcpp-partition-definition`（新，N-7） | hello13 的形状：分区里声明、签名含类类型的函数与成员，定义在实现单元；只打开调用方，definition 在 10 s 内到 `.cpp`（修前停在声明）；clangd 重启后同样 | conformance（真 mcpp） |
| `mcpp-split` 加三项 | B：未打开的实现单元在磁盘上加定义，10 s 内到 `.cpp`；C：新建实现单元；D：解析不了的单元给 `implementation-unreadable` | conformance（真 mcpp） |
| `mcpp-emit-partial-download`（新，B-8） | mock mcpp 的部分回答里一个成员报 `MCPP_OFFLINE_DOWNLOAD_REQUIRED`：其余成员被使用，状态带询问（`askOnline`） | conformance（mock） |
| mcpp 源码 | 实机：`driver.cpp` 的 `phase0_…`、`phase4b_…` 与 `cmd_build.cppm` 的 `prepare_build` 冷启动后到 `.cpp` | 手工 |
| `cmake-fetchcontent-offline`（新） | 首次配置离线、缺依赖 → `producer-needs-download`，工作区不变；`mcppls.describeOnline` 在私有目录完成（CI 用本地 git 仓库代替网络） | conformance |
| `xmake-modules`（新，B-5） | 私有配置目录与 builddir、工作区不变、`network.mode:private` 下缺包归为 `needs_download` | conformance |
| `build-discovery-off`（新，B-7） | `off` 时不执行任何程序、不读已有 `compile_commands.json`、状态为 L4 | conformance |
| qt-demo 实机 | 修后：`main.cpp` 零诊断（已构建过时），或只有一条 `generated-files-missing`（未构建时）；没有 `std-fallback-kit`；没有非 C++ 条目的日志 | 手工 |
| V-W | GalTranslPP Windows：修前 / 修后首次到 `.cpp` 的时间与停在 `.ixx` 的调用点数 | CI（待同意） |

## 6. 待决策

| # | 问题 | 建议 |
|---|---|---|
| D3 | N-1 批量变化的阈值 | 单次 > 20 个文件时改为空闲时计划重启（先实测重启后的分片重建） |
| D4′ | kit 回退的范围（修正版） | 只在 std 单元自身编译失败时整体切换，同时移除工具链 std 单元，提供"重试工具链的 std"；不做按文件回退 |
| D6 | Q1-3 读取项目 `target/` 里的生成物 | 只在受信任的工作区、只读 |
| D7 | xmake / meson 优先级 | xmake 提到 P1（离线配方已验证）；meson P3 |
| D8 | V-W 是否推送到 GalTranslPP 的临时分支跑 Windows CI | 建议跑，结果决定 N-7 的并发与上限 |
| D9 | N-7 的"其余"级（空闲时把所有实现单元过一遍前台）是否默认开启 | 开启；实现单元多时由 V-W 定上限；提供 `mcppls.index.primeImplementationUnits`（`auto`/`off`）关掉 |
| D10 | N-6 的 BMI 方案是否在 0.0.7 做 | 不做；先登记上游，等上游态度与 N-7 的实际开销再定 |

待查：

| # | 问题 | 下一步 |
|---|---|---|
| O-1 | mcpp 的 `prepare_build`：主接口导出、默认参数、定义在实现单元，定义文件打开时索引里也没有这个符号（`workspace/symbol` 为空），跳转只到声明 | 缩成最小复现（主接口导出函数 + 实现单元定义 + 实现单元 `import :partition`）；看 clangd 的 SymbolCollector 对"规范声明来自 BMI"的符号是否跳过；结果并入 N-6 的上游登记。0.0.6 内完成定位，修法另定 |

## 7. 自我 review

| # | 问题 | 处理 |
|---|---|---|
| R1 | **第 1 版的 D4"tier 1/2 只按文件回退"做不到。** 一个 clangd 的数据库里，模块名到提供者只能有一个映射；让一部分文件用 kit 的 `std`、另一部分用 gcc 的 `std`，就要同时放两个 `std` 提供者，clangd 只会取其一，另一半文件静默地用错 std | 改为 D4′：整体切换只在确认 std 自身编译失败时发生，并移除工具链 std 单元。**需要你再确认** |
| R2 | 第 1 版把 Q1-3 放在 P1，但 16:29 的会话证明它是 qt-demo 唯一持续的问题 | 升为 P0，并做了端到端验证 |
| R3 | Q1-3 依赖 mcpp 私有目录与 `target/` 的路径对应，属于耦合 mcpp 的实现细节 | 写明为临时对策；对应不上时只报 issue、不猜；M-2 落地后按 S1 声明处理 |
| R4 | N-7 的临时打开会让 clangd 为该单元构建模块依赖，大项目里每个几秒 CPU | 并发上限 2、相关级单次上限 16、已过一遍且未变化的跳过；V-W 用实测数据定上限 |
| R5 | 第 1 版的 xmake 结论（"私有 XMAKE_CONFIGDIR + --builddir"）没说清楚为什么需要配置步骤，且离线配方有漏洞（仍联网更新仓库） | §3.4 重写：CDB 命令本身不编译，隔离是因为模块扫描写 builddir；离线配方补上 `network.mode:private` 并验证 |
| R6 | B-2 的 `askOnline`、B-7 的新设置会改变 S3（`cxxModules/status` 的 issue 与 `project` 字段） | 按规范流程改 S3、更新 `conformance/traceability.json`，和实现同一个 PR |
| R7 | D5（2.5 s 后 L4 先上）意味着慢一点的 producer（2.5–60 s）会多一次切换重启，L4 期间的索引白做 | 接受（用户已定）；切换不计入重启预算；有模型缓存时不受影响（缓存优先） |
| R8 | `buildDiscovery = off` 与 `buildTool = off` 容易混淆 | 设置说明里并列写出两者的差别；`off` 优先 |
| R9 | 第 1 版把 `windows.h` 推测为 std 误判导致 | 撤回推测：Linux 三次会话均无；GalTranslPP 这类 Windows 项目在 Linux 上缺 `Windows.h` 属预期 |
| R10 | N-2 依赖"definition 与 declaration 相同"来判断"只知道声明"，会多一次 declaration 请求 | 只在 definition 结果落在接口单元时才发这一次；有 N-7 的相关级之后命中率应该很低，保留作为兜底 |
| R11 | **第 2 版对问题 2 的判断错了。** 它说"路由正确、hello 正常，问题只在索引没轮到"，实际是后台索引对模块单元建错；hello 正常是因为签名简单、碰巧对得上（只开 `main.cpp` 的 hello7 也全对，所以不是探测方式的问题；但第 2 版有几组探测同时打开了实现文件，那几组本来就不能说明后台索引） | §2.3 重写，N-7 取代 N-5；新增按"分区 + 类类型参数"的最小复现与夹具 |
| R12 | 第 3 版一度用"直接跑 clangd、不经 mcppls"做对照，想证明是纯上游问题；但那组对照里连 `Point::sum`、`twice` 也停在声明（去掉 BMI 参数后后台索引同样失败，且更糟），不是干净对照 | 不用它下结论；根因以 clangd 源码 + mcppls 下的 debug 日志为准 |
| R13 | 第 2 版向 mcpp 要"emit 生成出生成物"（M-2），与 mcpp 规范 R2.1/R2.5（不写工程、不运行 action）冲突，mcpp 不会接受 | M-2 改为"在 S1 里标明生成物"；Q1-3 不再是临时对策，长期保留 |
| R14 | 部分回答 + 需要下载的组合没有覆盖（B-8）；mcppls 丢弃 note 级诊断，`HOST_TOOL_DEFERRED` 看不到 | 新增 B-8 |
| R15 | N-7 靠"关闭后动态索引仍保留"这一观察（最长验证 120 s），不是 clangd 的承诺 | 夹具里检查关闭后 10 分钟仍正确；clangd 重启后重做；若某版 clangd 关闭即丢，改为保持打开（占内存，设上限） |
| R16 | N-7 只修跳到定义；Find References、Call Hierarchy、Rename 同样受后台索引缺陷影响 | 写明范围（§2.6），留给 N-6；状态或文档里说明"跨文件引用只覆盖打开过的文件" |
| R17 | 本版开始时 mcpp 2026.9.27.1 尚未发布，复测用的是本机从 b439fd97 构建的二进制；会话后段本机的 `mcpp` 已是 2026.9.27.1，fresh 副本上的复测用的是它 | 结论不变；发布版上的 mcpp 源码跳转实测留到 V-W 一起重跑 |
| R19 | 复测时在 qt-demo 原项目里跑了 2026.9.27.1 的 emit，它改写了 `qt-demo/.mcpp/.xlings.json`（mtime 17:28），而当时只比对了项目根目录的列表，没发现 | 已告知；这也是 mcpp 的缺陷（emit 写工程，违反 R2.1），列在 mcpp#724 的附带发现里。之后的实测只在副本上跑 |
| R18 | 第 3 版一开始把 N-7 写成"在 mcpp 源码上验证有效"，实际三个探测点只验证了两个，第三个（`prepare_build`）另有原因 | 改为"三处中的两处"，新增 O-1；N-7 的夹具只断言已验证的形状 |

## 8. 附：复现

- qt-demo：在项目目录 `mcppls --payload <payload> check src/main.cpp`；`mcpp emit build-database` 看 M-1/M-2 的原始输出；
  修法验证：emit 输出中私有目录替换为 `<副本>/target/.build-mcpp`、去掉 `.ui/.qrc/.ts` 后 `mcppls --database db.json check src/main.cpp`。
- 跳转：hello 系列在会话临时目录，用自写 LSP 客户端对 `mcppls serve` 每秒请求一次 definition；B = 启动后改写未打开的
  `types.cpp` 并发 `didChangeWatchedFiles`；D = 在 `greet.cpp` 的全局模块片段里 `#include "ui_generated.h"`。N-4 会把它们
  固化进 `mcpp-split` 夹具。
- clangd 源码：`llvm-project-23.1.0.src.tar.xz`（`.payload-cache/`）中的 `clangd/index/Background.cpp:178`、
  `clangd/SourceCode.cpp:1240`、`clang/lib/Driver/Types.cpp`。
- CMake：`FetchContent_Declare(fmt …)` + `-G Ninja -DFETCHCONTENT_FULLY_DISCONNECTED=ON`，私有构建目录。
- xmake：见 §3.4 的两条命令；对照组为不加 `network.mode:private`（出现 `updating repositories`）和不加 `--builddir`（出现项目 `build/`）。
- 后台索引根因：hello13 = hello2 + 分区 `:state`（`struct State`、`int phase_a(State&)`、`int phase_p(const Point&)`、
  `std::string phase_s(const std::string&)`、`struct Box { int f(const std::string&); int g(int); }`），定义分在
  `phase_a.cpp`、`phase_b.cpp`、`box.cpp`；只打开 `main.cpp`、`driver.cpp`，`mcppls --log-level debug` 的日志里看
  `Failed to compile …, index may be incomplete`。
- mcpp 源码：`b439fd97` 检出到临时目录、`mcpp build`，`mcppls serve --mcpp <新 mcpp>`，只打开 `src/build/prepare/driver.cpp`
  与 `src/cli/cmd_build.cppm`；再临时打开 / 关闭 `manifest.cpp`、`graph.cpp` 验证 N-7。
- clangd 源码：`clangd/index/Background.cpp:254` 起的 `BackgroundIndex::index()`（无 ModulesBuilder）。

## 9. 0.0.6 实施计划（单 PR）

决定（2026-09-27）：D1、D2、D4′、D5、D8、D9 同意；D10 改为"上游缺陷凡是 mcppls 侧能做的，直接在 mcppls 侧实现"（混合引擎：
mcppls 自己的引擎做一部分，clangd 做一部分），同时登记上游。另加一项：**把 mcppls 的全部可配置项收拢成一个配置模块**，并配一章文档。
全部进 0.0.6，一个 PR。

### 9.1 实施时的新发现（改变了第 3 版的做法）

- mcppls **已有**"按需找定义"（`search_definition_`，`src/engine/clangd.cpp`）：clangd 只给出接口里的声明时，打开该模块的其他单元再问一次。
  它在 mcpp 源码上失败，是因为只打开 4 个单元（`UNITS_PER_SEARCH`），且只按文件名 stem 和目录排序，不看名字定义在哪：`phase0_…`
  声明在 `state.cppm`，模块 `mcpp.build.prepare` 有 17 个其他单元，挑中的 4 个里没有 `manifest.cpp`。所以 N-8 不是另起一套，
  而是让选单元这一步**按名字**（先词法扫描哪些单元里有这个名字的定义），并在 clangd 仍对不上时（O-1）直接给出词法找到的位置。
- mcpp 2026.9.27.1 构建不了本仓库（mcpp#725，已提）：CI 固定 2026.9.26.1，不受影响；nightly 会先遇到。本 PR 不改清单，等 mcpp 修复；
  在 #24 登记 UP-M。

### 9.2 用户体验的硬规则（用户要求，2026-09-27）

1. **插件侧的提示一律非阻塞**：VS Code 右下角的通知，用户可以一直不点；不用模态框，不在激活、启动或任何请求的路径上 `await` 它。
2. **提示不影响 mcppls 的后续功能**：弹出询问时，L4 已经在工作；用户点不点、什么时候点，都只影响"是否联网获取"这一件事。
3. **自动分级**：L4 → L3 → L1/L2 的升级都是自动的，不需要用户操作；更好的模型到达时计划内切换一次（不计入重启预算）。
4. **环境自己变好也要接得住**：用户在终端里自己跑了构建、装好了依赖（`mcpp build`、`xlings install`、`cmake`），mcppls 通过监视构建输入与
   后台退避重试（需要下载时 30 s、1 min、2 min、5 min，之后每 5 min；监视到的输入变化立即重试）发现环境完善，自动升级；
   此时尚未点的询问失效（插件收到新状态后撤回或忽略），不再重复弹出。
5. 同一件事每个工作区只问一次（D2）；"不再询问"记在工作区状态里；缺的东西变了才会再问。

### 9.3 任务与依赖

| # | 任务 | 依赖 | 角度 |
|---|---|---|---|
| T0 | 分支 `release/0.0.6`；本计划 | — | — |
| **T1 配置模块** `src/config/settings.cppm`（`mcppls.config.settings`）：每个设置一行——键、类型、取值、默认、命令行拼写、所在位置（服务端 / 客户端）、生效方式（重载模型 / 重启）、起始版本、说明、旧名（升级映射）；解析 命令行 > initializationOptions > 默认，未知取值回落默认并记为问题；`mcppls settings [--format markdown\|json]` 输出参考表；`report` 带 `settings`（生效值、来源、问题） | T0 | 架构、一致性、无感升级 |
| T1a | 命令行全局选项、`session_options`、`handle_initialize_` 都改为从 T1 读；`workspace/didChangeConfiguration` 对"重载模型"类设置直接生效 | T1 | 架构、优雅 |
| T1b | 文档 `docs/30-settings.md`（及 zh-CN）的参考表由 `mcppls settings --format markdown` 生成；单元测试比对注册表与文档、VS Code `package.json` 三者一致 | T1 | 一致性 |
| T2 | Q1-2 剔除非 C/C++ 条目；Q1-3 生成物目录（项目 `target/` 只读替换，`generated-files-missing`，监视生成物出现） | T0 | 稳定性、体验 |
| T3 | Q1-1 + Q1-4：std 回退只认 std 单元自身编译失败；数据库代际；回退时移除工具链 std 单元 | T0 | 稳定性 |
| T4 | N-8：选单元按名字（词法定义扫描，含分区与实现分区）；clangd 仍只给声明时返回词法定位；`UNITS_PER_SEARCH` 以名字命中为准 | T0 | 体验、架构（混合引擎） |
| T5 | N-7：相关级（打开文件时，其模块与直接导入模块的实现单元）、磁盘变化级（未打开且摘要变了）、其余级（空闲时）；设置 `index.primeImplementationUnits` | T4、T1 | 体验、稳定性（并发与上限） |
| T6 | N-3：`implementation-unreadable` | T5 | 体验 |
| T7 | B-1：`BuildSystemProvider` 注册表；mcpp / CMake / compile-commands 平移 | T0 | 架构 |
| T8 | B-3 CMake：首次配置离线（`FETCHCONTENT_FULLY_DISCONNECTED`）、预设（`CMakePresets.json` 的 `binaryDir` 与配置）；B-5 xmake；B-6 meson | T7 | 跨平台、兼容性 |
| T9 | B-2 + B-8 + B-4：需要下载时询问（状态里 `askOnline`、服务端命令 `mcppls.describeOnline`、一次性联网）、部分回答 + 需要下载、首个模型等 2.5 s、需要下载时退避重试 | T7、T1 | 体验（§9.2） |
| T10 | B-7：`buildDiscovery`、`buildDiscovery.providers`、`buildDiscovery.askBeforeDownload` | T1、T7 | 体验 |
| T11 | VS Code：非阻塞询问、"不再询问"、新设置；Zed/CLion 的 initializationOptions 与文档 | T1、T9 | 体验、跨平台 |
| T12 | 规范：S3（issue 的 `askOnline`、`project.settings`、新 issue 码）+ schema/traceability；设计记录（BD5、BD7、RD1、新决定）；`docs/20-projects.md`；CHANGELOG；#24（UP-M：mcpp#724、#725；UP：后台索引与模块） | 全部 | 一致性 |
| T13 | 验证：单元测试；夹具 `mcpp-rules-generated`、`std-generation-race`、`mcpp-partition-definition`、`mcpp-split`（磁盘改动）、`mcpp-emit-partial-download`、`cmake-fetchcontent-offline`、`build-discovery-off`；xmake 夹具（Linux）；三平台 CI | 全部 | 稳定性、跨平台 |
| T14 | 版本 0.0.6、PR、CI 全绿、自我 review、squash 合入、Release、本地验证、交付目录 | T13 | — |

并行：T1、T2+T3、T4、T7+T8 互不依赖，可同时进行；T5/T6 在 T4 之后，T9/T10/T11 在 T1 与 T7 之后。

### 9.4 各角度的检查项

| 角度 | 要求 |
|---|---|
| 架构 | 配置只有一个来源（T1）；构建工具只经 `BuildSystemProvider`；上游缺陷的补偿都是登记过的 WA 或混合引擎的一部分 |
| 稳定性 | 新的后台打开都走现有 `background_` 的上限与超时；不为索引重启 clangd；代际检查避免竞态误判 |
| 优雅简洁 | 复用 `search_definition_`、`background_`、`toolrun`、现有 issue 通道；不引入新进程 |
| 用户体验 | §9.2 五条；状态栏说清楚层级与原因 |
| 兼容性 | 旧设置名继续被接受（注册表的旧名映射）；旧客户端不认识 `askOnline` 也能工作；mcpp 旧版本的文字识别保留 |
| 跨平台 | 新代码不含平台分支（平台差异只在 `modules/os`）；xmake/meson/CMake 的参数在 Windows 上同样成立；三平台 CI |
| 一致性 | 设置表、文档、`package.json` 由测试互相校验；S3 规则有 traceability |
| 无感升级 | 模型缓存格式不变；设置默认值保持旧行为（除 D1、D5 这两处已定的变化）；升级后首次启动不需要用户操作 |
