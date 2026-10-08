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
