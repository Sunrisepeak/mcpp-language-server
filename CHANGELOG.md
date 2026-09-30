# Changelog

Notable changes to mcpp-language-server. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); each release is one dated section, and a
release's notes are that section.

Versions are three-part semantic versions, `MAJOR.MINOR.PATCH`, and every editor plugin carries the
product version unchanged.

## [0.0.8] — 2026-09-30

Completion that shows up. In VS Code 1.125 and later with Copilot (built into VS Code) or another
inline completion, the completion list stayed closed while you typed -- the editor waited for the
inline completion and did not even ask the server -- which read as "no keyword or variable
completion"; C and C++ now open it as you type, beside the grey text. Writing a module with autosave
no longer takes the module you are writing from clangd every few seconds, and a completion clangd
does not answer always has the file's words. A partial build description brings its stand-ins in one clangd
restart instead of one per module, the provisional model no longer prepares the project's modules,
and a clangd crash keeps what an upstream report needs. The analysis and the plan are
`.agents/docs/2026-09-30-0.0.8-plan.md`.

In a large project, completion no longer falls further behind with every save until it stops
showing: units mcppls opened to prepare modules are closed once built, a completion clangd answers
late reaches the word it was asked in, and a clangd that keeps every file queued without finishing
anything is restarted. xmake projects follow `xmake.lua` and your own `xmake f` without mcppls writing
anything into the project, and a build that names no C++ standard gets C++23 for its modules, the
standard `import std` is for. CLion (built for 2026.2.3) and Zed are opened for real in CI, and in
CLion a file is answered by one engine. That second part is `.agents/docs/2026-10-01-0.0.8-part2-plan.md`.

### Completion

- **The completion list opens while you type in C and C++, alongside inline completions.** VS Code
  1.125 changed `editor.quickSuggestions` to `{"other": "offWhenInlineCompletions"}`: with an
  inline-completion provider installed, the list was not opened while grey text showed, and otherwise
  only after 750 ms without a keystroke. On the reporter's project the server received 24 completion
  requests in 18 minutes of editing, and none while they reproduced it; it answered them in 21 ms at
  the median. The VS Code extension now sets `{"other": "on"}` as the language default of `[cpp]` and
  `[c]` (workaround `WA-VSCODE-002`, with a canary that fails once VS Code's own default changes);
  other languages keep the editor's default, a `[cpp]` / `[c]` value of yours wins, and one you set for
  every language is overridden with a line in the log saying how to keep it.
- **A completion clangd did not answer is never empty.** A file clangd does not serve (set aside,
  contained, clangd backing off after crashes) and a completion clangd answered with an error get the
  words of the file nearest the cursor and the keywords, as an incomplete list -- not on an import
  line, where only module names belong.
- **Completion in a file clangd rebuilds slowly still arrives.** Past its 1 s budget a completion
  got the file's words and clangd's request was cancelled, so the next keystroke queued behind the
  same rebuild and missed its budget too: on a large Qt and modules project, 25 of 48 completions got
  only words, and in some files clangd's never came. clangd's late answer now keeps working for up to
  10 s, the requests you make while typing the same word wait for it, and it reaches them as soon as it
  comes, its edits ending at each cursor; moving on to another word cancels it. Nobody waits for the
  kept request, so its running out is not a timeout.
- **Saving no longer rebuilds modules nothing open needs.** Each unit mcppls opens in clangd to
  prepare a module was kept open until all preparation was done, and clangd checks every open file
  again on each save: with autosave, the modules behind 15 such units (70 on xlings) were rebuilt at
  every pause, on the workers the file you typed in was waiting for. They are closed as soon as their
  module is built; the BMI stays in clangd's module cache on disk and is reused.

### Writing modules

- **The module you are writing keeps clangd.** With autosave, a module saved mid-edit does not
  compile, and containment took the module's own file from clangd together with its importers -- no
  completion, one error with no position instead of the real ones -- until the next save handed it
  back: in 92 s of writing a module it happened 18 times and half the completions in it were empty.
  The unit whose compile failed now always stays with clangd, which reads it from the editor with its
  real errors and completion. Its importers are contained as before while it does not compile (keeping
  them with clangd made clangd rebuild the modules between at every save, and on xlings an importer had
  no diagnostics for minutes); their completion now has the file's words. New fixture
  `module-edit-autosave`; ux scenario U16.
- **No "preparation stalled" while you edit the module being prepared.** An edit to a source that a
  module being prepared is built from counts as progress; the report lists the preparation units
  running, since when, and what clangd says of each.
- **The status stays ready through a save.** Once the workspace has settled, modules rebuilt because
  of an edit make it *preparing* again only when that lasts 30 s.

### Build descriptions and clangd

- **A partial model's stand-ins arrive together.** clangd reports the modules it cannot find one per
  session, so a partial model (a workspace member whose build program failed) brought its stand-ins in
  one clangd restart each -- four rounds of about 26 s on GalTranslPP. The modules whose units clangd
  already could not scan (a header or the command) are taken with the first report: one replan, one
  restart.
- **The provisional model prepares only the standard library.** With nothing cached, the project's
  modules were built with the kit's commands and built again with the build tool's a few seconds
  later. They are now prepared once the build tool's model is in. (clangd on Windows still crashes on
  GalTranslPP's files while the provisional model serves them: those are the files' own builds, not
  preparation; issue #34.)
- **A clangd busy for minutes without finishing anything is restarted.** A file clangd keeps queued
  is waiting for a worker, and was left to wait; after a module imported by much of a project was
  rewritten with autosave, clangd kept every core busy for more than fifteen minutes with every open
  file queued and nothing done, which the stuck watch (next to no CPU) did not see. A file queued for
  four minutes while clangd finished nothing for three now restarts it as a recovery, with an incident.
  The cause, named by that incident: a module's importer closed in clangd while its build ran --
  containment closes importers of a module that does not compile -- sometimes leaves the build's thread
  spinning with nobody to answer, and three such threads take all of clangd's workers. On Linux a
  worker thread at a full core for half a minute on a file clangd has not said it was building
  restarts clangd at once.
- **Modules of a build that names no C++ standard are read as C++23.** xmake writes no `-std` when
  `xmake.lua` sets no language, and clangd read `import std` with Clang's own gnu++17, which has no
  modules -- it could not even scan GCC's `std.cc`, while GCC 16 built it. Module units whose build
  names no standard are now read as C++23, the standard `import std` is for (the log says so once, and
  the report's `plan.standardAssumed`); a standard the build names is followed. Other units that name
  none are read with their compiler's own default where it differs from Clang's (GCC 16: gnu++20), and
  module units a build puts below C++20 are said once.
- **A standard library module that does not compile says so.** A scan failure of `std`'s own unit,
  or of a unit outside the workspace, was taken for a half-typed file and ignored; it is now an issue
  naming the module and clangd's first error.
- **A clangd crash keeps what an upstream report needs**: LLVM's stack dump as clangd printed it and
  the clangd binary's version and SHA-256, in the incident and in the report's `lastExit` (the
  released clangd carries no symbols).

### xmake

- **xmake describes the project whenever it can run.** mcppls runs xmake privately and no longer
  reads a `compile_commands.json` of yours first, so edits to `xmake.lua` (a late `set_languages`, a
  new target) reach the model with nothing to regenerate. The file is read only when xmake cannot run
  (xmake not on PATH, `mcppls.buildTool: off`, the private run failing on a project it never
  described); then it is watched, and a notice says once when it is older than your `xmake.lua`. Set
  `mcppls.buildTool` to `off` to read your own file on purpose. A project that used to read its own
  file may get one model replacement on the first start after upgrading.
- **Your own `xmake f` choices are followed.** mcppls reads `.xmake/<plat>/<arch>/xmake.conf` (read
  only) and configures its private run with the same platform, architecture, mode, toolchain and
  project options, so a debug project is described with `-O0 -g` rather than release flags. An option
  xmake refuses is left out with a notice, and nothing is ever written into your project.
- A `compile_commands.json` a tool is rewriting is read again a second later (up to five times)
  instead of being reported as invalid and raising a model-stale issue for a moment.
- The scanned-sources model shown while the build tool is still working no longer says the workspace
  is untrusted.

### Editors

- **CLion: one engine per file.** A project CLion models itself (a loaded CMake, compilation
  database or Makefile workspace, or a `CMakeLists.txt` at the root) is answered by CLion's own
  engine, and mcppls starts for every other project, so a file no longer gets two lists of
  completions and two sets of diagnostics. Settings | Tools | mcppls, "Also for projects CLion
  models", turns mcppls on for those too and says once per project that both engines answer.
- **CLion 2026.2.3.** The plugin is built against CLion 2026.2.3 and still loads in 2025.2 and later;
  the Plugin Verifier checks both on every pull request, and a headless CLion 2026.2.3 test (the
  server runs, a wrong import is diagnosed, `import ` completes a module name, closing the project
  stops the server) runs on release branches, nightly and pull requests that reach the plugin.
- **Zed in CI.** A real Zed (Linux, pinned stable, software Vulkan) opens a project and must
  initialize mcppls, send it the open file and receive diagnostics, with the recommended
  `language_servers` setting and with Zed's defaults, where both servers run; quitting Zed must leave no
  mcppls. The extension's choice of server is covered by `cargo test` on every pull request. The Zed
  README and the editors page make putting mcppls first and clangd off an install step.

### Diagnosis and CI

- The diagnostic bundle records the editor settings that decide whether completion shows while you
  type (`client.editor` in `environment.json`), and the report when each method was last answered
  (`requests[*].lastAt`), how much was edited (`documents`), how often completion fell back to the
  file's words (`completion.wordsWithoutCore`), and the preparation units running and since when.
- `mcppls-devtools measure budgets` gives each scenario's numbers over many runs (median, 95th
  percentile, largest, 1.3 x the 95th percentile); nightly's `ux-budgets` job runs it over the night's
  rounds, for budgets set from measurements.
- The Open VSX release check waits up to ten minutes for a VSIX to be listed.
- The report names the ten slowest files and, per file, how many completions got only words
  (`slowestFiles`), what building each file cost clangd (`engines[].details.buildTimes`: preambles,
  imported modules, AST builds) and how often clangd's late completions were used (`completion.late`).
- clangd's include cleaner stays on in module units: a new fixture, `include-cleaner-modules`, holds
  that in a module interface's global module fragment, an implementation unit and an importer it
  reports no header something uses; the troubleshooting page says how to turn it off (`.clangd`).
- Conformance: fixtures `xmake-basic`, `xmake-late-config`, `xmake-user-mode`, `xmake-no-standard` and
  `compdb-midwrite` (the Linux job installs xmake and GCC 16 from xlings), and the runner's checks
  `engine-command`, `status-never`, `write-midway` and `diagnostic-code-lines`.

## [0.0.7] — 2026-09-30

Stability and speed while you write. A person's request is answered within a budget (completion in
a second) instead of waiting up to 30 s behind clangd, background work waits until you pause, and
a restart the build description needs waits until you stop typing. A clangd that keeps crashing is
backed off instead of given up, stale module locks no longer stall preparation, and a warm start no
longer rebuilds every BMI (#30). `std` is no longer built with the project's macros, which gave
GalTranslPP "no member named 'views' in namespace 'std'". Android under Termux works (#32). When
mcppls cannot recover by itself it saves a diagnostic bundle, says where, and VS Code offers a
prefilled report, a cache reset or turning mcppls off for that workspace.
The analysis, the measurements and the plan are `.agents/docs/2026-09-30-stability-performance-plan.md`.

### Responsiveness

- **Requests have a budget.** Completion and signature help wait 1 s for clangd, hover 2 s,
  go-to-definition 10 s. Past it mcppls answers with what it has -- for a completion the words of the
  file nearest the cursor, as an incomplete list the editor asks again for as you type; for a hover
  while modules are being prepared, a line that says so -- and clangd's request is cancelled. On
  GalTranslPP completion took 29.9 s at the median. `mcppls report` counts per method which engine
  answered.
- **clangd gets the machine's threads but one** (2 to 8, fewer with little memory), no longer a
  quarter of the cores; module preparation takes every worker but one while a file you opened waits
  on it, half of them otherwise. `mcppls.engine.workers` overrides the count.
- **Background work waits for a pause.** Implementation units are built for clangd's index one at a
  time, only after 10 s without typing, opening a file or asking for anything, and typing pauses it.
- **Restarts wait for you to stop typing.** A restart for a changed build description, or for work
  clangd would not let go of, waits until typing has paused 3 s (60 s at most). A crash or a clangd
  that stopped answering still restarts at once.
- **Editing does not rerun the build tool.** A saved source whose module declaration and imports are
  unchanged only updates the index; mcpp is asked again only when a build file, another input or a
  source's module structure changes, and edits that keep coming are one run.
- **The build tool's deadline follows the project.** 5 minutes the first time, then three times the
  last run (1 to 10 minutes), instead of a fixed 60 s: GalTranslPP's `mcpp emit build-database` takes
  53-79 s on a 4-core runner, so every session there ran on scanned sources.

### Stability

- **A warm start rebuilds nothing (#30).** The cached model lost the options derived from the build
  database, so clangd got other commands after every restart and rebuilt every module; preparation
  now also records which command each module was built with.
- **`std` is built as the build tool builds it (GalTranslPP).** It was compiled with a
  representative unit's arguments, `-D`s and all; GalTranslPP defines `_RANGES_`, MSVC STL's own
  `<ranges>` header guard, which left `std` without `std::views`. Only the standard library's
  configuration macros (`_ITERATOR_DEBUG_LEVEL`, `_GLIBCXX_ASSERTIONS`, `_LIBCPP_HARDENING_MODE`, ...)
  are kept now; the dropped ones are logged.
- **A clangd that keeps crashing is not given up.** Five exits in five minutes back it off for 1, 2,
  4, then 8 minutes (`engine-crash-loop`, with Restart clangd); mcppls's own engine answers meanwhile,
  and the crash issues clear once clangd has stayed up a minute. 0.0.6 left the session on keyword
  completion until you restarted by hand.
- **Stale module locks are cleared.** A clangd killed while building a module left its lock, and on
  Windows the next clangd waited for it forever (preparation stuck at 12/25 for eight minutes). Locks
  are removed before clangd starts, and one another process holds is removed when clangd says it waits
  on it.
- **Guards no longer mistake a busy clangd for a stuck one.** A timeout behind the server's own
  background work does not set its file aside; "file is queued" does not count as no progress; the
  disk-safety wait follows how long the file took to build; a stand-in replacing a stand-in is no
  change; a unit opened in the background that takes longer than two minutes to build, while clangd goes
  on preparing modules and answering, is left alone instead of restarting clangd (on GalTranslPP that
  restart came every two minutes and preparation never finished). A file set aside and handed back while
  you type turns the status degraded only after 30 s.
- **A server restarted after a crash takes its workspace back at once**: the lease records its owner's
  process, so a dead one is not waited out.
- **A first open is not spoiled by the provisional model.** Until the build tool answers, sources are
  read with the kit's commands; a module clangd could not find that way (xlings: a unit that did not scan
  without the build's include paths) was given a stand-in in the build tool's plan too, and 49 modules
  stayed unprepared until a file changed. What clangd concludes under the provisional model now stays
  with it.
- **Typing on Windows no longer waits for clangd to read.** Everything sent to clangd was written from the
  server's one event loop, and a clangd reading slowly -- a busy 4-core Windows machine, a pipe of a few
  kilobytes, and with a half-typed import the whole file in every change -- held that loop for 11 to 13 s
  an edit on GalTranslPP: completions budgeted at 1 s reached the editor after 8 to 60 s. What is sent to
  clangd now waits on a thread of its own.
- **The event loop says when it is held.** A turn of the server's loop that takes 250 ms or more is logged
  with what it was spent on, and `mcppls report` counts how long client messages waited and what writing
  to the client cost (`server.eventLoop`). That is how the stall above was found.

### Recovery

- **Reset This Workspace's Cache** (VS Code, `:McpplsResetCache` in Neovim, the server command
  `mcppls.resetCache` for other clients) stops the root's engines, removes its cache and starts again;
  nothing to find and delete by hand. A folder reached through a symbolic link or a short name (every
  macOS temporary directory, `RUNNER~1` on Windows) is found as well.
- **Old BMIs are pruned.** Each unit keeps the BMIs of its two newest commands; older ones are removed
  in the background when clangd starts. `mcppls cache --prune` does it for every workspace no server
  has open.
- **What cannot be recovered from is captured when it happens.** A crash loop, a clangd that cannot
  start or run, a corrupt installation or preparation that stopped making progress writes a redacted
  diagnostic bundle at once (`<cache>/bundles/auto-<code>-<time>.zip`, the newest five, never uploaded)
  and the log says where it is and where to report it. VS Code shows one notification: **Report
  Issue...** (a prefilled bug report, and the bundle revealed for you to attach), **Restart Server**,
  **Reset This Workspace's Cache**, **Turn Off in This Workspace**, **Show Logs**. A server that crashes
  three times in three minutes is not restarted forever; the extension keeps a crash report.
- **`mcppls.enable = false`** keeps mcppls off in one workspace, so a project it cannot serve does not
  mean uninstalling it; the status item turns it back on. Zed, CLion and Neovim have their own ways,
  in their READMEs.
- **Issue forms**: the bug report asks for what the extension prefills and a maintainer needs; there is
  a feature request form.

### Platforms

- **Android under Termux (#32).** termux's PRoot answers `execveat` with a directory handle with ENOSYS,
  and its seccomp fast path left a register rewritten after `openat`: the server could not start
  clangd ("not supported") and failed to read files ("outside every preopened directory"). The vendored
  openkal-linux 0.15.1 falls back to `execve`, the server prefers it under PRoot and reports the sandbox
  (`server.sandbox`); an engine that cannot be started there is `engine-start-failed`, with why.
- **Code - OSS and VSCodium** run the extension's end-to-end suite in CI, as VS Code does.

### Editors

- The extension's description starts with its name: "mcppls - C++20/23 named modules that just
  work: ...".
- The palette command Reset This Workspace's Cache and the server's `mcppls.resetCache` have different
  ids: `vscode-languageclient` registers every command a server advertises, and the same id made the
  client fail to start ("command 'mcppls.resetWorkspaceCache' already exists").

### Specifications

- **S3:** an issue may carry `bundle`, a diagnostic bundle the server wrote for it by itself; it is
  redacted, stays on the machine, and a client offers once, without blocking, to report it (S3-4-22 to
  S3-4-25). `mcppls.resetCache` (section 5.6, S3-5.6-1 to S3-5.6-3); the codes `engine-crash-loop`,
  `engine-start-failed`, `preparation-stalled` and `payload-corrupt`; the report's `server.sandbox`. All
  additive; protocol version 1.

### Also

- Conformance fixtures: `reset-cache`, `mcpp-emit-edits`, and user-experience scenarios with response
  budgets on pinned mcpp and xlings checkouts (`ux-mcpp`, `ux-xlings`; the `ux` CI job on every pull
  request, three rounds a night): first responses, typing with and without autosave, fan-out saves,
  build-graph changes, killing clangd or the server, stale locks, a truncated module file, a git checkout,
  idle CPU and memory, the status timeline. The budgets were calibrated on the CI runner's first rounds;
  the plan's record says why each one moved. `clangd-cannot-load` checks the automatic bundle. CI adds `code-oss-e2e`, `proot` (the server under termux's PRoot on x64
  and arm64) and a nightly Termux job.
- Upgrading: the first start after installing 0.0.7 describes the project again (the model cache's
  format changed) and may build its modules once; every warm start after that rebuilds nothing. Settings,
  caches and editor configuration need no change.
- Deferred: Windows CPU readings for the stuck-clangd watch (K-1), lower priority for clangd (R-9),
  symbolized crashes (K-3; clangd still crashes on Windows under the provisional model, and is backed off
  with a bundle written), moving a broken module cache aside by itself (C-3).

## [0.0.6] — 2026-09-27

Go-to-definition reaches implementation units, even when you never opened them. A Qt project's
`ui_*.h` is found, its forms and resources no longer confuse clangd, and a bad moment no longer moves
the whole project to another standard library. xmake and meson projects are described like CMake and
mcpp ones, never by writing into your project, and a build description that needs a download no longer
leaves you waiting: the project is served at once, you are asked once in a notification you may ignore
forever, and building in your own terminal is picked up by itself. Every setting now has one
definition and one generated reference.
The analysis, the measurements and the plan are `.agents/docs/2026-09-27-qt-demo-navigation-discovery-plan.md`.
Upstream defects behind this are registered in issue #24 (UP-08, UP-17, UP-M4, UP-M5).

### Navigation

- **Definitions in implementation units are reached.** clangd's background index compiles a module
  unit without building the modules it imports, so a definition in an implementation unit was indexed
  apart from its declaration, or not at all, until that file had been open: in mcpp's own `prepare`
  module go-to-definition stayed on the declarations for as long as it was measured. mcppls now builds
  implementation units through clangd's foreground, a few at a time (WA-CLANGD-008): the units of an
  opened file's module and of the modules it imports first, a unit changed on disk (a `git pull`, another
  program, a coding agent) again, the rest once clangd is idle. On mcpp's source the same requests now
  land in the `.cpp` from the first answer. `mcppls.index.primeImplementationUnits = off` turns it off.
- **A definition clangd cannot link is found by name.** The search a definition request starts opens the
  units that define the name first, not the first four by file name; when clangd still answers with the
  declaration, the definition found in the module's units is the answer (same name, scopes and parameter
  types, never a guess). Asked on a definition, you still go back to the declaration.
- **An implementation unit that does not build says so** (`implementation-unreadable`): usually a missing
  header, which is why its definitions cannot be reached.

### Projects and build tools

- **Qt and other build rules (mcpp-community/mcpp#724).** A rule's inputs (`.ui`, `.qrc`, `.ts`) are no
  longer given to clangd as C++. The files a rule generates are read from your project's own `target/`
  when a build has written them; otherwise the status says which are missing and offers **Build in
  Terminal**, and the model reloads by itself once they appear. On qt-demo, `main.cpp` went from a fatal
  missing `ui_mainwindow.h` to no diagnostic at all.
- **xmake and meson are supported.** xmake is asked with its own `xmake project -k compile_commands`
  (nothing is compiled) and meson with `meson setup`, both into mcppls's cache, offline; your project
  directory stays untouched (checked file by file). Both are L3. `check`, `query` and the project
  boundaries now know `xmake.lua` and `meson.build`, and a subproject's own `CMakeLists.txt`,
  `xmake.lua` or `meson.build` belongs to the project above it.
- **CMake follows your presets and never downloads without asking.** A preset's `binaryDir` is looked
  for, and mcppls's private configure uses the preset's generator, toolchain file and cache variables.
  That configure is disconnected from its first run (`FETCHCONTENT_FULLY_DISCONNECTED`), where 0.0.5 let
  the first one download.
- **A download is asked about once, and never waited for.** When the build description needs something
  that is not on the machine, the project is served from its sources at once; a notification offers
  **Download and Continue** (the build tool may reach the network, this once), **Run in Terminal** or
  **Don't Ask Again**, and may stay unanswered forever. Meanwhile the description is asked again offline
  after 30 s, 1 and 2 minutes and every 5 after, and at once when a build file changes, so a build you
  run in your own terminal upgrades the project by itself. A partial answer from mcpp whose missing
  member needs a download gets the same offer.
- **The first model comes sooner.** With nothing cached, the build tool has 2.5 s (was 10) before the
  project is served from its sources by both engines; its model replaces that one in one switch that
  never counts against clangd's restart budget.
- **`mcppls.buildDiscovery = off`** detects no build system at all; `mcppls.buildDiscovery.providers`
  leaves out single ones.

### Engine

- **The standard library is not replaced by a bad moment.** A report that `std` has no unit, from a
  clangd that had not read the database the unit had just joined, moved qt-demo from libstdc++ to the
  bundled libc++ and restarted clangd twice. Such a report is now weighed against the database clangd
  has read, and the kit replaces the toolchain's `std` only when std's own unit fails to compile or does
  not exist.

### Settings

- **Every setting has exactly one definition** (`src/config/settings.cppm`), from which the command line,
  `initializationOptions`, `workspace/didChangeConfiguration`, `mcppls report` and the reference in
  [docs/30-settings.md](docs/30-settings.md) (both languages) are derived; a test holds them and the VS
  Code settings to it. `mcppls settings` prints the reference. A value outside a setting's vocabulary
  falls back to its default and is reported, never applied. New: `buildDiscovery`,
  `buildDiscovery.providers`, `buildDiscovery.askBeforeDownload`, `index.primeImplementationUnits`, and
  command-line spellings for `compiler` and `semanticKit`. A settings change that only needs the model
  reloaded is applied without a restart.

### Specifications

- **S3:** a `producer-needs-download` issue may carry `askOnline`, and `mcppls.describeOnline` describes
  the project once with the network; a client offering it never blocks on the question and ignores an
  answer that comes after the need is gone (S3-4-16 to S3-4-21). `project.source` gains `xmake` and
  `meson`; the report carries the settings in effect. All additive; protocol version 1.

### Also

- Conformance fixtures: `mcpp-partition-definition`, `mcpp-rules-generated`, `mcpp-emit-provisioned`,
  `mcpp-emit-partial-download`, `build-discovery-off`, `cmake-fetchcontent-offline`; `mcpp-emit-needs-download`
  covers the offer and the online description.
- mcpp 2026.9.27.1 cannot build this repository (mcpp-community/mcpp#725); CI stays on 2026.9.26.1.

## [0.0.5] — 2026-09-26

Issue #23 is fixed: modules are built again for projects compiled with LTO for the MSVC ABI. An
autosave of a half-typed `import` no longer stalls clangd, restarts can no longer leave it stuck, a
project mixing C++23 and C++26 reads both, and one command exports everything a problem report needs,
with your name, paths and secrets replaced.
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
- **A build tool's partial answer is used.** mcpp now describes every workspace member it can plan,
  with an `error` naming each one it could not (S2 0.3.0; mcpp-community/mcpp#699). Before, one
  member's failure lost the whole database, which is why issue #23's CI read GalTranslPP from scanned
  sources. The rest is used, and the status names the missing member by its build description
  (`producer-partial`).
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

### C++26

- **A C++26 file can import what a C++23 one built.** A module's BMI is only imported under the
  standard it was built with, so in a project mixing C++23 and C++26 the C++26 files lost `import std`
  and every module ("C++26 was disabled in precompiled file"). The module units of a context are now
  read with one standard, the newest they name; plain units keep their own, and the status profile
  names the standard (`standard`, S3).
- **Sources nothing describes are read as C++26** with the semantic kit, GCC 14 and later, and Clang 17
  and later (C++23 before), so C++26 library names such as `std::saturating_add` are there.
- Contracts (P2900) and reflection (P2996) are not in clang 23 and show clang's errors; VS Code colors
  `contract_assert`, and `pre` and `post` as contract specifiers.

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
  `compdb-rejected-command`, `mcpp-emit-wait`, `compdb-lto-msvc`, `inferred-bom`, `inferred-cxx26`,
  `compdb-mixed-standards`, `mcpp-emit-partial`, `completion-keywords` and `diagnostic-bundle`.
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
