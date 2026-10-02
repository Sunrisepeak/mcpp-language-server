# mcppls 0.0.10 方案：让模块缓存有界、可见、可清 —— 从 mcppls 侧根治 clangd 模块缓存的无限增长（C-7 … C-13、X-6）

状态：方案第 6 版（待 review）· 2026-10-03 · 基于 mcppls `main` **0.0.9**（2026-10-02 发布）
目标版本：**0.0.10**

第 1 版 → 第 2 版（按 2026-10-02 的 review 决定）：

- **D1–D4、D6–D8 按推荐定稿**（见 §11）；
- **D5**：基础修复照旧（C-9 回收 + 24 h grace），**并新增 C-13**：把缓存数据做到状态栏，点状态栏打开可视化面板，
  面板里有清理按钮，让开发者随时自己触发；
- **上游部分不再单独成文、也不向上游反馈**：只在 mcppls 仓库的 **issue #24**（上游缺陷唯一登记处）留一条评论，
  草稿并入本文 §8；mcppls 自己 workaround。

第 2 版 → 第 3 版（按 2026-10-02 的第二轮 review）：

- **D9 用 v2**：面板是真正的 **webview**（明细 + 控制按钮 + 克制的趣味动画），不再是 QuickPick 列表；
- **D10 只有一个状态栏块**：复用现有 `mcppls.statusBar`，缓存作为它的**分段**，
  用**长度预算**（默认 36 字符）与**分级**解决"提示太长不优雅"；装不下的进 tooltip；
- **面板成为控制枢纽**：清理 / 预演 / 重置 / 重启引擎 / 重启服务端 / 抓日志 / 打开日志 / **打开日志目录** / 设置，
  绝大多数复用既有命令，只有"清理/预演/打开日志目录/打开面板"是新的；
- **D11 定稿**，并新增 D12（长度预算默认 36）、D13（动画开关 + `prefers-reduced-motion`）、
  D14（webview 只当视图，扩展侧校验后转发）。

第 3 版 → 第 4 版（按 2026-10-02 的第三轮 review）：

- **D12 / D13 / D14 定稿**；
- **面板改走“简洁优雅”**：四条克制原则（**一个图**、**一个主按钮**、**默认一屏**、**四层信息**），
  去掉环形图与多图布局，改成一条横向堆叠条 + 一条微型趋势线；明细（最大模块 / 实例 / 每 context 分解）默认折叠；
- **功能一个不少，但分量分层**：主操作 `Sweep cache` → 次级（维护 3 + 日志 3）→ 链接（设置 / 文档 / 开源仓库 / 新建 issue）；
- **补上“方便”的那几个入口**：一键进设置页（`@ext:` 过滤）、打开开源仓库、新建 issue（**复用已有的 `issueUrl.ts` 预填**）、
  抓取日志（**复用已有的诊断包，包里已含报告**）、打开日志目录（新命令，路径由服务端给）；
- **反馈给了三步提示**：① 抓日志 ② 新建 issue ③ 拖 zip 进 issue；**不自动上传、不内置日志阅读器、不代替 GitHub 表单**。

第 4 版 → 第 5 版（按 2026-10-02 的第四轮 review）：

- **UI 不回显引擎**（D17）：标题行不写 `clangd 23.1.0`——引擎名/版本是实现细节，mcppls 之后可能换引擎；
  标题行改给状态与规模（`● Ready · 176 units · 48 modules`），引擎与 profile 移入 `详情 ▾`，
  按钮文案写 `重启引擎`（命令 id 不变），状态栏同样不带引擎名。**但给支持用的事实不隐藏**：
  `详情 ▾`、agent 提示词、issue 预填里都带引擎名与版本。
- **面板补上“索引情况”**（D18）：状态行给 `state` + 模型来源 + 模块准备进度（服务端已有的 `{done,total}`），
  索引只给**布尔**；没有数字就不编数字（原因见 §C-13.3：服务端未解析背景索引进度，且 clangd 的索引进度令牌与
  服务端注册的令牌会撞，日志里就有 `Progress handler for token backgroundIndexProgress already registered`）。
- **“反馈”改成“开源”，核心是给本地 agent 的提示词**（D19）：`复制 Agent 提示词`（只读排障指令 + 环境事实 +
  假设清单 + 安全约束 + 输出格式）、`复制 issue 提示词`（把结论整理成 issue 草稿，先给人看）；
  步骤提示**常显一行 + 悬停给完整四步 + 点击后原地回执**。**不自动上传**：日志不出本机，发布前必须经用户确认。

第 5 版 → 第 6 版（按 2026-10-03 的 review 决定）：

- **D9 改定：不做 webview 标签页、不做侧边栏**。UI 收敛为两件原生的事：**悬停状态栏出只读卡片**
  （大数字 + 文本堆叠条 + 四类数字 + 上次清理），**点击状态栏弹出 QuickPick 枢纽**（操作菜单）。
  理由：期望形态是"从底部状态栏悬浮的小面板"，而 VS Code 没有锚定状态栏的浮窗原语（核心的状态栏弹出菜单
  不开放给扩展）——tooltip 的位置最接近（就浮在状态项正上方），QuickPick 是唯一的点击弹出菜单；
  顺带删掉整个 webview 基建（`tsconfig.webview`、CSP/nonce、无障碍 E2E、harness 连带），扩展保持零 webview。
  服务端三个接口（C-13.1）与 UI 形态无关，以后要面板可零协议改动地加回。
- **QuickPick 枢纽带图标与分组**（D20）：每条操作以 codicon 开头（`$(clear-all) 清理缓存`…），
  用 `QuickPickItemKind.Separator` 分成 `缓存 / 清理 / 维护 / 日志 / 开源` 五段，明细走多级下钻。
- **落实 review 拍板的八处实现修正**：C-9 回收挪出启动路径（并入后台清扫 `instances → copies → budget`）；
  Agent 提示词单一来源下沉服务端（`cxxModules/cache` 加 `prompts`，CLI 同一函数渲染）；
  mtime 上界定义为"正在用该缓存根的最早一代引擎的启动时刻，没有则 now"（免持久化、崩溃后仍正确）；
  off 状态点击保持一键开启（"点击目标恒定"按状态算）；设置名统一 `mcppls.cache.totalBytes`；
  全局预算多进程并发不加锁（写明后果与边界）；死 guest 目录回收挂 10 s renew tick（先原子改名再后台删）；
  副本判定加"同目录存在去戳同名 `.pcm`"兜底（消灭"模块真叫这名字 → 每次重启重编"的假阳性循环）。
- **D17 / D18 / D19 定稿**（按推荐）。

依据：

- **真实案例**：`mcppls-cache-20261002-222834.zip`（Windows 11、clangd 23.1.0、项目 GalTranslPP、mcppls 0.0.9）。
  逐条分析见 [.agents/reviews/mcppls-cache-20261002-cause-analysis.md](../reviews/mcppls-cache-20261002-cause-analysis.md)；
  收集脚本 [.agents/reviews/mcppls-cache-collect.ps1](../reviews/mcppls-cache-collect.ps1)。
  关键数字：26.5 小时长到 **64.36 GiB**；模块产物 68.9 GB 里**只有 2.03 GB 是本体**，**97.1% 是重复副本**；
  `instances/` 三个孤儿目录占 **94.5%**；保留日志里 **219 次** clangd 异常退出（≈305 MB/次）。
- **clangd 侧机制**（逐行读过 `llvmorg-23.1.0`）：`clang-tools-extra/clangd/ModulesBuilder.cpp`
  —— 布局 `<项目根>/.cache/clangd/modules/<源文件名>-<哈希>/<命令哈希>/`（§46-65）；
  copy-on-read 副本 `getCopyOnReadModuleFilePath`（§198-209）、只在 `~CopyOnReadModuleFile` 删除（§437-461）；
  唯一回收是 `garbageCollectModuleCache()`，阈值 `--modules-builder-versioned-gc-threshold-seconds`
  默认 **3 天**、按 **atime**（§40-44、§986-1018）；GC 由 llvm/llvm-project#193973 引入（main，2026-04-24 合入）。
- **本仓库现状**：`C-2`/`RD13`（每 unit 留最新 2 个命令目录）、`C-4`/`RD12`（clangd 启动前无条件清 `.locks`）、
  `C-5`（租约记 pid + 进程启动时间）、0.0.7 计划 §9.7「每工作区 4 GB、合计 16 GB，超出按最近使用淘汰」、
  0.0.7 计划 T12（重置缓存的编辑器命令）。
- **代码事实**（本次核对）：
  - `src/engine/clangd/bmi.cpp:26-47` `module_of_bmi()` 已按三段式时间戳形状识别副本名（模块名含 `-` 也不误判）；
  - `src/engine/clangd.cpp:1618-1626` clangd 启动前清 `.locks` + `prune_module_builds_()`，注释已论证"此刻没有 clangd 用这棵缓存"；
  - `src/cli/cache.cpp:38-45` `directory_bytes()`，`:49-96` `clean()`/`prune()`，`:146-148` 那句"最多两份"的误导提示；
  - `src/orchestrator/instance.cpp:75-79` `owner_gone()` 需要 `/proc`，**Windows 上恒为 false**；
  - `modules/platform/src/process.cpp:544-568` `process_alive()`、`:572-599` `cpu_seconds()` 的 Windows 分支**直接 `return std::nullopt`**；
  - `src/orchestrator/instance.cppm:9-10` `LEASE_RENEWAL { 10s }` / `LEASE_EXPIRY { 30s }`；
  - `modules/platform/src/fs.cpp:100-103` `remove_all` 吞错误码；
  - 服务端命令：`src/server/session.cpp:284` 处理 `mcppls.resetCache`，`src/orchestrator/routing.cpp:155-159` 宣告命令列表；
  - 编辑器：`editors/vscode/src/status.ts:138` 已有状态栏项（`mcppls.statusBar`，点击 = `mcppls.showLogs`）与
    `LanguageStatusItem`；`cacheReset.ts` 定了"扩展命令 id ≠ 服务端命令 id"的约定；`test/{unit,suite,suite-stress}` 三套测试；
  - 规范：S3 现有 id 到 `S3-4-28`，命令到 §5.6；§7 规定"新版本只增加可选字段与新消息"；
    `docs/specs/CHANGELOG.md` 与 `conformance/traceability.json` 是规范变更的配套件。

编号：缓存为 **C-7 … C-13**（接 0.0.7 计划的 C-1 … C-6），平台为 **X-6**（接 X-1 … X-5），
场景测试为 **U-***，上游登记为 **UP-24**（issue #24 的评论）与 **WA-CLANGD-011**（`workarounds.cpp`）。
证据等级：**已验证**（有实测数据）、**代码确认**（读代码可证）、**推断**（待验证）。

---

## 0. 摘要

### 0.1 问题的一句话

clangd 把 BMI 的 **copy-on-read 副本**（每个 30~40 MB）留在 mcppls 给它的缓存目录里，**进程被打断就不删**；
它自己的 GC 要等 **3 天**、还按 Windows 上不可靠的 atime 判断。于是"崩溃越频繁 → 垃圾越多"，
而 mcppls 既**没有把 clangd 的中间产物当成自己的缓存来管**（`--prune` 只删整个命令目录，删不到目录内部的副本），
也**从不回收孤儿 `instances/<token>` 目录**（本例 94.5%），用户还**看不见**这件事（只有一句"最多两份"的误导提示）。
26.5 小时 64 GiB，就是这些乘起来的结果。

### 0.2 根因修复的定位

**"根本修复"落在 mcppls，而不是等 clangd 修**，理由是责任边界已经在 `RD12` 确立过：

> mcppls 把 `<context>/cdb` 交给 clangd 当 `--compile-commands-dir`，因此 `<cdb>/.cache/clangd` 是
> **mcppls 拥有、clangd 使用**的目录。mcppls 已经在这里无条件清 `.locks`（C-4/RD12），
> 只是还没有把同一套所有权延伸到 **副本**、**容量**、**可见性**。

因此本方案做四件事：

1. **每一代 clangd 结束后，把上一代留下的副本清掉**（C-7）——那 97.1% 的根治；
2. **给整棵缓存一个预算**（C-8：4 GiB/工作区、16 GiB/全局），超了按 LRU 淘汰副本；
3. **孤儿实例目录自我回收**（C-9）+ Windows 进程身份（X-6）；
4. **让它可见、可清**（C-13）：状态栏一行数字，悬停出只读卡片，点击弹出清理菜单（QuickPick，图标 + 分组），菜单里有清理按钮。**不重启引擎、不重新编译。**

### 0.3 效果预估（按本案数据）

| 项 | 现在 | 方案落地后 |
|---|---:|---:|
| 单工作区缓存峰值 | 64.36 GiB（26.5 h） | ≤ 4 GiB（可配），稳态 ≈ 本体 2 GB |
| 副本数量 | 6837 个 `.pcm` / 256 目录（最多 127 份/目录） | 每目录 ≤ 1 份副本（运行中），启动后 0 份残留 |
| 孤儿实例目录 | 3 个 / 60.80 GiB，永不回收 | 下一次启动或 `cache --prune` 后回收；悬停卡/枢纽上可见 |
| 用户可见性 | 一句误导提示（"最多两份"） | **一个**状态栏项（分段 + 长度预算 + 分级）+ 悬停只读卡片（大数字 + 文本堆叠条 + 四类数字）+ 点击弹出的 QuickPick 枢纽（图标 + 分组） |
| 谁能回收 | 只有 `cache --clean`（整棵删，需重建） | 启动自动 + 菜单按钮 + `cache --prune` + 命令（**不重建**） |
| 手动入口 | 命令面板里 6 个零散命令 | QuickPick 枢纽：清理/预演/重置/重启引擎/抓日志/开日志/开日志目录/开缓存目录 + `开源` 区（复制 agent 提示词 / 新建 issue / 仓库 / 文档 / 设置） |
| Windows 进程判活 | `process_alive()` 恒 `nullopt`，`owner_gone()` 恒 false | 有创建时间的真实身份（X-6） |

---

## 1. 设计目标与非目标

**目标**

1. **有界**：任何工作区的模块缓存不得超过配置上限；全局不超过配置上限。
2. **自愈**：用户不需要知道缓存布局。服务端每次启动都回到预算内；不需要人工 `--clean`。
3. **可见可清**：状态栏有数字，悬停卡有分解，枢纽有按钮，开发者可随时主动清理（C-13）。
4. **不白花时间**：清扫只删"副本"与"已被取代的命令目录"，**绝不删当前命令的已发布 BMI**（`<module>.pcm`）——
   删副本的成本是**一次文件拷贝**，删本体才是**重新编译 30~40 MB 的模块**。
5. **不碰活着的 clangd**：任何删除都发生在"被删的东西没有引擎在用"的前提下（启动前 / 无 `owner.lease` 的其它工作区 / mtime 早于上一代停止时刻）。
6. **失败可见**：删不掉要说出来（`fs::remove_all` 现在吞错误）。
7. **跨平台**：Windows 上判活、判死、回收、卡片与枢纽都要真的能用（现在前者都不行）。

**非目标**

- 不修 clangd 的 Build AST / preamble 崩溃（上游）；本方案让"崩了也不再长胖"。
- 不改缓存位置（`D:\mcpplsCache` 由 `MCPPLS_CACHE_DIR` 决定，是用户选择）。
- 不改 `mcppls cache --clean` / `mcppls.resetCache` 的语义（整棵删 + 重启，仍是"大锤"）。
- 不向上游提 issue/patch：#24 只**记录**，mcppls 自己 workaround（§8）。
- **不做第二个状态栏项**（D10）：缓存是现有 `mcppls.statusBar` 的一个分段；装不下就进 tooltip。
- **不做 webview 标签页、不做侧边栏/活动栏视图**（D9，第 6 版改定）：UI 只用两件原生的事——悬停 tooltip
  只读卡片 + 点击 QuickPick 枢纽；因此也不引任何图表库/前端框架，扩展保持零 bundler、零运行时依赖、零 webview。
- **不做仪表盘**：卡片与枢纽只回答”缓存多大、要不要清、出问题怎么反馈”三件事。

---

## 2. 机制设计

### C-7 启动前清扫"版本化副本"（根治 97.1%）

**判定 `is_versioned_copy(fileName, directory)`**：复用已有的形状解析，不用正则。
`src/engine/clangd/bmi.cpp` 的 `module_of_bmi()` 已经能把"`-YYYYMMDD-HHMMSS-<序号>` 三段"从名字尾部剥掉
（且模块名里带 `-` 也不会误判）。新增导出：

```cpp
// mcppls.engine.clangd.bmi
bool is_versioned_copy(std::string_view fileName, std::string_view directory);
// 形状匹配（module_of_bmi(name) != 去掉 .pcm 的名字）**且**同目录存在去戳后的同名 .pcm。
// clangd 的 copy-on-read 副本必然写在它拷贝的本体旁边；本体不在旁边，就当它是一个真的模块名，不删——
// 这消灭了"用户模块恰好叫 foo-20260101-120000-1 → 每次启动被删 → 每次重启重编"的假阳性循环。
```

**清扫范围**：给定一个缓存根（`<cdb>/.cache/clangd/modules`）递归，删除所有 `is_versioned_copy()` 为真的文件。
保留：`<module>.pcm`、目录结构（命令哈希目录必须留，删了就要重编）、`.locks/` 交给 C-4。
报告（C-10）、预算淘汰（C-8）与清扫（C-7/C-13）用**同一个**谓词，"报的"与"删的"不漂移。

**触发点**

| 时机 | 位置 | 安全性依据 |
|---|---|---|
| 每次启动 clangd 前（**异步**） | `src/engine/clangd.cpp:1618-1626` 的 `clear_module_locks` 旁 | 同一处注释已论证"此刻没有 clangd 用这棵缓存"（C-4/RD12） |
| `mcppls cache --prune` | `src/cli/cache.cpp:71-96` | 只对没有 `owner.lease` 的工作区（现有前提不变） |
| 清理菜单 / `mcppls.sweepCache` | C-13 | 见 C-13 的安全前提（mtime 上界；不停引擎） |

**"异步 + mtime 上界"（关键取舍）**：36 GB / 6800 个文件的删除在 Windows 上要几十秒到几分钟，
**不能阻塞 clangd 启动**。因此：

- 清扫在后台线程做（先例：incidents 的 prune、`ToolchainVerifier`）；
- 只删 **mtime 早于清扫上界** 的副本。上界的定义（第 6 版定稿，免持久化）：**正在使用该缓存根的最早一代
  引擎的启动时刻；没有引擎在用 → now**。启动路径的清扫发生在新代启动之前，自动落到 `now`；
  引擎运行中的交互清扫（C-13）用当前代的启动时刻——语义上就是"上一代停止时刻"的安全近似，
  且服务端自己崩溃重启后依然正确（上界永远是内存里现成的值，不需要落盘）。这样即使新一代 clangd
  已经在写新副本，也绝不会被误删；
- 删除量、耗时、失败数记入日志与 `report`（`record_event("cache-swept", {...})`）。

**成本**：删掉的副本会在该模块下次被需要时由 clangd 重新 `copy`（一次文件拷贝），**不是重新编译**。

### C-8 容量预算（4 GiB/工作区、16 GiB/全局，LRU）

**常量**（做成设置，见 D3）：

| 设置 | 默认 | 含义 |
|---|---|---|
| `mcppls.cache.maxBytes` | `4G` | 单个工作区缓存上限 |
| `mcppls.cache.totalBytes` | `16G` | 所有工作区缓存合计上限 |
| `mcppls.cache.instanceGraceHours` | `24` | 无 `instance.json` 的旧版本实例目录，多久未动可删（C-9） |

**淘汰顺序**（每一步都要求被删的东西"没有 clangd 在用"）：

1. 删版本化副本（C-7）；
2. 删"被取代的命令目录"（既有 `stale_module_builds(dir, 2)`，C-2/RD13）；**仅在**引擎启动路径、`cache --prune`，
   以及 `mcppls.sweepCache` 显式给出 `categories: ['staleCommands']` 且该 context 没有引擎在用的时候（见 C-13 的边界）；
3. 仍超上限 → 按 **最后使用** 淘汰**版本化副本**（本工作区自己的树，或全局范围内没有活实例的工作区）；
4. 仍超上限 → **不删本体**，改为报告 + 状态栏颜色 + 枢纽动作（D2）。

**"最后使用"的定义**（避免实现随意）：取三者中最新者——该工作区 `instance.json`/`owner.lease` 的心跳、
该工作区 `model.*.json` 的 mtime、整棵树里最新的文件 mtime。全局淘汰只在"没有活实例"的工作区之间进行。

**触发**：服务端启动后的一次后台任务（引擎启动前，启动即干净，顺序 `instances → copies → budget`）；`mcppls cache --prune`；清理菜单按钮；
不做常驻定时器（实例目录的日常回收挂既有租约 tick，见 C-9，不新增定时器）。

### C-9 实例目录自述与回收（根治 94.5%）

**新文件：每个实例在自己缓存目录里写 `instance.json`**（10 s 心跳，与 `LEASE_RENEWAL` 同频）：

```json
{ "token": "37f284d48a579e45", "pid": 12345, "started": "…", "version": "0.0.10",
  "root": "D:/VSProj/GalTranslPP", "at": 1780000000000, "shared": true }
```

- owner 写 `<workspace>/instance.json`；guest 写 `<workspace>/instances/<token>/instance.json`。
- 谁写谁删：不引入"guest 写 owner 的 `owner.lease`"（违反 `instance.cppm` 里"guest 只读 owner 目录"的既有约定）。
- `pid`/`started` 在 Windows 上从 X-6 来；取不到就留空，判活退化为纯心跳（与今天一致，不会更差）。

**回收规则**（触发点：**服务端启动的后台清扫任务**——与 C-7/C-8 合成一次 `instances → copies → budget`；
owner 的 10 s `renew()` tick 上做一次"便宜版"——只 `stat` 几个 `instances/*/instance.json`，对心跳过期的目录
**先原子改名**为 `<token>.trash-<本实例token>` 再交后台线程删，tick 因此保持毫秒级；
`cache --prune` 同步执行；清理菜单按钮同启动任务。**回收不在 `acquire()` 里做**：它只判定所有权与写自己的
`instance.json`，启动路径新增阻塞保持 0 ms——owner 长跑期间死掉的 guest 目录因此最多 40 s 后被收走，
而不是等 owner 下次重启）：

| 情况 | 判定 | 动作 |
|---|---|---|
| `instance.json` 存在且 `now - at < 2 × LEASE_EXPIRY`（60 s） | 活着的实例 | 跳过 |
| `instance.json` 存在但已过期 | 死掉的实例 | 删目录 |
| 目录里**没有** `instance.json`（0.0.8/0.0.9 遗留） | 无法判定 → 用整棵树最新 mtime | 早于 `instanceGraceHours`（24 h）则删，否则留到下次 |
| 删除失败（占用/权限/杀软） | — | 记日志 + incident，**不静默** |

**为什么遗留目录要 24 h grace**（D5，已定）：老版本不写 `instance.json`，也没有 guest 名册，
唯一的活信号是"它还在往树里写文件"；一个**空闲但活着**的老 guest 与一个**遗留**目录无法区分。
24 h 是安全余量；而 C-13 让用户随时能在枢纽里**立即**回收（不再只能 `--clean`）。

**与 C-13 的关系**：`cache --prune` 的前提是"工作区没有 `owner.lease`"，但**owner 死了、guest 还活着**时，
`instances/<live-guest>` 也必须被保护 —— 保护它的是它自己的 `instance.json` 心跳，而不是 `owner.lease`。

### X-6 平台进程身份（让 Windows 的判活/判死真的能用）

今天的事实（**代码确认**）：`modules/platform/src/process.cpp:544-568` 的 `process_alive()` 与
`:572-599` 的 `cpu_seconds()` 在 Windows 上直接 `return std::nullopt`；因此 `src/orchestrator/instance.cpp:76` 的
`owner_gone()` 恒为 false，"编辑器没了服务器就退出"（0.0.9 的 CHANGELOG，Linux/macOS 专用）在 Windows 也没有。

**新增**（`mcppls.platform.process`，平台分支用 `if constexpr (mcppls::os::FAMILY == …)`，无宏）：

```cpp
struct ProcessIdentity { std::int64_t pid; std::string started; };   // started: 进程创建时刻的稳定标识
std::optional<ProcessIdentity> process_identity(std::int64_t pid);   // linux: /proc/<pid>/stat 字段 22
                                                                     // windows: OpenProcess + GetProcessTimes
                                                                     // macos: sysctl KERN_PROC_PID / p_starttime
std::optional<bool> process_alive(std::int64_t pid);                 // 补 Windows 分支
std::optional<double> cpu_seconds(std::int64_t pid);                 // 补 Windows 分支（GetProcessTimes 用户+内核）
```

**改用它**：`instance.cpp` 的 `this_process()`/`owner_gone()` 不再依赖 `/proc` 的存在性检查；
Windows 上"上一个服务器崩了、30 秒内重启"不再被当成第二实例（这正是实例目录增生的第一个原因）。

### C-10 可观测性：一条命令看清"是不是又漏了"

`mcppls cache`（文本与 `--format json`）改为**分类**报告：

```
workspace                          contexts instances trash   modules copies        total
GalTranslPP-195b30bd1fb850f7          3.8 G   0 B(0)   0 B        48    0 (0 B)    3.8 G / 4.0 G
```

`--format json` 增字段：`instances { count, bytes }`、`copies { files, bytes, oldestSeconds }`、
`canonical { files, bytes }`、`overLimit`、`sweep { freedBytes, files, failed }`。
`report`/status 里加缓存项（C-13 用它喂状态栏）。

顺带修掉 `src/cli/cache.cpp:146-148` 那句误导（"一个模块最多存两份"——本案实际是 **127 份**）。

### C-11 `mcppls cache --prune` 成为伞形清理命令

保持"只对没有服务端打开的工作区"这一前提（并按 C-9 尊重活实例的心跳），扩展行为并新增选项：

| 选项 | 作用 |
|---|---|
| `--prune`（既有，语义扩展） | 被取代的命令目录（原有）+ **版本化副本**（新）+ **孤儿实例目录**（新）+ `trash`（原有）+ 上限淘汰（新） |
| `--instances` | 只报告/只清 `instances/` |
| `--older-than <dur>` | 只删早于该时长的副本/实例（例如 `7d`） |
| `--max-size <bytes>` | 本次运行的淘汰目标，覆盖设置 |
| `--dry-run` | 只报告将要删什么、共多少字节（枢纽"预演"也用它） |

**同一个实现，两个前台**：`cache --prune` 与枢纽按钮/C-13 的命令共用 `orchestrator::cache` 的规则，
区别只在"允许删什么"（见 C-13 的安全前提）——避免两处逻辑漂移。

### C-12 文档、设置与设计表

| 文件 | 改动 |
|---|---|
| `src/config/settings.cpp` | 新增 `cache.maxBytes`（`4G`）、`cache.totalBytes`（`16G`）、`cache.instanceGraceHours`（`24`）、`cache.showInStatusBar`（`auto`）；`summary` 与 `summaryZh` 都要写 |
| `docs/30-settings.md` + `docs/zh-CN/30-settings.md` | 同样的四行 |
| `docs/50-troubleshooting.md:311` + `docs/zh-CN/50-troubleshooting.md:120` | 现在只说"每单元保留最新两条命令构建的 BMI"；补 C-7/C-8/C-9 的实际行为、`--prune` 的新语义，以及"看状态栏/悬停卡 → 点清理"的路径 |
| `docs/93-devtools.md` + zh-CN | `mcppls cache` 一行同步（新选项与分类输出） |
| `.agents/docs/design.md` | `RD13` 扩写（副本与容量）；新增 `RD18`（实例目录回收）、`RD19`（缓存所有权与预算）；§7"已知限制"写明"缓存峰值 = clangd 一代生命周期 × 模块体积，mcppls 每次启动把它收回预算" |
| `CHANGELOG.md` | 0.0.10 段落（崩溃风暴下不再增长、状态栏与悬停卡/枢纽、`--prune` 新语义） |
| 与 UI 有关的规范/文档 | 见 C-13.5 的表（S3、`docs/specs/CHANGELOG.md`、traceability、`docs/10-editors.md`） |

### C-13 状态栏 · 悬停卡片 · QuickPick 枢纽 · 主动清理（新增，对应 D5 的 UI 部分）

#### C-13.1 数据来源（服务端是唯一来源）

**（a）`cxxModules/status` 增一个可选字段 `cache`（粗粒度，防通知风暴）**

```ts
cache?: {
  bytes: number;              // 向上取整到 100 MB：这一粒度不变就不重发（S3-4-1 要求"任何字段变化 MUST 发送"）
  limitBytes: number;
  state: 'ok' | 'near' | 'over';            // near: ≥ 70% 上限
  copies:    { files: number; bytes: number };
  instances: { count: number; bytes: number };
  lastSweep?: { at: number; freedBytes: number };
}
```

可选字段 + 新消息是 S3 §7 允许的向后兼容扩展（协议版本仍是 1）；老客户端忽略，老服务端不发。

**（b）新请求 `cxxModules/cache`（S3 §5.7）**：卡片与枢纽要的明细 ——

```ts
interface CacheReport {
  state: 'starting'|'loading'|'preparing'|'ready'|'degraded'|'error';       // 与 status 同源，卡片/枢纽标题用
  project: { name: string; source: string; level?: number; tier?: number };// 只给名字与来源，不回显引擎
  plan: { units: number; modules: number };                                 // 规模（"176 units · 48 modules"）
  progress?: { done: number; total: number; label: string };                // 模块准备；label 如 "preparing modules"
  indexing?: boolean;                                                       // 索引只有布尔（D18）
  engines: { name: string; version: string; role: string; state: string }[];// 只在提示词 / issue 预填里出现（D17）
  profile?: { compiler?: string; stdlib: string; target: string; standard?: string };
  contexts: { name: string; canonical: Bytes; copies: Bytes; trash: Bytes; instances: Bytes }[];
  copies:    { files: number; bytes: number; oldestSeconds: number };
  instances: { token: string; version: string; root: string; at: number; bytes: number }[];
  largest:   { module: string; bytes: number; copies: number }[];            // top-N ≤ 20
  limits:    { perWorkspace: number; total: number; over: boolean };
  lastSweep?: { at: number; freedBytes: number; files: number; failed: number };
  paths:     { cacheRoot: string; logDirectory: string; bundlesDirectory: string; report?: string };
  cli?:      { cacheQuery: string; sweep: string };
  prompts:   { agent: string; issue: string };    // 服务端渲染、唯一来源：扩展取回原样进剪贴板，CLI 同一函数打印（见 C-13.4）
}
```

**只读，不触发清理。服务端缓存报告结果**（至多 30 s 一算，清扫完成后立即重算）——悬停与打开枢纽都走缓存，
不在巨大目录树上重复遍历。`prompts` 由服务端渲染（编辑器名/版本来自 initialize 的 `clientInfo`，服务端本来就有）。
老服务端不认识这个方法 → 卡片/枢纽按 C-13.3 的降级规则退回粗粒度数字。

**（c）新命令 `mcppls.sweepCache`（S3 §5.8）**

```ts
// workspace/executeCommand { command: "mcppls.sweepCache", arguments: [SweepCacheParams] }
interface SweepCacheParams {
  root?: DocumentUri;
  categories?: ('copies' | 'instances' | 'trash' | 'staleCommands' | 'budget')[];  // 缺省: 除 staleCommands 外全部
  dryRun?: boolean;
  maxBytes?: number;
}
interface SweepCacheResult { ok: true; freedBytes: number; files: number; instances: number; roots: number; dryRun: boolean; alreadyRunning?: boolean }
```

规范里要写死的安全前提（**与 `mcppls.resetCache` 的关键区别**）：

- **MUST NOT 停止或重启任何引擎**，也 **MUST NOT** 让已排队的请求失败 —— 这是它相对"重置"的价值（不重建）；
- 只清"此刻没有引擎在用的"东西：**版本化副本**（mtime 早于 C-7 定义的清扫上界——正在用该根的最早一代的启动时刻，无则 now）、**孤儿实例目录**（心跳过期）、`trash`；
- **MUST NOT** 删除任何 `<module>.pcm`（已发布本体）；**默认不动**"被取代的命令目录"（那是 C-2 在引擎启动路径上的职责），
  只有显式给出 `categories: ['staleCommands']` 且该工作区没有引擎在用本 context 时才做；
- 一次只跑一个清扫（进程内互斥），进行中再次调用返回 `alreadyRunning: true`；
- 并发安全：清扫对象是"本实例自己的缓存树"，owner 与 guest 各清各的，不交叉。

#### C-13.2 VS Code：**一个**状态栏项 —— 复用 + 分段 + 长度预算 + 分级

**先回答 D10：只有一个 mcppls 状态栏块。** 第 2 版草稿曾提议"新增独立缓存项"，那是两个块，作废。
现在**复用现有的 `mcppls.statusBar`**（`editors/vscode/src/status.ts:138`），
把缓存作为它的一个**分段**；装不下就进 tooltip，而不是把状态栏撑长。

**分段与优先级**

| 段 | 优先级 | 分级/文本 | 何时出现 |
|---|---|---|---|
| S1 模块状态 | 必显 | T0 `$(check) C++ Modules`；T1 `$(warning) C++ Modules: <短句>`；T2 `$(error) …`；忙 `$(sync~spin) C++ Modules: Preparing 12/25` | 总是（沿用现有状态机与配色语义，不动） |
| S2 缓存 | 可丢 | T0 `$(database) 3.8 GB`；T1 `$(database) 3.8/4.0 GB`（≥70% 上限）；T2 `$(database) 4.6 GB` + `$(warning)` | 有数据且预算装得下；`mcppls.cache.showInStatusBar: auto\|always\|never`（默认 `auto` = 只在 T1/T2 或用户展开时显示） |
| S3 活动 | 可丢 | `$(sync~spin) Sweeping…`、`$(sync~spin) Restarting engine…` | 正在做那件事时（**顶替** S2 的位置） |

**长度预算**（"长度合适即可"的落地）：`mcppls.statusBar.maxLength`，默认 **36** 字符（可 24–60）。

1. 先放 S1；必要时用**已有的** `shorten()`（`editors/vscode/src/statusText.ts`）截短；
2. 有余量才放 S2（或正在活动的 S3）；
3. `$(icon)` 按 2 字符计宽（它渲染成图标，不是等宽字符）；
4. 任何情况下总长 ≤ 预算；**长度怎么变都不改变点击目标**（永远整项可点）。

**分级配色**（缓存**不得**盖住模块问题）：整项 tier = `max(S1 的 tier, S2 的 tier)`；
T0 无背景；T1 `$(warning)` + 默认背景；T2/T3 沿用既有 `warningBackground`/`errorBackground`
（`status.ts:66-84` 已定的两条背景规则），文案永远 **S1 在前**——第一眼是"项目怎么了"，缓存是第二眼。

**悬停 tooltip（只读卡片，C-13.3 的读入口）**：`MarkdownString`——模块状态 + 缓存四类数字 + 上限 + 最老副本 +
上次清理结果 + 日志目录 + 文本堆叠条 + 一行"点击打开清理菜单"。若 10 分钟 spike 证实命令链接可点
（`isTrusted: { enabledCommands: ['mcppls.sweepWorkspaceCache', 'mcppls.copyAgentPrompt'] }`），
卡片尾部换成两个命令链接（`清理缓存`、`复制 Agent 提示词`）；spike 不成立就保持纯文本，一切操作走点击。

**点击**：`mcppls.openCacheHub`（新命令）弹出 QuickPick 枢纽（C-13.3）。**这是行为变化**：现在点击是
`mcppls.showLogs`（枢纽里一键可达，命令面板里的 `mcppls.showLogs` 保持不变）。因此要同步改 `bar.command`
相关的单测与 E2E 断言（`test/unit`、`test/suite`）。**例外（第 6 版定稿）**：`off` 状态（本工作区关闭）时
点击保持现状的一键开启（`TURN_ON_COMMAND`，`status.ts:160`）——恢复路径必须最短；
"点击目标恒定"**按状态算**（off = 开启，其余 = 枢纽），进单测。

**动画（第 6 版收窄）**：沿用既有的 `setPulsing()`（`status.ts` 的 `pulseTimer`/`pulseLit`）——
准备/清扫期间柔和脉冲 + `$(sync~spin)`；枢纽的"进行中"用 QuickPick 原生 `busy`。
**不新增动画、不新增设置**（原计划的 `mcppls.cache.animations` 取消——原生控件自行尊重 `prefers-reduced-motion`）。

**引擎无关（D17）**：状态栏文案**不出现引擎名/版本**——`clangd` 也好、以后换的别的引擎也好，都是实现细节；
引擎的 name/version/role/state 与语义 profile 只在提示词与 issue 预填里。单测断言整条文本不含引擎名。

#### C-13.3 VS Code：悬停只读卡片 + QuickPick 枢纽（D9 第 6 版改定；原生控件，无 webview）

第 5 版是 webview 标签页；第 6 版改定为**悬停出卡片、点击出菜单**。理由：期望形态是"从底部状态栏悬浮的
小面板"，而 VS Code 没有锚定状态栏的浮窗原语（核心的状态栏弹出菜单不开放给扩展）——tooltip 的位置最接近
（就浮在状态项正上方），QuickPick 是唯一的点击弹出菜单。随之而来：

- **砍掉整个 webview 基建**：`tsconfig.webview.json`、`media/*.css`、CSP/nonce、`localResourceRoots`、
  webview 侧安全与无障碍 E2E、harness 的连带改动。扩展保持**零 webview、零 bundler**，
  E2E 反向锁死 `webviewPanelCount ≡ 0`；
- **动画与图表缩水**：保留状态栏既有 `setPulsing()`；堆叠条退化为等宽文本条；趋势线砍掉（数字在 `mcppls cache` 里）；
- 服务端三个接口（C-13.1）与 UI 形态无关，以后真要面板可零协议改动地加回——这个决定便宜且可逆。

**克制原则（映射到两个原生表面）**：**一张卡**（悬停即一屏，四层信息：状态行 / 大数字 / 分解 / 说明）、
**一个主操作**（`清理缓存` 在清理段第一条）、**分组 ≤ 5**、不加任何装饰。

**两条硬规则（沿用）**

- **不回显引擎（D17）**：卡片、枢纽的标题/条目/desc 里**没有 `clangd`**（条目写 `重启引擎`）；引擎名/版本、
  role、state 与语义 profile（编译器 / stdlib / target）只进**提示词与 issue 预填**（支持用的事实不隐藏）。
- **索引与准备，如实显示（D18）**：卡片状态行给 `state` + 模型来源 + **模块准备进度**（服务端已有的 `{done,total}`）；
  **索引只给布尔**，**没有数字就不编数字**。依据（已验证）：服务端今天没有解析背景索引的 done/total，而且
  clangd 自己的索引进度令牌与服务端注册的令牌会撞（日志里就有 `Progress handler for token
  backgroundIndexProgress already registered`）——要显示索引数字得先解决这个冲突，那是单独一件事（§7 已知限制）。

**（a）悬停卡片（tooltip，只读）**

```
**C++ Modules — GalTranslPP**
Ready · 176 units · 48 modules                     ← 状态与规模，不带引擎名

**缓存 3.79 GB / 4.00 GB（95%）**
`▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░`                        ← 等宽文本条：四类各用一种字符
已发布 1.90 GB · 副本 0 B · 实例 0 B · 垃圾箱 0 B
最老副本 0 分钟 · 上次清理 2 分钟前（释放 1.2 GB / 6837 个）
日志目录：D:\mcpplsCache\log

点击打开清理菜单
```

- 纯 `MarkdownString`；数字来自状态通知的粗粒度 + 缓存的 `CacheReport`（卡片随状态通知刷新；
  tooltip 显示中不热刷新，重悬停即新）；
- 多根工作区：聚合一行总量，每根再一行；
- 命令链接是**增量项**：10 分钟 spike 证实可点时（见 C-13.2），末行换成 `清理缓存` / `复制 Agent 提示词`
  两个链接；不成立就保持纯文本，功能一件不少（都走点击）。

**（b）QuickPick 枢纽（点击状态栏弹出；操作）**

`vscode.window.createQuickPick()`；`title = C++ Modules — <项目名>`；清扫进行中 `busy = true` 且
`ignoreFocusOut = true`（菜单不消失，完成后"上次清理"行原地回执，3 s 后恢复）。
多根工作区第一步先选根（单根跳过）。数据行与命令行同列，**每条以 codicon 开头、按段分组（D20）**：

```
C++ Modules — GalTranslPP
─ 缓存 ──────────────────────────────────────────────
 $(database) 3.79 GB / 4.00 GB        desc: 副本 0 B · 实例 0 B
 $(history)  上次清理 2 分钟前          desc: 释放 1.2 GB · 6837 个文件
 $(chevron-right) 最大模块 top 5…      desc: 明细下钻
─ 清理 ──────────────────────────────────────────────
 $(clear-all) 清理缓存（不重启、不重编）   [$(eye)]   ← 唯一主操作；按钮 = 预演
─ 维护 ──────────────────────────────────────────────
 $(debug-restart) 重启引擎
 $(refresh) 重启服务端
 $(trash) 重置缓存…                    desc: 会重新编译模块
─ 日志 ──────────────────────────────────────────────
 $(file-zip) 抓取日志（含报告）
 $(output) 打开日志
 $(folder-opened) 打开日志目录
 $(folder-opened) 打开缓存目录
─ 开源 ──────────────────────────────────────────────
 $(copy) 复制 Agent 提示词              desc: 日志不出本机
 $(github) 新建 issue                   desc: 预填版本与环境
 $(repo) 仓库        $(book) 文档        $(gear) 设置（@ext: 定位）
```

- **图标与分组（D20）**：label 原生渲染 `$(codicon)`（`clear-all/eye/trash/debug-restart/refresh/
  file-zip/output/folder-opened/copy/github/repo/book/gear/database/history/chevron-right`）；
  分组用 `QuickPickItemKind.Separator`（缓存/清理/维护/日志/开源五段）；`desc` 是右对齐的弱化说明（带实时数字）；
  "预演"是 `清理缓存` 条目上的 **QuickInputButton**（`iconPath: ThemeIcon('eye')`，tooltip "先看要删多少"），
  点击 = `dryRun: true`，结果原地回显，不开新菜单；
- **数据行的行为是刷新**（选中 `$(database)`/`$(history)` 行 = 重新拉一次报告）；下钻（`$(chevron-right)`）
  是一层只读明细：`最大模块 top 5` → `pybind11.ixx · 37.8 MB × 127 份`（detail 给路径）、`复制 issue 提示词`；
  Esc 逐级返回；
- **命令面板标题不带图标**（VS Code 不渲染命令标题里的 codicon）——图标只存在于枢纽内，这正是要枢纽的原因之一；
- **走什么**：主操作 = **新** `mcppls.sweepCache`（扩展侧 `mcppls.sweepWorkspaceCache`；**不重启、不重编**）；
  维护 = 既有 `mcppls.resetWorkspaceCache`（二次确认，文案写明**会重新编译模块**）/ `mcppls.restartClangd`
  （**条目写"引擎"，命令 id 不变**）/ `mcppls.restartServer`；日志 = 既有 `mcppls.exportDiagnosticBundle`
  （**诊断包里已经含报告**：`src/bundle/writer.cpp` 的 `Kind::report` 第一项，所以"抓取日志"一个条目就够）/
  `mcppls.showLogs` / **新** `mcppls.revealCacheDirectory`（日志目录与缓存目录两个条目，路径来自
  `cxxModules/cache.paths`，只有服务端知道真值）；开源链接 = `issueUrl.ts` 的 `buildIssueUrl()`/`feedbackIssueUrl()`、
  `REPOSITORY` = `https://github.com/Sunrisepeak/mcpp-language-server`、
  `workbench.action.openSettings` + `@ext:sunrisepeak.mcpp-language-server`（一键落到本扩展的配置页）；
- **降级**：老服务端（无 `mcppls.sweepCache`）→ 清理段整段不出现、`title` 注明
  "当前服务端版本不支持清理，可用 CLI：mcppls cache --prune"；`MethodNotFound`（不认识 `cxxModules/cache`）→
  枢纽退回 status 的粗粒度数字；`off` → 枢纽不打开（off 状态点击 = 一键开启，见 C-13.2）；
- **键盘**：QuickPick 原生键盘可达（↑↓ 选择、Enter 执行、Esc 逐级退出），无鼠标依赖，主题自动跟随。

**开源段：先本地 agent 分析，实在不行再提 issue（D19）**——步骤提示进 `复制 Agent 提示词` 的
description（常显一行）；点击后该行原地回执 `已复制 ✓ 粘给本地 agent——日志不会离开本机`（3 s 后恢复）。

**两个提示词（服务端渲染，单一来源——第 6 版定稿）**：模板在 `orchestrator::cache` 的
`agent_prompt(facts)` / `issue_prompt(facts)`（C++ 侧文本进版本库，评审可改，金测防漂移），经
`cxxModules/cache` 的 `prompts` 字段返回，CLI `mcppls cache --prompt agent|issue` 由**同一函数**打印；
扩展侧 `mcppls.copyAgentPrompt` 只做"取回 → `vscode.env.clipboard.writeText()`"，**零本地拼接**。
编辑器与版本来自 initialize 的 `clientInfo`（服务端本来就有），CLI 里省略该行。内容固定包含：

1. **`复制 Agent 提示词`（主，给本地 agent 的只读排障指令）**：
   - **环境事实**：mcppls 版本、编辑器与版本、OS/arch、工作区根、**缓存根与日志目录**、
     刚生成的诊断包/报告路径（若已生成）、引擎 name/version（D17：UI 藏、排障给）；
   - **要看的命令**（只读）：`mcppls cache --format json`、`mcppls cache --modules`、`mcppls report`、
     `<logDir>/server-*.log*` 的尾部、`incidents/` 的内容；
   - **假设清单**（本案总结的，直接喂给 agent，省得它瞎猜）：副本数与本体占比（`copies` vs `canonical`）、
     `instances/` 里孤儿目录数与大小、`trash` 是否非空、`clangd exited unexpectedly` 的次数与时间分布、
     `cache --prune` 是否 freed≈0、`MCPPLS_CACHE_DIR` 是否被设置、磁盘余量；
   - **安全约束**（写死）：只读，**不删任何文件、不跑 `--clean`/`-CleanAll`、不改配置、不外发日志**，
     需要删除或发布时先停下来问人；
   - **输出格式**：五句话——是不是 bug / 属于哪一类 / 证据 / 本地能做什么 / 需不需要提 issue。
2. **`复制 issue 提示词`（次级，在下钻里）**：让同一个 agent 把上面的结论整理成 issue 草稿：
   用仓库 `.github/ISSUE_TEMPLATE/bug_report.yml` 的字段、附诊断包路径与报告摘要、**先给用户看，用户同意后再发**。

**不自动上传（D16 的原话）**：提示词里明确"日志不出本机、发布前必须经你确认"；卡片与枢纽**不发任何网络
请求**、**不自动打开浏览器**（只有点 `新建 issue` / `仓库` 才打开，由扩展侧 `openExternal` 完成），也不内置日志阅读器。

> `新建 issue` 沿用既有 `editors/vscode/src/issueUrl.ts`：`REPOSITORY`、`BUG_REPORT_TEMPLATE`
> （读 `.github/ISSUE_TEMPLATE/bug_report.yml`，有单测防漂移）、四个预填字段、6000 字符长度兜底；
> 为"用户主动反馈"加一个**小变体** `feedbackIssueUrl()`（`code` 可省、标题不带 `[code]`）。
> 诊断包走既有 `redact` 脱敏（家目录、用户名、主机名任一残留就不生成包）。

**文案规范**：动词开头、≤ 1 行；需要确认的条目以 `…` 结尾；危险操作**写明代价**（"会重新编译模块"）；
不堆术语；QuickPick 等宽字体天然对齐数字。

**状态齐全（两个表面各自给出路）**：服务端不在 → 卡片只显示模块状态与失败原因，枢纽给
`打开日志` / `重启服务端`；数据超时 → 数据行 desc 标 `stale`，选中即刷新；`loading` → 枢纽 `busy`；
客户端没声明 `status: true` → 状态栏照旧但无缓存分段，命令仍可从命令面板单独调用。

#### C-13.4 其它编辑器与 agent

共同分母是 **命令 + `mcppls cache --format json`**：Claude Code / Copilot CLI / nvim / Zed / CLion
用命令面板调 `mcppls.sweepCache` 或直接跑 CLI；悬停卡与 QuickPick 枢纽是 VS Code 专有，`docs/10-editors.md` 说明这一点。

**给本地 agent 的提示词不是 VS Code 专有，且有单一来源**（第 6 版定稿）：模板由**服务端**渲染
（`cxxModules/cache` 的 `prompts` 字段；编辑器名/版本来自 initialize 的 `clientInfo`，CLI 里省略该行），
CLI 的 `mcppls cache --prompt agent|issue` 由**同一函数**打印；扩展侧 `mcppls.copyAgentPrompt`
只是"取回 → 写剪贴板"，零本地拼接——在 Claude Code / Copilot CLI / 其它编辑器里拿到的是
逐字节相同的同一段只读排障指令，金测防漂移（§6）。

#### C-13.5 规范与文档配套（按 skill：规范 = 文本 + 例子 + fixture + traceability 一起）

| 文件 | 改动 |
|---|---|
| `docs/specs/s3-lsp-extensions.md` | §4 增 `cache` 字段与 `S3-4-29…`（含"100 MB 粒度"与"可选字段"两条 MUST）；新增 §5.7 `cxxModules/cache`（`S3-5.7-*`）、§5.8 `mcppls.sweepCache`（`S3-5.8-*`，含"不得停引擎""不得删本体"） |
| `docs/specs/CHANGELOG.md` | 一条：只增可选字段与新消息，协议版本仍为 1（依据 S3 §7） |
| `conformance/traceability.json` | 每条新 id 给证据：`check: cache-budget/status-cache` 等、`test: tests/test_spec.cpp: …` |
| `docs/10-editors.md` + `docs/zh-CN/10-editors.md` | 悬停卡与枢纽怎么用（一个主操作 + 维护 + 反馈三步）、四个链接都指向哪里、其它编辑器用命令 + CLI 的等效做法 |
| `editors/vscode/src/issueUrl.ts` + `.github/ISSUE_TEMPLATE/bug_report.yml` | **复用**：新增 `feedbackIssueUrl()`（`code` 可省、标题不带 `[code]`），与既有 `issueUrl.test.ts` 同源；模板的字段 id 不许改（测试在读它） |
| `docs/30-settings.md` + zh-CN | 见 C-12（四行设置） |
| `docs/50-troubleshooting.md` + zh-CN | 见 C-12 |
| ~~`editors/vscode/tsconfig.webview.json` / `media/cachePanel.css`~~ | **两行删除**（第 6 版无 webview；构建保持纯 `tsc -p ./`，零新编译产物、零样式文件） |
| `editors/vscode/test/harness` | harness 已在数 `createWebviewPanel`（`editors/vscode/src/extension.ts:534`）：**期望保持 0**，并新增断言锁死——悬停卡与枢纽都不得引入 webview |

**能力协商上刻意做的事**（已核对 S3 §3、§7）：

- **不新增客户端能力开关**。`cache` 是 `cxxModules/status` 的可选字段，跟随既有的 `status: true` 选择：
  声明了 `status` 的客户端就拿到它，不认识它的客户端忽略（符合 S3 §7"新版本只增加可选字段"）；
  再加一个 `cache?: boolean` 会多出一条"MUST NOT 发送"的规则，收益为零。
- 明细请求 `cxxModules/cache` 按 S3-3-2 走：客户端只要声明过 `cxxModules`（协议版本 1）即可发；
  老服务端不认识该方法，回答 `MethodNotFound`，枢纽据此收起明细与清理段（退回粗粒度数字）。
- 命令 `mcppls.sweepCache` 由服务端在 `executeCommandProvider.commands` 里宣告（与 `mcppls.resetCache` 同处），
  客户端**不得**用同一个 id 注册自己的命令（S3-5.6-3 的既有规则）。

---

## 3. 代码落点

| 位置 | 动作 |
|---|---|
| `src/engine/clangd/bmi.cpp` / `.cppm` | `is_versioned_copy(fileName, directory)`（形状 + 同目录有去戳同名 `.pcm` 兜底） |
| **新** `src/orchestrator/cache.cppm` / `cache.cpp` | `mcppls.orchestrator.cache`：规则唯一实现。`sweep_copies(dir, before) -> Sweep`（`before` 按 C-7 上界定义）、`sweep_instances(workspaceDir, now, grace) -> Sweep`（含先改名后删）、`report(workspaceDir) -> CacheReport`（C-10 与 C-13.1(b) 共用，服务端缓存 ≤ 30 s）、`enforce_budget(...) -> Sweep`、`agent_prompt(facts)` / `issue_prompt(facts)`（提示词唯一来源，服务端与 CLI 同函数）、`struct Sweep { bytes, files, instances, failed, alreadyRunning }` |
| `src/engine/clangd.cpp:1618-1626` | 启动前调起后台清扫（异步；`before` 按 C-7 上界定义——此时尚无活代 → `now`），记 `record_event("cache-swept")`；引擎记下每代启动时刻供交互清扫做上界 |
| `src/orchestrator/instance.cpp` / `.cppm` | 写/删 `instance.json`；**`acquire()` 不再做回收**（只判定所有权，0 ms）；`renew()` tick 做便宜回收（stat + 原子改名 `<token>.trash-*`，删除交后台）；`owner_gone()` 改用 X-6 |
| `src/orchestrator/workspace.cpp` | 启动时后台清扫一次（`instances → copies → budget` 合成一条任务，不再只有 `enforce_budget`）；`reset_cache()` 顺带处理 `instance.json`（**注意：它现在只遍历 `model.*.json`，别把 `instance.json` 留在原地**）；状态里加 `cache` 字段 |
| `src/cli/cache.cpp` | 分类报告 + 新选项（C-10、C-11） |
| `src/server/session.cpp:284` 附近 + `src/orchestrator/routing.cpp:155-159` | 新命令 `mcppls.sweepCache` 的分发与宣告；新请求 `cxxModules/cache` 的路由 |
| `modules/platform/src/process.cpp` / `.cppm` | X-6 |
| `modules/platform/src/fs.cpp:100-103` | 增加"返回失败"的删除变体（现有调用点不动） |
| `editors/vscode/src/tooltipCard.ts`（新，**不含 vscode**） | 悬停卡 markdown 视图模型：大数字、文本堆叠条、四类数字、多根聚合、转义、命令链接变体（spike 布尔控制）（纯函数，mocha 可测） |
| `editors/vscode/src/cacheHub.ts`（新，**不含 vscode**） | QuickPick 枢纽视图模型：条目与 codicon、五段 separator、`$(eye)` 预演按钮、下钻层级、结果回执、降级规则（纯函数，mocha 可测） |
| `editors/vscode/src/cacheHubView.ts`（新，含 vscode） | `createQuickPick()` 的创建与事件接线（`onDidTriggerItemButton` / `onDidChangeSelection`）、`busy` 与 `ignoreFocusOut` 状态机、Esc 层级栈 |
| `editors/vscode/src/status.ts` | **复用**现有 `mcppls.statusBar`：分段（S1/S2/S3）+ 长度预算 + tier 合成；tooltip 改为 `tooltipCard.ts` 的只读卡片；点击改指枢纽（off 状态保持一键开启）；沿用 `setPulsing()` |
| `editors/vscode/src/cacheReset.ts` / 新 `cacheSweep.ts` | 扩展命令 id（`mcppls.sweepWorkspaceCache`、`mcppls.openCacheHub`、`mcppls.copyAgentPrompt`、`mcppls.revealCacheDirectory`）与服务端命令 id（`mcppls.sweepCache`）必须不同；结果解析与 `advertises*` |
| `editors/vscode/src/commands.ts:516-531` | 注册上面四个新命令（"打开日志目录/缓存目录"用 `revealFileInOS` + `paths.logDirectory` / `paths.cacheRoot`）；`新建 issue` / `仓库` 用既有的 `issueUrl.ts`（`REPOSITORY`、`buildIssueUrl`，新增 `feedbackIssueUrl()` 变体）与 `vscode.env.openExternal`；`复制 Agent 提示词` = 取 `prompts.agent` → `vscode.env.clipboard.writeText()`（同 `commands.ts:307/373/487` 的既有写法，**零本地拼接**） |
| ~~`editors/vscode/src/agentPrompt.ts`~~（**该计划删除**） | 提示词唯一来源改为**服务端**（`orchestrator::cache` 的 `agent_prompt`/`issue_prompt`，经 `cxxModules/cache.prompts` 暴露；模板文本在 C++ 侧进版本库，评审可改，金测防漂移） |
| `editors/vscode/package.json` | 新命令、新设置（`mcppls.cache.*`、`mcppls.statusBar.maxLength`） |
| `editors/vscode/test/harness` + `test/{unit,suite,suite-stress}` | `webviewPanelCount ≡ 0` 断言；枢纽/卡片 E2E；分段/预算/tier/卡片/枢纽的单测 |
| `src/engine/clangd/workarounds.cpp` / `.cppm` | `WA-CLANGD-011`（§8） |
| `tests/test_cache.cpp`（新）、`tests/test_instance.cpp`、`tests/test_process.cpp`（扩） | §6 |
| `conformance/fixtures/cache-budget`（新）、`workaround-canaries`（扩） | §6 |

---

## 4. 数据格式

`instance.json`（v1）——**只在同一个实例自己的缓存目录里**，不做跨进程写同一个文件：

| 字段 | 类型 | 说明 |
|---|---|---|
| `token` | string | 16 hex，与目录名一致 |
| `pid` | int | 取不到则省略 |
| `started` | string | 进程创建标识，取不到则省略 |
| `version` | string | 写它的 mcppls 版本 |
| `root` | string | 工作区根 |
| `at` | int | 心跳，毫秒；判活只看它 |
| `shared` | bool | true = guest |

兼容：读不懂或字段缺失一律按"过期"的**保守**路径处理（与"无此文件"同路径 → 还要整树 mtime 超 grace 才删）。
将来若要加"guest 名册"，只需加字段，不需要新文件。

---

## 5. 非功能预算

| 项 | 预算 | 说明 |
|---|---|---|
| 启动路径新增阻塞 | **0 ms** | 清扫全在后台；启动只读一次 `instance.json` |
| 单次清扫耗时（本案规模：6800 文件 / 64 GB） | 后台，不阻塞；Windows 上几十秒~几分钟 | 失败逐条计数，不中断；枢纽显示"清理中" |
| 状态通知频率 | 只在 `state` 或 100 MB 粒度变化时 | 避免 clangd 建模块时刷屏 |
| 稳态磁盘占用 | ≤ 4 GiB/工作区、≤ 16 GiB 全局 | 本案本体 2.03 GB |
| 额外重编 | **0** | 只删副本与（启动路径上的）被取代命令目录 |
| 状态栏宽度 | **一项**，总长 ≤ `mcppls.statusBar.maxLength`（默认 36 字符） | 装不下丢 S2/S3，不丢 S1、不撑长；点击目标**按状态恒定**（off = 一键开启，其余 = 枢纽） |
| 状态栏刷新 | 状态通知到达 + 清扫开始/结束 | 无定时器；脉冲动画只在真有事时 |
| 卡片/枢纽开销 | 两个原生控件：tooltip 悬停即有、QuickPick Esc 即走，**无常驻 UI、无 webview** | E2E 断言 `webviewPanelCount ≡ 0` |
| 动画 | 仅状态栏既有脉冲 + QuickPick 原生 `busy`；**无新增动画、无新设置** | 原生控件自行尊重 `prefers-reduced-motion` |
| 报告数据量 | `cxxModules/cache` 的 top-N ≤ 20，实例列表全量（通常 0–3），`prompts` ≤ 6 KB；服务端缓存报告（至多 30 s 一算，清扫完成即算） | 悬停/打开不重复遍历巨大目录树 |

---

## 6. 测试与证据（"这个改动要证明什么"）

**单元测试（C++）**

- `tests/test_cache.cpp`（新）：
  - `is_versioned_copy()`（带同目录兜底）：`pybind11-20261002-194715-990444.pcm`（旁有 `pybind11.pcm`）→ true；
    `pybind11.pcm` → false；`my-module-1.pcm` → false；`std.pcm` → false；
    `foo-20260101-120000-1.pcm` **无** `foo.pcm` 在旁 → **false**（真模块名，不删）；有 → true；
  - 合成"127 份副本 + 1 份本体"→ 清扫后只剩本体，字节数下降 >95%；
  - 上界：无引擎在用 → 上界 = now，界内全删；有引擎在用 → mtime 晚于当前代启动时刻的副本**不被**删；
  - 上限：≥4 GiB 合成树 → 淘汰后 ≤ 上限，**本体一个不少**，`failed` 计数正确；
  - `sweep_instances()`：心跳新鲜不删 / 过期删 / 无 `instance.json` 且 mtime 新于 grace 不删、旧于 grace 删；
    **`acquire()` 本身零删除调用**（注入 recorder 断言——启动路径 0 ms 的守护）；
    renew tick 路径：过期目录先被**原子改名**为 `<token>.trash-*` 再后台删，tick 内只有 stat + rename；
  - `agent_prompt()` / `issue_prompt()`（金测）：含缓存根/日志目录/工作区根实际值、七条假设、四条约束、
    五句输出格式；与 `cache --format json` 的 `prompts` 字段同一来源（同一字符串）；
  - `report()`：分类数字与目录实际一致；`copies` 与 `canonical` 分得开；与清扫用同一谓词。
- `tests/test_instance.cpp`（扩）：owner/guest 各写自己的 `instance.json`；`release()` 删自己的目录；
  第二个实例在 lease 过期后接管、由**启动后台任务**收掉遗留实例目录（注入 `now`，不打时间牌）；
  owner 长跑中：`renew()` tick 把死 guest 的目录改名进 `.trash-*` 并后台删（活 guest 不动）；
  `reset_cache()` 之后工作区里没有残留 `instance.json`。
- `tests/test_process.cpp`（扩，X-6）：`process_identity(self)` 非空且稳定；已死 pid → `process_alive()==false`；三平台各跑。

**单元测试（TypeScript，`editors/vscode/test/unit`，mocha `--ui tdd`）**

- 状态栏（纯函数部分）：
  - **分段与预算**：`ok` 时 36 字符预算下 S1+S2 都进得去；`Preparing 12/25` + 缓存时只留 S1（S1 被 `shorten()` 截短）；
    `$(icon)` 计宽 2；预算 24/36/60 三档都不超长；**同一状态下 `bar.command` 恒定**
    （on/starting/error = 打开枢纽；off = 一键开启——第 6 版决定的例外）；
  - **分级**：`max(S1, S2)` 的 tier 合成（缓存 over 但模块正常 → warning 而不是 error；模块 error + 缓存 over → error）；
- `tooltipCard.ts`（悬停卡 markdown，纯函数）：
  - 卡片含：状态与规模行（**不含引擎名**，D17）、`大数字 / 上限`、文本堆叠条（四类各一种字符，全 0 不除零）、
    四类数字、最老副本、上次清理、日志目录、"点击打开清理菜单"；
  - 多根：聚合一行 + 每根一行；老服务端粗粒度变体（无明细字段时省略文本条与四类数字）；
  - 服务端字符串进 markdown 前转义（含 `|`、反引号、换行的路径不破坏卡片结构）；
  - 链接变体由一个布尔输入控制（spike 通过 → 两个命令链接，allowlist 恰为那两个命令 id；不通过 → 纯文本行）。
- `cacheHub.ts`（QuickPick 枢纽视图模型，纯函数）：
  - **图标与分组（D20）**：每条 label 以 `$(codicon)` 开头（正则校验形状合法）；五段 separator 恰为
    缓存/清理/维护/日志/开源；`清理缓存` 带唯一 QuickInputButton（`$(eye)`，tooltip"先看要删多少"）；
  - 数据行（`$(database)`/`$(history)`）选中 = 刷新；`最大模块` 下钻一层（top-N 只读，Esc 返回）；
  - **主操作唯一**：`清理缓存` 是清理段第一条；`重置缓存…` 带代价说明（"会重新编译模块"）；
  - **引擎无关（D17）**：所有 label/desc **不含** `engines[].name`；
  - **索引如实（D18）**：`indexing: false` → 不出现"索引中"；`progress` 缺失 → 只显示"准备中"、不显示 `x/y`；
  - **降级**：无 `mcppls.sweepCache` → 清理段整段不出现、`title` 注明 CLI 替代；`MethodNotFound` →
    退回粗粒度数字 + CLI 提示；`off` → 不产出条目（off 状态点击根本不开枢纽）；
  - `dryRun` / `alreadyRunning` / 失败（`failed > 0`）三种结果的回执文案；清扫中 `busy + ignoreFocusOut` 状态机；
  - **复制提示词**：`prompts.agent` **原样**进剪贴板（mock clipboard 断言 verbatim，无本地拼接）；
  - **命令 id 不撞**：扩展命令（`mcppls.sweepWorkspaceCache` 等）与服务端命令（`mcppls.sweepCache`）不同名
    （沿用 `cacheReset.ts` 的约定断言）。
- `issueUrl.test.ts`（既有）继续读 `bug_report.yml` 校验字段 id；`feedbackIssueUrl()` 预填 version/editor/os
  且长度 ≤ 6000、标题无 `[code]` 前缀；
- `statusText.test.ts`（既有）继续覆盖 `shorten()`；`$(icon)` 计宽不破坏原有断言。

**conformance fixture**

- `cache-budget`（新）三个 check：
  - `status-cache`：`cxxModules/status.cache` 的字段形状、`limitBytes` 与设置一致、100 MB 粒度（改 50 MB 不重发，改 200 MB 重发）；
  - `cache-report` / `sweep-command`：合成"上一代留下的副本"（fixture 自己造文件）→ `cxxModules/cache` 报出份数 →
    `mcppls.sweepCache`（先 `dryRun: true` 断言"报的与做的一致"）→ 副本归零、**引擎世代未变**（断言没有重启）、
    本体仍在；`paths.logDirectory` 指向真实存在的目录；
  - `sweep-while-running`：引擎正在准备模块时调用 `sweepCache`，断言不删 mtime 晚于当前代启动时刻的副本、不重启引擎。
- `workaround-canaries`（扩）：`WA-CLANGD-011` 的金丝雀——杀掉正在准备模块的 clangd，重启后该缓存根的版本化副本为 0。

**编辑器 E2E（`editors/vscode/test/suite`）**

- 主套件：合成缓存数据 → 状态栏文本出现缓存分段且**总长 ≤ 预算** → 点击 → 枢纽弹出（五段分组、图标齐全）→
  "预演"给出与"立即清理"一致的字节数 → 清理后枢纽数字下降、状态栏长度不增长 → Esc 逐级退出；
  **全程 `webviewPanelCount === 0`**（锁死：这套 UI 不允许悄悄长出 webview）；
- 纯键盘跑通枢纽（↑↓/Enter/Esc，无鼠标事件）；
- `off` 状态：点击状态栏 = 一键开启（不开枢纽）；
- `suite-stress`：清理期间引擎世代不变；卡片与枢纽只在通知/打开时拉数据（断言无轮询）；
- 悬停卡冒烟：含 `<` `|` 的路径按字面出现在 markdown 中（卡片结构不被破坏）。

**三平台**：`mcpp run -p devtools -- check all`；单测 Linux/macOS/Windows（本次一半是 Windows 特有）；CI 全绿。

**真实复测**：请报告人用 0.0.10 候选版本重跑 [mcppls-cache-collect.ps1](../reviews/mcppls-cache-collect.ps1)（只读），
期望：`instances/` 为空或只剩 1 个活的；`copies` 占比从 97.1% 降到 <10%；总大小从 64.36 GiB 降到 ≤4 GiB；
状态栏一行显示 `≤4 GiB`，悬停卡片上"副本"几乎为 0。

---

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 删掉正在被 clangd 映射的副本 | 被删集合永远满足"没有引擎在用"：启动前 / 无活实例的工作区 / mtime 早于上一代停止时刻（C-13 的命令也照此） |
| 误删模块本体 → 用户重新编译 | `is_versioned_copy()` 判定；本体**永不**在删除集合里（D2）；`sweepCache` 的 `staleCommands` 默认关闭 |
| "立即清理"时引擎正在跑 | 只清副本（带上界）+ 孤儿实例 + trash；**不停引擎**；进行中互斥；枢纽显示"清理中" |
| 老版本 guest 的目录被误删 | 无 `instance.json` 一律 24 h grace；枢纽可"预演"（`dryRun`）；文档写明 |
| Windows 删除失败静默 | `fs` 的失败计数变体 + 日志 + incident；枢纽/`cache`/`report` 显示 `failed` |
| 状态通知风暴 | 状态里的 `cache` 是粗粒度（100 MB）+ 只在 `state` 变化时发；明细走 `cxxModules/cache` 请求 |
| 清理 64 GB 拖慢启动 | 异步 + 不阻塞（§5） |
| 多编辑器/多版本并存 | 心跳按实例自己的目录；租约语义不变；guest 不写 owner 目录 |
| 状态栏被别的提示挤长 | 只有**一项** + 长度预算：装不下丢 S2/S3 进 tooltip，永不撑长 |
| 全局预算多进程并发 | 每个进程只淘汰"无活实例"的根；最坏竞争是两边同时删同一批死文件（删除幂等、失败计数可见）。**不加锁文件**——新锁正是本方案要清理的那类问题 |
| 悬停卡的命令链接点不进（鼠标移入 tooltip 即消失） | 10 分钟 spike 定夺；不成立则卡片纯只读，一切操作走点击进枢纽，功能一件不少 |
| 设置类型（`Kind` 无 bytes） | 新增 `Kind::bytes`（D3） |
| **索引数字拿不到**（D18 的依据） | 服务端今天只报模块准备进度；clangd 的背景索引进度令牌与服务端注册的 progress 令牌会撞（日志证据：`Failed to create background index progress bar: … Progress handler for token backgroundIndexProgress already registered`）→ 卡片只给索引布尔；**要数字需先单独解决令牌命名/多路复用**，不在本方案里做 |
| 规范 id/证据漏配 | 新 id 与 `conformance/traceability.json` 同 PR；`docs/specs/tools/validate.py` 必过 |

---

## 8. 上游：只在 issue #24 记录，mcppls 自己 workaround

**不做**：不向上游提 issue/patch，不新增文档；
**要做**：在 mcppls 仓库的 **issue #24**（上游缺陷唯一登记处）加一条评论 `UP-24`，并在
`src/engine/clangd/workarounds.cpp` 注册 `WA-CLANGD-011`。下面这段就是**待粘贴到 #24 的评论**（本文件即草稿，不另存）。

> **UP-24 · [clangd] a copy-on-read BMI is left behind whenever clangd dies before releasing it, and the only GC waits three days (by atime)**
>
> Labels: `clangd`, `clang:modules`
>
> **Symptom.** When clangd reuses a published BMI it first copies it to a timestamped sibling
> (`<module>-YYYYMMDD-HHMMSS-<serial>.pcm`) and hands that copy to clang; the copy is removed only in the owner's
> destructor (`ModulesBuilder.cpp` 23.1.0 §198-209, §437-461). A clangd that is killed by a crash never runs it, so
> the copy stays. One real workspace (Windows 11, MSVC STL, 23.1.0) after 26.5 h: **64.36 GiB**, 6837 `.pcm` for 48
> module units, where one file per unit×command directory is **2.03 GB (2.9%)** — **97.1% leftovers**, up to **127
> copies in one directory**, 219 retained "clangd exited unexpectedly" lines (≈305 MB leaked per crash). Three sampled
> copies of `pybind11` were byte-identical (39,639,212 bytes; SHA-256 `5d0b9efe…9289ab`) with distinct ids — copies,
> not rebuilds, and not hard links.
>
> **Affected.** 23.1.0 (persistent cache + copy-on-read + the 3-day atime GC). 22.x uses a per-process temp layout.
>
> **Upstream status.** `unfiled` here (mcppls does not file; recorded in the mcppls register). The GC exists
> (llvm/llvm-project#193973, merged to main 2026-04-24) and is the intended answer, but: (1) a crash leaks and the
> window is 259200 s, so peak ≈ leak rate × 3 days; (2) it reads `st_atime` — the PR body notes atime is unreliable
> on some systems, and NTFS last-access updates are off by default, so a copy still mapped by a live clangd can be
> selected and the removal fails with a sharing violation (only logged); (3) the option's description says "versioned
> copy-on-read module files" while `collectModuleFiles()` takes **every** `.pcm` under the cache root, so lowering the
> threshold also deletes published BMIs (the cost becomes rebuilding 30–40 MB modules, not re-copying them).
>
> **What mcppls does (workaround).** `WA-CLANGD-011`. mcppls owns `<cdb>/.cache/clangd` (it already clears `.locks`
> there before clangd starts, RD12) and extends that to the copies: before each clangd start, and for every workspace
> no instance has open, it removes files whose name parses as the versioned shape, **keeping the published
> `<module>.pcm`**, in the background and only for files older than the previous clangd's stop time; it also enforces a
> size budget per workspace (4 GiB) and in all (16 GiB), reclaims orphaned per-instance cache directories, and shows
> the whole thing in the editor status bar with a one-click sweep. Design:
> `.agents/docs/2026-10-02-cache-growth-root-fix-plan.md` (C-7, C-8, C-9, C-13). mcppls deliberately does **not** lower
> `--modules-builder-versioned-gc-threshold-seconds` by default (point 3).
>
> **When that can go.** When the bundled clangd leaves no copy-on-read file behind after a process dies, or removes an
> earlier clangd's leftovers of the same cache root within minutes without touching the published BMI. Canary:
> `conformance/fixtures/workaround-canaries` — kill clangd mid-preparation, restart, count the copies (expected 0).
>
> **Evidence.** `mcppls-cache-20261002-222834.zip` + `.agents/reviews/mcppls-cache-20261002-cause-analysis.md`;
> `ModulesBuilder.cpp` of `llvmorg-23.1.0` §40-44/§198-209/§437-461/§986-1018; #193973.
>
> **TODO.** [ ] post this comment and add its row to #24's index [ ] put the link in `WA-CLANGD-011.upstream`
> [ ] attach a minimal reproduction once the crash-symbol work of the 0.0.7 plan (T15/K-3) lands.

`#24` 索引行：

```
| UP-24 | clangd leaves a copy-on-read BMI behind whenever it dies before releasing it; the only GC waits 3 days and reads atime (unreliable on Windows, and it removes published BMIs too) | 23.1.0 (bundled) | unfiled (recorded here) | C-7/C-8/C-13 in .agents/docs/2026-10-02-cache-growth-root-fix-plan.md; WA-CLANGD-011 |
```

`WA-CLANGD-011` 注册表条目（放进 `REGISTRY`，现有 10 条 → 11 条；常量加进 `workarounds.cppm`）：

```cpp
{
    .id = LEFT_BEHIND_MODULE_COPIES,
    .title = "clangd leaves the copy-on-read BMI it hands a reader behind when it dies; mcppls removes the previous clangd's leftovers before starting the next one",
    .fixedIn = "",
    .upstream = "#24 UP-24 (unfiled); GC from llvm/llvm-project#193973 (3-day atime threshold) is in 23.1.0, the leak is not",
    .evidence = ".agents/reviews/mcppls-cache-20261002-cause-analysis.md §2; .agents/docs/2026-10-02-cache-growth-root-fix-plan.md C-7; conformance fixtures cache-budget, workaround-canaries",
    .added = "0.0.10",
    .removeWhen = "the bundled clangd leaves no copy-on-read file behind after a process dies, or removes an earlier clangd's leftovers of the same cache root within minutes",
    .canary = "conformance/fixtures/workaround-canaries: clangd killed while preparing modules leaves versioned copies in the cache root",
    .premise = "the cache directory under <cdb>/.cache/clangd belongs to this server while it holds the workspace lease, so anything the previous clangd left there and no reader holds may be removed (same premise as RD12, which clears .locks there)",
},
```

**崩溃本身**（Build AST / preamble 的 `0xC0000005` / `0x80000003`）已有的 #24 条目与 `WA-CLANGD-009` 覆盖；
本条只登记"崩溃的代价"，不重复登记崩溃。

---

## 9. 实施计划与提交

| # | 任务 | 条目 | 依赖 |
|---|---|---|---|
| T1 | **平台**：`process_identity()`；Windows 的 `process_alive()` / `cpu_seconds()` | X-6 | — |
| T2 | **缓存卫生模块**：`orchestrator::cache`（副本清扫、预算、报告数据、`agent_prompt`/`issue_prompt` 渲染），`bmi::is_versioned_copy`（含同目录兜底），`fs` 的失败可见删除 | C-7、C-8 | — |
| T3 | **引擎挂点 + CLI**：启动前后台清扫（`instances → copies → budget`）、`cache` 分类报告、`--prompt` 与新选项 | C-7、C-10、C-11 | T2 |
| T4 | **实例**：`instance.json`、回收（启动任务 + renew tick）、`owner_gone` 用 X-6 | C-9 | T1、T2 |
| T5 | **服务端接口**：status 的 `cache` 字段、`cxxModules/cache` 请求（含 `prompts` 渲染与报告缓存）、`mcppls.sweepCache` 命令（含互斥与 dryRun） | C-13.1 | T2、T4 |
| T6 | **编辑器 UI**：状态栏**分段/预算/分级**（复用现有项）+ 悬停卡（`tooltipCard.ts`）+ QuickPick 枢纽（`cacheHub.ts`，图标与分组）+ 新命令（`openCacheHub`/`sweepWorkspaceCache`/`copyAgentPrompt`/`revealCacheDirectory`）、设置与 package.json、E2E（含 `webviewPanelCount ≡ 0` 锁死） | C-13.2、C-13.3、C-13.4 | T5 |
| T7 | **规范与文档**：S3 §4/§5.7/§5.8 + `docs/specs/CHANGELOG.md` + traceability；四份文档（含 zh）；`design.md` 的 RD 行与 §7；CHANGELOG | C-12、C-13.5 | T2、T5 |
| T8 | **上游登记**：`workarounds.cpp` 的 `WA-CLANGD-011`；#24 的 `UP-24` 评论与索引行；canary fixture | §8 | T2 |
| T9 | **验证**：C++/TS 单测（三平台）、`cache-budget` 与 `workaround-canaries`、`vscode-e2e`、`check all`、请求人复测 | §6 | 全部 |

**提交建议（一个 PR 四段，可分别验证）**

1. **平台（T1）**：判活/身份/CPU 时间，独立可证；
2. **缓存有界（T2+T3）**：`is_versioned_copy`、清扫、预算先各自单测，再接引擎与 CLI；
3. **实例与接口（T4+T5）**：`instance.json` + 回收 + status/请求/命令；
4. **UI 与记录（T6+T7+T8）**：悬停卡/枢纽与状态栏、规范与文档、#24 与 workaround 注册。

版本：0.0.10，一个 PR，CI 全绿；CHANGELOG 写明"崩溃风暴下缓存不再无限增长""状态栏与悬停卡/枢纽可查看与清理缓存"。

---

## 10. 验收标准

1. 复现本案形态（40 MB 模块 × 上百份副本 + 2 GiB 本体）时，**服务端启动后副本数为 0**，本体一个不少。
2. 单工作区 ≤ `cache.maxBytes`（默认 4 GiB）；全局 ≤ `cache.totalBytes`（默认 16 GiB）；
   达不到上限时**报告**而不是删本体（D2）。
3. 有存活 clangd 的缓存根，**任何时候都不会被删文件**（单测用文件句柄/世代断言；`sweep-while-running` fixture）。
4. 三个孤儿实例目录这类场景：owner 下一次 `acquire()` 后 `instances/` 只剩活着的实例；
   0.0.9 遗留目录在 24 h 后自动回收；`cache --prune` 立即回收；失败出现在日志/incident 里。
   **owner 已死但某个 guest 还活着时，`cache --prune` 必须放过那个 guest 的目录**（看它自己的 `instance.json` 心跳，
   而不是工作区级的 `owner.lease`）——用单测断言。
5. Windows 上 `process_alive()`/`process_identity()` 有真实结果；"崩溃后 30 秒内重启"不再生成新实例目录。
6. `mcppls cache [--format json]` 与卡片/枢纽的分类字段与实际目录一致；`--dry-run` 与实际删除集合一致。
7. **UI —— 状态栏（一项）**：只有一个 `mcppls.statusBar`；
   S1 模块状态永远在、S2 缓存按预算进、S3 活动顶替 S2；任意数据下**总长 ≤ `statusBar.maxLength`（默认 36）**；
   `max(S1,S2)` 的 tier 决定颜色（缓存 over 不把模块的 error 顶掉，模块正常时缓存 over 才变 warning）；
   **悬停出只读卡片**（四类数字 + 上限 + 文本堆叠条 + 最老副本 + 上次清理 + 日志目录）；
   点击打开枢纽，**off 状态点击 = 一键开启**（按状态恒定）；`mcppls.cache.showInStatusBar` = `auto/always/never` 生效；
   **整条文本与卡片、枢纽的所有文案不含引擎名**（提示词/issue 预填里必须有——D17）。
8. **UI —— QuickPick 枢纽**：点击状态栏（或命令面板 `mcppls.openCacheHub`）弹出；
   五段分组（缓存/清理/维护/日志/开源）、每条操作带 codicon、`清理缓存` 是唯一主操作且带 `$(eye)` 预演按钮；
   数字与 `cxxModules/cache` 一致；”预演”报出的字节数与”立即清理”实际一致；
   清理后数字下降、**引擎未重启、未重新编译**；重置/重启引擎/重启服务端/抓日志/打开日志/打开日志目录/打开缓存目录
   都从枢纽可达；`复制 Agent 提示词` 的剪贴板内容 = 服务端 `prompts.agent` **原样**（含缓存根/日志目录/七条假设/
   四条约束，零本地拼接），`复制 issue 提示词` 在下钻里且内容含模板字段与”先给人看再发”；
   `新建 issue` 打开的 URL 预填版本与系统、`抓取日志` 生成的 zip 路径出现在该 URL 里；
   **状态行如实**：有 `progress` 才显示 `x/y`，索引只显示布尔（无数字不编）；
   老服务端下清理段不出现且 title 注明 CLI 替代；
   **全程零 webview（`webviewPanelCount === 0`）、零轮询、零网络请求**（链接仅在用户点击时由扩展侧 `openExternal`）。
9. **UI —— 可达性与安全**：枢纽纯键盘可跑通（↑↓/Enter/Esc，原生控件）；卡片是纯 `MarkdownString`
   （服务端字符串转义后进入，含 `|`/反引号的路径按字面显示）；三套主题自动跟随（只用原生控件，无自定义样式）。
10. 规范：S3 新 id 在 `traceability.json` 里有证据，`validate.py` 通过；`docs/specs/CHANGELOG.md` 有记录。
11. 文档（含 zh-CN）、设置表、`design.md`、CHANGELOG 与实现对得上；`check all` 与全部 fixture 三平台通过。
12. 请求人复测：总缓存 64.36 GiB → ≤4 GiB，`copies` 占比 <10%；状态栏与悬停卡读数与实际一致。

---

## 11. 决定表（review 结论）

| # | 决定 | 结论 |
|---|---|---|
| D1 | 清扫范围：只删版本化副本，不整棵删 | **已定（按推荐）** |
| D2 | 副本删净仍超上限：报告 + 枢纽动作，**不删本体**（软上限） | **已定（按推荐）** |
| D3 | 新增 `Kind::bytes`（`"4G"`/`"unlimited"`）承载三行缓存设置 | **已定（按推荐）** |
| D4 | 每实例在自己缓存目录写 `instance.json`，不扩 `owner.lease` | **已定（按推荐）** |
| D5 | 旧版本遗留实例目录：**24 h grace 自动删** | **已定**；此外由 C-13 让用户随时在枢纽里立即清理 |
| D6 | 编辑器命令：新增"清理本工作区缓存"（服务端 `mcppls.sweepCache` / 扩展 `mcppls.sweepWorkspaceCache`） | **已定（按推荐）** |
| D7 | 不改 clangd 的 GC 阈值（钝器，会删本体）；只在 troubleshooting 里作为临时手段 | **已定（按推荐）** |
| D8 | `--prune` 扩展为伞形 + `--instances/--older-than/--max-size/--dry-run` | **已定（按推荐）** |
| D9 | UI 形态：**悬停只读卡片 + 点击 QuickPick 枢纽**（第 6 版改定，替代第 3 版的 webview 标签页；无标签页、无侧边栏） | **已定（悬停 + QuickPick）** |
| D10 | 状态栏：**只有一个块** —— 复用现有 `mcppls.statusBar`，缓存做成分段（S1 必显 / S2 缓存 / S3 活动），配**长度预算**与**分级**，装不下进 tooltip | **已定（复用，不新增）** |
| D11 | `cache` 字段放 `cxxModules/status`（粗粒度、可选）+ 明细走新请求 `cxxModules/cache`；协议版本仍为 1 | **已定（按推荐）** |
| D12 | 状态栏长度预算默认值：`mcppls.statusBar.maxLength = 36`（可 24–60）；`$(icon)` 计宽 2；装不下先丢 S3/S2 | **已定** |
| D13 | 动画：仅状态栏既有脉冲 + QuickPick 原生 `busy`；**无新增动画、无新设置**（第 6 版收窄，原 `cache.animations` 取消） | **已定（收窄）** |
| D14 | ~~webview 只当视图~~ **已作废**（第 6 版无 webview）；其精神保留：卡片/枢纽只消费服务端数据，扩展侧校验后才转发 LSP/命令 | **作废 → 精神保留** |
| D15 | 信息密度：**一张卡**（悬停即一屏）、**一个主操作**（清理缓存）、分组 ≤ 5、信息 ≤ 四层；明细走下钻 | **已定（第 6 版重映射）** |
| D16 | 反馈流程：**不自动上传**；只给本地 agent 提示词 + 复用 `issueUrl.ts` 预填 + 复用诊断包（含报告）；不内置日志阅读器 | **已定** |
| D17 | UI **不回显引擎**：卡片/枢纽的标题、条目、desc 不带引擎名与版本（条目写"重启引擎"）；引擎与 profile 放提示词、issue 预填 | **已定（按推荐）** |
| D18 | 索引情况**如实**：状态行给 `state` + 模型来源 + 模块准备 `{done,total}`；索引只给布尔，**没有数字就不显示数字**（索引数字要先解决 clangd 的 progress 令牌冲突，属单独一件事） | **已定（按推荐）** |
| D19 | `开源` 区：`复制 Agent 提示词`（只读排障：环境事实 + 命令 + 假设清单 + 安全约束 + 输出五句）+ `复制 issue 提示词`；步骤常显一行 + 点击原地回执；**零网络请求** | **已定（按推荐）** |
| D20 | QuickPick 枢纽的图标与分组：每条操作以 codicon 开头（`$(clear-all)` 等），`QuickPickItemKind.Separator` 分五段（缓存/清理/维护/日志/开源），预演为条目上的 `$(eye)` QuickInputButton，明细多级下钻 | **已定** |

---

## 12. 与既有计划的关系

| 既有条目 | 本方案的关系 |
|---|---|
| C-2 / RD13（每 unit 留最新 2 个命令目录） | **保留**，作为 C-8 淘汰顺序第 2 步、C-13 的 `staleCommands`（默认关） |
| C-4 / RD12（启动前清 `.locks`） | **同一所有权原则的延伸**：C-7 在同一个位置清"上一代的中间产物" |
| C-5（租约记 pid + 启动时间） | **补齐 Windows 实现**（X-6）；今天在 Windows 上完全不起作用 |
| 0.0.7 计划 §9.7 / T12（4 GB / 16 GB 上限、重置命令） | **具体化**（C-8 的淘汰顺序与常量、C-11/C-13 的入口、可观测字段、测试） |
| T15 / K-3（崩溃符号、最小复现） | §8 的 UP-24 复用其产物：最小复现要在 K-3 之后附符号化栈 |
| 新增 S3 规则 | 按 skill：文本 + schema/例子 + fixture + `traceability.json` 同 PR |

---

## 13. 与本次案例的对照

| 案例里看到的现象 | 哪一条治它 |
|---|---|
| 6837 个 `.pcm`、97.1% 是重复副本、单目录 127 份 | C-7（每次启动清到 0）、C-13（状态栏/悬停卡上看得见） |
| 26.5 h 长到 64 GiB，clangd 的 GC 3 天才动 | C-7 + C-8（每次启动回到预算内，不等 GC） |
| 3 个孤儿实例目录 60.80 GiB | C-9 + X-6（不再因 30 s 误判而增生） |
| `mcppls cache --prune` 只 freed 61.2 MB / 2277.7 MB | C-11（覆盖副本与实例）+ C-10（分类报告，不再误导） |
| Windows 上 `owner_gone()` 恒 false；`process_alive()` 恒 nullopt | X-6 |
| 用户只有 `--clean` 一条路（要重建） | C-7/C-8/C-9/C-11 + **C-13 枢纽按钮**（不重建） |
| 用户根本不知道缓存里发生了什么 | C-13（状态栏一行 + 悬停卡分解 + 枢纽入口 + 上次清理结果） |

---

## 14. 自我 review（本版做过的检查与改动）

**本版改掉的问题**

1. **状态通知风暴**：S3-4-1 要求"任何字段变化 MUST 发送"，而缓存字节在 clangd 建模块时一直变 →
   `cache` 字段改成**粗粒度**（100 MB 取整）+ 只在 `state` 变化时发，明细走 `cxxModules/cache`（D11）。
2. **"清理中引擎在跑"的边界**：互动清理若也做"删被取代的命令目录"，会与 C-2（只在引擎启动路径做）冲突 →
   `mcppls.sweepCache` 的 `staleCommands` **默认关闭**，且只在"该 context 没有引擎在用"时允许。
3. **`reset_cache()` 的遗漏**：它只遍历 `model.*.json`，`instance.json` 会被留在原地 →
   已在 §3 的 `workspace.cpp` 行写明。
4. **全局上限的"最后使用"没有定义** → 在 C-8 里定义了三个来源取最新。
5. **`cache --prune` 会误伤活着的 guest**：它的前提是"没有 `owner.lease`"，而 owner 死了 guest 可能还活着 →
   C-9 明确"保护活 guest 的是它自己的 `instance.json`"，并写进验收第 4 条。
6. **面板的"可视化"是否等于 webview**（第 1 版的小结，**第 3 版已按 D9 改为 webview**）：
   v1 当时选 QuickPick + 文本条形图以避免没算过的 webview 成本；第 3 版把这份成本算清了（§C-13.3、§C-13.5）。
7. **其它编辑器**：面板是 VS Code 专有，已在 C-13.4 明确共同分母（命令 + CLI），避免读成"所有编辑器都有面板"。
8. **规范配套**：S3 新 id、`docs/specs/CHANGELOG.md`、`traceability.json`、`validate.py` 都进了 T7 与验收第 10 条
   （skill 的"规范变更要 5 件套一起"）。
9. **上游部分**：删掉独立文档与"向上游提 issue"的动作；只剩 #24 的一条评论 + `WA-CLANGD-011`（§8）。

**第 2 版自我 review 又抓到的**

10. **`C-12` 被引用却没有段落**：标题、§9 的 T7 都写了 C-12，§2 里没有它（第 1 版有）→ 已补 `### C-12 文档、设置与设计表`。
11. **能力协商没交代**：新请求/新命令/新字段到底要不要新的能力开关，方案没说 → 已核对 S3 §3 与 §7，
    在 C-13.5 明确：**不新增客户端开关**（`cache` 跟随既有 `status: true`），请求按 S3-3-2，命令按
    `executeCommandProvider` + S3-5.6-3 的 id 约定。
12. **验收第 4 条不够硬**："owner 死了但 guest 还活着"这一支只在散文里 → 已写进验收第 4 条并要求单测断言。

**第 3 版自我 review（按 D9=v2 / D10=只有一个块 / 面板当枢纽 重写 C-13 之后）**

13. **"两个状态栏块"作废**：第 2 版确实提了独立项 `mcppls.cacheStatusBar` → 改成**复用现有项 + 分段**，
    并补上"长度预算 + 分级 + 点击目标恒定"（这正是"提示太长不优雅"的解）。
14. **点击行为变化要连带改测试**：点击从 `mcppls.showLogs` 变成打开面板 → 已写明要同步改
    `bar.command` 的单测与 E2E 断言，日志在面板里一键可达、命令面板不变。
15. **webview 在当前构建方式下怎么编译**：扩展是 `tsc -p ./`、无 bundler、无 `media/` 目录、今天零 webview →
    已加 `tsconfig.webview.json`（`"module": "none"`、`"lib": ["dom"]`）与 `media/cachePanel.css`，
    并明确"不引第三方图表库"。
16. **webview 的安全边界**：服务端给的路径会进 DOM → 已写 CSP + nonce + `localResourceRoots` +
    "只用 `textContent`" + "webview 只发类型化消息，扩展侧校验"（D14），并进验收第 9 条。
17. **E2E harness 会数 webview**：`editors/vscode/src/extension.ts:534` 的 `webviewPanelCount` 现在就存在 →
    加面板会让"0 个 webview"的期望失效，已列进 harness 的改动项。
18. **"抓 log"被说清楚**：不是新造一个抓取机制，而是把既有的 `collectReport` / `exportDiagnosticBundle` /
    `showLogs` 加一个新的 `revealCacheDirectory`（路径由服务端 `paths.logDirectory` 给）都放进面板。
19. **动画不是装饰也不是信息**：加了 `prefers-reduced-motion` 降级、`mcppls.cache.animations` 开关、
    "动画不承载信息"与 150–300 ms 的时长上限；脉冲沿用已有的 `setPulsing()`，不新增机制。
20. **面板数据量与生命周期**：`top-N ≤ 20`、隐藏即释放、不轮询（`retainContextWhenHidden: false`）——
    避免"为了好看"给每个窗口加一个常驻 webview。

**第 4 版自我 review（按“简洁优雅 + 方便入口”重写 C-13.3 之后）**

21. **"可视化"被做成了仪表盘**：第 3 版有环形图 + 条形 + sparkline 三处图形、十个等权按钮 → 收敛为
    **一个图**（堆叠条 + 微型趋势）、**一个主按钮**、**默认一屏**、**四层信息**（D15），并给了可检验的视觉规范表。
22. **"方便入口"原来缺三个**：进设置页（`@ext:` 过滤，直接落到 mcppls 的配置）、打开开源仓库、新建 issue →
    都补进链接层；`抓取日志` 这一条**合并**了原来的"抓取报告 + 导出诊断包"两个按钮（诊断包里已含报告，
    见 `src/bundle/writer.cpp` 的 `Kind::report`），并把"仅导出报告"降级到详情里。
23. **反馈不能新造**：已有 `issueUrl.ts`（`REPOSITORY`、`buildIssueUrl`、四个预填字段、6000 字符兜底、
    读 `bug_report.yml` 的单测）与 `redact`/bundle 的隐私规则 → 面板只做预填 + 打开 + 三步提示（D16）；
    **不自动上传、不内置日志阅读器**，避免"为了反馈再养一个子系统"。
24. **反馈的 URL 可能是"主动反馈"而不是错误**：`buildIssueUrl()` 现在要求 `code` → 已写明加一个 `feedbackIssueUrl()` 变体
    （`code` 可省、标题不带 `[code]`），并进单测。
25. **文案与安全补齐**：需要确认的按钮以 `…` 结尾、危险操作写明代价；webview **永不自己 `openExternal`**
    （链接也走扩展侧，防注入跳转）。

**第 5 版自我 review（按“不回显引擎 + 开源区 + 索引情况”重写之后）**

26. **引擎名写进了 UI**：第 4 版的标题行是 `● Ready · clangd 23.1.0`，还把"重启 clangd"当按钮 → 已改为
    引擎无关：标题给规模（`176 units · 48 modules`）、按钮写"重启引擎"、状态栏文案同样不带引擎名（D17）；
    同时明确"藏 UI 不等于藏事实"——`详情 ▾`、提示词、issue 预填里必须有引擎与版本。
27. **"索引情况"差点被编出来**：服务端只有模块准备的 `{done,total}`，没有背景索引的数字，且 clangd 的
    `backgroundIndexProgress` 令牌与服务端的 `mcppls/<key>` 会撞（日志里有原文）→ D18 定为"索引只给布尔、
    没有数字就不显示数字"，并把"要数字先解决令牌冲突"写进 §7 风险表。
28. **"反馈"变成一个子系统**：改名为"开源"后只做两件事——**复制提示词**（本地 agent 只读分析）与
    **新建 issue**（既有 `issueUrl.ts` 预填）；提示词本身成为**可评审、可单测、进版本库**的产物
    （`agentPrompt.ts`），并且**不是 VS Code 专有**（CLI `mcppls cache --prompt` 暴露同一份）。
29. **步骤提示三种交互都算过**：常显一行（不点也能看到）＋ 悬停完整四步 ＋ 点击原地回执（"已复制 ✓ 日志不出本机"）。
30. **面板的零网络承诺写进验收**：除用户点链接外**零网络请求**；剪贴板只在扩展侧写，webview 不碰。

**第 6 版自我 review（按"悬停卡 + QuickPick 枢纽、图标与分组"重写 C-13，并落实 review 拍板的实现修正之后）**

31. **"重启引擎"的 desc 里差点又写了 `clangd`**：初稿给该条目配了引擎名做说明——违反 D17；已去掉，
    引擎名只在提示词与 issue 预填里。
32. **枢纽数据行的行为没定义** → 定为"选中 = 刷新报告"（数据行不是命令，选中必须有可预期的事发生）。
33. **`cxxModules/cache` 会在巨大树上反复遍历**（悬停就请求一次，案发时 6800 文件 / 64 GB）→ 服务端**缓存报告**
    （至多 30 s 一算，清扫完成即算），卡片与枢纽共用缓存值。
34. **`mcppls.cache.animations` 失去对象**（webview 没了，动画只剩既有脉冲与原生 busy）→ 设置取消，D13 收窄改写。
35. **harness 的 `webviewPanelCount` 从"要改期望"反转为"锁死 0"**：并写进 E2E——将来谁为了省事加回一个 webview，
    测试会说话。
36. **提示词下沉服务端后，`agentPrompt.ts` 计划整个删除**：模板漂移从"两个实现"变成"零份重复"；
    C++ 侧金测（七条假设 / 四条约束 / 五句输出 / 与 `--format json` 同源）。
37. **C-9 挪出 `acquire()` 后，回收的三个入口各自写清**：启动后台任务（全量）、renew tick（stat + 原子改名的
    便宜版）、`cache --prune`（同步）——§5 的"启动 0 ms"与 C-9 不再互相矛盾；单测断言 `acquire()` 零删除调用。
38. **副本判定加同目录兜底后写进了谓词签名**（`is_versioned_copy(fileName, directory)`），报告 / 清扫 / 预算
    共用同一谓词，"报的"与"删的"不可能漂移；`foo-20260101-120000-1.pcm` 无 `foo.pcm` 在旁 → 不删。
39. **两处图标相关的边界写明**：命令面板标题不渲染 codicon（图标只在枢纽里，这是枢纽存在的理由之一）；
    QuickPick label 的 `$(name)` 与状态栏 `shorten()` 计宽规则互不影响（两个表面，各自计宽）。
40. **多根工作区的聚合定了口径**：卡片聚合一行 + 每根一行，枢纽第一步选根（单根跳过）——从"还没验证"清单里
    结案。

**还没验证、留给实现的**

- **悬停卡的命令链接能否点进**（鼠标移入 tooltip 点击命令链接）：10 分钟 spike——
  `isTrusted: { enabledCommands: ['mcppls.sweepWorkspaceCache', 'mcppls.copyAgentPrompt'] }`；
  不成立则卡片纯只读（功能不丢，一切操作走点击进枢纽）。
- **QuickPick 条目与 desc 在窄窗口（1366）下的观感**：label 是否超长截断、desc 是否完整——真机目测一次，
  必要时收紧文案长度（单测同步上限）。
- `GetProcessTimes` 的创建时间在"系统时间被改"时的稳定性（**推断**：与 `/proc` 的 starttime 一样只在同一 boot 内可比；
  实现时若要跨 boot，需带 boot id 或退化为心跳）。
- **状态栏预算的实际手感**：36 字符在 1366×768 + 左侧还有别的扩展图标时是否真的"合适"，
  以及 codicon 计宽 2 是否够准（VS Code 不提供文本宽度 API，只能用字符数近似）——需要一次真机目测，必要时按 D12 调默认值。
- 删除 64 GB 在真实 Windows + 杀软下的耗时（§5 的预算是量级估计，实测后再定是否需要分批/限速）。
