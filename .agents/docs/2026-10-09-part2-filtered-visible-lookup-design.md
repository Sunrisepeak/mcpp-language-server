# Part 2：补全时按名称加载导入声明的设计边界

状态：Linux 根因证据与私有原型设计，尚未实现生产优化；不导出补丁、不合入 PR。延续 Part 2 的真实语义、200ms、输入失效及资源验收。

原 URI、同一 CDB 的 Qt 配对实验中，全头文件稳定语义执行约 53–56ms，Modules 约 132–136ms。新增私有 `SemaLookup.cpp` 插桩显示：Modules 的 `std` lookup 每次收集 17,144 个声明，枚举约 91–111ms，consumer 阶段约 11–12ms；全头文件收集 5,823 个声明，枚举约 34–42ms，consumer 约 6–7ms。该窗口包含 `Ctx->lookups()`、查找表物化及向量收集，尚不能将全部窗口归为磁盘读取或 `GetDecl`。所有选定 completion 均为真实 typed Sema；这是定位样本，尚非正式性能验收。

`DeclContext::lookups()` 在 namespace 具有 external visible storage 时调用 `ASTReader::completeVisibleDeclsMap()`。后者通过三类序列化表的 `findAll()` 获取 ID，再对全部 ID 执行 `GetDecl`。clangd 的模糊筛选发生在这些工作之后。仅在已有 `lookups()` 迭代器上筛名称，无法消除前面的加载成本。

ASTReader 子阶段计量已完成：6 次 `std` namespace lookup 各读取 17,192 个 ID、生成 2,889 个名称映射；`GetDecl` 累计约 93–108ms，整个 `completeVisibleDeclsMap` 约 97–113ms，差额约 4–5ms。6 次包含 ASTWorker 和 completion，不能解释为 6 个 completion 请求。逐 ID chrono 会增加计量开销，仍只用于归因。原始报告、两阶段插桩 diff、编译/链接参数和日志已归档于引擎 `tests/evidence/part2-linux/visible-lookup-profile/`；三个短报告均通过 typed Sema 控制。

准备验证的最小路线是保留原 BMI 格式和普通 lookup，在补全专用路径枚举序列化名称，先按匹配规则排除必不匹配的 identifier，再加载剩余声明。它不是答案缓存，也不以 index 代替 Sema。需要同时满足：

- 匹配规则与当前 clangd FuzzyMatcher 一致，或采用证明包含全部合法匹配的保守筛选；不能改成 starts-with 丢失模糊结果。空词、无法解释的名称和非 identifier 保留原路径。
- 普通解析、诊断及非补全 lookup 完全沿用完整查找。补全选择性加载不得将整个外部查找表标成已完整加载，也不得污染后续普通名称查找。
- 收集 local、external、module-local、TU-local 表；处理 pending override、merged table 和相同 name 的所有 ID。不能只读取第一张表或选择一个重载。
- using directive、inline namespace 和影响查找遍历的声明始终保留；对相关名称执行原有 shadow、acceptable/visibility、模块可达性与 using-shadow 处理。
- 请求级筛选的有效期覆盖同步 Sema completion，异常、取消和嵌套调用后恢复；不得跨 ASTContext 保存 Decl 指针或跨请求复用未验证状态。
- 优先复用现有表迭代能力；新增存储统计真实内存，不建立隐含持久名称缓存。若更改公共布局或 virtual API，执行全部受影响消费者重建，不能只替换少量对象后运行。

下一步用私有原型验证按名称加载的收益。最小控制涵盖限定 namespace、模糊词、空词、using/inline namespace、重载/模板、导入可见性、普通 lookup 不受影响及取消。真实 Qt 语义和 GCC 插入通过后才做短 A/B；无新增插桩的最终构建再跑完整分布。该路线若不能保全查找语义，则撤回原型，而不是关闭 external loading。

这是已有 UP-25 性能路径的进一步定位，已核对单一登记 #24 的 UP-25；当前不声明新的上游缺陷。具体优化及正式上游补偿状态按 contributing 规则随最终实现补齐。

## 私有原型首轮结果

原型位于 `/tmp/mcppls-part2-filtered-lookup`，从最终 67 源提取 `SemaLookup.cpp`、`ASTReader.cpp` 和私有 `MultiOnDiskHashTable.h`，增加按名称选择的 table 读取；没有更改 BMI 格式、公共布局或 virtual API。仅在带非空、长度≤63补全词的 qualified namespace lookup 中启用。名称筛选使用大小写不敏感子序列，是当前 clangd fuzzy match 的保守超集；非 identifier、非 ASCII、空词及长词回到原路径。临时 thread-local target/pattern 只覆盖该次查找表枚举；递归普通查找与 consumer 回调前恢复原状态，部分加载后保留 external visible storage 标志。

它当前是可执行实验，不是正式接口方案：生产实现还须用显式补全上下文/接口替代私有跨库 thread-local 传递，验证 ordinary lookup、嵌套/取消恢复、表 override/merge、模块可见性与所有相关消费者。

- 实际 clangd FuzzyMatcher 的 124,656 组名称/词对照中，18,934 组匹配未被子序列筛选误删，包含大小写、下划线及长词边界；此有限控制不代替全量语义证明。
- 原 Qt URI/CDB/编译参数，1 次启动、1 轮编辑、4 次 completion：全部 typed Sema 和实际 GCC 选定插入通过。warm 118.29/113.54ms、edited 187.96ms；历史同条件短基线约 175–179/248ms。尚非完整 A/B/A 或正式 p95。
- 每次 `std::ve` 选择性加载 4,649 个 ID、346 个名称，原完整路径约 17,192 个 ID。Qt trace 的稳定语义执行约 73.86–77.46ms，edited 模块验证约 82.71ms。
- 小模块 fixture 通过跨 namespace using、inline namespace、重载，以及 `v_t`→`vector_template` 的非前缀 fuzzy 控制；实际 GCC 插入通过。using 场景基线/候选四次返回的全部 label/kind/实际编辑元组一致。最初 `vr`→`vector` 的 fixture 预期不符合原 FuzzyMatcher 的强匹配规则，该负记录保留，不作为候选正确性或性能结论。

下一步完成正式接口与正确性控制，再在无额外插桩的候选上跑短匹配 A/B/A；确认收益后才扩大至 12 语境与 3×30 验收。当前不能称 PR 达标或 release 完成。

## 显式接口候选与短 A/B/A

独立 worktree `/tmp/mcppls-filtered-lookup-formal` 已实现显式接口候选：consumer 的 `getExternalNameFilter` 默认返回空谓词，clangd recorder 以实际 FuzzyMatcher 提供匹配函数；Sema 只有补全专用 qualified lookup 传递谓词，普通 lookup 保持原接口。ExternalASTSource 的同步入口将谓词绑定到精确 source/context，仅允许一次消费；内部栈作用域在返回时恢复，嵌套普通加载及复合 source 不匹配时沿用完整路径。没有跨库私有 target/pattern 全局符号，也没有新增持久 AST 缓存。内部线程局部栈指针只承载这一同步入口的动态作用域。

consumer 增加 virtual 方法，因此先重建全部 141 个 clangd 消费者与 7 个相关 LLVM 对象，再执行。两套二进制均成功链接；140 项 CompletionTest 通过，包括新增选择性加载后普通名称仍可查找、完整加载仍可关闭 external storage、递归普通调用不接收谓词、取消触发返回后作用域清理，以及 PCH qualified fuzzy/using/inline namespace/重载/空词控制。取消测试证明作用域清理，不是 Sema 全路径即时取消延迟证明。CIndex、interpreter 不在该局部二进制中，后续干净 kit 构建仍需覆盖它们。

无新增诊断插桩的短 A/B/A 已完成，三组均使用原 URI/相同 CDB 字节/参数及独立新建 CDB/cache 目录，各 1 启动×5 轮。全部 36 个选定请求为真实 Sema，全部选定 GCC 插入通过：

| 短样本 | 基线前 | 候选 | 基线后 |
|---|---:|---:|---:|
| warm p95，6 请求/组 | 168.44ms | 112.82ms | 179.36ms |
| edited p95，5 请求/组 | 247.26ms | 187.05ms | 252.40ms |
| edited p50 | 245.48ms | 183.09ms | 240.14ms |

候选观察到的主进程 VmHWM 约 703,780KiB，基线约 732,484/736,628KiB；采样包含冷阶段，未计子进程，不能代替稳定期物理缓存/RSS 资格。证据、完整 diff 与构建配方归档在引擎 `tests/evidence/part2-linux/filtered-lookup-formal/`。候选 diff SHA `aef3a329d3d7628d9a13e6b2cb9c9bee6e37178b4dfbb766f6be05f8bb5fb405`，引擎 SHA `6de880807d5d81c7c98341570d123fb88847928c523fc06042c92a11dba6ddb7`。尚未导出，12 语境及最终 3×30、其他平台、产品与发布长期验证仍未完成。
