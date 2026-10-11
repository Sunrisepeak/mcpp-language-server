# 0.0.12：mcpp 构建程序（build.mcpp）的语义支持（#51）

日期：2026-10-11。状态：**已实现**，随 0.0.12（PR #46）发布。

上游：mcpp 2026.10.11.1（mcpp-community/mcpp#797，PR #798、#800）。mcpp 把 `build.mcpp`、`[build-dependencies]` 的库和引擎接口编进同一条计划，即构建程序图，并在 S1 文档中以 `build-program` 配置的集合描述它（mcpp SPEC-005 R3.13）。

## 1. 问题

用 mcppls 0.0.12 打开 mcpp 2026.10.11.1 的工程时，`build.mcpp` 没有任何语义，定义、悬停、补全全部超时。原因是 mcpp 的编译命令用 `-x c++` 声明 `.mcpp` 的语言，而 GNU/MSVC 参数翻译会删除构建自带的所有语言写法。clang 驱动无法从 `.mcpp` 推断语言，clangd 因此报 "expected exactly one compiler job"。

除此之外，issue #51 还列出了以下需求：

- 按配置选择集合；
- 构建程序集合内的模块解析；
- 引擎接口只读；
- 跨配置导入的诊断（SPEC-007 P1）；
- 编辑器语言关联；
- 旧版 mcpp 的版本门槛。

## 2. 决策

| # | 决策 |
|---|---|
| D1 | 计划在翻译后重新声明单元的语言。驱动可识别的扩展名维持原状；无法识别的（`.mcpp`）追加 `-x c++`；可导入单元追加 `-x c++-module`。判断依据为 `project::clang_names_language`，它对齐 clang `Types.cpp` 的扩展名表。 |
| D2 | 默认上下文先放目标集合、后放构建程序集合。两种配置都编译的源文件（同时出现在两张表中的包）按目标的参数读取。构建程序的工具链与目标不同时（交叉构建），默认上下文不读取它们，由各自的上下文读取。 |
| D3 | 默认上下文中，若目标与构建程序提供同名模块、源文件不同（例如同一个包的两个版本），保留目标的提供者。构建程序的那一份记入 `otherContextSources`，不报 `ambiguous-module`，因为两种配置互不可见，构建本身不存在歧义。 |
| D4 | 原生模块索引知道每个单元所属的配置。跨配置导入在 import 处报错：目标导入构建程序的模块报 `build-program-module`，引擎接口与 `[build-dependencies]` 的库各有一条消息；`build.mcpp` 导入目标的模块报 `target-module`。导入补全只给出本配置可见的模块；模块名跳转和悬停优先本配置的提供者。 |
| D5 | 引擎接口（集合 `mcpp:core`）只读，对它的格式化和 codeAction 返回 `[]`，prepareRename 返回 `null`，rename 返回 `RequestFailed`。从项目文件发起、但结果会改到引擎接口的 rename 被整体拒绝，不做部分修改。VS Code 通过 `configurationDefaults` 设置 `files.readonlyInclude: **/.mcpp/engine/*/modules/**`。 |
| D6 | 当前上下文不读取的构建程序，以及旧版 mcpp 未描述的 `.mcpp` 文件，都不交给 clangd：未进入计划的 `.mcpp` 文件先等待计划，再被排除。文件上给出一条 information 级诊断 `build-program-context`，说明由哪个上下文读取，或者从哪个 mcpp 版本起才有描述（`BUILD_PROGRAMS_MCPP_VERSION`）。未被读取的文件不做模块诊断。 |
| D7 | 各编辑器把 `.mcpp` 关联为 C++：VS Code、Neovim、CLion、Claude Code 和 Copilot CLI 由插件完成；Zed 的扩展无法给内置语言增加后缀，改为在 README 中给出 `file_types`。 |

## 3. 规范

- S1 §7 增加一段说明性文字：构建自己运行的程序构成独立配置，`visible-sets` 只列出同一配置内的集合。§7.1 中 `configuration` 字段的说明提及 `build-program`。profile 版本不变。
- `validate.py` 中的 mcpp 契约增加构建程序集合的检查：名称、配置内可见性、`mcpp:std:host`、`<package>:build` 为 executable、`mcpp:core` 提供 `mcpp` 与 `mcpp.core`。

## 4. 测试

| 层 | 内容 |
|---|---|
| 单元 | `test_normalize`：`.mcpp` 单元的 `-x c++`；默认上下文的顺序；同一个包两个版本时的处理；交叉工具链下的默认上下文与 `app:build` 上下文。`test_server`：跨配置诊断、补全过滤、按配置跳转。 |
| 真实 mcpp | `mcpp-build-programs`：15 项检查，覆盖诊断、定义、悬停、补全、rename 拒绝、跨配置诊断和工作区不变，运行于 linux-x64、darwin-arm64 和 win32-x64。CI 在固定版本旁安装 `MCPP_BUILD_PROGRAMS_VERSION`（2026.10.11.1），默认版本保持不变；mcpp 输出的文档同时经过规范校验。 |
| 模拟 | `mcpp-build-programs-cross`：根据真实输出生成，并把构建程序改为第二个工具链。覆盖默认上下文的提示、`app:build` 上下文、引擎接口只读（格式化、codeAction、prepareRename、rename）。`mcpp-emit` 增加一个旧版 mcpp 未描述的 `build.mcpp`。 |
| VS Code | e2e 断言 `.mcpp` 已关联到 cpp，并断言 VS Code 实际采用了 `files.readonlyInclude` 的默认值。 |

## 5. 兼容性

旧版 mcpp 和其他构建系统的文档中没有 `build-program` 集合，计划、诊断和补全的行为都不变。唯一可见的变化是：打开的 `.mcpp` 文件不再交给 clangd 猜测命令，文件上会出现一条说明。
