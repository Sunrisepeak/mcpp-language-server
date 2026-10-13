# 跨仓库优化方案：build.mcpp 的语义、输入时的 CPU、空闲时的空转

日期：2026-10-14。状态：**第三版：已实现**（§11 实施记录）。第二版的决定见 §9，自我 review 见 §10；第三版补充按维度的拆分与任务依赖（§0），以及实现中与方案不同之处（§11.3）。

## 0. 拆分：维度、任务与依赖

### 0.1 按维度

| 维度 | 要求 | 落在 |
|---|---|---|
| 架构 | 节流放在 mcppls 的 clangd 引擎适配层（不改 mcppls-clangd、不改 orchestrator 的路由）；配套扩展只读 mcppls 的 API，不复制候选表；上游缺陷只提 issue，mcppls 自己不依赖其修复 | A4、A6、B3、§7 |
| 稳定性 | 节流不得让请求永远挂起：被取代的回 `ContentModified`，文件被移交（关闭、搁置、clangd 重启）时等待的请求交给下一个引擎；生产者输入去重只比内容，删除总算变化 | A1、A4 |
| 优雅简洁 | 每项一个最小改动：A1 一张内容哈希表；A2 一张否定缓存；A3 只改解析函数和一处兜底；A4 一个 `Typing` 结构加定时器 | A1–A4 |
| 用户体验 | 输入时不再闪报错、风扇不再满转；`build.mcpp` 不再有误报；模块失败说清楚是哪个模块、为什么；一键关闭其他语言服务 | A3、A4、B1–B3 |
| 兼容性 | `mcppls.engine.diagnosticsWhileTyping: always` 保留 0.0.15 行为；mcpp-vscode 0.6.0 与 mcppls 0.0.16、mcpp-vscode 0.7.0 与 mcppls 0.0.15 都能工作（A6 的 `**/*.mcpp` 选择器、B3 的本地只读兜底） | A4、A6、B3 |
| 跨平台 | 新 fixture 用语义工具包（`--no-discover`），在 linux-x64、linux-arm64、darwin-arm64、win32-x64 上都跑；A3 的解析覆盖 Windows 路径；插件库只用 gcc 16 / llvm 22 libc++ / clang + libstdc++ 共同接受的 C++23 | A3、A4、C2 |
| 一致性 | 设置项经注册表生成文档与 package.json；三个仓库的版本、CHANGELOG、文档同步 | 各 PR |
| 无感升级 | 新设置默认 `auto`，无需用户操作；B1 的 `[mcpp-build]` 设置迁移写进 CHANGELOG；插件修复为补丁版本（源码兼容） | A4、B1、C1 |

### 0.2 任务与依赖

```
C1 ─┬─> C2(llvm 线)
    └─> C2(editor 线) ──(固定一个已发布的 mcppls)
A1, A2, A3 ──(相互独立)
A4: 验证 clangd 行为 -> 实现 -> 基准门槛 -> fixture
A6 ──(接口约定先行)──> B3（B3 对旧 mcppls 有兜底，不阻塞）
B1, B2 ──(依赖 mcppls 自 0.0.12 起的 .mcpp 关联)
发布顺序：mcpp-plugins 0.19.1 -> mcpp-index；mcpp-vscode 0.7.0；mcppls 0.0.16（三者互不阻塞）
```

三个 PR 并行实现；每个 PR 内部按上图顺序。

范围：三个仓库，每个仓库一个 PR。

| PR | 仓库 | 内容 |
|---|---|---|
| PR-A | mcpp-language-server（mcppls，本仓库） | 空闲空转的两个循环、模块失败的翻译、输入期间的 AST 节流、给配套扩展的接口 |
| PR-B | mcpp-community/mcpp-vscode | 不再把 `build.mcpp` 认成独立语言、与 mcppls 去重、「其他 C++ 语言服务」一键关闭 |
| PR-C | mcpp-community/mcpp-plugins | `json::value` 在 clang + libstdc++ 下的编译失败；CI 覆盖 gcc 16、llvm 22 与编辑器读取三种组合，约束插件库只用三者共同接受的 C++23 |

上游 mcpp 的相关问题只提 issue，不在本方案的 PR 内（§7，#807）。

## 1. 现象

1. VS Code 打开 qt-demo 的 `build.mcpp`：开着 mcpp-vscode 时报 `unknown mcpp::rules`（owner `mcpp-build`）；关掉 mcpp-vscode 后，改报 `Module file '…/module-hints/mcpp.rules.qt.pcm' not found`（owner `mcppls`）。
2. 不管项目大小，反复输入 `std::vec`、`std::arr`、`std::fin` 时风扇加速，`clangd.main` 占用高，中间的报错又多又变。
3. 关闭 mcpp-vscode 后，`build.mcpp` 的体验明显变好。

## 2. 复现环境与数据

- 环境：Linux x86_64；qt-demo 的副本（`mcpp build` 后 Qt 6.11.1 就位）；mcpp 2026.10.11.1；工具链 gcc 16.1.0；mcppls 0.0.15 release payload（mcppls-clangd 0.0.16）。
- 驱动：一个 LSP 客户端脚本，逐字输入，每个编辑后发送 VS Code 常见的一批请求：completion、semanticTokens/full、documentHighlight、codeAction、inlayHint、documentSymbol、foldingRange、signatureHelp。
- 被测文件：`src/main.cpp`（Qt 头文件、`import std`、`import nlohmann.json`）。

| 测量 | 结果 |
|---|---|
| 输入 `std::vec`、删除、`arr`、删除、`fin`（31 次编辑，间隔 150 ms，共 4.5 s） | clangd 4.4 CPU 秒（约一个核心满载）；mcppls 0.2 CPU 秒 |
| 同样的按键打在注释里 | clangd 6.6 CPU 秒：错误恢复与拼写纠正不是主因 |
| 同上，`--answer-cache off` | clangd 4.4 CPU 秒：answer 层的后台补全在这个节奏下不显著 |
| clangd trace（`CLANGD_TRACE`），输入窗口内 | BuildAST 12 次 × 157 ms = 1.9 s；Sema completion 21 次 × 108 ms = 2.3 s；其余合计 < 0.1 s |
| 只开 `build.mcpp`、不操作，60 s | `mcpp emit` 每 2 s 跑一次（"project model loaded again: unchanged"）；clangd 收到内部补全约 30 次/分钟 |
| 极小项目（只有 `import std`）输入 `std::ve` | 0.65 CPU 秒，单次补全 30–130 ms |

结论：输入时的 CPU 主要来自 clangd 每个编辑都完整重建一次 AST，再加上补全自己的一次解析。成本取决于当前文件包含和导入了什么，与项目大小无关。中间报错又多又变，就是每个中间版本的诊断。

## 3. 问题清单与根因

| # | 问题 | 根因 | 位置 | 证据 | PR |
|---|---|---|---|---|---|
| F1 | `build.mcpp` 开着 mcpp-vscode 时拿不到 mcppls 的语义 | mcpp-vscode 声明 `languages: [{ id: "mcpp-build", filenames: ["build.mcpp"] }]`。VS Code 中文件名匹配优先于扩展名匹配，mcppls 的 `.mcpp → cpp` 因此失效；mcppls 客户端的 `documentSelector` 只有 `cpp`、`c`，`build.mcpp` 根本不会发给 mcppls | mcpp-vscode `package.json` languages；mcppls `editors/vscode/src/extension.ts:320` | 两种状态下诊断的 owner 不同（`mcpp-build` 与 `mcppls`） | B1, A6 |
| F2 | `unknown mcpp::rules` 误报 | mcpp-vscode 的文本检查用 `\bmcpp::(\w+)` 取第一段名字，只比对引擎接口的名字表，不看 `import mcpp.rules.qt;` 带进来的命名空间 | mcpp-vscode `src/buildscript/analysis.ts:436` | 列位置正好落在两处 `mcpp::rules` 上 | B2 |
| F3 | clangd 编不出 `mcpp.plugins` | `struct value { std::vector<std::pair<std::string, value>> members; … get() { for (auto const& m : members) … } }`：clang 在 `import std`（libstdc++ 16 的 std 模块）下实例化 `pair<string, value>`，需要 `value` 的析构，析构又需要 `pair` 完整，形成循环，最终报 `incomplete type … used in type trait expression`。GCC 能容忍；同样的代码用 `#include` 时 clang 也能通过 | mcpp-plugins `src/plugins.cppm:277`（origin/main caf136b 仍存在） | 最小复现：`import std` + 上述结构，clang 22 + libstdc++ 16 报错；改成 `struct member` 并把 `get()` 移到类外后通过 | C1 |
| F4 | CI 没发现 F3 | Linux 的 fixture 36/40 用默认工具链 gcc（容忍这种写法）；其余 4 个固定 llvm 的是设备后端 fixture（hip、cuda、sycl、multi-rule）。mcpp 的 llvm 工具链在 Linux 上用 **libc++**，而 clang + libc++ 也能通过（已验证：`mcpp build --toolchain llvm@22.1.8` 下 qt-demo 的构建程序与 mcpp.plugins 编译通过）。macOS 是 clang + libc++，Windows 是 MSVC。**clang + libstdc++ 的 `import std`**——编辑器里的 clangd 读一切 gcc 工程时用的正是这个组合——从未被编译过，mcpp 也没有让 llvm 工具链改用 libstdc++ 的写法 | mcpp-plugins `.github/workflows/ci.yml` | `mcppls diagnostics build.mcpp` 能在无界面下复现 F3 的报错 | C2 |
| F5 | 用户看到 clang 原始的 `module-hints/…pcm not found`，而不是"哪个模块没编过、为什么" | `parse_module_failure` → `diagnostic_source` 取原因里第一个 `path:line:col:`。这次原因以 `In module 'std' imported from …/plugins.cppm:25: …/type_traits:1539:30: error:` 开头，匹配到的是 libstdc++ 头文件，对不上任何模块，失败被静默丢弃，`doomedModules_` 里没有它 | `src/engine/clangd/process.cpp:126`、`src/engine/clangd.cpp:2743` | 日志里没有 "could not build module"，也没有 `module-failed` 事件 | A3 |
| F6 | 空闲时每 2 s 跑一次 `mcpp emit` | `mcpp emit` 每次都重写 `~/.mcpp/cache/build-database/<h>/target/.build-mcpp/host/<h>/mcpp.lock`（构建程序的合成宿主包的 lock）：内容不变（md5 相同），mtime 变了。mcpp 又把它列在 `data.watch` 里，mcppls 看到变化就重新加载，重新加载又跑 emit，emit 又重写它，形成自触发循环 | mcppls `src/orchestrator/workspace.cpp:3741` `handle_watched_files`（`is_build_file("mcpp.lock")`）；mcpp `src/build/prepare/records.cpp:314`（无条件写）、`src/build/build_database.cppm:591`（列入 watch） | 日志：`didChangeWatchedFiles mcpp.lock (type 2)` 每 2 s 一次；手动 emit 前后 md5 相同、mtime 变化；`data.watch` 中确有该路径 | A1（上游 #807） |
| F7 | 空闲时 clangd 每 500 ms 收到一次内部补全，上限 30 次/分钟，永不停止 | `answer::trim` 不保存空结果，`prefetch_place` 因此一直选中同一个位置（`build.mcpp` 里的 `mcpp::rules::`，模块失败，结果永远为空） | `src/orchestrator/answer.cpp:501`、`answering.cpp:327`、`workspace.cpp:1323` | 60 s 空闲中 clangd 收到 30 次 `textDocument/completion`，`Scopes: ["mcpp::rules::"]` | A2 |
| F8 | 输入时 clangd 约一个核心满载 | clangd 的更新 debounce 自适应于重建耗时（这里约 120–157 ms），而人的输入间隔通常 150–250 ms，所以几乎每个键都触发 BuildAST。VS Code 的 semanticTokens、documentHighlight、codeAction、inlayHint 也都需要最新版本的 AST，会强制重建 | clangd TUScheduler；mcppls 原样转发 `didChange`（`src/engine/clangd.cpp:2126` `send_change_`） | trace：BuildAST 占输入窗口 45% | A4 |
| F9 | 关掉 mcpp-vscode 后体验变好 | F1 + F2：开着它时 `build.mcpp` 只有文本检查；关掉后 mcppls 接管，但又暴露出 F3/F5 | — | 使用中的观察 | B1, B2 |

## 4. PR-A：mcppls（目标版本 0.0.16）

### A1 生产者输入按内容去重（修 F6）

- 每次生产者运行后，记录模型 watch 列表中**具体文件**（不含 glob）的内容哈希。这些文件都很小（`mcpp.toml`、`mcpp.lock`、build.mcpp）。源文件的 glob 照旧走索引更新，不受影响。
- 收到 `didChangeWatchedFiles`（客户端的动态 watch，或 W9.3 的轮询兜底）时，内容哈希没变的构建文件和生产者输入直接忽略。删除（type 3）和新建（type 1）不做哈希比较。
- 自己的生产者运行期间及结束后 1 s 内，对生产者输出目录（`~/.mcpp/cache/build-database/…`）下的写入不做反应。哈希比对已经覆盖了这种情况，这一条是双保险。
- 测试：mockmcpp 增加一个模式，每次 emit 都用相同内容重写 lock 文件。fixture 断言打开文件、空闲 20 s 内生产者只运行 1 次。

### A2 预取不在空结果上重试（修 F7）

- 空结果或失败的位置记入否定缓存，键为 (uri, place, fingerprint)。文件文本或模型代数变化之前，同一位置不再预取。
- 文件的模块前置被判定失败（doomed）时，该文件不做预取。
- 测试：在一个前置模块失败的构建程序 fixture 中空闲 30 s，断言每个位置的内部补全次数 ≤ 1。

### A3 模块失败的翻译（修 F5）

- `parse_module_failure` 识别 `In module 'M' imported from <P>:<L>:` 前缀（可能多层嵌套），候选失败单元取 `<P>`。`<P>` 可能是单元里包含的头文件而非单元本身，所以先在 `moduleSources_` 里查，查不到再走下一条兜底。
- 兜底：失败单元映射不到任何模块时，不再丢弃。导入它的文件改发一条可读诊断："a module this file imports could not be built: <第一条 error>"。
- 指向 `module-hints/<M>.pcm` 的 `ast_file_not_found` 一律改写：已知失败时用 `module-failed`（沿用 0.0.13 W2 的措辞）；未知原因时说"clangd 没有为它构建 BMI，原因见日志"。
- 测试：用本次日志中的真实行作 `parse_module_failure` 的单元测试；扩展 `build-program-broken` fixture，让前置模块在 std 模块内部报错。

### A4 输入期间的 AST 节流（F8，在 mcppls 内处理，不改 mcppls-clangd）

clangd 支持 `didChange` 的扩展字段 `wantDiagnostics`（0.0.15 payload 中已确认存在这个字符串）。它没有 debounce 的命令行参数，所以节流放在 mcppls：

1. **连续输入期间不要诊断。** 同一文档两次编辑间隔小于 `Q` 时视为一段连续输入，转发 `didChange` 时带 `wantDiagnostics: false`。最后一次编辑后静止 `Q`，补发一次 `didChange`：版本号相同、`contentChanges: []`、`wantDiagnostics: true`。
   - `Q = clamp(1.2 × 该文件最近一次 BuildAST 耗时, 300 ms, 1000 ms)`，BuildAST 耗时 mcppls 已经从 clangd 日志里收集（`note_build_line`）。
2. **连续输入期间暂缓需要 AST 的请求。** semanticTokens、documentHighlight、codeAction、inlayHint、documentSymbol 等暂存；静止后每种方法只转发最新的一个，被取代的回 `ContentModified`（-32801，VS Code 会静默丢弃）。
   - completion、signatureHelp 不受影响：它们用 preamble 加自己的一次解析，不等 AST。
3. **设置项** `engine.diagnosticsWhileTyping`：`auto`（默认，按上述策略）或 `always`（0.0.15 的行为）。
4. **先做验证（第一步）：** 在 mcppls-clangd 上确认"相同版本、空 `contentChanges`、`wantDiagnostics: true`"会为最新文本建 AST 并发布诊断。如果不行，退而发送完整文本，版本号仍然不变。
5. **基准门槛：** 本次的驱动脚本收进 `tools/bench`，作为"输入"场景（qt-demo 形态的 fixture）。要求连续输入期间 clangd CPU 下降 ≥ 40%，停止输入后诊断在 `Q` + 一次 BuildAST 内到达。
   - 用户体验上的副作用：中间状态的报错不再闪动，停顿后才出现。
6. **mcppls 内部依赖"每个版本都有诊断"的地方，要一起改**，否则节流会被误判成 clangd 卡住：
   - 卡住检测（`awaitingDiagnostics_`、`schedule_stuck_check_`）与自旋检测（`spin_.sent`）：节流中的版本不计入等待。
   - 构建程序的判定（`check_build_program` 读 `coreDiagnosticsVersions`）：编辑 build.mcpp 时立即补发一次带诊断的 `didChange`，不等 `Q`。
   - 需要最新诊断的请求（pull diagnostics、MCP / CLI 的 `diagnostics`、`verify`）到达时立即补发，不等 `Q`。
   - 启动指标 "first diagnostics" 不受影响：首次打开不走节流。
7. **剩余成本：** 补全自己的一次解析（本次 21 × 108 ms）不在 A4 范围内。下一步的杠杆是让 clangd 少返回 `isIncomplete: true`，从而减少 VS Code 每个键重新请求的次数；单独立项，先测量。

### A5（可选）连续输入期间，被取消的补全不全部保留

`cancel_into_background` 只保留每个文档最新一个被取消的补全，其余真正取消。本机测量未见差异，所以在 `taskset -c 0-3` 下测量，有收益才做。

### A6 VS Code 客户端：对配套扩展的接口（配合 PR-B）

- `documentSelector` 增加 `{ scheme: 'file', pattern: '**/*.mcpp' }`。即使有别的扩展给 `.mcpp` 另指定语言 id，`build.mcpp` 也照样交给 mcppls，这也兼容已发布的 mcpp-vscode 0.6.0。服务端对计划读取的 `.mcpp` 文件，把 languageId 统一成 `cpp` 再交给 clangd。
- 扩展的 exports 增加 `otherCppFeatures()`，返回 `{ active: [{ extensionId, displayName, settable }], canRestore: boolean }`，复用 `conflictCandidates.ts` 的判定。配套扩展不必复制候选表。

## 5. PR-B：mcpp-vscode（目标版本 0.7.0）

### B1 不再把 `build.mcpp` 认成独立语言（修 F1）

- 删除 `mcpp-build` 语言及其 `filenames: ["build.mcpp"]`。`build.mcpp` 回到 `cpp`（由 mcppls 的 `.mcpp` 关联负责）。
- mcpp-vscode 自己的 build-script 能力改为注册在 `{ language: 'cpp', pattern: '**/build.mcpp' }` 上。
- 语法：`source.cpp.mcpp-modules` 继续注入 `source.cpp`，`source.mcpp-build` 的 grammar 和 `mcpp-build-language-configuration.json` 删除（cpp 自带的注释、括号配置等价）。
- 迁移：用户若写过 `"[mcpp-build]": {…}` 的语言专属设置，会不再生效；CHANGELOG 中说明改成 `"[cpp]"`。

### B2 mcppls 在时去重，不在时修正（修 F2）

- **mcppls 在时**（已安装、`mcppls.enable` 为 true、本工作区没有被 `mcppls.turnOffInWorkspace` 关掉，以 `lastStatus()` 可读为准）：关闭 `unknownSymbol` 诊断（R2 已确认），也关闭 `import` 后的模块补全和模块悬停（mcppls 按配置可见性提供，更准确）。
- 保留互补的部分：SPEC-007 的规则类检查（`rpathInLinkFlag`、`rawPathFlag`、`shellSyntaxInAction`、`actionNeedsRole`、`prepareNeedsOutputDir`）和 snippets。
- **mcppls 不在时**：`unknownSymbol` 按导入判断——文件中有 `import mcpp.<X>…;` 时，`mcpp::<X>` 不报。
- 测试：`test/artifacts.test.ts` 改为断言不存在 `mcpp-build` 语言；`test/buildscript/analysis.test.ts` 增加 `import mcpp.rules.qt;` + `mcpp::rules::qt::compile` 不报的用例。

### B3 「基本信息」→ 语言服务：「其他 C++ 语言服务」一行

| 状态 | 图标 | 文本 | 点击 |
|---|---|---|---|
| 有其他语言服务在工作（C/C++ IntelliSense、clangd 扩展、ccls） | `warning` | 「其他 C++ 语言服务：C/C++、clangd」 | 执行 mcppls 自己的 `mcppls.turnOffOtherCppFeatures`（它会询问作用范围，写的是 `C_Cpp.*` / `clangd.*`，不是 `mcppls.*`，符合 mcpp-vscode 的契约；ccls 没有开关，该命令会引导到扩展视图禁用它） |
| 只有 mcppls | `pass`，用 `disabledForeground` 着色 | 「仅 C++ Modules 在工作」 | 无 |
| mcppls 曾经关闭过它们 | 右键菜单 | 「恢复」 | `mcppls.restoreOtherCppFeatures` |

- 数据来源：A6 的 `otherCppFeatures()`；mcppls 较旧时退回本地只读检测（`extensions.getExtension` + `getConfiguration().get`）。
- 刷新时机：`extensions.onDidChange`，以及 `C_Cpp.intelliSenseEngine`、`clangd.enable` 的配置变化。
- `contract.ts` 的 CAPABILITIES 表增加这两条 forward 能力（`danger: "none"`，因为 mcppls 会自己询问范围）。

## 6. PR-C：mcpp-plugins

目标：插件库（`src/`）和规则包（`rules/`）只使用 **gcc 16、llvm 22（libc++）、clang + libstdc++（编辑器）三者都接受的 C++23**，并由 CI 逐条线保证。

### C1 修复 `json::value`（修 F3）

```cpp
struct value;
struct member;
struct value {
    enum class kind { null, boolean, number, string, array, object };
    kind                type = kind::null;
    std::string         text;
    std::vector<value>  items;
    std::vector<member> members;
    const value* get(std::string_view key) const;
};
struct member { std::string first; value second; };
inline const value* value::get(std::string_view key) const {
    for (auto const& m : members) if (m.first == key) return &m.second;
    return nullptr;
}
```

- 源码兼容：`members` 只在 `plugins.cppm` 内部使用（`get()` 和 `:382` 的 `out.members.emplace_back(std::move(key), std::move(v))`），`rules/` 没有用到；成员仍叫 `first` / `second`。
- `emplace_back(k, v)` 构造聚合 `member` 依赖 C++20 的圆括号聚合初始化，三种组合都支持；MSVC 由现有 Windows 线覆盖。
- 已验证三种组合：
  - clang 22 + libstdc++ 16 的 `import std`：预编译通过（原写法在同一命令下报错）；
  - `mcpp run --toolchain gcc@16.1.0` 与 `--toolchain llvm@22.1.8`：编译、运行通过，`get()`、`emplace_back`、`push_back({…})` 的结果正确。
- 发布：qt-demo 这类项目固定了 `plugins = "0.15.2"`，修复版本发布后需要升级版本号才能拿到。

### C2 CI：三条 Linux 线（修 F4）

| 线 | 工具链 | 标准库 | 覆盖谁 | 现状 |
|---|---|---|---|---|
| gcc | `gcc@16.1.0` | libstdc++ | 默认在 Linux 上构建的用户 | 已有（consumers job，36 个 fixture 用默认工具链） |
| llvm | `llvm@22.1.8` | libc++ | 选 llvm 工具链的用户；与 macOS 的 clang + libc++ 同族 | **新增** |
| editor | mcppls 自带的 clangd（clang 23）读 gcc 构建 | libstdc++ | 用 gcc 构建、在编辑器里打开 build.mcpp 的用户（F3 发生在这里） | **新增** |

- **llvm 线**：与 consumers 并列的 matrix 项，对工具链中立的 fixture 运行 `mcpp build --toolchain llvm@22.1.8`（或设 `MCPP_TOOLCHAIN`），有测试的跑 `mcpp test`。
  - 载荷 ABI 绑定 libstdc++ 的 fixture（Qt 官方二进制，如 qt-consumer）不能完整链接，改跑 `mcpp build --configure-only --toolchain llvm@22.1.8`。已验证这一步会编译并运行构建程序：rules-qt 的 ABI 警告由构建程序发出，退出码 0。构建程序与插件库的代码因此仍在 llvm 下编译。
  - 自带设备工具链的 fixture（hip、cuda、sycl、multi-rule）不覆盖工具链，保持现有配置。
- **editor 线**：安装固定版本的 mcppls（release 压缩包，版本写在 workflow 中；R3 已确认依赖 mcppls 发布）。在 gcc 线构建之后，对每个带 build.mcpp 的 fixture 运行 `mcppls diagnostics build.mcpp --format json`，`counts.error > 0` 即失败，并把诊断写进 job summary。
- **约束写进文档**：在 CONTRIBUTING 中写明"插件代码只用三条线共同接受的 C++23"，并列出已知的坑：
  - 自引用的容器元素不要用 `std::pair` / `std::tuple`，即本次的 F3；
  - 不依赖某个编译器独有的宽松行为与扩展；
  - 构建程序避免 `std::println`（macOS 14 的链接问题，ci.yml 已有记录）。
- 成本：llvm 线与 gcc 线并行，共享 sandbox 缓存的 key 前缀；editor 线只做诊断，不重复构建。

## 7. 上游 mcpp：issue #807（R5，已提交）

mcpp-community/mcpp#807："emit build-database rewrites the build program's host mcpp.lock on every run and lists it in data.watch, so a watcher re-runs emit forever"。

- 复现：两次 `mcpp emit build-database --format json` 之间，`.build-mcpp/host/<h>/mcpp.lock` 的 mtime 变化、md5 不变，且该路径在 `data.watch` 中。
- 位置：`src/build/prepare/records.cpp:314` 无条件 `lockfile::write`；`src/build/build_database.cppm:591` 对构建程序成员把合成包的 `mcpp.lock` 列入 watch。
- 建议：内容变化时才写（write-if-changed）；可选，不把合成包的 lock 列入 `data.watch`。
- A1 让 mcppls 不再依赖这个修复。

## 8. 顺序与兼容

1. **C1**：最小改动，发布后用户升级插件版本即可用。
2. **A1–A3**：三个 bug，各配 fixture。
3. **B1–B3**，同时做 **A6**：B1 依赖 mcppls 的 `.mcpp` 关联（0.0.12 起就有）；B3 在没有 A6 的旧版 mcppls 上走本地只读检测。
4. **C2**：llvm 线随时可加；editor 线依赖一个 mcppls 发布，包含 A3 时诊断更可读，不包含也能用。
5. **A4**：先验证 clangd 的行为，再实现，以基准门槛决定是否合入；**A5** 视测量结果决定。
6. **#807**：与上述各项相互独立。

## 9. 已确认的决定

| # | 决定 | 结论 |
|---|---|---|
| R1 | A4 默认开启（`auto`） | 确认 |
| R2 | mcppls 在时，mcpp-vscode 完全关闭 `unknownSymbol` | 确认 |
| R3 | mcpp-plugins 的 editor 线依赖 mcppls 发布 | 确认 |
| R4 | A6 的 `**/*.mcpp` 选择器让 mcppls 接管所有 `.mcpp` 文件 | 确认 |
| R5 | 向 mcpp 提 issue | 已提交 #807 |
| R6 | mcpp-plugins CI 覆盖 gcc 16 与 llvm 22，插件代码限于共同接受的 C++23 | 确认，落在 C2 |

## 10. 自我 review

| # | 发现 | 处理 |
|---|---|---|
| S1 | 第一版说 llvm fixture 用 libc++ 是"推断" | 已实测：`--toolchain llvm@22.1.8` 在 Linux 上是 libc++，且 clang + libc++ 编得过 F3 的代码。**只加 llvm 线抓不到 F3**，所以 C2 保留 editor 线，三线缺一不可 |
| S2 | C1 第一版只在 clang + libstdc++ 下验证过 | 已在 gcc 16 与 llvm 22 下用 `mcpp run` 补验证，覆盖 `emplace_back` 与 `push_back({…})` 两种写法 |
| S3 | A4 会让"每个版本都有诊断"的假设失效，mcppls 自己的卡住检测、自旋检测、构建程序判定可能误判 | 补进 A4 第 6 条，列明要一起改的位置和立即补发的情况 |
| S4 | A4 只节流 AST，补全自己的解析（约一半成本）仍在 | 写明剩余成本与下一步的杠杆（`isIncomplete`），不夸大 A4 的收益；门槛按 trace 中 AST 占比定为 ≥ 40% |
| S5 | A1 若对 glob 或删除也做哈希，会漏掉真实变化 | 限定为具体文件与 changed 事件 |
| S6 | A3 的 `imported from <P>` 中，`<P>` 可能是头文件 | 先查 `moduleSources_`，查不到走兜底诊断，不再静默丢弃 |
| S7 | B1 会让用户已有的 `[mcpp-build]` 设置失效 | CHANGELOG 中写明迁移方法 |
| S8 | F6 的循环是在驱动脚本（无动态 watch，走轮询兜底）下观察到的 | VS Code 的 watcher 同样会对 mtime 变化报告 changed，预计同样发生；A1 的 fixture 两条路径都测（已实现：`mcpp-emit-watch` 与 `--no-dynamic-watch` 两种跑法）。日志里持续出现 "loaded again: unchanged" 即是此现象 |
| S9 | `build.mcpp` 的 VS Code 侧问题依赖 PR-B 才彻底解决 | A6 先让 mcppls 在 mcpp-vscode 0.6.0 下也能拿到 build.mcpp；在 PR-B 发布前，0.6.0 的文本检查与 mcppls 的诊断会同时出现 |
| S10 | qt-demo 固定了插件版本 | C1 发布后需要升级；修复是否回补到 0.15.x 由 mcpp-plugins 的发布策略决定 |

## 11. 实施记录

### 11.1 PR-A：mcppls 0.0.16

| 项 | 实现 | 证据 |
|---|---|---|
| A1 | `Workspace::Impl::describedInputs`：构建文件与模型中具体（非 glob）watch 项的内容哈希。生产者运行前记录（覆盖），模型采用后补记未知的文件（不覆盖）；`handle_watched_files` 对内容未变的 changed / created 事件不重新加载，deleted 总算变化 | `mcpp-emit-watch` 新增 W0：mockmcpp 的 `rewriteEachRun` 每次运行用相同内容重写 `host/mcpp.lock`。动态 watch 与 `--no-dynamic-watch` 两种跑法都通过；0.0.15 两种跑法都失败 |
| A2 | `answering::Layer::fruitless_`：预取得到空结果或失败的位置（K1）连同"指纹 + 变化计数"记下，`prefetch_place` 跳过；任一文件变化或计划变化后重新可选 | `test_answer`：空结果后选下一个位置，`changed()` 后回到原位置 |
| A3 | `parse_module_failure` 剥去 `In module 'M' imported from <unit>:<line>:` 与 `In file included from <unit>:<line>:` 前缀（多层），失败单元取第一个 `<unit>`，原因附上第一条 error（截去源码摘录）；映射不到模块时记入 `unplacedFailures_`（按导入者），导入者的 `ast_file_not_found` 改写为可读诊断；什么都不知道时说"clangd 没有为它构建 BMI，原因见日志"（`module-not-built`） | `test_server`：真实日志行（路径已匿名化）、Windows 多层嵌套、`In file included from`；新 fixture `module-fails-in-import`（四个平台）：0.0.15 报 `ast_file_not_found`，0.0.16 报 `module-failed` 并说明原因 |
| A4 | clangd 引擎适配层 `Typing`：除构建程序与模块单元外，文件的 `didChange` 带 `wantDiagnostics: false`（文本未变的 `didChange` 照原样转发）；静止 `Q = clamp(1.2 × 上次构建, 300 ms, 1 s)` 后发送同版本、空 `contentChanges`、`wantDiagnostics: true`；期间 semanticTokens / documentHighlight / codeAction / inlayHint / documentSymbol / foldingRange / documentLink / codeLens 每种只留最新一个，被取代的回 `ContentModified`；`textDocument/diagnostic` 立即结束静止；关闭、搁置、clangd 重启时等待的请求交给下一个引擎。设置 `mcppls.engine.diagnosticsWhileTyping`（`auto` / `always`） | 先验证 clangd：`wantDiagnostics: false` 不发布；同版本空变更 + `true` 发布最新文本的诊断，包括 AST 已为请求建好的情况。新 fixture `typing-pause`（四个平台）：21 个键期间 0 次发布（0.0.15：15 次），最后版本 5 s 内有诊断，最后一个 semanticTokens 有结果。基准（qt-demo 形态，`std::vec` 删 `arr` 删 `fin`，每键 VS Code 的一批请求，间隔 200 ms，三轮）：clangd CPU 5.23–5.36 s → 2.90–3.06 s（−43%），BuildAST 16 → 3，semanticTokens 14 → 1 |
| A5 | 未做 | 本机测量未见差异（§4 A5 的前提），保持可选 |
| A6 | 客户端 `documentSelector` 增加 `{ scheme: 'file', pattern: '**/*.mcpp' }`；服务端对 `.mcpp` 统一 languageId 为 `cpp`；扩展 API 增加 `otherCppFeatures()` | `suite-conflicts` 两种回答下断言 `otherCppFeatures()` 的 active 与 canRestore |

### 11.2 PR-B：mcpp-vscode 0.7.0

mcpp-community/mcpp-vscode#18，已合入。B1–B3 按方案实现；VS Code 1.132 中的本地验证：`build.mcpp` 的 languageId 为 `cpp`，诊断只来自 mcppls 与规则检查，没有 `mcpp-build` 来源；有 clangd 扩展时「其他 C++ 语言服务」一行为警告图标且可点击，关闭后为暗色的通过图标、不可点击。

### 11.3 与方案不同之处

| 项 | 方案 | 实际 | 原因 |
|---|---|---|---|
| A1 | created（type 1）不做哈希比较 | 已知内容相同的 created 也忽略 | 经重命名写入（原子写）的文件会被报告为 created；轮询兜底下首次出现的 lock 也是 created。从未见过的文件仍按新文件处理 |
| A2 | 前置模块失败的文件不预取 | 不单独判断 | 否定缓存已把每个位置限制为每次变化一次；判断 doomed 需要引擎新增接口，收益不抵复杂度 |
| A4 | 只暂缓 semanticTokens 等五类 | 另加 foldingRange、documentLink、codeLens | 三者同样要求最新 AST |
| A4 | 只豁免构建程序 | 另豁免模块单元，以及文本未变的 `didChange` | `typing-import`、`typing-autosave` 回归：编辑器重发的同文本 `didChange` 被延后到静止时才到 clangd，恰逢另一个模块单元正输入到一半，导入者的 preamble 以文本方式导入该模块重建，之后 10 s 的输入检查保持期内不再更新。模块单元在每个版本上构建，导入者才能随之更新 |
| B1 | 删除 `mcpp-build` 语言 | 另把 `build.mcpp` 声明为 `cpp` 语言的文件名 | mcppls 被禁用时 `build.mcpp` 仍按 C++ 打开 |
| B2 | 诊断 owner 保持 | 改名为 `mcpp-build-script` | 与已删除的语言 id 区分 |
| B3 | 一个 forward 能力 | 拆为关闭、恢复两个能力 | 旧的单一能力在关闭命令缺失时会退而执行恢复 |
