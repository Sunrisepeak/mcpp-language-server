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
| adapter 基线 | 当前 PR 插件源码，一次打包；原公开 0.0.19 payload 的所有成员保持相同 | adapter／CI 变更；不依赖新引擎或新服务器的编译与接纳 | 当前插件在稳定 IDE 中驱动旧公开后端的兼容性 |
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

候选触发范围包含服务器源码、模块、版本／payload pins、打包与 adapter。早期
基线在 PR 中仅为 adapter／CI 变更运行。报告分别记录实际插件版本、实际服务端
版本、ZIP SHA256、IDE 版本／build 与断言结果。只上传该公开报告，不上传原始
IDE 日志或工程路径。基线成功不会提升 candidate、台账或发布资格。

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
caret／lookup 操作在 EDT，关闭项目走实际 CloseProject action。单候选补全可由
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

当前 adapter 与上述完整冻结 0.0.19 payload 在 261 的真实模块、CMake 两个
工程阶段已全部通过，Strict IDE 错误检查保留。额外的 C++ 类型错误诊断、实际
模块补全插入、定义跳转和关闭后端进程均有断言。SDK 试用提示的正常 Close
操作放在关闭工程之前，避免 SDK 自身 tooltip 的后台销毁线程错误；没有过滤
该错误。只有完成整个 IDE context 的阶段才记为已验证，失败报告仍保留实际
版本、ZIP 身份与已完成阶段，不把没有认证的功能记为产品失败。

修复后的 0.0.20 原生服务器与旧公开引擎组成独立诊断 ZIP，已有初始化、诊断和
补全的局部结果，但 261 的真实 Goto action 停在平台准备阶段，没有发出
`definition` 请求；正在核对焦点与 indexing 前置条件。这不是正式候选，也没有
把局部成功写成整组通过。完整 candidate 的两个稳定版本仍须经过原接纳门禁。

另保留一个旧 headless 互操作反例：同文档先由 OneEngine 写入 Syntax，随后
Nova 的 `FrontendDaemonHost` 经 RD document 初次绑定回包覆盖为原默认值及
None。旧两参调度与本轮显式 NON_MODAL 对照均能复现；已排除“NON_MODAL
事件不执行”的解释。没有循环重写用户选择，没有新增未经验证的 provider。
临时观测代码全部恢复。原 headless 套件保留，CI 结果仍须据实际检查判断。

262 完整 GUI、当前 PR 的实际 CI／required checks 与正式 payload 尚待记录；
本地基线和原生诊断控制均不提升候选台账或发布资格。
