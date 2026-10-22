# #59 / #63：跟随构建目标、模型身份与 nightly 可靠性

日期：2026-10-22。状态：**自审后实施中**。本文保留设计边界和验收条件；工作区实现不等于已通过全平台验收，后续结果以实际证据为准。

范围：只修改 mcppls 仓库；不要求修改 mcpp、clangd，不发布 GitHub 评论或变更 issue 状态。本文记录目标接口与验收条件；实施进度见末尾，未完成的平台与性能验收不以代码已存在代替。

## 0. 设计自审与任务依赖

完整目标包含实现、测试、规范与文档、单 PR、全部 CI、最终自审、squash 合入、release 及本地验证，最后提供上传产物目录。允许并行推进；不额外发布 issue 评论或修改 issue 状态。

| 角度 | 自审发现 | 实施约束与证据 |
|---|---|---|
| 架构 | CDB 是选择事实，producer 模型是分析事实，混合会丢失模块语义 | 选择函数放 project，orchestrator 只负责快照/代数，扩展只显示 server 状态 |
| 稳定性 | 只在下一次 load 增 generation，可能短暂采用已经过期的结果 | 输入变化时即标记选择过期，提交模型前重新检查快照；半写短暂保留旧目标但不得标新目标 |
| 优雅简洁 | 直接把所有 CDB 变更放入 fingerprint 会重启循环；缓存另建一套进程发现也会重复逻辑 | 用 mode+requestedTarget 语义身份；producer 协议确认复用现有 provider/toolrun |
| 用户体验 | 失败后保留旧模型是有用回退，但新标签会误导用户 | actual/pending/source 分开，明确未应用；固定配置出错不偷偷换宿主 |
| 兼容性 | mcppls.mcpp 已是字符串，旧 cache 没有 target 选择身份 | 新项放 buildDiscovery；旧 cache 不判 fresh，但允许可说明的有限回退 |
| 跨平台 | command 拆词、Windows 路径、wrapper/response file 会让简单提取误判 | 复用原有拆词，保守拒绝不能证明唯一的输入；native mock fixture 跨平台，不以本机测试替代矩阵 |
| 一致性 | 设置注册、报告/S3、扩展 tooltip、诊断和缓存可能分别看到不同目标 | 一个选择快照贯穿 producer 和模型；注册表生成配置；规范/schema/traceability 一起更新 |
| 无感升级 | 默认 auto 改变旧版本忽略 CDB 的行为，旧 CDB 可能过期 | 明示来源及 project 退出选项；无唯一目标不猜测，不全局删 BMI，切回有效目标可复用 |

依赖关系：

```text
T0 方案自审、基线和分工
 ├─ T1 UTF-8 裁剪/测量输出 ───────────────┐
 ├─ T2 Termux 内层绑定 ─────────────────┤
 ├─ T3 目标解析 + provider 参数 ─┐       │
 ├─ T4 当前 producer/cache 身份 ─┼─ T5 生命周期/设置/状态 ─ T6 fixture/规范/文档
 └─ T7 U13/U15/U8 取证 ─ 对应修复 ──────┤
                                        └─ T8 单 PR/全平台 CI/最终自审/验收记录
                                             └─ squash/main CI/release
                                                  └─ 下载校验/本地验证/上传目录
```

T1/T2、T3、T7 可以独立推进，T4/T5 统一集成，避免同文件并发改写。任何性能修复都需要 T7 证据；不以调高预算满足 T8。发布版本通过 devtools 统一更新；所有验收完成后再发布，Marketplace 上传由用户完成。

## 1. 目标与责任边界

用户在多架构项目中执行 `mcpp build --target i686-windows-msvc` 后，希望编辑器跟随该目标；工作区也应能固定分析目标。mcppls 采用构建事实，不枚举猜测项目支持的平台，不以目录更新时间推断最近构建。

本期的准确承诺是：**默认跟随根 CDB 中明确、唯一的项目目标平台；工作区显式配置可以覆盖；没有可靠信息时交给 mcpp 默认解析。** 根 CDB 反映最近发布的编译配置，不保证是最近一次编译成功的配置。只跟随 target，不宣称恢复完整构建配置。

| 责任 | 归属 |
|---|---|
| 项目默认目标及构建参数语义、针对给定 target 生成数据库 | mcpp；沿用已有能力 |
| 选择分析目标、传递参数、缓存隔离、展示实际状态 | mcppls |
| 固定工作区分析目标，或明确选择使用完整 CDB | 用户配置，由 mcppls 提供入口和文档 |
| BMI 重建或后台资源消耗的底层原因 | 以测量证据归因，不预先转给 clangd |

mcpp provider 优先于 compile-commands provider 的既有顺序保留。读取 CDB 的 target 是为 producer 选择输入，不把两个数据库混合，也不把所有编译参数拼进 producer 的输出。

## 2. 已核查的事实与未决问题

核查基线为 `d3c3a94c284d5053baf080d6871f36edaa458bc3` / 0.0.16；2026-10-21 查询远端 main 为同一提交。当前源码构建成功。上游 mcpp 依据本地源码 `0e79128a`，实际 issue 所用版本的行为仍需集成验证。

| 条目 | 结论 | 本期处理 |
|---|---|---|
| #59-1 Termux 脚本不可见 | 内层 Debian 缺少脚本目录绑定，CI 配置问题 | 修绑定并复跑该环境 |
| #59-2 measure UTF-8 崩溃 | 当前真实 runner 可稳定复现 | 修文本截断和序列化边界 |
| #59-3 U13 idle 资源 | 本地对照也观察到有限后台工作和 RSS 早期上涨；尚未证明持续泄漏或给出根因 | 分离工作期与真实空闲期，保留原始 CPU/RSS 指标，不先改预算 |
| #59-4 两核 gating | 从 0.0.7 起已有 job 级 continue-on-error | 剔除“缺少配置”判断，不新增放行 |
| #59-4 U15 停顿 | 两逻辑核重放证明：整体节点 done 不动时，仍有真实模块完成；存在进度口径遗漏 | 以当前准备任务的真实模块完成更新活动时间；保留原始节点停顿指标 |
| #59-4 U8 撤销后重建 | 干净 importer 对照已复现完成构建数增加；根因尚未确定 | 对比有效命令、缓存归属与产物；不能仅凭物理文件变化判重建 |
| #59-5 producer 身份 | 当前指纹校验传入缓存中的旧身份，存在缺口 | 校验当前身份；增加有区分力的 fixture |
| #63 默认 x64 | 默认 provider 选择符合现设计；缺少跟随/覆盖机制 | 本文 §3–§7 |

本地证据：mock producer 返回 x64、同根 CDB 为 i686 时，默认 source=mcpp / target=x64；限定 providers=[compile-commands] 后 source=compile-commands / target=i686，均无项目问题，后者不运行 producer。这证明发现链路，不是 Windows SafetyHook 端到端验证。

UTF-8 复现：mock LSP hover 返回 159 个 ASCII 字符加“中文”；真实 runner 初始化及检查均通过，随后写 measure 抛 type_error.316，exit 134。证明失败机制，未证明历史 xlings 崩溃来自同一个字段。

producer 切换复现：同一 workspace/cache 从旧 mock 切换新 mock，虽然日志出现 cache-fresh 和 loaded again: unchanged，后台确实执行新 emit（23 ms），about.mcpp 正确更新。因此“70 ms 说明没执行 emit”和“缓存永久不更新”不能作为已证实根因。

## 3. 无需修改 mcpp 的信息来源

### 3.1 根目录 compile_commands.json 为本期自动来源

mcpp 的 `src/build/compile_commands.cppm` 定义：配置目录 CDB 保存该配置的命令，根 CDB 为当前配置的整体副本；切换配置会替换根文件，相同内容不重写；fast path 也会恢复根 CDB。`build`、`test`、`run`、`--configure-only` 均可能发布它。

所以产品文案使用“跟随已有编译数据库的目标”，不使用“最近一次成功构建”。不存在成功记录时不阻止使用已配置但编译失败的工程。

本期只读取已选 mcpp 项目根目录的 `compile_commands.json`，解析其链接指向的内容。不扫描 target/ 中的候选文件，不按 mtime 排序，不把其他 build 目录的 CDB 当最新来源。

根 CDB 没有统一的 producer 所有权标记。即便由外部工具生成，也只能称为“现有 CDB”，不能宣称 mcpp 最近构建记录；固定 `project` 模式可以停用跟随。

### 3.2 不依赖 target/.build_cache

该文件是 mcpp 内部缓存：包含 target/profile/outputDir 等，新写记录位于前部，但 target 可以为空，存在旧格式、workspace selection/group、no-cache 和 test 等覆盖限制。它不是稳定的最新构建查询协议。

本期不解析它作为目标来源，也不要求 mcpp 新增协议。未来若要完整恢复 profile/toolchain/features 或严格判断构建结果，再单独设计正式 provider 接口。

## 4. 配置、优先级与范围

### 4.1 一个设置覆盖三种意图

拟新增 `mcppls.buildDiscovery.mcppTarget`，字符串，默认 `auto`；CLI `--mcpp-target`。通过现有设置注册表生成扩展配置、文档与本地化，遵循现有 CLI 优先级；编辑器作用域按下文明确。

| 值 | 行为 |
|---|---|
| `auto` | CDB 有明确唯一目标则传给 mcpp；没有则不传 --target |
| `project` | 不读取 CDB 选择目标，不传 --target，完全采用 mcpp 默认规则 |
| 目标三元组，例如 `i686-windows-msvc` | 固定传 --target，忽略 CDB 的目标变化 |

名称放在 buildDiscovery 下，因为现有 `mcppls.mcpp` 是可执行文件路径，不能直接兼容地把它改为承载 target 的对象。

示例：

```json
{
  "mcppls.buildDiscovery.mcppTarget": "i686-windows-msvc"
}
```

自动模式优先级：**mcppls 显式目标 > CDB 明确目标 > mcpp 自己的默认规则**。mcpp 默认规则包含项目 `[build].target`、全局目标和宿主回落，mcppls 不复制解析规则。auto 下 CDB 与 manifest 默认目标不同可以是正常的 CLI 覆盖结果，不自动纠正。

项目级配置有两个入口，语义分开：

- 若要改变项目的真实默认构建目标，使用 mcpp 已有 `[build].target`，分析选择 `project`；不新增一份 mcppls 对 mcpp manifest 的解释。
- 若只固定编辑器分析目标，将上述配置提交到 `.vscode/settings.json` 或 `.code-workspace`；CLI/其他客户端用相同 server 选项传入。本期不新增独立 `.mcppls` 配置文件。

当前扩展一个窗口共享一个 server 会话，显式设置采用 window 作用域，作用于该会话中的 mcpp 根；`auto` 则按每个根的 CDB 独立选择。单根项目可直接实现项目级固定；多根工作区若要逐根固定不同目标，需要额外的 root→target 设置协议，作为后续范围，不伪称现有窗口配置已支持。

该设置仅影响已选中的 mcpp provider。显式 S1 database、compile-commands provider、discovery=off、未信任工作区的原有规则保持一致。`buildTool=off` 仍不启动 producer；现有 CDB 已符合所选目标时可以使用，否则显示“未应用”并保留有效旧模型，不能宣称已强制切换或执行过 producer。

### 4.2 profile 与完整命令的边界

本期不从 `-O`、宏、输出路径猜 profile、features 或 toolchain，也不新增 profile 跟随开关。传 target 后由 mcpp 生成其他参数；因此“目标与 CDB 相同”不代表“全部编译参数与 CDB 相同”。

要求完全复用 CDB 时，用户使用已有 providers=[compile-commands]。`mcppls.database` 要求 S1，不能拿它直接指向 CDB；discovery=off 也不是“优先 CDB”。项目默认 profile 已可由 mcpp 配置管理。

## 5. 目标提取：只采用明确事实

在 project 层建立可单测的选择函数，输入配置模式、工作区根、CDB 快照，输出 requestedTarget、来源、原因及证据。函数不执行编译器或 shell。

1. 使用现有 CDB 参数解析能力；优先 arguments，command 使用现有平台命令行拆分规则。禁止执行 command。
2. 以现有源码归属和角色信息确定项目目标侧翻译单元。明确排除 build.mcpp 构建程序、已识别的宿主生成工具、外部依赖和标准库 producer；不能按目录名随意排除正常项目源码。
3. 候选条目读取显式 `--target=…`、`--target …`、`-target …`。不以 `JLX_ARCH_X86` 宏、编译器文件名或输出目录推断。未解析的 response file、wrapper、`-m32/-m64` 等可能改变目标的输入不能忽略后仍判定明确。
4. 每个纳入决策的条目都必须可判定，所有目标一致才输出 unique。缺失、冲突或宿主角色无法可靠区分，输出 unknown/ambiguous；不按第一条或多数投票。
5. 两种 triple 拼写只有存在经过测试的等价规则时才合并。不自行将 i686 替换为 x86_64；不维护一份项目“支持平台”列表。未知但语法合法的 triple 交给 mcpp 验证，参数通过 argv 传递。

首版实现前用 #63 摘录与真实 mcpp 的 Windows CDB 验证上述条目选择能得到 unique。若现有解析/角色信息不足，该情况回落并提示，不能通过放松唯一性标准声称已支持。显式配置不受自动提取能力限制。

缺失 CDB 是正常回落；存在 CDB 但无法提取唯一目标时报告原因，默认采用 mcpp 的项目规则，不能悄悄称为“跟随成功”。非法显式设置报告配置错误，不静默改用宿主。

## 6. producer、缓存与切换生命周期

### 6.1 一次选择贯穿整个模型加载

模型加载开始时捕获不可变的目标选择快照；把同一 requestedTarget 传给项目 provider、producer 命令、缓存键和报告。mcpp 主 executable 和协商得到的替代 executable 使用同一目标，不能协商后丢参数。

目标为空时保留当前命令；非空时追加 `--target <value>`。失败不偷偷重试无目标命令；不支持此参数的旧 producer 明确报告，旧模型只能标为旧模型保留。

producer 回答后区分 requested target 和数据库的 effective target。known alias 可比较；确实不一致或数据库含无法映射的多目标时，不标记本次选择已应用，保留证据并报告问题。最终实际分析目标以模型/引擎命令为准。

### 6.2 #59-5：检查当前身份，而不是旧身份自证

当前 workspace::adopt_cached_model 用 cached->producer / producerVersion 计算 current fingerprint；同进程 ProducerAnswers 只以 executable 路径和 root 记版本。两处一起修。

新模型身份至少包含：provider、workspace、当前 producer 解析路径、文件身份/stamp、协商版本、选择模式、requested target、影响 producer 的显式参数。有效 target 随模型保存。选择来源变化写报告；语义相同的目标和参数无需只因 CDB 字节变化重跑。

producer 路径与版本通过现有 tool environment 和 protocol 查询获取；遵循异步环境解析、toolrun、有界执行、默认离线。身份查询去重，按文件变化失效，不在每个请求启动 --version。xlings launcher 路径未变不等于实际 producer 未变；新会话重新确认协议版本，运行中在环境/可执行文件/已知选择输入变化时重新确认。成功 emit 的 envelope 版本优先于先前能力查询的版本。若 launcher 文件、项目 pin 和所有可观察输入都未变化，其内部全局选择变化无法仅靠文件监听立即检测；下一次实际 emit 或新会话确认，不承诺无事件情况下即时发现。

环境尚未解析或版本未确认时，缓存只能称“待确认”。已经知道目标不同的缓存不得按新目标采用；相同目标、producer 尚待确认的旧模型可沿用现有有界启动回退，但标记可能陈旧。旧 envelope 缺少选择身份的 mcpp 缓存不会被采用；本会话重新生成，升级后可能有一次冷启动。既有按文件内容比较不能覆盖 producer 或目标变化。

缓存只允许匹配当前选择的模型被确认；实际数据库、编译器事实或目标选择改变时，answer cache、completion、诊断及准备状态随模型代数切换失效，复用现有机制。producer 版本改变会重新确认模型并更新版本、问题和报告；若其输出的分析语义完全相同，可沿用既有计划与 BMI，避免只因版本标签变化重启 clangd。BMI 按有效命令和工具链隔离，不全局清缓存；切回旧目标允许复用仍有效的 BMI。

### 6.3 文件变化与竞争

- auto 模式 watch 根 CDB；没有文件时也能观察创建。文件替换、符号链接目标变化走现有动态 watch/轮询兜底。
- CDB 解析与 producer 身份查询在后台执行，LSP 主循环只做轻量状态检查；快照有大小/条目数上限。合并事件，读完整快照，稳定后按“目标选择是否改变”决定重跑；相同目标的格式/mtime/内容变化不独自触发 producer。其他构建输入仍按既有规则处理。
- 半写 JSON 按现有短期重试保留旧模型并标记读取中；稳定无效/冲突则明确回落项目规则并显示原因。删除确认后也回落，不无限保留已失去来源的自动目标。
- 使用 generation：i686 请求后又变为 x64，晚到的 i686 结果不得提交。同一选择去重，每次 producer 完成不反向触发下一次；目标提取不写根 CDB。不支持 emit 的旧 producer 仍沿用 configure-only 兼容路径，由 mcpp 写项目 CDB，并显示已有 producer-writes-project 提示。
- 准备期间保留旧模型的可用答案可以沿用，但状态标注旧目标和待应用目标；不得用新目标标签包装旧诊断。失败时说明仍在使用哪个旧目标。
- 多根工作区在 auto 下各自选择；显式设置遵循 §4 的会话范围。共享实例缓存要校验选择身份；不允许两个不同分析目标的实例互相确认模型。
- 轮询模式必须覆盖首次 watch 注册后、首轮扫描前创建输入的竞态；不能把这类新文件静默当作已有基线。动态 watch 与 polling 两条路径分别验收。工作区关闭后轮询 worker 必须退出，避免移除项目后继续全目录 stat。

## 7. 展示与接口

状态栏/tooltip 显示实际 target 和来源（工作区设置、现有 CDB、mcpp 默认），切换时显示 pending target；选项原始拼写与 effective triple 可以不同。

报告与 cxxModules/status 拟追加选择信息：mode、requestedTarget、source、reason、pending、evidencePath；已有 profile.target 继续表示实际模型目标。原始命令、producerRun、实际模型保持可追溯。路径沿用现有脱敏规则。

VS Code 设置项即可完成显式选择，不在本期新增猜测候选平台的 Quick Pick。现有 context 选择是数据库内部的 set 选择，不冒充跨架构重新规划。

若修改 S3 协议，正文、schema、示例、fixture、traceability 一起更新；Neovim/Zed/agent 读取不到新字段时仍可使用既有 profile.target。所有诊断文案区分“目标选择失败”和“源码编译失败”。

## 8. #59 的确定修复

### R1：UTF-8 和 measure 序列化

conformance 的 hover 等 detail 目前直接按字节 substr，随后严格 JSON dump。增加共用 UTF-8 前缀/尾部裁剪函数，检查所有存入 JSON 的 detail/log tail 边界，保留合法码点；输出预算仍按字节计算，不必引入字素簇库。

measure/evidence 外部文本输出边界允许替换非法 UTF-8，并捕获写入/序列化失败，输出可读错误。测量文件无法写出仍返回非零，不把失败静默变成成功。不能只在 stdout 换字符而让 JSON 继续崩溃；也不全局放宽协议解析。

### R2：Termux 内层挂载

nightly 的 proot-distro login 增加 `/mnt/scripts:/mnt/scripts` 绑定或将脚本放到已有 /work；选前者以减少改动。验证脚本可读且 server 检查实际执行。termux 本身已有 continue-on-error，不改变其 gating。

## 9. #59 性能项：先定位，按证据修

| 任务 | 采集与判断 | 可接受结果 |
|---|---|---|
| P1 U13 | 同窗口记录 clangd CPU/RSS、CLANGD_TRACE、请求来源、priming/后台索引阶段；比较默认与关闭 priming 的对照 | mcppls 重复调度则修去重/节流；一次性有限工作则区分工作期与稳定空闲期；上游问题附最小证据。不得无依据提高预算或宣布泄漏 |
| P2 U15 | 状态时间线关联 producer、模块开始/结束、worker 活动与真实进展 | 区分长任务、无活动卡住和状态遗漏；显示当前任务与耗时，不通过递增 done 或心跳掩盖停顿；必要时新增活动指标，原停顿指标保留 |
| P3 U8 | 保存撤销前后 producer 数据库、引擎 CDB、命令规范化结果、PCM 变化与 clangd 重建原因 | 优先查命令漂移/生成输入重写/GC；正常失效证明后才调整断言。修复目标是复用有效旧产物，不保留无效 BMI |

已有实验提供方向，但不替代最终验收：U15 同次两逻辑核重放中，节点无完成间隔为 123.64 秒，真实引擎无活动间隔为 25.42 秒；据此应补真实活动口径，不能伪造节点完成。U8 在八个干净 importer 打开的隔离场景中，配置切回后实际完成数增加 20，不能再简单归为计数误差；约 304 MB 缓存也不能支持“3 GiB 配额耗尽”的解释。U13 仍观察到早期 RSS 上涨后趋稳，其长期性质与具体分配来源未定；此处不采纳尚未验证的趋势替代门槛。

后续重跑历史原场景与两核限制，记录 payload/源码/mcpp 版本。不同环境的数据不能冒充原问题复现。若确有上游缺陷，再按 #24 规范登记，本轮设计不直接新增上游 workaround。

两核轮次已有 continue-on-error；nightly 应清楚区分单个检查失败、job 结果和 workflow 最终结果，不把放行当作性能修复。

## 10. 实施拆分与验收

| 阶段 | 内容 | 依赖与完成条件 |
|---|---|---|
| A | R1 UTF-8；R2 Termux | 可独立提交；各自复现先失败，修后通过 |
| B | 当前 producer 身份校验、目标身份数据结构 | #59-5 新旧 producer 不同内容可观察，旧缓存不自证；为 C 提供身份基础 |
| C | 设置、CDB target 提取、provider 参数、动态切换、状态与文档 | 基于 B；不改 mcpp，完成下表 conformance 与 Windows 集成 |
| D | P1/P2/P3 性能取证与必要修复 | 与 A–C 可分开推进；每项独立给出定位和修复/预算变更依据 |

实现不要求多 agent 或并行执行。代码仍遵循 C++ modules、toolrun 与默认离线约束。

| 验证 | 必须证明 |
|---|---|
| 目标配置矩阵 | auto 无 CDB、唯一 i686、固定目标覆盖、project 禁用跟随、非法设置、provider/discovery/buildTool 开关 |
| CDB 解析 | Windows/POSIX arguments/command、中文与空格路径、链接、alias、冲突/缺失、宿主条目、response file 和改变目标的未支持参数；不执行数据库命令 |
| 冷/暖启动 | x64 cache + i686 CDB 不采用旧架构；无身份旧 cache 不 fresh；相同目标暖启动不额外重建 |
| 运行中切换 | 创建/改写/删除/半写/冲突、i686→x64→i686、晚到响应、内容相同重写；目标标签与生效模型一致，无 producer 循环 |
| producer 身份 | PATH 选中新 executable；同一路径替换；launcher 版本变化；mock 协议计数和数据库内容不同，既查版本又查 producer 确实执行 |
| 参数与错误 | mock 记录精确 argv，断言 --target；替代 producer 也保留参数；失败不无参数回退；effective target 不符可见 |
| conformance | 上述发现/规划行为通过真实 server LSP fixture，不只测试字符串拼接；mock executable 按现有跨平台方式扩展 |
| answer/BMI | 切换后不返回旧目标答案；切回复用有效 BMI；新目标不会被旧模型“确认” |
| UTF-8 | 前/尾截断边界、emoji/中文/非法外部字节；真实 runner 检查成功且 measure 是合法 JSON，写入失败有明确非零结果 |
| 原生集成 | Windows x64 宿主、i686 与 x64 producer、模块条件声明（含 #63 类 eax 场景）；Linux/macOS 回归；所有 runner 平台包含 arm64 |

实施阶段按变更运行 `mcpp test`（含 release）、相应 conformance、`mcpp run -p devtools -- check all`；涉及规范再运行 schema/traceability validator。原生平台证据未完成前不宣称全部修复。实施阶段的具体结果在末尾及证据目录记录。

## 11. Review 决策点与已知限制

1. 默认 auto 是否采用根 CDB 明确唯一 target，并接受“最近发布配置”而非“最近成功构建”的语义？建议接受；不要通过目录时间、宏或多数投票猜测。
2. 是否采用一个 `buildDiscovery.mcppTarget=auto|project|<triple>` 设置？建议接受；单根项目可提交工作区配置，多根逐根显式覆盖暂不纳入。避免额外布尔开关及现有 mcpp 路径命名冲突。
3. 本期只跟随 target，完整 CDB 由已有 provider 选择完成；profile/toolchain/features 的完整配置选择另议。建议接受。
4. 不依赖 mcpp 内部 .build_cache，不要求上游修改；无法确定目标时明确回落和解释，不投票猜测。建议接受。
5. 目标切换与 producer 身份修复合并验收；UTF-8/Termux 独立修，性能项独立取证，不因 nightly 红色就归因上游。建议接受。

目标不存在、CDB 缺失或混合目标、默认规则变化、外部工具写入 CDB 都有明确回落，但不会做到任意工程零配置。若真实 #63 CDB 无法满足唯一性，显式目标仍是首版可用入口；不能在未验证前承诺自动跟随全部交叉项目。

## 12. 证据索引

- [Issue #59](https://github.com/Sunrisepeak/mcpp-language-server/issues/59)、[Issue #63](https://github.com/Sunrisepeak/mcpp-language-server/issues/63)、[上游登记 #24](https://github.com/Sunrisepeak/mcpp-language-server/issues/24)。本草案未修改这些 issue。
- 本仓库：`src/project/mcpp.cpp`（producer argv / ProducerAnswers）、`src/project/providers.cpp` / `detect.cpp`（优先级）、`src/project/modelcache.cpp` / `src/orchestrator/workspace.cpp`（缓存校验与模型采用）、`src/config/settings.cpp`、`editors/vscode/src/status.ts` / `commands.ts`。
- 本仓库：`src/bin/conformance.cpp`（detail 截断 / measure dump / U13/U15/U8）、`.github/workflows/nightly.yml`（挂载 / gating）、`conformance/fixtures/ux-{mcpp,xlings}/scenario.json`。
- 上游 mcpp 基线 `0e79128a`：`src/build/compile_commands.cppm`（根 CDB 发布语义）；`src/build/execute.cppm`（内部记录及限制）；`src/build/prepare/toolchain.cpp`（默认目标）；`src/cli.cppm`（emit 的 target/profile 参数）。这些是核查事实，不是新增稳定协议承诺。
- 当次复现临时材料位于 `/tmp/mcppls-issue-audit/`；临时目录不作为长期验收依赖，实施时转为仓库内 fixture。此前口头归因以本文 §2 的证据分级为准。

## 13. 实施检查点（最终验收尚未完成）

- 已实现目标配置、自动提取、当前 producer 确认、动态切换及状态展示；补齐旧 CDB 回退校验和冷启动拒绝错误架构的竞态。
- 已修 UTF-8 artifact 输出、Termux 脚本绑定及 polling watch 注册初期漏事件；有界 CDB 读取不依赖事前 stat 维持内存上限。
- 当前 Linux 工作区检查点：dev/release 各 40 个单测执行体通过，22 组目标/身份/polling conformance 通过，规范和仓库检查通过。二进制 hash、范围及原始记录索引见 [验证简报](2026-10-22-issues-59-63-evidence/README.md#validation)。
- 真实 VS Code E2E 通过：44 passing、1 项平台专用 pending；准确 release-profile 二进制 hash 与验证范围见简报。发布 VSIX 的安装/升级仍需单独验证。
- U15 已以真实模块完成活动修复停顿判断。原 xlings cold/warm/edits/long 完整链路曾通过，U13 保留 600 秒、0.01 core、10% RSS 门槛；该次 CPU 为 0.007208 core，RSS 净变化为 -50.28%。另一个无文件/无索引的隔离实验定位到内置引擎未变 memo JSON 的周期性分配，属于已知未修局限；原 gate 通过不能证明该局限消失。详见 [简报的 memo 结论](2026-10-22-issues-59-63-evidence/README.md#memo)。最终重放中的失败也在简报中保留。
- 原 mcpp 四核 cold/warm/edits 已通过；U8 在该链路撤销时新增实际构建为零。两逻辑核的完整 edits/faults 验收仍在进行，不以窄场景替代。不会通过放宽既有预算结束性能验收。
- 尚未完成：全平台 CI、Termux 实际环境、性能最终重放、最终自审、squash 合入、release 与下载/升级验证。单 PR 将保持草稿直到验收完整。

后续自审补充：target-only 设置更新仅调度 mcpp 根，不重跑其他 provider；无关 provider 的模型比较不纳入 mcpp 选择身份。真实 LSP 回归在修前观察到额外 model-unchanged，修后不再出现。轮询生命周期对照：四次移除根目录，修前遗留四条线程，修后回到相同线程基线。对应原始记录由简报的索引定位，不在 PR 内展开 JSON。

只读交叉自审另发现 clang-cl 探测遗漏目标参数，会把正确 i686/ARM64 命令读成宿主 x64 并被新校验拒收。已补目标参数传递及有区分力的 host/target 单测，保留 cl.exe 原行为；补修后 Linux dev/release 各 40 个执行体通过，仍需最终提交的原生 CI 验收。

U8 两逻辑核完整重放随后确实未通过：undo 窗口有 28 次实际构建。原 release key 全部保留，最终 release 引擎也能复用它们。进一步 PCM 元数据与进程时序证明：旧 dev 引擎退出前读到了原地更新的 release CDB，产生 dev 依赖闭包与 release 命令混合的新 key。这里同时存在实验与驱动缺陷：runner 的 ready 判断在 dev 准备 `done=0 / wanted=140` 时已允许撤销；驱动在旧进程未完全退出时发布新 CDB。

补修任务：runner 在改变配置前、撤销前及撤销后检查明确的有限工作状态并有界等待，不用 CPU/RSS 或固定延时选择起点，保留等待活动与原零重建门槛；驱动对需要重启的计划保留最新待发布快照，旧进程 reaped 后才发布 CDB 和生成 prime 文件，再启动新进程。普通无需重启的增量更新仍直接发布。写入失败不得记成功或启动读取旧配置的新引擎，应保留待发布计划供明确重试。新增 native regression 后先以旧实现确认失败，再统一编译、测试并重跑原两逻辑核链路；目前这组补修尚未验收。

原两核完整链暴露另一个调度缺陷：普通模型 apply 重置 implementation sweep 完成状态后，没有待处理队列或新的 prime completion 时不会再调用 pump。900 s drain 最终同时看到 sweep 未完成、未调度，不能视为空闲。已在成功增量 apply 后调用既有 pump，遵循原工作/安静期判断；native 同计划重应用对照在旧实现有 4 个断言失败，补修 dev/release 各 40 个执行体及 engine 65 cases / 650 assertions 全通过，仍待完整新字节链及最终 CI。历史原链结果与独立 runner SIGSEGV 均保留在验证简报，不能由这一项修复替代验收。

最终自审补充恢复边界：首份或普通增量 CDB 单次写失败后，用户修复磁盘直接重启，不应依赖另一次 apply 才恢复接受流量和模块/implementation 元数据。start_process 在无接收流量状态下，发布保留的完整快照后重新应用，再启动；保留首次写失败提前返回，不让旧进程采用尚未发布的状态。新增只失败一次直接恢复的原生对照覆盖首份与增量计划，旧实现 5 个断言失败，补修 dev/release 各 40 执行体通过；其余最终验收继续。

原生 Windows CI 暴露 CDB 中 8.3 短名与工作区长名被当成不同项目的问题。目标提取、有效目标校验和模型 profile 现统一使用平台规范路径判断归属，保留原编译参数和嵌套项目边界。另修未信任 mcpp 根对扫描回退模型错误执行 producer 选择代数校验引起的无限重载；可信根的过期结果拒收保持有效。新 Workspace 对照在旧 guard 有 6 个断言失败；补修 dev/release 各 40 执行体通过（engine 67/693，target 9/92），24 组真实 LSP 通过。新字节原生 CI、完整性能与发布升级仍待验收，结果统一记入简报。

workspace 声明成员自身的别名身份也必须保留：原路径/一层 glob 匹配失败时，以声明成员目录的规范身份再核对，按一次归属快照缓存，保持嵌套与外部项目排除。对应旧实现行为对照为 8 个断言失败；正式结果与最终字节统一由简报更新。


发布预检后的状态自审：`072c230` 首轮 CI 的 U15-edits 出现 13/min > 12/min，原产物缺逐次状态时间，原因未确定。独立 native 对照证明 engine apply 的同步回调会提前发布新 profile；采用同步 apply 内的状态批次边界，所有参与引擎完成后发布一次，不改异步状态和既有 hold。runner 同时保存紧凑转换轨迹及严格 `<60s` 窗口支撑，保持原计数/门槛。旧产品恰一项断言失败、补修 dev/release 回归通过；其余新产品验证仍待完成，正式结果统一由简报记录。

最终范围审计补充：`1341938` 产品 PR CI 与原九阶段本地 53 项均通过，但原 Windows target fixture 使用 mock producer，不足以证明 §5/§10 的真实 mcpp 双目标生产链。新增真实 Windows fixture 以 mcpp 实际生成的完整 i686/x64 CDB 在同一会话切换，保留原模拟场景用于错误与竞态验证；该新增项须原生 CI 实际通过后才满足验收。产品/runner 未因此调整预算或实现。正式发布与原 0.0.16 profile 升级仍需后续实际执行。
