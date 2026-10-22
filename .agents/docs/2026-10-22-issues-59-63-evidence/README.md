# #59 / #63 验证简报

日期：2026-10-22 JST。状态：**实现已提交，完整验收仍有失败项；PR 保持草稿。**

本目录只保留结论、复现方法及原始记录索引。45 份原始记录共 26,769 行、约 1.0 MB，已移出 PR；失败、对照和历史检查点均保留。源码方案见 [详细设计](../2026-10-22-issues-59-63-target-and-reliability-design.md)，操作见 [复现说明](reproduce.md)。

**构建配置边界：**早期本地历史性能链使用 dev server/runner，后续 release 检查点逐项另列。CI/nightly UX 则使用 release server/runner（`ci.yml` 的 payload build→stage 打包→UX 下载链，nightly 复用该 payload）。两者不能混称。复现默认已对齐 release，并补齐 xlings 的 faults 阶段；同一最终产品字节的 release 原完整链已完成，不能用它覆盖已有 dev、U5 或中断记录。

## 结论与证据边界

| 项目 | 已证实结论 | 当前验收状态 |
|---|---|---|
| #63 目标选择 | mcpp provider 未传 target 会采用默认架构；新增 auto / project / 显式 triple，统一请求、生效目标和缓存身份 | Windows 原生暴露长路径/8.3 短路径归属错误，已统一三处判断；本地 24 组 LSP 通过，新的原生平台 CI 待完成 |
| #59 producer 升级 | 缓存旧 producer 身份不能证明当前 producer；已改为当前身份确认 | mock 实际调用、版本、不同输出及 executable 替换回归通过 |
| UTF-8 / Termux | runner 存在多字节裁剪缺陷；内层 Debian 缺脚本绑定 | UTF-8 native 回归通过；Termux 实际环境待 nightly |
| U15 停顿 | 节点 done 不变时仍可能完成实际模块；不能据此认定引擎无进展 | 历史两逻辑核 cold：原节点间隔 77.73 s、真实无活动 4.00 s；faults：59.66 s / 7.42 s，均无 stalled 样本。最终完整链不能用历史通过替代 |
| U8 撤销 | 两核重放发现 28 次构建；旧 release key 全保留，11 个新增 PCM 使用 release 命令及旧 dev 依赖闭包 | 已修 CDB 在旧进程结束前发布的问题；另修测试未等待有限工作完成。native 回归通过；`fb1cb51` 原两核 mcpp 及 xlings edits 均 undo=0；此前 900 s 丢调度失败与补修见下文 |
| U13 空闲 | 原 xlings 完整链曾通过 600 s 门槛：0.007208 core、RSS -50.28% | 源码 `5e19005` 完整链通过：CPU 0.006125 core、RSS -50.38%；独立 memo 局限见下文 |
| SIGSEGV | 两次原生两逻辑核 runner cold 退出 139；GDB2 捕获 reader，GDB5 观察到 server SIGSEGV，GDB7 捕获 runner 主线程 | **未解决**。GDB7 live text 与冻结 ELF 完全一致；不能仅凭堆栈名称归为 parser、delete 或硬件问题。GDB2 合法 429,463 B completion、深度 7；各线程历史栈使用不足 100 KiB，不支持栈耗尽 |
| 两核 warm | `5e19005` 首次 ready 17.99 s，超过原 12 s；保留失败 cold 后的真实 cache lineage | 历史失败不覆盖；`fb1cb51` 新原链 cold/warm 通过，warm ready 5.809 s，零重建 |

U8 修复同时处理驱动和测量：需要重启时，新 CDB/生成输入等旧进程 reaped 后才发布；普通增量先生成 prime/stub 再发布引用它们的 CDB。写失败不报成功，最新计划可重试。测量按明确任务状态有界等待，撤销前抓计数，包含撤销后所有快速构建，零重建预算不变。

两核 edits 的末态为 preparation 156/156、所有请求/构建队列为零，但 implementation sweep 同时 `sweepComplete=false`、`sweepScheduled=false`。源码中普通计划应用会重置 complete，却可能没有后续事件调用 pump。补修是在成功增量 apply 后安排真实扫描；不能把这个末态直接视为空闲或放松 900 s 上限。新增 native 对照已验证：同一回归在旧实现有 4 个断言失败，补修版 65 cases / 650 assertions 全通过；覆盖重新调度、实际文件重建、在途任务可见和不重启。原运行未执行配置切换，不能用于证明 undo 零重建。原始对照位于 `.payload-cache/59-63-sweep-validation/{before-dev,fixed-dev-final}.log`。

`5e19005` 的 xlings 完整链四阶段均通过：cold 112.687 s、warm 317.797 s、edits 645.876 s、long 782.284 s。U13 固定 600 s 空闲中 RSS 1272.258→631.234 MiB，CPU 0.006125 core；此前有限工作等待 60.067 s、约 0.949 core，单独记录。原始结果位于 `.payload-cache/59-63-performance/xlings-validated-staged-{cold,warm,edits,long}.json`；两核失败记录为同目录 `mcpp-validated-two-core-edits.json`。它们是快照归档之后的新结果，不混入旧哈希索引。

`fb1cb51` 对应冻结 dev 字节的两条历史链已结束：mcpp cold/warm/edits/faults 为 178.400/315.871/548.177/266.397 s；xlings cold/warm/edits/long 为 118.072/315.753/660.035/764.864 s，记录的检查均通过。mcpp / xlings warm firstReady 为 5.809 / 5.604 s，已有 BMI 176 / 111，零重建；U8 撤销实际构建与物理变化均为零。xlings long 的固定 600 s 空闲 CPU 0.005301 core，RSS 1333.805→672.992 MiB（-49.54%）。该本地 xlings 链未执行 faults，不等于完整 nightly round 1。

最终自审另补修单次 CDB 发布失败后的恢复：旧实现仅发布 pending 文件，没有完成首份/增量计划的内部状态；现在在停止接收流量时重新应用完整快照，再启动引擎。新增回归不再第二次 apply，旧代码 5 个断言失败，补修 dev/release 各 40 执行体通过（engine 66 cases / 673 assertions）。对照在 `.payload-cache/59-63-publication-retry/`，后续完整性能链仍需对应补修字节。

恢复补修 `dc12357` 的 dev mcpp 链已结束：cold 139、warm firstReady 22.96 s 超过 12 s，edits/faults 通过，撤销实际构建为零。dev xlings cold/warm/edits/long 均通过；原 600 s 空闲 CPU 0.009926 core，接近 0.01 门槛，RSS 1448.520→820.371 MiB（-43.36%），不视为有充足 CPU 余量。未修改的 PR 前 0.0.16 dev 基线也观察到 SIGSEGV，系统 GDB 另发生 SIGILL；这只能说明基线也有异常，不能排除本 PR 的其他崩溃路径或确定硬件原因。原始记录位于 `.payload-cache/59-63-performance/` 的 `baseline-016-envelope.json`、`diagnostic-sigsegv-summary.json` 及 `mcpp-retryfix-two-core-*`。

同一提交的独立 release mcpp 链 cold/warm/edits/faults **全部通过**（160.576 / 310.311 / 517.397 / 234.596 s），warm firstReady 4.136 s，176 个已有模块零重建、零首次构建；edits 撤销实际构建与物理变化均为零。faults 最长真实无活动 6.522 s、无 stalled 样本、无非计划引擎重启。release xlings cold/warm/edits/faults/long 五阶段也全部通过（101.591 / 310.187 / 623.094 / 176.416 / 869.289 s）；原 600 s U13 CPU 0.006091 core，RSS 1082.797→1102.410 MiB（+1.81%），peak 1543.660 MiB。身份见 `release-retryfix-envelope.json`，阶段结果为 `mcpp-release-retryfix-two-core-*`。这仍不等于所有平台与完整验收成功。

此前 `dc12357` 的 [nightly 39675199436](https://github.com/Sunrisepeak/mcpp-language-server/actions/runs/39675199436) 中，复用 CI 的 xlings cold/warm/edits/faults 已全通过，下载 `ux-ux-xlings` artifact 的四份 measure 均零失败（119.405 / 310.503 / 685.644 / 176.905 s）；warm 零重建、配置撤销零重建。该 CI 场景不含 long，不能替代 600 s 验收；原始下载在 `.payload-cache/59-63-nightly-dc12357-ux-xlings/`。Linux 第一组 conformance 同时通过；旧整条运行因已确认的路径/未信任缺陷被补修任务替代，其失败记录保留，新完整平台验收另行执行。

最终产品源码 `a9483e1` 的冻结 release mcpp 重放四阶段全部实际退出 0（175.727 / 310.318 / 517.146 / 270.194 s）；warm firstReady 4.2 s、176 已有模块零重建，U8 撤销实际/物理构建均零，faults 最长真实无活动 9.443 s、无 stalled 样本。xlings 新完整重放五阶段亦全部退出 0（102.154 / 311.380 / 627.425 / 171.530 / 748.934 s）；原 600 s U13 CPU 0.005372 core，RSS 1287.348→1152.652 MiB（-10.46%），peak 1831.254 MiB。有限工作 60.012 s / 0.944925 core 单列，不混入空闲测量。原始记录为 `.payload-cache/59-63-performance/{mcpp-member-alias-replay-two-core,xlings-member-alias-replay-staged}-*`。

同一源码此前 xlings U5 确实失败：诊断刷新 7.0618 s 超过原 6 s；第二轮原场景为 1.9 s。旧 mcpp warm 和 xlings edits 外层脚本还在写完 measure 后退出 143，未进入后续阶段，信号发送者未知；ptrace 被系统拒绝，未更改系统设置。两次中断及 U5 失败均保留，不用新通过覆盖。U5 两窗口没有项目重描述或模块构建，仅有 clangd AST 工作；归一化工作区/缓存后 CDB 参数相同，但不足确定耗时差异的原因。

当前 CI 的 Linux x64 / Windows 第二组原生 conformance 已通过，untrusted 分别 8.2 / 16.0 s。Linux ARM64 target fixture 暴露测试供给缺口：已安装系统 clang，但 mock 未收到绝对编译器路径，输出裸 `clang++`，违反 S1-6-3 的 driver 约定并被按数据库目录解析。补修仅在 cross-tools job 为 `CONFORMANCE_CLANGXX` 提供可执行的绝对路径，保留产品、平台矩阵和原 target 断言；产品二进制与性能链不变，新 CI 仍需完整通过。

旧 Windows 第一组取消后的日志确认 selection/windows/race/live/auto-fallback 均为 0 failure；mismatch 的 C1 却把报告脱敏后的 `~/AppData/...` 当作真实路径，未完成缓存故障注入，后续失败不能用于判断缓存拒收行为。仅该 helper 改为明确请求 `redact:false`；默认报告请求、2 s drain 超时、谓词及所有性能参数不变。Linux home 内缓存的旧 runner 诊断对照准确重现 C1 失败；修后原始完整 mismatch fixture 在 dev/release 均通过，runner native 各 6 cases / 65 assertions 通过。记录位于 `.payload-cache/59-63-cache-report/`。服务器 SHA 仍为下表 730294c5；该后续 helper runner SHA 为 `0028e2c56181c0d0202238f49731b0cc13f195e785b3681b149d4a008c6f1434`，不冒充此前九阶段 runner 字节。

同份旧 Windows 日志另有 U17 真失败：73 次请求 / 24 个词 = 3.04，超过原 2.0 门槛。保留窗口见 42 条不确定缓存先答、随后引擎回答，clangd 508–766 ms；不能把重询全归为引擎结果截断。相关 typing/answer/completion/参数生成代码与 0.0.16 一致，但不等于排除本 PR 的间接影响；此前同 fixture 曾通过 44/24。当前没有确定根因，继续保留原 2.0 gate 等新完整 CI 验收。精简分析为 `.payload-cache/59-63-performance/u17-windows-analysis.json`。

<a id="validation"></a>
## 当前本地补修检查点

`2d36600` 的 nightly macOS 原生 conformance 实际失败（run `39694307079`，job `119552078685`）：mock producer 在准备步骤直接异常退出，旧 generated-module 场景因此反复超时，并非新 target 断言已运行。真实 cross artifact 的初始化表与反汇编确认：24 字节全局默认 target 字符串超过 libc++ SSO 22 字节，在 `okm_early_init` 前调用 allocator；后者读取尚为空的 `libc.auxv`。代码签名页哈希全部一致；未采集 Mac crash report，不把静态证据说成完整平台运行证明。原 artifact、SHA 和最小反汇编保存在 `.payload-cache/59-63-macos-mock-startup/`。

补修仅把 mock 状态放进 `main` 并显式传参，另给原生两 profile 和打包工具增加协议启动检查；prepare 失败显示程序、退出码及超时状态。release server SHA 仍为下表 730294c5，原九阶段性能产品字节不变。Linux dev/release 已构建，mock 启动与 runner native 各 6 cases / 65 assertions 通过；dev 31 组完整 LSP 场景通过。release 场景与原生 macOS CI 仍须验证，当前失败未放行。同期 Windows 第一组、ARM64 与 Linux 目标场景均通过；Windows 原 U17 为 40/24=1.67、零超时，旧 73/24 失败与未决根因仍保留。

路径身份、未信任回退及声明成员别名补修使用同一组冻结 release server/runner；同一 server 字节的原完整性能链已完成；最后原生平台 CI 和交付升级仍待验证。测试 helper 的后续字节单独标记，不混称性能 runner。

| 检查 | 结果 | 原始记录（`.payload-cache/59-63-member-alias/`；特殊前缀另注） |
|---|---|---|
| Linux dev / release | 各 40 个执行体通过 | tests-dev-complete.log / tests-release.log |
| native 回归 | engine 67 cases / 693 assertions；target 9 / 119；runner 6 / 65，均零失败 | 同上 |
| untrusted 旧代码对照 | 新 Workspace 回归实际执行，旧 guard 6 个断言失败；补修后通过 | ../59-63-path-identity/before-untrusted-dev.log |
| 真实 LSP | 23 组目标/producer/polling/untrusted + 显式目标 untrusted，共 24 组通过 | fixtures-results/summary.json / untrusted-explicit.json |
| 规范与仓库 | 0 failure，316 规则，4 个历史 pending；仓库检查通过 | specs.log / check-all.log |
| 原生 Windows | 旧提交暴露 8.3/长名归属错误；产品已统一三处判断，原五个 fixture 保留 | 旧 CI 日志在本目录；新平台矩阵待完成 |

声明成员别名在旧 boundary 对照中有 8 个断言失败；修复后 literal 与一层 glob 都保留归属，目标冲突不再因跳过成员而被误判为唯一。对应 `before-dev.log`。本次第一轮全量 dev 中两个引擎时序 case 有 3 个断言失败，完整日志 `tests-dev.log` 保留；未改代码/预算，随后单独 engine 67/693 与全量 dev/release 各 40 均通过。不能把首次失败抹去或据此确认环境根因。`7ba8b24` 刚启动的性能链因已确认成员兼容回归而停止并记为 superseded，不记 PASS 或预算失败。

当前冻结字节的真实 VS Code 1.143.0 E2E 为 44 passing / 1 平台预期 pending，工作区未改（`vscode-e2e.log`）。

此前 `dc12357` 的实际 VS Code 1.143.0 E2E 为 44 passing / 1 平台预期 pending，扩展单测 137 passing；其 CI Linux payload 下载校验及 21 项真实 clangd Windows-target 检查通过。官方 0.0.16 profile/cache 克隆升级至该 CI VSIX 的预检通过：旧无选择身份模型重新生成，定义导航正常，工作区未改，原旧 profile/cache 保持不变。它们是补修前检查点，正式 release 下载与升级尚待执行。

## 环境与二进制身份

- Linux x86_64 / i9-13900K：mcpp 两逻辑核为 4–5，xlings 为 12–15；逻辑 CPU 不等于物理核心。构建和测试限定 24–31，不跨亲和性宣称加速。
- 本地构建 mcpp 2026.10.21.1 / LLVM 22.1.8；性能 server producer 显式固定 2026.9.30.2，fixture prepare launcher 另外记录。
- 引擎为已校验的官方 v0.0.16 payload，`23.1.0-mcppls.4` / fork `92a4c630f4691890e6133c87d3e4591bc5fd8258`；本期未修改引擎。
- fixture pins：mcpp `d1f1c98fa26d953e084a183a9672fc1e0ce63558`，xlings `84572b0cc8502a16afaa4ae00867aef17f18c2c2`。历史二进制身份留在索引与各运行 envelope，不混用不同提交的记录。

| 完整性能链 release 二进制 | SHA-256 |
|---|---|
| server | 730294c58cffd92b694ea72eeb39ccdf57f710a57cdcf5930c0199860651611d |
| runner | b3fb89f81ebaf4b83ed89edaa84cc666836e8af8f25f2c9cbe9c3c82e9f67b68 |

## 后台补全节奏修复：新验收进行中

`0e2a146` 的 PR CI `39702173244` 在 Windows U17 实际失败：66 请求 / 24 词 = 2.75，42 次 incomplete 重询；同源码 nightly 的该场景为 46/24 = 1.92，通过原 2.0 门槛。两份原始日志均保留，不能用后一次通过抹掉失败。macOS 两组原生 conformance 已通过，之前的 mock 启动问题已有平台运行证明。

已定位并补修 mcppls 的独立缺口：K1 缓存先答后，引擎的后台回答未进入文件已有的 completion pace；计时也从后台 detach 才开始，漏掉此前等待。现在从原请求开始计时，并将成功的 completion/signature-help 后台回答记录一次；C-2 与答案层共同保留的请求不重复记样。detach 的过期、淘汰和通知时效仍沿用原时间。等待上限、`isIncomplete` 和 U17 原输入/门槛不变。

同一实际 Workspace/HeldCore 回归在旧实现产生 4 个断言失败（只记到约 150 ms 尾段、下一请求过早缓存先答等），修复后 dev 的 17 cases / 112 assertions 全通过。原始对照在本地 `.payload-cache/59-63-background-completion-pace/{before-dev,fixed-dev}.log`。这证明漏记缺口已修复；Windows U17 是否稳定通过仍须新平台 CI 验收，不能由该单测替代。当前产品字节已改变，上表旧九阶段结果仅为历史记录，新的完整九阶段性能链也必须重跑。

修补后的 dev/release 完整 workspace 均为 6 members / 57 test programs / 0 failed；runner 新回归各 7 cases / 88 assertions。实际 VS Code 1.143.0 为 44 passing / 1 macOS-only pending，退出 0，workspace unchanged。规格、devtools 全检查通过。完整 LSP、原九阶段性能链及新平台 CI 尚在验收，不能将上述局部结果写成全部放行。

为核对下一份 U17 证据，runner 追加最多 256 条逐词请求元数据及丢弃计数，不存正文或候选标签；原生 CI 保存首次运行的 measure artifact，诊断重跑不覆盖失败。详见 [复现步骤](reproduce.md#7-后台补全节奏与逐词证据)。

<a id="memo"></a>
## 已定位但未修复的引擎局限

无打开文件、关闭 background index、仅载入 30,298,617 B memo 的隔离实验中，180 s RSS 从 176.832 增至 266.754 MiB（+50.85%），零 memo 写入。官方 [0030 patch](https://github.com/Sunrisepeak/mcppls-clangd/blob/92a4c630f4691890e6133c87d3e4591bc5fd8258/patches/0030-UP-35-module-input-sources-and-memos.patch) 的 timer 每 30 s 先构造全部 JSON，再检查版本是否变化；warm adoption 不增加该版本。这是独立分配问题，不证明持续泄漏，也不等同于原 U13 gate 失败。

合理的引擎修复是先检查廉价变更计数，再构造有变化的快照并保留其对应版本；本期没有以关闭 persistence、等待首个 written 日志、固定延时或调整 RSS 起点代替修复。原 U13 的 600 s / 0.01 core / 10% RSS 门槛保持不变。

<a id="raw"></a>
## 原始记录如何核对

[raw-index.tsv](raw-index.tsv) 逐文件列出大小与 SHA-256；所有历史失败和对照均在索引中，未按通过与否筛选。

- 本地归档：`.payload-cache/59-63-evidence-raw/checkpoint-20261021T202025Z-5e19005.tar.gz`
- 归档 SHA-256：`674b5072ddfb27204aef5bc2de61cf13ad38176f3e5a79c00a5cda382e15a8c0`
- 全部运行日志、ledger/PCM 快照及新阶段结果：`.payload-cache/59-63-performance/`。core 单独留在该目录，未纳入 PR 或这个 JSON 归档。
- 已结束的大日志以 `.log.gz` 无损压缩，`compressed-logs.jsonl` 记录原始字节数、SHA-256 和解压核验结果；原 `.log` 路径改用 `gzip -cd 文件.log.gz` 读取。当前运行日志、JSON、core 和上述历史归档未改变。
- 归档是截至该时间的历史快照；后续结果持续记录在简报及 CI artifacts。CI/nightly 已上传各阶段 measure；新复现默认也写入独立输出目录。

原早期 `/tmp` 跟踪曾在环境重建时丢失，不能冒充完整原始日志；已保留的 JSON 与之后持久目录的新证据区分记录。
