# Issue #23 分析：Windows/MSVC + LTO 下 clangd 模块扫描失败、clangd 反复退出

状态：分析完成（未改代码）· 2026-09-26 · `main` 04b186a（0.0.4）· 捆绑 clangd 23.1.0（clangd/clangd 官方 Windows
发布版）· issue：<https://github.com/Sunrisepeak/mcpp-language-server/issues/23>

证据来源：

- 本机（Linux x64）：Clang 22 驱动、`clang-scan-deps`、捆绑 clangd 23.1.0，对照 llvm-project 23.1.0 源码。
- **Windows 实测**：Sunrisepeak/GalTranslPP 临时 PR #1 的 workflow（windows-2025，VS 18 / MSVC 14.51，mcpp 2026.9.25.1，
  LLVM 22.1.8，Qt 6.11.1，vcpkg 22 个 port），跑 mcppls 0.0.4 的 win32-x64 payload。第 4 轮（run 36178100611）复现了
  报告者的环境：mcpp producer 成功，引擎数据库 **227 条、227 条带 `-flto`、0 条带 `-c`**（报告者也是 227 条）。
  第 3 轮（run 36164741493）因 CI 的 Qt 不全（`lupdate.exe` 依赖 `Qt6Qml.dll`）producer 失败，意外覆盖了兜底模型路径。

## 结论

| # | 问题 | 归属 | 结论 | 证据 |
|---|---|---|---|---|
| **M1** | clangd 数据库去掉 `-c`、保留 `-flto`：windows-msvc 目标下驱动报 `LTO requires -fuse-ld=lld`，clangd 的 P1689 扫描失败，**不建任何 BMI** | mcppls | **根因，已确认** | 本机复现 + Windows 实测：可信模型阶段扫描失败 12 次**全部**是这条；210 次 hover 全空 |
| **U1** | clangd 23.1.0（Windows）在"没有 BMI、import 解析不了"的模块单元上 Build AST 时崩溃（`0x80000003`） | clangd 上游，**由 M1 触发** | 已确认；修 M1 即消除 | `GPPDefines.ixx`：原样命令 `--check` 崩溃（E3a），加 `-c` 后 exit 0（E3b）；编辑器会话 20 秒内崩在它上面（E3c） |
| **U2** | clangd 23.1.0（Windows）在 `NormalJsonTranslator.Core.cpp` 等文件上 Build AST 崩溃，**与 `-c`、LTO 无关** | clangd 上游 | 已确认，根因未定位 | 加 `-c`、BMI 正常、跨模块 hover 正常时仍崩（E3d）；Linux 上同形状不崩 |
| **M2** | 扫描器不认 UTF-8 BOM：文件以 BOM + `export module X;` 开头时认不出模块接口 | mcppls | 已确认，**新发现** | 本机：可信模型下 mcppls 模块引擎误报 `module 'm' not found`，去掉 BOM 即消失；兜底模型下还漏掉 `-x c++-module`（CI：3 个 `.ixx`）。GalTranslPP 中 `GPPDefines`、`ProgressBar`、`TerminalController` 受影响 |
| **M3** | 崩溃嫌疑判断不准：按"退出前 10 秒碰过的文件"判定，而 clangd 已打印崩溃文件 | mcppls | 已确认 | E1：clangd 报 `NormalJsonTranslator.Core.cpp`，mcppls 隔离 `GPPDefines.ixx`；前两次崩溃嫌疑为空。**报告者"`GPPDefines.ixx` 被隔离"很可能同属误判** |
| **M4** | producer 尚未回答时的临时模型（`firstOrigin: inferred`）用机器上找到的 MinGW（`x86_64-w64-mingw32`）、不带项目 include 目录喂给 clangd；这一阶段的崩溃耗掉重启额度 | mcppls | 已确认 | E1：5 次退出中 3 次发生在前 18 秒的临时阶段；E4（关 LTO）可信阶段本身正常，仍因临时阶段的崩溃出现 `engine-restart-capped` |
| **M5** | producer 失败时的兜底扫描把 `vcpkg_installed/` 当项目源码（349 条中 166 条），产生伪 `ambiguous-module`（`proxy.v4`） | mcppls | 已确认，影响小 | 第 3 轮 |

一句话：**报告者看到的"模块全部不可用 + clangd 反复退出 + `GPPDefines.ixx` 被隔离"，是 M1（没有 BMI）→ U1（Windows clangd 在
无 BMI 的模块单元上崩溃）→ M3（隔离错文件）叠加 M4（临时阶段的崩溃先耗掉额度）的结果**；U2 是独立的上游崩溃；M2 是同一项目
暴露的独立 bug。

## 1. M1：命令是怎么变成这样的、为什么只坏在扫描上

- `src/normalize/plan.cpp:220-236`：单元参数来自 `options_arguments`（`semantic.cpp:86` 会加 `-c`）或构建命令，再经
  `translate_gnu`/`translate_msvc`。
- `src/normalize/gnu.cpp:20,49`：`ALWAYS_STRIP_EXACT` 删掉 `-c`，此后无处补回；`-flto`、`-O3`、`-g` 原样保留。
  `src/normalize/msvc.cpp:33-40` 放行所有 `-f*`，输出同样无 `-c`。所有派生条目（单元 `:473`、游离文件 `:296`、prime
  `:499-501`、std `:523-547`、stand-in `:577-581`）都继承。`tests/test_normalize.cpp:70,114` 断言输出无 `-c`——首个版本
  起就是这样，0.0.3/0.0.4 都受影响，不是回归。kit 路径（`semantic_subset`）不受影响。
- 驱动检查（`clang/lib/Driver/Driver.cpp:4348-4364`）只在 `FinalPhase == Link`、windows-msvc、开 LTO、`-fuse-ld` 非 lld
  时报 `err_drv_lto_without_lld`。
- clangd 建 AST/BMI 走 `createInvocation`（`clang/lib/Driver/CreateInvocationFromArgs.cpp:51`），会插入 `-fsyntax-only`，
  不受影响（Windows 日志里 cc1 为 `-O3 -fsyntax-only -flto=full`）；而模块扫描
  （`clangd/ProjectModules.cpp:213-240` → `DependencyScanningTool.cpp:121` 的 `containsError()`）用原命令，直接失败。
  `CompoundProjectModules::getRequiredModules` 直接用扫描结果 → 空 → 不建 BMI；`MODULE_HINTS` 也要扫描确认，同样失败。

本机复现（`-###` 不执行任何步骤）：

| 命令形状（`clang++ -x c++-module a.ixx -###`） | 退出码 |
|---|---|
| `--target=x86_64-pc-windows-msvc -flto` / `-flto=thin` | 1，`LTO requires -fuse-ld=lld` |
| 同上加 `-c`，或加 `-fuse-ld=lld` | 0 |
| `--target=x86_64-linux-gnu -flto` | 0 |

## 2. Windows 实测（第 4 轮，报告者环境）

| 实验 | 设置 | 结果 |
|---|---|---|
| E0 | `mcppls report`，不开文件 | 在引擎开始干活前结束，无异常（快照意义有限） |
| E1 | `mcppls serve` 编辑器会话 15 分钟（GPPDefines.ixx、ApiTool.ixx、GPPDefines.cpp、NormalJsonTranslator.Core.cpp、GPPCLI.cpp） | 可信阶段扫描失败 12 次全为 LTO；clangd 90 秒内退出 5 次后被判不可用（`state: unavailable`）；**210 次 hover 全空** |
| E2 | `mcppls check GPPDefines.ixx` | 扫描 LTO 失败 1 次 |
| E3a | clangd `--check GPPDefines.ixx`，mcppls 的数据库原样 | **崩溃 `0x80000003`** |
| E3b | 同上，数据库每条加 `-c` | **exit 0，无扫描失败，无崩溃** |
| E3c | clangd 单独会话，原样数据库 | 第一轮请求前即崩在 `GPPDefines.ixx` |
| E3d | clangd 单独会话，加 `-c` | 无扫描失败；跨模块 hover 生效（`string` → `provided by <string>`）；约 100 秒后崩在 `NormalJsonTranslator.Core.cpp`（U2） |
| E4 | E1 但默认 profile 改为 `fast-release`（无 LTO） | 可信阶段无扫描失败、无崩溃；48 次 hover 有内容（6 次跨模块）；但临时阶段崩溃 2 次、随后 `engine-restart-capped`、文件被隔离与"隔离后仍在处理"的重启（M3/M4） |

E1 的时间线（mcppls 事件 × clangd 崩溃上下文）：

| 时间 | 模型阶段 | clangd 报的崩溃文件 | mcppls 的嫌疑 |
|---|---|---|---|
| 19:15:23 | 临时（MinGW，无 include） | `GPPCLI.cpp` | 无 |
| 19:15:27 | 临时 | `GPPCLI.cpp` | 无 |
| 19:15:38 | 临时 | `NormalJsonTranslator.Core.cpp` | **`GPPDefines.ixx`**（错） |
| 19:15:55 | 可信（MSVC + `-flto`） | `ApiTool.ixx` | `ApiTool.ixx` |
| 19:16:45 | 可信 | `NormalJsonTranslator.Core.cpp` | `NormalJsonTranslator.Core.cpp` |

没有拿到崩溃栈：clangd 只打印了 `Signalled during AST worker action: Build AST` 与 `Exception Code: 0x80000003`，未写
minidump，Application 事件日志为空。`0x80000003` 在 LLVM 的 Windows 构建中通常意味着内部 `abort()`（LLVM 把 SIGABRT 转成
陷阱以进入崩溃处理），日志中没有断言或 `LLVM ERROR` 文本。

## 3. M2：BOM

`src/project/scan.cpp` 没有跳过 UTF-8 BOM。第一行是 `module;` 的文件不受影响（后面的 `export module` 仍被识别），第一行就是
`export module X;` 的文件会被判为非模块。可信模型下 clangd 的数据库用 mcpp 给的 `ide.role`/`provides`（mcpp 构建数据库自带
这两个字段），所以不影响给 clangd 的命令；但 mcppls 自己的模块引擎按文本扫描，会误报。兜底模型下还会漏掉 `-x c++-module`，
clang 把 `.ixx` 当成链接输入（`'linker' input unused`）。

本机复现：一个 mcpp 项目，`src/m.cppm` 写成 BOM + `export module m;`，`mcppls check src/main.cpp` 输出
`module main.cpp:1: module 'm' not found`；去掉 BOM 后无此行。

## 4. 修复建议（未实施）

| 优先级 | 项 | 做法 | 测试 |
|---|---|---|---|
| P0 | **M1** | 保留删除构建自带的 `-c`/`--precompile`，但在 `translate_gnu`、`translate_msvc`（与 `kit_arguments`）输出末尾补一个 `-c`；所有派生条目随之修正。不删 `-flto`：`-c` 一次关掉所有链接阶段检查，且不动项目的 LTO | `test_normalize.cpp:70,114` 改为断言恰好一个 `-c`；新增 windows-msvc + `-flto` 用例；一个 Linux 可跑的 clangd 级 fixture（本机已证明 windows-msvc 目标在 Linux 上也复现） |
| P0 | **M2** | 扫描器（及模块引擎的文本扫描）跳过开头的 UTF-8 BOM | 单测：BOM + `export module`、BOM + `module;` |
| P1 | **M3** | 解析 clangd 的崩溃上下文（`Signalled during AST worker action …` 后的 `Filename:`），以它为唯一嫌疑；没有上下文时才回退到现有启发式 | 用 mock clangd 输出崩溃上下文的 conformance fixture |
| P1 | **M4** | producer 尚未回答的 mcpp 项目：临时模型不把 MinGW 等"猜来的"工具链喂给 clangd（或至少读取 `mcpp.toml` 的 `[toolchain]` 与 `include_dirs`）；模型从临时切到可信时清零崩溃计数与隔离 | fixture：producer 延迟回答 + 临时阶段崩溃不导致 `engine-restart-capped` |
| P2 | **M5** | 兜底扫描排除包管理器目录（`vcpkg_installed`、`.conan*`、`node_modules`、`target` 等） | 单测 |
| P2 | 诊断 | `engine-exit` 事件与报告记录 clangd 退出码（`Connection::exit_code()` 已有）与崩溃文件；把 `Scanning modules dependencies for X failed: <reason>` 汇总成 issue；崩溃上下文行不受 `LineLimiter` 限流 | — |

预期效果（按 Windows 实测推断）：M1 修复后可信阶段扫描恢复、BMI 可建、U1 不再触发（E3b/E3d 已证明）；剩下 U2 这类上游崩溃，
由 M3 精确隔离到单个文件、M4 不让临时阶段耗掉额度，clangd 对其余文件保持可用。

## 5. 上游（clangd）

- U1 与 U2 都是 clangd 23.1.0 Windows 官方发布版在 Build AST 时的崩溃，应向 llvm-project 报告，前提是一个最小复现和调用栈。
- 下一步：在 GalTranslPP 的 CI 上用 Windows SDK 的 `cdb.exe` 挂 clangd 取栈；并用 LLVM 22.1.8 自带的 clangd 对照，判断是否为
  23.x 回归。U1 可从 `GPPDefines.ixx`（只有 import、无 include）开始缩小。

## 6. 给报告者的回复要点

- 根因已在 Windows 上复现并定位（M1），下一个版本修复；**无需**关闭 LTO 或加 `-fuse-ld=lld`。
- `GPPDefines.ixx` 被隔离很可能是误判（M3），与 clangd 在无 BMI 时的崩溃（U1）有关；修复 M1 后这类崩溃在实测中消失。
- BOM 开头的模块接口（`GPPDefines.ixx` 等）目前会被 mcppls 自身误判（M2），同版本修复。
- 可选：在 VS Code 设置 `"mcppls.trace.server": "verbose"` 后复现一次并附上日志，便于确认 `NormalJsonTranslator.Core.cpp`
  一类的独立崩溃（U2）在其机器上是否同样出现。
