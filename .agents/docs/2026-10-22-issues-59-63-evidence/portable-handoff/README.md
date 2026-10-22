# PR65 的交接证据包

约 1 MB 的证据包只保留在 PR 的提交历史里，不进入主分支（0.0.17 发布设计 D5）。它位于提交
`fcb3450e7960d093e93cfb0aca9ae2ad7f007d70` 的本目录：`evidence.tar.gz`（1,002,457 字节，222 个文件，SHA-256
`af07e0600be611c59f8943c492c37743e1ab3640282d35a6b8044a4a7c95f25f`），以及逐文件记录路径、字节数和 SHA-256 的
`manifest.json`、整包记录 `archive.json` 和校验恢复脚本 `verify.py`。

该提交是 PR #65（`refs/pull/65/head`）和 0.0.17 release PR 的祖先，GitHub 在合并后仍保留这两个 ref。获取与校验：

```bash
git fetch origin pull/65/head
git worktree add /tmp/pr65-evidence fcb3450e7960d093e93cfb0aca9ae2ad7f007d70
cd /tmp/pr65-evidence
python3 .agents/docs/2026-10-22-issues-59-63-evidence/portable-handoff/verify.py
python3 .agents/docs/2026-10-22-issues-59-63-evidence/portable-handoff/verify.py --extract --into /tmp/pr65-evidence-files
```

包含：两 profile 全 workspace 与 32 组 LSP 的实际终态和日志、70 项 fixture 的结果、旧产品重复 emit 反例、四项首次
nightly 性能失败、Windows/Termux/Zed 首次失败与原始 LSP、trace runner 对照、U7a 四阶段 measure/observer/provenance、
崩溃栈及 parser context、符号化结果、memo 控制与原发布补丁、测试前源码快照。不含 ELF、PCM/对象/index 缓存、完整
项目工作副本、30 MB 的 memo seed 和全部 verbose server 日志。
