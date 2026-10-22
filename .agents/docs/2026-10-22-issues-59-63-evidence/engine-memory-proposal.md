# 引擎 memo 空闲分配：本地修复提案

状态：供 review、随 PR 同步的交接候选，当前不实施引擎修复。未编译 LLVM、未修改任何引擎工作树，未发引擎评论、PR 或 release。原设计首段限定“只修改 mcppls 仓库”；本提案只是存放在本仓库的评审材料。根任务尚未决定是否跨仓实施；先确认引擎修改范围是本提案的 scope 建议，不是 skill 强制的审批步骤。用户既有指令禁止 GitHub issue 评论及状态变更；即使后续允许修改引擎，也不自动授权登记上游问题或对外发布，相关外发仍须另获明确授权。不能用修改 payload lock 代替尚未完成的引擎修复和验收。

## 已证明的缺陷与尚未证明的归因

精确发布输入为 fork `92a4c630f4691890e6133c87d3e4591bc5fd8258`、`23.1.0-mcppls.4`。原始 `0030` 位于本地 `engine-memory-review/pinned-0030.patch`，其 SHA 与提案 SHA 记录于 [engine-memory-proposal-source.json](engine-memory-proposal-source.json)。

`Store::write()` 先调用 `memos(Verdicts, Scans)`，复制两个缓存的 snapshot、构造完整 JSON，之后才判断版本是否与 `Written*` 相同。因此，每个无变化的 30 秒周期也会构造并销毁整份 JSON；“没有写文件”不等于没有分配。该控制流缺陷可直接由发布源码证明，与任何一次 RSS 曲线是否复现无关。

首次 `1341938` nightly xlings round1 的原 U13 仍为 FAIL：600 秒 CPU `0.0061084758` core，RSS `948.0859375 → 1188.25 MiB`，增长 `25.3314655%`；原 CPU `0.01`、RSS `10%` 门槛不变。已知有限工作 `126.418657918s` 已单列，前后 preparation/index/request 状态一致，记录的 engine activity 为零。原始 measure 是 `.payload-cache/59-63-real-target-integration/first134-nightly-xlings-round1-measures/ux-xlings-1-long.json`。

这些数据**尚不能证明这次 +25.3% 全部来自 memo JSON**：原 artifact 没有分配栈、逐 PID smaps/allocator 归属或序列化期间 heap 证据；当前 activity 计数也不是全引擎分配计数。提案修复一个独立、已知的无效分配路径，不能先承诺修复全部 U13，更不能把之后的 PASS 覆盖首次 FAIL。

## 最小运行时改动

[engine-memory-proposal.patch](engine-memory-proposal.patch) 仅涉及三个发布源码文件，21 行新增、2 行修改：

1. `ModuleValidationCache::changeCount()` 在 `Mutex` 内读取 `Changes`。
2. `ModuleDependencyScanCache::changeCount()` 在 `Mu` 内读取 `Stores`，与现有 `snapshot()` 读取同一计数。
3. 周期 `Store::write()` 在调用 `memos()` 前比较两个 cheap counter。均等于最近**成功发布**的版本则立即返回；有变化仍走现有 snapshot、JSON、临时文件和 rename 流程，并保留现有 snapshot 后的版本判断。

不改变 30 秒周期、JSON 格式、持久化开关、缓存准入、BMI/输入验证、RSS 测量起点、预算或观察频率。计数器采样不遍历 Entries，不复制 manifest，也不构造 JSON。两缓存分别短暂加锁，不同时持有两把锁；无需引入新的跨缓存锁或 Linux 专用 allocator 操作。

当前发布补丁中，validation 的插入/判 stale 递增 `Changes`；scan 的 `Entries.push_front` 与 `++Stores` 位于同一 `Mu` 临界区（已有 `0023` 发布补丁上下文）。实施时仍需审计完整发布树的所有持久化可见变化：这次优化复用原先写入决策的版本语义，不应顺便声称修复未计数的 eviction、adopt 或 eligibility 变化。

## 并发、失败与退出边界

| 时序 | 行为及保证 |
|---|---|
| 无变化 | 至多读取两个计数；直接返回，不调用 `memos()`。 |
| 变化恰好发生在 cheap 检查之后 | 这一 tick 可返回；`Written*` 没有前移，下一 tick 仍可发现变化。不是事件被消费掉。 |
| 构造 JSON 时又发生变化 | 成功 rename 后只保存 `snapshot()` 捕获的两个版本，不能重新读取 live counter 标记为 written。后续变化保持 dirty。 |
| 两个 snapshot 之间有写入 | 沿用原来两个独立 snapshot 的语义，不承诺跨两个缓存的事务原子性；较新的版本会再次被持久化。 |
| create/write/flush/rename 失败 | 沿用原分支，`Written*` 不更新；即便下一轮没有新缓存变化，已有 dirty 版本仍会重试。不能在 cheap 检查时提前更新 written。 |
| 最终 flush | 先 join Store worker，再显式 `write(CheckForChanges=false)`，保留原实现先做 snapshot 的退出路径。周期 fast path 不用于最终 flush。 |

最后一项有必要：发布版 `ClangdMain` 在 `flushModuleMemoStore()` 之后仍注释可能存在 lingering background threads。Store 的 join 只停止持久化 worker，不能假定所有缓存生产者已经停止。若最终 flush 也使用 early-return，会新增“读到旧 counter 后并发产生变化就直接退出”的窗口。因此本提案保留旧最终 snapshot 路径。

也必须说明其上限：若缓存生产者在最终 snapshot **之后**仍产生新条目，旧实现本身就没有全量退出保证。本提案不修改 shutdown 顺序，不应宣称持久化了进程最后一个后台动作。若用户要求这个更强契约，需另审 producer drain → final snapshot 的生命周期排序；不能靠循环到版本稳定、无限等待或改周期补上。

## 原生 canary 与负对照

[engine-memory-native-canary.patch](engine-memory-native-canary.patch) 是独立的拟议测试补丁，尚未编译。它在真正的 `memos()` 入口计数，而非在成功写文件处计数；增加 test-only accessor，以及 **Store worker 已 join 后**才能调用的周期 write 测试入口。没有新的用户配置或 LSP 字段。

`UnchangedMemosDoNotConstructJSON` 使用真实缓存和真实 Store write：

1. 制造一个 dirty validation 条目，启动 Store 后 flush/join，确认构造次数增加一次、文件可读。
2. 无版本变化时调用周期 write 入口十次，要求构造计数完全不增加，磁盘 bytes 相同。旧实现虽然也不重写文件，却会再构造十次 JSON，因此同一 canary 必须在仅含测试补丁的 `.4` 实现上 FAIL。
3. 再次 markFresh，要求下一次周期 write 构造一次并改变持久化文件 bytes；证明 fast path 不把后续 dirty 数据吞掉。
4. 最终 flush 仍构造一次 snapshot，验证有意保留的退出策略。读取磁盘的 MemoryBuffer 在 rename 前销毁，避免 Windows 映射阻止替换。

Store 是进程 singleton，该生命周期 canary 应在独立原生测试进程执行；不能靠等待 30 秒、RSS 阈值或仅比较 mtime 来证明没有 JSON 构造。测试补丁可单独应用于旧实现作负对照；其 poll 入口拒绝在 worker 仍 joinable 时运行，避免测试制造双 writer。

合入前还必须补齐这些有控制屏障和故障注入的原生测试，不能以 sleep 近似并发顺序：

- 在 cheap counter 读取与返回之间发布一次变化：下一次 poll 或强制 flush 包含该版本。
- 在 snapshot 取得版本后、rename 成功前发布变化：written 仅推进至 snapshot 版本，下一次 poll 仍构造并保存新版本。
- 分别注入 create/stream flush/rename 失败：文件保持上一有效版本；不发生新 cache mutation 时下一次 poll 仍重试。
- producer 静止后最后一次变化由 flush 持久化；producer 在 snapshot 后继续变化的场景必须显式表述为旧 shutdown 契约边界，不写成已保证。
- 覆盖 scan 与 validation 两个缓存的 mutation/counter 对应关系、从旧会话 adopt 后的无变化读取，以及 dirty scan 重试；若发现未计数的持久化可见变化，先修正版本协议，不能用 advisory cache 为漏测开脱。

## 验证与发布顺序

以下是跨仓实施范围获确认后的候选流程；当前只保留本地提案，外发及正式发布另按用户授权执行：

1. 在精确发布源树检查补丁上下文，实施最小修复、原生负对照与上述并发/失败测试；仅在本地记录独立 UP 候选及已有输入/persistence 事项关联。用户既有指令禁止 GitHub issue 评论及状态变更，不能因扩大引擎修改范围而自动向 issue #24 登记；如确需外发，另获明确授权。当前没有发出任何登记或评论。
2. 对 payload.lock 已列出的 linux-x64、linux-arm64、darwin-arm64、win32-x64 构建并运行相关原生测试；并发测试在支持的 Linux 工具链做 TSAN。新 canary 不以 Linux-only skip 规避 Windows/macOS。
3. 先比较旧/新引擎的 JSON 构造次数；再在相同项目 pin、producer、CPU、原阶段 lineage 下运行原 U13 600 秒、0.01 core、10% RSS 预算，以及原完整场景。保留所有失败及 allocator/进程归属证据，不关闭 persistence，不延长起始等待，不改 RSS 算法，也不挑一次 PASS 当根因证明。
4. 为候选修复分配下一未使用 engine 版本（例如仅在尚未占用时使用 `23.1.0-mcppls.5`），先完成真实跨平台构建与原生验证，准备本地或经授权的 draft 资产及 engine.json，记录 fork/LLVM commit、features、build/binary identity。此时尚不正式发布引擎，不能重用 `.4` 名称替换资产。
5. mcppls 先 pin 上述候选资产，更新 `packaging/payload.lock.json` 候选中的 engine 版本、四个平台 URL/file/size/SHA256；从候选分发位置独立下载并按 lock 校验，刷新 payload manifest/identity，并逐条复查本地记录的 canary/WA 去留。保持 persistent-module-memos feature，kit 是否变化由实际 ABI/内容决定，不凭推测重发。
6. 使用候选锁构建、验证完整 payload 与编辑器/发布产物，跑 dev/release native、跨平台 CI 和完整原性能验收；这些验证均使用尚未正式发布的候选资产，不依赖先发布引擎才能验证。通过后再按已获批准的发布流程正式发布相同字节的引擎资产，核对正式下载的 SHA/identity 与候选一致，再进行 mcppls 合入及正式 release。若正式 URL 变化则只迁移 lock URL 并复核下载，不重建或替换已验证字节；资产字节变化必须重新验证。更换 engine identity 导致旧 memo 候选不被采纳是兼容性边界，需按新引擎 cold→warm lineage 验证，不能要求复用已不兼容 BMI 来制造零重建。

当前只验证了提案补丁的 unified-diff 格式（`git apply --stat`）。Store 新文件内容从 pinned `0030` 完整重建；两个 header 的 snapshot 上下文来自同一发布补丁。没有在真实 LLVM 工作树执行 `git apply --check`、编译或运行测试，不能把本地提案称为已可发布修复。
