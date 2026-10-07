# 本轮本地证据

对应 [维护版clangd接入方案](../../2026-10-07-maintained-clangd-0.0.12-plan.md)。

- `direct_probe.py`：真实qt-demo文件的只读LSP重放；记录每个prefix的latency/labels与range原始回答。
- `run_vscode.cjs` / `vscode_probe.cjs`：启动真实VS Code，候选通过VSIX安装；原版复用已安装扩展的独立副本。只改buffer，最后恢复，不保存真实项目源文件。
- `results/direct-*-quality.json`：绕过mcppls产品缓存的候选质量对照。
- `results/qt-*-direct.json`：旧qt probe的16轮数据；起始轮受索引/并行测试影响，不能将cold字段当工作区就绪时间。
- `results/vscode-*.json` / `.log`：真实VS Code、扩展版本、payload、二进制hash、状态、80个provider请求的候选与时间；启动有EMFILE环境警告。
- `results/vscode-e2e.log`：installed VSIX的modules和semanticTokens测试，12 passing。
- `results/vscode-fork-server.log`：候选从已安装扩展的payload路径运行的服务端记录。
- `results/inferred*`：fork新增导出候选超时；同服务器的原版对照通过。
- `results/canaries*`：WA-001仍挂起、WA-010行为部分变化。重头文件倍率伴随missing header，不用作有效性能证据。
- `results/tidy-const-views*`、`typing-autosave*`：保留mcppls保护时的产品验证。
- `results/soak.log`：6次clean reply + 3次SIGKILL，不是9次有效回复。
- `results/payload.json`：真实打包manifest；engine仍显示23.1.0，完整fork身份缺口在方案中说明。
- `results/issue-24-comments.json`：本次读取的线上登记快照。
- `results/fork-release-jobs.json`：四平台package job状态快照，不是四平台产品测试。

本地VSIX（未发布、未替换用户默认扩展）在
`/tmp/mcppls-0012-fork-review-hCVnxE/mcppls-0.0.12-fork-linux-x64.vsix`。
它来自0.0.11基线，仅通过项目版本工具改为0.0.12，加上fork引擎；不能当作全部0.0.12功能已实现。
本机构建的clangd使用本机xlings glibc interpreter，因此该包只用于本机测试。

本轮还运行了fork构建、ledger检查与7项LLVM lit测试（全部通过）；直接工具输出中的名单与结果整理在方案§4.4，未伪造对应原始log文件。
