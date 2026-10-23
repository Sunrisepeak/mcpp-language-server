# mcppls 0.0.17 联动发布设计（mcppls 0.0.17 + mcppls-clangd 0.0.17）

日期：2026-10-23。状态：**实施中**（§10 记录实施与设计的差异）。

本文取代 [`2026-10-23-0.0.17-part2-design.md`](2026-10-23-0.0.17-part2-design.md)（part2 设计）。那份文档里的数据和分析已并入本文，本文可以独立阅读。

## 0. 目标与已定决定

**目标**：引擎和 mcppls 一起发布 0.0.17，把 0.0.16 以来已知的问题都在这一版里处理掉。

**发布标准**从 CI 的角度定义（§6）：

- **底线**：不低于 0.0.16。
- **目标**：尽量全绿。

| # | 决定 |
|---|---|
| D1 | W4（autosave reload）和 W5（隔离期）进入 0.0.17 |
| D2 | U5/U6 慢补全是否阻断，看测试内重复覆盖的结果（§5） |
| D3 | 引擎和 mcppls **联动**发布 0.0.17，引擎修复不再推迟到下一版 |
| D4 | CI 底线是不低于 0.0.16，目标是全绿 |
| D5 | 约 1 MB 的证据包只留在 PR 的提交历史里，不进 main |
| E3 | U7a 崩溃的根因尽量复现并修复；如果查不到，靠 W5 把用户感受到的影响降下来，根因在 #59 里记为未解决 |
| W6 | 偶发慢补全不靠多跑 12 次构建，而是在测试内覆盖 12 次；同时整体降低整条链路的构建成本 |
| PR | 新开 0.0.17 release PR，#65 的提交并入其中 |

## 1. 现状

### 1.1 mcppls-clangd（引擎）

- 已发布 v0.0.16（`23.1.0-mcppls.4`，10-13 发布）。之后没有新的 PR，也没有新的发布。补丁序列到 `0035-UP-40`。
- mcppls 0.0.16 和 0.0.17 候选版本锁定的是**同一份引擎**，所以 mcppls 侧的所有对比都不涉及引擎的变化。
- **引擎自己的定时 CI 从 10-07 起每天都红**，0.0.16 发布时就是红的：
  - `e2e`：gate 中 `product_completion_both` 和 `product_diagnostics_both` 两项 FAIL，其余三项 PASS（`fork_version_detected`、`fork_memo_fired`、`no_latency_regression`）。
  - `soak`：sanitizer 构建（`build-san`）中，`modules.test`、`modules-stale-lock.test`、`modules-validation-cache.test` 三项 lit 测试失败，日志里有 "Failed to build module prerequisites"。冷构建大约要 3 小时。
- 已知的引擎问题（0.0.16 上同样存在）：
  - memo 每 30 s 构造一次完整的 JSON，空闲时 RSS 会上涨。
  - U7a 场景里，AST 构建时发生崩溃，崩溃栈中有 realloc。Linux/macOS 的 release 是 stripped 的，这个崩溃无法符号化。

### 1.2 mcppls

- main 在 `fd869bd`：#64 已 squash 合入，版本号已是 0.0.17，还没打 tag。
- PR #65（draft）在 main 之上有 4 个提交：

| 提交 | 内容 | 是否改 server |
|---|---|---|
| `072c230` | 子进程消息在 writer 上编码 | 是（`src/lsp/connection.*`） |
| `1341938` | 计划应用后发布已提交的 workspace 快照 | 是（`workspace.cpp` 19 行） |
| `e01e1c1` | 真实 mcpp Windows target 的测试 | **否** |
| `fcb3450` | watched source、editor roots；CI 环境修复（termux 官方源、Zed 预装 clangd、保存失败阶段日志）；证据包 | 是 |

- `fcb3450` 的 PR CI 有 3 类失败，没有一类来自 server：
  1. `test_check`：`verify.py` 不在 allowlist 中（×6）。
  2. Windows 上 overflow trace 的断言失败（合并树中的第 466、467、470 行）。
  3. `mcpp-target-windows-real` 用了 PATH 上的旧版 mcpp `2026.9.30.2`（`\"` 引号缺陷，上游 2026.10.5.2 已修复）。
- `fcb3450` 还没跑过 nightly。

### 1.3 nightly 对比（0.0.16 定时 8 晚 vs 候选 4 次 workflow_dispatch）

| job | 0.0.16 | 候选 | 说明 |
|---|---|---|---|
| ux-xlings 1（U13） | 8/8 红（CPU） | 2/4 红（RSS） | 见 §1.4 |
| ux-mcpp 1/2/3 | 1/24 红（U9 121 s） | 3/12 红（U15 13 ×2，U5/U6 离群 ×1） | U15 在 0.0.16 上也一直是 10–12，预算 12 |
| ux-mcpp two-core | 8/8 红 | 0/4 红 | 变好 |
| ux-xlings two-core | 0/8 红 | 2/4 红 | U5 有 1–2 次 server 超时；U7a p95 1.53/1.71 s，预算 1.5 |
| self-hosting、startup | 8/8、4/8 红 | 0/4 红 | 主要来自 CI/runner 修复 |
| termux | 8/8 红 | `e01e1c1` 因镜像 404 红 | `fcb3450` 已修，待验证 |
| Zed 默认模式 | 绿 | `e01e1c1` 因 GitHub API 403 红 | `fcb3450` 已修，待验证 |

**关键事实**：`e01e1c1` 的 server 代码和 `1341938` **完全相同**，但前者的 nightly 中 ux 全绿，后者红了 4 个 job。也就是说，同一份产品代码在不同的运行中结果不同，坏结果是按"那一次运行"聚在一起的。所以单次 nightly 不足以判断产品好坏，必须做重复测量和同机对照（§5）。

### 1.4 U13 RSS：失败和通过分成两组

| run | 结果 | idle 开始 → 结束 | idle CPU |
|---|---|---|---|
| `38366b9` | 通过 | 1391.8 → 660.3 MB（−52.6%） | 0.0059 |
| `e01e1c1` | 通过 | 1494.3 → 675.4 MB（−54.8%） | 0.0080 |
| `072c230` | 失败 | 624.7 → 1086.6 MB（+73.9%） | 0.0060 |
| `1341938` | 失败 | 948.1 → 1188.3 MB（+25.3%） | 0.0061 |

- 通过的两次：测量期间发生了 trim。
- 失败的两次：trim 发生在测量开始之前，之后 10 分钟内 RSS 一直涨，没有再 trim。
- 每次的 idle CPU 合计约 3.6 CPU 秒。这和"memo 每 30 s 构造一次约 30 MB 的 JSON，共 20 次"的量级相符。
- 0.0.16 的测量起点更早，正好落在高位，所以这个增长被掩盖了。
- mcppls 侧唯一能让它变绿的办法是"挑测量起点"，而 #59 的 review 已经禁止这样做。所以根本的修复在引擎里（E1）。

## 2. 问题清单（按仓库）

### 2.1 mcppls-clangd 0.0.17（`23.1.0-mcppls.5`）

| # | 问题 | 方案 | 验收 |
|---|---|---|---|
| E1 | memo 周期性构造 JSON（U13 RSS） | 新补丁 `0036`：先比较两个缓存廉价的 changeCount，没有变化就不构造 JSON；最后一次 flush 仍然强制写 snapshot。不改 30 s 周期，不改 JSON 格式 | 原生 canary（旧实现必须失败）；mcppls nightly 中 U13 RSS 增长 < 5%（预算 10%）。runner 在 idle 两端各取一次 `$/memoryUsage`（M6）：如果 MemoryTree 不变而 RSS 仍在涨，再考虑第二步，在 flush 之后调用 `malloc_trim` |
| E2 | 崩溃不可符号化 | release 为 linux-x64/arm64 发布 `objcopy --only-keep-debug` 生成的 debug 文件（按 build-id 命名），为 macOS 发布 dSYM。用户下载的包大小不变 | 拿一个已知崩溃，用发布出来的符号文件离线符号化成功 |
| E3 | U7a：AST 构建时崩溃（栈中有 realloc） | 用 E5 修好的 sanitizer 构建，在本地跑 U7a 序列（U5→U6→U16→U7a）来复现。查到原因就修，并补 lit 回归测试；查不到，就在 #59 中记为未解决，由 W5 降低影响 | 复现成功时：修复前崩溃，修复后不崩溃 |
| E4 | 定时 `e2e` 的 product gate 红（从 10-07 起） | 先归类：是 gate 引用的 mcppls 产品或接口过时，还是引擎行为确实变了。前者改 e2e，后者修引擎 | 定时 `e2e` 变绿 |
| E5 | 定时 `soak` 的 sanitizer 构建中 3 项 modules lit 失败 | 先修 sanitizer 构建下的 module prerequisites 环境（很可能是 resource-dir/标准库路径问题，需要读日志确认），然后给 `build-san` 加上缓存，让它不再每次都冷构建 3 小时 | 定时 `soak` 变绿，热构建时间 < 1 h |
| E6 | 如果 W6 取证指向引擎（例如补全要等 preamble 或模块前置重建） | 根据取证结果决定是否在引擎侧让补全使用旧 preamble | 取证之后再定 |

### 2.2 mcppls 0.0.17

| # | 问题 | 方案 | 验收 |
|---|---|---|---|
| M1（W4） | autosave 下正在输入的文件，每次保存都触发 reload（U15） | 复用 `editedAt + EDIT_SETTLE`：文件打开、并且还在输入时，把结构 reload 推迟到 `edit_quiet_at`，最多推迟 10 s。不改 status 语义，不改预算。`BUILD_PROGRAM_QUIET` 和 `DOOM_RETRY_QUIET` 的合并留到 0.0.18 | 确定性 fixture `mcpp-autosave-import-typing`：旧产品 ≥ 8 次，新产品 ≤ 2 次；nightly 中 U15 ≤ 8 |
| M2（W5） | crash/exit 之后一律隔离 2 min（U9 121 s，U7a 107 s） | ① 没有 crash context 的退出，第一次只记 strike，10 min 内第二次才隔离（针对 U9）。② 有 crash context 的隔离：等 import 闭包里的模块全部构建成功、并且身份变化后（身份取 mcppls 观察到的 `module-built` 序号），交回重试一次（针对 U7a）。其余规则不变；"5 min 内 5 次退出"仍然走退避重启 | 6 个确定性用例，旧产品必须在 2 个上失败 |
| M3（W6） | U5/U6 偶发慢补全（8.26 s，1/12） | 取证：扩展现有的 `slow-request` 记录，加入 `queueMs`、`engineMs`、`answerLayerMs` 和发送时的 clangd fileStatus；completion 的阈值降到 1 s，最多保留 32 条。重复覆盖见 §5。修复方案在取证之后再定（mcppls 侧或 E6） | §5 的验收 |
| M4（W10） | ux-xlings two-core：server 超时，U7a p95 略超 | 从保存下来的失败阶段日志中取出超时的 method 和原因（是 `waited too long for …` 还是 `did not answer … in time`）。如果是 mcppls 的等待上限在两核机器上太紧，就修 mcppls；如果是 clangd 在两核上确实答不过来，带着证据来找你 | two-core 在 §5 的重复测量中通过 |
| M5（W3） | Windows overflow trace 断言 | 先让测试在失败时打印诊断信息。候选解释是：mock 计数的 completion 里包含打字开始之前的那些请求，所以慢请求在客户端的 index 低于 256。在证据出来之前，不放宽断言，也不按平台跳过 | 三个 OS 都通过，原因写入本文 |
| M6 | U13 的归因手段 | runner 在 idle 两端各发一次 `$/memoryUsage`，记录 MemoryTree 和 RSS | U13 的 measure 中有这两个字段 |

### 2.3 CI 与仓库

| # | 问题 | 方案 |
|---|---|---|
| C1 | **nightly 用不了候选引擎**：在 reusable 调用中 `github.workflow` 是 `nightly`，不满足 Candidate engine 步骤的放行条件，会直接报错。结果是 E1 的效果（U13）只能等引擎发布后才能验证，违反"先在产品 CI 中验证引擎"的规则 | 放行**非 main 分支上手动触发的** nightly。定时 nightly、main 和 release 照旧拒绝 |
| C2 | nightly 每次都要重建一遍 ci（约 46 min） | 见 §4 R1 |
| C3 | 没有办法定向、重复地跑 | 见 §4 R2 和 §5 |
| C4（W2） | Windows 真实 fixture 用了旧版 mcpp | fixture 在 `scenario.json` 中声明 `"requires-mcpp": ">=2026.10.5.2"`，由 runner 检查；CI 只要矩阵项里有这类声明，就安装 release 版 mcpp。两个环境变量合并为 `MCPPLS_REAL_MCPP`。默认 pin 本版不动。验收：win32 conformance 上 i686→x64→i686 实际跑通（到目前为止从未通过） |
| C5（W11） | termux 和 Zed 默认模式 | `fcb3450` 已经修了，在 nightly 上实际验证。如果再遇到环境故障，修环境后重跑，不对单个 job 豁免 |
| C6（W12、W1） | 证据包与本地验收 | 见 §3.3；本地验收必须在提交后的干净树上跑，报告里写明 SHA |

## 3. 联动流程与 PR 组织

### 3.1 分支与 PR

- **mcppls**：
  - 从 `fcb3450` 拉出 `release/0.0.17`，后续工作作为新提交加在上面，开一个新 PR，标题写"0.0.17: … (mcppls-clangd 0.0.17)"。
  - #65 关闭，并附上新 PR 的链接。`fcb3450` 同时是新 PR 和 `refs/pull/65/head` 的祖先，这满足 D5 的前提。
- **mcppls-clangd**：从 main（`92a4c63`）拉出 `engine-0.0.17`，开 PR。

### 3.2 顺序（沿用 `docs/92-release.md` 的 "A new engine"）

1. 引擎 PR 的 CI 构建 `clangd-<platform>-portable`。
2. mcppls release PR 提交 `packaging/engine-candidate.json`，指向这次引擎构建。PR CI 和手动 nightly（C1 修好之后）都用这个候选引擎来跑。
3. 两边发现的问题各自修复；候选文件始终指向最新的引擎构建，直到两边都绿。
4. 合并并发布引擎（mcppls-clangd v0.0.17）。然后在同一个提交里，把 `payload.lock.json` 切换到已发布的 archive，并删除候选文件。
5. 用已发布的引擎字节跑一遍 PR CI。如果发布出来的 clangd sha256 和候选版本**相同**，就不再跑 nightly；**不同**，就再跑一次 nightly（§4 R5）。
6. squash 合并 mcppls，按发布流程（§7 第 6 步）发布。

### 3.3 证据包（D5）

1. 不 rebase，也不 force-push 越过 `fcb3450`。
2. 合并前的最后一个提交，用 `git rm` 删除 `portable-handoff/` 下的 `evidence.tar.gz`、`manifest.json`、`archive.json`、`verify.py`。`README.md` 改写成一份指引，写明：
   - 提交号 `fcb3450`；
   - 整包的 SHA-256；
   - 获取方法：`git fetch origin pull/65/head && git worktree add /tmp/ev fcb3450`。
3. `verify.py` 不在最终树里，所以 `test_check` 和 allowlist 的问题也就自然消失了。
4. squash 合并后核对：main 的树里和 `git rev-list --objects` 里都找不到这个 blob。

## 4. 降低整条链路的构建成本

当前每轮迭代的成本（实测）：

| 环节 | 耗时 |
|---|---|
| mcppls PR CI | 约 46 min（最长的是 ux-xlings 31 min、conformance 22 min、build 12 min） |
| mcppls nightly | 约 78 min，其中 ci 约 46 min，ux 约 20–35 min |
| 引擎 PR CI（build-test） | 31–37 min（缓存命中时）；174 min（未命中） |
| 引擎 release | 87–135 min |
| 引擎 soak | 约 3 h（每次冷构建 sanitizer） |

改进：

- **R1：nightly 复用已有的 payload**。workflow_dispatch 新增输入 `payload-run`（某次 PR CI 的 run id）。设置后跳过 `ci` job，下游 job 用 `download-artifact` 的 `run-id` 取 `payload-<platform>`。
  - 效果：每次 nightly 从约 78 min 降到约 35 min。更重要的是，nightly 测的就是 PR CI 测过的**同一份字节**。
  - 定时 nightly 不受影响，仍然完整构建。
- **R2：定向运行**。workflow_dispatch 新增 `fixtures`、`stages`、`repeat` 和 `payload-from` 四个输入（§5）。只跑需要的部分：
  - 验 U13：只跑 ux-xlings 的 long 阶段，约 15 min；
  - 验 U5/U6：只跑 edits 阶段的重复测量。
- **R3：引擎在本地迭代**。补丁在本地构建 clangd，换进 payload 来跑 fixture（做法见 local-engine-payload-swap 记录：修 `payload.json` 中的 identity 和 sha256，并用 `payload --verify` 检查）。`CLANGD_TRACE` 和 `--log-level debug` 用来看 span 与 memo 计数。引擎 PR **只在里程碑时推送**（补丁就绪、修复完成），不每改一处就推。
- **R4：mcppls 先本地、后批量推送**。本地在干净树上跑通 32 组 LSP 和相关 fixture 之后，再把几个提交一次推上去，避免每个提交都花 46 min 跑 CI。
- **R5：引擎发布后的复验**。如果引擎 release 产出的 clangd 字节和候选版本一致，只需要跑一次 PR CI；不一致才再跑 nightly。第一次发布时记下两者的 sha256，以后就知道是否需要这一步。
- **R6：soak 加缓存**（E5）。`build-san` 改用和 build-test 相同的缓存策略。

## 5. 在测试内覆盖 12 次，而不是构建 12 次

**原则**：构建和准备只做一次，同一份 payload 在测试里重复跑多个会话。重复的单位是**新起一个 server 会话**，因为离群值看起来是会话级的（同一会话里 U5 和 U6 同时变慢）。

**runner 改动**：`mcppls-conformance run` 新增两个参数：

- `--repeat <N>`：在同一个 workspace、同一份已准备好的缓存上，依次启动 N 个新的 server 会话。
- `--checks <ids>`：只跑指定的检查项，例如 `U5-typing,U6-typing-autosave`。

每次会话的 measure 单独保存（`-rep<i>`），另外输出一份汇总：每次的 p95/max、超预算的次数，以及各次 server 证据中的 `slow-request`。预算不变。

**分布方式**：

- 12 次重复分到 4 个并行 job，每个 job 3 次。这样既有同机重复，也覆盖不同的机器。
- 每次会话约 4.5 min（U5 + U6），每个 job 约 15 min 加准备时间，整轮约 30 min，不需要额外构建。

**同机 A/B**：

- `payload-from=v0.0.16` 时，每个 job 在同一台机器上**交替**跑 0.0.16 的 payload 和候选 payload，顺序为 A B B A。这样可以抵消时段和机器的差异。
- 1.3 节已经说明，机器和时段的噪声足以把同一份代码判成红。
- 0.0.16 的 payload 来自 v0.0.16 release 的 `payload-linux-x64.tar.gz`。runner 要能容忍 0.0.16 没有的报告字段（当作 null），并记录缺了哪些字段。

**次数与统计（说明）**：

- 如果离群率确实是 1/12，12 次重复能看到它的概率约为 65%；24 次约 88%；36 次约 96%。
- 所以分两个阶段：
  - **取证阶段**：跑 3 轮 × 12 次，每轮约 30 min，用来抓到离群值和它的日志。
  - **验收阶段**：最终 head 跑 12 次，并且必须与 0.0.16 做同机 A/B。

**D2 判定**：

1. 最终 head 的 12 次全部在预算内，并且在同机 A/B 中，候选版本 max 的中位数不比 0.0.16 高出两组合并标准差的 2 倍以上，才算通过。
2. 取证阶段抓到离群值时，必须先归因并修复（在 mcppls 或 E6），然后重新验收。

U13 和 two-core 也走同样的方式：U13 每边 3 次（只跑 long 阶段）；two-core 用 `taskset -c 0-1` 跑 6 次。

## 6. CI 发布标准

### 6.1 底线（不低于 0.0.16）

- **mcppls PR CI**：每个 job 都通过，包括新增的 fixture（0.0.16 发布时 PR CI 是全绿的）。
- **mcppls nightly**：
  - 0.0.16 上一直是绿的 job，必须是绿的。
  - 0.0.16 上就是红的 job，原因不能变，结果也不能变差。
- **引擎 PR CI**：build-test、verify-series、win-historical-crash-capture 都通过，和 0.0.16 的 PR 相同。

### 6.2 目标（尽量全绿）

- mcppls nightly 的每个 job 都通过，包括 continue-on-error 的 termux 和 two-core。0.0.16 上红的每个 job，这一版都有对应的修复：U13 → E1，two-core → M4，termux → C5，U9 → M2，U15 → M1。
- 引擎的定时 `e2e` 和 `soak` 变绿（E4、E5）。
- 有余量：U15 ≤ 8；U13 的 RSS 增长和 CPU 都低于预算的 50%。

### 6.3 最终判定

- 在最终 head 上（引擎已发布、候选文件已删除），用 R1 复用 PR CI 的 payload，**跑两次 nightly**，加上 §5 的验收重复测量。
- 某个 job 红了先归类：
  - **环境问题**（镜像、限流、runner 故障）：修好环境后重跑。
  - **产品问题**：修复之后，最终 head 改变，计数从零开始。
- 不改预算，不挑测量起点，也不靠重跑直到变绿。
- 如果只达到底线、没达到目标：把没变绿的项目和证据带给你决定，不自动放行。

## 7. 执行顺序

1. **第一批，两边同时开工**：
   - CI：C1（nightly 可以用候选引擎）、R1（复用 payload）、R2 和 §5 runner 的 `--repeat/--checks`。这些是后面所有验证的前提。
   - 仓库：C6、C4、M5（先上诊断版）。推一次 PR CI，跑第一次 nightly。这一次 nightly 同时完成三件事：验证 C5，为 M4 拿到日志，并作为 0.0.17 head 的第一个基线。
   - 引擎：E5（修 sanitizer 构建并加缓存），E1 在本地开发。
2. **第二批**：
   - mcppls：M1、M2、M3 取证字段、M6。
   - 引擎：E1 推到引擎 PR；E3 用 sanitizer 构建在本地复现；E4 归类；E2 符号发布。
   - mcppls PR 加上 `engine-candidate.json`。
3. **取证与修复**：
   - §5 取证阶段跑 3 轮 × 12 次，抓 U5/U6 离群值和 two-core 的日志。
   - 按结果修复：M3/M4，或者 E6。
   - U13 用候选引擎做 3 + 3 次 A/B。
4. **收口**：
   - 引擎 PR 全绿，合并并发布 v0.0.17。
   - mcppls 切换 lock、删除候选文件。
   - 跑原九阶段、两个 profile 的 workspace、32 组 LSP、两次 nightly，以及 §5 的验收。
5. **D5**：最后一个提交删除证据包。
6. **发布**：squash 合并，然后依次完成：
   - main CI；
   - draft release；
   - 逐一下载 12 个资产并校验；
   - 用两个原 0.0.16 profile 实际升级；
   - 正式发布，并再次下载回验；
   - 核对 main 中没有证据包的 blob。

## 8. 验收清单

**引擎**

- [ ] E1：原生 canary 中旧实现失败、新实现通过；mcppls nightly 中 U13 RSS 增长 < 5%
- [ ] E2：发布的符号文件能离线符号化一个已知崩溃
- [ ] E3：U7a 崩溃已复现并修复；或者在 #59 中记录复现过程和未解决的原因
- [ ] E4、E5：定时 `e2e`、`soak` 变绿；`soak` 热构建 < 1 h
- [ ] 引擎 PR CI 全绿；v0.0.17 已发布；mcppls 已切换 lock

**mcppls**

- [ ] M1：fixture 中旧产品失败、新产品通过；U15 ≤ 8
- [ ] M2：6 个用例全部通过，旧产品在其中 2 个上失败
- [ ] M3：§5 验收通过；离群值已归因，或者在 3 × 12 次取证中都没有出现
- [ ] M4：two-core 通过 6 次重复
- [ ] M5：三个 OS 都通过，原因已写入本文
- [ ] C4：win32 上 `mcpp-target-windows-real` 实际通过
- [ ] C5：termux 和 Zed 默认模式在 nightly 中通过
- [ ] PR CI 全绿，最终 head 上两次 nightly 全绿（或者已按 §6.3 把未达标项带来请你决定）
- [ ] 原九阶段完整通过；本地两个 profile 的验收基于干净的提交树
- [ ] D5：最终树里没有证据包，main 中查不到它的 blob
- [ ] 发布流程完成；关闭 #63；更新 #59

## 9. 风险

- **E3 可能复现不出来**：崩溃可能依赖 CI 机器的时序。缓解：sanitizer 构建会把很多时序相关的问题变成确定性的报错；即使根因没查到，M2 也能把用户感受到的影响降到几秒。
- **E4、E5 可能牵出引擎测试体系里更多的问题**：它们从 10-07 起就是红的，原因还没读过。如果工作量超出预期，按 §6.3 带来请你决定，不阻塞底线。
- **M3 离群值可能在取证阶段抓不到**：36 次都没出现时，95% 置信度下离群率 ≤ 约 8%。到那时，按"验收的 12 次全部通过"放行，并把取证字段随 0.0.17 一起发布，用户导出的诊断包里也会带上这些字段。
- **C1 放宽了候选引擎的使用范围**：只放行非 main 分支上的手动 nightly；release workflow 中"拒绝候选文件"的检查保持不变，所以候选引擎仍然到不了发布版。
- **M1**：正在输入 import 的用户最多要等 10 s 才能看到模型更新。`import` 行的模块名补全由 mcppls 自己提供，不受影响。
- **M2 规则 ①**：一个没有 crash context 的真实崩溃循环，会多崩溃一次；最坏情况由退避重启约束。
- **成本**：用了 R1–R6 之后，每次迭代的主要成本是 PR CI 约 46 min，加上定向 nightly 约 15–35 min。引擎 PR CI 只在里程碑时触发。

## 10. 实施记录（与设计的差异）

- **W4 的机制被本地复现证实，上限改为 30 s**：新 fixture `mcpp-autosave-import-typing` 按 U6 的方式交替输入
  import 行和函数体（10 Hz，每秒自动保存，40 s）。0.0.16 在该 fixture 上跑出 6 次 producer、13 次状态变化/分钟，
  复现了 U15 的失败；只输入 import 的连续输入不能复现（防抖窗口一直被推后）。设计里的 10 s 上限在不停顿的输入下
  恰好是 6 次/分钟、12 次状态变化，正好卡在预算上，没有余量；改为 30 s 后为 3 次、4 次状态变化。停顿两秒仍会触发
  运行，所以只有完全不停顿的输入才会碰到这个上限。
- **W5**：`clangd-exit-strike`（无 crash context 的 kill）和 `clangd-crash-closure`（指认文件的崩溃 + 闭包重建）
  在 0.0.16 上失败，在新产品上通过；`clangd-crash-context` 等回归 fixture 不变。
- **E2 改为发布未 strip 的 clangd**，不加 `-g`：给每个目标文件加调试信息会让 ccache 超过 GitHub 的 10 GB 缓存上限，
  每次引擎 CI 都变成约 3 h 的冷构建。未 strip 的二进制保留了符号表，没有额外构建成本，足以给崩溃地址补上函数名。
- **E4 根因**：`lsp_driver.py` 要求 `publishDiagnostics.version == doc_version`，而 mcppls 发布的合并诊断不带这个
  可选字段，所以产品层的两个 gate 自 10-07 起一直失败。现在接受不带版本、但落在只存在于最新文本中的那一行的诊断；
  本地 5 个 gate 全部通过。
- **E5 根因**：module worker 用 `RLIMIT_AS` 限制地址空间，而 sanitizer 运行时启动时就已映射了数 TB 的 shadow 内存，
  所以 worker 的第一次 mmap 就失败（"module worker failed: ERROR: Failed to mmap"）。新补丁 0037 在 sanitizer 构建下
  让上限只约束 worker 在已有映射之外新增的部分。本地 ASan+UBSan 构建下三个 lit 测试和 soak 的 kill/restart 循环都通过。
- **E3 未复现**：用本地 ASan+UBSan 构建的 clangd（E5 修好之后才可用），在真实 ux-mcpp 上完整跑了一遍
  cold、warm、edits（U5、U6、U16、U7a、U8）：没有任何 sanitizer 报告，没有 crash context；两次重启都是计划内的
  （引擎数据库变化）。几个预算检查因为 ASan 变慢而超时，与崩溃无关。U7a 的根因按约定记为未解决，影响由 W5 规则 ②
  兜住（`clangd-crash-closure`）。
- **M5（W3）根因**：新加的诊断输出显示 Windows 上只发出了 65 个补全请求。Windows 的 sleep 精度是 15.6 ms，
  "1000 Hz 打一秒"实际只有约 64 次按键，mock 故意放慢的第 270 个请求根本没有发出。所以"统计和 trace 不是同一组请求"
  这个最初的解释不成立，"最慢的那个落在 256 之前"也只是表象。测试改为 100 Hz 打 6 秒，三个 OS 上都至少有 300 个请求。
- **C4（W2）的真正阻碍不是 mcpp 版本**：换成 2026.10.11.1 之后 prepare 能通过了，但 clangd 构建 MSVC 的 `std.ixx`
  失败（`<generator>: unknown type name 'suspend_always'`）。原因是 Clang 对 32 位 x86 MSVC 目标不定义
  `__cpp_impl_coroutine`（见 clang 的 `InitPreprocessor.cpp`："Support for coroutines on 32-bit x86 Microsoft
  platforms is incomplete"），MSVC STL 的 `<coroutine>` 因此为空。也就是说，任何 Clang 都无法为 i686 构建 MSVC 的
  `std` 模块，mcppls 退回语义工具包是正确的做法。`fcb3450` 给这个 fixture 加的 `import std` 已撤掉；64 位 MSVC 的
  `std` 仍由 `mcpp-llvm-msvc`、`mcpp-split-msvc` 覆盖。这一限制已写进 docs/20-projects.md。
- **CI 权限**：本仓库默认的 token 只有 contents/packages 读权限。`zed-smoke.yml` 声明 `actions: read` 会让
  `ci.yml` 一启动就失败（startup_failure），所以改为继承调用方的权限，只有 nightly 授予 `actions: read`。
- **M6 未实施**：clangd 的 `$/memoryUsage` 要经过 mcppls 转发，涉及协议面。U13 的效果改由同机 A/B 直接衡量
  （`ux-baseline`、`measure compare`）。
