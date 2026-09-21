# 命名与归属

[English](../91-naming.md) | **简体中文**

一个服务端、一个简称、四个编辑器插件；四个应用市场的标识方式各不相同，插件在每个市场里都要能被认出是同一个产品。

## 归属

由 **Sunrisepeak** (<speakshen@163.com>) 以个人项目维护。版权行、包的 `authors` 字段、插件的 vendor 和 publisher 字段，都只写这一个名字。

仓库是 `Sunrisepeak/mcpp-language-server`。如果项目以后迁移到某个组织名下，要改的地方就是下面列出的这些——还有文末讨论的 schema `$id`。

## 各处的名字

| | 取值 | 原因 |
|---|---|---|
| 项目 / 包 | `mcpp-language-server` | 完整拼出它是什么；包索引里用的名字。只用于包本身——每个插件都用下面的简称 |
| 可执行文件、模块命名空间、设置前缀 | `mcppls` | 命令行输入和代码中使用的名字：`mcppls serve`、`import mcppls.lsp`、`mcppls.trace` |
| 插件显示名 | **C++ Modules (mcppls)** | 先说它做什么，方便被搜到；括号里的简称让四个列表读起来像同一个产品 |
| 语义工具包的包名 | `mcppls-kit` | 独立的产物，有自己的版本号，名称取自生成它的程序 |

## 各编辑器里的插件标识

每个应用市场的标识符形状不一样。规则在哪都一样：owner 段是 `sunrisepeak`，product 段是简称。

| 编辑器 | 标识符 | 显示名 | 说明 |
|---|---|---|---|
| VS Code | `sunrisepeak.mcppls` | C++ Modules (mcppls) | `package.json` 里的 `publisher.name`；发布前，发布者必须已经在 Marketplace 上存在 |
| Zed | `mcppls` | C++ Modules (mcppls) | Zed 的扩展 id 是扁平的，简称本身就是 id |
| CLion / IntelliJ | `io.github.sunrisepeak.mcppls` | C++ Modules (mcppls) | JetBrains 要求反向域名；个人项目用 `io.github.<user>` 这种形式 |
| Claude Code | marketplace `mcppls` 里的 `mcppls-lsp` | C++ Modules (mcppls) | 本仓库自己的 marketplace 文件里的一个插件 |

在支持设置和命令的每个编辑器里都用 `mcppls.*`，整个产品用一个名字回答「这叫什么」。

VS Code 扩展是 `sunrisepeak.mcppls`，不是 `sunrisepeak.mcpp-language-server`。包和插件是两回事——xlings 装的是服务端，市场装的是插件——这两个名字里，只有 `mcppls` 已经被可执行文件、设置前缀、模块命名空间和 `mcppls-kit` 一起用着。便于搜索由显示名负责，因此它把「C++ Modules」放在前面。

## Schema 标识符

[`specs/schema/`](../specs/schema/) 下三个 JSON Schema 的 `$id` 是 `https://github.com/mcpp-community/mcpp-language-server/...`。schema 的 `$id` 是标识符，不是链接：使用者靠它识别 schema，改动它就是该 schema 的一次版本事件。在 schema 下一次升级之前，这些 `$id` 保持不变。
