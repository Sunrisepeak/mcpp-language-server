# #59 / #63 验证简报

日期：2026-10-22 JST。状态：**实现已提交，完整验收仍有失败项；PR 保持草稿。**

本目录只保留结论、复现方法及原始记录索引。45 份原始记录共 26,769 行、约 1.0 MB，已移出 PR；失败、对照和历史检查点均保留。源码方案见 [详细设计](../2026-10-22-issues-59-63-target-and-reliability-design.md)，操作见 [复现说明](reproduce.md)。

**构建配置边界：**下述本地历史性能链使用 dev server/runner。CI/nightly UX 则使用 release server/runner（`ci.yml` 的 payload build→stage 打包→UX 下载链，nightly 复用该 payload）。两者不能混称。复现默认已对齐 release，并补齐 xlings 的 faults 阶段；最终 release 重放尚未完成，不能用它覆盖已有 dev 失败。

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

同一提交的独立 release mcpp 链 cold/warm/edits/faults **全部通过**（160.576 / 310.311 / 517.397 / 234.596 s），warm firstReady 4.136 s，176 个已有模块零重建、零首次构建；edits 撤销实际构建与物理变化均为零。faults 最长真实无活动 6.522 s、无 stalled 样本、无非计划引擎重启。release xlings 全五阶段已启动，尚未结束。身份见 `release-retryfix-envelope.json`，阶段结果为 `mcpp-release-retryfix-two-core-*`。这仍不等于所有平台与完整验收成功。

此前 `dc12357` 的 [nightly 39675199436](https://github.com/Sunrisepeak/mcpp-language-server/actions/runs/39675199436) 中，复用 CI 的 xlings cold/warm/edits/faults 已全通过，下载 `ux-ux-xlings` artifact 的四份 measure 均零失败（119.405 / 310.503 / 685.644 / 176.905 s）；warm 零重建、配置撤销零重建。该 CI 场景不含 long，不能替代 600 s 验收；原始下载在 `.payload-cache/59-63-nightly-dc12357-ux-xlings/`。Linux 第一组 conformance 同时通过；其他平台与整条 nightly 尚未全部结束。

<a id="validation"></a>
## 当前本地补修检查点

路径身份与未信任回退补修使用同一组冻结 release server/runner；它们还需新的原生平台 CI 和完整性能链验证，不能用前一提交的通过替代。

| 检查 | 结果 | 原始记录（`.payload-cache/59-63-path-identity/`） |
|---|---|---|
| Linux dev / release | 各 40 个执行体通过 | tests-final-dev.log / tests-final-release.log |
| native 回归 | engine 67 cases / 693 assertions；target 9 / 92；runner 6 / 65，均零失败 | 同上 |
| untrusted 旧代码对照 | 新 Workspace 回归实际执行，旧 guard 6 个断言失败；补修后通过 | before-untrusted-dev.log |
| 真实 LSP | 23 组目标/producer/polling/untrusted + 显式目标 untrusted，共 24 组通过 | final-fixtures-results/summary.json / untrusted-explicit.json |
| 规范与仓库 | 0 failure，316 规则，4 个历史 pending；仓库检查通过 | specs-final.log / check-all-final.log |
| 原生 Windows | 旧提交暴露 8.3/长名归属错误；产品已统一三处判断，原五个 fixture 保留 | 旧 CI 日志在本目录；新平台矩阵待完成 |

此前 `dc12357` 的实际 VS Code 1.143.0 E2E 为 44 passing / 1 平台预期 pending，扩展单测 137 passing；其 CI Linux payload 下载校验及 21 项真实 clangd Windows-target 检查通过。官方 0.0.16 profile/cache 克隆升级至该 CI VSIX 的预检通过：旧无选择身份模型重新生成，定义导航正常，工作区未改，原旧 profile/cache 保持不变。它们是补修前检查点，正式 release 下载与升级尚待执行。

## 环境与二进制身份

- Linux x86_64 / i9-13900K：mcpp 两逻辑核为 4–5，xlings 为 12–15；逻辑 CPU 不等于物理核心。构建和测试限定 24–31，不跨亲和性宣称加速。
- 本地构建 mcpp 2026.10.21.1 / LLVM 22.1.8；性能 server producer 显式固定 2026.9.30.2，fixture prepare launcher 另外记录。
- 引擎为已校验的官方 v0.0.16 payload，`23.1.0-mcppls.4` / fork `92a4c630f4691890e6133c87d3e4591bc5fd8258`；本期未修改引擎。
- fixture pins：mcpp `d1f1c98fa26d953e084a183a9672fc1e0ce63558`，xlings `84572b0cc8502a16afaa4ae00867aef17f18c2c2`。历史二进制身份留在索引与各运行 envelope，不混用不同提交的记录。

| 当前补修 release 二进制 | SHA-256 |
|---|---|
| server | 432b76d1073dd77b1f4fbdab4fa3c388aede6b4ca69f3b49d0a3f3658bde095a |
| runner | 3283243c33c3b7c9ecff4fd162ab244aaaf3e007063ca589cb8be56c93a5b866 |

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
