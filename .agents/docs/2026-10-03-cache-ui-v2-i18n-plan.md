# mcppls 0.0.10 追加方案：缓存 UI v2 与双语 —— 悬浮卡信息架构重排、QuickPick 枢纽修整、agent 自检任务书

状态：第 3 版（已按真机反馈实现）· 2026-10-03 · 基于 PR #39 分支 `cache-growth-root-fix`（0.0.10 尚未发布）
目标版本：**0.0.10**（并入 PR #39，squash 后仍是一个提交）

本文是对 0.0.10 主方案（`2026-10-02-cache-growth-root-fix-plan.md` v6，下称"主方案"）UI 层的追加优化，
起因是 2026-10-03 对已实现 UI 的真机 review 反馈：**卡片排版乱、枢纽菜单要优化、扩展半英半中**。
主方案的 D9（hover + QuickPick、零 webview）与 D20（codicon + 分组）形态不变，本文只重排**内容层**。

第 1 版 → 第 2 版（按 2026-10-03 真机验证的三轮反馈追加，均已实现）：

- **分行是真问题**：v1/v2 的卡片各行用单个换行连接，markdown 把段内换行折叠成空格，整卡挤成一段
  （v1 "很乱"的一部分根源）。分区间改空行分隔、分区内硬换行（行尾两空格）、表格独立成块。
- **明细自动获取**：表格只在拿到 `cxxModules/cache` 明细时渲染，而明细原先只有打开枢纽才会取——
  悬停落到纯数字回退。现在 StatusController 自己取（30 秒节流，对齐服务端报告缓存），落地即重画。
- **颜色绕过消毒**（修订 UI-7 的"无颜色"）：hover 的 markdown 剥离 style 属性，但 **emoji 方块是彩色
  纯文本**（🟦🟧🟪🟫🟩🟡🟥⬜），消毒器碰不到。条形改为 8 格定长 emoji，一类一色（已发布蓝/副本橙/
  实例紫/垃圾箱棕；预算绿→接近黄→超限红；准备进度绿）；行名即图例，颜色不是唯一信息载体。
- **一张网格**：大数字标题并入表格成为加粗"合计/预算"行，缓存区整体是一张表，列对齐由网格保证；
  粗数字回退渲染同样的形状（合计行 + 预算条），卡片形态不变。
- **命名去歧义**（修订 UI-14）："本地自检"会被读成"工具自动检查"。动作全部动词开头、语义直说：
  卡片 `清理缓存 / 打开日志与报告 / 复制 Agent 提示词`，枢纽 `复制 Agent 排障提示词`，
  命令面板 `Copy the Agent Prompt for Cache Troubleshooting / 复制缓存排障的 Agent 提示词`。

---

第 2 版 → 第 3 版（按真机验证的第二轮反馈追加，均已实现）：

- **回到无色点阵**：emoji 与 SVG 彩色条均被否（emoji 廉价、SVG 引图片过重）。条形回归
  `█░` 12 格定宽等宽码样式，颜色完全不用——档位由状态点形状与百分比列承载。
- **第一行成型**：`模块/单元/描述源` 并入标题行（`● **demo — Ready** · 4 modules · 8 units · inferred`），
  名字按"整行不换行"的预算截断（约 70 列）。
- **布局规则写死**（"各种情况不乱"）：条形永远同宽；四类行永远齐全（零值也在）；缺失的事实
  （无 plan/无 source/无 sweep/无明细）只去掉自己的那一部分，不重排其余；准备进度只在
  preparing 态多一行；粗数字回退与明细版同一表格形状。
- **修"打开目录"**：`revealFileInOS` 的语义是在**父目录**里选中（Linux 即打开父目录），
  所以点"日志与报告"打开的是缓存根的上一级。目录改用 `vscode.env.openExternal(file://)` 直接打开自身。

## 0. 摘要

三件事，全部落在编辑器扩展与服务端文本层，协议只增一个字段：

1. **双语（i18n）**：扩展 UI 英文源 + 简体中文包（`package.nls*` + `l10n/` + `strings.ts`），跟随 VS Code
   显示语言；服务端日志、CLI、提示词保持英文。修复 0.0.10 引入的"命令面板英文、缓存 UI 中文"不一致。
2. **悬浮卡 v2**：三段式信息架构 —— 项目（状态/模块数/索引进度）→ 缓存（表格 + 每行独立条形图）→
   动作（三个最常用按钮）+ 出处（仓库链接，替代脚注）。
3. **QuickPick 枢纽 v2 + 自检任务书**：四组重划、修复"明细下钻永远打不开"的死代码（含结构性根因：
   用图标字符串前缀分发行为）、数据驱动分发；服务端 agent 提示词从"排障信息"升级为"任务书"，
   疑似 bug 时**征得开发者同意后由 agent 自己起草 issue、呈给开发者过目**，日志永不由 agent 上传。

## 1. 已核实的数据事实（设计依据）

| 卡片/枢纽要显示的 | 来源 | 状态 |
|---|---|---|
| 索引进度 `{done,total}` | status 通知 `progress`（preparing 态） | 现成（`statusText.ts:75` 已用） |
| 模块数 / 单元数 | `cxxModules/cache` 的 `plan.units/modules` | 现成 |
| 仓库地址 | `package.json` repository（`issueUrl.ts` 已在用） | 现成 |
| 缓存根 / 日志目录 | `paths.cacheRoot`、`paths.logDirectory` | 现成（`workspace.cpp:2844`） |
| 打包报告目录 | 实际落在 `<cache>/bundles/`（`bundle/writer.cppm:26`） | **字段缺，需服务端补 `bundlesDirectory`** |
| agent / issue 提示词全文 | `prompts.agent/issue`，服务端单一生成处 | 机制现成，内容要重写 |

## 2. 动机（review 发现的三个问题）

- **P-1 半英半中**：`package.json` 26 个命令标题与既有通知全英文；0.0.10 新增缓存 UI（卡片、枢纽、
  设置描述）硬编码中文。英文用户在英文菜单里看到中文卡片。
- **P-2 卡片排版乱**：全散文行 + `·` 连接，数字不对齐、窄卡乱换行；一根四字符总条（▓▒░·）无刻度、
  需记图例；日志目录长路径每次悬停都占行；各信息无层次。
- **P-3 枢纽有死代码且脆弱**：`cacheHubView.ts:99` 以 `label.startsWith('$(chevron-right)')` 进明细
  下钻，但 `hubItems()` 没有任何条目用该图标 —— **"最大模块"明细不可达**；行为分发靠解析图标字符串，
  图标一改行为即静默丢失。另有分组过碎（5 组 14 项）、两个目录入口近似重复、数据行与动作行无区分、
  清理后只改一行顶部数字滞旧。

## 3. 设计目标与非目标

目标：一眼可读（对齐 + 条形 + 层次）、双语一致、最常用操作一步可达、明细可达、agent 提示词自成任务书。

非目标：hover 里上色（VS Code 剥离 style 属性，平台约束）、webview/侧边栏（主方案 D9 已锁零 webview）、
服务端/CLI 本地化、新增任何设置项、协议破坏。

---

## 4. 机制设计

### 4.1 悬浮卡 v2 —— 三段式（UI-2 … UI-7）

```
● **demo-project — 就绪**
  模块 87 · 单元 12 · mcpp
  准备索引 34/120 ▕██████░░░░░░░░░ 28%          ← 仅 preparing 态出现

  **缓存 312 MB / 4 GB · 8%**
  | 构成     |    占用 | 占比 |                |
  |----------|-------:|-----:|----------------|
  | 已发布   | 210 MB |  67% | `█████████░░░░░` |
  | 副本拷贝 |  64 MB |  21% | `███░░░░░░░░░░░` |
  | 实例目录 |  38 MB |  12% | `█▌░░░░░░░░░░░░` |
  | 垃圾箱   |   2 MB |   1% | `▏░░░░░░░░░░░░░` |

  [$(clear-all) 清理缓存] · [$(folder-opened) 日志与报告] · [$(copy) 本地自检]

  [$(github) github.com/sunrisepeak/mcpp-language-server](https://…) [$(copy)]
```

- **UI-2 三段信息架构**：项目 → 缓存 → 动作+出处，每段一个职责；整卡目标 ≤ 12 渲染行。
- **UI-3 项目段**：状态点 `●` 正常 / `◐` 降级 / `○` 超预算或错误（形状承载含义，不依赖颜色）；
  项目名 + 模块数 + 单元数 + 描述源（mcpp/cmake/…）；preparing 态加一行**真实**进度条
  （status 自带 `{done,total}`）；没有数字不编数字（承主方案 D18）。
- **UI-4 缓存段**：markdown 表格四行，占用/占比右对齐；**每行一条独立同字符条**替代原四字符总条
  （不用记图例，长短即大小，`░` 打底给刻度感）；统一条形语言 `█ ▌ ▏ ░`，全部在 code span（等宽）。
  删除：总条图例行、日志目录路径行（进枢纽明细）。
- **UI-5 动作段 = 三个最常用**（短名 + tooltip 全名）：
  `清理缓存` → `mcppls.sweepWorkspaceCache`（现成）；`日志与报告` → reveal 打开**缓存根目录**
  （`logs/` 与 `bundles/` 并排，一次点击两处都在眼前，标签诚实）；`本地自检` → 复制自检提示词。
- **UI-6 出处段**：仓库 https 链接（点击开浏览器）+ `$(copy)` 走新**内部**命令
  `mcppls.copyRepositoryUrl`（不进命令面板）。hover 文本不可选中（平台约束），"可复制"只能靠命令链接。
- **UI-7 颜色不可用，写死为非目标**：hover 的 MarkdownString 被 VS Code 消毒剥离 style；
  可视化手段 = 表格对齐 + 字符条 + 链接内 codicon。

### 4.2 QuickPick 枢纽 v2 —— 四组 + 修复下钻（UI-8 … UI-10）

```
demo-project — 312 MB / 4 GB · 8%
输入以筛选操作…

─ 概览 ─
 $(database)  缓存占用            312 MB · 8% ▏██░░░░     ← 数据行：选中即刷新
 $(chevron-right) 明细：最大模块与目录    5 个 >          ← 修复：显式下钻入口
 $(history)   上次清理            释放 64 MB · 3 分钟前
─ 清理 ─
 $(clear-all) 清理缓存（不重启、不重编）   [eye 预演]      ← 主操作仍在最前
 $(trash)     重置缓存…          会重新编译
─ 诊断 ─
 $(file-zip)  抓取诊断包（含缓存报告）
 $(output)    打开日志
 $(folder-opened) 打开目录…    >  缓存 / 日志 / 诊断包     ← 两个目录入口合并为下钻
─ 反馈 ─
 $(copy)  本地自检提示词 · $(github) 新建 issue · $(repo) 仓库 · $(book) 文档
 $(gear)  打开设置                                     ← 从"开源"组挪入
```

- **UI-8 四组**：`概览 / 清理 / 诊断 / 反馈`（原五组：日志并入诊断、开源更名反馈、设置挪入反馈）；
  目录入口合并为一个下钻，三精确目标由 `paths` 的 `cacheRoot / logDirectory / bundlesDirectory` 支撑。
- **UI-9 修复死下钻 + 数据驱动分发**：显式 `$(chevron-right) 明细` 条目常驻；
  每个 QuickPickItem 与 HubEntry 用 Map/扩展字段绑定，**消灭 `label.startsWith` 分发** ——
  这是"图标一改、行为静默丢失"的结构性根因（本次死代码即其产物）。
- **UI-10 行为细节**：概览数据行与动作行明确区分（前者选中=刷新/下钻，后者执行）；
  `matchOnDescription = true`（可按描述筛选）；清理完成后用新报告**整列表重画**（现只改一行）。

### 4.3 自检提示词 v2 —— agent 任务书（UI-12 … UI-14，服务端文本重写，协议不动）

`prompts.agent`（`cache.cpp` `agent_prompt` 重写）四段结构：

1. **事实区**（服务端填好）：版本/平台/项目路径/缓存数字/预算/上次清理/日志文件路径。
2. **检查清单**（细化检查放这里，不占卡片）：每条 = 只读命令 + "正常长什么样"
   （`mcppls cache report`、`mcppls cache prune --dry-run`、日志尾部扫描、实例租约状态），
   agent 照单跑、能自己判定。
3. **输出契约**：先给开发者一句话结论（正常 / 可释放 X MB / 疑似 bug）+ 证据；
   默认只 dry-run，任何删除须开发者明确同意。
4. **疑似 bug 分支（UI-13，review 修正稿）——同意之后全部由 agent 自己做，开发者只"同意 + 过目"**：

```
agent 发现疑似 bug
  → 问开发者："这看起来是 mcppls 的 bug，要我起草一个 issue 吗？"
  → 开发者同意
  → agent 自己：按提示词内嵌的 issue 模板 + 已核实的版本/环境起草正文
               （标题 / 复现步骤 / 证据摘录 / 期望 vs 实际）
  → agent 自己：把草稿呈给开发者过目（先给人看再发）
  → 开发者认可
  → agent 自己：打开预填 new-issue URL（服务端在提示词里生成，带版本+环境 query；
               正文超 URL 上限时贴在回复里，由开发者粘贴）
  → 日志 zip / 诊断包：agent 只给出本地路径与内容说明，明确不代为上传；
               开发者要附，自己拖进 GitHub 表单。
```

- **UI-12** 即上述四段任务书结构；issue 模板复用 `issue_prompt` 单一来源（自检提示词内嵌引用）。
- **UI-14 命名统一**：`Agent 提示词` → `本地自检提示词`（卡片短名"本地自检"）、
  `issue 提示词` → `issue 草稿提示词`；卡片、枢纽、命令标题三处一致。

### 4.4 双语 i18n（UI-1）

| 层 | 本地化 | 机制 |
|---|---|---|
| 清单（命令标题、设置标题/描述） | en + zh-cn | `package.json` 用 `%key%`，`package.nls.json` / `package.nls.zh-cn.json` |
| 运行时（卡片、枢纽、通知、状态栏） | en + zh-cn | `strings.ts` 包装 `vscode.l10n.t()`，`l10n/bundle.l10n.{json,zh-cn.json}` |
| 服务端日志、CLI、agent/issue 提示词 | **保持英文** | 可 grep、测试断言英文、agent 提示词英文更稳 |
| 文档 | 已有 en+zh | 不动 |

跟随 VS Code 显示语言自动切换，**不新增设置**；测试经 `strings` 模块取文案（locale 无关），
zh 包键齐全性由单测保证。

---

## 5. 代码落点

- 扩展：新 `strings.ts`、`l10n/` 两个 bundle、`package.nls*`；`tooltipCard.ts` 重写（三段/表格/条形）；
  `cacheHub.ts` 四组重构 + 显式明细条目；`cacheHubView.ts` 数据驱动分发 + 清理后重画；
  `commands.ts` 增 `mcppls.copyRepositoryUrl`（内部）、reveal 增 `root` 目标；`status.ts` 卡片接线；
  `package.json` 标题全部 `%key%` 化。
- 服务端：`workspace.cpp` `paths` 增 `bundlesDirectory`（一行）；`cache.cpp` `agent_prompt` /
  `issue_prompt` 重写（单一来源，CLI `--prompt` 同函数受益）。
- 测试：单测（卡片表格行、枢纽结构、strings 键齐全、提示词关键行）；E2E `cacheHub.test.ts` 断言走
  `strings`；conformance `cache-budget` fixture 增 `bundlesDirectory` 存在性断言。
- 规范/文档：S3 §5.7 示例增字段与新规则 id（自 S3-4-31 起）、§5.8 提示词任务书结构；
  10-editors（en+zh）卡片/枢纽截图与文案；design.md 记 i18n 决策；CHANGELOG 0.0.10 条目扩写。

## 6. 测试与证据（"这个改动要证明什么"）

1. **双语一致**：en 与 zh-cn 显示语言下卡片/枢纽全本地化；en 下无残留硬编码中文（单测：键齐全 + E2E 走 strings）。
2. **卡片可读**：en/zh 各 ≤ 12 渲染行；窄项目名 clamp 不破行（单测：长名字截断）。
3. **明细可达**：E2E 点 `$(chevron-right) 明细` 出现"最大模块"列表（回归 P-3 死代码）。
4. **数字即时**：E2E 清理后概览行数字用新报告更新。
5. **提示词任务书**：单测断言四段与 bug 分支关键行存在；总长 < 4 KB（剪贴板无压力）。
6. **协议兼容**：conformance 断言 `bundlesDirectory` 存在；旧客户端忽略新字段（JSON 前向兼容）。
7. CI 全绿（三平台 + conformance + e2e + ux + release checks）；扩展仍零 webview。

## 7. 风险与对策

- **表格在状态栏 tooltip 的渲染**：hover markdown 支持表格，但需在真机确认对齐效果 —— 验证环境
  （独立 profile 的 VS Code）已有，作为实现后的第一项人工检查；不达标则退化为 code 块对齐布局（等宽保底）。
- **zh 文案超状态栏长度预算**：沿用既有 `maxLength` clamp 与 2 字符图标预算，不新增机制。
- **l10n 包漂移**：键齐全性单测在 CI 把关（en 为源，zh 缺键即红）。
- **提示词变长**：检查清单限定只读命令各一条 + 一行预期，总量 < 4 KB。

## 8. 实施计划与提交

T1 i18n 基建（strings/nls/l10n/键齐全测试）→ T2 服务端（bundlesDirectory + 提示词重写，与 T1 并行）→
T3 卡片 v2 → T4 枢纽 v2（含下钻修复）→ T5 接线与新命令 → T6 测试补齐（单测/E2E/conformance fixture）→
T7 规范与文档 → T8 CI 全绿 + 自 review + 重建 VSIX 并更新验证环境。

全部提交进 PR #39 分支；版本保持 0.0.10；squash 后仍是一个提交。

## 9. 验收标准

§6 的七条全部为绿；用户在验证环境里：中/英显示语言各看一遍卡片与枢纽，明细可达，
清理后数字即时，本地自检提示词粘贴给 agent 能按任务书走完"结论 → （可选）征询 → 起草 → 过目"。

## 10. 决定表（review 结论）

| # | 决定 | 状态 |
|---|---|---|
| UI-1 | i18n 范围与机制（扩展双语、服务端英文、无新设置） | 本文定稿 |
| UI-2 | 卡片三段式信息架构 | 本文定稿 |
| UI-3 | 项目段：状态点 + 模块/单元/描述源 + 真实进度行 | 本文定稿 |
| UI-4 | 缓存段：表格四行 + 每行独立条；删总条/图例/日志路径行 | 本文定稿 |
| UI-5 | 动作段三常：清理缓存 / 日志与报告（开缓存根）/ 本地自检 | 本文定稿 |
| UI-6 | 出处段：仓库链接 + copyRepositoryUrl 内部命令，替代脚注 | 本文定稿 |
| UI-7 | hover 无颜色，写死为非目标（平台约束） | 本文定稿 |
| UI-8 | 枢纽四组：概览/清理/诊断/反馈；目录合并下钻 | 本文定稿 |
| UI-9 | 修复死下钻；数据驱动分发，消灭 startsWith | 本文定稿 |
| UI-10 | 数据行/动作行区分；matchOnDescription；清理后整列表重画 | 本文定稿 |
| UI-11 | 服务端仅增 `paths.bundlesDirectory` | 本文定稿 |
| UI-12 | 自检提示词任务书化（四段） | 本文定稿 |
| UI-13 | bug 分支：同意后 agent 自办全流程，日志永不由 agent 上传 | 本文定稿（review 修正稿） |
| UI-14 | 命名统一：本地自检提示词 / issue 草稿提示词 | 本文定稿 |

## 11. 与既有计划的关系

- 主方案 v6 的 D9（hover + QuickPick、零 webview）、D20（codicon + 分组）**不变**；本文是其内容层的
  v2 重排，C-13.2/C-13.3 的卡片与枢纽内容定义以本文为准。
- 服务端三接口（C-13.1：status `cache` 字段、`cxxModules/cache`、`mcppls.sweepCache`）不动，
  仅 `cxxModules/cache` 响应增一个 `paths.bundlesDirectory`。
- "日志不出本机、不自动上传"约束（主方案 D19）在 UI-13 的 agent 流程里再次写死。

## 12. 自我 review（本版做过的检查）

- 下钻死代码已实读代码确认（`cacheHubView.ts:99` vs `cacheHub.ts` 图标集合），根因归到 startsWith 分发。
- hover 两个平台约束（无颜色、文本不可选中）均已写死为设计边界而非待办。
- 无新设置、无协议破坏（只增可选字段）、扩展仍零 webview。
- 数据事实表逐项对过源码行号（statusText.ts / workspace.cpp / bundle writer）；`bundlesDirectory`
  是唯一需要服务端补的数据。
- 提示词长度、表格渲染、zh 状态栏宽度三处风险各有对策与保底。
