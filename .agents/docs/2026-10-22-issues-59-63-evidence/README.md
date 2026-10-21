# #59 / #63 验证简报

日期：2026-10-22 JST。状态：**实现已提交，完整验收仍有失败项；PR 保持草稿。**

本目录只保留结论、复现方法及原始记录索引。45 份原始记录共 26,769 行、约 1.0 MB，已移出 PR；失败、对照和历史检查点均保留。源码方案见 [详细设计](../2026-10-22-issues-59-63-target-and-reliability-design.md)，操作见 [复现说明](reproduce.md)。

## 结论与证据边界

| 项目 | 已证实结论 | 当前验收状态 |
|---|---|---|
| #63 目标选择 | mcpp provider 未传 target 会采用默认架构；新增 auto / project / 显式 triple，统一请求、生效目标和缓存身份 | Linux 目标/身份/polling 22 组通过；原生矩阵继续验证 |
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

`fb1cb51` 对应冻结字节的新链已通过两条 cold/warm/edits：mcpp 178.400/315.871 s（warm firstReady 5.809 s、176 个已有 BMI、零重建），xlings 118.072/315.753 s（warm firstReady 5.604 s、111 个已有 BMI、零重建）。两核 edits 548.177 s、xlings edits 660.035 s 均通过，U8 撤销实际构建与物理变化均为零；此前 900 s 丢调度的 beforeChange drain 已在 6.204 s 正常结束。faults/long 尚未全部结束，不能据此声称完整性能验收通过。

最终自审另补修单次 CDB 发布失败后的恢复：旧实现仅发布 pending 文件，没有完成首份/增量计划的内部状态；现在在停止接收流量时重新应用完整快照，再启动引擎。新增回归不再第二次 apply，旧代码 5 个断言失败，补修 dev/release 各 40 执行体通过（engine 66 cases / 673 assertions）。对照在 `.payload-cache/59-63-publication-retry/`，后续完整性能链仍需对应补修字节。

<a id="validation"></a>
## 当前本地检查点

| 检查 | 结果 | 对应原始记录 |
|---|---|---|
| Linux dev / release | 各 40 个执行体通过 | local-validation.json |
| engine / runner native 回归 | 65 cases / 640 assertions；6 cases / 65 assertions | publication-regression.json |
| 旧驱动对照 | 新增回归中 2 cases、12 assertions 失败；修复版通过 | publication-regression.json |
| 目标、producer、polling | 22 组通过 | local-validation.json |
| 实际 VS Code 1.143.0 | 44 passing、1 项平台专用预期 pending；工作区未改变 | vscode-e2e.md |
| polling 生命周期 | 四次移除根后：旧版遗留 4 线程，修复版回到基线 | polling-lifecycle.json |
| 规范 / 仓库检查 | 0 failure；4 个历史 pending 规则保留 | local-validation.json |

这些结果对应源码检查点 `5e19005`，并非已发布 VSIX 或全平台通过声明。最终 CI/nightly、完整性能重放、最终自审、合入及下载/升级验证尚未完成，进度以 [PR #64](https://github.com/Sunrisepeak/mcpp-language-server/pull/64) 与其 Actions 为准。

implementation sweep 补修检查点：Linux dev/release 均 40 个执行体通过，engine 65 cases / 650 assertions；规范及仓库检查通过。日志位于 `.payload-cache/59-63-sweep-validation/`，新完整链结果使用 `*-sweepfix-*` 文件名。它们不覆盖 `5e19005` 的旧失败记录，也尚不能证明 runner SIGSEGV 已修。

| 补修二进制 | SHA-256 |
|---|---|
| dev server | 82660eb36b7d42918f780697dad5569d32a1ed1482c068b0e2f5ff6b42b178f1 |
| dev runner | f6d2595d4e8558c38b1f7191422b05b568216e290f9ad36a615cb45ce2814702 |
| release server | 824b825acd9fceff1fc685b9f8eb7cd309f336dbc03dfc5163b78743eb08d7db |

## 环境与身份

- Linux x86_64，i9-13900K 混合核心；两逻辑核用 4–5，xlings 用 12–15。逻辑 CPU 与物理核心不同；本机数据不等于 GitHub runner，跨亲和性结果不用于宣称加速。
- 构建 mcpp 2026.10.21.1 / LLVM 22.1.8；server producer 显式指定 2026.9.30.2。fixture prepare 的 launcher 与 server producer 分开记录。
- 引擎使用校验过的 v0.0.16 payload，`23.1.0-mcppls.4`，fork `92a4c630f4691890e6133c87d3e4591bc5fd8258`；本期未修改引擎。
- fixture pins：mcpp `d1f1c98fa26d953e084a183a9672fc1e0ce63558`；xlings `84572b0cc8502a16afaa4ae00867aef17f18c2c2`。

| 二进制 | SHA-256 |
|---|---|
| dev server | a405994b23537a2471c29663d2c0a9295e093565ef2e9769d0f6ffb65b051fac |
| runner | 37d486fe6951a0478910e275be91f5f4d93d269d951f0d8f0f1eb1d841fa4445 |
| release server | 086ec76e66d0e76919281e8a2d0d0203aa5dc6fd2acd0077217fba29f37a9f4c |

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
- 归档是截至该时间的历史快照；后续结果持续记录在简报及 CI artifacts。CI/nightly 已上传各阶段 measure；新复现默认也写入独立输出目录。

原早期 `/tmp` 跟踪曾在环境重建时丢失，不能冒充完整原始日志；已保留的 JSON 与之后持久目录的新证据区分记录。
