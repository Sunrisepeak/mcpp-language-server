# 出问题时

[English](../50-troubleshooting.md) | **简体中文**

## 从这里开始

**C++ Modules: Collect Diagnostic Report**（命令行对应 `mcppls report`）。它打开的 JSON 里，几乎每个问题最终都要用到的信息都在：

| 报告中的字段 | 回答的问题 |
|---|---|
| `project.source`, `project.tier`, `project.level` | 构建描述来自哪里（`tier`，即 README 的 L1..L4），以及它的文档完整到什么程度（`level`，S1 自己的数，和 `tier` 是两个问题） |
| `project.notices` | 值得知道、但不减少任何功能的事实：丢弃了过期的数据库条目、找回了生成的模块而没有用占位、用别的 mcpp 代替工程固定的版本来描述工程 |
| `project.origin` / `firstOrigin` | 本次会话是从缓存、从构建工具，还是从扫描到的源码启动的 |
| `project.producer`, `producerRun` | 运行了哪个构建工具、耗时多久、如何结束、是否离线 |
| `toolEnvironment` | 构建工具是在哪个环境中启动的，以及与编辑器环境不同的那些变量的**名称**（不含值） |
| `toolRuns` | 最近二十次外部运行，每条都带命令、耗时和结果 |
| `plan` | 交给引擎的内容：条目、占位单元、被省略了什么以及原因 |
| `engines` | clangd 的状态、重启次数、被搁置的文件，以及 `workarounds`：本服务端针对这个 clangd 版本规避的 clangd 缺陷 |
| `requests`、`completion`、`documents` | 每种请求来了多少次、答得多快、由哪个引擎作答、最后一次是什么时候（`lastAt`）；补全有多少次用文件里的词作答；编辑了多少次、最后一次在什么时候——编辑一直在继续，而 `requests["textDocument/completion"].lastAt` 停住不动，说明是编辑器不再来要补全 |
| `events` | 本次会话的事件日志 |
| `logTail` | 日志的末尾部分 |

日志文件不会随编辑器关闭而消失：报告里写明路径，日志按时间戳保存在缓存目录下。

报告本来就是为了给别人看的：其中你的主目录写作 `~`，用户名和主机名写作 `<user>`、`<host>`，看起来像密钥的内容（token、密码、API key、邮箱）写作 `<redacted>`。工程自己的路径保留——看报告要的正是它们。

**C++ Modules: Export Diagnostic Bundle** 更进一步：生成一个 zip，写在缓存目录的 `bundles/` 下（保留最新的 5 个），**从不上传**，里面是排查问题通常需要的全部内容——

| 问题包里的文件 | 内容 |
|---|---|
| `report.json` | 上面的报告 |
| `environment.json` | 系统、编辑器和插件的版本、其他 C/C++ 插件、你的 mcppls 设置、决定打字时是否弹出补全的编辑器设置（`client.editor`：C++ 下的 `editor.quickSuggestions` 及其来源、内联建议、自动保存、装了哪些内联补全插件）、payload、探测到的工具链，以及少数几个环境变量（`PATH`、`LANG`、`LC_*`、`MCPP_*`、`XLINGS_*`），其他的一概不收 |
| `logs/` | 服务端最近三次会话以及最近一天内其他会话的日志，还有插件自己的日志 |
| `incidents/` | clangd 崩溃、卡住或文件被搁置时服务端记下的现场——崩溃时还有 clangd 打印的 LLVM 栈转储，以及 clangd 可执行文件的版本和 SHA-256，这是向上游报告需要的 |
| `engine/` | 交给 clangd 的数据库，以及生成它的计划 |
| `manifest.json` | 每个文件的大小和 SHA-256，以及每条脱敏规则各替换了多少处 |

——每个文件都做同样的替换。写出之前，会在整个问题包里按各种写法搜索你的主目录、用户名和主机名；只要还有残留，**就不写出问题包**，提示会说明是哪个文件，并可以选择 *Retry with Project Paths Hidden*，把工程路径也一并替换。源文件从不打包；事故记录只带与问题相关的那几行。其他编辑器用 `workspace/executeCommand` 的 `mcppls.exportBundle` 执行同一操作，命令行则是：

```bash
mcppls report --bundle problem.zip --root path/to/project   # 可加 --hide-project-paths、--no-source-excerpts
```

崩溃转储（dump）默认不包含（需要时用 `--include-dumps`）：它装的是内存，无法脱敏。`--no-redact` 保留一切原样，只用于在你自己的机器上排查；编辑器里不提供这个选项。

## 常见症状

**“xmake needs libtool, libpthread-stubs downloaded”。** 这些名字不是你的代码缺的库：它们是 xmake 为你的 `xmake.lua` 所要求的包而要下载或构建的东西，包括构建工具（设成从源码构建的包会带进自己的构建工具，比如 `libtool`、`meson`），而 mcppls 保持离线，不会自己去下载。有三条路：在通知里选 **Download and Continue**；在终端里运行 `xmake`（做完后会重新读取描述）；或者用系统包管理器安装（`apt install libtool libpthread-stubs0-dev`、`pacman -S libtool`、`brew install libtool`）——xmake 的包定义里写了对应的系统包，找到系统里的就直接用。如果状态里写的是 `producer-install-failed`，说明已允许联网、安装本身失败了；消息里有 xmake 的 `error:` 行和 `install.txt` 日志的路径，常见原因是源码构建需要的工具（autotools、编译器）没有安装。

**所有功能失效，任何位置都无法跳转到定义。** 看报告里的 `project.source`。如果一个用了构建系统的项目里它是 `inferred`，说明构建工具没有给出答复；原因在 `project.issues` 和 `toolRuns` 的最后一条里。常见原因是构建工具需要下载东西：这时状态栏会提议在你的终端里运行它。

**跳转到定义能用，补全不能用，或者标准库缺失。** 看 `profile`。语义工具包意味着没找到可用的编译器；`import std` 仍然能解析，但诊断来自 libc++，不是来自你的工具链。

**原本正常，后来某个模块突然解析不了了。** `plan.standIns` 列出了没有任何单元提供的模块——mcppls 给它们分配占位单元，这样一个坏掉的模块不会拖垮项目的其余部分，日志里也会逐个写明模块名和原因。真正的问题在 `plan.issues` 里，或者在你的构建本身。

**VS Code 里打字时不弹补全列表，或者停下来等一会儿才出来。** VS Code 1.125 及以后的版本，装了内联补全插件（VS Code 已内置 GitHub Copilot）时，会先等内联补全再决定开不开列表；内联补全显示灰字时就根本不开：`editor.quickSuggestions` 的默认值是 `{"other": "offWhenInlineCompletions"}`。这时服务端根本没被问到。从 0.0.8 起，插件为 C 和 C++ 文件设为 `{"other": "on"}`（`WA-VSCODE-002`），列表和灰字同时出现。这个默认值会盖过你为所有语言设的 `editor.quickSuggestions`；日志里会说明一次，把它写在 `"[cpp]"` 和 `"[c]"` 下就能保留你的设置。诊断包里能看出来：`environment.json` 的 `client.editor`，以及报告里 `requests["textDocument/completion"]` 与 `documents` 的对比（一直在编辑，却没有补全请求）。

**某个模块编译不过。** 受影响的只有直接或间接导入它的文件：这些文件由 mcppls 自己的引擎立即应答（模块跳转、符号、`import` 补全，其他补全给出文件里的词），在引向失败的那条 import 上带一条 `module-failed` 诊断，并且在失败模块自己的源码或编译命令变化之前不会再交给 clangd；其余文件照常由 clangd 应答。编译失败的那个单元本身从不被拿走（0.0.8）：它就是你正在写的文件，clangd 从编辑器读取它，给出真实的报错和补全——开着自动保存时，写到一半存到磁盘上的内容照例编译不过。这是代码本身的问题，所以在出问题的地方以诊断的形式告诉你：状态保持 *ready*（列出类别为 `code` 的 `modules-doomed`），也不会一直停在 *preparing*。报告里引擎的 `doomedModules` 和 `filesRoutedToOwnEngine` 会列出它们。

**状态说明什么，不说明什么。** 代码里的错误（少了 `;`、import 了没有任何单元提供的模块、某个模块编译不过）以诊断的形式出现在出错的位置，也就是 Problems 列表里；状态保持 *ready*。*degraded* 表示服务端丢了本来能给你的功能，并说明丢了什么、在哪里：“clangd stopped responding on main.cpp”、“the workspace is not trusted”、“the macOS SDK was not found”。每个状态 issue 都带有 `category`（`code`、`engine`、`environment`、`project`），说明这是谁的问题；只有 `code` 以外的类别会让状态变成 *degraded*，而且要持续三秒才会显示，所以自己很快就会消失的情况不会出现在状态栏上。工作区进入 *ready* 之后，因编辑而重建模块（比如保存了一个项目里大部分模块都导入的模块）要持续 30 秒以上才会重新显示 *preparing*（0.0.8）；回到 *ready* 从不延迟。

**输入 `import` 时编辑器卡死（0.0.3 及更早版本）。** clangd 23.1 遇到模块名以 `.` 结尾、而且 `.` 就在行尾的文件（`import hello.`、`export module a.`）时永远处理不完，这个文件之后的所有版本都排在它后面等待；而输入任何带点的模块名都会经过这个状态。mcppls 0.0.4 改为把这一行在点后补上 `;` 再交给 clangd，clangd 会立即报告这个错误（规避措施 `WA-CLANGD-001`）；报告里的 `engines[].details.workarounds` 会列出它。

**开着自动保存输入 import 时，这个文件有几秒钟不走 clangd。** clangd 从磁盘上的文本读取一个文件的 import，而不是从编辑器里读（0.0.4 及更早版本会就此永久卡住：`import hello.` 被自动保存后，这个文件的所有请求都得不到应答）。一次保存把 clangd 会卡住的内容写到磁盘上时——以 `.` 结尾的模块名，或者 import 了项目里（还）没有的模块——这个文件改由 mcppls 自己的引擎应答，直到再次保存；状态里以 `file-unsafe-on-disk`（类别 `code`）列出它并说明原因，状态仍是 *ready*。没有任何单元提供的模块会在保存后一秒内得到替身单元，等 clangd 读到带替身的数据库（大约六秒后），文件就交还给 clangd。

**“Import directive must end with a ';'” 标在了别的行上，或刚输入的 import 报 “module X not found”。** clangd 把缺少 `;` 的指令报在它后面的代码上；mcppls 会把这条诊断移回指令所在行（规避措施 `WA-CLANGD-006`）。刚输入、还没保存的 import，clangd 要等文件保存后才会构建（它从磁盘读取 import）；只要这个模块在项目里，这时给出的是信息级提示 “module 'X' is in the project; clangd loads it once the file is saved”，而不是错误（`WA-CLANGD-007`）。

**某个 view 被提示 “can be declared 'const'”，加了 const 却编译不过（clang-tidy）。** clang-tidy 23.1 的 `misc-const-correctness` 会对保存 `std::views::filter`、`drop_while`、`chunk_by`、`split` 视图（或建立在它们之上的视图）的变量给出这条提示，但这类视图没有 const 的 `begin()`（issue #37）。只有 clangd 配置里设了 `Diagnostics.ClangTidy.FastCheckFilter: None` 时这项检查才会运行。从 0.0.9 起 mcppls 会去掉这条诊断，这项检查的其他诊断照常保留（规避措施 `WA-CLANGD-010`）。在此之前，或者想让这项检查完全安静，可以在项目的 `.clangd` 里 `Diagnostics.ClangTidy.CheckOptions` 下设置 `misc-const-correctness.AnalyzeValues: false`。

**不用模块的项目里，补全比直接用 clangd 慢（0.0.8 及更早版本）。** 开启模块支持时，clangd 每次补全都要重新扫描一遍文件的模块依赖，像 `vulkan.hpp` 这样很重的头文件每次要多花约 170 ms（issue #37）。从 0.0.9 起，没有模块单元、没有模块 import、也没有 `import std` 的项目，clangd 启动时不开模块支持；加入第一个 import 时会重启一次 clangd 并开启它（规避措施 `WA-CLANGD-009`，报告的 `events` 里有对应的 `engine-restart`）。

**“clangd would not finish main.cpp”。** 某个文件的构建超出了预算——该文件上次构建耗时的五倍，最少 20 秒——而编辑器还在等它：不管 clangd 忙不忙，它都不会完成这个文件了。这个文件改由 mcppls 自己的引擎应答（提供模块层面的功能），直到它的文本发生变化（让 clangd 卡住的那份文本永远不会再交给它），同时立即重启一个不带这个文件的 clangd。`events` 日志里有一条带具体数字的 `engine-spin`。

**某个规避措施还需要吗？** `--disable-workaround WA-CLANGD-<n>`（可重复）可以关掉一个；日志开头几行会列出正在使用的规避措施。每个规避措施在一致性测试里都有一个对应的检测项（`workaround-canaries`），clangd 更新修好了对应缺陷后，这个检测项就会失败。

**编辑器找到的编译器和我终端里的不一样。** `toolEnvironment.source` 应该是 `login-shell`。如果是 `editor`，原因在 `toolEnvironment.reason` 里——从桌面项启动的编辑器不带任何 shell 配置。`mcppls.toolEnvironment` 控制这一行为。

**每次启动都很慢。** 第二次会话应该很快：模型连同构建工具所读一切内容的指纹一起被缓存，与之匹配的会话会立即套用计划，并在后台确认；已经构建好的模块会复用，不会重建（0.0.6 及更早版本在热启动时会把每个模块都重建一遍，issue #30）。`project.firstOrigin` 会说明发生了哪种情况。如果它一直是 `producer`，说明指纹没有匹配上——该看报告里的 `project.producerRun` 和构建文件的时间戳。

**补全只有文件里的词，或悬停提示说正在准备模块。** 每种请求给 clangd 的都有预算——补全和签名帮助 1 秒，若某文件的补全 clangd 总是刚好超过 1 秒才回答（clangd 23.1 的模块导入方，上游缺陷 UP-25），预算最多放宽到 2.5 秒，让这些答案不会正好在预算线上被取消——悬停 2 秒，跳转到定义 10 秒——超过之后 mcppls 用手上有的东西作答。补全这时给出的是文件里离光标最近的那些词，是一份不完整的列表，所以你继续输入时编辑器会再问一次；悬停在模块准备期间给出的是一行说明。这是 clangd 正忙着处理模块，不是故障。从 0.0.8 起，clangd 迟到的补全不再丢弃：它最多再算 10 秒，你在同一个词里继续输入时发出的请求都等它，一到就交给它们，所以在 clangd 重建得慢的文件里，列表仍会在你打完这个词之前出现。报告里的 `requests.<method>.answeredBy` 按方法统计了各由哪个引擎作答；`engineP50Ms`、`engineP95Ms` 是引擎作答的那些请求在引擎里花的时间，`overheadP50Ms`、`overheadP95Ms` 是 mcppls 在其外加的时间，由此看出一次慢的补全慢在谁；`completion.late` 统计 clangd 迟到的答案和用上它们的请求；`slowestFiles` 列出最慢的十个文件，以及每个文件有多少次补全只拿到了词；`engines[].details.buildTimes` 说明 clangd 构建每个文件花在哪里（preamble、导入的模块、AST 构建次数）。

**模块单元里出现"未使用的头文件"警告。** 这是 clangd 的 include cleaner，默认开启，mcppls 不关它：在模块接口的全局模块片段、实现单元和导入方里，它和在普通文件里一样，只报没有任何东西用到的头文件（有一个 conformance fixture 在 clangd 升级时守着这一点）。要关掉，在项目的 `.clangd` 或你的 clangd `config.yaml` 里写 `Diagnostics: { UnusedIncludes: None }`；mcppls 启动的 clangd 两处都会读。

**准备模块期间编辑器有点卡。** clangd 拿到机器的线程数减一（最少 2 个、最多 8 个，内存小的机器更少）；`mcppls.engine.workers`（`auto` 或一个数字）可以覆盖这个值。有你打开的文件在等模块准备时，准备工作用掉除一个之外的全部 worker，否则用一半。为 clangd 的索引构建实现单元——也就是对从没打开过的 `.cpp` 文件做跳转到定义所需要的——一次只构建一个，而且要在你 10 秒内没有输入、没有打开文件、也没有发起请求之后才开始。clangd 因构建描述变化需要的重启，要等到输入停顿 3 秒之后（最多等 60 秒）；崩溃或者 clangd 不再应答，仍然立即重启。从 0.0.8 起，mcppls 为准备模块而在 clangd 里打开的单元，在模块一建好就关掉（BMI 留在 clangd 磁盘上的模块缓存里）：以前它们要等全部准备结束才关，期间每次保存都要被 clangd 重新检查一遍，开着自动保存时，就占走了你正在编辑的文件要等的 worker。

**clangd 反复重启。** 看 `engines[].restarts`、`engines[].details.restartBudget` 和 `events` 日志。每种原因各有十分钟三次的重启额度：引擎数据库变化（`plan`）、恢复停止应答或空转的 clangd（`recovery`）、clangd 退出（`crash`）。用完之后，同类的下一次重启依次等待一、二、四、八分钟——是退避而不是拒绝，所以卡住的 clangd 总能恢复——状态会说明（`engine-restart-capped`）并提供 **Restart clangd** 按钮（其他编辑器用 `workspace/executeCommand` `mcppls.restartEngine`），它立即重启且从不计入额度。切换工具链、profile 或 context 也从不计入，模块编译不过从来不是重启的理由。引擎数据库的每次变化都会记入日志并写明改了什么（`engine database changed: … compiled otherwise (main.cpp: argument 3: -O0 -> -O2)`），重启密集时能直接看到原因。

**“clangd crashed while building NormalJsonTranslator.Core.cpp”。** clangd 会说明它在哪个文件上崩溃（崩溃上下文），隔离的就是这个文件：它改由 mcppls 自己的引擎应答，同时重启一个不带它的 clangd。报告里的 `engines[].details.lastExit` 有退出码、文件、clangd 当时在做什么，Windows 上还有异常码。五分钟内退出五次之后，clangd 会在 1、2、4、8 分钟后重新启动，状态会说明（`engine-crash-loop`，带 **Restart clangd**）；它不会在本次会话里被放弃。clangd 稳定运行满一分钟后，崩溃和超时这两类 issue 会自行清除。最后的办法见[下文](#mcppls-无法自行恢复时)。

**“mcpp could not describe tools/updater/mcpp.toml”。** 构建工具描述了工作区的其余部分，并说明了它没能描述的那一部分（例如某个成员的构建程序失败）；那部分的文件按其余部分提供的信息来读，其余部分照常工作（`producer-partial`，S2 0.3.0）。修好消息里指出的问题，下次重新加载就会一并描述它。

**“clangd rejected the compile command for module scanning”。** clangd 在构建模块之前，会用数据库里每个单元自己的编译命令扫描它的 import；编译器驱动拒绝的命令会让扫描失败，模块也就一个都不会构建——issue #23 的 `LTO requires -fuse-ld=lld` 就是这种情况。状态会用驱动的原话写出第一处拒绝（类别 `environment`），`engines[].details.scanFailures` 记录次数。命令找不到头文件时也这样说明（类别 `project`）。正在输入的文件扫描失败是常态，只计数。

**“C++26 was disabled in precompiled file”。** 某个模块用一种 C++ 标准构建，却在另一种标准下被导入；clang 会拒绝。mcppls 对同一上下文中的模块单元统一按其中最新的标准来读（报告里的 `plan.languageStandard`、状态 profile 里的 `standard`），所以这条错误只应来自 0.0.5 之前 clangd 构建的模块（下次改动时会重建），或者构建本身就混用了标准——那样构建工具自己的编译器也会拒绝。

**刚打开项目时，一段时间内只有模块层面的功能。** 识别出构建系统的项目，要等构建工具描述完项目（构建工具要多久就多久，最长到它的时限：第一次 5 分钟，之后是上一次用时的三倍，在 1 到 10 分钟之间）才把它交给 clangd，而不是先给 clangd 一个从源码猜出来、之后还要推翻的模型；这期间由 mcppls 自己的引擎提供模块跳转、import 上的悬停和 `import` 补全。第二次会话会直接从缓存的模型开始。

**Windows 上准备卡住，或者 clangd 一直在等某个模块。** 构建模块时被杀掉的 clangd 会留下一把锁，下一个 clangd 就一直等它（0.0.6 及更早版本）。mcppls 会在 clangd 启动之前清掉过期的模块锁，clangd 每次在日志里说自己在等另一个进程持有的锁时也会再清一次。

**服务端崩溃后重启，新的服务端立即拿回工作区。** 重启后的服务端不必等待就能接管工作区：租约里记录了所有者的进程号和启动时间，所以在 Linux 上所有者已经不在会被立即认出（其他平台上租约在半分钟内过期）。

**服务端把出错的现场记在哪里。** 崩溃、clangd 卡住或空转、文件被隔离、重启被推迟、某个规避措施的前提被发现不成立，每一种都会留下一份“事故”：工作区缓存下的一个目录（`incidents/<UTC 时间>-<类型>/`，保留最近二十份、一周内），里面有事发前的经过、clangd 最近的日志（clangd 以 `info` 级别记录到内存，从不写进默认日志）、每个相关文件在编辑器与磁盘上不同的那几行，以及 clangd 哪个线程在占用 CPU。诊断包会带上它们。

**“clangd stopped making progress; it was restarted”。** clangd 有请求一直没答，期间也没答任何别的请求，并且五秒内几乎没用 CPU：它在等一个不会来的东西，而不是在编译（编译会一直占着一个核，这种情况不会被打断）。`events` 日志里有一条带具体数字的 `engine-stuck`。已经观察到 clangd 23.1 在某个模块的源文件一秒内被改两次之后出现这种情况。在 Windows 上服务端读不到 clangd 的 CPU 时间，所以检测不到；clangd 不再应答的文件仍会被逐个搁置。同样的提示也用于相反的情况（0.0.8）：clangd 在每个核上忙了几分钟，你打开的文件却一直处于"queued"，什么也没完成——三分钟里没有任何文件的诊断、没有应答、没有模块建好，而且有文件已经排队四分钟。这种情况出现在一个被项目大部分代码导入的模块开着自动保存被重写之后；上面那种卡死检测看不到它，因为 clangd 一直在占用 CPU。`events` 日志里有一条 `engine-busy-without-progress`，事故记录会保存 clangd 的日志。在 Linux 上能更早发现原因：某个 clangd 工作线程在一个 clangd 这段时间里从没说过在构建的文件上占满一个核达半分钟（文件关闭时 clangd 放手、却从未停下的构建），会立即重启 clangd（`engine-orphan-spin`，事故记录里有这个线程和它的 CPU 时间）。

**“The bundled clangd cannot run on this system”。** clangd 根本没有启动起来：系统的程序加载器拒绝了它，加载器的原话在状态和日志里（例如 ``version `GLIBCXX_3.4.30' not found``）。重启改变不了这一点，所以不会再重启；这期间由 mcppls 自己的引擎应答模块跳转、`import` 补全和模块诊断。在 Linux arm64 上，内置的 clangd 需要 glibc 2.34 和 GCC 12 的 libstdc++——它能运行的系统列在[安装指南](00-install.md)里。其他平台上出现这个提示，通常是 musl 系统（Alpine），或者 payload 损坏了。由编辑器自己启动 `mcppls` 的，可以用 `--clangd PATH` 换成你自己的 clangd（23.1 或更新）。

**在 Termux 或 PRoot 里。** `mcppls report` 把服务端运行所在的沙箱显示为 `server.sandbox`（比如 `proot`）。引擎在那里起不来时，状态里是 `engine-start-failed` 并写明原因；见 [Android 上的 Termux](00-install.md#android-上的-termux)。

## 重置工作区缓存

准备一直完不成、引擎反复崩溃，或者模型看起来不对时，重置这一个工作区的缓存，不用再手动删目录：

- **VS Code**：**C++ Modules: Reset This Workspace's Cache**
- **Neovim**：`:McpplsResetCache`
- **其他客户端**：`workspace/executeCommand` `mcppls.resetCache`

它会停掉这个根目录的引擎，删除它的缓存——模型、引擎数据库，以及 clangd 的模块缓存和锁——然后重新开始。日志保留。之后的第一次会话是冷启动。

旧的模块会自动清理：每个单元保留它最新两条编译命令构建出的模块（BMI），更旧的在 clangd 启动时于后台删除。`mcppls cache --prune` 会对所有没有实例打开的工作区做同样的清理。

缓存的其余部分有预算看管（`mcppls.cache.maxBytes`，默认每工作区 4 GB）：clangd 崩溃后遗留的 copy-on-read 副本会在下一个 clangd 启动前被清扫，死实例的目录按心跳回收，仍然超出预算的部分只报告——绝不动已发布的模块本体。在 **VS Code** 里看状态栏（悬停看分解，点击打开菜单，其中 **Sweep the Module Cache** 只清理没有引擎占用的东西，不重启、不重编），或者用 `mcppls cache --format json` 看分类数字、`mcppls cache --prune --dry-run` 预演清理。

## mcppls 无法自行恢复时

clangd 一直崩溃、引擎起不来或在这台机器上跑不了、安装已损坏，或者准备工作不再有进展：mcppls 会立即写出一个诊断包，位置是 `<cache>/bundles/auto-<code>-<time>.zip`（和任何诊断包一样做过脱敏，保留最新的 5 个，从不上传），并在日志里写明它在哪里，附上报告问题的链接。VS Code 会为此弹出一条通知，每个问题只弹一次，按钮有：

| 按钮 | 作用 |
|---|---|
| **Report Issue…** | 打开一份已填好内容的 GitHub bug 报告，并把诊断包的位置显示给你，供你附上 |
| **Restart Server** | 重启服务端 |
| **Reset This Workspace's Cache** | 见[上文](#重置工作区缓存) |
| **Turn Off in This Workspace** | 在这个工作区里把 `mcppls.enable` 设为 `false` |
| **Show Logs** | 打开日志 |

三分钟内崩溃三次的服务端不会再被重启；扩展会保存一份崩溃报告——一个文件夹，里面有客户端日志、服务端日志的末尾、它的 stderr 和基本信息——放在扩展全局存储下的 `crash-reports/` 里，并告诉你位置。

## 在一个工作区里关掉 mcppls

开关是 `mcppls.enable`（默认 `true`，按工作区生效）。在 VS Code 里，**C++ Modules: Turn Off in This Workspace** 会设置它，**Turn On in This Workspace** 会改回来；关闭期间状态项显示 “C++ Modules: off in this workspace”，点击它就会重新打开。Zed、CLion 和 Neovim 的做法见 [10-editors.md](10-editors.md)。

## 提交 bug 报告

用 [New issue](https://github.com/Sunrisepeak/mcpp-language-server/issues/new/choose) 里的模板：**Bug report** 会问版本、编辑器、操作系统和构建系统、发生了什么、你期望什么，以及复现步骤，并接收诊断包（**C++ Modules: Export Diagnostic Bundle**，或 `mcppls report --bundle`）；没有诊断包时，至少附上诊断报告。上面通知里的 **Report Issue…** 会替你把表单填好。诊断包和报告都已替换你的用户名、主目录、主机名和密钥，也都不含你的文件内容；附上之前仍请先看一遍。mcppls 所规避的 clangd 或 mcpp 缺陷汇总在 [issue #24](https://github.com/Sunrisepeak/mcpp-language-server/issues/24)：提交前先看一遍，如果你的是新问题，就在下面加一条评论。
