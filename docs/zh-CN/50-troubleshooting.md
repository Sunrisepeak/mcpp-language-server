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
| `engines` | clangd 的状态、重启次数、被搁置的文件 |
| `events` | 本次会话的事件日志 |
| `logTail` | 日志的末尾部分 |

日志文件不会随编辑器关闭而消失：报告里写明路径，日志按时间戳保存在缓存目录下。

## 常见症状

**所有功能失效，任何位置都无法跳转到定义。** 看报告里的 `project.source`。如果一个用了构建系统的项目里它是 `inferred`，说明构建工具没有给出答复；原因在 `project.issues` 和 `toolRuns` 的最后一条里。常见原因是构建工具需要下载东西：这时状态栏会提议在你的终端里运行它。

**跳转到定义能用，补全不能用，或者标准库缺失。** 看 `profile`。语义工具包意味着没找到可用的编译器；`import std` 仍然能解析，但诊断来自 libc++，不是来自你的工具链。

**原本正常，后来某个模块突然解析不了了。** `plan.standIns` 列出了没有任何单元提供的模块——mcppls 给它们分配占位单元，这样一个坏掉的模块不会拖垮项目的其余部分，日志里也会逐个写明模块名和原因。真正的问题在 `plan.issues` 里，或者在你的构建本身。

**某个模块编译不过。** 受影响的只有直接或间接导入它的文件：这些文件由 mcppls 自己的引擎立即应答（模块跳转、符号、`import` 补全），在引向失败的那条 import 上带一条 `module-failed` 诊断，并且在失败模块自己的源码或编译命令变化之前不会再交给 clangd；其余文件照常由 clangd 应答。状态会显示 *degraded* 和"N modules cannot be prepared because M failed"，不会一直停在 *preparing*。报告里引擎的 `doomedModules` 和 `filesRoutedToOwnEngine` 会列出它们。

**编辑器找到的编译器和我终端里的不一样。** `toolEnvironment.source` 应该是 `login-shell`。如果是 `editor`，原因在 `toolEnvironment.reason` 里——从桌面项启动的编辑器不带任何 shell 配置。`mcppls.toolEnvironment` 控制这一行为。

**每次启动都很慢。** 第二次会话应该很快：模型连同构建工具所读一切内容的指纹一起被缓存，与之匹配的会话会立即套用计划，并在后台确认。`project.firstOrigin` 会说明发生了哪种情况。如果它一直是 `producer`，说明指纹没有匹配上——该看报告里的 `project.producerRun` 和构建文件的时间戳。

**clangd 反复重启。** 看 `engines[].restarts` 和 `events` 日志。重启之间会拉开间隔，而且十分钟内最多三次；超过之后状态会说明（`engine-restart-capped`），原本想靠重启解决的部分改由 mcppls 自己的引擎应答。模块编译不过从来不是重启的理由。如果重启密集出现，通常说明编译参数一直在变。

## 提交 bug 报告

附上诊断报告。它会写出你机器上的路径，所以先读一遍再附——按设计，它不包含任何环境变量的值，也不包含文件内容。
