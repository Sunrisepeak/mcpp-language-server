# Settings and the command line

Every configurable behaviour of mcppls has exactly one definition: a row of the registry in
`src/config/settings.cppm`. The table below -- the VS Code settings, the
command-line options, and the environment variables that configure something -- is generated from
that registry (`mcppls settings --format markdown`), and `tests/test_settings.cpp` holds it, the
zh-CN mirror and `editors/vscode/package.json` to the registry so the three cannot drift apart.

**Precedence.** The command line beats `initializationOptions` beats the default; a later
`workspace/didChangeConfiguration` updates a value `initializationOptions` (or an earlier
`didChangeConfiguration`) set, but never one the command line set. A value outside its own
vocabulary (an unknown enumeration member, say) is never applied -- it falls back to the default
and is recorded as a problem (`mcppls report`'s `settings.problems`, and `mcppls settings` itself
carries none since it only ever prints the registry).

**Applies.** How a change to a setting already running takes effect: `restart` needs mcppls
restarted (the VS Code extension already does this for the settings below that need it);
`reload` only reloads the project's model, which happens without a restart; `immediately` needs
neither -- the next time it is read is the next time it matters.

**A renamed setting keeps working.** A setting the registry gives an alias for is still accepted
under its earlier name (dotted key or `initializationOptions`/`didChangeConfiguration` form alike);
none of the settings below have been renamed yet, so none carry one today.

`initializationOptions` and `didChangeConfiguration` both accept a nested object
(`{"semanticTokens": {"modules": false}}`) or a dotted key (`{"semanticTokens.modules": false}`),
either wrapped in a top-level `mcppls` object or not.

<!-- settings:begin -->
### Project and build tools

| Setting | Values | Default | Command line | Applies | What it does |
|---|---|---|---|---|---|
| `mcppls.buildTool` | `offline`, `online`, `off` | `offline` | `--build-tool` | reload | How the project's build tool may be run. `offline`: run it without the network -- if it then cannot describe the build without downloading something, the status says what is missing and offers to run it in your terminal. `online`: let it reach the network, with ten minutes instead of one. `off`: never run it; the build system is still detected and its own generated files are still read (see `buildDiscovery` for turning that off too). |
| `mcppls.toolEnvironment` | `auto`, `editor` | `auto` | `--tool-environment` | restart | Which environment build tools are started in. `auto` reads your login shell's environment once, in the background, on POSIX -- an editor started from a desktop entry or a Dock icon carries none of your shell configuration, so without this the build tool it finds may not be the one your terminal finds. On Windows the editor's environment already matches the terminal's. `editor` always uses the editor process's environment. |
| `mcppls.producerTimeout` | a non-negative number of seconds | `0` | `--producer-timeout` | reload | How long a build tool may take to describe the project. `0`, the default, uses the design's own bound: offline, 5 minutes the first time and then three times what the last run took, between 1 and 10 minutes; ten minutes once `buildTool` is `online`. Set it to watch that bound work, or to fix one for a build whose time varies. |
| `mcppls.untrusted` | `true`, `false` | `false` | `--untrusted` | restart | Run no build tool and no compiler; an untrusted workspace is also read as though `buildDiscovery` were `off`. |
| `mcppls.discoverCompilers` | `true`, `false` | `true` | `--no-discover` | reload | Look for a compiler on the machine for a source the build description does not cover. Off: such a source uses the semantic kit instead. |
| `mcppls.buildDiscovery` | `auto`, `off` | `auto` | `--build-discovery` | reload | Whether the project's build system is detected at all. `off`: nothing is read or run implicitly -- only an explicitly configured `database`, else sources are scanned. `buildTool` still governs whether a detected build tool may be *run*; this governs whether it is looked for in the first place. |
| `mcppls.buildDiscovery.providers` | `mcpp`, `cmake`, `xmake`, `meson`, `compile-commands` (comma-separated) | `mcpp`, `cmake`, `xmake`, `meson`, `compile-commands` | `--build-discovery-providers` | reload | Which build system providers `buildDiscovery` may use; leave one out to stop mcppls from detecting it (for example, to use only a CMake build directory that already exists and never let xmake run). |
| `mcppls.buildDiscovery.askBeforeDownload` | `true`, `false` | `true` | — | immediately | When the build tool needs a download to finish describing the project, a client may offer to fetch it. Off: the status says a download is needed, and nothing asks. |

### Engines

| Setting | Values | Default | Command line | Applies | What it does |
|---|---|---|---|---|---|
| `mcppls.engine` | `clangd`, `none` | `clangd` | `--engine` | restart | The core semantic engine. mcppls's own module engine always runs beside it; `none` means module-level features only. |
| `mcppls.engine.workers` | a string | `auto` | `--engine-workers` | restart | How many files clangd builds at once (its `-j`), shared by module preparation, indexing and the files you edit; one is always kept for what you are waiting on. `auto`: one fewer than the hardware threads, between two and eight, and no more than half the memory in gigabytes. A number sets it. |
| `mcppls.compiler` | a string | *(empty)* | `--compiler` | reload | Use this compiler for module semantics instead of what was detected: an absolute path, a name on `PATH`, or `kit` to force the bundled semantic kit. Empty means discovered automatically. |
| `mcppls.semanticKit` | `auto`, `off` | `auto` | `--semantic-kit` | reload | Whether the bundled standard library kit may be used at all: `auto`, when no compiler is found; `off`, never (without a compiler, only module-level features remain). |
| `mcppls.requestTimeout` | a non-negative number of seconds | `60` | `--request-timeout` | restart | How long an engine request may take before it is answered without the engine. A request a person waits for (hover, definition, completion and the like) waits at most 30s in all, including while clangd starts or prepares its modules, and is then answered by mcppls's own engine. |
| `MCPPLS_ENGINE_ARGUMENTS` | a string | *(empty)* | — | restart | Extra arguments appended to clangd's own command line, for troubleshooting (e.g. `-j=8 --background-index-priority=background`). |

### Editor experience

| Setting | Values | Default | Command line | Applies | What it does |
|---|---|---|---|---|---|
| `mcppls.semanticTokens.modules` | `true`, `false` | `true` | — | restart | Color `import`, `module`, `export` and module names from the server's semantic tokens. Off: only the grammar's colors. |
| `mcppls.semanticTokens.moduleType` | `true`, `false` | `false` | — | restart | A client declares it knows the custom `module` semantic token type and the `partition` modifier; off for every client but this one, since none else advertises it. Not a package.json setting: VS Code's own extension always declares it, fixed, because it contributes that token type itself. |
| `mcppls.completion.triggerOnSpace` | `true`, `false` | `true` | — | restart | Show the module list as soon as a space is typed after `import` or `export import`. A space anywhere else never reaches the server. A client that says nothing gets this only when it identifies itself as VS Code or a fork of it; every other client opts in with `initializationOptions.completion.triggerOnSpace: true`. |
| `mcppls.index.primeImplementationUnits` | `auto`, `off` | `auto` | `--prime-implementation-units` | restart | Build a module's implementation units in clangd in the background, a few at a time, so go-to-definition reaches a definition that only an implementation unit has, before that file was ever opened. clangd's own background index cannot see a module unit's imports (WA-CLANGD-008). `off`: only the units a definition request searches, and the files you open, are indexed for this. |
| `mcppls.enable` | `true`, `false` | `true` | — | immediately | Start mcppls for this workspace. `false` in a workspace's settings keeps the extension installed but inactive there -- for a project it cannot serve yet -- without uninstalling it; the status bar item turns it back on. |
| `mcppls.detectConflicts` | `true`, `false` | `true` | — | immediately | Offer once to turn off another C++ extension's language features in this workspace, and say so when one becomes active later. VS Code only: no other client arbitrates between language servers. |

One editor setting is given a value for C and C++ by the VS Code extension rather than read by the server:
`editor.quickSuggestions` is `{"other": "on", "comments": "off", "strings": "off"}` for `[cpp]` and `[c]`, so the
completion list opens while you type. VS Code's own default since 1.125, `{"other": "offWhenInlineCompletions"}`,
keeps it closed while an inline completion such as Copilot's shows grey text (`WA-VSCODE-002`). Your own value under
`"[cpp]"` or `"[c]"` wins; one you set for every language does not, and the extension's log says so once.

### Diagnostics and logging

| Setting | Values | Default | Command line | Applies | What it does |
|---|---|---|---|---|---|
| `mcppls.logLevel` | `debug`, `info`, `warning`, `error` | `info` | `--log-level` | restart | The server's own log level. |
| `mcppls.disableWorkaround` | `WA-CLANGD-<n>` (repeatable) | *(none)* | `--disable-workaround` (repeatable) | restart | Turn off a registered clangd workaround (`WA-CLANGD-<n>`; the register of upstream defects is issue #24), to see whether it is still needed; repeatable. `mcppls report` lists every registered workaround under `engines[].details.workarounds`. |
| `mcppls.trace.server` | `off`, `messages`, `verbose` | `off` | — | immediately | Log the LSP traffic to the C++ Modules output channel (at Trace level); `verbose` adds the server's debug log (at Debug level, by also passing `--log-level debug`). Set the channel's own log level to see them. |
| `MCPPLS_LOG_LEVEL` | `debug`, `info`, `warning`, `error` | *(empty)* | — | restart | Overrides the log level the VS Code extension starts the server with, ahead of `trace.server`. |

### AI review

| Setting | Values | Default | Command line | Applies | What it does |
|---|---|---|---|---|---|
| `mcppls.ai.enabled` | `true`, `false` | `false` | — | immediately | Show the AI-era features: Review Changes reviews the workspace's changes against `HEAD` with mcppls's rules and shows the findings, with their evidence, as problems. Nothing is sent to a model unless this is on and a model source is separately configured. |

### Paths

| Setting | Values | Default | Command line | Applies | What it does |
|---|---|---|---|---|---|
| `mcppls.database` | a path | *(empty)* | `--database` | reload | A workspace's own S1 build database, relative to its root, used instead of detecting one. |
| `mcppls.mcpp` | a path | *(empty)* | `--mcpp` | reload | The `mcpp` executable for mcpp projects; empty means found on `PATH`. |
| `mcppls.payload` | a path | *(empty)* | `--payload` | restart | Payload directory with clangd and the semantic kit; overridden per-file by `clangd` and `kit` below. |
| `mcppls.clangd` | a path | *(empty)* | `--clangd` | restart | clangd executable, overriding the one the payload carries. |
| `mcppls.kit` | a path | *(empty)* | `--kit` | restart | Semantic kit directory, overriding the one the payload carries. |
| `MCPPLS_CACHE_DIR` | a path | *(empty)* | — | restart | Overrides the whole cache directory mcppls otherwise picks under the user's cache home (workspace models, toolchain probes, logs, diagnostic bundles). |
<!-- settings:end -->

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
mcppls settings [--format markdown|json] [--lang en|zh-CN]  the table above, or its machine form
mcppls print-environment <marker>               prints this process's environment between two markers
mcppls version
```

Every global option above (`mcppls --help`) is a registered setting's command-line spelling; see the
table above for what each does, its default, and how a change takes effect.

`print-environment` exists for the server itself: it is what the login shell is asked to run when
`mcppls.toolEnvironment` is `auto`.
