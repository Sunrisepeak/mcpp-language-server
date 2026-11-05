---
subject: clion-stable-runtime-validation
status: active
---

# CLion 稳定版的打包插件运行验证

读者：维护 mcppls 插件兼容性与发布资格的贡献者。
本文回答：如何让同一个实际插件包在两个稳定 IDE 中验证，而不等待新的引擎
资格或把旧后端的成功误记为新后端通过。
范围之外：放宽引擎接纳、以静态符号检查代替界面功能，或发布未合格的后端。

日期：2026-11-05。关联 mcpp#885、mcpp-language-server#75。

## 验证入口与依赖

| 入口 | 输入 | 启动条件 | 能证明什么 |
|---|---|---|---|
| 当前服务器早期回归 | 当前 adapter 与既有 Linux x64 release 行编译的当前原生服务器；公开 0.0.19 engine／kit 全部成员保持相同；一次打包 | 所有影响 adapter／服务器／payload 的变更；等待原 build，不依赖 candidate 接纳 | 当前 adapter 与当前服务器在冻结公开引擎上的双稳定兼容性 |
| candidate | 本轮 qualified payload，经原 headless 套件后一次 buildPlugin | 原引擎接纳、payload、headless 检查均成功 | 实际新候选组合的双稳定 IDE 运行结果 |
| 独立发布回归 | 已发布的插件 ZIP | workflow_dispatch | 指定公开发布包的运行兼容性 |

三个入口都运行同一个独立 Gradle 测试项目；它没有插件 main output 依赖，
不会为每一个测试 IDE 重新编译插件。固定矩阵为 CLion 2026.1.5.2
（261.27258.141）与 2026.2.4.1（262.11879.106）。原 Plugin Verifier 与
2026.2.3 headless 源码测试保留。

测试以真实 IDE 打开带空格和 Unicode 的合成工程，验证 provider 注册、server
Running 与实际版本、内置 payload 的启动、编辑器中的缺失 import 诊断、真实
编译器的真实 C++ 类型错误诊断、补全弹窗里带 mcppls 标记的模块项、选择后的插入、到导出定义的跳转，以及关闭
工程后的进程退出。第二个 CMake 工程验证默认不被接管。没有外部 mcpp 或语言
服务器的 PATH，HOME 与缓存隔离；不会读取用户现场工程或日志。

候选与早期回归的触发范围包含服务器源码、模块、版本／payload pins、打包与
adapter。报告分别记录实际插件版本、实际服务端版本、ZIP SHA256、IDE 版本／build
与断言结果。既有 Linux x64 release 编译行上传小型原生服务器 artifact，
早期打包只等待 build 与变更分类；八个 build 矩阵项都在 Ubuntu 交叉编译，
不等待 macOS runner，但任一 build 失败仍会阻止该回归。没有新增服务器重复编译
或 LLVM 构建。原 candidate 的 server／payload／引擎接纳依赖保持不变。
只上传该公开报告，不上传原始 IDE 日志或工程路径。早期回归成功不会提升
candidate、台账或发布资格。

## 真实 GUI 找到的调度缺口

公开 0.0.19 插件在选择 “Use mcppls Only” 后打开另一个源文件的实际运行中，
Strict IDE 错误栈经过 `OneEngine.fileOpened → onEdt → lower`，随后
`HighlightingSettingsPerFile` 的改变触发 PSI cache 清理，进入 TransactionGuard
的 write-unsafe 检查。该处源码与 PR75 修改前相同。

后台回调的默认 modality 不保证模型修改的写安全。修正仅把已有 EDT 调度明确
为 `ModalityState.nonModal()`，保留 disposed 条件、用户选择与还原逻辑，不关闭
线程或写安全检查。该行为符合 JetBrains 的
[线程与 modality 规则](https://plugins.jetbrains.com/docs/intellij/threading-model.html)。
真实旧包保留为失败对照；修正后的实际 ZIP 仍由严格的 IDE 错误处理器检查。

## 输入暂停期间的请求生命周期

固定公开 0.0.19 后端的独立重放得到明确反例：同一文档同一版本连续发送三个
`documentSymbol`，以及三个位置不同的 `documentHighlight`，没有任何客户端取消，
前两个请求均收到 `-32801`，最后一个成功。服务端 `Typing.waiting` 按方法覆盖
请求，既不能区分参数，也没有证明内容发生异常变化。这是 mcppls 通用引擎调度
缺陷，不是 CLion 插件应通过隐藏异常补偿的行为。
[LSP 3.17](https://github.com/microsoft/language-server-protocol/blob/gh-pages/_specifications/lsp/3.17/specification.md)
将正常编辑与异常内容变更区分，正常的队列内变化不应使用该错误码。

修正保留每个请求的完整参数、到达顺序、回调和原始截止时间；实际客户端取消
只移除其 ID。暂停结束后仍有效的请求转交引擎，超时、文件关闭或引擎退出时
完成回调。没有通过更换错误码、关闭严格 IDE 检查或放宽超时隐藏问题。

新增生命周期测试还发现两处旧路径缺口：诊断请求以 map key 的引用结束暂停，
删除该元素后继续使用 URI，导致悬空引用；引擎 shut_down 只停止进程，没有完成
已排队请求。前者以 URI 值所有权修正，后者在停止接纳后完成未发出与已发出的
客户端请求。测试同时检查同版本、跨版本、跨文档、不同位置、真实取消、超时、
关闭文件与退出后每个 future 的归宿；typing-pause fixture 追加实际协议重放。
这些变更不修改 clangd、引擎 pins 或上游版本组。

## 测试适配与产品问题分离

Remote Driver 本身要求读写语义：读取 daemon highlights 放在 read-action 中，
caret／lookup 操作在 EDT，关闭项目走实际 CloseProject action。定义跳转在实际
编辑器取得焦点、索引完成且项目服务确认 `mcppls-only` 后发送真实 Ctrl+B，
并检查选中的编辑器确实打开导出模块。单候选补全可由
平台直接插入，因此 fixture 有两个真实同前缀模块，以验证弹窗选择；不把这些
测试适配问题当作产品缺陷。

本地并行 IDE 除 DISPLAY 外还共享固定 Driver 端口；本地 GUI 验证串行占用，
编译、证据处理及原 UX 实验继续并行。CI 的两个矩阵项使用独立 runner，自然
隔离。框架与 Driver 固定为 262.10968.63，避免未固定的 EAP 依赖。生命周期
按官方 [集成测试模型](https://plugins.jetbrains.com/docs/intellij/integration-tests-intro.html)
启动并关闭实际 IDE。

## 证据状态

本地 canonical 2026.2.3 SDK 已打包当前 0.0.20 adapter，内置公开 0.0.19
payload 的 3598 个文件成员逐一 SHA256 相同。ZIP SHA256 为
`cbacfb0443e92d4996d9d61d858bde24e938e01e5fdb836bceb307fa253d9d00`。
字节码核验确认调用显式 NON_MODAL 的调度重载。

同一 ZIP 已通过 Plugin Verifier 1.411 的两个稳定 SDK，Internal API 使用为零。
261 有 21 处 deprecated 警告，262 另有 1 处 scheduled-for-removal 警告；门禁未过滤。
这仅证明静态 API 兼容，不能替代真实功能运行。

早前 261 本地基线的模块与 CMake 两阶段通过，但当时尚未追加显式索引完成、
编辑器焦点前置，不能替代当前完整测试流程。SDK 试用提示的正常 Close 操作
放在关闭工程之前，避免 SDK tooltip 的后台销毁线程错误；没有过滤错误。
只有完成整个 IDE context 的阶段才记为已验证。

CI 42007093672 的实际 ZIP 已冻结，SHA256 为
`ae373842604e1c230d2bd099ac0b54ccf8f8d06d4ccd6a0473c9b54bbc7009b1`。
它与上述本地 ZIP 的全部 payload 文件及 adapter class 成员相同，仅构建 Manifest
不同。CI 42013684711 的两个稳定 IDE 都准确失败于 `MODULES.NAV_DEFINITION`；
固定同一个 CI ZIP 的 261 本地重放完成 17 项前置后同样失败。

单变量对照保留该 ZIP、项目、`mcppls-only`、索引、焦点、120 秒导航期限、
3 分钟 context 期限与 Strict IDE 错误门禁。原测试以 SDK `invokeAction` 调用
`GotoDeclaration`；固定 Driver 实现将 inputEvent、component 和 place 均传 null。
改为向已经取得焦点的真实编辑器发送默认键位 Ctrl+B 后，261 完整模块与 CMake
两阶段的 30 项检查通过，包括到正确导出模块的跳转与后端退出。两个对照的持久化
项目设置都为 `mcppls-only`；正式测试还直接断言项目服务的实际 effective 值。

这说明原测试入口未提供真实键盘事件对应的编辑器上下文，不能将该失败直接归为
服务器、SDK 或 IDE 产品缺陷。协程采样没有证明某个具体 awaiter；失败对照的
原生动作后端记录了 `GenericBackendActionOperationHandler` 的 requirements-not-ready
与 ActionUnavailable，范围只限该动作准备路径。没有通过直接 LSP 请求代替导航，
也没有删掉索引、焦点或选中目标断言。

修复后 0.0.20 原生服务器与旧公开引擎组成的独立诊断 ZIP 曾完成初始化、诊断与
补全部分；它与冻结 CI 基线的输入不同，不能作为后者的完整资格，也不是正式候选。
完整 candidate 的两个稳定版本仍须经过原接纳门禁。

另保留一个旧 headless 互操作反例：同文档先由 OneEngine 写入 Syntax，随后
Nova 的 `FrontendDaemonHost` 经 RD document 初次绑定回包覆盖为原默认值及
None。旧两参调度与本轮显式 NON_MODAL 对照均能复现；已排除“NON_MODAL
事件不执行”的解释。没有循环重写用户选择，没有新增未经验证的 provider。
临时观测代码全部恢复。原 headless 套件保留，CI 结果仍须据实际检查判断。

262 使用同一冻结 CI ZIP、真实键盘入口与实际项目服务设置断言，完成 19 项检查，
包括 `NAV_DEFINITION` 与 `MODULE_CLOSE`，但 Strict context 关闭时因公开 0.0.19
后端返回 `the file changed again` 而失败。错误在导航键盘事件之前已发生；本轮
堆栈只证明 LSP response error，未单独捕获该次请求的方法与 ID，不能把它计为
某一方法的精确重放。该消息与前述旧队列反例一致，但不据此推断该次请求的方法与 ID；
修复后组合的同流程结果见下文。没有抑制错误，262 整轮不记为通过。

修复后的固定原生 0.0.20 诊断 ZIP（SHA256
`2e5f7a3eb3b8e87c862a49297564125c2d5b7607cc36cbc000def52c33e57ed0`）
在 261 与 262 均通过完整 30 项检查，Strict、模块与 CMake 两阶段全部通过。
两版均直接确认实际 `mcppls-only`、真实 Ctrl+B 的正确导出定义与关闭后的后端退出。
其 3593 个 engine／kit 文件成员及 47 个 adapter class 与冻结 CI 基线逐字节相同。
该结果证明本轮当前 adapter／修复后服务器／公开引擎组合可用；因为服务器载体与
版本都改变，不将其表述成对单一内部实现行的性能或因果证明。

早期 CI 因此使用当前原生服务器与冻结公开 engine／kit，避免把已知旧服务器缺陷
永久设为当前修复的必过前提。公开 0.0.19 输入仍作为错误对照保留。打包步骤验证
原生服务器版本与 SHA256，替换 manifest 中服务器身份，记录当前源码 commit 与
非 candidate 标记，并重新核对所有 engine／kit 成员未变。

修正后的实际 CI／required checks 与正式 payload 尚待记录。本地早期回归和原生
诊断控制均不提升候选台账或发布资格。

报告只包含固定枚举 stage、projectKind、逐项 passedChecks、实际公开版本和 ZIP
摘要；失败控制台只打印固定阶段。原异常和 Strict IDE 检查仍使测试失败，原始
JUnit／IDE 日志不上传。缺失 IDE 的负向控制确认报告仍存在且控制台不暴露输入路径。
