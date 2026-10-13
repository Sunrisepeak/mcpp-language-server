# 模块工程怎么组织

[English](../25-module-projects.md) | **简体中文**

不按这里做也能构建。这里写的是让大型工程（模块、头文件、第三方代码混在一起）构建快、补全快、也容易保持一致的组织方式。每一条都写了原因，方便你判断什么时候不适用。

## 模块的代价在哪里、什么时候付

模块接口编译一次，得到一个 BMI（二进制模块接口）。导入它的文件加载 BMI，而不是再解析一遍源码。由此有两种代价：

- **构建。** 基于 BMI 编译，比解析生成它的那些头文件便宜得多：在我们测过的一个中等规模工程里，导入一个包装模块的单元，比直接 include 同样那些头文件的单元编译快大约四倍。但改动一个接口，会重建它的 BMI，以及所有直接或间接导入它的 BMI。
- **编辑。** 在导入了模块的文件里，每次补全、悬停、跳转都要为这次请求加载该文件整个导入闭包的 BMI。BMI 越少、越小，答得越快。每个重头文件只出现在一处的闭包，每个声明只加载一次；十个模块各带一份同一头文件的闭包，就要加载十份，编译器在每次请求时都要把它们合并。

下面的规则都由这两种代价推出来。

## 1. 每个重头文件只放进一个包装模块

每个大的第三方头文件（JSON、TOML 库，GUI 框架，`vulkan.hpp`）放进一个专门模块的全局模块片段里，导出你的代码用到的名字：

```cpp
// src/third_party/toml.cppm
module;
#include <toml.hpp>
export module third_party.toml;

export namespace toml {
    using toml::value;
    using toml::parse;
    using toml::find;
}
```

其他模块都 `import third_party.toml`，自己不再 include `<toml.hpp>`。

**原因。** 放在某个模块全局模块片段里的头文件，会成为这个模块 BMI 的一部分。在十个模块里 include，它就在十个 BMI 里；导入其中几个的文件，每次构建、每次补全都要加载每一份并合并重复的声明。放在一个包装模块里，它只解析一次、存一次、加载一次。

## 2. 不要把标准库的 `#include` 和 `import std` 混用

导入了 `std` 的单元里，不要 include 任何标准头文件：既不直接 include，也不通过会 include 它们的头文件间接 include。需要标准库的第三方头文件放进它自己的包装模块（规则 1），它的 `#include <vector>` 就留在包装模块的全局模块片段里。

```cpp
// 避免：同一批声明来自两处
module;
#include <vector>
#include "my_lib.hpp"        // 里面 include 了 <string>、<map>
export module app.core;
import std;
```

```cpp
// 推荐
export module app.core;
import std;
import third_party.my_lib;   // 包装模块 include my_lib.hpp，<string>、<map> 随之留在那里
```

**原因。** `import std` 把标准库作为一个模块带进来；同一单元里的 `#include <vector>` 又带进同一批声明的一份文本副本。编译器要逐对证明它们相同再合并，这很慢；在现在的编译器上，这也是混合工程里 "redefinition"、"ambiguous" 错误最常见的来源。

## 3. 全局模块片段只放接口需要的东西

接口的全局模块片段里，只放它导出的声明会提到的头文件。只有实现需要的头文件放进实现单元：

```cpp
// app/net.cppm —— 接口
export module app.net;
import std;
export namespace app::net {
    struct Response { int status; std::string body; };
    Response get(std::string_view url);
}
```

```cpp
// app/net.cpp —— 实现
module;
#include <curl/curl.h>       // 只在这里：导入方永远不加载它
module app.net;
namespace app::net {
    Response get(std::string_view url) { /* ... */ }
}
```

**原因。** 接口 include 的东西都在它的 BMI 里，每个导入方都要加载。实现单元 include 的东西只影响它自己。改动实现单元不会改变 BMI，导入这个模块的东西都不用重建。

## 4. 函数体放进实现单元

模块接口里，在类内定义的成员函数不像在头文件里那样隐式 `inline`；而改接口单元里的任何函数体，都是在改接口。经常改的函数体放进 `module X;` 实现单元；接口里留下导入方需要看到的东西：声明、模板、`constexpr` 函数、小的 inline 辅助函数。

**原因。** 接口单元的每次改动都会重建它的 BMI，以及导入它的所有 BMI。在实现单元里改函数体，只重建那一个单元。

## 5. 用什么就导入什么，避免层层再导出

`export import` 用于把若干分区汇总成一个门面模块，或者某个类型本身就是模块接口的一部分。不要用它把模块恰好用到的东西转手出去：

```cpp
// 避免
export module app.defines;
export import std;                 // 每个导入 app.defines 的文件都经由它导入了 std
export import third_party.toml;

export module app.tool;
export import app.defines;         // ……每个导入 app.tool 的文件，也拿到了上面全部
```

```cpp
// 推荐
export module app.tool;
import std;
import third_party.toml;
import app.defines;                // 它只导出自己的名字
```

**原因。** `export import` 把整个被再导出的闭包放进每个导入方的可见范围：每次请求加载的 BMI 更多，每个补全列表里的名字更多，导入方还多了一个自己从没声明过的依赖——从链条中间去掉它，很远的文件就会坏。永远不要 `export import std`。

## 6. 大模块按改动的边界拆开

接口有几千行、又被工程大部分代码导入的模块，任何一处改动都会连同它的所有导入方一起重建。按照一起改动的边界把它拆成几个模块（或者门面后面的几个分区）：稳定的核心类型是一块，正在开发的部分是另一块。

**原因。** 一次改动之后重建的量，等于导入被改接口的那些东西的量。依赖图底部是小而稳定的模块、经常改的模块靠近顶部，这个量就小。

## 7. 平台头文件单独包装

```cpp
// src/platform/win32.cppm
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
export module platform.win32;

export namespace win32 {
    using ::HANDLE;
    using ::CreateFileW;
    using ::CloseHandle;
}
```

**原因。** `windows.h` 和 POSIX 头文件很大，还定义了会和普通代码冲突的宏（`min`、`max`、`ERROR`）。宏不会越过模块边界，包装起来以后它们就留在包装模块里。其他代码确实需要某个宏时，把它放进一个只定义这个宏的小头文件。

## 8. 模块名跟目录走

模块按目录命名（`app/net/` 里是 `app.net`），每个目录一个主接口，外面没人导入的部分用分区（`app.net:http`）。这样构建工具、mcppls 和人都能从模块名找到它的源码。

## 模块工程里的纯头文件代码

不导入任何模块的文件（头文件，以及只 include 的单元）照旧工作：引擎在它们里面像不开模块支持时一样从索引补全，不为工程里的模块付出代价。完全没有模块的工程，引擎不开模块支持。上面的规则针对的是导入了模块的文件。

## 看清代价在哪里

- 报告里的 `slowestFiles` 列出补全最慢的十个文件，`engines[].details.buildTimes` 说明每个文件的构建花在哪里：preamble、导入的模块、AST 构建。
- 日志行 `Prerequisites of <file>: <n> modules, <n> reused, <n> built …` 说明一个文件的导入闭包有多大、花了多少（见[出问题时](50-troubleshooting.md)）。
- `Rebuilding module <M>: <why>` 指出引起重建的那次改动。

导入闭包有几十个模块的文件，或者无论改什么都会重建的模块，是这些规则最先见效的地方。
