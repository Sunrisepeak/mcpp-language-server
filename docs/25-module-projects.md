# Laying out a module project

**English** | [简体中文](zh-CN/25-module-projects.md)

Everything here builds without it. These are the layouts that keep a large project — modules,
headers and third-party code mixed — fast to build, fast to complete in, and easy to keep
consistent. Each comes with the reason, so you can tell when it does not apply.

## What a module costs, and when

A module interface is compiled once into a BMI (a binary module interface). A file that imports it
loads the BMI instead of parsing the source again. Two costs follow from that:

- **Building.** Compiling against a BMI is much cheaper than parsing the headers it was made from:
  in a mid-sized project we measured, a unit compiled about four times faster against a wrapper
  module than against the same headers included directly. But an edit to an interface rebuilds its
  BMI, and the BMIs of everything that imports it, directly or not.
- **Editing.** A completion, a hover or a jump in a file that imports modules loads the BMIs of the
  file's whole import closure for that request. The fewer and smaller those BMIs, the faster the
  answer. A closure with each heavy header in one place loads each declaration once; a closure where
  ten modules carry their own copy of the same header loads ten copies, and the compiler merges them
  on every request.

The rules below follow from these two costs.

## 1. Each heavy header in exactly one wrapper module

Put each large third-party header (a JSON or TOML library, a GUI toolkit, `vulkan.hpp`) in the
global module fragment of one module of its own, and export the names your code uses:

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

Every other module imports `third_party.toml` and never includes `<toml.hpp>` itself.

**Why.** A header included in the global module fragment of a module becomes part of that module's
BMI. Included in ten modules, it is in ten BMIs, and a file importing several of them loads every
copy and merges the duplicates — on every build of that file and on every completion in it. In one
wrapper it is parsed once, stored once, loaded once.

## 2. Do not mix `#include` of the standard library with `import std`

In a unit that imports `std`, include no standard header: not directly, and not through a header
that includes one. A third-party header that needs the standard library goes into its own wrapper
(rule 1), where its `#include <vector>` stays in the wrapper's global module fragment.

```cpp
// avoid: the same declarations from two places
module;
#include <vector>
#include "my_lib.hpp"        // includes <string>, <map>
export module app.core;
import std;
```

```cpp
// prefer
export module app.core;
import std;
import third_party.my_lib;   // the wrapper includes my_lib.hpp, and with it <string> and <map>
```

**Why.** `import std` brings the standard library as one module. An `#include <vector>` in the same
unit brings a textual copy of the same declarations. The compiler has to prove each pair the same
and merge it, which is slow and, with today's compilers, the most common source of "redefinition"
and "ambiguous" errors in mixed projects.

## 3. Keep the global module fragment to what the interface needs

The global module fragment of an interface should hold only the headers its exported declarations
mention. Headers that only the implementation needs go into an implementation unit:

```cpp
// app/net.cppm -- the interface
export module app.net;
import std;
export namespace app::net {
    struct Response { int status; std::string body; };
    Response get(std::string_view url);
}
```

```cpp
// app/net.cpp -- the implementation
module;
#include <curl/curl.h>       // only here: importers never load it
module app.net;
namespace app::net {
    Response get(std::string_view url) { /* ... */ }
}
```

**Why.** What an interface includes is in its BMI, and every importer loads it. What an
implementation unit includes costs only that unit. An edit to an implementation unit does not change
the BMI, so nothing that imports the module is rebuilt.

## 4. Put function bodies in implementation units

In a module interface, a member function defined inside its class is not implicitly `inline` as it
is in a header, and changing any body in an interface unit changes the interface. Keep bodies that change often in `module X;` implementation units,
and leave in the interface what importers need to see: declarations, templates, `constexpr`
functions, small inline helpers.

**Why.** Every edit to an interface unit rebuilds its BMI, and with it the BMIs of everything that
imports it. A body edited in an implementation unit rebuilds that one unit.

## 5. Import what you use; avoid re-export chains

`export import` is for a module that is a facade over its partitions, or for a type that is part of
the module's own interface. Do not use it to pass on what a module happens to use:

```cpp
// avoid
export module app.defines;
export import std;                 // every importer of app.defines now imports std through it
export import third_party.toml;

export module app.tool;
export import app.defines;         // ...and every importer of app.tool, everything above
```

```cpp
// prefer
export module app.tool;
import std;
import third_party.toml;
import app.defines;                // exports only its own names
```

**Why.** An `export import` puts the whole re-exported closure into every importer's visible scope:
more BMIs loaded for each request, more names in every completion list, and a dependency the
importer never stated, so removing it from the middle of the chain breaks files far away. Never
`export import std`.

## 6. Split large modules along the lines they change

A module whose interface is thousands of lines, and that most of the project imports, is rebuilt
— with all its importers — on every edit to any part of it. Split it into separate modules (or
partitions behind a facade) along the lines that change together: a stable core of types, and the
parts that are worked on.

**Why.** The rebuild after an edit is the size of what imports the edited interface. Small, stable
modules at the bottom of the graph and frequently edited ones near the top keep that small.

## 7. Platform headers get their own wrapper

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

**Why.** `windows.h` and the POSIX headers are large and define macros (`min`, `max`, `ERROR`) that
collide with ordinary code. Macros do not cross a module boundary, so wrapped, they stay inside the
wrapper. If other code needs a macro, put it in a small header of its own that defines only that.

## 8. Names follow directories

Name a module after its directory (`app/net/` holds `app.net`), give each directory one primary
interface, and use partitions (`app.net:http`) for the parts nobody outside imports. A build tool,
mcppls and a person can then find a module's source from its name.

## Header-only code in a module project

Files that import no module (headers, and units that only include) work as before: the engine
completes in them from its index, as without module support, so they do not pay for the project's
modules. A project with no module at all runs the engine without module support. The rules above
matter for the files that import.

## Seeing what it costs

- The report's `slowestFiles` lists the ten files with the slowest completion, and
  `engines[].details.buildTimes` where each file's build went: preamble, imported modules, AST
  builds.
- The log line `Prerequisites of <file>: <n> modules, <n> reused, <n> built …` says how large a
  file's import closure is and what it cost ([50-troubleshooting.md](50-troubleshooting.md)).
- `Rebuilding module <M>: <why>` names the edit that caused a rebuild.

A file whose closure is dozens of modules, or a module that is rebuilt whenever anything changes, is
where these rules pay off first.
