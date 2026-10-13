# Agent 与 CI

[English](../40-agents.md) | **简体中文**

编辑器问的是"光标下面是什么"。Agent 问的是"这个符号是什么意思、谁在调用它、我的改动有没有把什么弄坏"——而且它问的时候没有光标、没有窗口，也没有人在看结果。给出的答案与编辑器得到的相同，只是换成了适合 Agent 的形式。

规范见 [S5](../specs/s5-semantic-query.md)，提供的能力如下。

| 能力 | MCP 工具 | 命令行 |
|---|---|---|
| 按名称、标识符或位置查找符号 | `cxx_symbol` | `mcppls query symbol` |
| 引用与调用方——在符号所属模块**及每一个导入它的模块**中搜索，包括再导出 | `cxx_references` | `mcppls query refs` |
| 被调用方 | `cxx_references` | `mcppls query calls` |
| 文件的大纲、模块的描述、模块图 | `cxx_outline`, `cxx_module` | `mcppls query outline\|module` |
| 该文件是以什么方式构建的（参数、标准库、上下文） | `cxx_build_context` | `mcppls query context` |
| 针对磁盘上当前内容的最新诊断 | `cxx_diagnostics` | `mcppls diagnostics` |
| 编辑后验证：改动过的文件及所有导入它们的内容，重新构建一遍 | `cxx_verify` | `mcppls verify --changed` |
| 原地检查一段候选代码，不用把它写入文件 | `cxx_verify` | `mcppls verify` |
| 一处改动影响到什么 | `cxx_impact` | `mcppls impact` |
| 审查一处改动：语义 diff、影响范围、带证据的规则；输出为 SARIF、LSP 或 Markdown | `cxx_review` | `mcppls review --base REV --format sarif` |

相比 grep，使用这些工具的理由有两点：

- **它们按模块来，不是按文件来。**"谁调用了它"的搜索范围是所有导入了其声明模块的模块，而不是恰好出现该名称的文件。
- **它们从编辑器所用的同一个模型给出答案**，这样 Agent 和人看到的是同一份构建。

**答得快，也说清楚是哪一种答案。** 查符号、查引用依赖编译器的答案（悬停、跳转）。编译器一时答得慢时，它之前对同一问题给过的答案可以先顶上，查询不必等上几秒。凡是用到这类答案的结果，都会在 `provenance` 里写明：

```json
"provenance": { "authoritative": false, "answers": 3,
                "notAuthoritative": [ { "source": "cache:K1", "certainty": "uncertain", "ageMs": 41000,
                                        "note": "The location comes from an answer kept 41 s ago; files changed since, so it may be out of date." } ] }
```

`authoritative: true` 表示每个答案都是编译器针对这次问题给出的。传 `fresh: true`（MCP）或 `--fresh`（命令行）可等编译器自己的答案；`cxx_verify`、`cxx_impact`、`cxx_review` 总是这样做。要让所有查询都等，把 `mcppls.answer.agent` 设为 `authoritative`（见[设置](30-settings.md)）。

**依赖模型的审查**是可选的，默认关闭。关闭时，审查只运行确定性规则，服务端完全不发起网络调用。接入方式，以及共享工作区守护进程，见 [editors/agents/README.md](../../editors/agents/README.md)。
