# mcppls 0.0.12：维护版 clangd 接入方案与本地验收

> **历史记录。** 0.0.12 的最终范围、决策与实施见 [Part 3](2026-10-09-0.0.12-part3-convergence-plan.md) §8。文中 `.agents/docs/2026-10-08-0.0.12-part2-evidence/`、`.agents/docs/assets/`、`tests/evidence/` 等原始证据路径位于 `mcpp-language-server-0.0.12-evidence.tar.zst`（[evidence-0.0.12](https://github.com/Sunrisepeak/mcpp-language-server/releases/tag/evidence-0.0.12)），完整增量历史在 `archive/0.0.12-joint` 分支。

状态：供 review；本地候选已打包并在真实 VS Code 验证，**当前 fork 不满足默认发布门槛**。
日期：2026-10-07（JST）。范围是分析、测试包与方案；正式 lock、生产行为和发布流程尚未切换。

联合 release 范围与执行门槛已扩展到 [mcppls 0.0.12 + mcppls-clangd 联合方案](2026-10-07-0.0.12-joint-release-plan.md)：覆盖 #24/#37/#43/#44、真实根因修复、性能与正确性、Windows crash corpus、四平台分发以及逐项 workaround 退休。本文件保留原始分析和测量；第 1/5/6 节的初步接入建议以联合方案的 G0–G4 为准。**仅关闭不安全 fast path 并恢复原版速度不再视为满足 0.0.12 优化目标。**

## 1. 结论与 review 决策

独立仓库的维护模型可行：固定 LLVM 基线，携带小型 patch series 和 overlay，四平台产物交给 mcppls 的 payload 流程。但当前实现没有覆盖 #24 的主要稳定性问题，而且补全加速引入了真实语义回归。建议下个版本接入维护版引擎的身份、分发与验收机制，**先修补全门控和模块编辑后的候选更新，再决定默认启用**；不要直接把当前七个 patch 全量视为已修复并删除 mcppls 的保护。

需要 review 的四个决定：

1. 采用维护版 clangd；正式切换以本文件的发布门槛为准。当前包只作本地试验。
2. 首先收紧或关闭 0006 的索引快速路径。成员、指针成员、类型作用域、字符串/注释、未知模块或索引不完整均须回到 Sema；正确性优先于 100 ms 指标。
3. 分离 `llvm-base-version` 与完整 `engine-version`，保留已有 23.1.0 kit。payload/report 必须能看到 `23.1.0-mcppls.N`、fork commit 和二进制 SHA-256。
4. 保留死锁、异常退出、autosave、缓存与后台索引保护；每项 workaround 根据实测 canary 单独退休，支持用户指定的原版 clangd。

## 2. 本次检查的身份与证据

| 对象 | 本次固定值 |
|---|---|
| 工作区原始 checkout | mcppls `6893a4e`，0.0.10；未在此处升级版本 |
| 测试服务器源码基线 | 当前远端 main `a0cd29b735971d56042e91c6162ae373bcbb193b`，0.0.11 |
| 测试候选 | 在隔离本地 clone 中用 `mcppls-devtools version --set 0.0.12` 生成版本，再 release 构建；**仅版本变化，无额外 0.0.12 产品功能** |
| fork checkout / 远端 main | `2724791464ff826454cb7c3a9946d2097a4fd259` |
| LLVM 基线 | `llvmorg-23.1.0`；`UPSTREAM` 当前是 tag，需要补解析后的 commit SHA |
| fork binary identity | `clangd version 23.1.0-mcppls.0` |
| VS Code | 1.132.0，Visual Studio Code，desktop，linux-x64 |
| 真实项目 | `/home/speak/test/mcpp/qt-demo`，Qt 6.11.1，gcc/libstdc++ 16.1.0，`import std; import nlohmann.json;` |
| 测试包 | `/tmp/mcppls-0012-fork-review-hCVnxE/mcppls-0.0.12-fork-linux-x64.vsix`，约 36.17 MB（vsce 报告） |
| VSIX SHA-256 | `f612d0a224c1b8eb8d6e7da70529fc13ba84befd86d94dd85172705051a685d8` |
| 打包 clangd SHA-256 | `a3855155d2e2a6746d07805153a59f7c352b8b768ff7401f9cd4aed41c9bd209` |
| 打包 mcppls SHA-256 | `28f1543eecbc0897f2d7d4c1621d65c2eee848b42411d56b2c6ca1adbb4491b7` |

测试包通过现有 payload assemble/verify 和 vsce 打包，随后实际安装到独立 extensions-dir；VS Code 中运行真实扩展和它的 bundled server/clangd，不是 mock LSP。qt-demo 的修改只通过 VS Code buffer / LSP 传递，probe 最后复原，文件内容 hash 前后相同。候选最终报告 core clangd 与 mcppls 都 ready。

原始输出与可复跑脚本在 [assets/2026-10-07-clangd-review](assets/2026-10-07-clangd-review)。`results/vscode-fork-server.log` 记录了使用已安装候选 payload 的命令路径；VS Code JSON 内记录二进制 hash。临时包和内存盘缓存只保证当前机器、当前会话可用。

外部依据：[缺陷登记 #24](https://github.com/Sunrisepeak/mcpp-language-server/issues/24)、[fork PR #1](https://github.com/Sunrisepeak/mcppls-clangd/pull/1)。完整 comments 快照保存在 `results/issue-24-comments.json`。本次未修改线上 issue 或发布 release。

## 3. #24 覆盖矩阵

七个 patch 映射六个 UP 编号，其中 UP-25 两个 patch，UP-13 是取证设施。不能按 patch 数计算已修复问题数。ledger 当前没有 `steady`：0001–0004、0006 为 `stabilizing`，0005、0007 为 `draft`。

| UP | 当前 fork 的实际覆盖 | 下个版本处理 |
|---|---|---|
| 01 半成品 `import a.` 挂起 | 无 patch；本次 canary 仍挂起超过 10 s | 保留 WA-001 与磁盘隔离；高优先级回补确认过的上游修复 |
| 02 unresolved import 死锁 | 无 patch | 保留 WA-002/stand-in；增加真实复现后修上游 |
| 03 prerequisite 串行构建 | 无 patch | 保留 WA-003；独立衡量冷启动 |
| 04 查 provider 扫整库 | 无直接修复；0002 是 BMI freshness memo，不能算 provider lookup 修复 | 保留 WA-004 |
| 05 MSVC `align_val_t` 歧义 | 0001 回补 upstream #218152；Linux 上游回归测试通过 | Windows/MSVC STL 验收后对维护版单独停用 WA-005；原版继续支持 |
| 06 单文件不再回答 | 无 patch | 保留 quarantine |
| 07 快速编辑后无 CPU 卡死 | 无 patch | 保留 StuckWatch |
| 08 后台索引不准备 import，声明/定义分裂 | 无 patch；0006 更依赖索引，放大缺口 | 保留 WA-008；与 UP-17 合并研究 |
| 09 semanticTokens/range 缺失 | 0004 实现方法和 capability；按行裁剪，比补丁声明的 start-within 范围更宽 | 明确扩大范围策略并补边界/部分重叠测试；观察到范围外token本身不是LSP违规 |
| 10 unprovided `export import` 不结束 | 无 patch | 保留已知限制与超时隔离；列为稳定性补丁优先项 |
| 11 dependency scan 被 link 错误打断 | 无 patch；mcppls 已用 `-c` 处理 | 保留现有正确命令生成 |
| 12 Windows unresolved-import AST crash | 无根因 patch | 保留 containment；用符号和最小 corpus 定位 |
| 13 Windows 正确命令仍 AST crash | 0007 添加 opt-in minidump；不是 crash 修复 | 不宣称关闭；配置采集与符号匹配后再修根因 |
| 14 未保存 import 不构建 | 无 patch | 保留 WA-007；本次 autosave fixture 仍看到信息级提示 |
| 15 缺分号诊断落下一行 | 无 patch | 保留 WA-006；本次 autosave fixture 通过 |
| 16 contracts / reflection | 未实现 | 继续注明上游语言能力限制 |
| 17 已打开文件函数漏索引 | 无 patch | 不因索引快速补全而忽略；准备 reduction |
| 18 死进程残留模块锁 | 0003 Windows 本机 PID 探测，默认等 60 s；Linux 用 LLVM 原有逻辑 | 保留 mcppls cache lease/锁处理；Windows kill/restart/live-owner/PID reuse 分别验收 |
| 19 command hash 目录不回收 | 无 patch | 保留两代目录与 prune |
| 20 fan-out save crash loop | 无 crash 根因 patch；0007可协助 Windows 取证，不能解决已观测 Linux crash | 保留 backoff、bundle、重启控制 |
| 21 didClose 后 worker 空转 | 无 patch | 保留 K-7/K-8 |
| 22 ranges 错误 const 建议 | 0005 对所有传入 `operator|` 的变量宽泛抑制；canary 从 [4,5,6] 变 [5,6] | 部分覆盖；仍保留 WA-010，补 const 正例和四种非 const view 反例 |
| 23 非模块重头文件每次 rescan | 无根因 patch；0002/0006 的收益不能证明这一项关闭 | 保留 WA-009；本次重头文件 canary 存在 include 错误，其倍率不作为有效性能验收 |
| 24 崩溃留下 copy-on-read BMI | 无清理修复；0002记录 published BMI 身份不等于回收文件 | 保留 WA-011、4/16 GiB budget 与 sweep |
| 25 每次模块补全约 1 s | 0002减少 freshness validation；0006用索引跳过 Sema，快但有语义回归 | 不宣称完整根治；先保证 fallback，再做速度/质量双门槛 |

UP-M1…M7 与 UP-P1/P2 属于 mcpp、openkal/PRoot，不应计入 clangd fork 的修复覆盖。

## 4. 实测结果与发布阻断

### 4.1 clangd 直接请求：快了，但丢失语义

同一真实文件、同一 mcppls-generated CDB，buffer 内在 `cli.process(app);` 后插入补全点。每个 prefix 请求 3 次，下表是中位数与单轮 item 数；绕过 mcppls 的 answer cache。

| prefix | 原版 23.1.0 | fork | 结果质量 |
|---|---:|---:|---|
| `nlohmann::j` | 1007 ms，22 items | 69 ms，12 items | 两侧有 json；数量/内容改变，尚无候选覆盖等价证明 |
| `std::ve` | 1143 ms，65 items | 68 ms，2 items | fork 有 vector，但混入全局 Qt 的 qVersion，缺构造函数候选 |
| `cli.` | 1035 ms，32 items | 78 ms，100 items | 原版含 addHelpOption/addOption 等成员；fork 返回 Counter/std::ranges 等全局候选 |
| `cli.ad` | 1239 ms，7 items | 75 ms，100 items | fork 返回 AdlTester/ADJ_ESTERROR 等，不含预期成员 |

另一个沿用 fork 开发时 qt probe 的 16 轮 alternating-prefix 回放：fork p50 71.9 ms / p95 77.3 ms；vanilla p50 1073.6 ms / p95 3879.6 ms。vanilla 前数轮受后台索引/并行测试影响，后 8 轮约 988–1074 ms；因此不用该 p95 推导稳定态倍数或严格的首次就绪时间。`cold_ms` 只是首个请求延迟，不是工作区冷启动全程。

### 4.2 真实 VS Code：产品表现与缓存

每个 prefix 10 轮 buffer body edit + VS Code `executeCompletionItemProvider`；p50/p95 取后 9 轮，不包含第一轮。原版使用已安装 0.0.11，同基线的候选只生成了 0.0.12 版本与替换的 clangd。

| prefix | 0.0.11 + 原版 p50 / p95 | 0.0.12 test + fork p50 / p95 | 最后一轮候选数 原版 / fork |
|---|---:|---:|---:|
| `nlohmann::j` | 2.4 / 11.8 ms | 74.1 / 75.2 ms | 22 / 12 |
| `std::ve` | 3.7 / 4.6 ms | 72.6 / 81.5 ms | 65 / 2 |
| `cli.` | 1001.6 / 1021.7 ms | 77.6 / 94.6 ms | 32 / 100 |
| `cli.ad` | 2.0 / 2.7 ms | 75.5 / 82.6 ms | 32 / 100 |

这里的数毫秒来自产品 answer cache / prefix reuse，不能读成 vanilla clangd 比 fork 快几十倍。原版前三类首个请求约 1003–1005 ms，前两类首次还出现了 fallback 的 lexical items；fork 候选持续落在约 70–95 ms，但成员候选错误与直接请求一致。`cli.ad` 原版可复用 `cli.` 的成员集合，由编辑器进一步筛选，故与 clangd 直接请求的 7 items 不矛盾。

这组测试是 VS Code 扩展宿主驱动真实 provider，不是逐键打开建议框、人工点击 acceptance 的交互测试。下一轮还需实际键入 `j → js`、`. → a → ad`、选中补全项并检查插入内容/resolve/snippet；不能用返回非空代替用户可用性。

### 4.3 根因与现有门槛的缺口

- 0006 的条件只判断 `Prefix.Name`、`Qualifier`、空 prefix 等，并未可靠排除 `.`、`->`。它的 `CurrentLine = Content.rsplit('\n').second` 取的是整个文件最后一行，不是光标所在行。qt-demo 末尾空行使空 prefix 也进入快速路径。
- 有 `Preamble->RequiredModules` 就启用，没有落实设计里的 std-only、索引完整性或未命中回退。`std::` 的限定也只是文本猜测，不能替代 Sema 可见性。
- 0004 的 range 从 `int before = 0; int inside = 1; int after = 2;` 请求 `[0:20, 0:34)`，返回 `[0,4,6,…,0,16,6,…]`，包含位于 char 4 的 before。与补丁声明的精确 start-within 切片不符；但 [LSP 3.17 semanticTokens](https://github.com/microsoft/language-server-protocol/blob/main/_specifications/lsp/3.17/language/semanticTokens.md) 允许返回更宽且完整正确的范围，也建议保留部分重叠token。因此这项是策略/验收缺口，**本次未证明协议错误，不作为成员补全同等级的发布阻断**。需要 start/end、空范围、UTF-16、部分重叠、同一行和跨行测试，避免简单丢掉 start 以前的token又引入新问题。
- `inferred` 的 C7在模块接口新增导出 `greet2` 后等待180 s仍只有原有/lexical候选。保持**同一个0.0.12测试服务器**，只换回原版payload的clangd/kit，对照C7在2.5 s通过，整套7.2 s、0 failures。这是模块编辑后的质量回归，不能由常规 hover/definition 和首次 completion 成功覆盖；本次未进一步隔离0002与0006各自对该回归的贡献。
- `ci/ci_build.sh` 的 lit-subset 列了 6 个老测试，未跑新增 range/tidy 专项；0006没有专用语义质量回归测试。
- `tests/e2e/compare.py` 把“answered but no module symbols”当通过；这不足以验证模块导出符号。速度门槛也不能允许用错误全局候选替换成员答案。
- ledger 写“只有 steady 可以 release”，但 release workflow 只依赖 package，没有执行该状态门槛或依赖质量/soak/VSIX验收。当前全系列均不是 steady。

### 4.4 其他验收

| 验收 | 结果与含义 |
|---|---|
| `cmake --build … --target clangd -j 4` | 退出 0；当前本地源码对应的构建可用 |
| patch ledger validator | 7 patches / 7 rows，通过；不代表产品正确 |
| 定向 LLVM lit | validation cache、stale lock、range、modules、module_dependencies、modules_no_cdb、pr218152，7/7；range 现有测试漏起始列 |
| installed VSIX 的 VS Code modules + semanticTokens suites | 12/12，0 failures；工作区 hash 未改变 |
| fork kill/restart 小循环 | 9 cycles：6 个 clean reply，3 个 kill；退出 0；不是 9 个回复，也没有验收 copy leak/长期 fan-out |
| `tidy-const-views` 产品 fixture | 0 failures，2.2 s；依赖保留的 WA-010，不能证明 fork 根因完全修复 |
| `typing-autosave` 产品 fixture | 0 failures，37.0 s；包含半成品导入、恢复、诊断定位、未保存 import；依赖 mcppls 原有 containment |
| `workaround-canaries` | 1 failure：WA-010 出现 [5,6]，说明行为改变但未完全恢复正确；WA-001 仍挂起。WA-009 的 4.97×伴随 missing header，性能结论无效 |
| `inferred` 产品 fixture | 1 failure，185.9 s：C7新增模块导出 `greet2` 后completion仍不含它；其余检查通过 |
| `inferred` 原版对照 | 同一个0.0.12服务器 + 已安装0.0.11的原版payload：0 failures，7.2 s，C7用时2.5 s |

本机曾空间耗尽，只删除了本轮隔离 clone 的 obj/pcm 中间文件后继续；保留测试包、源码和结果。VS Code 有系统 inotify/EMFILE 警告，不能把本次结果扩展为完整磁盘 watcher 验收。没有修改系统限额、停止用户进程或替换用户默认扩展。

## 5. 下个版本接入设计

### 5.1 两条版本轴和可追溯身份

现有 `packaging/payload.lock.json` 的 `clangd-version` 同时驱动 manifest 与 libc++ kit 匹配。直接改成 `23.1.0-mcppls.0` 会违反 kit 的完整字符串匹配；继续填 `23.1.0` 又隐藏真实 fork。这次测试正暴露这个缺口：hash 是 fork，VS Code 状态和 payload 仍报 23.1.0。

建议扩展 lock/manifest 的引擎元数据，不给 libc++ 虚构一个带 fork suffix 的版本：

```json
{
  "llvm-base-version": "23.1.0",
  "engine-version": "23.1.0-mcppls.1",
  "engine-source": {
    "repository": "Sunrisepeak/mcppls-clangd",
    "commit": "<reviewed immutable commit>",
    "upstream-commit": "<resolved llvmorg-23.1.0 commit>",
    "patch-series-sha256": "<digest>",
    "features": ["module-validation-cache", "stale-lock-recovery"]
  }
}
```

`.1` 是修正后候选命名的建议，不是已经存在的发布身份。旧字段兼容迁移，完整 engine version 用于 report/diagnostics，LLVM base 用于 resource dir、kit 兼容；kit 本身仍是 libc++ 23.1.0。厂商后缀不应单独授权删除 workaround，features 还要匹配已验证的发行元数据/hash；用户替换的 clangd 以实际 capability/canary 为准。

需要改动的边界：`modules/pack` lock/trim/assemble/verify、`modules/base` 版本生成检查、`src/engine/payload.cpp` 的 kit 选择、报告和诊断 bundle、S4/schema/fixtures，以及编辑器状态显示。当前 parser 已保留完整 `--version` 后缀；问题主要在 manifest 提前给了旧版本，以及 kit equality。

### 5.2 四个平台与包格式

保留 `linux-x64 / linux-arm64 / darwin-arm64 / win32-x64` 单一平台表，替换每个平台的锁定输入，kit 依赖暂时不动。不能用一个共同 linux/mac artifact key 掩盖实际四个平台。

fork tarball 当前是 `clangd-<version>-<platform>/clangd/{bin,lib}`，现有官方 zip/LLVM tar 输入的 trim 路径假设需要适配。先给四种维护版归档写 trim fixtures，再导入固定 release asset 的 size/SHA-256；禁止使用 latest URL。

fork `package.sh` 当前没放 LLVM LICENSE.TXT，本轮从同基线源码补入才可 assemble。正式归档必须含 LICENSE、headers、artifact provenance 与不包含自身的 SHA256SUMS。当前脚本重定向到包内 SHA256SUMS 再 `find .`，有把该文件自身收入校验表的风险，需要排除并实际 `sha256sum -c`。

本轮本机构建的 ELF interpreter 是 `/home/speak/.xlings/data/xpkgs/xim-x-glibc/2.44.3/lib64/ld-linux-x86-64.so.2`，动态需求最高 GLIBC_2.38 / GLIBCXX_3.4.30。**这个测试包只适用于本机，不可作为通用 Linux release。** fork CI 的 ubuntu-22.04 构建也需检查 PT_INTERP/RPATH/动态依赖，并在最低支持系统验证；不能由本机运行成功推断可移植。

release 四平台构建 snapshot（run [37479646231](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/37479646231)）：linux-x64、linux-arm64、darwin-arm64 package success，win32-x64 仍 in progress。该 snapshot 不是四平台产品验收，也不是 Windows crash 修复证明。

### 5.3 所有分发入口使用同一维护版

- VS Code/CLion/Zed 与完整 payload：由同一锁定输入组装，真正测试“安装的包”，覆盖用户 compiler、semantic-kit、无 SDK 等 profile。
- xlings 当前只分 server 与 kit，clangd 依赖 `llvm-tools`/PATH；仅换 VSIX 不会让 CLI 用户用维护版。建议发布独立 `mcppls-clangd` xpkg（同 release asset/hash），让安装/启动解析其固定路径；不要把用户通用 llvm-tools 替换掉。若决定 CLI 继续原版，release notes 要明确分发差异。
- 选择顺序：用户显式 `--clangd` → payload engine → 明确安装的 mcppls-clangd 路径 → PATH fallback；report 必须显示来源、version、hash、kit。现有显式 override 保持可用。
- 回滚采用额外修正提交，恢复上一组输入并重新打包；测试候选保留方便 A/B。不额外在每个 VSIX 放两份庞大 clangd。

### 5.4 workaround 与稳定性

先保留 WA-001/002/003/004/006/007/008/009/010/011/012，以及 quarantine/StuckWatch/backoff/K-7/K-8。WA-005 与 Windows 死锁清理只能在各自回归与最老支持引擎策略满足后按维护版能力退休。

WA-012 的 adaptive budget 仍用于 Sema fallback。快速路径真的答到客户端以后再观察 budget 分布，不因为 version suffix 把预算保护全部关掉。索引质量门槛必须包括 UP-08/17；单纯 `isIncomplete=false` 的 client filtering 无法补回第一次已经丢失的成员或导出符号。

0007 默认不采集 minidump，mcppls 尚未传 `--crash-dumps-dir`；接入取证还要安排启动参数、与二进制匹配的 PDB、保留数量/字节预算和诊断导出规则。当前使用 full-memory dump，目录和上传应作为诊断功能明确控制；不以“打了这个 patch”宣称 UP-12/13/20 已关闭。

## 6. 执行顺序与发布门槛

| 阶段 | 工作 | 退出条件 |
|---|---|---|
| A：修正 fork 的可用性 | 0006真实光标行、成员/作用域门控、未命中回 Sema、模块编辑后索引/候选更新；0004明确range策略；补专用 lit/quality corpus | qt-demo 两种成员 prefix 的正确候选恢复；std 限定不混入全局 qVersion；新增模块导出立即可补全；range策略测试通过 |
| B：稳定 fork 工件 | 固定 upstream SHA、修 LICENSE/SHA256SUMS、完整版本/provenance、平台归档和最低系统构建 | 四平台 hash/headers/license/动态依赖验证；需要发布的 patch 达到 steady |
| C：mcppls接入 | 版本轴拆分、trim、kit匹配、report与分发解析 | 旧 payload 和用户原版 override 仍能正常启动；维护版身份和 kit 显示正确 |
| D：再次组装0.0.12候选 | 从 review 后的0.0.11基线生成版本，全平台正式方式打包 | unit/release checks、conformance、installed VSIX suites 全部通过，canary变化有逐项解释 |
| E：真实工程门槛 | qt-demo、mcpp、mcppls、xlings、Windows GalTranslPP，实时键入与选择补全；fan-out save、未保存import、close/reopen、kill/restart、缓存增长 | 成员/作用域/导出符号不回归；20+ warm requests 中正确快速路径 p95 <200 ms；Sema fallback按预算正常返回；无崩溃循环/失控缓存 |
| F：默认启用 | 锁定受验候选、报告review与变更记录 | review接受上述证据与各UP仍存的限制后切换默认 |

性能 gate 比较的是**正确回答的请求**：必须验证结果包含预期符号、候选 scope、insertText/snippet、resolve；任何空结果、lexical fallback 或错误全局候选都不能当一次快速成功。原版 p50/p95 与 fork 分别在 core ready、cold start、body typing、prefix reuse、module edit invalidation 五种场景记录。

Windows验收单独包含 MSVC STL、现存 crash corpus、保存高 fan-out接口、dead/live lock owner、UTF-8 路径、PDB/二进制匹配；Linux小循环不能替代。cache验收统计死进程后的 copy-on-read 文件与 command dirs，不只是下一次 completion 有回复。

review 后优先实现 A/B/C；UP-01/02/10 与 UP-24 是后续主要稳定性补丁线，不能被“UP-25 p95已达标”覆盖。根治模块 Sema 补全（BMI completion tables 等）保留为独立研究任务，不把 ASTContext 对象缓存当成可直接实现的跨请求共享。

## 7. 复跑入口

本轮隔离目录保留 `/tmp/mcppls-0012-fork-review-hCVnxE/source`。本机源码、包、fixture与日志均可检视。脚本中的机器路径和VS Code版本是本轮固定值，换环境先调整。

```bash
# 真实 VS Code：安装候选并在 qt-demo 调用真实 provider
node .agents/docs/assets/2026-10-07-clangd-review/run_vscode.cjs \
  /tmp/mcppls-0012-fork-review-hCVnxE fork
# 原版对照：在独立 profile 使用已安装0.0.11的副本
node .agents/docs/assets/2026-10-07-clangd-review/run_vscode.cjs \
  /tmp/mcppls-0012-fork-review-hCVnxE vanilla

# 绕过产品缓存：成员/限定名补全和range边界
python3 .agents/docs/assets/2026-10-07-clangd-review/direct_probe.py \
  /tmp/mcppls-0012-fork-review-hCVnxE/payload/clangd/bin/clangd \
  /home/speak/test/mcpp/qt-demo \
  /home/speak/.cache/mcppls/workspaces/qt-demo-b32b527216152f14/contexts/default/cdb
```

本轮保留线上历史与用户修改；只在 `.agents/docs` 新增分析和证据，正式接入待此方案review。
