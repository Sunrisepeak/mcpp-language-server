# Changelog

Notable changes to mcpp-language-server. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); each release is one dated section, and a
release's notes are that section.

Versions are three-part semantic versions, `MAJOR.MINOR.PATCH`, and every editor plugin carries the
product version unchanged.

## [0.0.5] — 2026-09-26

Issue #23 is fixed: modules are built again for projects compiled with LTO for the MSVC ABI. An
autosave of a half-typed `import` no longer stalls clangd, restarts can no longer leave it stuck, and
one command exports everything a problem report needs, with your name, paths and secrets replaced.
The analysis of #23 with its Windows measurements is `.agents/docs/2026-09-26-issue-23-lto-module-scan.md`;
the plan, its decisions and what was measured is `.agents/docs/2026-09-26-issue-23-fix-plan.md`.
clangd's own defects behind all of this are registered in issue #24.

### Engine

- **Modules are built again under LTO for the MSVC ABI (issue #23).** clangd's module scan failed on
  every unit with the driver's `LTO requires -fuse-ld=lld`, so no module was built, imports were not
  found, and on Windows clangd then crashed until it was given up on. Every command mcppls gives
  clangd now compiles only (`-c`), which no link-phase check can fail; `-flto` and the rest of your
  command stay as they are. clangd rebuilds its module cache once, the first time a project is opened.
- **An autosave of a half-typed import no longer stalls clangd.** clangd reads a file's imports from
  its text on disk, not from the editor, so `import hello.` saved by `files.autoSave` spun a clangd
  worker at a full core (it crashed clangd on Windows), past the 0.0.4 workaround, and every request
  for the file waited behind it until clangd was restarted; an import of a module that does not exist
  yet, saved, deadlocked it the same way.
  - Every save and watched change is checked: such a file is answered by mcppls's own engine until it
    is saved again, and the status says so as a problem of the code being typed, staying *ready*.
  - A build clangd began on it just before is given 1.5 s to finish, else clangd restarts without it.
  - A module nothing provides that a save put on disk gets its stand-in within a second.
  - Measured on the hello project: 0 CPU for every clangd thread over 20 s with `import hello.` on
    disk, where 0.0.4 used a full core.
- **A crash sets aside the file clangd names.** clangd says which file it crashed on; that one is set
  aside, instead of whatever was asked about or edited in the last ten seconds (in #23, the wrong
  file). The exit code, the file and, on Windows, the exception code are in the report and the status.
- **Restarts are backed off, never refused.** Each reason has its own budget of three restarts in ten
  minutes: the database changing, recovering a clangd that stopped answering, and clangd exiting. Past
  it, the next one waits 1, 2, 4, then 8 minutes, so a stuck clangd always comes back; in 0.0.4 it
  stayed stuck until the window aged out.
  - **C++ Modules: Restart clangd** restarts it at once and is never counted.
  - Switching the toolchain, the profile or the context is not counted either.
- **Fewer restarts while typing.** A stand-in coming or going no longer restarts clangd, nor does a
  changed command of a file clangd does not have, and a restart the database asks for waits two seconds
  so the changes that follow share it. The stand-in given to a module clangd could not find is no
  longer taken for a new provider, which dropped it again and restarted clangd. A change to a file's
  imports is planned once the file has been quiet for two seconds.
- **clangd starts with the build tool's model.** A project whose build system was found no longer
  gives clangd a model guessed from its sources while the build tool runs, which clangd then had to
  unlearn (in #23, three crashes on it before the real model came). mcppls's own engine answers
  module features meanwhile, for up to a minute.
- **A command clangd rejects is said so.** When clangd's module scan fails on a command, the status
  names the first rejection in the compiler's words, as an environment problem; a missing header, as
  the project's.
- A source saved with a UTF-8 byte order mark still declares its module. When no build tool describes
  a project, `vcpkg_installed/`, `vcpkg/`, Conan and xmake caches and vendored vcpkg packages are not
  scanned, so their modules no longer show up as ambiguous.

### Diagnostics

- A directive missing its `;` is reported on the directive, not on the code after it
  (`WA-CLANGD-006`).
- An import typed and not saved yet, of a module in the project, is information ("module 'X' is in
  the project; clangd loads it once the file is saved"), not a `module not found` error
  (`WA-CLANGD-007`).

### Completion and coloring

- **A space after `import ` or `export import ` opens the module list** in VS Code; spaces anywhere
  else never reach the server. Setting `mcppls.completion.triggerOnSpace` (default on). Other editors
  can opt in with `initializationOptions.completion.triggerOnSpace: true`.
- **Module keywords come from mcppls**, merged with clangd's: `import`, `export import`, `module;`,
  `export module`, `module` and `module :private;`, each where it can start a declaration, and still
  there when clangd is stuck or restarting. Accepting `import` opens the module list. No name is
  suggested after `export module `.
- The `export` of every export declaration (`export namespace`, `export {`, `export int f()`) is
  colored like the one of `export module`.

### Reports and diagnostics bundle

- **C++ Modules: Export Diagnostic Bundle** (`mcppls report --bundle out.zip`, `workspace/executeCommand`
  `mcppls.exportBundle`): one zip with the report, the environment, recent server logs, incidents and
  the engine database, written locally and never uploaded.
  - Your home directory becomes `~`, your user and host names `<user>` and `<host>`, and tokens,
    passwords, keys and e-mail addresses `<redacted>`.
  - If anything is left after that, the bundle is not written at all, and the editor offers to retry
    with project paths hidden.
  - Source files are never included; crash dumps only on request.
- The diagnostic report is redacted the same way by default (`cxxModules/report` `redact`, S3-5.5-3).
- **Incidents.** A crash, a stuck or spinning clangd, a file set aside, a restart held back and a
  broken workaround premise each leave a directory under the workspace's cache (`incidents/`, the
  newest twenty, for a week). It holds clangd's latest log (clangd now logs at `info`, into memory
  only), the files' editor-versus-disk lines and which clangd thread was busy.
- Every change of the engine database is logged with what changed. The status buttons say what they
  do (Restart clangd, Export Diagnostic Bundle).

### Build

- The macOS server runs on macOS 11 again (it said 14.0), built with mcpp 2026.9.26.1.

### Known limits

- clangd's own defects are worked around, not fixed: `import a.` (UP-01, whose upstream fix is
  deferred), imports read from disk (UP-14), and a Windows crash on some units with correct commands
  (UP-13). The last one is contained: the file clangd names is set aside. Issue #24 tracks each.

### Testing

- New conformance fixtures, each failing on 0.0.4: `typing-autosave`, `clangd-crash-context`,
  `compdb-rejected-command`, `mcpp-emit-wait`, `compdb-lto-msvc`, `inferred-bom`,
  `completion-keywords` and `diagnostic-bundle`.
- `diagnostic-code` checks take a line, a severity and codes that must be absent; `completion-contains`
  takes a trigger character.

## [0.0.4] — 2026-09-25

Typing an `import` no longer freezes the editor, the status bar says whose problem it is, and
`import` is colored. The investigation, the plan and its measurements are in
`.agents/docs/2026-09-25-import-hang-status-highlight.md`.

### Engine

- **Typing a dotted import froze everything.** clangd 23.1 never finishes a file in which a module
  name ends in `.` at the end of its line (`import hello.`, `export module a.`): it spins at a full
  core, and every later version of the file waits behind it. Typing `import hello.greet;` went
  through that text every time. The feature requests for the file went unanswered for 30 s at a
  time, and the status turned *degraded*. On Windows, the same text crashed clangd instead. clangd is now given that line with `;` right after the
  dot, which it reports at once as the error it is; nothing else about the text changes.
  Measured: every keystroke answered within 0.4 s, where 0.0.3 answered nothing for 30 s.
- **A file clangd will not finish is found and recovered even while the user keeps typing.** Before,
  the guards took a busy clangd for a compiling one, and an edit to the file postponed setting it
  aside for two minutes; the edit that caused the hang, and the edits fixing it, kept postponing it.
  - A file's build now has a budget: five times its own last build, never under 20 s.
  - Past the budget, with the editor waiting on the file, clangd is restarted at once, past the
    restart cap if need be.
  - The file goes to mcppls's own engine, which gives module-level features, and returns to clangd
    as soon as its text is anything other than the text clangd stopped on.
  - Event `engine-spin`.
- **Workarounds for clangd's own defects are registered in one place**
  (`src/engine/clangd/workarounds.cpp`). Each entry has the versions it applies to, the upstream
  defect, and when it can go. The report lists the ones in use (`engines[].details.workarounds`),
  and `--disable-workaround WA-CLANGD-<n>` turns one off.
- **Half-typed imports no longer churn the engine database.** A module nothing provides, imported
  by a file changed in the last five seconds, gets its stand-in only once the file is quiet; a
  unit that provides a module still gets its stand-in at once. A name that is no module name
  (`hello.`, as a build tool's scan of a file saved mid-edit can report) is never planned.

### Status

- **A problem in your code is a diagnostic, not a lost feature.** A missing `;`, an import of a
  module nothing provides, or a module that does not compile is reported where it is, in the
  Problems list, and the status stays *ready*. *degraded* now means the server lost something, and
  it says what and where, for example "clangd stopped responding on main.cpp; module-level features
  only for it until it changes". Status issues carry a `category` (`code`, `engine`,
  `environment`, `project`; S3).
- A change to *degraded* is shown only once it has lasted three seconds, so a condition that passes
  by itself never flickers in the status bar.

### Editors

- **`import`, `module` and `export` are colored**, and so are module names:
  - The server sends semantic tokens for module syntax, which clangd sends none for (a custom type
    `module`, with `namespace` for clients that do not ask for it), with and without clangd.
  - The VS Code extension adds a grammar that colors them as you type, since VS Code's own C++
    grammar leaves `import` uncolored.
  - Settings: `mcppls.semanticTokens.modules` in VS Code, `semantic_tokens_modules` in Neovim.
- **Other C++ extensions** can be turned off, or back on, at any time:
  - Commands *Turn Off Other C++ Language Features* and *Restore Other C++ Language Features*, in
    this workspace or everywhere.
  - A notice when one becomes active later.
  - The C/C++ extension's debugger keeps working.
  - Neovim has `disable_conflicting`.

### Known limits

- clangd 23.1 also never finishes a file in which `import std;` comes before an `export import` of
  something nothing provides. Only a file outside the project, holding such ill-formed code, gets the
  command that exposes it. mcppls sets such a file aside after two minutes, and the defect is to be
  reported upstream with the first. `.agents/docs/2026-09-25-import-hang-status-highlight.md` §13 has
  the details.

### Testing

- Conformance kinds `type-text` (a line typed one key at a time, each step answered in time, the
  status never turning *degraded*) and `clangd-check` (a workaround's canary).
- Fixtures on every platform:
  - `typing-import`;
  - `typing-import-spin`: the real spin, with the workaround off, recovered within its budget;
  - `workaround-canaries`: it fails once a clangd update fixes the defect.
- `module-faults` and `failure-at-base` now expect *ready*, naming their code issues.

## [0.0.3] — 2026-09-24

Linux arm64 is a platform, the extension is on Open VSX and is found by searching *mcppls*, and a
clangd that cannot run no longer leaves an editor stuck at start. The plan and its measurements are
in `.agents/docs/2026-09-24-0.0.3-plan.md`.

### Platforms

- **linux-arm64**: `mcppls-linux-arm64.vsix` and `payload-linux-arm64.tar.gz`, for VS Code on arm64
  Linux, and for Remote-SSH, Dev Containers and WSL on arm64 machines. The server is static; the
  clangd is LLVM's own 23.1.0 build, which needs glibc 2.34 and a GCC 12 libstdc++: Ubuntu 22.04+,
  Debian 12+ and openEuler 24.03+ (tested), Fedora 36+ (by package versions). On older systems
  (Ubuntu 20.04, Debian 11, RHEL/Rocky 8 and 9, openEuler 22.03, Amazon Linux 2023) the status says
  so and mcppls's own module features remain. `docs/00-install.md` lists them.
- The platforms a release ships are the rows of `packaging/payload.lock.json`; the extension, the
  release manifest and every per-platform CI job are checked against them
  (`mcppls-devtools check platforms`). A server knows its platform from its OS and its architecture,
  so an arm64 Linux server calls itself `linux-arm64`.

### Engine

- A clangd that dies before its handshake no longer holds the server's `initialize`: before, an
  editor waited for good (still unanswered after 330 s), with no features and no reason given. Now a
  second such exit answers it with mcppls's own features, and a loader's refusal on clangd's
  standard error (a library, or a version of one, missing) is issue `engine-incompatible` at once:
  status *error*, the loader's message quoted, no restarts, since none can help.

### Editors

- The VS Code extension and the CLion plugin carry the mcpp mark as their icon.
- The extension is found by searching *mcppls* (it was in none of the fields a store indexes), and
  by *c++ modules*, *cxx modules*, *ixx* and a few more.
- **Open VSX**: Cursor, VSCodium, Windsurf and other VS Code-compatible editors install it from there.
  A release reaches Open VSX when it is published (`publish-openvsx.yml`), after the same local
  verification that precedes the VS Code Marketplace upload.

### Testing

- Conformance fixture `clangd-cannot-load` (a stand-in clangd that fails the way the loader does, on
  every host) and the runner's `initialize-within`. linux-arm64 is cross-built, assembled,
  conformance-tested and end-to-end tested in VS Code on an arm64 runner, like every other platform.

## [0.0.2] — 2026-09-23

A module that does not compile, an old pinned mcpp or a stale compile database no longer leaves a
project stuck: measured on xlings with its pinned mcpp 2026.8.8.4, 0.0.1 never left *preparing*,
timed requests out at 10 s and kept four cores busy; 0.0.2 is ready in 8 s with no timeouts. The
findings and the plan are in `.agents/docs/2026-09-22-real-project-experience.md`.

### Engine

- A module clangd cannot compile affects only what imports it: those files are answered at once by
  mcppls's own engine, carry one `module-failed` diagnostic on the import that leads there, and are
  not prepared or sent to clangd again until the failed module's own source or command changes.
  Everything else keeps clangd.
- A module that does not compile is never a reason to restart clangd, nor is a file clangd is slow
  on while it rebuilds after a change to that file or to a module it imports (the restart waits
  until that change is two minutes old and is still needed); an edit elsewhere in the project does
  not excuse it, and clangd answering nobody is contained whatever was edited. Restarts are capped
  at three in ten minutes (`engine-restart-capped`), and modules known to fail are not prepared
  again after one.
- Every request is answered: one a person waits for (hover, definition, completion and the like)
  waits for clangd at most 30 s in all, whether clangd is starting, the file is waiting for its
  database, or its modules are being prepared, and is then answered by mcppls's own engine. Before,
  a request queued while clangd started or while its file was held had no limit at all.
- A clangd that is stuck, not busy, is restarted within seconds: it left a request unanswered,
  answered nothing else, and used next to no CPU for five seconds. clangd 23.1 has been seen to
  hang this way after a module's source changed twice within a second, answering nothing for
  minutes; a long compile keeps a core busy and is left alone. Linux and macOS (Windows gives the
  server no process times).
- A hover that would show nothing while the modules a file imports are still being built says so,
  the way it already did while mcppls prepares them: silence reads as "there is nothing here".
- The status settles: preparation that makes no progress for a minute ends in *degraded*, naming
  what failed (`modules-doomed`, `preparation-stalled`), instead of *preparing* for good.
- clangd's own `E[` lines are logged as warnings and its `I[`/`V[`/`D[` chatter at debug; a module
  clangd could not build is a warning; each stand-in is named in the log with its reason.
- A request on a file clangd has not been given yet waits for it instead of being answered empty,
  and can be cancelled; a late clangd publish no longer replaces a `module-failed` diagnostic.

### Project model

- An untrusted workspace no longer reads a `compile_commands.json` or build database a trusted
  session (or another build) already left on disk; it is L4 (sources only) by definition, as the
  docs always said.
- An mcpp that cannot `emit build-database` no longer means immediately configuring the project (or
  giving up): mcppls looks for a newer mcpp installed elsewhere on the machine and, if one advertises
  it, asks that one instead, read-only — the project still builds with the mcpp it pins. The status
  and `mcppls check` say "described by mcpp X (the project pins Y)".
- A compile database entry naming a file that no longer exists is dropped rather than breaking the
  model; the status notes it as stale. A module a dependency's build generates is looked for where
  builds leave it (the project's own build directory, mcpp's build-database cache) before mcppls
  falls back to an empty stand-in.
- `cxxModules/status` gained `project.tier` (spec S3-4-8, S3-4-9): which kind of source described
  the project (the README's L1..L4), independent of `project.level` (S1's own document-conformance
  number). The status bar and the Neovim plugin now show `L<tier>`, never `level`, which the two
  numbers sharing a range made easy to misread as the same thing.
- Scanning and a file opened while browsing no longer fold a nested project's own sources (a
  conformance fixture, a vendored copy, an example with its own `mcpp.toml` or `CMakeLists.txt`) into
  the workspace's model.
- The tier says how a model was obtained: an mcpp project described through mcpp's
  `compile_commands.json` is L3, not L1. A model is never replaced by one of a worse tier; the one in
  hand is kept and marked possibly stale.

### Testing

- `mcppls-conformance` has a `stress` check — seeded random use (opening and switching files, hover,
  definition, references, completion, symbols) with budgets for timeouts, p90, stalls, CPU and
  memory — and client profiles (`--client vscode|neovim|zed|plain`).
- New fixtures: a module generated at build time (with a current, an old and a negotiated mcpp), a
  100-module chain whose base does not compile, and pinned real projects: this repository and xlings,
  as committed and with its old mcpp pin. The mock mcpp can be too old for `emit build-database`.
- `mcpp run -p devtools -- stress` runs the matrix and compares against a baseline; Neovim and the
  VS Code suite each have a stress scenario; CI runs the failing-base fixture and the stress checks
  on Linux, macOS and Windows and the generated-module fixtures on Linux and macOS (their mock mcpp
  is POSIX-only), each client profile at least once per platform, and the real projects nightly and
  before a release.
- A fixture that fails prints the last lines of the server's own log, so a failure seen only in CI
  says what the server was doing meanwhile.

### Editors

- Neovim: a Lua plugin in `editors/nvim` (Neovim 0.10 or later) that starts mcppls for C and C++
  buffers through Neovim's own LSP client, by `require('mcppls').setup()` or, on 0.11 and later,
  `vim.lsp.enable('mcppls')`; `:McpplsStatus`, `:McpplsRestart`, `:McpplsReload`, a statusline
  component, and a one-time warning when clangd or ccls attaches beside it. CI runs it on Neovim
  0.10.4, 0.11.5 and 0.12.5 on Linux, and 0.10.4 and 0.12.5 on macOS and Windows.

## [0.0.1] — 2026-09-22

The first public release. Every artifact is a file on the GitHub release page; its `MANIFEST.md` says
what each one is and how to install it, and `SHA256SUMS` covers them all. The VS Code extension is
also on the VS Code Marketplace as `sunrisepeak.mcpp-language-server`; nothing is on Open VSX or the xlings index
yet.

### Module semantics on any compiler

- mcpp, CMake (`FILE_SET CXX_MODULES`), a bare `compile_commands.json`, or sources alone are
  normalized into one module description (L1–L4), with that toolchain's `std` — GCC, Clang, MinGW,
  clang-cl and MSVC, including MSVC STL's `import std`.
- A pinned clangd 23.1 is driven with it; mcppls's own engine adds module-name navigation, `import`
  completion, the module graph, and diagnostics for unresolved, ambiguous and cross-module partition
  imports.
- One payload per platform — the server, clangd and the semantic kit — so `import std` resolves even
  with no usable compiler on the machine. Its files are checked against their hashes at startup.

### It is never held by what it starts

- Build tools, compiler probes, CMake and git run in a process unit of their own, with deadlines that
  end the whole unit and bounded reads.
- Runs the server starts itself are offline (`mcppls.buildTool`); when a download is needed, the
  status says what is missing and offers to run the build tool in your terminal.
- Build tools get the login shell's environment on POSIX. The last session's model is cached with a
  fingerprint of its inputs and used at once, then confirmed in the background.

### Faults stay where they are

- A module nothing provides gets a stand-in unit, so one broken module does not take its importers
  with it. clangd restarts only when an imported module's unit changes, behind a rate gate; a file
  clangd stops answering is set aside and answered by mcppls's own engine.
- Degradation is visible in the status, and one diagnostic report — `mcppls report`, or "Collect
  Diagnostic Report" in VS Code — carries the model, the plan, the engines and the recent external
  runs. Logs outlive the editor.

### For coding agents and CI

Semantic queries, post-edit verification and change review over MCP (`mcppls mcp`, `--daemon` to
share a workspace) and the command line: symbols, references and callers followed through module
imports, fresh diagnostics, cross-toolchain verification, and review findings with evidence as SARIF,
LSP diagnostics or Markdown. Model-backed review is optional, off by default, and never done by the
server process itself.

### Editors

- VS Code (a VSIX per platform, carrying its payload), Zed, CLion, Claude Code and GitHub Copilot CLI.
- Built from source, every plugin is one command: `mcpp run -p devtools -- extension --editor
  vscode|zed|clion --install`, and `uninstall` to remove it. Zed's recommended last step stays its
  own palette action (`--link` does it from the command line).
- The VS Code extension writes the server's log at the level of each line; a healthy start is no
  longer shown as errors.

### Specifications

S1 build database, S2 discovery, S3 LSP extensions for modules, S4 semantic kit and S5 semantic
queries, versioned separately, with rule identifiers traced to tests, conformance checks or code.
