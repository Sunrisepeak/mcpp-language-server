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

## 常见症状

**所有功能失效，任何位置都无法跳转到定义。** 看报告里的 `project.source`。如果一个用了构建系统的项目里它是 `inferred`，说明构建工具没有给出答复；原因在 `project.issues` 和 `toolRuns` 的最后一条里。常见原因是构建工具需要下载东西：这时状态栏会提议在你的终端里运行它。

**跳转到定义能用，补全不能用，或者标准库缺失。** 看 `profile`。语义工具包意味着没找到可用的编译器；`import std` 仍然能解析，但诊断来自 libc++，不是来自你的工具链。

**原本正常，后来某个模块突然解析不了了。** `plan.standIns` 列出了没有任何单元提供的模块——mcppls 给它们分配占位单元，这样一个坏掉的模块不会拖垮项目的其余部分，日志里也会逐个写明模块名和原因。真正的问题在 `plan.issues` 里，或者在你的构建本身。

**某个模块编译不过。** 受影响的只有直接或间接导入它的文件：这些文件由 mcppls 自己的引擎立即应答（模块跳转、符号、`import` 补全），在引向失败的那条 import 上带一条 `module-failed` 诊断，并且在失败模块自己的源码或编译命令变化之前不会再交给 clangd；其余文件照常由 clangd 应答。这是代码本身的问题，所以在出问题的地方以诊断的形式告诉你：状态保持 *ready*（列出类别为 `code` 的 `modules-doomed`），也不会一直停在 *preparing*。报告里引擎的 `doomedModules` 和 `filesRoutedToOwnEngine` 会列出它们。

**状态说明什么，不说明什么。** 代码里的错误（少了 `;`、import 了没有任何单元提供的模块、某个模块编译不过）以诊断的形式出现在出错的位置，也就是 Problems 列表里；状态保持 *ready*。*degraded* 表示服务端丢了本来能给你的功能，并说明丢了什么、在哪里：“clangd stopped responding on main.cpp”、“the workspace is not trusted”、“the macOS SDK was not found”。每个状态 issue 都带有 `category`（`code`、`engine`、`environment`、`project`），说明这是谁的问题；只有 `code` 以外的类别会让状态变成 *degraded*，而且要持续三秒才会显示，所以自己很快就会消失的情况不会出现在状态栏上。

**输入 `import` 时编辑器卡死（0.0.3 及更早版本）。** clangd 23.1 遇到模块名以 `.` 结尾、而且 `.` 就在行尾的文件（`import hello.`、`export module a.`）时永远处理不完，这个文件之后的所有版本都排在它后面等待；而输入任何带点的模块名都会经过这个状态。mcppls 0.0.4 改为把这一行在点后补上 `;` 再交给 clangd，clangd 会立即报告这个错误（规避措施 `WA-CLANGD-001`）；报告里的 `engines[].details.workarounds` 会列出它。

**“clangd would not finish main.cpp”。** 某个文件的构建超出了预算——该文件上次构建耗时的五倍，最少 20 秒——而编辑器还在等它：不管 clangd 忙不忙，它都不会完成这个文件了。这个文件改由 mcppls 自己的引擎应答（提供模块层面的功能），直到它的文本发生变化（让 clangd 卡住的那份文本永远不会再交给它），同时立即重启一个不带这个文件的 clangd。`events` 日志里有一条带具体数字的 `engine-spin`。

**某个规避措施还需要吗？** `--disable-workaround WA-CLANGD-<n>`（可重复）可以关掉一个；日志开头几行会列出正在使用的规避措施。每个规避措施在一致性测试里都有一个对应的检测项（`workaround-canaries`），clangd 更新修好了对应缺陷后，这个检测项就会失败。

**编辑器找到的编译器和我终端里的不一样。** `toolEnvironment.source` 应该是 `login-shell`。如果是 `editor`，原因在 `toolEnvironment.reason` 里——从桌面项启动的编辑器不带任何 shell 配置。`mcppls.toolEnvironment` 控制这一行为。

**每次启动都很慢。** 第二次会话应该很快：模型连同构建工具所读一切内容的指纹一起被缓存，与之匹配的会话会立即套用计划，并在后台确认。`project.firstOrigin` 会说明发生了哪种情况。如果它一直是 `producer`，说明指纹没有匹配上——该看报告里的 `project.producerRun` 和构建文件的时间戳。

**clangd 反复重启。** 看 `engines[].restarts` 和 `events` 日志。重启之间会拉开间隔，而且十分钟内最多三次；超过之后状态会说明（`engine-restart-capped`），原本想靠重启解决的部分改由 mcppls 自己的引擎应答。模块编译不过从来不是重启的理由。如果重启密集出现，通常说明编译参数一直在变。

**“clangd stopped making progress; it was restarted”。** clangd 有请求一直没答，期间也没答任何别的请求，并且五秒内几乎没用 CPU：它在等一个不会来的东西，而不是在编译（编译会一直占着一个核，这种情况不会被打断）。`events` 日志里有一条带具体数字的 `engine-stuck`。已经观察到 clangd 23.1 在某个模块的源文件一秒内被改两次之后出现这种情况。在 Windows 上服务端读不到 clangd 的 CPU 时间，所以检测不到；clangd 不再应答的文件仍会被逐个搁置。

**“The bundled clangd cannot run on this system”。** clangd 根本没有启动起来：系统的程序加载器拒绝了它，加载器的原话在状态和日志里（例如 ``version `GLIBCXX_3.4.30' not found``）。重启改变不了这一点，所以不会再重启；这期间由 mcppls 自己的引擎应答模块跳转、`import` 补全和模块诊断。在 Linux arm64 上，内置的 clangd 需要 glibc 2.34 和 GCC 12 的 libstdc++——它能运行的系统列在[安装指南](00-install.md)里。其他平台上出现这个提示，通常是 musl 系统（Alpine），或者 payload 损坏了。由编辑器自己启动 `mcppls` 的，可以用 `--clangd PATH` 换成你自己的 clangd（23.1 或更新）。

## 提交 bug 报告

附上诊断报告。它会写出你机器上的路径，所以先读一遍再附——按设计，它不包含任何环境变量的值，也不包含文件内容。
