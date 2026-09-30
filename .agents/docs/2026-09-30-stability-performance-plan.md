# mcppls 0.0.7 方案（单 PR：修复 + 优化）：proot 起不了子进程、热启动重建全部 BMI、GalTranslPP 半不可用、用户体验场景测试

状态：方案第 2 版（0.0.7 单 PR；§0.0 的决定已定，§9 为剩余待确认项）· 2026-09-30 · 基于 mcppls `main` 7265cdf（0.0.6）

第 1 版 → 第 2 版：按 2026-09-30 的决定（§0.0）把全部条目收进 0.0.7 一个 PR（§10）；调度改为"用户体验优先"（§4.3）；
新增以 mcpp 与 xlings 为测试项目的用户体验场景测试与响应速度预算（§6.4、§6.5）；总体自我 review 重写（§11）。

依据：

- #32（termux + proot，linux-arm64）：用户上传的诊断包 `mcppls-bundle-20260929T151357.144Z.zip`（5 份服务端日志、report、
  environment）；termux/proot HEAD `d4d2a19`（2026-09-24，v5.1.107.95）与 proot-me/proot 源码；本机（x86_64）两种复现：
  seccomp 过滤器让 `execveat` 返回 ENOSYS，以及本机从源码构建的 termux-proot；
- #30（热启动重建全部 BMI）：issue 正文与 draft #29 的测量；代码追踪；本机小型 mcpp 项目冷启动 / 热启动的引擎数据库 diff；
  临时 worktree 里写出的失败单测（主仓库未改）；
- GalTranslPP（Windows，MSVC STL，LLVM 22.1.8，mcpp 2026.9.29.4，Qt 6.11.1）：本地源码 `/home/speak/workspace/scode/GalTranslPP-mcpp`
  （`pr3/rebase-3.1.2`）；Sunrisepeak/GalTranslPP#1 的 Windows CI 探针两轮（4 核 runner，§3.1），产物在 scratchpad `c-galtransl/r1`、`r2`；
- 代码审计：`src/engine/clangd.cpp`、`src/engine/clangd/{guard,primer}.cpp`、`src/orchestrator/workspace.cpp`、
  `modules/platform/src/process.cpp`、openkal-linux 0.15.0；本机 `~/.cache/mcppls`（6.6 GB）里 clangd 模块缓存的实际形态；
- Linux 实测（§6.3）：mcppls 0.0.6 发布版 payload，项目为 mcpp（176 个模块）与 xlings 的副本，S1–S8 共八类场景；
- Code-OSS（§7）：VSCodium 1.135.06055 + 0.0.6 的 `mcppls-linux-x64.vsix`，对照 Microsoft VS Code stable。

编号：#32 为 X-*（spawn），#30 为 W-*（warm start），GalTranslPP 为 G-*，调度与看门狗为 R-* / K-*，缓存为 C-*，
用户体验场景测试为 U-*，Code-OSS 为 O-*。每条结论标明证据等级：**已验证**（复现或实测）、**代码确认**（读代码可证，未跑）、
**推断**（待验证）。

## 0. 摘要

### 0.0 已定的决定（2026-09-30）

| # | 决定 | 落到哪里 |
|---|---|---|
| D1 | **termux / proot 是正式支持的环境**：proot 的两种模式（默认 seccomp 加速、`PROOT_NO_SECCOMP=1`）都必须能用 | X-1..X-5 全部进 0.0.7；CI 里 proot 作业为必跑（§1.3、§10） |
| D2 | std 的参数来源：mcpp 项目用 producer 的 `mcpp:std` 集合；没有 std 描述的来源用宏白名单 | G-1（§3.2） |
| D3 | worker 数、预建并发、后台索引（N-7）、超时与重启，**一律从用户体验出发：用起来感觉快、无感** | §4.3 的调度策略（R-1、R-2、R-5、R-7、R-8、R-9、K-6） |
| D4 | 启动 clangd 前**无条件清空** `.cache/clangd/modules/.locks/` | C-4（§5） |
| D5 | 给 mcppls 增加各种场景的实用测试与响应速度测试，**以 mcpp 与 xlings 两个项目为测试项目**，满足较好的用户体验要求 | U-* 场景与预算（§6.4）、实现与 CI（§6.5） |
| D6 | 这份方案是 **0.0.7 的单 PR**：修复 + 优化一起做 | §10 |

### 0.1 结论

1. **#32：根因确定（已验证）。** openkal-linux 启动子进程一律用 `execveat(dirfd, 相对名)`；termux 的 PRoot 只支持
   `execveat(AT_FDCWD, …)`，其余直接返回 `-ENOSYS`（`src/syscall/enter.c:1877-1888`，2023-05 起未变）。ENOSYS 被映射成
   `kal_err_not_supported`，于是登录 shell、mcpp、编译器探测、clangd **全部**"cannot start …: not supported"。mcppls 自己能
   起来，是因为 VS Code 的 Node 用的是 `execve`。在本机用 seccomp 过滤器和真实的 termux-proot 都复现出了与诊断包逐字相同的
   报错；在同一过滤器下"`execveat` 失败再用绝对路径 `execve`"可以启动。修法在 openkal（X-1），mcppls 这边要把
   "引擎崩溃"改成"这个环境不允许启动程序"（X-2）。proot 定为正式支持（D1）后，默认 seccomp 模式下更早出现的另一种失败
   （"program is outside every preopened directory"，X-4）也要修：根因是 PRoot 的 seccomp 快速路径改写了 `openat` 的 RSI 却不恢复，
   违反系统调用的寄存器约定，而 openkal 的内联汇编把参数寄存器声明为只读、编译器复用了它（已用最小复现与打补丁的 openkal 验证）。
   修法同在 openkal（参数寄存器改为读写操作数，X-4）。两种模式、x64 与 arm64 的 CI 作业为必跑（X-3）。
2. **#30：根因确定（已验证），而且比 issue 里描述的更贵。** 模型缓存写入时丢了 `optionsDerived` 标志，热启动时计划
   按结构化选项重新拼命令：参数重排，`-O*`、`-g*`、`-W*` 被丢掉。issue 没发现的两点（(a) 已在 Linux 与 Windows 实测，(b) 代码确认）：
   (a) producer 确认"unchanged"后，`adopt_model` 仍把**模型对象换成了 producer 的**（`workspace.cpp:1182`），所以之后
   **第一次重新计划**（编辑 import 后 2 s、保存、监视到的源文件变化、打开计划外的文件）就把参数翻回 producer 形态，
   所有模块单元的命令都变 → 重启 clangd → 全部 BMI 再建一遍，一次热启动付两次钱；
   (b) primer 判断"BMI 已存在"时不看命令哈希，也不看依赖（`clangd.cpp:3591`），把上一轮留下的、命令不同的 BMI 当成可用，
   于是一个模块也不预建，clangd 在用户打开的那个文件的 worker 里**串行**重建全部 176 个——这就是 issue 里
   "0 个 primed、176 个 Built module、55–78 s"的直接原因。修法：持久化标志 + 缓存格式升版本（W-1），冷热数据库必须逐字节相同的
   回归检查（W-2），不再按文件名猜 BMI 是否可用（W-4）。
3. **GalTranslPP：三个现象都在 Windows CI 上复现了，根因都落到了代码上（已验证）。**
   - **`std::views` 误报（G-1）**：项目给所有单元定义了 `_RANGES_`，它恰好是 MSVC STL `<ranges>` 的头文件守卫。真实构建里 std 在 mcpp
     的 `mcpp:std` 集合里单独编、不带项目宏；mcppls 却忽略 producer 的 std 单元，用"代表单元"的参数重编 `std.ixx`（`plan.cpp:613-643`），
     引擎数据库里 std 的命令带着 `-D_RANGES_` 等全部 9 个项目宏，编出的 std 模块里 `<ranges>` 是空的。探针里 clangd 在
     `NormalJsonTranslatorHelperTool.cpp` 报了 19 个 `no_member`；`ConditionTool` 因此编不过，连同它在内 4 个源文件被交给 mcppls 自己的引擎，
     预建永远停在 24/25。修法：std 用 producer 的 std 集合，没有时只保留白名单宏。JSON `{…}` 报错没观察到（那几个文件没拿到 clangd 的诊断），
     倾向于同源（G-2）。
   - **慢、"写半分钟就死一次"（G-3）**：4 核 runner 上 `-j=2`、预建上限 1，冷启动预建 31 分钟；打字时补全 p50 **29.9 s**，16 次里 12 次超时——
     补全总是等满 30 s 后由 mcppls 回退作答。每个热启动的会话里 #30 链条让 182 个单元同时换命令、clangd 重启；自动保存下 1.5 s 的磁盘安全期限
     误判重启（K-4）；排队等 worker 的文件被首诊断守卫当成卡住、放到一边（K-5）。在扫描模型下（emit 超过 60 s 时，G-4），clangd 还会约每 26 s
     崩一次，5 次后 mcppls 放弃 clangd（K-2），数据库来回翻转重启（R-6）。
   - **一直 preparing、只能删缓存（C-4 + C-6）**：mcppls 重启 clangd 时只等 500 ms 就 `TerminateProcess`（`connection.cpp:84-97`），正在建模块的
     clangd 留下 `.cache/clangd/modules/.locks/` 里的锁；Windows 上下一个 clangd 永远等这把锁。探针复现：杀掉 clangd 后 **480 s 停在 12/25**，
     只删锁文件 145 s 恢复。上面那些重启每一次都可能触发它。mcppls 查的是 `<pcm>.lock`，位置不对，也从不清理。
4. **Linux 实测（mcpp 176 模块、xlings，§6.3）把"一直 preparing / 只能删缓存 / 死掉"都复现了出来，而且都不需要 Windows：**
   - **过期的模块锁**（C-4，P0）：clangd 把锁放在 `.cache/clangd/modules/.locks/`，内容是"主机名 pid"；pid 被复用或主机名不同时
     clangd 永远等，预建停在 158/180，状态在 preparing 与 degraded 之间来回 9 分钟以上；运行中只删这两个锁文件就恢复。mcppls 查的是
     `<pcm>.lock`，位置不对。
   - **崩溃后重启 = 冷启动**（C-5，P0）：崩溃后 30 s 内重启（VS Code 会立刻重启崩溃的服务端），旧租约还"活着"，新实例用私有目录
     冷启动（66 s 对 3.6 s）。另外热启动本来就走 #30 的链条，杀之前建好的 79 个模块全部重建。
   - **扇出保存被大量导入的接口，clangd 崩溃循环**（5 / 5 次），随后 K-2 放弃 clangd，error 状态持续 15 分钟以上。
   - **#30 在会话中途发作**：热启动后第一次重新计划，526 个单元里 342 个命令变了，clangd 重启、全部 BMI 重建（W-3，已验证）。
   - **笔记本**：模拟 8 线程时预建 116 s，32 线程默认 49 s；瓶颈是预建上限，`-j` 调大无益（R-2）。首次跨模块跳转冷启动时总是等满
     30 s 回退。
5. **缺的不是某一个修复，而是约束**：没有"交互请求永远留一个 worker"的预算，没有"引擎命令是模型的纯函数"的不变式，
   没有覆盖大项目 / 打字 / 崩溃 / 热启动复用 / 低核数 / Windows / 容器的性能与稳定性基线（发布门槛只量了一个 3 文件
   fixture 的首次跳转）。0.0.7 用"用户体验优先"的调度补上前两条（§4.3），用 mcpp 与 xlings 上的 U1–U15 场景测试与响应速度预算
   补上第三条（§6.4、§6.5），并把它们作为合入与发布的门槛。
6. **Code-OSS：没有发现 mcppls 的问题，缺的是持续测试（已验证）。** 现有 E2E 只在 Microsoft VS Code 上跑（`runTest.ts`
   写死 `downloadAndUnzipVSCode`）。给 harness 加上"指定编辑器"后，VSCodium 1.135 上 main（26 通过）、conflicts、stress 三个套件
   与 VS Code 1.139 结果完全相同；Open VSX 按平台发对了包，冲突检测对 Open VSX 上的 clangd 扩展生效。风险在没测到的环境：
   termux 的 code-oss（就是 #32）、Flatpak / Snap 沙箱、远程扩展宿主、Windows / macOS 的 VSCodium。§7 给出 CI 方案（O-1..O-5）。

### 0.2 条目总表

| # | 问题 | 根因 / 机制 | 修法 | 证据 | 优先级 |
|---|---|---|---|---|---|
| X-1 | proot 下所有子进程起不来 | PRoot 不支持带 dirfd 的 `execveat` | openkal：ENOSYS 时退回绝对路径 `execve` | 已验证 | P0 |
| X-2 | 状态说"clangd 崩溃" | 启动失败复用 `engine-crashed`（`clangd.cpp:1418`） | `engine-start-failed` + 全局"不能启动程序"诊断 + report 记 `sandbox` | 已验证 | P0（D1） |
| X-3 | proot 没有测试 | — | seccomp-ENOSYS 单测；CI 里从源码构建 termux-proot，x64 与 arm64、两种模式跑 conformance 与 VSIX 套件，必跑 | — | P0（D1） |
| X-4 | proot 默认（seccomp）模式下"program is outside every preopened directory" | PRoot 的 seccomp 快速路径改写 `openat` 的 RSI 不恢复；openkal 的 `okl::sys()` 把参数寄存器声明为只读，`fs.cpp:46` 复用了 RSI | openkal：参数寄存器改为读写操作数（x86_64、aarch64，连同 openkal-musl）；mcppls 在预打开表缺 `/` 时告警；向两个 PRoot 报 issue | 已验证（最小复现 + 打补丁后两种模式都通过） | P0（D1） |
| X-5 | proot 下的其它环境差异 | 已查：inotify、`/proc` 读数、`PR_SET_PDEATHSIG`、clangd 在本机 proot 下都正常；Android 的 `hidepid` 未测 | 探测 proot 并记入 report；CI 固定检查这些项；文档写明支持范围 | 已验证（本机 x86_64） | P1（D1） |
| W-1 | 热启动参数与冷启动不同 | `optionsDerived` 不进缓存 | 持久化标志，缓存 envelope 2→3 | 已验证（失败单测） | P0 |
| W-2 | 回归检查缺失 | `module-cache-reused` 只看 std | 冷热引擎数据库逐字节相同；热启动零新 `.pcm` | — | P0 |
| W-3 | 第一次重新计划后又重启 clangd | `adopt_model` 在 unchanged 时换了模型对象 | 保持一个模型；计划不同却"unchanged"时记 incident | 已验证（Linux：526 个单元里 342 个命令变了，全部 BMI 重建） | P0 |
| W-4 | primer 把错命令的 BMI 当可用 | `module_already_built_` 按文件名合并所有命令哈希 | 去掉猜测（全部预建，clangd 复用有效 BMI）或自建清单 | 代码确认 | P1 |
| W-5 | 命令抖动 | 同一模块在本机缓存里多达 11 种命令哈希 | 命令稳定性不变式 + 每次变化记原因 | 已观察 | P1 |
| G-1 | `std::views` 误报，连带 `ConditionTool` 编不过、4 个文件失去语义 | std 单元照搬代表单元的 `-D`，项目的 `-D_RANGES_` 正是 MSVC `<ranges>` 的守卫 | std 用 producer 的 std 集合；否则宏白名单 | 已验证（Windows CI，真实模型） | P0 |
| G-2 | JSON `{…}` 报错 | 未定位：两次探针里相关文件都没拿到 clangd 的诊断；倾向与 G-1 同源 | G-1 后复测；向报告人要截图或诊断包；否则上游登记 | 未复现 | P1 |
| G-3 | 补全 p50 30 s、频繁"死掉" | 2 个 worker 被饿死（R-1/R-2）；#30 每会话一次全量换命令重启（W-3）；K-4、K-5 的误判；扫描模型下还有崩溃 + 放弃（K-2）与翻转（R-6） | 见各条 | 已验证（Windows CI） | P0 |
| G-4 | 大项目永远拿不到 mcpp 的模型 | 离线时 producer 硬期限 60 s（`workspace.cpp:1065-1068`），GalTranslPP 在 4 核 Windows runner 上 `mcpp emit` 要 58–79 s；被杀后只剩扫描模型，也不写缓存，每次会话重来 | 期限按该项目上次 emit 的耗时自适应，有进展就不杀；向 mcpp 报 emit 慢 | 已观察（CI），代码确认 | P0 |
| C-1 | 只能手工删缓存 | 没有编辑器命令；CLI `cache --clean` 要求服务端不在用 | "重置本工作区缓存"命令：停服 → 清 → 重启 | — | P0 |
| C-2 | 缓存无上限 | 无 GC；本机 6.6 GB | 只留当前命令的 BMI + 容量上限 + 启动时后台清理 | 已观察 | P1 |
| C-3 | 坏缓存不会自愈 | 无完整性检查 | 同一故障跨会话重复时自动把 clangd 模块缓存移开一次 | 推断 | P1 |
| C-4 | 一直 preparing、只能删缓存 | clangd 的锁在 `.cache/clangd/modules/.locks/`；Windows 上主人死了也一直等，Linux 上 pid 被复用或主机名不同时一直等；mcppls 查的是 `<pcm>.lock`（`clangd.cpp:3604`） | 启动前清空 `.locks/`；解析"Still waiting for module lock"；硬杀后清掉被杀 pid 的锁 | 已验证（Windows 480 s 卡住、删锁恢复；Linux 复现） | P0 |
| C-5 | 崩溃后重启变成冷启动 | 租约无 pid、按心跳 30 s 过期（`instance.cppm:12-13`），崩溃后立即重启的服务端被当成第二实例、用私有目录 | 租约记 pid + 启动时间，持有者已死即接管 | 已验证（Linux：66 s 对 3.6 s） | P0 |
| C-6 | mcppls 的重启在制造过期锁 | 重启只等 500 ms 就 terminate，Windows 上即 `TerminateProcess`（`connection.cpp:84-97`） | 先发 LSP `shutdown`/`exit` 等足 5 s，超时才 terminate，之后清锁 | 代码确认 + CI 现象 | P0 |
| R-1 | 用户请求排在后台构建后面 | primer、N-7、定义搜索各自限流，互不知道 | 统一后台预算：`worker − 1`；有交互时 N-7 让路 | 代码确认 | P0 |
| R-2 | 笔记本上 `-j=2`、预建 1 个 | `engine_workers = max(2, 物理核/4)`；瓶颈是预建上限，`-j` 调大无益 | 按 D3 重定 worker 与后台并发（§4.3），`mcppls.engine.workers` 设置 | 已验证（模拟 8 线程：预建 116 s 对 49 s） | P0 |
| R-3 | 自己造成的等待被算成 clangd 卡住 | 超时计入 Quarantine | 有后台构建在跑时的超时不计入隔离 / stall | 代码确认 | P1 |
| R-4 | 冷启动过渡模型的 BMI 白建 | 2.5 s 后按 L4 预建，producer 一到就重启 | 过渡模型下只预建 std，producer 回答后再开始 | 已观察（xlings producer 约 5 s） | P1（D3） |
| R-5 | N-7 在大项目上长期占满 worker | 每打开一个文件立即排 16 个，空闲 15 s 后排全部，开始后不让路 | 只在空闲时做、一次 1 个、有活动立即暂停（§4.3） | 已验证（ready 后约 45 s 请求变慢） | P0（D3） |
| R-7 | 用户的请求等 10–30 s 才有回应 | 所有交互请求统一 10 s / 30 s（`clangd.cppm:46-52`） | 按方法分级的等待预算：补全 1 s 内必有回应（回退结果标 incomplete），其余见 §4.3 | 已验证（补全 p50 29.9 s） | P0（D3） |
| R-8 | 重启打断正在用的人 | plan / recovery 重启立即执行 | 非紧急重启等输入空闲 ≥ 3 s（最多推迟 60 s），并走 C-6 | 代码确认 | P1（D3） |
| R-9 | 后台构建和编辑器抢 CPU | openkal 不能降低子进程优先级（design.md §7） | openkal 加优先级选项，clangd 以低于正常的优先级运行（实测后定） | 推断 | P1（D3） |
| R-6 | 打字时数据库来回翻、clangd 反复重启 | 有真实单元的模块被换成替身后立刻"重试"（`clangd.cpp:2138-2159`） | 替身取代不算变化；扫描失败报问题而不是换替身 | 已观察（CI）+ 代码确认 | P0 |
| K-1 | Windows 上检测不到卡死 | `cpu_seconds` 在 Windows 返回空（`process.cpp:520-549`） | GetProcessTimes（经 openkal 或平台层） | 代码确认 | P1 |
| K-2 | 崩 5 次后 clangd 再也不回来 | 5 分钟 5 次崩溃即放弃，不安排重启（`clangd.cpp:1966-1969`） | 按 RD6 退避重试，状态说明反复崩溃 | 已观察（CI）+ 代码确认 | P0 |
| K-3 | clangd 崩溃栈无法符号化 | payload 的 clangd 去掉了符号 | 每个发布附带符号文件，incident 里离线符号化；扇出崩溃做最小复现交上游 | 已观察（Linux S5 5/5 崩溃） | P1 |
| K-4 | 自动保存时误判"clangd 不会完成"而重启 | `DISK_SETTLE` 1.5 s 短于重型文件的正常构建，重启不经预算（`clangd.cpp:243`、`:2698-2702`） | 期限取该文件上次构建时间的倍数，重启走 RestartGate | 已验证（Linux S4） | P1 |
| K-5 | 排队等 worker 的文件被当成卡住 | 首诊断守卫在 "file is queued" 时也计时（`clangd.cpp:1727-1752`）；CPU 饥饿时同样误判 | "queued" 不计时；以 clangd 的 CPU 进展判断 | 已验证（Windows CI、Linux 负载 87） | P1 |
| K-6 | 状态闪烁、degraded 太敏感 | 状态随每个事件立即变化；preparing / degraded 来回（锁的场景 9 分钟） | 状态加迟滞：问题持续 30 s 才 degraded；preparing 显示进度与原因 | 已观察 | P1（D3） |
| U-* | 没有用户体验的场景测试 | 发布门槛只量 3 文件 fixture 的首次跳转 | mcpp 与 xlings 上的 U1–U15 场景与响应速度预算，进 CI 与发布门槛（§6.4、§6.5） | — | P0（D5） |
| O-1/O-2 | Code-OSS 没有持续测试 | E2E 写死 MS VS Code 与 `bin/code` | harness 加 `MCPPLS_E2E_EDITOR`；CI 跑 VSCodium | 已验证（套件全过） | P1 |
| O-3..O-5 | Open VSX 与少见环境 | 发布后无校验；诊断包不记编辑器分支；proot / Flatpak 未测 | 发布后校验、记 `appName`、proot 作业装 VSIX | 已验证 / 未测 | P2 |

### 0.3 0.0.7 的范围（单 PR）

**进 0.0.7**：mcppls 侧的全部条目——X-1..X-5、W-1..W-5、G-1、G-3、G-4、C-1..C-6、R-1..R-9、K-1..K-6、U-*（场景测试与预算）、O-1..O-5，
以及 G-2 若在 G-1 之后复测证实同源。目标一句话：**std 与真实构建一致；热启动不重建；打字不重启；用户在等的请求永远有 worker、
1 s 内有回应；坏了能自己好；以上都有 mcpp 与 xlings 上的场景测试卡住。**

**不在 mcppls 这边、只登记**（issue #24 上游登记，0.0.7 里用 mcppls 侧的办法兜住）：clangd 扇出编辑时的崩溃（K-3 做最小复现）、
Windows 上 LLVM 判断不了锁主人死活（C-4/C-6 兜住）、PRoot 不支持带 dirfd 的 `execveat`（X-1 兜住）、PRoot 的 seccomp 模式不恢复
改写过的参数寄存器（X-4 兜住）、mcpp emit 在大工作区上 53–79 s
（G-4 兜住）。

**依赖**：X-1（`execveat` 退路）、X-4（系统调用包装的寄存器约束）、K-1（Windows 的进程 CPU 时间）、R-9（子进程优先级）需要 openkal 先发一版，mcppls 再升依赖（§10.1 的 T1）。

### 0.4 剩余待确认

见 §9：§0.0 之外剩下的 11 项都给了建议并按建议执行，review 时可改。

## 1. #32：termux / proot 下所有子进程都起不来（X-*）

### 1.1 现场

诊断包：linux-arm64，Debian 13 trixie（termux + proot），VS Code 1.136.2，mcppls 0.0.6（payload `from-source`、`dirty`，
commit 7265cdf），8 个逻辑处理器、12 GB 内存。五次会话的日志里，服务端启动的每一个程序都以同一句失败：

```
tool environment is this process's: the login shell /bin/sh could not be started: cannot start /bin/sh: not supported
mcpp configure could not start: cannot start ~/.xlings/subos/current/bin/mcpp: not supported
probe of /usr/bin/g++ failed: /usr/bin/g++ did not answer any query: cannot start /usr/bin/g++: not supported
engine issue engine-crashed: clangd could not start: cannot start …/payload/clangd/bin/clangd: not supported
```

report 里 clangd 为 `unavailable`、`restarts: []`；只剩 mcppls 自己的引擎（documentSymbol、semanticTokens 由 `merged` 回答）。

### 1.2 根因（已验证）

1. openkal-linux 0.15.0 的 `kal_process_spawn`（`src/process.cpp:172-340`）先 `clone(SIGCHLD)`，子进程里
   `execveat(base, 相对名, argv, envp, 0)`（`:314`），失败原因经管道回传；`translate`（`src/sys.h:318`）把 ENOSYS 映射成
   `kal_err_not_supported`，mcppls 打印为 "not supported"（`modules/platform/src/process.cpp:34`）。
2. termux/proot HEAD `d4d2a19`，`src/syscall/enter.c:1877-1888`：

   ```c
   case PR_execveat:
       if ((int) peek_reg(tracee, CURRENT, SYSARG_1) == AT_FDCWD) { … set_sysnum(tracee, PR_execve); … }
       else {
           note(tracee, ERROR, SYSTEM, "execveat() with non-AT_FDCWD fd is not currently supported");
           status = -ENOSYS;
   ```

   来自 `2d7c70e`（2023-05-13，"Minimal support for execveat(AT_FDCWD)"，v5.1.107.72），此后 40 次改动 `enter.c` 都没动它；
   `enter.c:2866` 的注释也承认 apk-tools v3 的 `memfd_create + execveat` 在 PRoot 下不可用。上游 proot-me 完全不处理
   `execveat`，直接交给内核——`-r /` 下的静态程序能跑，真正的 guest rootfs 加动态链接的 clangd 大概率不行（未验证）。
3. 复现（本机 x86_64，产物在 scratchpad `a-proot/`）：
   - seccomp 过滤器只让 `execveat` 返回 ENOSYS：`mcppls report` 出现与诊断包相同的
     `the login shell /bin/bash could not be started: cannot start /bin/bash: not supported`，report 里
     `{"code":"engine-crashed","message":"clangd could not start: cannot start …/clangd: not supported"}`；去掉过滤器一切正常；
   - 本机构建的 termux-proot（`PROOT_NO_SECCOMP=1 proot -r / mcppls report …`）：编译器探测
     `cannot start …/g++: not supported`；
   - 同一过滤器下，`execveat` 失败后用 `readlink("/proc/self/fd/<dirfd>") + "/" + name` 拼绝对路径再 `execve`：子进程正常退出 0。
4. **另一种失败（X-4，根因确定，已验证）**：termux-proot 默认的 seccomp 加速模式下，13 次运行都更早失败在
   `program is outside every preopened directory: /bin/bash`（`modules/platform/src/process.cpp:97`）；`PROOT_NO_SECCOMP=1`
   后才变成上面的 "not supported"。根因是 **PRoot 在 seccomp 快速路径上违反了系统调用的寄存器约定**，而 openkal 的内联汇编恰好依赖这条约定：
   - Linux 的 `syscall` 只改 RAX、RCX、R11。openkal-linux 的 `okl::sys()`（`src/sys.h:46-84`，aarch64 为 `:126-172`）把参数寄存器声明为
     只读输入，编译器因此可以在系统调用之后继续使用它们里面的值。
   - PRoot 翻译 `openat(dirfd, path, …)` 时，把 RSI 里的路径指针换成它写在栈指针下方的一份副本；openat 在 seccomp 过滤器里是"不需要
     看返回"的调用（`syscall/seccomp.c:394`，flag 0），PRoot 以 `PTRACE_CONT` 放行（`tracee/event.c:474-482`），**从不在返回时恢复 RSI**
     （`tracee/reg.c:296-326` 只在返回的那一站恢复）。ptrace 模式会停在返回处，所以没事。
   - openkal-linux `src/fs.cpp:46` 的 `t[1] = { "/", 1, … }` 紧跟在 `openat("/")` 之后，编译出来就是"把系统调用之后的 RSI 存成这一项的名字"
     （发布版 mcppls 反汇编 `0x305259`：`mov %rsi,0x7acb38`）。于是 `"/"` 这一项的名字指向一块已经无效的栈，mcppls 第一次解析程序路径
     （第一次启动子进程，即登录 shell）时找不到 `/`，报 outside every preopened directory。
   - 复现（scratchpad `g-proot-seccomp/`）：`rsi.c` 做一次原生 `openat` 再读回 RSI，原生与 ptrace 模式 `PRESERVED`，termux-proot d4d2a19 与
     proot-me 5.4.1 的 seccomp 模式都 `CLOBBERED`；用 openkal-linux 真实源码编的 `t1` 打印预打开表，seccomp 模式下第二项 `name=''`。
   - 打上补丁（参数寄存器改为 `"+D"`、`"+S"` …，外加 X-1 的 `execveat` 退路，`openkal-linux-proot.patch`）后：预打开表在两个 PRoot、两种模式下
     都是 `'/'`；按 mcppls 的启动路径写的 `t2`（解析 `/bin/echo`、`kal_process_spawn`、读管道）在 termux-proot 与 proot-me、两种模式、以及原生
     环境下都成功。没有用补丁重新构建 mcppls 本身（要改 `~/.mcpp/registry` 里的传递依赖），这一步留到 T1。
   - 失败 1（`execveat`）只在 termux-proot 上有；proot-me 5.4.1 把带 dirfd 的 `execveat` 交给内核，未打补丁的 openkal 也能启动子进程。
5. **proot 下的其它方面（已查，本机 x86_64）**：inotify 可用（mcppls 自己不用 inotify，文件监视在客户端或 mcppls 的轮询线程里，clangd 自己监视）；
   `/proc/<pid>/stat`、`/proc/self/stat` 可读（CPU 读数可用）；`PR_SET_PDEATHSIG` 行为与原生相同；payload 里的 clangd 23.1.0 在 proot 下
   `--version`、`--check` 都正常。没法在这里测的：Android 真机的内核、seccomp 策略与 `hidepid`，以及 aarch64 上的寄存器问题（形状相同，未实测）。

### 1.3 修法

- **X-1（P0，openkal）**：`kal_process_spawn` 的子进程分支里，`execveat` 返回 `-ENOSYS` 时退回 `execve(绝对路径)`。
  绝对路径在 **clone 之前**由父进程算好（根预打开目录就是 `"/" + name`，其余用 `readlinkat` 读 `/proc/self/fd/<b>`），
  子进程里不分配内存；进程级记住"execveat 不可用"，之后直接走 `execve`。代价只是在 `execveat` 坏掉的环境里失去 dirfd
  解析的防竞争性质；不受 `process.cpp:285-311` 注释里 `/dev/fd/N` 与解释器问题的影响（`execve` 用的是真实路径）。
  备选：`kal_spawn` 带一个可选的绝对路径（接口变化，mcppls 的 `Process::spawn` 本来就要求绝对路径）。mcppls 平台层做不了：
  kal 的接口只收 `{dirfd, 相对名}`。
- **X-2（P0，mcppls）**：
  - 启动失败不再用 `engine-crashed`（`clangd.cpp:1418`；它和真正的崩溃 `:1942` 共用一个代码，而且因为不进 `crashes_`，
    `status.failed` 永远是 false，`:382`）。新代码 `engine-start-failed`，归入 `environment` 类（同 `engine-missing`、
    `payload-corrupt`，`:1320-1331`）；
  - 服务端级诊断：登录 shell 或前几次启动都以 `not_supported` 失败时，只报一条"这个环境不允许 mcppls 启动程序
    （`execveat` 未实现，常见于 PRoot / Termux）；只剩模块级功能"，而不是十几条 info 日志；
  - report 的 environment 加 `sandbox`：`/proc/self/status` 的 `TracerPid ≠ 0`、`PROOT_*` 环境变量。
- **X-3（P0，测试，D1）**：proot 是正式支持的环境，测试随之成为必跑项：
  - 平台层单测：seccomp 过滤器让 `execveat` 返回 ENOSYS，启动 `/bin/true` 与一个 `#!` 脚本（openkal 与 mcppls 各一份）；
  - CI 作业：从固定 commit 构建 termux-proot（ubuntu-24.04 与 ubuntu-24.04-arm），在默认（seccomp）与 `PROOT_NO_SECCOMP=1` 两种模式下
    跑 conformance 的 `inferred`、`mcpp-emit`、`timing` 与 VS Code 的 main 套件（VSIX 模式），断言 clangd ready、跨模块跳转可用；
  - 报告人（本机 termux，linux-arm64）在发布前用候选 VSIX 复测一次，作为 0.0.7 的验收项之一。
- **X-4（P0，D1，openkal）**：`okl::sys()` 的全部参数寄存器改为读写操作数（x86_64 的 `"+D"(a), "+S"(b), "+d"(c)`、r10 / r8 / r9 的
  `"+r"`；aarch64 的 x1–x5 同样改为 `"+r"`）。这与内核 ABI 兼容、没有代价，并且覆盖 openkal 的每一个调用点，而不只是 `fs.cpp:46`。
  openkal-musl 里沿用的 musl `arch/*/syscall_arch.h` 是同样的写法，一起改（今天能用只是碰巧）。mcppls 平台层加一道便宜的防线：
  `preopens()` 里没有名为 `/` 的项时记一条 warning，而不是等到启动子进程时才报一个看不懂的错。向 termux/proot 与 proot-me 各报一个
  issue：seccomp 模式下改写过参数的系统调用应在返回时恢复寄存器（或停在返回处）。
- **X-5（P1，D1）**：启动时探测 proot（`TracerPid` 指向 proot、`PROOT_*` 环境变量），report 记下 `sandbox: proot`；§1.2 第 5 条查过的
  inotify、`/proc` 读数、`PR_SET_PDEATHSIG`、clangd 在 X-3 的作业里固定为检查项；`docs/00-install.md` 写明 termux 的支持范围与已知限制
  （Android 的 `hidepid` 可能让 `/proc/<pid>/stat` 不可读，此时 StuckWatch 不工作，与今天的 Windows 相同）。

## 2. #30：热启动重建全部 BMI（W-*）

### 2.1 根因（已验证）

1. 冷启动：producer 的数据库经 `spec::complete_options`（`src/project/model.cpp:327`）结构化，并标记
   `set.optionsDerived = true`、`unit.optionsDerived = true`（`src/spec/options.cpp:267,277`）。计划据此**原样使用**
   `unit.arguments`（`src/normalize/plan.cpp:306-314`）：

   ```cpp
   const auto effective = effective_options(set.optionsDerived ? std::nullopt : set.options,
                                            unit.optionsDerived ? std::nullopt : unit.options);
   const auto arguments = effective ? options_arguments(*effective, …) : project::expand_response_files(unit.arguments, …);
   ```

2. 缓存：`spec::to_json` 把结构化选项写进 `ide.options`，但**不写标志**；`spec::from_json` 读回时标志保持默认 false
   （`src/spec/database.cpp:190-266`、`:306-330`；`database.cpp` 里根本没有 `optionsDerived`）。热启动的模型于是被当成
   "producer 自己声明了选项"，计划走 `options_arguments`（`src/normalize/semantic.cpp:46-89`）重新拼命令：固定顺序
   （标准、宏、`-iquote`、`-I`、`-isystem`、…），`-O*`、`-g*`、`-W*`、`-w`/`-pedantic`、`-fdiagnostics-*` 被当作非语义参数丢弃
   （`src/spec/options.cpp:36-48`、`structure_gnu` `:101`），`-isystemX` 变成 `-isystem X`，热启动路径也不展开响应文件。
3. `describe_model` 同样看不到这个标志，所以 producer 确认时判为 "loaded again: unchanged"，不重新计划。
4. 本机复现（小型 mcpp 项目，同一缓存目录冷启动再热启动，比较 clangd 读的 `contexts/default/cdb/compile_commands.json`）：
   9 个条目全部不同，例如

   ```
   冷：clang++ -std=c++23 -O0 -g --no-default-config -nostdinc++ -isystem<x>/c++/v1 … -D__MCPP_TARGET_LINUX__=1 -c -x c++-module …
   热：clang++ -std=c++23 -D__MCPP_TARGET_LINUX__=1 -isystem <x>/c++/v1 … --no-default-config -nostdinc++ -c -x c++-module …
   ```

   失败单测（worktree `scratchpad/b-cache`，`tests/test_spec_options.cpp:237`，mcpp 2026.9.26.1）：
   `derived options restate the arguments; read back as the producer's own, the plan drops -O2 and reorders the rest`。
5. 受影响的来源：只有经过 `complete_options` 的——mcpp 与 S1 构建数据库（`--database`、CMake 构建数据库）。CMake 的
   compile_commands、xmake、meson、inferred 不派生选项，往返不变（读代码，未跑）。
6. `-O*` 不只是缓存键的问题：它决定 `__OPTIMIZE__`，`#ifdef __OPTIMIZE__` 的代码在两条路径下语义不同。

### 2.2 issue 没看到的两层（代码确认）

- **第一次重新计划后再付一次钱。** `adopt_model` 在 unchanged 时依然执行 `model = std::move(loadedModel)`
  （`src/orchestrator/workspace.cpp:1180-1182`），会话里的模型从此是 producer 的（标志为 true）。之后任何一次
  重新计划——编辑 import / module 声明后 `EDIT_SETTLE` 2 s（`workspace.cpp:862`）、编辑中 5 s 内保存（`:1889`）、
  监视到未打开的源文件变化（`handle_watched_files`）、打开计划外的文件（`note_opened`）、doom/unresolved 被遗忘
  （`clangd.cpp:759`）——都会得到 producer 形态的参数；所有模块接口单元都是 provider，`argumentsChanged` 为真，
  clangd 以 `plan` 原因重启（`clangd.cpp:655`、`:742-751`），primer 清零，BMI 再全部建一遍。
- **primer 以为已经建好了。** `module_already_built_`（`clangd.cpp:3591-3607`）用 `cached_bmis_`（`:3573`）列出
  `.cache/clangd/modules/<源文件名>-<路径哈希>/<命令哈希>/*.pcm`，**去掉路径哈希、合并所有命令哈希目录**，只要有同名
  `.pcm`、没有 `.lock`、且比模块自己的源文件新，就算"已建"。命令不同（#30）、依赖的接口变了、甚至另一个目录里同名的源文件，
  都会被当成可用。结果是 primer 一个也不预建，clangd 在用户打开的文件的 worker 里按依赖顺序**串行**重建——这正是 issue
  测到的 "0 modules primed、Built module 176、Reusing persistent module 0"。
- **缓存越积越多。** 本机 `~/.cache/mcppls` 6.6 GB；mcpp 工作区 176 个模块 878 个 `.pcm`，xlings 134 个模块 628 个（1.2 GB），
  hello 的 `greet.cppm` 有 11 个命令哈希目录。除了 #30，还有别的命令抖动来源（W-5），而且从不清理（C-2）。

### 2.3 修法

- **W-1（P0）**：`spec::to_json` 在集合与单元的 `ide.options` 里写 `"derived": true`，`from_json` 读回；
  `modelcache.cpp` 的 envelope `version` 2 → 3（`:145`、`:161`），旧缓存被忽略而不是被误读。`describe_model` 走
  `to_json`，自动包含标志。不选"缓存 producer 原始文档、读回时再结构化"：结果相同，代价更高，而且 `model.cpp` 在结构化之后
  还有删单元、`use_project_build_output`、规范化路径等改动。也不选"两边统一再规范化"：会改变冷启动命令本身，只是把差异藏起来。
- **W-2（P0）**：回归检查。
  - 单测：上面的失败单测进主线；`tests/test_project.cpp` 加 `save_model`/`load_model` 级的计划输出前后对比；
  - conformance 新检查 `engine-database-stable`：`timing`、`mcpp-gcc`、`self-mcpp` 上冷启动再 `--expect-warm` 热启动，
    两次的 `compile_commands.json` 逐字节相同；
  - `module-cache-reused` 从 `std` 扩到项目模块：热启动新增 `.pcm` 为 0，clangd 日志里 `Built module` 为 0。
- **W-3（P1）**：`adopt_model` 判为 unchanged 时保留原模型对象（或换了就重新计划，二选一且一致）；另加一道防线：
  重新计划时若模型与设置都没变而引擎命令变了，记 `plan-drift` incident，附第一处差异。
- **W-4（P1）**：`module_already_built_` 不再猜。首选直接去掉这条捷径——命令一致、BMI 有效时 clangd 自己会复用
  （`Reusing persistent module`），primer 并行打开 `import M;` 的代价是一次加载加校验；§6.3 的实测会给出这个代价。
  如果实测显示代价不可忽略，再改为 mcppls 自己的清单：prime 完成时记下 (模块, 我们写进数据库的命令, `.pcm` 路径)。
- **W-5（P1）**：命令稳定性作为不变式："引擎命令 = f(模型内容, 设置, 引擎 traits)"。`engine-plan-diff` 已经记第一处差异；
  汇总成"每个单元的命令为什么变了"，找出 11 个哈希目录的其余来源（模块提示参数、上下文 / profile 切换、版本升级）。

## 3. GalTranslPP：`std::views` 误报、JSON `{…}` 报错、补全慢 / 频繁"死掉" / 一直 preparing（G-*）

### 3.1 现场与探针

用户现场：VS Code + mcppls 0.0.6，Windows，项目用 mcpp（2026.9.29.x）+ LLVM 22.1.8（`x86_64-pc-windows-msvc`）构建，能编过。
`NormalJsonTranslator.Run.cpp` 报 `No member named 'views' in namespace 'std'`（clang，`no_member`）；JSON 的 `{ … }` 初始化报错；
补全很慢，"写不到半分钟就死一次"，日志一直 preparing，只能手工删缓存。

探针：Sunrisepeak/GalTranslPP#1 的分支 `ci/mcppls-issue23-probe`，提交 `4c701bb`（并入 `pr3/rebase-3.1.2`，mcpp 2026.9.29.4，
换成 mcppls 0.0.6）、`aaf690b`（runner 缺 WER 注册表键时补上）。新的 `.github/mcppls-probe/lab.py` 依次做：真实构建作参照
（`mcpp build --workspace --profile fast-release`）；冷缓存打开 7 个文件，逐条导出诊断和所在行，测准备中 / 空闲时的补全、hover、
semanticTokens；导出 clangd 用的 std 与各单元的命令；用项目自己的 LLVM 22.1.8 分别带 / 不带 `-D_RANGES_` 编 `std.ixx`；
`mcppls check Run.cpp`；10 Hz 打字 200 s（无自动保存 / 1 s 自动保存各一次）并每秒取状态；准备中途分别杀 clangd、杀服务端、
杀服务端进程树、正常退出，再用同一缓存启动，卡住就先只删 `.lock`、再删整个模块目录。
Run：第一次 https://github.com/Sunrisepeak/GalTranslPP/actions/runs/36601166906 （emit 超过 60 s，全程推断模型）；
第二次 https://github.com/Sunrisepeak/GalTranslPP/actions/runs/36616368503 （`ea1eaeb`，`--producer-timeout 600`，mcpp 真实模型，227 个条目）。

### 3.2 `std::views` 误报（G-1，已验证：真实模型下复现）

1. 项目五个成员的 `mcpp.toml` 都定义 `_RANGES_`（`GalTranslPP/mcpp.toml:41`：`defines = [… "SPDLOG_WCHAR_FILENAMES", "_RANGES_"]`）。
2. MSVC STL 的 `<ranges>` 以 `#ifndef _RANGES_` / `#define _RANGES_` 开头（microsoft/STL `stl/inc/ranges`）——这个宏就是它的
   头文件守卫，定义了它，`<ranges>` 就是空的。项目的用意应是让全局模块片段里间接包含的 `<ranges>` 不再展开，`std::views`
   只从 `import std` 来。
3. 真实构建没问题：mcpp 的数据库里 std 单独是一个 `mcpp:std` 集合，只带 `-std=c++23` 和工具链参数，没有项目的宏。
4. mcppls 不用 mcpp 给的 std 单元：计划的第 0 步把 std 源文件跳过（`src/normalize/plan.cpp:236-244`、`:257`），第 5 步按工具链
   清单重新注入，参数是"代表单元"的（`:613-643`：`entry.arguments = driver + representative.arguments`），而代表单元的参数保留了
   项目的全部 `-D`（`src/normalize/gnu.cpp:110-134`）。于是 clangd 编 `std.ixx` 时带着 `-D_RANGES_`，得到的 std 模块里没有
   `std::views`、`std::ranges::to`；`std.ixx` 本身能编过，所以 RD7 的"std 编不过就换 kit"也不会触发，没有任何提示。
5. `Run.cpp` 第 23 行 `savedTranslCacheMap | std::views::keys | std::ranges::to<std::vector>()` 就是第一处报错点。
6. **已验证（第一次探针的 E-hyp）**：用项目的 LLVM 22.1.8 按 clangd 的 std 命令编 MSVC `std.ixx`，再编一个 `import std;` 后用
   `std::views::keys` / `std::views::zip` 的测试文件：不带 `-D_RANGES_` 时 `std.pcm` 42.3 MB、测试通过；带 `-D_RANGES_` 时 40.6 MB，
   测试报 `error: no member named 'views' in namespace 'std'`（与用户看到的逐字相同），`-std=c++23` 与 `-std=c++26` 结果一样。
   `std::ranges::to` 没有报错——它在 `<__msvc_ranges_to.hpp>`，另有守卫；`std::ranges::sort` 在 `<algorithm>`，所以错误只落在
   `views` 上。那次会话是推断模型，std 的引擎命令里没有项目宏（`stdCommandDefines: []`），所以会话本身没复现；mcpp 模型下 std
   是否确实带上 `-D_RANGES_`，由第二次探针的 E-cdb 给出。
7. **已验证（第二次探针，mcpp 真实模型，227 个条目）**：引擎数据库里 `std.ixx` / `std.compat.ixx` 的命令带着项目的全部宏——
   `-DSPDLOG_COMPILED_LIB -DPROJECT_NO_ANSI -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DSPDLOG_WCHAR_FILENAMES
   -D_RANGES_ -D__MCPP_TARGET_WINDOWS__=1 -DQT_CORE_LIB`（MSVC STL，不是 kit）。clangd 随后在 `NormalJsonTranslatorHelperTool.cpp`
   报了 19 个 `no_member`（`std::views::…`，第 117、195、294、378、394、417、418、440、574、706、727、746、821 行），每次发布都在。
   **连锁**：`ConditionTool.ixx` 用了 `std::views::filter`（第 78、245 行），于是"module ConditionTool did not compile"，
   RP1.1 的闭包隔离把 `ConditionTool.ixx`、`Dictionary.cpp`、`NormalJsonTranslator.Core.cpp`、`SkipTrans.cpp` 和一个 prime 单元都交给
   mcppls 自己的引擎，预建永远停在 24/25（或 25/27），状态 degraded。也就是说 G-1 不只是一条误报，它让一串文件失去 C++ 语义。
   `Run.cpp` 本身这次没有拿到 clangd 的诊断（排队等 worker 时被放到一边，见 §3.6），但它第 23 行的 `std::views::keys` 是同一个错。

修法 G-1（P0）：std 单元的参数不再从代表单元照搬。
- producer 描述了 std 怎么编（mcpp 的 `mcpp:std` 集合）时，用它的参数（S1 已有这个信息，只是计划第 0 步有意忽略了——那条规则
  是为 CMake 的 `import std` 定的，对 mcpp 不对）；
- 没有描述时（CMake、compile_commands、推断）：从代表单元取驱动、目标、sysroot、`-std`、标准库选择和影响 ABI 的 `-f*`，
  **宏只保留白名单**（`_ITERATOR_DEBUG_LEVEL`、`_HAS_*`、`_DEBUG`、`/MD` 系列带来的 `_DLL`/`_MT`、`_GLIBCXX_*`、`_LIBCPP_*`），
  其余 `-D`/`-U`/`-include` 一律不进 std；
- 回归：`inferred-msvc` / `mcpp-msvc` 类 fixture 加一个定义 `_RANGES_`（以及 libstdc++ 下的 `_GLIBCXX_RANGES`）的变体，断言
  `std::views::keys` 能补全、无诊断；Linux 上用 libstdc++ 的守卫宏做同样的测试，不必等 Windows。

### 3.3 JSON `{ … }` 报错（G-2，未定位）

项目里大量 nlohmann 花括号初始化：`json::array({ {{"role","system"},{"content",…}} })`（`NormalJsonTranslator.Batch.cpp:135`），
`json{ … }`、`json result = { {"file",…}, {"lines", json::array()} }`（`NormalJsonTranslator.TransAgent.cpp:437, 495, 526, 536, 608,
620, 836`）。json 通过 `3rdParty/3rdModule/json.ixx` 以模块提供，只导出 `basic_json`、`json`、`ordered_json` 等少数名字，
不导出 `detail::json_ref`。两个方向：(a) 与 G-1 同源——json 模块的全局模块片段包含 `nlohmann/json.hpp`，
它在 `__cpp_lib_ranges` 下会用 `<ranges>`，而 std 模块缺了 ranges；(b) clang 对模块里 `initializer_list<json_ref>` 构造的误诊断，
与 mcppls 无关，要进 issue #24 的上游登记。G-1 修好后先复测：若消失即 (a)。
第二次探针（真实模型）里仍没有观察到：`TransAgent.cpp`、`Batch.cpp` 始终没拿到 clangd 的诊断（被放到一边或由 mcppls 自己的引擎回答），
所以这几行 clangd 会说什么仍未知。倾向于 (a)——这些文件都经 `ConditionTool` / json 模块走进了 G-1 的连锁——但没有直接证据；
向报告人要一张报错截图或诊断包能最快定下来。

### 3.4 大项目拿不到 mcpp 的模型（G-4，CI 观察 + 代码确认）

第一次探针（run 36601166906）里，`mcpp emit build-database` 在 4 核 Windows runner 上要 **58–79 s**；而离线时 producer 的
硬期限是 60 s（`src/orchestrator/workspace.cpp:1065-1068`，设置 `producerTimeout` 默认 0 即 60 s）。超过就被杀，会话一直停在
扫描出来的模型（L4：没有项目的 include 目录和宏，`toml.hpp` 之类找不到，报错满屏，clangd 在这种命令下也更容易崩——旧探针
run 36328652984 的 11 次崩溃就发生在扫描模型上）；producer 的模型从没成功过，模型缓存也就从不写入，**每次会话都重来一遍**，
退避重试还会每次再烧 60 s CPU。第二次探针（run 36616368503，`--producer-timeout 600`）里 emit 用了 53.6 s——同一台 runner 上 53–79 s 不等，正好跨在 60 s 两边，
所以用户那边是否拿到真实模型取决于机器和当时的负载；拿到了就踩 G-1，拿不到就踩扫描模型的那一套（§3.5 第一次探针）。

修法 G-4（P0）：
- 硬期限不再是常数：按该项目上次成功 emit 的耗时（存在模型缓存旁）取 `max(60 s, 3 × 上次)`；没有历史时，producer 仍在产出
  （stderr 有输出、CPU 在走）就不杀，直到 10 分钟的绝对上限。扫描模型照常先顶上（BD5 的 2.5 s 不变），只是不再放弃真实模型。
- 被杀时写 `producer-timeout` 问题并说出耗时与期限，给"在终端运行"与"放宽期限"两个动作，而不是只在日志里留一句。
- 向 mcpp 报 emit 慢（M-*）：58–79 s 花在哪（工作区解析、插件、vcpkg 查询）需要 mcpp 那边给出分段计时。

### 3.5 "写半分钟就死一次"与"只能删缓存"（G-3，第一次探针实测，模型为扫描 / 推断）

第一次探针的会话**都没拿到 mcpp 的模型**：一开始工作区未受信任（`untrusted-workspace`，只扫描源文件），受信任后 emit 又在 60 s 被杀
（G-4），于是全程是推断模型——先是 kit（`x86_64-w64-mingw32` + libc++），60 s 后换成 MSVC 工具链的推断模型，缺项目的 include
目录（`pybind11/stl.h`、`spdlog/spdlog.h`、`unicode/unistr.h` 找不到，模块依赖扫描失败 474–610 次）。所以下面的数字说明的是
**机制**，不是用户那一次的原样；真实模型下的结果见 §3.6（第二次探针，run 36616368503）。

E-typing（在 `Run.cpp` 里 10 Hz 打字 238 s，每 10 s 一次补全、偶尔 hover）：

| | 无自动保存 | 1 s 自动保存 |
|---|---|---|
| clangd 崩溃（`0x80000003`，"Build AST"） | 9 次（约每 26 s 一次） | 9 次 |
| clangd 重启 | 9–10 | 12–13 |
| 状态累计（loading / degraded / preparing / error） | 63 / 135 / 36 / 3 s | 63 / 123 / 48 / 4 s |
| 补全 p50 / p95 / max | 0.0 / 3.9 / 4.6 s | 0.0 / 4.6 / 5.3 s |
| hover p50 / max | 16.3 / 23.9 s | 21.4 / 23.9 s |

- **崩溃**：clangd 23.1.0 在构建 `NormalJsonTranslator.Run.cpp`、`TransAgent.cpp`、`TransAgent.ixx` 的 AST 时崩溃，异常码
  `0x80000003`；换成 MSVC 推断模型（`--target=x86_64-pc-windows-msvc`）之后照样崩（18:16:52、18:17:01、18:17:44、18:18:01、
  18:19:21 各一次）。崩溃发生在命令缺 include 目录、扫描失败的文件上；第二次探针（真实模型）里一次也没崩（§3.6）。这是 clangd 自己的缺陷，
  要带最小复现进 issue #24 的上游登记（Linux 上扇出编辑也能触发，§6.3 S5）。
- **数据库来回翻转（R-6，代码确认，新发现）**：clangd 报 `module AbslContainers could not be built: Don't get the module unit for
  module AbslContainers` → mcppls 给它一个空的替身、把 `abslcontainers.ixx` 移出数据库（"1 providers moved" → 2 s 后重启 clangd）→
  同一秒又 "trying module AbslContainers again"，把单元加回来（又一次 "providers moved"）。18:16:04 与 18:17:45 各来一轮。
  原因在 `forget_changed_unresolved_`（`clangd.cpp:2138-2159`）：F13 的修正把"当前提供者是替身"当成空，但记录下来的提供者是
  真实单元 `abslcontainers.ixx`，空 ≠ 它，于是判为"变了"、立刻重试。F13 只覆盖了"根本没有提供者"（hello 里敲 `import h`）的情形，
  **有真实单元、只是 clangd 扫描不到它**的模块每轮都会来回翻，每次翻转都是一次计划原因的重启。
- **放弃 clangd（K-2，代码确认）**：5 分钟内崩溃 5 次，clangd 被标为不可用，**不再安排重启**（`clangd.cpp:1966-1969`），状态为
  error，直到用户手动重启——这与 RD6"退避而不拒绝"的原则相悖（崩溃原因的计数在 guard 的预算之外）。约每 26 s 崩一次，两分钟多就到
  这个上限。E-warm 的五个场景在 t = 240 s 时状态都已经是 error。
- **"只能删缓存"在这次探针里能说明什么**：E-warm 的五个场景在 t = 240 s 时状态都已是 error（K-2 已放弃 clangd），缓存里只有
  2 个 `.pcm`，预建根本没开始——所以 `.lock` 假设**没有被检验到**（始终没有 `.lock` 文件，也没有正在进行的构建可杀）。能说明的是：
  同一会话里 clangd 被放弃后一直停着（230 s，删不存在的锁后又 151 s）；而杀服务端、杀进程树、正常退出之后**不删任何东西只重启**
  都恢复了（108–129 s 到 degraded/ready）。在推断模型下，用户"删缓存 + 重启"起作用的是重启（清掉 K-2 的放弃状态和崩溃计数）；
  真实模型下确实有缓存本身的原因：过期模块锁（§3.6，C-4）。
- **看门狗的其它路径没有触发**：两次打字会话里请求超时、文件隔离、`engine-stalled` 都是 0——这一轮的"死"全部来自崩溃 + 放弃，
  以及 R-6 的翻转重启（AbslContainers 每 11–13 s 一轮）。准备中的请求延迟 1–30 s，两次碰到 30.0 s 的 `INTERACTIVE_LIMIT` 上限；
  空闲时补全 / hover / semanticTokens 都是 0.0 s 量级（由 mcppls 自己的引擎回答）。参照构建（冷缓存 `mcpp build --workspace`）用了
  4085 s。
- **"每 30 s"**：与崩溃间隔（约 26 s）吻合；另外 hover 的 16–24 s 接近 `INTERACTIVE_LIMIT` 30 s，是 worker 被占、请求排队的表现（§4.2）。

修法：
- **R-6（P0）**：`forget_changed_unresolved_` 把"提供者被替身取代"视为未变；只在真实单元的时间戳或命令变化、或它的扫描失败消失时重试。
  更根本的是：clangd 说"找不到模块的单元"而计划里明明有这个单元时，原因是 clangd 扫描不了它（缺 include），替身只会让它导出的
  一切消失、连锁报错；此时应把这个单元的扫描失败作为问题报出来，而不是换替身。
- **K-2（P0）**：崩溃也按 RD6 退避（1、2、4、8 分钟）而不是放弃；崩溃总在不同文件上时（不是单个文件的问题），把状态说成
  "clangd 在这个项目上反复崩溃"并附最近的崩溃上下文，给"重启 clangd"与"导出诊断包"两个动作。
- **C-4 升为 P0**：这次探针没有构建可杀，锁的假设没检验到；第二次探针（§3.6）在 Windows 上复现了，Linux 实测（§6.3 S6）也复现了。
- G-4（真实模型能及时拿到）和 G-1 修好之后，这个项目上的命令不再缺 include，崩溃的触发条件本身就会少很多；两者都是先决条件。

### 3.6 真实模型下的"慢、死、一直 preparing"（G-3，第二次探针，已验证）

第二次探针的会话都拿到了 mcpp 的模型；**clangd 一次也没崩**。用户描述的现象换了一种样子出现，而且每一种都能落到具体代码上：

- **慢：两个 worker 被饿死。** 4 核 runner 上 `-j=2`、预建上限 1。冷启动预建到稳定用了 **1864 s（31 分钟）**，最后 degraded 25/27
  （G-1 的连锁）。预建期间补全 p95 10.0 s、max 19.8 s，hover max 30 s，semanticTokens p95 / max 60 s（请求期限）。打字实验里补全：

  | 会话 | 补全次数 | 超时 | p50 | p95 | max |
  |---|---|---|---|---|---|
  | 无自动保存 | 16 | 12 | 29.9 s | 34.1 s | 34.1 s |
  | 1 s 自动保存 | 24 | 3 | 17.7 s | 52.7 s | 54.9 s |

  补全要等将近 30 s 才回来（`INTERACTIVE_LIMIT` 到点后由 mcppls 自己回答），这就是"补全提示延迟很慢、半不可用"。空闲时所有请求 0.0 s 量级。
- **"死"：重启与放到一边。** 两次打字会话分别重启 clangd 3 次、6 次：
  - **#30 链条**（W-3）：每个热启动的会话里，producer 确认"loaded again: unchanged"后第一次重新计划，**182 个单元全部换了命令**
    （`std.compat.ixx: argument 1: -std=c++23 -> -ID:\a\GalTranslPP\…`，缓存的结构化顺序 → producer 的原始顺序），clangd 以 plan 原因重启、
    BMI 全部作废（19:58:44、20:01:27 各一次）。
  - **K-4**：自动保存时"still building NormalJsonTranslator.Run.cpp 1500 ms after its text on disk was found to spin it: restarting clangd"。
  - **放到一边**："clangd kept working on TransAgent.cpp after it was set aside"而重启；`Run.cpp`、`TransAgent.cpp` 两次因"clangd published
    no diagnostics for it in two minutes"被放到一边——而当时 clangd 给它们的状态是 **"file is queued"**，即还在等 worker，根本没开始建。
    首诊断守卫把"排队"当成了"卡住"（K-5 从 P2 升为 P1）。
- **一直 preparing、只能删缓存：过期模块锁（C-4），Windows 上已复现。** E-warm 的 kill-clangd：预建到 12/25 时 `taskkill /F` clangd，
  服务端自己起了新的 clangd，但 `.locks/9081AFCD4C6A1B5C.lock` 留了下来，新 clangd 一直等它——**480 s 停在 12/25**；只删这个锁文件，
  145 s 到 ready。与 Linux 不同，这里锁的主人已经死了 clangd 也等（Linux 上主人已死会被正确破锁，§6.3 S6），说明 Windows 上 LLVM
  判断不了锁主人的死活。
  **mcppls 自己的每一次重启都在制造这种锁**：重启时关掉 clangd 的输入后只等 500 ms，就 terminate，2 s 后 kill（`src/lsp/connection.cpp:84-97`），
  而 Windows 上 terminate 就是 `TerminateProcess`（openkal-windows 0.10.1 `src/process.cpp:573-576`）。正在建模块的 clangd 500 ms 内不会退出，
  于是被硬杀、锁留下、下一个 clangd 永远等。上面那些重启（#30 每个会话一次、K-4、放到一边后的 recovery、崩溃后的 crash 重启）每一次都可能
  把会话卡死在 preparing，直到有人删缓存。这就是报告人"log 一直 preparing，得亲自删缓存"的完整链条。
  （kill-server / kill-tree / graceful 三个场景里锁在再次启动前消失了，下一次启动 92–258 s ready——服务端整棵进程树退出时锁被带走；
  只有"服务端还活着、只重启 clangd"这种 mcppls 最常做的操作会留下锁。）

修法（补充 §4 / §5）：
- **C-4（P0，已验证）**：启动 clangd 前清空 `.locks/`；运行中解析"Still waiting for module lock"；**mcppls 硬杀 clangd 之后**立即删掉
  被杀的 pid 名下的锁（锁文件内容就是"主机名 pid"）。
- **C-6（P0）重启先请 clangd 自己退出**：发 LSP `shutdown` / `exit` 并给足时间（例如 5 s，建模块时 clangd 会在当前构建结束后退出），
  超时才 terminate；terminate 之后执行 C-4 的清锁。
- **K-5（P1）**：首诊断守卫在 clangd 报"file is queued"（等 worker）时不计时。
- R-1、R-2 是"慢"的根治：4 核机器上交互请求必须有自己的 worker，预建上限与 worker 数要按 §6.3 重新定。

## 4. 调度与看门狗（R-*、K-*）

### 4.1 现状（代码确认）

- **clangd 的 worker 数**：`engine_workers = max(2, (硬件线程 / 2) / 4)`（`guard.cpp:289-293`，macOS 不除 2）。
  8 线程 → 2，16 线程 → 2，32 线程 → 4。`-j` 同时限制 clangd 的前台构建（每个打开的文件、每个 prime 单元都是一个
  ASTWorker，共享这个信号量）和后台索引线程池。#32 的诊断包里就是 `-j=2`。
- **预建并发**：`preparation_limit = max(1, workers / 2)`，有文件在等别的东西时 `workers / 4`（`guard.cpp:295-299`）。
  笔记本上**一次只预建 1 个模块**（诊断包：`"preparation": {"limit": 1}`）。
- **N-7（WA-CLANGD-008）**：为了索引，在 clangd 前台打开实现单元，固定 `IMPLEMENTATIONS_AT_ONCE = 2`（`clangd.cpp:311`），
  每打开一个文件立即排入最多 16 个相关单元（`:312`），clangd 空闲 15 s 后排入**全部**实现单元（`:3383-3395`）。只在"预建进行中"
  时不开新的（`:3367`），一旦开始，用户开始打字也不让路；clangd 不能打断一个正在进行的构建。每个实现单元的前台构建都会
  先建它 import 的模块——所以它实际上在后台把整个项目的模块串行建了一遍。
- **交互请求的期限**：clangd 自己 10 s（`INTERACTIVE_TIMEOUT`），总共 30 s（`INTERACTIVE_LIMIT`，`clangd.cppm:46-52`）。
  超时后：文件还没出过诊断且预建有进展时再等（`keep_waiting`，`clangd.cpp:60-63`）；否则交给 Quarantine
  （`guard.cpp:70-92`，调用处 `clangd.cpp:1163-1167`）：
  - 超时期间 clangd 回答过别的请求：同一文件两次超时就隔离 2 分钟（此后翻倍，最多 16 分钟），期间只由 mcppls 自己的引擎回答
    （关键字、模块名，没有 C++ 语义补全）。文件自己 10 s 内改过（`SELF_EDIT_GRACE`，即正在打字）或它 import 的模块刚变过，
    不计入隔离；
  - 超时期间 clangd **谁也没回答**：一分钟内有两个不同文件这样超时就判 stalled、重启 clangd。这一判断在"正在打字"的豁免
    **之前**（`guard.cpp:83-86`），打字时也会触发——只要另一个打开的文件（旁边的编辑器、大纲视图的 documentSymbol）也在等。
- **卡死检测**：StuckWatch 在"请求 3 s 没回、之后 5 s CPU 低于 5% 核"时判卡死并重启（`clangd.cppm:30-31`）。但
  `cpu_seconds` 只在 Linux 读 `/proc`、macOS 调 `ps`，**Windows 返回空**（`modules/platform/src/process.cpp:520-549`），
  StuckWatch 在 Windows 上从不工作；只开一个文件时 Quarantine 的"谁也不回答"也凑不够两个文件。
- **冷启动过渡模型**：没有缓存时 producer 有 2.5 s（`FIRST_MODEL_WAIT`，`workspace.cpp:319`），之后按扫描结果（L4）服务并
  开始预建；producer 的模型到了以 `user` 原因重启 clangd（`clangd.cpp:748`），过渡模型下建的 BMI 命令不同，全部作废。

### 4.2 这些机制合在一起的效果（§3.6、§6.3 已验证其中大部分）

在一台 8 线程的 Windows 笔记本上打开 GalTranslPP 的一个实现单元：2 个 worker，1 个在预建（每个模块的全局模块片段带
Qt / pybind11 / absl / toml，单个 BMI 就要几十秒），另一个是用户的文件；预建结束后 N-7 立刻开 2 个重量级实现单元，
两个 worker 都被占住（用户停下来想 15 s，N-7 就会排入全部实现单元）。之后：

- 用户在打字：补全要等其中一个构建结束，10 s 后回退成关键字补全；文件不会被隔离，但只要另一个打开的文件的请求也在这一分钟内
  超时，clangd 就被判 stalled 而重启，重启后预建从头开始，worker 又被占满；
- 用户停下来看代码（hover、跳转）：10 s 超时两次，文件被隔离 2 分钟，这段时间补全只剩关键字。

实测（§3.6）里更常见的是另一条路：排队等 worker 的文件两分钟没出诊断，被首诊断守卫放到一边（K-5），再因"clangd kept working on
… after it was set aside"而重启。

对用户来说两种都是"写不到半分钟就死一次"。热启动再叠加 §2.2：第一次重新计划后 clangd 重启、primer 以为都建好了、
用户的文件串行重建全部模块，状态长时间停在 preparing。

### 4.3 修法：用户体验优先的调度（D3）

**目标**：用户感觉快、无感。落成四条可测的规则（每条都有 §6.4 的预算卡着）：

1. **用户在等的东西最先。** 用户正在看、正在改的文件，和它依赖的模块，排在所有后台工作前面；用户发出的请求永远有一个 worker。
2. **后台工作只用空闲时间。** 为索引构建实现单元、预建与打开文件无关的模块，只在用户停手之后做，用户一动就让路。
3. **先快后准。** 请求在它的等待预算内拿不到 clangd 的回答，就先给 mcppls 能给的（标明不完整，编辑器会再问），而不是让人干等 30 s。
4. **看不见的维护。** 重启、模型切换、数据库更新尽量在用户空闲时做，状态栏不闪、不吓人；真出了问题才说，而且说清楚能做什么。

**R-1（P0）统一的后台预算与优先级。** 把 primer（预建）、N-7（索引用的实现单元构建）、定义搜索放进一个调度器，共用
`background = workers − 1` 个名额（给用户的文件和请求永远留 1 个 worker），按优先级分配：

| 级 | 工作 | 条件 |
|---|---|---|
| 0 | 用户打开的文件本身（clangd 自己排） | 永远 |
| 1 | 打开文件的依赖闭包的预建 | 有文件在等它时可以用满 `workers`——此时用户等的就是它，没有别的要让 |
| 2 | 定义搜索（用户点了跳转，clangd 只给出声明） | 用户发起，限时 |
| 3 | 与打开文件无关的预建、N-7 | 只在输入空闲 ≥ 10 s 且没有未回答的请求时；一次 1 个；有 didChange / 请求立即停止开新的 |

**R-2（P0）worker 数。** 按线程数而不是"物理核的四分之一"：`workers = clamp(硬件线程 − 1, 2, 8)`，再按内存封顶
`≤ 可用内存 GB / 2`（一个并发的模块构建峰值约 1–2 GB，§6.3 里 `-j4` 冷启动 6.8 GB）。4 线程 → 3（后台 2），8 线程 → 7，
16 线程以上 → 8。依据：§6.3 里瓶颈是预建上限而不是 `-j`，4 线程时上限 1 让预建慢到 131 s；给交互留 1 个 worker 之后，后台并发 2–3
预计把 4 线程机器的冷启动预建降到 60–70 s。公式的最终常数以 §6.4 的 U1、U4、U14（响应、内存）实测定稿；设置 `mcppls.engine.workers`
（`auto` 或数字，进 `src/config/settings.cppm` 的注册表，RD10）。修订 C7：C7 测到的"首个 hover 从 13.5 s 到 7.5 s"来自减少争用，
R-1 的保留 worker 与 R-7 的回退保住了这个收益，不再需要靠把 `-j` 压到四分之一。

**R-5（P0）N-7 只用空闲时间。** 打开文件时不再立刻排 16 个实现单元；它们进 R-1 的第 3 级：空闲 ≥ 10 s 才开始，一次 1 个，
用户一动就暂停（正在建的那个建完为止，不再开新的）。大项目也做全量，只是更慢地、在空闲里做完。代价与兜底：还没进索引的实现单元，
跳转时由已有的按需定义搜索（`search_definition_`，N-8 的按名字选单元）打开，用户点一次跳转仍然到实现，只是第一次慢 1–3 s。

**R-7（P0）按方法分级的等待预算。** 取代统一的 10 s / 30 s（`clangd.cppm:46-52`）：

| 请求 | clangd 的等待预算 | 到点之后 |
|---|---|---|
| 补全 | 1 s | 先回 mcppls 的结果（关键字、模块名、本文件与索引里的标识符），`isIncomplete: true`，编辑器下一次按键会再问；取消这次 clangd 请求 |
| signatureHelp | 1 s | 空结果 |
| hover | 2 s | mcppls 的模块级 hover，没有则空 |
| 定义 / 声明 | 3 s | mcppls 的模块级跳转或词法定位（N-8）；都没有才继续等，最多 10 s |
| semanticTokens、documentSymbol、folding | 不等 | 立即给 mcppls 的结果，clangd 的就绪后发 `workspace/semanticTokens/refresh` |
| references、rename、callHierarchy | 60 s | 用户主动发起的长操作，带 `$/progress` |

keep_waiting（文件在等预建时延长等待）只保留给定义这一类；补全永远不因为预建而多等。

**R-8（P1）空闲时重启。** plan 与 recovery 原因的重启不再立即执行：等输入空闲 ≥ 3 s，最多推迟 60 s；重启走 C-6 的体面退出；
重启期间的请求按 R-7 回退作答，状态栏只显示"正在重新加载 clangd（原因）"这类 info，不显示 warning。crash 原因的重启照旧立即做。

**R-9（P1）后台构建不抢前台。** openkal 给 `kal_spawn` 加一个"低于正常优先级"的选项（POSIX 在子进程里 `setpriority`，
Windows 用 `BELOW_NORMAL_PRIORITY_CLASS`），clangd 以它启动，编辑器与用户自己的程序在 clangd 满负荷时仍然流畅。风险是机器被别的程序
占满时 clangd 也变慢；以 U4（后台工作中的请求延迟）与 U1（冷启动）的实测决定是否默认打开，设置 `mcppls.engine.lowPriority`。

**R-3（P1）自己造成的等待不算 clangd 的错。** 超时时若有 R-1 调度的后台单元在构建，按 `rebuilding` 处理（计入"谁都不回答"，
不计入隔离）；`request-timeout` 事件加 `busyWorkers`、`backgroundBuilding`，让 incident 能说清是谁占着 worker。

**R-4（P1）过渡模型不预建。** 没有缓存、producer 还没回答时，只做 mcppls 自己的语法级服务和 std 的预建，不预建项目模块；producer 的
模型到了再开始（xlings 的 producer 约 5 s，§6.3）。R-8 让随之而来的切换发生在空闲时。

**K-6（P1）状态不闪。** 问题持续 30 s 才从 ready 变 degraded（崩溃、放弃这类立即变）；preparing 显示"N/M 个模块，正在建 X"，
等在锁上、等在 producer 上各有一句话；一分钟内状态变化超过 6 次时合并成一条。

**K-2（P0）崩溃不再导致放弃**：见 §3.5。Linux 上扇出保存 `mcpp.manifest.types` 5 / 5 次触发崩溃循环，说明这不是 Windows 或推断模型
独有的；放弃之后会话一直是 error（15 分钟以上），这是 0.0.6 里"死掉"最直接的原因。

**K-3（P1）崩溃可符号化**：发布时保留 clangd 的符号文件（不进 payload，放在 release 资产里），incident 记下模块偏移，
`mcppls-devtools` 离线符号化；拿 `mcpp.manifest.types` 的扇出崩溃做最小复现，进 issue #24 的上游登记。

**K-4（P1）磁盘安全检查的重启**：`DISK_SETTLE` 1.5 s 改为 `max(1.5 s, 3 × 该文件上次构建耗时)`；由此引起的重启走 RestartGate 的
recovery 预算，并按 R-8 在空闲时做。

**K-5（P1）首诊断守卫**：clangd 报 "file is queued"（等 worker）时不计时；CPU 饥饿时"两分钟无诊断且没在预建"会误判，改为同时要求
clangd 在这段时间里 CPU 几乎没动（K-1 之后三个平台都能读）。

**K-1（P1）Windows 的 CPU 读数。** `GetProcessTimes`：经 openkal-windows 增加 `kal_process_times`（与 X-1、R-9 同一次 openkal 发版），
平台层的 `cpu_seconds` 用它。之后 StuckWatch 在三个平台行为一致。

## 5. 缓存：一键恢复、容量、自愈（C-*）

- **C-1（P0）** VS Code 命令"mcppls: 重置本工作区缓存"：停服务端 → 删除该工作区缓存（等同 `mcppls cache --clean <名字>`）→
  重启；Zed / CLion 插件同名命令。今天 CLI 有 `cache --clean`，但必须先停掉正在用这个缓存的服务端，编辑器里没有入口，
  用户只能手工删目录。
- **C-2（P1）GC**：每个单元只保留当前数据库命令对应的 BMI 目录和最近 N 个；每个工作区一个容量上限（默认 2–4 GB，设置可调），
  跨工作区按最近使用淘汰；启动后在后台线程执行，`mcppls cache --prune` 手动执行。
- **C-3（P1）自愈**：同一工作区连续 N 个会话出现同类故障（重启预算用尽、preparation-stalled、crash 在同一模块），
  下一次启动先把 clangd 的模块缓存移到一旁（保留一份给 incident），从干净状态开始一次，并在状态里说明。
- **C-4（P0）过期的模块锁**（Windows §3.6、Linux §6.3 S6 都已复现）：clangd 23.1 的锁在 `.cache/clangd/modules/.locks/<hash>.lock`（符号链接）与
  `<hash>.lock-<随机>`（内容"主机名 pid"）。mcppls 的做法：
  - 启动 clangd 之前，删掉 `.locks/` 下所有锁——这个缓存目录只属于持有租约的这个实例（C-5 之后租约可靠），此刻没有任何 clangd 在用它；
  - 运行中解析 clangd 日志的"Still waiting for module lock <path> after Ns"：锁的主人不是当前 clangd 的 pid，就删掉它并记 incident；
    是当前 clangd 自己（它自己在建）则不动；
  - `module_already_built_` 与 incident 里的"锁"改看 `.locks/`，不再看 `<pcm>.lock`（`clangd.cpp:3604`）。
- **C-5（P0）租约**：租约写 pid 与进程启动时间；读到的租约若持有者进程已不在（或 pid 被别的程序复用、启动时间不符），立即接管主目录，
  不必等 30 s 过期。第二实例的私有目录（`instances/<token>`）退出时若主目录空闲，把新建的 BMI 合并回去或至少不再每次冷启动。
- 被杀时的半截 `.pcm`：§6.3 S6 里截断、清零、写垃圾的 `.pcm` 都被 clangd 检出并重建，这一项不需要 mcppls 处理。

## 6. 按场景的性能与稳定性（U-*）

### 6.1 原则

1. **交互优先，且有预算。** 用户在等的请求永远有一个 worker；后台工作（预建、索引、定义搜索）只用空闲时间、有活动就让路（R-1、R-5）；
   拿不到 clangd 的回答就先给 mcppls 能给的，不让人干等（R-7）。
2. **引擎命令是模型的纯函数。** 同一项目、同一设置，无论模型来自 producer、缓存还是重新计划，clangd 拿到的命令逐字节相同
   （W-1..W-5）。命令变化必须有原因，并被记录。
3. **看门狗不能把自己造成的慢当成 clangd 的错**（R-3），并且在三个平台上行为一致（K-1）。
4. **持久状态有上限、可校验、能自愈**（C-*）。坏缓存不能让用户只剩"手工删目录"这一条路。
5. **每个优化都要有场景和数字**：以 mcpp 与 xlings 为测试项目（D5），先量 0.0.6 的基线，改完再量；U1–U15 的预算是合入与发布的门槛（§6.4）。

### 6.2 场景矩阵

| 维度 | 取值 |
|---|---|
| 项目规模 | S：hello / `timing`（≤ 10 模块）；M：mcpp（176 模块）、xlings、本仓库；L：GalTranslPP（重型 GMF + Qt + MSVC STL）；XL：合成的 500 模块深 / 宽图 |
| 机器 | 2 核 CI；4 核 8 线程笔记本（`taskset` 模拟）；8 核 16 线程；32 线程工作站；Windows / macOS / Linux x64 / linux-arm64；proot / 容器 / Flatpak |
| 缓存状态 | 冷；热；升级 mcppls 后的热；上次会话被杀后的热；人为损坏（截断 `.pcm`、错版本的模型缓存） |
| 操作 | 打开；打字（10–20 Hz，含半个标识符、未闭合括号、半个 `import`）；补全 / hover / 跳转 / semanticTokens；保存被广泛 import 的接口（扇出）；`git checkout` 改 100 个文件；改 `mcpp.toml`；切 profile / context；杀 clangd；杀服务端；两个窗口同一工作区；producer 挂起 |

每个场景量：首次诊断、首次补全、首次跨模块跳转、请求 p50 / p95 / max、超时与回退次数、clangd 重启与原因、被隔离文件数、
preparing 的持续时间、新增 `.pcm` 数、clangd 峰值 RSS 与 CPU。

### 6.3 Linux 基线（实测）

mcppls 0.0.6 发布版 payload，本机 x86_64 32 线程；项目为 mcpp（180 个待预建模块）和 xlings（约 110 个）的副本；每个场景记录负载，
负载 60 / 250 的两次早期冷启动只作证据不作数据。`taskset` 下 `hardware_concurrency` 跟随亲和掩码，`-j` 与预建上限随之变化，
可以模拟笔记本（只模拟线程数，不模拟内存带宽）。驱动脚本在 scratchpad `e-perf/harness/`（`startup.py`、`s3_latency.py`、
`s4_typing.py`、`s5_fanout.py`、`s6_crash.py`、`s6b_cache.py`、`s7_graph.py`），是 §6.5 在运行器里实现场景测试时的参照（逻辑照搬，不把脚本放进仓库）。

**S1 / S2 / S8：启动**（秒，自 initialize 起；"预建完"= preparing 回到 ready；"首个正确跳转"= 第一次跨模块跳转拿到正确结果）

| 运行 | 负载 | 预建完 | 首个正确跳转 | 首个 clangd 诊断 | 新建 BMI | clangd 峰值 RSS |
|---|---|---|---|---|---|---|
| 冷，默认（32 线程：`-j4`，上限 2） | 3.4 | 49.4 | 36.8 | 26.5 | 185 | 6.8 GB |
| xlings 冷 | 9.4 | 44.1 | 27.5 | 21.7 | 131 | 5.6 GB |
| 热，冷启动后第一次（#30） | 5.7 | 35.2 | 35.1 | 26.4 | **185** | 1.9 GB |
| 热，第二次（复用） | 6.1 | **2.8** | **2.7** | 2.6 | 0 | 1.7 GB |
| xlings 热 1（#30）/ 热 2 | 8.8 / 6.3 | 11.3 / 7.5 | 17.5 / 10.5 | 11.2 / 7.3 | **140** / 0 | 2.8 / 1.9 GB |
| `-j=16` / `-j=32`（`MCPPLS_ENGINE_ARGUMENTS`） | 5.5 / 5.6 | 51.7 / 59.0 | 37.9 / 45.3 | 29.6 / 35.9 | 185 | 8.3 / 9.8 GB |
| `taskset 0-15`（`-j2`，上限 1） | 1.2 | **110.3** | 37.9 | 28.7 | 185 | 3.9 GB |
| `taskset 0-7`（模拟 8 线程笔记本） | 7.6 | **116.1** | 41.3 | 30.2 | 185 | 4.1 GB |
| `taskset 0-3` | 6.5 | **131.5** | 54.0 | 40.1 | 185 | 3.5 GB |
| `taskset 0-7` 热（#30 重建） | 7.4 | 39.2 | 39.2 | 30.1 | 185 | 2.1 GB |

- **瓶颈是预建上限，不是 `-j`**：`-j` 调到 16 / 32 没有收益（上限仍是 2），模拟笔记本（上限 1）预建时间翻倍以上。默认预建平均只用
  5.2 个核（共 32）。预建上限目前不能配置，所以"上限 3–4 会怎样"还没测到——R-2 需要先加开关再定公式。
- **#30 的代价**：mcpp 热启动 185 个 BMI 全部重建（35 s vs 复用时 2.8 s），xlings 140 个（11.3 s vs 7.5 s）。
- **首次请求**：冷启动的第一次跨模块跳转总是等满 30 s 上限后由 mcppls 自己回答；模拟笔记本上首次补全 11.8–16.9 s，超过 10 s 回退。
  xlings 的 producer 约 5 s，超过 2.5 s 的首个模型等待，clangd 先按过渡模型起、真实模型到了再重启（R-4 的实例）。
- **高负载冷启动（负载 25→87）**：约 140 s 没有建出一个模块，两个打开的文件被放到一边，clangd 被重启（"two minutes, and no module was
  being prepared"）——CPU 饥饿下首诊断守卫的误判（K-5）。

**S3：请求延迟**（秒，p50 / p95 / max；测试文件只 import std，依赖早已建好）

| 配置、阶段 | 补全（成员 / `std::` / 函数体） | hover | 最坏一次 |
|---|---|---|---|
| 默认，预建中 | .04/.06/.06 · .11/.12/.12 · .04/.36/.36 | .15/.32/.32 | 0.36 |
| 默认，空闲 | .04/.05/.05 · .10/.11/.11 · .03/.04/.04 | .001 | 0.11 |
| `taskset 0-7`，预建中 | .04/.10/**11.8** · .15/.79/**10.0** · .05/.30/6.7 | .30/.85/1.3 | 11.8 |
| `taskset 0-3`，预建中 | .04/.10/**16.8** · .11/.62/**10.0** · .03/.41/10.0 | .15/.60/5.8 | 16.8 |

- 默认配置下，依赖已建好的文件的补全**不会**排在预建后面；只有 2 个 worker 时，前 4 个请求等了 5.8–16.8 s（两次 10 s 超时），
  尽管 std 在 5 s 时就建好了——两个 worker 被另外两个打开文件的串行模块构建占着（推断，未用 trace 证实）。这就是 R-1 要解决的。
- `ready` 之后约 45 s 请求变慢（N-7 在构建实现单元）：默认最坏 2.3 s，模拟笔记本上成员补全 6.1 s、documentSymbol 3.1 s（R-5）。
- 重型文件 `graph.cpp`（50 个 import）的第一次 hover 等了 23–26 s。空闲时所有配置所有请求 ≤ 0.46 s。

**S4：打字风暴**（120 s，约 13 次编辑 / s，每秒一次补全，含半个标识符、未闭合括号、`import mcpp.build.`）

| 运行 | 补全 p50 / p95 / max | 重启与原因 | 放到一边 / 超时 |
|---|---|---|---|
| 默认，热 | .09 / 1.25 / 10.0 | 1 次 plan 重启（**#30：第一次重新计划后 526 个单元里 342 个命令不同**），40 s 后恢复 | 2 次超时 |
| 默认，冷（预建中打字） | .10 / .80 / 2.9 | 无 | 无 |
| 自动保存（每次编辑都写盘保存） | .10 / .66 / 2.3 | 2 次"clangd would not finish graph.cpp" | 12 次磁盘不安全 / 12 次放到一边 |
| `taskset 0-7`，热 | .09 / 1.8 / 10.0 | 1 次 plan 重启（#30） | 2 次超时 |
| `taskset 0-7`，冷 | .10 / 1.5 / 15.8 | 无 | 2 次超时 |
| `taskset 0-3`，冷 | .62 / 10.0 / 10.9 | 无 | 3 次超时 |

状态从没卡在 preparing，没有崩溃，spin / stall / stuck 守卫都没触发。自动保存下的 2 次重启是误判（K-4）：半个 import 写到盘上，
文件被判"磁盘不安全"，`DISK_SETTLE` 1.5 s（`clangd.cpp:243`）后构建还在跑就重启 clangd（`clangd.cpp:2698-2702`），而重型文件正常构建
就超过 1.5 s；这次重启也不经过重启预算。

**S5：扇出编辑**

| 编辑 | 结果 |
|---|---|
| `mcpp.log`（36 个导入者，开 8 个） | 9.6 s 诊断翻转，8 个文件都重新发布，14.6 s 安静；1 个 BMI 重建，无重启 |
| `mcpp.manifest.types`（被大量导入的接口） | **5 / 5 次 clangd 崩溃循环**（默认、`taskset 0-7`、只开一个导入者、关掉 N-7、关掉后台索引都一样）：保存后约 5 s "Signalled during AST worker action: Build AST"，1、2、4 s 后重启，5 分钟内第 5 次崩溃后 **mcppls 放弃 clangd**（K-2），之后 15 分钟以上状态一直是 error；连带 6–16 个文件被放到一边 |

payload 里的 clangd 去掉了符号，崩溃栈无法符号化（偏移在 `e-perf/runs/S5-default/server.log`，如 `#4 0x268b24b`）——K-3。

**S6：崩溃与缓存**

- 预建中 SIGKILL clangd 三次：新 clangd 1.1 / 2.1 / 5.4 s 后起来，期间请求 0.33–0.41 s 由 mcppls 回答，第一次杀后 85.8 s ready（基线约 50 s）。
- 预建 10% / 30% / 60% / 90% 时 SIGKILL 服务端（或连同 clangd）：clangd 不会成为孤儿；同一缓存再启动 52–72 s ready，**从不卡住**；
  没有留下半截或零字节的 `.pcm`。截断、清零、写垃圾的 `.pcm`（包括 std）都被检出并重建（3–4 s ready）。**但每次都是完整冷启动**：
  杀之前建好的 79 个模块全部重建（热启动走 #30 的链条）。
- **过期的模块锁（C-4，已复现"一直 preparing、只能删缓存"）**：clangd 23.1 在 `.cache/clangd/modules/.locks/` 下放
  `<hash>.lock`（符号链接）和内容为"主机名 pid"的 `<hash>.lock-<随机>`。同一主机、pid 已死：clangd 正确地破锁，7 s ready。
  **pid 又被别的进程占用，或主机名不同**：clangd 一直等（"Still waiting for module lock … after 10s/20s/…"，每代最长 260 s），
  预建停在 158/180，约 124 s 后状态变 degraded（`preparation-stalled`、`file-quarantined`），两个打开的文件被放到一边，此后 9 分钟在
  preparing 与 degraded 之间来回，跨模块跳转一直不可用。服务端运行中**只删这两个锁文件**，67–99 s 恢复，不用重启。
  现实中会碰到的情形：容器的主机名每次不同、macOS 改了主机名、重启后 pid 被复用、缓存在网络盘上；Windows 上更糟——主人已死也一直等
  （§3.6），而 mcppls 自己的每次重启都在 Windows 上硬杀 clangd（C-6）。mcppls 查的是 `<pcm>.lock`（`clangd.cpp:3604`），不是 clangd 实际放锁的地方；也没人解析"Still waiting for module lock"。
- **实例租约（C-5，新发现）**：服务端崩溃后 30 s 内再启动（VS Code 的语言客户端会立刻重启崩溃的服务端），旧租约看起来还活着
  （续约 10 s、过期 30 s，`src/orchestrator/instance.cppm:12-13`；租约只有 token 和心跳，没有 pid，`instance.cpp:40-73`），新实例于是
  "another instance serves …; this one uses …/instances/<token>"——用私有目录**冷启动**（66 s，对照 3.6 s），退出时丢掉这些成果。

**S7：构建图变化**：加 / 删一个模块文件，2.8–2.9 s 更新数据库，不重启，不重建；`mcpp.toml` 改 `default-profile = "dev"`：约 2 s 后重启，
185 个 BMI 重建，40.5 s ready，首次补全 10 s 超时（合理：命令确实变了）；改回：7.2 s ready，0 重建（旧 BMI 复用）。

**没测到**：macOS、Windows（Windows 数字来自 §3 的 CI）、真实的 8 线程笔记本、预建上限大于 2、xlings 的 S3–S7。

### 6.4 用户体验场景测试（U-*，D5）：mcpp 与 xlings

**测试项目**：mcpp（`self-mcpp` 的固定 commit `d1f1c98f`，176 个模块）与 xlings（`real-xlings` 的固定 commit `84572b0c`，约 110 个模块，
含构建时生成的模块）。两个项目的准备步骤沿用现有 fixture（`git fetch` 固定 commit + 一次联网的 `mcpp emit build-database`），
新 fixture 为 `ux-mcpp`、`ux-xlings`。

**参照机器**：GitHub `ubuntu-24.04` runner（4 vCPU、16 GB）——相当于一台 2 核 4 线程或 4 核笔记本，也是 §6.3 `taskset -c 0-3` 的档位。
本机复现用 `taskset -c 0-3`。另跑一个 `taskset -c 0-1` 的低配档，只记录不卡门槛。

**延迟与质量一起卡**：R-7 让每个请求都能在预算内回应，所以只卡延迟会被回退"刷"过去；U4、U5 同时要求由 clangd 作答的比例
（report 里每个方法已有 `answeredBy` 统计），延迟快而回退多同样算失败。

**预算从哪来**：以常见的交互阈值为准——约 100 ms 感觉是即时的，约 1 s 内思路不断，超过 10 s 注意力就走了；补全要"感觉快"，
p95 必须在 1 s 内、最坏不超过 3 s。冷启动的预算按 0.0.6 基线定初值，第一轮实现后按实测定稿，**只收紧不放宽**。
预算里的 mcpp / xlings 两个数分别对应两个项目。

| # | 场景 | 指标 | 预算（mcpp / xlings） | 0.0.6 基线（4 线程档，§6.3） |
|---|---|---|---|---|
| U1 | 冷启动：无缓存，打开 `src/main.cpp` 与一个重型实现单元 | 首个响应（documentSymbol、semanticTokens、模块名跳转） | ≤ 1 s | mcppls 自己的引擎，已满足 |
| | | 打开文件的首个 clangd 语义补全 | ≤ 45 s / ≤ 35 s | 首次补全超时回退；首个 clangd 诊断 40 s |
| | | 首个正确的跨模块跳转 | ≤ 45 s / ≤ 35 s | 54 s（且总有一次等满 30 s 回退） |
| | | 预建完成 | ≤ 90 s / ≤ 70 s | 131 s |
| | | 期间任一交互请求的最长等待 | ≤ 3 s（按 R-7 回退作答） | 30 s |
| U2 | 热启动：同一缓存再开 | 进入 ready；首个正确跨模块跳转 | ≤ 5 s；≤ 3 s | 35 s（#30）；复用时 2.8 s |
| | | 新建 BMI；前 5 分钟内 clangd 重启（含第一次重新计划后） | 0；0 | 185 个；1 次 |
| U3 | 空闲时的请求（一切就绪后） | 补全、hover、跳转、semanticTokens 的 p95 | ≤ 300 / 300 / 500 / 500 ms | 0.05–0.11 s，已满足 |
| U4 | 后台工作进行中的请求（预建、N-7 在跑；文件自己的依赖已建好） | 补全 p95 / max；hover p95 | ≤ 1 s / ≤ 3 s；≤ 1.5 s | 补全 max 11.8–16.8 s |
| | | 由 clangd 作答的比例（其余是 R-7 的回退） | ≥ 90% | — |
| U5 | 打字 2 分钟：10 Hz，含半个标识符、未闭合括号、`import mcpp.build.` 这类半个 import，每秒一次补全 | 补全 p95 / max | ≤ 1 s / ≤ 3 s | p95 10 s（4 线程档） |
| | | clangd 重启、放到一边、请求超时 | 0、0、0 | 1 次重启（#30）、若干超时 |
| | | 由 clangd 作答的补全比例 | ≥ 90% | — |
| | | 停手后诊断更新 | ≤ 3 s | — |
| U6 | U5 加自动保存（1 s） | 同 U5；磁盘安全检查引起的重启 | 同 U5；0 | 2 次（K-4） |
| U7 | 扇出保存：改并保存一个被广泛导入的接口（mcpp 用 `mcpp.log`，§6.3 时约 36 个导入者；xlings 取被导入最多的接口；模块名以固定 commit 为准），开 8 个导入者 | 诊断全部更新；期间补全 p95 | ≤ 20 s；≤ 1 s | 14.6 s（32 线程） |
| | 扇出保存：mcpp 的 `mcpp.manifest.types`（§6.3 里会触发 clangd 自己的崩溃；固定 commit 上若不再触发，就用 K-3 的最小复现构造一个） | clangd 从不被放弃；每次崩溃后恢复到能跳转；状态从不 error | ≤ 60 s | 5 次后放弃，error 15 分钟以上 |
| U8 | 构建图变化：加 / 删一个模块文件；改 `mcpp.toml` 的 profile | 数据库更新；不必要的重启 | ≤ 5 s；0（只有命令真变了才重启） | 2.8 s；— |
| U9 | 预建中杀 clangd | 恢复到能跳转；过期锁 | ≤ 15 s；无 | 85.8 s |
| U10 | 杀服务端后立即重启（模拟崩溃后编辑器自动重启） | 进入 ready；用主目录；新建 BMI | ≤ 5 s；是；0 | 66 s、私有目录冷启动 |
| U11 | 注入过期锁：主人 pid 已死 / pid 被复用 / 主机名不同 | 自动恢复 | ≤ 15 s | 后两种永远等 |
| U12 | `git checkout` 到 20 个 commit 之前再回来 | 重新计划；clangd 重启；恢复到能跳转 | ≤ 10 s；≤ 1 次；≤ U1 预算 | 未测 |
| U13 | 就绪后空闲 10 分钟 | clangd 平均 CPU；RSS 增长 | ≤ 1% 单核；≤ 10% | 冷启动后 65–89 s 安静 |
| U14 | 资源 | 冷启动 clangd 峰值 RSS | ≤ 5 GB / ≤ 4 GB | 4 线程档 3.5 GB；32 线程 6.8 GB |
| U15 | 状态（贯穿以上全部场景） | 无进展的 preparing；每分钟状态变化；error 状态 | ≤ 60 s；≤ 6 次；从不 | 锁的场景来回 9 分钟 |

每个场景同时落 `--measure` 测量文件，记录全部指标（包括没有预算的），用来看趋势。

### 6.5 场景测试的实现与在 CI 里的位置

- **在 conformance 运行器里实现**（C++，`src/bin/conformance.cpp`），不另写脚本：仓库的规则是脚本只保留一个（T5）。§6.3 用的
  Python 驱动（scratchpad `e-perf/harness/`）只作参照，照着它的逻辑实现。需要新增的检查类型：
  - `latency`：在给定的文件与位置反复发某类请求，分阶段（冷启动中 / 后台工作中 / 空闲）统计 p50 / p95 / max，按预算判定；
  - `typing`：按给定速率打一段脚本化的文本（含坏代码与半个 import），可选自动保存，期间按节奏发补全，统计延迟、重启、放到一边、超时；
  - `edit-save`：改写并保存一个文件（扇出），等诊断更新并计时；
  - `fault`：杀 clangd、杀服务端（杀完立即重启）、注入过期锁（三种主人）、截断 `.pcm`；
  - `timeline`：贯穿全程的状态时间线检查（U15）与 clangd 重启计数；
  - `bmi-reuse`：`module-cache-reused` 扩到全部模块，报告新建 BMI 数（U2、U10）；
  - `resources`：clangd 的峰值 RSS 与空闲 CPU（U13、U14），平台读不到时不判定（沿用 `stress` 的约定）。
- **fixture**：`conformance/fixtures/ux-mcpp`、`ux-xlings`，各一个 `scenario.json`，U1–U15 是其中的检查；冷启动与热启动用
  `--workspace-dir` / `--cache-dir` 复用（已有机制），预算写在检查里。
- **CI**：
  - 0.0.7 的 PR：`ux-mcpp`、`ux-xlings` 在 ubuntu-24.04 上**必须通过**，作为合入条件（两个 job 并行，各约 30–40 分钟）；
  - nightly：每晚各跑三轮，上传测量文件，看趋势；另跑 `taskset -c 0-1` 低配档与 linux-arm64；
  - pre-release：取代现在"`timing` fixture 的首次跳转中位数"那一条（`docs/92-release.md`），按 §6.4 的预算三轮连过才发布；
  - 普通 PR：跑一个 5 分钟的精简版（U2、U3、U5 的短版，项目用 `timing` 与 `mcpp-split`），不拖慢日常开发。
- **防止计时测试不稳**：每个预算判定取同一轮里的多次采样；nightly 三轮里失败一轮只记录，连续两晚失败才开 issue；CI runner 的负载
  （`/proc/loadavg`）与测量一起记录；延迟预算留出 runner 抖动的余量（初值来自 4 线程档的实测再加 20%）。
- **Windows**：mcpp / xlings 的 Windows 构建不在这次范围内；Windows 上的大项目由 GalTranslPP 探针（§3）覆盖，它的 E-diag / E-typing /
  E-warm 按 §6.4 同样的预算判定，作为 0.0.7 发布前的一次性验收。
- **可观测性**：report 里每个请求加"等 worker / clangd 处理 / 回退"的时间拆分；worker 占用时间线（谁在占：用户文件 / 预建 / N-7 / 搜索）；
  "为什么还在 preparing"的一句话解释。场景测试的失败信息直接引用这些字段。

## 7. Code-OSS 上的可用性测试（O-*）

### 7.1 实测（Linux x64，已验证）

VSCodium 1.135.06055（`vscode.env.appName` = `VSCodium`，`uriScheme` = `vscodium`）对照 Microsoft VS Code 1.139.1，
同一个 `mcppls-linux-x64.vsix`（0.0.6），同一份 E2E 套件（在 scratchpad 的镜像里给 `runTest.js` 打了补丁，见 7.3）：

| 套件 | VS Code 1.139.1 | VSCodium 1.135.0 |
|---|---|---|
| main | 26 通过，1 待定（macOS CLT 提示，Linux 上跳过），工作区未改动 | 相同 |
| conflicts | disable 6 通过，keep-both 3 通过，工作区未改动 | 相同 |
| stress | 1 通过，工作区未改动 | 相同 |

套件之外（经扩展的 `TestApi` 脚本化）：

- 打开 `main.cpp` 即激活，mcppls `ready`，clangd 23.1.0 `starting → ready`；装上的是 `…-0.0.6-linux-x64`，payload 与 kit 正确。
- **Open VSX**：`sunrisepeak.mcpp-language-server` 0.0.6 有 linux-x64 / linux-arm64 / darwin-arm64 / win32-x64 四个平台包；
  `codium --install-extension` 按 id 安装拿到 linux-x64，SHA256 与发布页的 VSIX 相同。扩展页有"not a verified publisher of the
  namespace"的提示条。
- **冲突检测**：装上 Open VSX 上真实的 clangd 扩展 0.6.0，report 把它列为 active；回答"在此工作区禁用"写出
  `.vscode/settings.json` `{"clangd.enable": false}`。cpptools 不在 Open VSX 上，这条路径只有 conflicts 套件里的桩覆盖。
- **诊断包**：`environment.json` 的 `editor` 为 `{"name": "VSCodium", "version": "1.135.0"}`（来自语言客户端的 `clientInfo`）；
  扩展自己那一节（`editors/vscode/src/commands.ts:288`）只记了 `vscode: "1.135.0"`，没有应用名，客户端这一侧分不出是哪个分支。
- `src/` 里没有任何只属于 Microsoft 构建的依赖：没有 proposed API、`extensionDependencies`、遥测或商店链接。

结论：**Code-OSS 本身没有发现 mcppls 的问题**；缺的是持续的测试，以及几个 Code-OSS 用户更常遇到、这里还没覆盖的环境。

### 7.2 没有覆盖到的（按风险排序）

1. **termux / Android 的 `code-oss`（proot 下的 arm64）**：扩展能激活，但任何子进程都起不来（#32，X-1 修好之前）。这是
   Code-OSS 用户里最可能出事的一群。
2. **Flatpak / Snap 打包的 VSCodium / Code - OSS**：沙箱里启动程序、读 `/proc`、访问 `~/.cache` 和工具链目录都可能受限；
   发行版的 `code` 包（Arch）用系统 Electron，行为与官方构建有差异。
3. **远程扩展宿主**（code-server、`vscodium-reh`、Remote-SSH 类）：只读了代码——`extensionKind: ["workspace"]`，payload 按扩展
   宿主所在平台选择，`remoteName` 只用于报告——没有实跑。
4. **Windows / macOS 上的 VSCodium**：没有跑；它们的 CLI 路径与 Microsoft 构建不同（7.3）。

### 7.3 修法

- **O-1（P1）harness 能指定编辑器**：`runTest.ts` 加 `MCPPLS_E2E_EDITOR=<可执行文件>`，设置了就不调用
  `downloadAndUnzipVSCode`。CLI 路径不能用 test-electron 的 `resolveCliArgsFromVSCodeExecutablePath`（写死 `../bin/code`，
  VSCodium 是 `bin/codium`）：Linux 上取 `dirname(exe)/bin/basename(exe)`，macOS / Windows 按各自的应用布局推导。
- **O-2（P1）CI 作业**：ubuntu-24.04 + xvfb，下载并缓存 VSCodium linux-x64，跑 main / conflicts / stress 三个套件；
  之后加 windows 与 macOS 的 VSCodium。
- **O-3（P2）发布后检查 Open VSX**：按 id 安装，版本与 SHA256 必须等于发布页的 VSIX（`publish-openvsx.yml` 之后一步）；
  申请 Open VSX 命名空间认证，去掉"未认证发布者"提示。
- **O-4（P2）诊断包记下编辑器分支**：扩展那一节加 `vscode.env.appName`、`appHost`、`uiKind`、`remoteName`。
- **O-6（P2，用户要求 2026-09-30）扩展简介以名字开头**：VS Code 的 `description` 由 "C++20/23 named modules that just work: … built in. (mcppls)"
  改为 "mcppls - C++20/23 named modules that just work: go to definition, completion, hover and references across modules for any compiler,
  with clangd and a standard library kit built in."；Zed、CLion 的一句话简介同样以 "mcppls - " 开头。商店与 Open VSX 的搜索结果里名字先出现。
- **O-5（P2）环境覆盖**：X-3 的 proot CI 作业同时装 VSIX 跑 main 套件（termux code-oss 的近似）；Flatpak 版 VSCodium 手工验证一次，
  结果写进 `docs/00-install.md` 的已知限制。

## 8. 验证计划

| 条目 | 怎么证明修好了 |
|---|---|
| G-1 | Linux：libstdc++ 下定义其 `<ranges>` 守卫宏的 fixture 变体，`std::views::keys` 可补全、零诊断；Windows：`mcpp-msvc` 类 fixture 加 `_RANGES_`；GalTranslPP 探针 E-diag 中 `Run.cpp` 零 `no_member`，std 的引擎命令里没有项目宏 |
| G-2 | G-1 之后同一探针复测 `Batch.cpp:135`、`TransAgent.cpp:437…` 的诊断；不消失则做最小复现交上游 |
| W-1..W-4 | 失败单测转绿；`engine-database-stable`（冷热数据库逐字节相同）在 `timing`、`mcpp-gcc`、`self-mcpp` 上通过；热启动新增 `.pcm` 为 0、`Built module` 为 0；`self-mcpp` 热启动首次跳转 ≤ 5 s（issue 测得 55–78 s） |
| W-3 | 热启动后编辑 import 触发重新计划，clangd 不重启（`engine-restart-scheduled` 事件为 0） |
| X-1、X-4 | openkal 的 conformance 测试在 seccomp-ENOSYS 过滤器下启动 `/bin/true` 与 `#!` 脚本；X-3 的 CI 作业在 termux-proot 默认与 `PROOT_NO_SECCOMP=1` 两种模式、x64 与 arm64 上，`inferred`、`mcpp-emit`、`timing` 与 VS Code main 套件通过；报告人在真机 termux 上用候选 VSIX 复测 |
| X-5 | proot 作业里文件监视（`mcpp-watch`）通过；report 记下 `sandbox: proot` |
| X-2 | 同一过滤器下 report 为 `engine-start-failed`、environment 带 `sandbox` |
| R-1、R-2、R-3、R-5、R-7 | U1、U3、U4、U5 通过（mcpp 与 xlings，4 vCPU）；`taskset -c 0-1` 低配档不劣于 0.0.6；U14 内存预算通过 |
| R-8、K-6 | U5、U8、U12 里没有在用户输入时发生的重启；U15 状态时间线预算通过 |
| R-9 | 打开与关闭优先级选项各跑一次 U1、U4：打开后 U4 不劣化，编辑器侧（VS Code E2E stress）p90 不劣化，再决定默认值 |
| R-4 | xlings（producer 约 5 s）冷启动：过渡模型期间零个项目模块预建，切换后零次计入预算的重启 |
| K-1 | Windows 探针：挂起 clangd 的线程（模拟卡死），StuckWatch 在 10 s 内判定并重启 |
| C-1 | E2E：执行重置命令后服务端回到 ready，缓存目录只剩新建内容 |
| C-2 | 连续 10 个会话、每次改一次构建参数，工作区缓存不超过上限，只剩当前命令的 BMI |
| C-4 | `e-perf/harness/s6b_cache.py --damage lock-live` / `lock-foreign`：15 s 内自动恢复，不需要手删；GalTranslPP 探针 E-warm 同样不卡 |
| C-5 | 杀服务端后 5 s 内重启：用主目录、0 个 BMI 重建、≤ 5 s ready |
| C-6 | GalTranslPP 探针：预建中触发一次 mcppls 的 plan 重启，下一个 clangd 不等锁，预建继续 |
| K-5 | GalTranslPP 探针：`-j=2` 下打开 7 个重型文件，零次"published no diagnostics in two minutes"误判 |
| K-2 | `s5_fanout.py` 保存 `mcpp.manifest.types`：clangd 崩溃后按退避恢复，不进入 error 放弃状态 |
| K-4 | `s4_typing.py --autosave`：零次"would not finish"重启 |
| R-6 | 让一个模块的单元扫描失败（缺 include）：数据库不再来回翻转，零次"providers moved"重启 |
| G-4 | GalTranslPP 探针（默认期限、不加 `--producer-timeout`）拿到 mcpp 模型并写入模型缓存 |
| O-1、O-2 | CI 的 VSCodium 作业三套件全绿 |
| U-* | `ux-mcpp`、`ux-xlings` 在 0.0.7 的 PR 上全绿；nightly 三轮测量上传；pre-release 按 §6.4 三轮连过；GalTranslPP 探针按同样的预算过一次 |

## 9. 决定与剩余待确认

已定（§0.0）：termux / proot 正式支持（D1）；std 参数来源与宏白名单（D2）；调度从用户体验出发（D3，§4.3）；启动前无条件清空 `.locks/`（D4）；
mcpp 与 xlings 上的场景测试与预算（D5）；0.0.7 单 PR（D6）。

剩余的都给出了建议并按建议执行，review 时可改：

1. **X-1 的位置**：openkal 里 ENOSYS 时退回 `execve`（建议，执行）；不改 `kal_spawn` 接口。
2. **W-4**：去掉"BMI 已存在"的捷径（建议）；若 U1 / U2 显示多出来的 prime 单元让冷启动变慢超过 5%，改为自建清单。
3. **R-2 的常数**：`workers = clamp(硬件线程 − 1, 2, 8)`、按内存封顶、给交互留 1 个 worker；最终常数以 U1、U4、U14 定稿（只朝更快、
   更省内存的方向调）。
4. **R-7 的补全预算 1 s**：到点先给 mcppls 的结果并标 incomplete。代价是 clangd 若在 1.2 s 才回答，这一次用户看到的是较粗的候选，
   下一次按键就会换成 clangd 的（建议 1 s；U5 若显示回退比例 > 10% 再调）。
5. **R-9 默认值**：以 U1 / U4 与编辑器侧 stress 套件的对比决定默认开或关。
6. **G-1 的宏白名单**：`_ITERATOR_DEBUG_LEVEL`、`_HAS_*`、`_DEBUG`、`_DLL`、`_MT`、`_GLIBCXX_*`、`_LIBCPP_*`（建议，执行）；
   只用于没有 std 描述的来源。
7. **缓存上限**：每个工作区只留当前命令的 BMI 与最近 2 份；每个工作区 4 GB、全部工作区合计 16 GB，超出按最近使用淘汰（建议）。
8. **GalTranslPP**：作为 0.0.7 发布前的一次性 Windows 验收（按 §6.4 预算），不进 nightly（外部仓库、单次 1–2 小时）。
9. **Code-OSS 的 CI**：0.0.7 先做 Linux 的 VSCodium（建议）；Windows / macOS 的 VSCodium 之后再加。
10. **真正的第二实例**（两个窗口开同一工作区，D38）：0.0.7 不变，仍用私有目录；C-5 解决的是"崩溃后重启被当成第二实例"这个常见情形。
11. **缓存格式 v3**：所有用户升级到 0.0.7 后的第一次启动是冷启动，发布说明写明（建议接受）。

## 10. 0.0.7 实施计划（单 PR）

### 10.1 任务与依赖

| # | 任务 | 依赖 | 条目 |
|---|---|---|---|
| T0 | 分支 `release/0.0.7`；本方案；关联 #30、#32 | — | — |
| T1 | **openkal 发版**：openkal-linux 的 `execveat` → `execve` 退路（ENOSYS，父进程先算好绝对路径）；`kal_spawn` 的"低于正常优先级"选项（三个平台）；openkal-windows 的 `kal_process_times`；`okl::sys()` 参数寄存器改为读写操作数（x86_64、aarch64；openkal-musl 的 `syscall_arch.h` 同改，X-4）；openkal 自己的 conformance（seccomp-ENOSYS、`#!` 脚本、在 PRoot 两种模式下跑预打开表与启动、优先级、CPU 时间）。mcppls 升 `openkal-llvm-runtime` | T0 | X-1、X-4、R-9、K-1 |
| T2 | **平台层**：启动失败分类与全局"不能启动程序"诊断、report 的 `sandbox`；预打开表缺 `/` 时告警；Windows 的 `cpu_seconds`；clangd 以低优先级启动（设置 `mcppls.engine.lowPriority`） | T1 | X-2、X-5、K-1、R-9 |
| T3 | **std 参数**：mcpp 用 producer 的 `mcpp:std` 集合（改计划第 0 步对 mcpp 的规则）；其它来源宏白名单；fixture 变体（libstdc++ 与 MSVC 的守卫宏） | T0 | G-1 |
| T4 | **#30**：`optionsDerived` 持久化、模型缓存 envelope 2 → 3；`adopt_model` 保持一个模型对象、`plan-drift` incident；失败单测转绿、`engine-database-stable` 检查；命令变化原因汇总 | T0 | W-1、W-2、W-3、W-5 |
| T5 | **primer**：去掉"BMI 已存在"的猜测（或自建清单，按 T11 的 U1 / U2 实测） | T4、T11 | W-4 |
| T6 | **锁、退出、租约**：启动 clangd 前清空 `.locks/`，解析"Still waiting for module lock"，硬杀后清掉被杀 pid 的锁，锁检查改看 `.locks/`；重启时先发 LSP `shutdown` / `exit` 等 5 s；租约记 pid 与进程启动时间，持有者已死即接管 | T0 | C-4、C-5、C-6 |
| T7 | **守卫**：崩溃按 RD6 退避、不再放弃；替身取代不算变化、扫描失败报问题；`DISK_SETTLE` 按构建耗时；"file is queued" 不计时、首诊断守卫加 CPU 条件；超时归因（`busyWorkers`）；状态迟滞与说明 | T2 | K-2、R-6、K-4、K-5、R-3、K-6 |
| T8 | **调度**：统一后台调度器与四级优先级；worker 公式与 `mcppls.engine.workers`；N-7 只用空闲时间；过渡模型不预建；非紧急重启等空闲 | T6、T7 | R-1、R-2、R-5、R-4、R-8 |
| T9 | **请求**：按方法分级的等待预算；补全回退标 incomplete 并取消 clangd 请求；semanticTokens / documentSymbol 不等、就绪后 refresh | T8 | R-7 |
| T10 | **producer 期限**：按上次 emit 耗时自适应、有进展不杀、10 分钟上限；`producer-timeout` 问题与"在终端运行 / 放宽期限"动作 | T0 | G-4 |
| T11 | **场景测试**：运行器的 `latency`、`typing`、`edit-save`、`fault`、`timeline`、`bmi-reuse`、`resources` 检查；`ux-mcpp`、`ux-xlings` fixture；CI（0.0.7 PR 必过、nightly 三轮、pre-release 门槛取代 `timing` 中位数、普通 PR 的 5 分钟精简版）。**先做**：用它量出 0.0.6 在 4 vCPU 上的基线，再由 T8 / T9 的结果定稿预算 | T0 | U-* |
| T12 | **缓存**：重置本工作区缓存的命令（VS Code、Zed、CLion、服务端命令）；GC（只留当前命令与最近 2 份、容量上限、启动后后台执行、`cache --prune`）；自愈（同类故障跨会话重复时移开模块缓存一次） | T6 | C-1、C-2、C-3 |
| T13 | **proot CI**：从固定 commit 构建 termux-proot（x64、arm64），两种模式跑 `inferred`、`mcpp-emit`、`timing`、`mcpp-watch` 与 VSIX main 套件；必跑 | T1、T2 | X-3、X-4、X-5、O-5 |
| T14 | **Code-OSS**：`MCPPLS_E2E_EDITOR` 与 CLI 路径推导；VSCodium 的 CI 作业；Open VSX 发布后校验；诊断包记 `appName` 等；扩展简介以 "mcppls - " 开头 | T0 | O-1..O-4、O-6 |
| T15 | **崩溃符号**：release 资产附 clangd 符号文件；incident 记模块偏移、devtools 离线符号化；扇出崩溃的最小复现与上游登记 | T0 | K-3 |
| T16 | **规范与文档**：S3 新 issue 码（`engine-start-failed`、`producer-timeout`、`module-lock-stale`、`engine-crash-loop`）与 schema / traceability；设置注册表（`engine.workers`、`engine.lowPriority`、缓存上限）与 `docs/30-settings.md`；`design.md` 决定表（修订 BD5、C7，RD6 扩到崩溃，新增 D1–D6）与已知限制（删去"openkal 不能降低子进程优先级"，写明 termux 支持范围）；`docs/00-install.md`、`docs/92-release.md`（新门槛）；CHANGELOG；#24 登记（clangd 扇出崩溃、Windows 锁、PRoot `execveat` 与 seccomp 模式的寄存器恢复、mcpp emit 耗时）；向 termux/proot 与 proot-me 各报 issue | 全部 | — |
| T17 | **验证**：单元测试；全部 conformance fixture（三平台）；`ux-mcpp`、`ux-xlings`；proot 作业；VSCodium 作业；GalTranslPP 探针按 §6.4 预算（Windows）；报告人在 termux 真机上用候选 VSIX 复测 | 全部 | — |
| T18 | 版本 0.0.7，一个 PR，CI 全绿，自我 review，squash 合入（Sunrisepeak 为作者、speak-agent 为 co-author），Release，Open VSX 校验，本地验证 | T17 | — |

并行：T1（openkal，外部依赖）最先开；同时进行 T3、T4、T6、T10、T11（先量基线）、T14、T15。T2 → T7 → T8 → T9 是主链。

**提交顺序**（一个 PR，按可以单独验证的三段排列，便于 review）：

1. **正确性**：T3、T4、T5、T6、T7 里的 K-2 / R-6、T10、T1 + T2 的 X-1 / X-2 / X-4。这一段做完，G-1、#30、"只能删缓存"、"死掉后不回来"、
   proot 都已修好，可以先跑一遍全部 fixture 与 GalTranslPP 探针。
2. **体验**：T8、T9、T7 其余、R-9，配合 T11 的预算定稿。
3. **卫生与覆盖**：T12、T13、T14、T15、T16。

### 10.2 CI 可用性验证矩阵（Code-OSS、linux-aarch64、termux / proot）

| 作业 | 平台 | 内容 | 频率 |
|---|---|---|---|
| `vscode-e2e`（已有） | linux-x64、linux-arm64、darwin-arm64、win32-x64 | Microsoft VS Code：main、conflicts、stress 三套件 | 每个 PR |
| `code-oss-e2e`（新） | linux-x64、linux-arm64 | VSCodium（固定版本，按平台下载并缓存）：同样三套件，`MCPPLS_E2E_EDITOR` 指向 VSCodium，另查诊断包的 `appName` | 每个 PR |
| `proot`（新） | linux-x64（ubuntu-24.04）、linux-arm64（ubuntu-24.04-arm） | 从固定 commit 构建 termux/proot；服务端在 proot 里（默认 seccomp 模式与 `PROOT_NO_SECCOMP=1` 两种）跑 conformance 的 `inferred`、`mcpp-watch@polling`、`typing-import`、`diagnostic-bundle`；再用 VS Code 的 main 套件，把 `MCPPLS_SERVER` 指向"在 proot 里启动服务端"的包装程序 | 每个 PR |
| `conformance`（已有） | linux-arm64 | 已有的 20 个 fixture（kit、clangd、服务端在 aarch64 上） | 每个 PR |
| `ux`（新） | linux-x64（4 vCPU） | `ux-mcpp`、`ux-xlings`，§6.4 的预算 | 每个 PR（0.0.7 起），nightly 三轮 |
| `termux`（新，nightly） | linux-arm64 | `termux/termux-docker` 里装 proot-distro 的 Debian，在其中解开 linux-arm64 payload，跑 `mcppls report` 与 `inferred` fixture——最接近报告人（termux + proot + Debian）的环境 | nightly |

termux 真机（Android）不进 CI：GitHub 的 runner 上没有可用的 Android arm64 环境；由报告人在发布前用候选 VSIX 复测（T17）。

### 10.3 实施分工（并行）

| 线 | 负责 | 文件范围（互不重叠） |
|---|---|---|
| 主线 | 引擎、编排、计划、模型缓存、租约、GC、规范与文档 | `src/engine/**`、`src/orchestrator/**`、`src/normalize/**`、`src/spec/**`、`src/project/**`、`src/cli/**`、`src/config/**`、`docs/**`、`.agents/docs/design.md` |
| 场景测试线 | 运行器的新检查类型、`ux-mcpp` / `ux-xlings`、`ux` 作业 | `src/bin/conformance.cpp`、`conformance/**`、`.github/workflows`（`ux` 作业） |
| 编辑器线 | 重置缓存命令、`MCPPLS_E2E_EDITOR`、诊断包 `appName`、`code-oss-e2e` 作业、Open VSX 发布后校验 | `editors/**`、`.github/workflows`（`code-oss-e2e`、`publish-openvsx.yml`） |
| 平台线 | openkal-linux 补丁（X-1、X-4）、预打开表告警、proot 探测、`proot` / `termux` 作业 | `vendor/openkal-linux/**`、`mcpp.toml` 的覆盖声明、`modules/platform/**`、`.github/workflows`（`proot`、`termux`） |

**0.0.7 不做、需要 openkal 规范新增接口的**：K-1（Windows 的进程 CPU 时间）、R-9（子进程优先级）——两者都要在 openkal 规范包里加新的
函数或 spawn 标志，并在三个实现里各实现一遍；在本仓库里覆盖规范包会波及所有依赖它的包。登记为 openkal 的待办，0.0.7 之后随 openkal
发版接入。K-3（崩溃符号）取决于 payload 里 clangd 的来源能否提供符号文件，0.0.7 先在 incident 里记下模块偏移，符号文件随之后的 payload
更新提供。C-3（跨会话自愈）在 C-4、C-6、K-2 修好之后价值很小，0.0.7 不做。

### 10.4 各角度的检查项

| 角度 | 要求 |
|---|---|
| 架构 | 后台工作只有一个调度器（R-1）；平台差异只在 `modules/os` 与 openkal；新设置只进注册表（RD10）；场景测试在运行器里，不加脚本 |
| 稳定性 | 没有"放弃"状态（K-2）；每次强杀 clangd 之后都清锁（C-4、C-6）；所有重启都经过 RestartGate；数据库不会在两个状态间来回（R-6） |
| 用户体验 | §4.3 的四条规则；§6.4 的 U1–U15 预算；状态栏说清楚在做什么、为什么、用户能做什么 |
| 性能 | 4 vCPU 上冷启动预建 ≤ 90 s（mcpp）；热启动零重建；内存 ≤ U14 |
| 兼容性 | 旧租约（没有 pid）按心跳判断；旧设置名继续被接受；旧客户端不认识新 issue 码也能工作 |
| 跨平台 | Linux、macOS、Windows、linux-arm64、termux / proot 都有 CI；R-9、K-1 在三个平台都实现 |
| 一致性 | S3 新规则有 traceability；设置表、文档、`package.json` 互相校验（已有测试） |
| 无感升级 | 缓存格式 v3 让升级后第一次启动冷启动一次（发布说明写明）；设置默认值的变化（worker 数）只会更快；不需要用户做任何事 |

### 10.5 验收

0.0.7 合入与发布的条件：CI 全绿（含 §10.2 的新作业）；`ux-mcpp`、`ux-xlings` 在 4 vCPU 上满足 §6.4 全部预算；proot 作业（x64、arm64，两种模式）通过；
VSCodium 作业通过；GalTranslPP 探针在 Windows 上 `Run.cpp` 零 `no_member`、打字时补全 p95 ≤ 1 s 且由 clangd 作答的比例 ≥ 80%（重型头文件、4 核，
比 mcpp / xlings 放宽）、杀 clangd 后 ≤ 15 s 恢复；
报告人在 termux 真机上 clangd 起得来、跨模块跳转可用。

## 11. 总体自我 review

### 11.1 每个现象是否闭环（现象 → 根因 → 修法 → 验收）

| 现象 | 根因（证据） | 修法 | 验收 |
|---|---|---|---|
| #32 proot 下一切都起不来 | PRoot 不支持带 dirfd 的 `execveat`（已验证）；默认模式下 PRoot 不恢复被改写的参数寄存器、openkal 复用了它（已验证） | X-1、X-2、X-4、X-5 | X-3 的 CI 两种模式 + 真机复测 |
| #30 热启动重建全部 BMI | 标志不进缓存（已验证）；第一次重新计划再翻一次（已验证）；primer 误认 BMI（代码确认） | W-1..W-5 | U2、`engine-database-stable` |
| `std::views` 误报 | std 带着 `-D_RANGES_` 编（已验证） | G-1 | fixture 变体、GalTranslPP E-diag |
| JSON `{…}` 报错 | **未定位**（两轮探针都没拿到这些文件的 clangd 诊断） | G-1 后复测；要截图 | GalTranslPP E-diag |
| 补全慢、半不可用 | 2 个 worker、预建 1 个、后台工作不让路、统一 10 s / 30 s 等待（已验证） | R-1、R-2、R-5、R-7 | U1、U4、U5 |
| 写半分钟就死一次 | 崩溃后放弃（已验证）、数据库翻转（已观察 + 代码）、#30 的重启（已验证）、K-4 / K-5 误判（已验证） | K-2、R-6、W-3、K-4、K-5、R-8 | U5、U6、U7、U15 |
| 一直 preparing、只能删缓存 | mcppls 的重启 500 ms 后硬杀 clangd，锁留下，Windows 上永远等（已验证）；崩溃后重启变冷启动（已验证） | C-4、C-5、C-6 | U9、U10、U11、GalTranslPP E-warm |
| 缺少场景化的性能与稳定性规划 | 发布门槛只量一个 3 文件 fixture | U-*、§6.5 | 0.0.7 PR 必过、pre-release 门槛 |

只有 JSON `{…}` 一条没有闭环；它的验收依赖 G-1 修好后的复测或报告人的截图。

### 11.2 这份方案本身的风险

- **PR 太大。** 18 个任务、跨 openkal 与 mcppls、三个编辑器插件和 CI。缓解：按 §10.1 的三段提交，第一段做完就能完整验证正确性；
  openkal 发版放在最前，避免卡在最后。
- **预算还是初值。** U1 / U4 / U14 的数字来自 0.0.6 在 4 线程档的实测加外推，R-2 在预建上限 > 2 时没有测过。风险是定得太松（没意义）
  或太紧（CI 常红）。缓解：T11 先做、先量基线；预算只收紧不放宽；nightly 三轮看分布再定稿。
- **R-2 与内存。** worker 变多，冷启动内存上去（32 线程 `-j4` 已经 6.8 GB）。内存封顶与 U14 卡住它；8 GB 内存的机器会自动退到 3–4 个 worker。
- **R-7 的回退质量。** 1 s 回退让补全"快"，但偶尔是较粗的候选；而且它会让延迟预算变得"容易"。所以 U4 / U5 同时卡"由 clangd 作答 ≥ 90%"，
  回退比例超标时先查 worker 是否仍被占（R-1），再考虑把预算调到 1.5 s。
- **R-9 的副作用。** 机器被别的程序占满时，低优先级的 clangd 更慢；所以先测再定默认值。
- **C-4 无条件清空锁** 的前提是这个缓存目录只有一个 clangd 在用。C-5 之后主目录只属于持租约的实例，第二实例用私有目录（各自的
  `.locks/`），前提成立；这条依赖写进代码注释与测试（U10、U11）。
- **K-2 不再放弃** 意味着在反复崩溃的项目上 clangd 会每隔 1–8 分钟重试一次；每次崩溃都把出事的文件放到一边，重试的代价有上限。
- **proot 正式支持的长期成本**：arm64 CI、termux-proot 的版本变化、Android 上的差异只能靠报告人复测。X-3 的作业固定 proot 的 commit，
  升级时显式更新。
- **CI 时长**：release PR 多约 40 分钟（两个 UX job 并行），nightly 多约 2 小时；普通 PR 只多 5 分钟。
- **计时测试不稳**：§6.5 的做法（多次采样、三轮、记录负载、20% 余量、连续两晚失败才开 issue）；若仍不稳，把该项降为"只记录"并在
  review 里说明，不能悄悄放宽预算。

### 11.3 与已有原则和决策的关系

- 贯彻 P1（故障只影响它所在的地方：R-6、C-4）、P3（重启是最后手段、有预算、不振荡：K-2、R-8、R-6）、P4（每个请求有期限和回退：R-7）、
  P6（资源有上限：R-2 的内存封顶、C-2 的缓存上限）、P7（降级要说出来：K-6、X-2）。
- 修订：BD5（producer 硬期限 60 s → 自适应，G-4）；C7（worker 公式，R-2）；RD6 扩到崩溃原因（K-2）；RD9（N-7）保留，但改在空闲时做（R-5）。
  不变：RD7（kit 替换规则；G-1 只改 std 用谁的参数）。
- `design.md` §7 的已知限制"openkal 不能降低子进程优先级"随 R-9 删除；新增"termux / proot 的支持范围"。

### 11.4 没覆盖到、需要 review 时知道的

- macOS 没有实测；场景测试的 Windows 部分只有 GalTranslPP 一次性验收，没有持续的 Windows 大项目基准。
- Zed / CLion 客户端、多根工作区、MCP / 命令行入口的性能没有单独看；R-7 的回退在它们上面的表现要在 T17 里各开一次确认。
- X-4 的修法在 x86_64 上验证过（打补丁的 openkal 在两个 PRoot、两种模式下启动子进程成功）；aarch64 的寄存器问题形状相同但没有实测，
  termux 真机（aarch64）是 T17 里报告人复测的重点。
- 报告人机器的核数、内存与 emit 耗时未知；0.0.7 的候选版本应请报告人用它导出一份诊断包，验证 R-2 与 G-4 在真实机器上的效果。

## 12. 附：复现

- #32：scratchpad `a-proot/`（`nosys.c`、`fb.c`、termux-proot 构建）；X-4：scratchpad `g-proot-seccomp/`（`rsi.c`、`ok/t1.cpp`、`t2`、`openkal-linux-proot.patch`）；
- #30：scratchpad `b-cache/`（worktree 与失败单测，`proj/` 小型 mcpp 项目）；
- Linux 基线：scratchpad `e-perf/`；
- Code-OSS：scratchpad `f-codeoss/`；
- GalTranslPP：Sunrisepeak/GalTranslPP#1 的探针分支 `ci/mcppls-issue23-probe`（`4c701bb`、`aaf690b`、`ea1eaeb`），run 36601166906 /
  36616368503 的产物解压在 scratchpad `c-galtransl/r1`、`r2`（`e-cdb/std-entries.json`、`e-diag/`、`e-typing-*/`、`e-warm/`、`hyp/`）。

scratchpad 在 `/tmp` 下，会话结束后可能被清理：#30 的失败单测（T4 的起点）、`f-codeoss/` 的 harness 补丁（T14 的起点）应在 T0 时
搬进仓库；`e-perf/harness/` 的驱动只作 T11 的参照；`e-perf/cache`（17 GB）不需要保留。

## 13. 实现记录（0.0.7，2026-09-30）

按 §10 做完的条目与方案的差异，以代码为准：

- **R-7**：预算在编排层（`routing::answer_budget`、`Workspace::answer_without_core`），与 F15 的关键字耐心合为一处；到点先答、再对
  clangd 发 `$/cancelRequest`，请求仍留在 clangd 引擎的 `pending_` 里直到 clangd 回应，所以超时、隔离与 StuckWatch 的判断不受影响。
  补全回退是本文件里离光标最近、首字母相同的词（跳过注释与字面量；`.`、`->`、`::` 之后不给），`isIncomplete: true`，关键字照旧并入。
  与 §4.3 表格的两处不同：
  - 定义 / 声明的预算直接是 10 s，没有 3 s 的"先给词法定位"。mcppls 引擎在 import 行上本来就排在 clangd 前面作答（优先级 100 对 0），
    按名字找定义（N-8）在 clangd 引擎的定义搜索里，编排层没有另一个可问的引擎。
  - semanticTokens、documentSymbol、folding 不设预算：所有客户端都异步处理它们，没有人看着转圈；截断后再 refresh 只会多一轮请求。
- **R-1**：没有单独的调度器对象，由三处限流合成：预建 ≤ `workers − 1`（有文件在等）或一半；N-7 在预建时不开、只在空闲时一次 1 个；
  定义搜索由用户发起、受请求的期限约束。效果与四级优先级一致，代码少一层。
- **R-5**：`Options::implementationQuiet`（10 s）；"活动"是打开 / 修改文件与交互请求。**R-8**：只有输入（didChange）推迟重启，悬停、
  补全不推迟——否则 fixture 的重试循环与"一直在 hover 的人"会把重启拖满 60 s；`engine-restart-postponed` 事件。
- **R-3**：超时时预建或后台单元在构建，按 `rebuilding` 处理（仍计入"谁都不回答"）；`request-timeout` 事件带 `workers`、`preparing`、
  `backgroundBuilding`。
- **K-6**：只对会自己消失的问题（`file-quarantined`、`engine-restart-capped`）持续 30 s 才报 degraded；崩溃、超时重启、项目与环境问题
  仍是 3 s（S3-4-15）。"一分钟内超过 6 次合并"没有做：30 s 的保持已经覆盖了观察到的翻转。
- **C-2**：每个单元保留最新的 2 个命令目录；clangd 启动前改名移到 `contexts/<ctx>/trash/`（启动不等），后台线程删除；`mcppls cache --prune`
  处理没有服务端占用（无 `owner.lease`）的全部工作区。没有做容量上限设置与跨工作区 LRU：每单元 2 份已把缓存限在项目 BMI 的两倍以内；
  需要时再加（记入剩余）。
- **K-7**：自动诊断包写在 `<cache>/bundles/auto-<code>-<time>.zip`，保留最新 5 个；issue 带 `bundle`（S3-4-22..25）；扩展的通知、
  崩溃报告、`mcppls.enable` 与 issue 模板在编辑器线里完成。
- **推迟**：K-1、K-3、R-9、C-3（§9 已定），以及 R-4（过渡模型不预建）：#30 修好后同一命令不再重建，R-8 让模型切换发生在空闲时，
  R-4 剩下的收益只在"没有缓存的第一次打开"，留到 0.0.8 按 U1 的实测决定。
- **G-2**（JSON `{…}`）仍待 GalTranslPP 交叉验证：用 PR 分支的构建复测 `Batch.cpp:135`、`TransAgent.cpp:437`。
