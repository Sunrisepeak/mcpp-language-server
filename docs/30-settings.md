# Settings and the command line

## VS Code settings

| Setting | Values | What it does |
|---|---|---|
| `mcppls.buildTool` | `offline` (default), `online`, `off` | How the project's build tool may be run. `offline`: run it without the network — if it then cannot describe the build without downloading something, the status says what is missing and offers to run it in your terminal. `online`: let it reach the network, with ten minutes instead of one. `off`: never run it; use the cached description or scanned sources |
| `mcppls.toolEnvironment` | `auto` (default), `editor` | Which environment build tools are started in. `auto` reads your login shell's environment once, in the background, on POSIX — an editor started from a desktop entry or a Dock icon carries none of your shell configuration, so without this the build tool it finds may not be the one your terminal finds. On Windows the editor's environment already matches the terminal's. `editor` always uses the editor process's environment |
| `mcppls.compiler` | a driver path, or `kit` | Use this compiler for module semantics instead of what was detected. `kit` forces the bundled semantic kit |
| `mcppls.semanticKit` | `auto` (default), `off` | Whether the bundled kit may be used at all |
| `mcppls.engine` | `clangd` (default), `none` | The core engine. mcppls's own module engine runs either way; `none` means module features only |
| `mcppls.ai.enabled` | `false` (default) | Whether the model-backed half of change review may be used. Off means the server makes no model calls |
| `mcppls.detectConflicts` | `true` (default) | Offer once to turn off another C++ extension's language features in this workspace, and say so when one becomes active later |
| `mcppls.semanticTokens.modules` | `true` (default) | Color `import`, `module`, `export` and module names from the server's semantic tokens. Off: only the grammar's colors |
| `mcppls.trace.server` | `off` (default), `messages`, `verbose` | Log the LSP traffic to the C++ Modules output channel (at Trace level); `verbose` adds the server's debug log (at Debug level). Set the channel's log level to see them |

## Commands

| Command | What it does |
|---|---|
| C++ Modules: Collect Diagnostic Report | Opens everything a bug report needs as JSON: the model and where it came from, the plan, the engines, request statistics, recent events, recent external runs, and the log tail |
| C++ Modules: Run the Build Tool in a Terminal | For when the build description needs a download. Offers your own terminal first, because that is where a proxy you set by hand actually is |
| C++ Modules: Show Module Graph | The project's modules and what imports what |
| C++ Modules: Select Context | Switch which set of the build database the file is seen through |
| C++ Modules: Restart Language Server / Show Logs | The usual two |
| C++ Modules: Turn Off Other C++ Language Features / Restore Other C++ Language Features | Turn off the C/C++ extension's IntelliSense (its debugger keeps working) and the clangd extension, in this workspace or everywhere; restore puts back exactly what was there |
| C++ Modules: Review Changes / Clear Review | Change review over the working tree, published as diagnostics (needs `mcppls.ai.enabled` for the model-backed rules) |

## Command line

```
mcppls [serve] [--payload DIR] [--clangd PATH] [--kit DIR] [--engine clangd|none] [--untrusted]
mcppls mcp [--root DIR] [--daemon]              MCP over stdio, the agent tools
mcppls query symbol|refs|calls|outline|module|context ...
mcppls diagnostics <file>... | verify [--changed] | impact | review [--base REV] [--format sarif]
mcppls daemon run|start|status|stop             the shared workspace daemon
mcppls check <file>                             the model, the profile, module diagnostics, then clangd --check
mcppls report [--root DIR] [--settle SECONDS]   what a bug report needs, as JSON
mcppls model [--root DIR] [--export s1|compile-commands|engine]
mcppls print-environment <marker>               prints this process's environment between two markers
mcppls version
```

Options that apply to every subcommand:

| Option | Default | What it does |
|---|---|---|
| `--build-tool offline\|online\|off` | `offline` | The command-line spelling of `mcppls.buildTool` |
| `--tool-environment auto\|editor` | `auto` | The command-line spelling of `mcppls.toolEnvironment` |
| `--producer-timeout SECONDS` | 60, or 600 when online | How long the build tool may take to describe the project. Longer for a genuinely slow build; shorter to watch the bound work |
| `--request-timeout SECONDS` | 60 | How long an engine request may take before it is answered without the engine. A request a person waits for (hover, definition, completion and the like) waits at most 30 s in all, including while clangd starts or prepares its modules, and is then answered by mcppls's own engine |
| `--untrusted` | — | Run no build tool and no compiler |
| `--no-discover` | — | Do not look for compilers; loose sources use the semantic kit |
| `--log-level debug\|info\|warning\|error` | `info` | |
| `--disable-workaround WA-CLANGD-<n>` | — | Turn off one of the registered workarounds for clangd's defects (repeatable), to see whether it is still needed; `mcppls report` lists them under `engines[].details.workarounds` |

`print-environment` exists for the server itself: it is what the login shell is asked to run when
`mcppls.toolEnvironment` is `auto`.
