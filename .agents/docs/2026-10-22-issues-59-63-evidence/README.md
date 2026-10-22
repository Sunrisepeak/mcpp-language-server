# #59 / #63 验证简报

日期：2026-10-22 JST。**产品原九阶段已通过；平台验收仍有 SC4 测量失败待修补验证，PR 保持草稿。**

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
| 测试供给 | 修 UTF-8 裁剪与写失败处理、Termux 内层脚本绑定、ARM64 mock 的绝对 compiler 路径、缓存注入使用真实路径。Mac mock 全局字符串早于 allocator 初始化，已移到 main；原生平台证明通过。 |

<a id="validation"></a>
## 最新产品证据

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

## 当前 SC4 测量修补

首次失败的 warm 导航为 0.469 s，SC4 于 0.490 s 报新增 owned `generation-1-2/payload.pcm`，main prerequisite 约 0.50 s 才完成；warm 日志没有重建记录。上游 read-copy 先创建文件、分块写入，再完成登记；旧 SC4 按 size 排除副本，存在扫描到短副本的竞态。现场没有该文件当时的 size/hash/ledger，不能断言本轮确切原因。

确定性 fake-LSP 在报告 diagnostics busy 时暴露短副本，报告结束时才补齐。旧 runner 同回归 3 个断言失败，报相同 added。SC4 cold/warm 先有界 drain 显式引擎工作，再执行原比较；缺失/超时、真实新 BMI 仍失败，首导航和零重建断言不变。dev/release 各 8 cases / 146 assertions 零失败，真实引擎原 cold/warm 两种 profile 也通过；首导航仍受原 15/5 s 门槛约束。新 release runner SHA 为 `4aa331d189f1a0f759a9ffae878672d6f6f867c30de382bb668b55d2131a8360`，server 字节不变，九阶段 fixture 不走该 SC4 路径。原日志、timing artifact 与反证在 `.payload-cache/59-63-background-completion-pace/`；操作见 [复现第 8 节](reproduce.md#8-sc4-先完成引擎工作再比较缓存)。

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
