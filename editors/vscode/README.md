# C++ Modules

C++20 and C++23 named modules that just work in VS Code.

Open a folder with `import` and `export module`, and go to definition, completion, hover, references and diagnostics follow your code across modules. It works with GCC, Clang and MSVC projects, with mcpp, CMake or a `compile_commands.json`, and with no build system or compiler at all.

## What you get

- Navigation, completion, hover and references across module boundaries, including partitions.
- Go to definition on a module name in `import hello.greet;`, and completion of module names after `import`.
- Diagnostics for imports that no module provides.
- Semantics of the compiler your project builds with. Without a compiler, the built-in standard library kit (libc++ 23.1) is used.

Everything ships inside the extension: the language server, clangd 23.1 and the standard library kit. Nothing is downloaded or installed at run time, and nothing is written to your project.

## Status

When a C++ file is open, the language status area shows one item, for example `C++ Modules · gcc 16.1.0`. It is busy while modules are prepared, and turns into a warning with a suggested fix when something limits the results.

## Commands

| Command | Use |
|---|---|
| C++ Modules: Select Context | Choose the target or configuration a file is analyzed in |
| C++ Modules: Show Module Graph | Modules, the files that provide them, and who imports them |
| C++ Modules: Restart Language Server | Restart the server and clangd |
| C++ Modules: Show Logs | Open the log |
| C++ Modules: Install Command Line Tools | Run `xcode-select --install` (macOS only) |
| C++ Modules: Collect Diagnostic Report | Open the server's status and this extension's version, settings and other installed C++ extensions as JSON, ready to copy or attach to an issue |
| C++ Modules: Run the Build Tool in a Terminal | Run the project's build command (`mcpp build` or the CMake configure step) in your own terminal, where a proxy or credentials you set by hand actually are |

With `mcppls.ai.enabled`:

| Command | Use |
|---|---|
| C++ Modules: Review Changes | Review the workspace's changes against `HEAD`: findings of mcppls's rules — removed or changed exports still in use, partition misuse, unresolved imports, new compiler errors — shown as problems with their evidence as related locations. Runs git and the compiler, so only in a trusted workspace; nothing is sent to a model |
| C++ Modules: Clear Review | Remove the review's problems |

## Settings

All settings are optional.

| Setting | Default | Use |
|---|---|---|
| `mcppls.compiler` | automatic | Follow this compiler instead of the discovered one |
| `mcppls.semanticKit` | `auto` | `off` never uses the built-in standard library kit |
| `mcppls.engine` | `clangd` | `none` runs without clangd: module-level features only |
| `mcppls.ai.enabled` | `false` | Show the review commands |
| `mcppls.detectConflicts` | `true` | Offer once to turn off other C++ extensions' language features in the workspace |
| `mcppls.trace.server` | `off` | Trace the language server protocol in the log |
| `mcppls.buildTool` | `offline` | How mcppls may run the project's build tool (mcpp, CMake) to learn how it is built: `offline` runs it without the network, offering to run it in a terminal when it needs a download; `online` lets it reach the network, with up to ten minutes; `off` never runs it, using only the cache or scanned sources |
| `mcppls.toolEnvironment` | `auto` | Which environment build tools are started in: `auto` reads the login shell's environment once in the background on Linux and macOS (Windows always matches the editor); `editor` always uses the editor process's own environment |

## Other C++ extensions

If the Microsoft C/C++ extension or the clangd extension also serves C++ files, results appear twice. The extension asks once whether to turn their language features off for the workspace; nothing changes without your answer.

This extension and `mcpp-community.mcpp-vscode` ("mcpp") are separate, and both are worth having installed together: `mcpp` handles building, toolchains and project operations, while this extension handles C++ module semantics and drives its own pinned clangd.

## macOS Command Line Tools

If the macOS SDK cannot be found, the language status item turns into a warning and the extension offers, once per machine, to run `xcode-select --install`. Declining changes nothing and is not asked again; the offer is also available any time as **C++ Modules: Install Command Line Tools**.

## Restricted Mode

In an untrusted workspace no compiler, build system or discovery command is run. Module navigation and the built-in standard library kit keep working.

## License

Apache-2.0.
