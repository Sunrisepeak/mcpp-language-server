# mcppls 修复与优化方案：issue #23（Windows/LTO）与 hello 项目实测（编辑中的卡死与重启）

状态：方案第 3 版（D1–D5 已定；新增 F18 导出与脱敏；D4 的防频繁策略；自我 review）；§7 为 0.0.5 的实施计划（一个 PR 全部实现） · 2026-09-26 · 基于 `main` 04b186a（0.0.4）

依据：

- 分析报告 `.agents/docs/2026-09-26-issue-23-lto-module-scan.md`（含 GalTranslPP 的 Windows CI 实测）；
- hello 项目实测：用户 VS Code 会话日志 `~/.cache/mcppls/logs/server-20260925-215311.673-2d10.log`，以及在副本上逐字输入、
  模拟自动保存、按线程采样 CPU 的复现；
- 上游问题统一登记在 issue #24（本文引用其中的 `UP-<nn>`）。**F 编号保持稳定**：#24 的评论引用了 F1、F3、F4、F5、F7、F11、
  F12、F16。

## 0. 摘要

### 0.1 两条问题链

**A. issue #23（Windows/MSVC + LTO）**：给 clangd 的命令没有 `-c` 却带 `-flto` → 模块扫描全部失败、不建 BMI（UP-11）→ Windows
clangd 在无 BMI 的模块单元上崩溃（UP-12）→ mcppls 隔离错了文件、临时模型阶段的崩溃先耗掉额度 → clangd 被判不可用。另有独立的
上游崩溃（UP-13）和 BOM 识别缺陷（M2）。

**B. hello 项目（Linux，`files.autoSave: afterDelay`）**：自动保存把写了一半的 `import hello.` 写到磁盘 → WA-CLANGD-001 只改写
LSP 文本、clangd 却从磁盘解析前置模块（UP-14）→ 该文件的 worker 进入 UP-01 死循环（实测单核满载，改回缓冲区也停不下）→
该文件的全部 clangd 请求（含关键字补全、hover）排队超时；同时写到一半的模块名让计划来回变（stand-in 增删、数据库 13↔14 条）→
每变一次重启一次（另一次来自切换工具链）→ 10 分钟 3 次后被上限锁死，之后空转的 clangd 无法再被恢复。另有诊断吵闹与位置偏移（UP-15）、未保存的新 import 不构建（UP-14）等体验问题。

### 0.2 条目总表（按主题）

| 主题 | # | 修什么 | 来源 | 优先级 | 估算 |
|---|---|---|---|---|---|
| **正确性** | F1 | 给 clangd 的每条命令都带 `-c` | #23 根因（UP-11/12） | P0 | 1 天 |
| | F2 | 扫描器跳过 UTF-8 BOM | #23 附带（M2） | P0 | 0.5 天 |
| | F16 | WA-CLANGD-001 的磁盘漏洞：磁盘上的半截 import 让 clangd 死循环 | hello：提示消失、请求超时的根因（UP-01×UP-14） | P0 | 1–1.5 天 |
| | F5 | 兜底扫描排除包管理器目录 | #23 CI（M5） | P2 | 0.5 天 |
| **引擎恢复** | F3 | 按 clangd 崩溃上下文精确隔离，记录退出码 | #23（M3；UP-12/13 止损） | P1 | 1 天 |
| | F13 | 编辑中的计划变化防抖；不为仍在输入的 import 建 stand-in；重启合并 | hello（3 次重启） | P1 | 1.5–2 天 |
| | F14 | 重启分预算、达到上限后退避，不再锁死 10 分钟 | hello（被锁死） | P1 | 1 天 |
| | F4 | producer 回答前 clangd 等待；模型切换时清零崩溃记账 | #23（M4） | P1 | 2–3 天 |
| **编辑体验** | F11 | 未保存缓冲区里新加的 import：not found 改写为"保存后可用" | UP-14 | P1 | 1 天 |
| | F12 | 漏 `;` 的错误移回本行；可选的诊断防抖 | UP-15 | P2 | 0.5 天 |
| | F9 | `import ` 空格后自动弹出模块列表（仅 import 行） | hello | P2 | 0.5–1 天 |
| | F15 | 模块语法关键字补全由 mcppls 提供 | hello | P2 | 0.5–1 天 |
| | F8 | 声明前的 `export` 与 `export module` 高亮一致 | 用户提问 | P2 | 0.5 天 |
| **可观察性** | F17 | 常开日志环形缓冲、事故快照、计划差异、workaround 前提守卫、状态栏给原因 | 本次排查成本 | P1 | 2–3 天 |
| | F18 | 一键导出问题包（环境、日志、事故、崩溃、引擎数据库），统一脱敏（用户名、主目录、主机名、密钥），校验不泄漏才写出 | 用户要求 | P1 | 2.5–3 天 |
| | F6 | 扫描失败汇总成 issue；崩溃/扫描行不被限流 | #23 | P1 | 1–1.5 天 |
| **其他** | F7 | macOS 部署目标降到 11.0 | UP-M1（mcpp#685 已修） | P2 | 0.5 天 |
| | F10 | mcpp#699 落地后显示失败成员的诊断 | UP-M2/M3 | 后续 | 0.5 天 |
| **验证** | V | Windows（GalTranslPP CI）+ Linux（hello/自动保存夹具）验收 | 全部 | — | 0.5–1 天 |

合计（不含 F10）约 **18–23 人日**。

### 0.3 版本划分建议

| 版本 | 目标 | 内容 | 估算 |
|---|---|---|---|
| **0.0.5 "稳"** | issue #23 关闭；hello 场景不再卡死、不再被锁死；出事能看清原因并一键导出 | F1、F2、F16、F3、F13、F14、F6、F7、F17 的 1–2 项（环形缓冲 + 事故快照）、F18 | 约 12–14 天（另加验证 V） |
| **0.0.6 "顺"** | 编辑体验 | F4、F11、F9 + F15、F12、F8、F5、F17 的 3–6 项 | 约 6–9 天 |
| 之后 | 跟随上游 | F10（mcpp#699）；UP-01 的上游处理与（必要时）自建 clangd（F16.3，D2：推迟） | — |

### 0.4 依赖关系

- **F3 → F14 → F17 → F18**：F14 的分预算需要 F3 给出的崩溃归因；F17 的事故快照复用 F3 的崩溃上下文与 F14 的原因分类；F18 把
  F17 的事故目录、日志与报告打包并脱敏（没有 F17 时 F18 仍可导出日志、报告与环境，只是少了事故现场）。
- **F13 ↔ F16**：两者都在"来自正在编辑的文档的变化"上做判断（F13 防抖计划变化，F16 按磁盘内容主动隔离），共用"文档最近编辑时间 +
  磁盘内容检查"的状态，一起实现。
- **F4 ↔ F14**：都改崩溃/重启记账（F4 的"模型切换清零"与 F14 的"分预算"），一起设计。
- **F9 ↔ F15**：都改补全路由（空格触发只交给 mcppls 引擎；关键字补全由 mcppls 提供并与 clangd 结果合并），一起实现。
- **与 mcpp 无依赖**：F7 只需 mcpp ≥ 2026.9.24.1（已发布）；F10 等 mcpp#699。

## 1. 正确性

### F1. 给 clangd 的每条命令都带 `-c`（P0）

**问题**：`src/normalize/gnu.cpp:20,49` 的 `ALWAYS_STRIP_EXACT` 删掉 `-c` 后无处补回；`translate_msvc`（`src/normalize/msvc.cpp`）
的输出同样没有；`semantic.cpp:86` 加的 `-c` 也会被 `translate_gnu` 删掉。最终阶段是 Link，windows-msvc + LTO 下驱动报错，clangd
的模块扫描失败（UP-11），不建 BMI，进而在 Windows 上触发 UP-12。

**改法**：

1. `translate_gnu`、`translate_msvc` 仍删除构建命令自带的 `-c`、`--precompile`，但在输出末尾**补一个 `-c`**（已有则不重复）；
   `kit_arguments` 同样补。
2. 派生条目自动继承：项目单元（`plan.cpp:473`）、游离文件（`:296`）、prime（`:499-501`）、std（`:523-547`）、stand-in
   （`:577-581`）。
3. **不删 `-flto` 等链接参数**：`-c` 一次关掉所有只在链接阶段做的驱动检查，不需维护链接参数清单，也不改变项目的 LTO（#23 报告者
   明确要求）。

**为什么安全**：clangd 建 AST/BMI 时 `createInvocation` 插入 `-fsyntax-only`，与 `-c` 并存时取更早的阶段，行为不变；CMake、mcpp
自己写的构建数据库本来就带 `-c`。

**测试**：`tests/test_normalize.cpp:70,114` 改为"恰好一个 `-c`"；新增 windows-msvc + `-flto` 的 GNU、clang-cl 与 kit 三种输入；
plan 级测试覆盖各派生条目；conformance：Windows CI 的 `mcpp-llvm-msvc-lto`，以及 Linux 可跑的 compdb 夹具（本机已证明该驱动检查
在 Linux 上同样触发）。

### F2. 扫描器跳过 UTF-8 BOM（P0）

**问题**：`src/project/scan.cpp` 从偏移 0 开始词法分析，BOM + `export module X;` 开头的文件认不出模块声明。所有文本扫描都走
`project::scan_source`（计划 `plan.cpp:288`、自带模块引擎 `engine/native/index.cpp:71`、AI review、查询视图），
`engine/native/exports.cpp` 另有一份镜像的声明解析。本机复现：可信模型下 mcppls 引擎误报 `module 'm' not found`；兜底模型下还会
漏掉 `-x c++-module`。

**改法**：`Lexer` 构造时若以 `EF BB BF` 开头则从偏移 3 开始，偏移仍按原文计算（语义 token、诊断位置不变）；`exports.cpp` 同样处理。

**测试**：BOM + `export module`/`module;`/`module m;`/`import` 四种；语法 token 列号与无 BOM 时一致；conformance `mcpp-bom`。

### F16. WA-CLANGD-001 的磁盘漏洞（P0）

**实测（hello 副本，0.0.4，`Worker:main.cpp` 在 20 秒内的 CPU tick）**：`import hello.` 只在缓冲区 → 2；自动保存写盘后 → 约 2000；
缓冲区改回 `import hello.greet;` 而磁盘仍是 `import hello.` → 约 2000。WA-CLANGD-001 只改写 LSP 文本，而 clangd 从磁盘解析前置模块
（UP-14），于是进入 UP-01 的死循环，只有重启能解开。这是 hello 场景"只有 `import hello.` 有提示""停久了请求超时"的根因；
会话里计入的 3 次重启另有来源（两次计划抖动见 F13，一次切换工具链），达到上限后，因空转而请求的重启又被拦下（22:02:25），
clangd 从此停在空转里（F14）。

**改法**：

1. **磁盘侧检测 + 主动隔离**：`didSave` 与文件监视时，对数据库内的文件用 WA-CLANGD-001 的同一判定检查磁盘内容；命中时在 clangd
   重建该文件的 preamble 之前把它交给 mcppls 自己的引擎（隔离），磁盘内容变回合法后交还。不等空转、不走重启、不耗额度。
2. **兜底**：若已在空转（`Worker:<file>` 持续满载且磁盘内容命中），立即重启 clangd 并隔离该文件到磁盘恢复为止，原因写明，不计入
   计划重启额度。
3. **根治（D2 已定：推迟）**：本期只做 1、2 的规避；上游处理（bisect `6dcfc17b1b`、提交、申请 23.1.x backport）推迟，已在 #24
   的 UP-01 标记为 deferred；若迟迟没有 backport，再自建 23.1.x + cherry-pick 作为后备。UP-14 若在上游修复（扫描改用含缓冲区
   的 `TFS`），磁盘路径也随之消失。

**测试**：conformance——逐字输入、在 `import hello.` 处写盘 + `didSave`；断言无满载 worker、无重启、其他文件照常应答，改成完整名字
并保存后该文件回到 clangd。

### F5. 兜底扫描排除包管理器目录（P2）

`src/project/infer.cpp:21` 的 `SKIPPED_DIRECTORIES` 加入 `vcpkg_installed`、`vcpkg`、`.conan`/`.conan2`、`.xmake`、`.cache`、
`.git`，并跳过含 `vcpkg.json` 的子项目根。#23 的 CI 中 producer 失败时 349 条里有 166 条来自 `vcpkg_installed/`，产生伪
`ambiguous-module`。测试：`test_project.cpp` 增加对应用例。

## 2. 引擎恢复

### F3. 按 clangd 的崩溃上下文精确隔离，记录退出码（P1）

**问题**：`handle_closed_`（`src/engine/clangd.cpp:1528-1575`）以"未答请求 + 退出前 10 秒碰过的文件"为嫌疑；Windows 实测中 clangd
报 `NormalJsonTranslator.Core.cpp`，mcppls 却隔离了 `GPPDefines.ixx`，前两次崩溃无嫌疑。

**改法**：在 stderr 转发处（`clangd.cpp:1066-1100`，与 `parse_module_failure` 并列）解析
`Signalled during AST worker action: <动作>` / `Filename:` / `Exception Code:`（或 POSIX 信号）；本代进程有崩溃上下文时只隔离
那个文件，没有时才回退启发式；`engine-exit` 事件与报告增加 `exitCode`（`Connection::exit_code()` 已有）、`crashFile`、
`crashAction`；issue 文案写明"clangd crashed while building `<文件>`"。

**测试**：解析器单测（Windows/POSIX、多行、夹杂日志）；conformance 用替身 clangd 打印崩溃上下文后退出。

### F13. 编辑中的计划变化不让 clangd 抖（P1）

**实测（用户会话日志）**：21:54 输入 `import hello…`，自动保存把 `import h` 写盘，clangd 报 `Failed to build module h`，`h` 是合法名
字，生成 stand-in，4 秒后撤掉 → 重启 1；22:01 新建 `src/test/test.cppm`，先与 `src/test.cppm` 重复声明 `hello.test`、后为
`hello.test.`，mcpp emit 两次失败，数据库 13↔14 条 → 重启 3。

**改法**：

1. 来自正在编辑的文档的 import/模块声明引起的计划变化（stand-in 增删、补位条目、provider 变化），在该文档停止修改约 2 秒后才应用；
   自动保存不算"稳定"。
2. 不为最近编辑过的文档里的 import 生成 stand-in（H3 从"非法名字"扩大到"仍在输入"）。
3. 计划驱动的重启合并为最多每 N 秒一次、总用最新计划；clangd 自己重读数据库（约 5 秒）能解决的不重启。
4. producer 在编辑中途失败时保持上一份计划，打开文件的补位条目也不来回增删。

**测试**：conformance——逐字输入 `import hello.test.a;`，中途模拟自动保存；断言无 stand-in、无 `engine-restart`。

### F14. 重启上限不再把 clangd 锁死（P1）

**实测**：22:01:24 起 `not restarting clangd again: already 3 restarts in the last 10 minutes`，22:02–22:04 clangd 对全部请求
不应答约 90 次，`main.cpp` 被隔离。

**改法**：重启按原因分预算（计划驱动 / 崩溃与卡住）；模型的工具链或 profile 变化（如用户切换工具链）引起的重启不计入任何预算；达到上限后按 1、2、4、8 分钟退避，到点即可恢复卡住的
clangd；受限期间状态栏给出原因与"Restart clangd"动作。

**测试**：预算与退避单测；conformance：连续触发计划重启后 clangd 仍能在退避后恢复。

### F4. producer 回答前 clangd 等待；模型切换时清零崩溃记账（P1）

**问题**：`start_inferred_load()`（`workspace.cpp` ~770）在 producer 回答前用语义套件的临时模型（Windows 上是 `x86_64-w64-mingw32`，
无项目 include 目录）喂给 clangd。#23 CI：E1 的 5 次退出中 3 次发生在前 18 秒；E4（关 LTO）可信阶段本身正常，仍因这一阶段的崩溃
`engine-restart-capped`。

**改法**：识别出构建系统（level 3）且 producer 正在运行时，clangd 等待（打开的文件进"等待数据库"队列，由 mcppls 引擎回答），上限为
producer 超时（默认 60 秒）；超时或失败后才用临时/兜底计划。模型来源变化（`inferred` → `producer`）时清零崩溃计数、释放因临时模型进入的
隔离、重置重启门。没有构建系统的项目保持现状。

**取舍**：等待期间只有模块级功能（GalTranslPP 温启动 producer 约 24 秒）；换来可信模型到达时 clangd 不带"前科"。

**测试**：mockmcpp 延迟回答（沿用 `mcpp-emit-hang`），断言回答前无 `engine-start`；"模型切换清零"单测。

## 3. 编辑体验

### F11. 未保存缓冲区里新加的 import（UP-14，P1）

**实测**：未保存时补全出 `import a.b;`，`Module 'a.b' not found` 45 秒以上不消失；保存后 1–8 秒消失（hello 与 cmp 两个项目）。

**改法**：clangd 对打开且有未保存修改的文档报 `Module 'X' not found`，而 X 在计划里有提供者、这行 import 只在缓冲区不在磁盘时，替换为
信息级诊断"module 'X' is in the project; clangd loads it once the file is saved"；X 不在计划里时保留原错误。其间 X 相关的模块级请求
由 mcppls 引擎回答。上游修复建议已写入 UP-14。

**测试**：增量输入不保存，断言无该错误、出现信息；保存后信息消失。

### F12. 输入 import 时的诊断位置与闪烁（UP-15，P2）

**实测**：输入 `import hello` 时 `Import directive must end with a ';'` 标在两行之后的 `auto main()` 上；逐字输入时每约 150 ms 一条
（`Unknown type name 'i'`、`Module 'h' not found`……）。

**改法**：该诊断落在第 N 行、且其上方最近的非空行是未以 `;` 结束的 `import`/`export import`/`module` 指令时，把范围移到那一行末尾
（登记为新的 `WA-CLANGD-<n>`）；可选：对正在编辑的文档把引擎诊断推迟到最后一次修改后约 400 ms，保存或停顿后照常发布。

### F9 + F15. 补全：空格触发与模块语法关键字（P2）

**实测（LSP 层）**：模块名补全（`import hello.`、`import hello.test.`、`import a`、`import :`）瞬时可用；关键字补全来自 clangd
（`i` 起即含 `import name;`，`e` 起含 `export`，`isIncomplete` 正确透传）；clangd 该文件卡住时关键字补全随之消失；`triggerCharacters`
是 clangd 的 `. < > : " / *`，没有空格；clangd 不提供 `export module`、`export import` 组合。

**改法**：

- F9：`triggerCharacters` 加空格；空格触发的补全只在行前缀为 `[export] import ` 时交给 mcppls 引擎，其余立即回空、**不转发 clangd**。
  **避免频繁处理（D4）**，由外到内四层：
  1. **编辑器侧拦截（VS Code）**：插件的 language client middleware 在发请求前检查 `triggerCharacter === ' '` 与当前行文本
     （`document.lineAt`），不匹配就直接返回空，**普通代码里的空格不产生任何 IPC**。
  2. **服务端门槛**：路由收到空格触发的请求时，只取该文档当前行到光标的文本（文档已在内存）做一次前缀判断
     （`^\s*(export\s+)?import\s$`：关键字后恰好一个空格、之后没有别的字符），不匹配立即回 `{isIncomplete:false, items:[]}`；
     不转发 clangd、不查索引、不写 info 日志（只计数）。
  3. **结果缓存**：模块名候选按计划代次缓存，匹配时直接复制，不随每次按键重建。
  4. **按客户端启用**：只对已知能优雅处理空结果的客户端（`clientInfo.name` 为 VS Code / Cursor）声明空格触发；Neovim、Zed、
     CLion 默认不声明，可用初始化选项 `mcppls.completion.triggerOnSpace` 打开。VS Code 另有 F15 的补充：接受 `import` 关键字后
     随即弹出模块列表（补全项带 `editor.action.triggerSuggest`），不依赖空格触发。
  报告（F17）记录空格触发的请求数、通过门槛的次数与耗时，用来确认开销可以忽略。
- F15：mcppls 引擎在行首上下文提供 `import`、`export import`、`module;`（仅文件开头）、`export module`、`module`（实现单元）、
  `module :private;`（仅接口单元），与 clangd 结果合并去重；接受 import 类关键字后接着弹出模块名列表。

**测试**：`completion-contains` 覆盖各前缀、空格触发的两种行、clangd 不可用时仍有关键字。

### F8. 声明前的 `export` 与 `export module` 高亮一致（P2）

**实测**：`module;`、`export module`、`export import`、`import`、`module :private;` 的关键字都有 `keyword` 语义 token；声明前的
`export`（`export namespace`、`export {`、`export int f()`）服务端不发，VS Code 内置 C++ 语法也不给作用域，显示为普通文本。

**改法**：`scan_syntax_tokens` 对模块单元里声明开头的 `export` 发 `keyword`；注入语法增加对应规则。

## 4. 可观察性

### F17. 一次复现就能看清原因（P1；0.0.5 先做 1–2）

这次排查的代价：用户的 info 级日志（487 行）只有"重启了""没应答"，没有哪个文件、哪个线程、clangd 看到的是缓冲区还是磁盘、计划为什么
变；每个关键事实都要另起环境复现才得到。

1. **常开的 clangd 日志环形缓冲**：clangd 以 `--log=info` 运行（D3 已定），最近 N 行（例如 4000 行 / 4 MB）只留在内存；出事时整段
   落盘并记入报告；需要更细时由事故触发临时切到 verbose 再采一段。
2. **事故快照**（崩溃/卡住/隔离/重启/达到上限各一份，`<cache>/incidents/<时间>-<类型>/`）：原因链；涉及文件的缓冲区版本与磁盘内容差异
   （只记涉及的行）；引擎数据库中的条目；`fileStatus` 时间线；按线程 CPU（Linux `/proc/<pid>/task`，Windows `GetThreadTimes`）；
   退出码与崩溃上下文（F3）；最近的计划差异。
3. **计划差异日志**：列出增删改的条目及原因；`units are compiled with other arguments` 写出哪个单元、哪个参数。
4. **workaround 前提守卫**：每个 `WA-CLANGD-<n>` 声明前提（WA-001：clangd 只看到 LSP 文本），运行时检测到前提被破坏即记事故（本次即
   "磁盘内容命中判定"）。
5. **状态栏写原因、给动作**："clangd 卡在 `main.cpp`：磁盘上的 `import hello.` 未写完（UP-01）"，附"查看事故 / 导出问题包（F18）/ 重启 clangd"。
6. **conformance 覆盖自动保存路径**（本次漏洞就是这样漏过的）。
7. **事故目录保留**：最近 20 次或 7 天，超出即删，避免无限增长。

### F18. 一键导出问题包，统一脱敏（P1）

**为什么**：现在出了问题，用户能给的只有 `Collect Report`（一份 JSON，最近 300 行日志，提示"含本机路径，请自行编辑"）。本次排查
真正用到的信息——多次会话的日志、clangd 自己的输出、线程 CPU、缓冲区与磁盘差异、工具链与编辑器环境、崩溃退出码——散在各处，
要靠开发者另起环境复现。用户也不该手工删除用户名和路径。

**入口**（同一个实现，服务端 C++，所有编辑器共用）：

- VS Code：命令 **"C++ Modules: Export Diagnostic Bundle"**；F17 的事故通知、状态栏受限提示、`Collect Report` 的结果页都提供它。
- 其他编辑器：`workspace/executeCommand` `mcppls.exportBundle`。
- 命令行：`mcppls report --bundle <out.zip> [--root <dir>] [--settle N] [--hide-project-paths] [--no-source-excerpts]
  [--include-dumps] [--no-redact]`；CI（如 GalTranslPP 的验证 workflow）直接把问题包作为 artifact 上传。

**内容**（zip，默认写到 `<cache>/bundles/mcppls-bundle-<时间>.zip`，完成后给出"在文件夹中显示 / 复制路径"；**从不自动上传**）：

| 文件 | 内容 |
|---|---|
| `manifest.json` | 格式版本、生成时间、包含了什么、应用了哪些脱敏规则及各自命中次数（**不含原值**）、各文件大小与哈希 |
| `report.json` | 当前的 `cxxModules/report`（引擎状态、计划、事件、工具运行、workaround） |
| `environment.json` | 操作系统/内核/架构、CPU 与内存、区域与代码页（Windows ACP）、编辑器名与版本、插件版本、其他 C++ 插件的安装与启用状态、mcppls 设置、payload 版本（`payload.json`）、clangd / mcpp / xlings 版本、工具链探测结果、环境变量**白名单**（`MCPP_*`、`XLINGS_*`、`LANG`/`LC_*`、`PATH`） |
| `logs/` | 最近 3 次会话或 24 小时内的服务端日志（超出按"头 + 尾"截断）；插件自己的输出通道日志 |
| `incidents/` | F17 的事故快照：clangd 日志片段、线程 CPU、fileStatus 时间线、缓冲区/磁盘差异、崩溃上下文与退出码 |
| `engine/` | 引擎数据库（`compile_commands.json`）、计划摘要、模块图 |
| `dumps/` | 仅 `--include-dumps` 或界面勾选时：Windows minidump（体积大，且含内存内容） |

**不包含**：源代码全文。事故里只带与问题直接相关的行（例如 `import hello.`），可用 `--no-source-excerpts` 关掉；项目路径默认按
主目录规则替换为 `~/…`，`--hide-project-paths` 再替换为 `<workspace>/…`。

**脱敏**（对包内每个文本文件执行，JSON 与纯文本都适用）：

1. **主目录 → `~`**：覆盖 POSIX 路径、Windows 路径（`\` 与 `/`、JSON 转义的 `\\`、盘符大小写）、8.3 短名（`C:\Users\RUNNER~1`）、
   `file://` URI 及其百分号编码（`%3A`）、WSL 的 `/mnt/c/Users/<名>`。
2. **用户名 → `<user>`、主机名 → `<host>`**：只在路径与已知字段（环境变量值、URI 的 authority）中替换，避免把普通单词误改。
3. **密钥 → `<redacted>`**：环境变量只收白名单；键名像 `api_key`/`token`/`secret`/`password`/`authorization` 的值；已知前缀
   （`ghp_`、`github_pat_`、`sk-`、`xoxb-`）；邮箱；编译参数里名字像密钥的 `-D<NAME>=<value>`；AI 网关相关设置的值。
4. **一致映射**：同一原值总是换成同一占位符，路径仍可比较；映射表本身不进包。
5. **写出前自检，失败即不写**：打包完成后在所有文件里搜索原始的主目录、用户名、主机名（上述各种写法）；只要还有残留，就不生成
   问题包，并报告是哪个文件、哪条规则漏了。
6. `--no-redact` 只给本地排查用，界面上不提供。

**同步改动**：`Collect Report` 默认也走同一套脱敏（它本来就是为贴到 issue 准备的），界面文案从"请自行编辑"改为"已替换用户名与路径，
可在导出前预览"。

**测试**：脱敏单测覆盖上面每种写法（Linux 与 Windows、JSON 转义、URI 编码、8.3 短名、WSL）；自检"残留即失败"；conformance：
在一个用户名较长且出现在各类路径里的临时 HOME 下导出，断言包里任何文件都不含原用户名与主目录、manifest 与实际内容一致、
大小在上限内（例如 25 MB）。

### F6. 扫描失败自解释（P1）

解析 `Scanning modules dependencies for <文件> failed: <原因>` 汇总为 issue（次数、前几个文件、第一条原因；驱动错误时措辞为
"clangd rejected the compile command for module scanning: `<原因>`"——#23 的报告者因此能直接看到 `LTO requires -fuse-ld=lld`）；
崩溃上下文、扫描失败、`Failed to build module` 行不受 `LineLimiter` 限流；报告增加 `scanFailures`。

## 5. 其他

- **F7（UP-M1）**：`.github/versions.env` 的 `MCPP_VERSION` 升到 ≥ 2026.9.24.1，`mcpp.toml` 的 `[build]` 加
  `macos_deployment_target = "11.0"`，用 `llvm-objdump --macho --private-headers` 验证 `minos 11.0`。
- **F10（mcpp#699 落地后）**：`parse_database_envelope`（`src/spec/discovery.cpp:62`）只要有 `data` 就接受、不看退出码，部分数据库
  会被直接使用（前提：mcpp 保持 `kindVersion = 1`）；届时只需把失败成员的诊断显示为 issue。
- **V（验证）**：
  - Windows：Sunrisepeak/GalTranslPP PR #1 的 workflow 加 `workflow_dispatch` 输入，改用 mcppls 某次 CI 的 win32-x64 payload；验收：
    E1 可信阶段 0 次 LTO 扫描失败、hover 有内容（含跨模块）、隔离的都是 clangd 报出的文件、不因临时阶段被判不可用、BOM 文件无误报。
  - Linux：把本次的逐字输入脚本（`import hello.` 写盘 A/B、`import hello.test.a;` 未保存、线程 CPU 采样）固化为 conformance 夹具；
    验收：无满载 worker、无计划驱动的重启风暴、关键字补全在任何时候都可用。
  - 问题包（F18）：两个验证环境都用 `mcppls report --bundle` 产出问题包并作为 artifact 上传；验收：包含事故快照与多次会话日志，且任何
    文件都不含 runner 的用户名与主目录（Windows `C:\Users\runneradmin`、8.3 短名 `RUNNER~1`，Linux 的 `$HOME`）。
  - 验证结束后关闭 GalTranslPP PR #1。

## 6. 决策与自我 review

### 6.1 已定的决策（2026-09-26）

| # | 决策 | 结论 |
|---|---|---|
| D1 | F4：producer 回答前 clangd 等待（最长 60 秒，只剩模块级功能） | 做，只对识别出构建系统的项目 |
| D2 | F16.3：UP-01 的根治是否捆绑自建 clangd | 本期只做 F16.1/F16.2 的规避；上游处理推迟，已在 #24 的 UP-01 标记（待账号权限恢复后写入，见 §6.4）；自建作为后备 |
| D3 | F17.1：常开的 clangd 日志级别 | `--log=info` 进环形缓冲，出事后临时提到 verbose 再采一段 |
| D4 | F9：空格触发如何避免频繁处理 | 四层：编辑器侧拦截（零 IPC）→ 服务端前缀门槛（不转发 clangd）→ 结果按计划代次缓存 → 只对已知客户端声明；并计数验证开销 |
| D5 | F15：`export module ` 后是否建议名字 | 不建议名字，只补关键字 |
| — | 新增 F18：问题包导出与脱敏 | 做，P1，进 0.0.5 |

### 6.2 这一版做了什么

- 写入 D1–D5；F9 增加 D4 的四层策略；F16.3、F17.1 按决策改写。
- 新增 F18，并把 F17 的事故通知、状态栏提示、`Collect Report` 都接到它；事故目录加保留上限（F17.7）。
- 重算：合计 18–23 人日；0.0.5 约 12–14 天（新增 F18）；依赖链改为 F3 → F14 → F17 → F18。
- 更正：F16 是"只有 `import hello.` 有提示、停久了超时"的根因，但不是"重启 3 次"的根因（那是 F13 的计划抖动加一次切换工具链）；
  §0.1、F16、§6.3 已按日志改写。F14 的"用户操作不计"改为以模型的工具链/profile 变化为判据。F18 的失败处理统一为"残留即失败"。

### 6.3 自我 review：检查过的点

- **编号与引用**：#24 引用的 F1、F3、F4、F5、F7、F11、F12、F16 都在，含义未变；新增的 F17、F18 未被外部引用。
- **优先级一致**：P0 只有"会让功能完全失效"的三项（F1、F2、F16）；P1 是"会反复失效或无法定位"的（引擎恢复、F11、F17、F18、F6）；
  体验项为 P2。
- **版本边界**：0.0.5 覆盖 issue #23 的关闭条件（F1、F2、F3、F6）与 hello 场景的根因与锁死（F16、F13、F14），并能导出现场（F17 部分、
  F18）；F4 放 0.0.6：hello 场景的 3 次重启里两次是计划抖动（F13 消除）、一次是切换工具链（F14 不再计入）；F4 针对的是 Windows 冷启动
  临时阶段的崩溃，F14 的分预算已能防止它把 clangd 锁死，F3 能正确归因，因此可以晚一个版本。
- **互相冲突的地方**：F13 的 2 秒防抖与 F11 的信息级诊断配合，不会出现"合法 import 被报错"；F9 的服务端门槛与 F15 的关键字补全
  走同一条补全路由，一起实现避免两次改动；F18 的脱敏同时用于 `Collect Report`，避免两套规则。
- **隐私**：问题包默认不含源代码全文、不含 minidump、不含非白名单环境变量；"残留即失败"的自检保证不会带着用户名写出。

### 6.4 剩余风险与待办

1. **F16 只覆盖了实测到的读盘路径**：后台索引同样读磁盘，可能在其他线程上遇到 UP-01。实现前先验证（写盘 `import hello.` 后观察
   `ground-worker-*` 线程）；若也会卡住，F16.1 需把该文件暂时移出后台索引，或在命中时推迟转发文件变化。
2. **F1 改变所有命令**：clangd 模块缓存按命令哈希，升级后首次打开会重建全部 BMI（一次性），更新说明写明。
3. **F13 的 2 秒防抖**让合法的新 import 晚约 2 秒生效；由 F11 的信息级诊断兜住体验。
4. **F14 的退避可能掩盖崩溃循环**：用 F3 的归因区分，崩溃循环仍走现有上限。
5. **Windows 的 UP-13** 在 0.0.5 只能止损，根因等取到调用栈（#24 待办）。
6. **F18 的脱敏漏网**：用户名很短或是常见词时，只在路径与已知字段替换，其他位置可能残留；由"残留即失败"兜底：界面上只提供
   "隐藏项目路径后重试"，不提供"仍然导出"；确需原样内容时只能在命令行用 `--no-redact`，且只用于本地排查。
7. **时序敏感的测试**（F13、F14、F16）：夹具用事件驱动而非固定 sleep，避免 CI 不稳定。
8. **issue #24 的待写入更新**：当前 gh 账号 `speak-agent` 对仓库只有 pull 权限，UP-01 的"D2：上游处理推迟"说明与索引中 UP-01、UP-14
   两行的更新已在本地准备好，需要切回有权限的账号（或给该账号授权）后写入。

## 7. 实施计划：0.0.5 一个 PR 做完（2026-09-26 定）

范围调整：§0.3 的两个版本合为 **0.0.5 一次发布、一个 PR**（与 0.0.4 的 #22 同样的形态）；F1–F9、F11–F18 全做，F10 等
mcpp#699（仍 open），F16.3 按 D2 推迟。

### 7.1 多角度的约束（每个工作包都按这 8 条自查）

| 角度 | 约束 |
|---|---|
| 架构 | 新能力放进独立模块，引擎只通过 `Host` 与工作区交互：崩溃/扫描行解析在 `engine.clangd.process`；重启预算在 `engine.clangd.guard`（`RestartGate` 改为按原因分预算 + 退避）；日志环形缓冲与事故记录是引擎无关的新模块（`Host::record_incident`，默认空实现，与 `record_event` 同形）；问题包与脱敏是新目录 `src/bundle/`，只读缓存目录与报告，不反向依赖引擎 |
| 稳定性 | 任何新逻辑都不阻塞事件循环（问题包在后台线程生成、以事件回到循环）；预算/退避/防抖都有上限；脱敏失败即不写出；事故目录有保留上限；时序测试用事件驱动 |
| 优雅简洁 | F1 一条规则（补 `-c`）替代链接参数清单；F18 一套脱敏同时服务问题包与 Collect Report；F9/F15 走同一条补全路由；每个 workaround 仍在注册表里（F12 是 `WA-CLANGD-006`） |
| 用户体验 | 不卡死、不锁死；状态栏说出原因并给出动作（导出问题包 / 重启 clangd）；import 行空格即出模块列表、关键字补全任何时候可用；`export` 高亮一致；未保存的新 import 是信息而非错误 |
| 兼容性 | 非 VS Code 客户端：空格触发默认不声明、可用初始化选项打开；新命令都走标准 `workspace/executeCommand`；报告与状态只增字段（S3 附加）；clangd 日志 info 行仍只进 debug 级日志，不增加默认日志量 |
| 跨平台 | 崩溃上下文同时认 Windows（`Exception Code`）与 POSIX（信号）；线程 CPU 在 Linux 读 `/proc`，其他平台退化为进程 CPU；脱敏覆盖 Windows 路径、8.3 短名、`file://` 编码、WSL；zip 写出器纯 C++、无平台依赖；F7 macOS 11 |
| 一致性 | 事件名 `engine-*`、报告字段 camelCase、文档中英同步、S3 规范 + traceability + 夹具一起改、#24 与 workaround 注册表对应 |
| 无感升级 | 设置默认值不变；新设置可选；缓存目录只新增 `incidents/`、`bundles/`；F1 让 clangd 命令哈希变化 → 首次打开重建 BMI 一次（CHANGELOG 写明）；不需要迁移 |

### 7.2 工作包与依赖

| 包 | 内容 | 主要文件 | 依赖 | 执行 |
|---|---|---|---|---|
| **A** | F1、F2、F5、F8 | `normalize/{gnu,msvc,semantic}.cpp`、`project/{scan,infer}.cpp`、`engine/native/exports.cpp`、VS Code 语法 | 无 | 并行（独立 worktree） |
| **C** | F9、F15 | `orchestrator/{routing,workspace(路由入口)}.cpp`、`engine/native{,/index}.cpp`、`server/session.cpp`（初始化选项）、VS Code middleware | 无 | 并行 |
| **D** | F18 + Collect Report 脱敏 + VS Code 两个命令（导出问题包、重启 clangd） | 新 `src/bundle/`、`cli/{options,commands}.cpp`、`server/session.cpp`（`mcppls.exportBundle`）、VS Code | 与 B 的契约（7.3） | 并行 |
| **B** | F3、F6、F14、F4、F13、F16、F11、F12、F17（1–7） | `engine/clangd{,/process,/guard,/workarounds}.cpp`、`orchestrator/workspace.cpp`、新事故模块 | 内部顺序：F3/F6 → F17.1–2 → F14 → F4 → F13+F16 → F11/F12 → F17.3–7 | 主线 |
| **E** | F7 | `.github/versions.env`、`mcpp.toml` | 无 | 主线 |
| **F** | 版本 0.0.5、CHANGELOG、docs（中英）、S3 规范/schema/traceability、design.md、本文实施记录 | — | A–E 合入后 | 主线 |
| **V** | Linux conformance 全量 + 新夹具；Windows：GalTranslPP 探针用本 PR 的 payload；问题包验收 | — | F 之后 | 主线 |

合入顺序：A、C、D 各自在 worktree 分支上完成并通过单测，按 A → C → D 合进 `release/0.0.5`（冲突只可能在
`session.cpp`、`workspace.cpp` 路由入口、`conformance.cpp`、`traceability.json`、VS Code `package.json`，均为追加），然后 B、E、F、V。

### 7.3 B ↔ D 的契约（问题包读什么）

- 事故目录：`<工作区缓存>/incidents/<UTC yyyyMMddTHHmmss.SSSZ>-<kind>/`，内含 `incident.json`（kind、时间、根目录、原因链、
  涉及文件与其缓冲区/磁盘差异的相关行、fileStatus 时间线、线程 CPU、退出码与崩溃上下文、最近计划差异、重启历史）与
  `clangd.log`（环形缓冲落盘）。保留最近 20 个或 7 天。
- 服务端日志：`<缓存>/logs/server-*.log`（`open_log_file`）；工作区缓存目录在报告的 `roots[].cacheDirectory`。
- 服务端命令：`mcppls.exportBundle`（D）、`mcppls.restartEngine`（B：绕过预算重启一次，记为用户操作、不计入预算）；VS Code 命令
  `mcppls.exportDiagnosticBundle`、`mcppls.restartClangd`（D）。状态 issue 的 `command` 可指向这两个（B）。

### 7.4 完成标准

单测（dev + release profile）、`devtools check all`、`validate.py`、全部 conformance（含新夹具）本地通过；PR 的 CI 三平台全绿；
自我 review（含一次独立 review agent）后无遗留问题；squash 合入；Release 工作流发布 0.0.5；下载发布产物、校验哈希、本地装 VSIX 与
payload 验证；最后报告产物目录。

## 8. 实施记录（0.0.5，2026-09-26）

一个 PR（#26，分支 `release/0.0.5`；并入 #25 的 S2 0.3.0，F10 因此不再推迟）完成 §7 的全部工作包：A、C、D 由三个并行分支各自实现、单测与夹具验证后合入，B、E、F 在主线实现。
各条的落点与验证：

| # | 实现 | 证据（在 0.0.4 上失败、在 0.0.5 上通过） |
|---|---|---|
| F1 | `translate_gnu`/`translate_msvc`/`kit_arguments` 末尾恰好一个 `-c`（在 `-x c++-module` 前），`-flto` 保留 | `test_normalize`；夹具 `compdb-lto-msvc`（Linux 上复现 `LTO requires -fuse-ld=lld`） |
| F2 | `base::byte_order_mark_size`；两个词法器从 BOM 之后开始，位置按原文；WA-CLANGD-001 的改写也跳过 BOM | `test_scan`、`test_exports`、`test_workarounds`；夹具 `inferred-bom` |
| F3 | `LogReader` 解析崩溃上下文（含 Windows 异常码）；退出在 500 ms 后结算，只隔离 clangd 点名的文件；`lastExit` 进报告与状态 | `test_server`（#23 的原始日志行）；夹具 `clangd-crash-context`（替身 clangd） |
| F4 | 识别出构建系统时，clangd 在 producer 回答或 60 s 前不接收计划、不被路由请求；临时模型被替换时清零其崩溃/重启/隔离记账 | 夹具 `mcpp-emit-wait` |
| F5 | `SKIPPED_DIRECTORIES` 与子目录 `vcpkg.json` | `test_project` |
| F6 | 扫描失败按驱动错误（environment）/缺头文件（project）/代码错误（仅计数）分类；关键行不受限流 | `test_server`；夹具 `compdb-rejected-command` |
| F7 | `MCPP_VERSION=2026.9.26.1`、`macos_deployment_target = "11.0"`；本机交叉编译读出 `minos 11.0` | 本机验证 |
| F8 | 模块单元中每个 `export` 都是 keyword；VS Code 注入语法新增规则 | `test_scan`/`test_tokens`、`grammar.test.ts`、`inferred` 夹具 |
| F9、F15 | 见 C 分支：四层防频繁、关键字合并、`mcppls.completion.triggerOnSpace`、S3 §6.2 | `test_completion`、夹具 `completion-keywords`、VS Code 单测；门槛开销均值 77 µs |
| F11、F12 | `WA-CLANGD-007`（未保存的 import 改为信息）、`WA-CLANGD-006`（缺 `;` 的诊断移回指令行），均在注册表内、可关闭 | `test_workarounds`；`typing-autosave` 的 D1、D2 |
| F13 | 编辑中的结构变化 2 s 静默后再计划；计划重启 2 s 合并；stand-in 进出不重启；clangd 未打开的文件改参数不重启；替身不被当作新提供者（hello 日志里第 1 次重启的真因） | `test_server`；`typing-autosave` 的 T6–T11 |
| F14 | `RestartGate` 按原因分预算，超限退避 1/2/4/8 分钟；用户重启与工具链切换不计 | `test_server` |
| F16 | 见下文"与方案的差异"1 | `typing-autosave`（0.0.4 在 T1 即全部请求无应答）；hello 副本线程 CPU 0 tick |
| F17 | clangd `--log=info` 进 4000 行内存环；事故目录（保留 20 个、7 天）含日志、编辑器/磁盘差异行、命令、fileStatus 时间线、线程 CPU；计划差异日志；workaround 前提守卫（WA-001/002）；状态按钮标题 | `test_server`、`test_process`；`typing-autosave` T3 |
| F10 | 并入 PR #25（S2 0.3.0，speak-agent）：`EnvelopeDiagnostic.path`；有 `data` 又有 `error` 的文档照常使用，每个未能描述的部分记为模型 issue `producer-partial`；mock mcpp 可给出这种文档；traceability S2-3.4-12/13 | `test_spec`；夹具 `mcpp-emit-partial`（0.0.4：用了数据但不报告） |
| F18 | 见 D 分支：`src/bundle/`（脱敏、zip、打包）、CLI/服务端/VS Code 入口、Collect Report 与 `cxxModules/report` 默认脱敏（S3-5.5-3） | `test_bundle`；夹具 `diagnostic-bundle`；CI 上传各平台问题包 |

**与方案的差异**（实现中发现，已按根因处理）：

1. **F16 扩为"磁盘文本对 clangd 是否安全"。** 按方案只查 `import hello.` 时，夹具暴露了第二种情况：自动保存把尚不存在的模块名
   （`import hello.e`）写到磁盘，clangd 从磁盘读到后同样卡死（UP-02 经 UP-14 的路径）。根因有三：H3 推迟 stand-in 的前提"正在输入的
   import 只在缓冲区里"被自动保存打破；计划对 inferred 模型的单元沿用加载时的 `requiredModules`，看不到正在编辑的 import；扫描器不认
   缺 `;` 的 import，而 clang 认（P1857）。已分别修正：编辑中的文件以缓冲区与磁盘的 import 合并计划，磁盘上已有的 import 立即得到
   stand-in；扫描器记录 `unterminatedImports`；磁盘检查同时覆盖"数据库里还没有、或 clangd 还没重读到"的模块，文件在 stand-in 被
   读到后（约 6 s）交还。自我 review 后收窄：这条只针对上一次计划之后新输入的 import（计划已见过的仍按原有机制处理），跳过 `std`
   与经模块清单解析的模块，且最多 30 s（计划始终不给单元时回到 0.0.4 的行为），避免打开文件时误隔离。
2. **F16 的"保存前已开始的构建"。** 夹具的输入方式（改动与保存几乎同时）让 clangd 在保存前开始的构建读到新磁盘内容。按方案的
   "隔离即可"不够：已开始的构建停不下来。现为先让它结束（1.5 s），仍未结束则重启 clangd 且不带该文件；Windows 上这一情形是崩溃，
   崩溃文件若已知磁盘不安全，同样只隔离到磁盘修好。
3. **F17.1 的"出事后临时切到 verbose"未做**：clangd 运行中不能改日志级别，需要重启；事故带 `info` 日志，需要 verbose 时按文档用
   `--log-level debug` 启动（design.md §7 已写明）。
4. F13 的"计划变回原样则取消重启"未做：取消需要让旧 clangd 重新与计划同步（打开/关闭文件），复杂度与收益不相称；以 2 s 合并、
   stand-in 与未打开文件不触发重启代替。

**验证**：dev 与 release profile 单测 29/29；`devtools check all`、`validate.py` 通过；Windows/macOS 交叉编译通过；本机 Linux
全部 conformance（CI 两部分清单 + 新夹具，共 50 个）通过；新夹具逐个对照 0.0.4 确认失败。hello 项目副本：`impo`/`expo` 从第一个字母起
有 `import`/`export module`；`import hello.` 写盘后 clangd 各线程 20 s 内 0 tick（0.0.4 约 2000）。

## 9. 追加：C++26 支持对齐（2026-09-26，用户要求并入 0.0.5）

调查结论（本机实测，clangd 23.1 + 语义工具包 libc++ 23 / 工具链 clang 22 + libc++ 22）：

| 问题 | 实测 | 处理 |
|---|---|---|
| 无构建描述的项目默认 `-std=c++23`（MSVC 族却是 `/std:c++latest` 即 C++26），C++26 库名（`std::saturating_add`）缺失 | `inferred-cxx26` 在 0.0.4 上失败 | 默认取读取它的编译器支持的最新标准：语义工具包、GCC ≥ 14、Clang ≥ 17（< 20 写作 `c++2c`）为 C++26，更老的为 C++23；工具包在单元未写标准时也取 C++26 |
| 同一项目混用 C++23 与 C++26：`std` 按代表单元（C++23）构建，C++26 单元 `import std` 直接失败（"C++26 was disabled in precompiled file"），经它导入的一切都丢失 | `compdb-mixed-standards` 在 0.0.4 上失败 | 计划第 5c 步：同一上下文中涉及模块的单元统一为其中最新的标准，普通单元保留自己的；报告 `plan.languageStandard/standardsSeen/standardsRaised`，状态 profile 增加 `standard`（S3，附加字段） |
| 用户看不到当前按哪个标准读 | — | 状态 profile 与报告给出；提升时写一行日志 |
| 契约（P2900）、反射（P2996） | clang 22/23 不认 `pre`/`post`/`contract_assert`（`-fcontracts` 不存在） | 上游能力缺口，不在 mcppls 能补的范围：文档写明；VS Code 注入语法为 `contract_assert` 与合约说明符位置的 `pre`/`post` 着色（纯外观）；服务端对 `contract_assert` 发 keyword 语义 token（clangd 可用时以 clangd 为准） |

其余 C++26 语言特性（包索引、`= delete("reason")`、占位符 `_`、`static_assert` 消息、`#embed` 等）以 clangd 23.1 的实现为准，mcppls
无需额外处理；标准库部分以构建所用的标准库为准。

## 不做的事

- 不删除 `-flto` 或其他链接参数（F1）。
- 不为 UP-13 做规避性改写：只止损，取栈与上游报告在 issue #24 跟踪。
- 不改写用户的文件（F16 只检测磁盘内容，不修改）。
- 问题包不自动上传，不含源代码全文与非白名单环境变量（F18）。
- mcpp 侧的 UP-M2/UP-M3 已合并提交为 mcpp-community/mcpp#699，在 issue #24 跟踪，本方案只做 F10 的适配。
