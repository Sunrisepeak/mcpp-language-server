# PR65 可获取的交接证据

此前交接评论仅引用原机器的未提交源码和 `.payload-cache`，其他机器无法获取。本次将修复、fixture、四份引擎提案提交到 PR 分支，并将精选原证据存入约 1 MB 的 `evidence.tar.gz`。`manifest.json` 逐项记录相对路径、字节数和 SHA-256；`archive.json` 记录整个包的 SHA-256。

在 PR checkout 根目录运行（仅依赖 Python 3.9+）：

```bash
python3 .agents/docs/2026-10-22-issues-59-63-evidence/portable-handoff/verify.py
python3 .agents/docs/2026-10-22-issues-59-63-evidence/portable-handoff/verify.py --extract --into .
```

第二条恢复精选证据到原相对路径；遇到已有且内容不同的文件会拒绝覆盖。也可先解到另一个空目录，避免混入当前调查。

包含：两 profile 全 workspace 与 32 组 LSP 的实际终态/日志、新增 70 项 fixture 的结果、旧产品重复 emit 反例、四项首次 nightly 性能失败、Windows/Termux/Zed 首次失败与原始 LSP、trace runner 对照、U7a 四阶段 measure/observer/provenance、崩溃栈及 parser context、符号化结果、memo 控制与原发布补丁、测试前源码快照。

不包含：ELF、PCM/对象/index 缓存、完整项目工作副本、30 MB 的生成 memo seed 和全部 verbose server 日志。摘要保留首次失败，原始数据不足的归因仍标未知。包是交接证据，不是可直接复用的完整性能运行环境；复现需按上一级 `reproduce.md` 重建工具及独立 workspace/cache。

两个 `state.json` 是归档时的历史快照，可能仍有 live/pending 字段；以实际 `workspace-tests.json`、`fixture-runs.json`、`fixtures-{profile}/summary.json` 和本次 `HANDOFF.md` 为准。旧路径中的绝对机器路径只用于原始 provenance，不要求新机器拥有同名目录。
