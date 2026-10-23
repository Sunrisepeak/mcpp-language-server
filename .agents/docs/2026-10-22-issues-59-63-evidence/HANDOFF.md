# PR65 同步交接

此前评论里的“本地修复”没有 commit/push，这是交接缺口。本次将其实际提交到 `fix/17-connection-writer-encoding`，并附带[可恢复的精简证据包](portable-handoff/README.md)。接手者不需要访问原机器。

获取：`gh pr checkout 65 --repo Sunrisepeak/mcpp-language-server`；或者在已有仓库中 `git fetch origin fix/17-connection-writer-encoding` 后检查该分支。不要在 `release/0.0.16` 分支上寻找未合入的修复。

## 已同步文件

- `src/orchestrator/workspace.cpp`、`src/project/scan.cpp/.cppm`：成功 producer 回答确认 run-start 源结构，防止条件 import 导致 body-only 保存重复 emit；代数/身份/stamp、失败、inflight、ABA 和未知预处理均保守处理。仅确认扫描 opt-in 保留预处理文本。
- `tests/test_answer_routing.cpp`、`tests/test_scan.cpp`、`conformance/fixtures/mcpp-watched-source-structure/`：对应原生及完整 70 项 LSP 对照。
- `src/server/session.cpp/.cppm`、`tests/test_session.cpp`：拒绝已有普通文件作为 workspace directory，保留原目录回退顺序。
- `editors/zed/tests/{smoke.sh,lsp_frames.py}`、CI workflows：真实目录与 mcppls 自身 clangd 初始化/进程检查、Zed PATH clangd、Termux 官方包源、首失败日志/producer envelope 保留。
- `conformance/fixtures/mcpp-target-windows-real/`：真实 std 依赖修正，原 validator 未放宽。
- `src/bin/conformance.cpp`、`tests/test_conformance.cpp`：有界 completion 请求时间线、最慢支持点及已有前后 report 的覆盖边界。
- 本目录 `engine-memory-{proposal.md,proposal.patch,native-canary.patch,proposal-source.json}`：完整四份本地引擎提案，**未应用到引擎、未编译 canary、未发布或修改 payload lock**。

## 实际验证与限制

修复是在 `e01e1c1` 基础上的 dirty snapshot 验证，包中保留 exact patch、未跟踪 fixture/test 快照、修订和二进制 hash；不能将这些记录冒充新 commit 的远端 CI。

| 验证 | dev | release |
|---|---|---|
| 完整 workspace | 6 members / 58 programs / 0 failed | 6 members / 58 programs / 0 failed |
| 原 31 组 LSP + 新 fixture | 32 组 / 0 failed | 32 组 / 0 failed |
| 新 watched-source fixture | 70 checks / 0 failed | 70 checks / 0 failed |
| 重点原生 | scan 23/146、Workspace 29/399、Session 3/12，均 0 fail | 同左 |

重点 Session 测试最初两处 string/JSON 比较编译失败，已修为显式 `.get<std::string>()`；首次日志和修订均保留。规范校验 316 rules、0 failure、4 项既有 pending。新源版本 CI、真实 Windows 双目标链、新产品完整性能验收、VSIX 实际升级与发布验证仍未完成。

## 未解决的问题

- 原 `1341938` nightly 四项失败仍成立：mcpp U5/U6 慢请求、U15 在 U6 内严格 60 秒窗口 13/min、xlings idle RSS +25.33%、two-core timeout/U7a 超时；不由后来 PASS 抹去。
- 新 trace 四阶段为 25 checks / 1 failure。本次 U5/U6 未复现历史慢请求；U7a 107.247s 来自 setup 阶段 config AST 崩溃后的 120s 隔离。其他七个 importer <6s，恢复后 AST 约 .730s；底层 crash 仍未归因。不要通过缩短冷却或改预算制造通过。
- 引擎 .4 的周期 memo JSON 在版本判断前构造，独立无效分配已证明；尚不能归因全部 RSS 增长。引擎提案只作为候选，不代表已获授权实施。
- 远端旧 `e01e1c1` CI/nightly 仍失败：Windows schema precheck、Termux 镜像 404、Zed 默认模式 API403 及 file-as-folder 产品缺陷；新修订需实际新 CI。

当前要求是同步并交接，PR 继续 draft，**不合并、不发布 0.0.17、不修改其他仓库或 issue 状态**。接手先核验本提交 diff 与证据，再按简报继续调查。发布暂缓；不能因为后来 CI 通过就自动发布。原两个实际 0.0.16 升级 profile 不重置。用户 `.zcodeignore`、`.agents/docs/2026-10.12-implementation-status-tmp` 未纳入提交。
