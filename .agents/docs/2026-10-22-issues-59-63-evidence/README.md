# #59 / #63 验证简报

日期：2026-10-22 JST。**PR #64 已合入；PR #65 的产品补修在提交 `1341938` 通过 PR CI 与本地原九阶段。最终范围审计补充真实 mcpp Windows 双目标链，新增验收仍待原生 CI；nightly 和正式发布/升级尚未完成。**

本目录只收结论、[复现步骤](reproduce.md)和 [SHA-256 索引](raw-index.tsv)。45 份原始记录共 26,769 行，已移出 PR；失败、对照、日志与 core 均保留。源码边界见 [设计方案](../2026-10-22-issues-59-63-target-and-reliability-design.md)。

## 归属与实现

| 问题 | 判断及处理 |
|---|---|
| #63 多目标项目 | mcpp provider 未传 target 会采用默认架构，属于 mcppls 的目标选择缺口。无需改 mcpp；auto 读取根 CDB 的唯一显式目标，project 用 mcpp 默认，显式 triple 用于项目配置。未声称恢复全部 profile/features/toolchain；不按目录时间或宏猜目标。 |
| #59 producer 升级 | 旧缓存的 producer 身份不能证明当前工具。现确认 executable 身份、能力及选择输入；旧/跨目标模型只作临时数据，当前 producer 确认后才正式采用。 |
| 路径与信任 | Windows 短/长名、声明成员 literal/一层 glob alias 共享项目归属；原 argv 不改。不受信任根不运行工具、不循环丢弃 source-scan 模型。 |
| U8 配置撤销 | 真实发现旧引擎消费新 CDB：需要重启时，等旧进程 reaped 才发布新 CDB/生成输入。另修增量 implementation sweep 丢调度、发布失败后的完整恢复。等待显式有限工作，零重建预算不变。 |
| U15 停顿 | 节点 done 不变不等于引擎没工作。记录真实 module completion 与无活动间隔，不虚增完成计数；保留原节点间隔、60 s 门槛及零 stalled 断言。 |
| U17 输入补全 | 后台引擎回答漏记文件 pace，延迟也只记 detach 后尾段。现从原请求起计时并记一次，等待上限、完整性标志与输入不变。旧实现同回归 4 个断言失败，修后通过。 |
| 只读符号 R1 | Windows nightly 从 `build.mcpp` 重命名接口 `cxxflag` 时仅返回项目引用修改，绕过既有文件过滤。属于 mcppls 的保护缺口：先解析 definition/declaration 归属，再请求修改；只读、未知、过期、取消或超时均不返回部分修改。项目/可写库符号仍可编辑，未改 mcpp 或引擎。 |
| 测试供给 | 修 UTF-8 裁剪与写失败处理、Termux 内层脚本绑定、ARM64 mock 的绝对 compiler 路径、缓存注入使用真实路径。Mac mock 全局字符串早于 allocator 初始化，已移到 main；原生平台证明通过。 |

<a id="validation"></a>
## 已完成检查点（不替代新增产品验证）

产品提交 `bcc9cbe`，本地 release server SHA `a958f0c8d20a02326c2b49741a6139daa8773d920a2bc95b433644f1baeba288`；原九阶段 runner SHA `53bd21e2cea543eb0572d02ba72df16c2478eed177557b040af50f1aea00fe43`。后续 SC4 仅改 runner，身份另记，不能混称字节相同。

| 检查 | 实际结果 |
|---|---|
| dev / release 全 workspace | 各 6 members / 57 test programs / 0 failed；答案路由 17 cases / 112 assertions；当时 runner 7 / 88。 |
| 完整真实 LSP | 每种 profile 31 组 target/producer/polling/untrusted 及旧构建程序场景，全部退出 0。 |
| 实际 VS Code 1.143.0 | 44 passing / 1 macOS-only pending，退出 0，workspace unchanged。 |
| 规范及仓库 | 0 failure；316 规则、4 个历史 pending；仓库检查通过。 |
| PR CI 39708049797 | 六个原生 OS/profile 单测全部通过；Windows 两组、macOS、Linux ARM64 conformance 已通过。Linux 第二组后续 timing 的 SC4 失败，整轮不能放行；整轮最终为 43 success / 1 failure / 2 skipped。 |
| 首次 Windows U17 | 47/24 = 1.96，原门槛 2.0；p95 0.68 s / max 0.73 s，零超时/错误。完整 47 条逐词 trace，零丢弃；不宣称没有波动。 |

原 release 九阶段保持原 fixture、pins、producer、CPU 配额与缓存 a/a/a/b（xlings long 回 a）：

| 项目 | cold / warm / edits / faults / long（秒） | 关键结果 |
|---|---|---|
| mcpp 两逻辑核 4–5 | 162.484 / 312.525 / 546.115 / 252.307 / — | 全部退出 0；warm 3.9 s、176 已有模块零重建；undo=0；faults 真实无活动 11.238 s、stalled/error=0。 |
| xlings 逻辑核 12–15 | 106.646 / 310.373 / 655.098 / 178.562 / 766.477 | 全部退出 0；111 已有模块零重建；undo=0；原 600 s U13 CPU 0.004387 core、RSS 1397.457→668.309 MiB（-52.18%）、peak 1599.121 MiB。 |

有限工作 76.093 s / 0.966528 core 单列，不混入空闲起点。记录为 `.payload-cache/59-63-performance/{mcpp-background-pace-final-two-core,xlings-background-pace-final-staged}-*`；两份 summary 和 envelope 记录版本、SHA、阶段退出码。构建/测试 CPU 24–31；producer 固定 2026.9.30.2。逻辑核不冒充物理核心。

## SC4 修补及新增 R1 验证

首次失败的 warm 导航为 0.469 s，SC4 于 0.490 s 报新增 owned `generation-1-2/payload.pcm`，main prerequisite 约 0.50 s 才完成；warm 日志没有重建记录。上游 read-copy 先创建文件、分块写入，再完成登记；旧 SC4 按 size 排除副本，存在扫描到短副本的竞态。现场没有该文件当时的 size/hash/ledger，不能断言本轮确切原因。

确定性 fake-LSP 在报告 diagnostics busy 时暴露短副本，报告结束时才补齐。旧 runner 同回归 3 个断言失败，报相同 added。SC4 cold/warm 先有界 drain 显式引擎工作，再执行原比较；缺失/超时、真实新 BMI 仍失败，首导航和零重建断言不变。dev/release 各 8 cases / 146 assertions 零失败，真实引擎原 cold/warm 两种 profile 也通过；首导航仍受原 15/5 s 门槛约束。新 release runner SHA 为 `4aa331d189f1a0f759a9ffae878672d6f6f867c30de382bb668b55d2131a8360`，server 字节不变，九阶段 fixture 不走该 SC4 路径。原日志、timing artifact 与反证在 `.payload-cache/59-63-background-completion-pace/`；操作见 [复现第 8 节](reproduce.md#8-sc4-先完成引擎工作再比较缓存)。

SC4 后提交 `9f84df0` 的 PR CI 39711971572 全绿（61 success / 1 expected skip）；Linux、Windows、macOS 原 timing 的 cold/warm 均零失败且 SC4/cacheDrain 通过，首次导航分别 4.603/0.531、8.690/0.525、5.163/0.581 秒。Windows 首次 U17 为 44/24；同头 nightly 为 45/24，均通过，nightly 的失败是另一项 R1，只返回两处项目修改。不能将 PR CI 全绿视为 nightly 全绿。

R1 补修使用同一原始请求的两阶段共享 10 秒预算，校验模型、打开文档版本/正文或闭合文件 stamp；本地取消先终结请求再通知引擎，迟到回调不能推进。Location/LocationLink 的路径和范围均校验。原 R1 断言不变，详细反证与修后结果保留在 `.payload-cache/59-63-rename-ownership/`；见 [复现第 9 节](reproduce.md#9-只读接口符号不能只改项目引用)。本次服务端字节改变，需重新跑原九阶段，前表只作历史检查点。

R1 补修本地 dev/release 全 workspace 均为 6 members / 57 programs / 0 failed；答案路由均 27 cases / 345 assertions。旧产品同测试为 10 failed cases / 151 failed assertions，日志直接保留项目部分 WorkspaceEdit（并非所有计数都是独立缺陷）。真实构建程序完整场景两 profile 均零失败且项目未变；dev 的 31 组 LSP 和实际 VS Code 44 passing / 1 macOS-only pending 亦通过。release LSP 重放与新二进制原九阶段、平台验收继续进行，尚不作为交付完成。新 release server SHA `edd8f16c6096351a2725cd977dcb06e0eb14dee64b0bfeac34f893f0beacee57`，身份及日志见上述新目录。

新提交 `f6ec554` 的 PR/nightly macOS release 均在既有 CPU 测试失败：固定 wall 2 s 后忙进程只增加 0.09/0.08 CPU s，低于原 >0.5 门槛；不能由此确认实际调度量或排除 sampler 缺陷。测试补修先以独立 native CPU oracle 完成 ≥0.75 CPU s，再测原 subject；READY/START/DONE 共用原 30 s 硬期限，原 CPU 断言不变。Linux dev/release 各 23 cases / 91 assertions 通过，native work 约 0.75 s、subject 增量 0.74/0.75 s；sampler 单次既有界限及清理可越过工作期限，迟到仍失败，macOS 原生验证待 CI；见[复现第 10 节](reproduce.md#10-cpu-采样前先确认实际-cpu-工作完成)。另一 macOS conformance 失败发生在 checkout DNS，产品测试尚未启动，首轮日志均保留。

同一新产品的原性能首轮 mcpp warm ready **15.219 s > 12 s**，真实失败；176 已有模块零重建/零首次构建、零错误/重启。producer/model/首次导航很快，慢段位于引擎 prerequisite reuse 检查与扫描。该段采样约 0.343 core；前检查点约 1.353 core，总 CPU 工作近似相当，但缺当时 IO、调度及宿主 pressure 证据，不归因外部负载或认定产品无关。其余阶段继续，不能将旧九阶段通过或后来诊断通过替代本轮失败。

## 合入检查点与发布前发送补修

`38366b9` 的 PR CI 39720450692 为 61 success / 1 expected skip；proot 首次下载 VS Code 超时，单项原样重试后实际 LSP/editor 通过，首日志保留。nightly 39720447763 为 62 success / 4 expected skips，全部八组 UX 的 34 阶段 / 207 检查实际通过。固定 observer 对照四组 cold→firstwarm 全部退出 0，warm 为 3.92/4.06/4.16/3.98 秒，没有证明观察器导致首次超时。

另一次预登记的完整九阶段最终验收为 9 exit 0 / 53 checks / 0 failed，保留原 CPU、pins、producer、cache lineage 和门槛；mcpp warm 3.824 秒，U8 undo 均为 0。原 600 秒 xlings idle 为 0.004819 core、RSS 1258.30→1121.86 MiB，有限工作单列。此前 15.219 秒失败仍未归因，未因新通过改写。该检查点于 `fd869bd` squash 合入；原始材料及小型终态摘要保存在 `.payload-cache/59-63-performance/light-observer-final-acceptance-f6ec554/`。

随后 draft Release 39732244483 的 macOS dev job 119670268987 在原 `test_process.cpp` 发送检查失败：4 MiB 入队应小于 2 秒；CPU oracle 测试通过。旧断言未打印实际耗时，不能给出精确超时值或归因宿主。代码确认 JSON 编码仍在调用线程同步执行；补修将拥有所有权的 JSON 快照交给 writer 编码、写入，保持 FIFO、UTF-8 和两种 framing，并隔离 writer 编码异常。原 4 MiB / 2 秒及 stop 10 秒门槛不变；新产品必须重新验收，不能借用前述产品通过。见[复现第 11 节](reproduce.md#11-发送队列的编码与快照)。

## 新增状态一致性回归与证据边界

`072c230` 的 CI 39735383721 有真实 U15-edits 失败：最忙一分钟 13 次状态转换，超过原 12 次门槛；169 次采样、总 28 次转换、两次配置重启、零 error。其余 cold/warm/typing/graph/config undo/faults 通过不抵消该失败。原 artifact 只有汇总，旋转 server 日志也没有完整 status 轨迹，**不能确定这 13 次的原因**。

独立回归证明同步 apply 的一致性缺陷：真实 clangd 在安装新 primer/安排重启前会调用 `host.status_changed()`，workspace 此时已采用新 profile。相同回调时序的定制 core 在旧产品上仅一个断言失败，暴露了引擎尚未 commit 的新 profile/ready；无回调对照通过。补修只合并同一同步 apply 栈内的状态通知，所有参与引擎完成后发布；不增加延时、不扩展 hold、不隐藏异步状态。修后 dev/release 各 28 cases / 357 assertions 通过，其他验证仍在进行。这一独立缺陷不能冒充首个 13 次失败的归因。

runner 新增紧凑 `stateTransitionTrace` 与 `busiestMinuteWindow`：保存初始状态、重启边界和实际计数的转换，带整数纳秒及采样索引，能按原严格 `<60s` 窗口重算。只保留 issue code，不复制完整 status 或路径；原采样、计数和门槛不变。完整新产品性能链与平台 CI/nightly 尚未完成。见[复现第 12 节](reproduce.md#12-同步-apply-的状态快照与时间线证据)。

## 补修产品最终本地验收与真实 Windows 生产链边界

提交 `1341938` 的 PR CI 39742414940 为 **46 success / 1 条件跳过**。两种 profile 各 6 members / 57 programs、31 组真实 LSP、原完整 build-programs 与实际 VS Code（44 passing / 1 平台专用 pending）均零失败。原九阶段单次完整运行 **9 exit 0 / 53 checks / 0 failed**，fixture、CPU、producer、cache lineage 与预算保持原值；mcpp/xlings warm ready 为 **6.572 / 2.435 秒 <12 秒**，已有模块分别 176/111，均零重建；xlings 另有 39 首次构建。U8 撤销均零重建，U15 edits 最忙一分钟为 4/2 次。

原 600 秒 xlings idle 为 **0.004954 core**，RSS **1108.40→1129.73 MiB（+1.92%）**；之前 62.021 秒有限工作单列，不混入空闲值。独立重算全部状态轨迹和严格 `<60s` 窗口，9 个 observer 均正常结束，303 个观测到的 PID/birth 身份已退出；1 秒采样可能漏掉瞬时子进程。终态摘要在 `.payload-cache/59-63-performance/light-observer-final-acceptance-status-1341938/terminal-summary.json`，原 13/min 与 15.219 秒失败及其未归因边界不变。

范围审计发现：原 `mcpp-target-windows` 是 mock producer + 真实 clangd，它证明目标传递和条件声明，但不能代替设计要求的真实 mcpp 原生双目标生产链。新增 `mcpp-target-windows-real` 使用真实 `mcpp build --configure-only --target` 生成两份完整 CDB，不改写条目；同一会话切换 i686→x64→i686，校验 auto 来源、实际引擎命令、32/64/32 条件声明、导航与诊断。Windows 原生 CI 实际通过前，该项保持待验收；没有把 mock 通过扩大为真实生产链通过。

## 历史失败与反证仍保留

| 记录 | 结论边界 |
|---|---|
| dev SIGSEGV / cold 139 | **未归因、未证明修复。** GDB2/5/7 捕获不同路径；冻结 ELF 与 live text 一致，合法 429,463 B / 深度 7 completion 与不足 100 KiB 栈不支持随意增加栈。PR 前 0.0.16 dev 也异常、系统 GDB 有 SIGILL，均不能排除 PR 或证明硬件原因。见 `diagnostic-sigsegv-summary.json`、`baseline-016-envelope.json`。 |
| warm 17.99 / 22.96 s > 12 s；U5 7.0618 s > 6 s | 真实预算失败，不由后来通过覆盖；原因未确定。dev 与 release 检查点分别记录，早期 xlings 未跑 faults 的链不冒充完整九阶段。 |
| wrapper 143、旧轮次停止 | 143 的发送者未知，保留 measure 与中断记录；确认缺陷后被新提交取代的运行标 superseded，不计通过。 |
| Windows U17 73/24、66/24 | 原 2.0 gate 真失败；同头另一次 46/24 或新产品 47/24 不抹去它。漏记 pace 有独立反证，但不能由一次通过断言所有波动已消失。 |
| sweep / 发布恢复 / untrusted / alias | 旧实现分别有 4 / 5 / 6 / 8 个断言失败，补修回归通过；见 `59-63-sweep-validation`、`59-63-publication-retry`、`59-63-path-identity`、`59-63-member-alias`。 |
| Mac mock / ARM64 / cache injection | 保留旧失败与 artifact。Mac 初始化表/反汇编非 crash report，代码签名页哈希正确非完整信任验证；补修后的原生 conformance 才是平台证明。 |

全部性能历史位于 `.payload-cache/59-63-performance/`。此前产品 `a9483e1` 的 release 九阶段亦通过，但不替代新增后台 pace 产品的上述重放。该检查点 server SHA `730294c58cffd92b694ea72eeb39ccdf57f710a57cdcf5930c0199860651611d`、runner SHA `b3fb89f81ebaf4b83ed89edaa84cc666836e8af8f25f2c9cbe9c3c82e9f67b68`。

<a id="memo"></a>
## 引擎已知局限与交付边界

仅载入 30,298,617 B memo、无打开文件且关闭 background index 的隔离实验，180 s RSS 176.832→266.754 MiB（+50.85%）、零 memo 写入。[0030 patch](https://github.com/Sunrisepeak/mcppls-clangd/blob/92a4c630f4691890e6133c87d3e4591bc5fd8258/patches/0030-UP-35-module-input-sources-and-memos.patch) 每 30 s 先构造 JSON 再检查版本，warm adoption 不增加版本。这证明独立分配问题，不证明持续泄漏或原 U13 必败。此 PR 未修改引擎；未以关 persistence、改 RSS 起点或固定延时冒充修复。

引擎仍为官方 v0.0.16 payload 的 `23.1.0-mcppls.4` / fork `92a4c630f4691890e6133c87d3e4591bc5fd8258`。正式 release 下载、真实 0.0.16→0.0.17 升级、发布后校验均未完成；旧升级 profile 保持原状。最终平台 CI/nightly 与只读 review 完成后才合并发布。

<a id="raw"></a>
## 原始记录如何核对

- [raw-index.tsv](raw-index.tsv) 为逐文件大小与 SHA，不按通过筛选；本地历史快照 `.payload-cache/59-63-evidence-raw/checkpoint-20261021T202025Z-5e19005.tar.gz`，SHA `674b5072ddfb27204aef5bc2de61cf13ad38176f3e5a79c00a5cda382e15a8c0`。
- 快照后的记录独立保存在上述目录、envelope 与 CI artifacts，不混入旧索引。较完整的历史叙述还可从 Git 的 `bcc9cbe` 版简报读取。
- 已结束日志的 `.log.gz` 为无损压缩，`compressed-logs.jsonl` 记录原始字节数/SHA/解压核验；core 保留本地。早期丢失的 `/tmp` 跟踪不冒充完整证据。
- 空间不足只清理本项目过期、无进程引用的构建中间文件；当前冻结、升级 profile、性能缓存、日志及 core 不动。清理范围与保留二进制 SHA 另有 JSON 记录。
