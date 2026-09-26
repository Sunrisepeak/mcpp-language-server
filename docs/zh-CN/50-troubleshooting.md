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
| `events` | 本次会话的事件日志 |
| `logTail` | 日志的末尾部分 |

日志文件不会随编辑器关闭而消失：报告里写明路径，日志按时间戳保存在缓存目录下。

报告本来就是为了给别人看的：其中你的主目录写作 `~`，用户名和主机名写作 `<user>`、`<host>`，看起来像密钥的内容（token、密码、API key、邮箱）写作 `<redacted>`。工程自己的路径保留——看报告要的正是它们。

**C++ Modules: Export Diagnostic Bundle** 更进一步：生成一个 zip，写在缓存目录的 `bundles/` 下（保留最新的 5 个），**从不上传**，里面是排查问题通常需要的全部内容——

| 问题包里的文件 | 内容 |
|---|---|
| `report.json` | 上面的报告 |
| `environment.json` | 系统、编辑器和插件的版本、其他 C/C++ 插件、你的 mcppls 设置、payload、探测到的工具链，以及少数几个环境变量（`PATH`、`LANG`、`LC_*`、`MCPP_*`、`XLINGS_*`），其他的一概不收 |
| `logs/` | 服务端最近三次会话以及最近一天内其他会话的日志，还有插件自己的日志 |
| `incidents/` | clangd 崩溃、卡住或文件被搁置时服务端记下的现场 |
| `engine/` | 交给 clangd 的数据库，以及生成它的计划 |
| `manifest.json` | 每个文件的大小和 SHA-256，以及每条脱敏规则各替换了多少处 |

——每个文件都做同样的替换。写出之前，会在整个问题包里按各种写法搜索你的主目录、用户名和主机名；只要还有残留，**就不写出问题包**，提示会说明是哪个文件，并可以选择 *Retry with Project Paths Hidden*，把工程路径也一并替换。源文件从不打包；事故记录只带与问题相关的那几行。其他编辑器用 `workspace/executeCommand` 的 `mcppls.exportBundle` 执行同一操作，命令行则是：

```bash
mcppls report --bundle problem.zip --root path/to/project   # 可加 --hide-project-paths、--no-source-excerpts
```

崩溃转储（dump）默认不包含（需要时用 `--include-dumps`）：它装的是内存，无法脱敏。`--no-redact` 保留一切原样，只用于在你自己的机器上排查；编辑器里不提供这个选项。

## 常见症状

**所有功能失效，任何位置都无法跳转到定义。** 看报告里的 `project.source`。如果一个用了构建系统的项目里它是 `inferred`，说明构建工具没有给出答复；原因在 `project.issues` 和 `toolRuns` 的最后一条里。常见原因是构建工具需要下载东西：这时状态栏会提议在你的终端里运行它。

**跳转到定义能用，补全不能用，或者标准库缺失。** 看 `profile`。语义工具包意味着没找到可用的编译器；`import std` 仍然能解析，但诊断来自 libc++，不是来自你的工具链。

**原本正常，后来某个模块突然解析不了了。** `plan.standIns` 列出了没有任何单元提供的模块——mcppls 给它们分配占位单元，这样一个坏掉的模块不会拖垮项目的其余部分，日志里也会逐个写明模块名和原因。真正的问题在 `plan.issues` 里，或者在你的构建本身。

**某个模块编译不过。** 受影响的只有直接或间接导入它的文件：这些文件由 mcppls 自己的引擎立即应答（模块跳转、符号、`import` 补全），在引向失败的那条 import 上带一条 `module-failed` 诊断，并且在失败模块自己的源码或编译命令变化之前不会再交给 clangd；其余文件照常由 clangd 应答。这是代码本身的问题，所以在出问题的地方以诊断的形式告诉你：状态保持 *ready*（列出类别为 `code` 的 `modules-doomed`），也不会一直停在 *preparing*。报告里引擎的 `doomedModules` 和 `filesRoutedToOwnEngine` 会列出它们。

**状态说明什么，不说明什么。** 代码里的错误（少了 `;`、import 了没有任何单元提供的模块、某个模块编译不过）以诊断的形式出现在出错的位置，也就是 Problems 列表里；状态保持 *ready*。*degraded* 表示服务端丢了本来能给你的功能，并说明丢了什么、在哪里：“clangd stopped responding on main.cpp”、“the workspace is not trusted”、“the macOS SDK was not found”。每个状态 issue 都带有 `category`（`code`、`engine`、`environment`、`project`），说明这是谁的问题；只有 `code` 以外的类别会让状态变成 *degraded*，而且要持续三秒才会显示，所以自己很快就会消失的情况不会出现在状态栏上。

**输入 `import` 时编辑器卡死（0.0.3 及更早版本）。** clangd 23.1 遇到模块名以 `.` 结尾、而且 `.` 就在行尾的文件（`import hello.`、`export module a.`）时永远处理不完，这个文件之后的所有版本都排在它后面等待；而输入任何带点的模块名都会经过这个状态。mcppls 0.0.4 改为把这一行在点后补上 `;` 再交给 clangd，clangd 会立即报告这个错误（规避措施 `WA-CLANGD-001`）；报告里的 `engines[].details.workarounds` 会列出它。

**开着自动保存输入 import 时，这个文件有几秒钟不走 clangd。** clangd 从磁盘上的文本读取一个文件的 import，而不是从编辑器里读（0.0.4 及更早版本会就此永久卡住：`import hello.` 被自动保存后，这个文件的所有请求都得不到应答）。一次保存把 clangd 会卡住的内容写到磁盘上时——以 `.` 结尾的模块名，或者 import 了项目里（还）没有的模块——这个文件改由 mcppls 自己的引擎应答，直到再次保存；状态里以 `file-unsafe-on-disk`（类别 `code`）列出它并说明原因，状态仍是 *ready*。没有任何单元提供的模块会在保存后一秒内得到替身单元，等 clangd 读到带替身的数据库（大约六秒后），文件就交还给 clangd。

**“Import directive must end with a ';'” 标在了别的行上，或刚输入的 import 报 “module X not found”。** clangd 把缺少 `;` 的指令报在它后面的代码上；mcppls 会把这条诊断移回指令所在行（规避措施 `WA-CLANGD-006`）。刚输入、还没保存的 import，clangd 要等文件保存后才会构建（它从磁盘读取 import）；只要这个模块在项目里，这时给出的是信息级提示 “module 'X' is in the project; clangd loads it once the file is saved”，而不是错误（`WA-CLANGD-007`）。

**“clangd would not finish main.cpp”。** 某个文件的构建超出了预算——该文件上次构建耗时的五倍，最少 20 秒——而编辑器还在等它：不管 clangd 忙不忙，它都不会完成这个文件了。这个文件改由 mcppls 自己的引擎应答（提供模块层面的功能），直到它的文本发生变化（让 clangd 卡住的那份文本永远不会再交给它），同时立即重启一个不带这个文件的 clangd。`events` 日志里有一条带具体数字的 `engine-spin`。

**某个规避措施还需要吗？** `--disable-workaround WA-CLANGD-<n>`（可重复）可以关掉一个；日志开头几行会列出正在使用的规避措施。每个规避措施在一致性测试里都有一个对应的检测项（`workaround-canaries`），clangd 更新修好了对应缺陷后，这个检测项就会失败。

**编辑器找到的编译器和我终端里的不一样。** `toolEnvironment.source` 应该是 `login-shell`。如果是 `editor`，原因在 `toolEnvironment.reason` 里——从桌面项启动的编辑器不带任何 shell 配置。`mcppls.toolEnvironment` 控制这一行为。

**每次启动都很慢。** 第二次会话应该很快：模型连同构建工具所读一切内容的指纹一起被缓存，与之匹配的会话会立即套用计划，并在后台确认。`project.firstOrigin` 会说明发生了哪种情况。如果它一直是 `producer`，说明指纹没有匹配上——该看报告里的 `project.producerRun` 和构建文件的时间戳。

**clangd 反复重启。** 看 `engines[].restarts`、`engines[].details.restartBudget` 和 `events` 日志。每种原因各有十分钟三次的重启额度：引擎数据库变化（`plan`）、恢复停止应答或空转的 clangd（`recovery`）、clangd 退出（`crash`）。用完之后，同类的下一次重启依次等待一、二、四、八分钟——是退避而不是拒绝，所以卡住的 clangd 总能恢复——状态会说明（`engine-restart-capped`）并提供 **Restart clangd** 按钮（其他编辑器用 `workspace/executeCommand` `mcppls.restartEngine`），它立即重启且从不计入额度。切换工具链、profile 或 context 也从不计入，模块编译不过从来不是重启的理由。引擎数据库的每次变化都会记入日志并写明改了什么（`engine database changed: … compiled otherwise (main.cpp: argument 3: -O0 -> -O2)`），重启密集时能直接看到原因。

**“clangd crashed while building NormalJsonTranslator.Core.cpp”。** clangd 会说明它在哪个文件上崩溃（崩溃上下文），隔离的就是这个文件：它改由 mcppls 自己的引擎应答，同时重启一个不带它的 clangd。报告里的 `engines[].details.lastExit` 有退出码、文件、clangd 当时在做什么，Windows 上还有异常码。五分钟内退出五次，clangd 在下次服务启动前不再使用；状态会提供 **Export Diagnostic Bundle**。

**“mcpp could not describe tools/updater/mcpp.toml”。** 构建工具描述了工作区的其余部分，并说明了它没能描述的那一部分（例如某个成员的构建程序失败）；那部分的文件按其余部分提供的信息来读，其余部分照常工作（`producer-partial`，S2 0.3.0）。修好消息里指出的问题，下次重新加载就会一并描述它。

**“clangd rejected the compile command for module scanning”。** clangd 在构建模块之前，会用数据库里每个单元自己的编译命令扫描它的 import；编译器驱动拒绝的命令会让扫描失败，模块也就一个都不会构建——issue #23 的 `LTO requires -fuse-ld=lld` 就是这种情况。状态会用驱动的原话写出第一处拒绝（类别 `environment`），`engines[].details.scanFailures` 记录次数。命令找不到头文件时也这样说明（类别 `project`）。正在输入的文件扫描失败是常态，只计数。

**“C++26 was disabled in precompiled file”。** 某个模块用一种 C++ 标准构建，却在另一种标准下被导入；clang 会拒绝。mcppls 对同一上下文中的模块单元统一按其中最新的标准来读（报告里的 `plan.languageStandard`、状态 profile 里的 `standard`），所以这条错误只应来自 0.0.5 之前 clangd 构建的模块（下次改动时会重建），或者构建本身就混用了标准——那样构建工具自己的编译器也会拒绝。

**刚打开项目时，最长一分钟内只有模块层面的功能。** 识别出构建系统的项目，要等构建工具描述完项目（最长一分钟，即构建工具自身的时限）才把它交给 clangd，而不是先给 clangd 一个从源码猜出来、之后还要推翻的模型；这期间由 mcppls 自己的引擎提供模块跳转、import 上的悬停和 `import` 补全。第二次会话会直接从缓存的模型开始。

**服务端把出错的现场记在哪里。** 崩溃、clangd 卡住或空转、文件被隔离、重启被推迟、某个规避措施的前提被发现不成立，每一种都会留下一份“事故”：工作区缓存下的一个目录（`incidents/<UTC 时间>-<类型>/`，保留最近二十份、一周内），里面有事发前的经过、clangd 最近的日志（clangd 以 `info` 级别记录到内存，从不写进默认日志）、每个相关文件在编辑器与磁盘上不同的那几行，以及 clangd 哪个线程在占用 CPU。诊断包会带上它们。

**“clangd stopped making progress; it was restarted”。** clangd 有请求一直没答，期间也没答任何别的请求，并且五秒内几乎没用 CPU：它在等一个不会来的东西，而不是在编译（编译会一直占着一个核，这种情况不会被打断）。`events` 日志里有一条带具体数字的 `engine-stuck`。已经观察到 clangd 23.1 在某个模块的源文件一秒内被改两次之后出现这种情况。在 Windows 上服务端读不到 clangd 的 CPU 时间，所以检测不到；clangd 不再应答的文件仍会被逐个搁置。

**“The bundled clangd cannot run on this system”。** clangd 根本没有启动起来：系统的程序加载器拒绝了它，加载器的原话在状态和日志里（例如 ``version `GLIBCXX_3.4.30' not found``）。重启改变不了这一点，所以不会再重启；这期间由 mcppls 自己的引擎应答模块跳转、`import` 补全和模块诊断。在 Linux arm64 上，内置的 clangd 需要 glibc 2.34 和 GCC 12 的 libstdc++——它能运行的系统列在[安装指南](00-install.md)里。其他平台上出现这个提示，通常是 musl 系统（Alpine），或者 payload 损坏了。由编辑器自己启动 `mcppls` 的，可以用 `--clangd PATH` 换成你自己的 clangd（23.1 或更新）。

## 提交 bug 报告

附上问题包（**C++ Modules: Export Diagnostic Bundle**，或 `mcppls report --bundle`），至少也附上诊断报告。两者都已替换你的用户名、主目录、主机名和密钥，也都不含你的文件内容；附上之前仍请先看一遍。
