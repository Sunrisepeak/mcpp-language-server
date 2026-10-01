# When something is wrong

## Start here

**C++ Modules: Collect Diagnostic Report** (or `mcppls report` on the command line). It opens JSON
carrying what almost every question turns out to need:

| In the report | Answers |
|---|---|
| `project.source`, `project.tier`, `project.level` | Where the build description came from (`tier`, the README's L1..L4), and how complete its document is (`level`, S1's own number — a different question from `tier`) |
| `project.notices` | Facts worth knowing that cost no feature: a stale database entry dropped, a generated module recovered instead of stubbed, a producer negotiated in place of the project's pinned mcpp |
| `project.origin` / `firstOrigin` | Whether this session started from the cache, from the build tool, or from scanned sources |
| `project.producer`, `producerRun` | Which build tool ran, how long it took, how it ended, whether it was offline |
| `toolEnvironment` | Which environment build tools were started in, and the **names** of the variables that differ from the editor's (never the values) |
| `toolRuns` | The last twenty external runs, each with its command, duration and outcome |
| `plan` | What the engine was given: entries, stand-ins, what was left out and why |
| `engines` | clangd's state, restarts, files set aside, and `workarounds`: the clangd defects this server works around for this version |
| `requests`, `completion`, `documents` | For each method, how many requests came, how fast they were answered, by which engine and when the last one was (`lastAt`); how often completion was answered with the file's words; how many edits there were and when the last one was — edits going on while `requests["textDocument/completion"].lastAt` stands still means the editor stopped asking |
| `events` | A journal of the session |
| `logTail` | The end of the log |

Log files outlive the editor: the report names the path, and they are kept under the cache
directory with timestamps.

The report is made to be shared: your home directory is `~` in it, your user and machine names are
`<user>` and `<host>`, and anything that looks like a secret (a token, a password, an API key, an
e-mail address) is `<redacted>`. The project's own paths are kept — they are what it is read for.

**C++ Modules: Export Diagnostic Bundle** goes further: one zip, written under the cache directory's
`bundles/` (the newest five are kept) and never uploaded, with everything a problem usually needs —

| In the bundle | What it is |
|---|---|
| `report.json` | The report above |
| `environment.json` | System, editor and extension versions, the other C/C++ extensions, your mcppls settings, the editor settings that decide whether completion shows while you type (`client.editor`: `editor.quickSuggestions` for C++ and where it comes from, inline suggestions, autosave, the inline-completion extensions installed), the payload, the toolchains found, and a few environment variables (`PATH`, `LANG`, `LC_*`, `MCPP_*`, `XLINGS_*`) — no other |
| `logs/` | The server's logs of the last three sessions and any other of the last day, and the extension's own log |
| `incidents/` | What the server wrote down when clangd crashed, hung or was set aside — for a crash, LLVM's stack dump as clangd printed it and the clangd binary's version and SHA-256, what an upstream report needs |
| `engine/` | The database clangd was given, and the plan behind it |
| `manifest.json` | Every file with its size and SHA-256, and how many replacements each redaction rule made |

— with the same replacements in every file. Before anything is written, the bundle is searched for
your home directory, user name and host name in every spelling; if any is left, **no bundle is
written** and the message says which file, and *Retry with Project Paths Hidden* replaces the
project's paths too. Source files are never included; an incident carries only the lines it is
about. Other editors run the same command as `workspace/executeCommand` `mcppls.exportBundle`, and
on the command line:

```bash
mcppls report --bundle problem.zip --root path/to/project   # --hide-project-paths, --no-source-excerpts
```

Crash dumps are left out unless asked for (`--include-dumps`): they hold memory, which cannot be
redacted. `--no-redact` keeps everything as it is, for looking at a problem on your own machine; it
is not offered in the editor.

## Symptoms

**Nothing works — no go-to-definition anywhere.** Look at `project.source` in the report. If it is
`inferred` on a project that has a build system, the build tool did not answer; `project.issues`
and the last `toolRuns` entry say why. A common cause is the build tool needing a download: the
status bar offers to run it in your terminal.

**"xmake needs libtool, libpthread-stubs downloaded".** Those names are not libraries your code is missing:
they are what xmake would fetch or build for the packages your `xmake.lua` requires, build tools included
(a package set to build from source brings its own, such as `libtool` or `meson`), and mcppls stayed offline
so that it would not download them on its own. Three ways on: choose **Download and Continue** in the
notification, run `xmake` in your terminal (the description is read again when it is done), or install them
with your system package manager (`apt install libtool libpthread-stubs0-dev`, `pacman -S libtool`, `brew
install libtool`): xmake's package recipes name the system packages, and xmake uses the system's when it
finds it. If the status says `producer-install-failed` instead, the network was allowed and the install
failed; the message has xmake's error lines and the path of its `install.txt` log, and the usual cause is
a tool the source build needs (autotools, a compiler) that is not installed.

**Go-to-definition works, completion does not, or the standard library is missing.** Look at
`profile`. A semantic kit means no usable compiler was found; `import std` still resolves, but
diagnostics come from libc++ rather than from your toolchain.

**It was fine, then a module stopped resolving.** `plan.standIns` lists modules nothing provides —
mcppls gives them stand-in units so that one broken module does not take the rest of the project
with it, and names each one in the log with the reason. The real failure is in `plan.issues` or in
your build.

**No completion list while you type in VS Code, or it shows only when you stop and wait.** VS Code
1.125 and later, with an inline-completion extension (GitHub Copilot is built into VS Code), wait for
the inline completion before opening the list, and do not open it at all while its grey text shows:
`editor.quickSuggestions` defaults to `{"other": "offWhenInlineCompletions"}`. The server is not even
asked. From 0.0.8 the extension sets `{"other": "on"}` for C and C++ files (`WA-VSCODE-002`), so the
list and the grey text show side by side. That default outranks a `editor.quickSuggestions` you set
for every language; the log says so once, and setting it under `"[cpp]"` and `"[c]"` keeps yours. A
diagnostic bundle shows it: `client.editor` in `environment.json`, and in the report
`requests["textDocument/completion"]` against `documents` (edits going on, no completion requests).

**A module does not compile.** Only what imports it, directly or not, is affected: those files are
answered at once by mcppls's own engine (module navigation, symbols, `import` completion, and the
words of the file for other completion), carry one `module-failed` diagnostic on the import that leads
to the failure, and are not sent to clangd until the failed module's own source or command changes;
everything else keeps clangd. The unit whose compile failed is never taken from clangd (0.0.8): it is
the file you are writing, and clangd reads it from the editor with its real errors and completion —
with autosave, what is on disk mid-edit does not compile as a rule. This is a problem
in the code, so it is told where it is, as diagnostics: the status stays *ready* (listing
`modules-doomed`, category `code`) and is never *preparing* for good. The engine's `doomedModules`
and `filesRoutedToOwnEngine` in the report list them.

**What the status says, and what it does not.** A mistake in your code (a missing `;`, an import
of a module nothing provides, a module that does not compile) is a diagnostic where it is, in the
Problems list; the status stays *ready*. *degraded* means the server lost something it would
otherwise give you, and names what and where: "clangd stopped responding on main.cpp",
"the workspace is not trusted", "the macOS SDK was not found". Each status issue carries a
`category` (`code`, `engine`, `environment`, `project`) saying whose problem it is; only the ones
other than `code` make the state *degraded*, and only once that has lasted three seconds, so a
condition that passes by itself never reaches the status bar. Once the workspace is *ready*, modules
rebuilt because of an edit (a save of a module most of the project imports) make it *preparing* again
only when that takes more than 30 seconds (0.0.8); the way back to *ready* is never held.

**The editor froze while typing an `import` (0.0.3 and earlier).** clangd 23.1 never finishes a
file in which a module name ends in `.` at the end of its line (`import hello.`, `export module a.`),
and every later version of the file waits behind it: typing any dotted import went through that
text. mcppls 0.0.4 gives clangd the line with `;` right after the dot instead, which clangd reports
at once (workaround `WA-CLANGD-001`); `engines[].details.workarounds` in the report lists it.

**While typing an import with autosave on, the file loses clangd for a few seconds.** clangd reads a
file's imports from its text on disk, not from the editor (0.0.4 and earlier could stall there for
good: every request for the file unanswered after `import hello.` was autosaved). When a save puts
something on disk clangd would stall on — a module name ending in `.`, or an import of a module the
project does not have (yet) — the file is answered by mcppls's own engine until it is saved again;
the status lists it as `file-unsafe-on-disk`, category `code`, with the reason, and stays *ready*.
A module nothing provides gets a stand-in within a second of the save, and the file goes back to
clangd once clangd has read the database with it, about six seconds later.

**"Import directive must end with a ';'" on the wrong line, or "module X not found" for an import you
just typed.** clangd reports a directive missing its `;` on the code after it; mcppls moves the
diagnostic back onto the directive (workaround `WA-CLANGD-006`). An import typed and not saved yet is
not built by clangd until the file is saved (it reads imports from disk); while the module is in the
project, that is an information-level "module 'X' is in the project; clangd loads it once the file is
saved", not an error (`WA-CLANGD-007`).

**A view "can be declared 'const'", and declared so it does not compile (clang-tidy).** clang-tidy 23.1's
`misc-const-correctness` says so of a variable holding a `std::views::filter`, `drop_while`, `chunk_by`
or `split` view, or a view built on one, although such a view has no const `begin()` (issue #37). It runs
only when your clangd config sets `Diagnostics.ClangTidy.FastCheckFilter: None`. From 0.0.9 mcppls drops
that diagnostic and keeps the check's other ones (workaround `WA-CLANGD-010`). Before then, or to keep
the check quiet altogether, set `misc-const-correctness.AnalyzeValues: false` under
`Diagnostics.ClangTidy.CheckOptions` in the project's `.clangd`.

**Completion is slower than plain clangd on a project without modules (0.0.8 and earlier).** clangd's
modules support scans a file's module dependencies again for every completion, which costs a heavy
header such as `vulkan.hpp` about 170 ms each time (issue #37). From 0.0.9, a project that has no module
units, no module imports and no `import std` gets clangd without its modules support; the first import
you add restarts clangd with it (workaround `WA-CLANGD-009`, `engine-restart` in the report's
`events`).

**"clangd would not finish main.cpp".** A file's build ran past its budget — five times its own
last build, never under 20 s — while the editor waited on it: clangd will not finish it, busy or
not. The file is answered by mcppls's own engine, with module-level features, until its text
changes (the exact text clangd stopped on is never given back to it), and clangd is restarted at
once without it. The `events` journal has an `engine-spin` entry with the numbers.

**Is a workaround still needed?** `--disable-workaround WA-CLANGD-<n>` (repeatable) turns one off;
the log's first lines name the ones in use. Each has a canary in the conformance suite
(`workaround-canaries`) that fails once a clangd update fixes its defect.

**The editor found a different compiler than my terminal.** `toolEnvironment.source` should say
`login-shell`. If it says `editor`, the reason is in `toolEnvironment.reason` — an editor started
from a desktop entry has none of your shell configuration. `mcppls.toolEnvironment` controls this.

**Everything is slow to start.** The second session should be fast: the model is cached with a
fingerprint of everything the build tool read, and a session that matches plans with it at once and
confirms it in the background, and modules already built are reused, not built again (0.0.6 and
earlier rebuilt every one on a warm start, issue #30). `project.firstOrigin` says which happened. If
it is always `producer`, the fingerprint is not matching — the report's `project.producerRun` and the
build files' timestamps are where to look.

**Completion shows only words from the file, or hover says modules are being prepared.** A request
has a budget for clangd — completion and signature help 1 s, hover 2 s, go-to-definition 10 s — and
past it mcppls answers with what it has. Completion is then the words of the file nearest the
cursor, an incomplete list, so the editor asks again as you type; hover, while modules are being
prepared, is a line saying so. It is clangd being busy with modules, not a failure. From 0.0.8,
clangd's late completion is not thrown away: it keeps working for up to 10 s, the requests you make
while typing the same word wait for it, and it goes to them as soon as it comes, so in a file clangd
rebuilds slowly the list still arrives before you finish the word. `requests.<method>.answeredBy`
in the report counts, per method, which engine answered; `engineP50Ms` and `engineP95Ms` are the time
clangd (or another engine) took for the requests it answered and `overheadP50Ms` and `overheadP95Ms` the
time mcppls added around it, so a slow completion shows whose time it was; `completion.late` counts clangd's late
answers and the requests they went to; `slowestFiles` names the ten slowest files and, per file,
how many completions got only words; `engines[].details.buildTimes` says what building each file
cost clangd (preamble, imported modules, AST builds).

**Unused-include warnings in module units.** They are clangd's include cleaner, on by default, and
mcppls leaves it on: in a module interface's global module fragment, an implementation unit and an
importer it reports only headers nothing uses, as in any other file (a conformance fixture keeps
that true across clangd updates). To turn it off, add `Diagnostics: { UnusedIncludes: None }` to the
project's `.clangd` or to your clangd `config.yaml`; the clangd mcppls starts reads both.

**The editor is sluggish while modules are prepared.** clangd gets the machine's threads but one
(between 2 and 8, fewer on a machine with little memory); `mcppls.engine.workers` (`auto` or a
number) overrides it. Preparation uses every worker but one when a file you opened is waiting on
it, half of them otherwise. Implementation units are built for clangd's index — what go-to-definition
into `.cpp` files you never opened needs — one at a time, and only after 10 s without typing,
opening a file or asking for something. A restart clangd needs for a changed build description waits
until typing has paused for 3 s (at most 60 s); a crash or a clangd that stopped answering still
restarts at once. From 0.0.8 each unit mcppls opens in clangd to prepare a module is closed as
soon as that module is built (its BMI stays in clangd's module cache on disk): kept open until all
preparation was done, every one of them was checked again on each save, and with autosave that took
the workers the file you typed in was waiting for.

**clangd keeps restarting.** `engines[].restarts`, `engines[].details.restartBudget` and the `events`
journal. Each reason has its own budget of three restarts in ten minutes: the engine database
changing (`plan`), recovering a clangd that stopped answering or spun (`recovery`), and clangd
exiting (`crash`). Past it, the next restart of that kind waits one, two, four, then eight minutes
— it is backed off, not refused, so a stuck clangd always comes back — and the status says so
(`engine-restart-capped`) with a **Restart clangd** button (other editors: `workspace/executeCommand` `mcppls.restartEngine`), which restarts
at once and is never counted. Switching the toolchain, the profile or the context is never counted
either, and a module that does not compile is never a reason to restart. Every change of the engine
database is logged with what it changed (`engine database changed: … compiled otherwise (main.cpp:
argument 3: -O0 -> -O2)`), so a burst names its cause.

**"clangd crashed while building NormalJsonTranslator.Core.cpp".** clangd names the file it crashed
on (its crash context), and that file is set aside: answered by mcppls's own engine while clangd is
restarted without it. `engines[].details.lastExit` in the report has the exit code, the file, what
clangd was doing and, on Windows, the exception code. After five exits in five
minutes clangd is started again after 1, 2, 4, then 8 minutes, and the status says so
(`engine-crash-loop`, with **Restart clangd**); it is not given up for the session. The crash and
timeout issues clear by themselves once clangd has stayed up for a minute. The last resort is
[below](#when-mcppls-cannot-recover-by-itself).

**"mcpp could not describe tools/updater/mcpp.toml".** The build tool described the rest of the
workspace and said which part it could not (a member whose build program failed, say); that part's
files are read with what the rest gives them, and the rest works as usual (`producer-partial`, S2
0.3.0). Fix what the message names, and the next reload describes it too.

**"clangd rejected the compile command for module scanning".** Before it builds a module, clangd
scans each unit of the database for its imports, with that unit's compile command; a command the
compiler driver rejects fails the scan, and no module is built — issue #23's `LTO requires
-fuse-ld=lld` was one. The status names the first rejection in the driver's own words (category
`environment`); `engines[].details.scanFailures` counts them. A header the command cannot find is
told the same way (category `project`). A scan of a file you are typing fails all the time and is
only counted.

**"C++26 was disabled in precompiled file".** A module was built with one C++ standard and imported
under another; clang refuses that. mcppls reads the module units of a context with one standard,
the newest they name (`plan.languageStandard` in the report, `standard` in the status profile), so
this should only come from a module clangd built before 0.0.5 — it rebuilds on the next change — or
from the build itself mixing standards, which the build tool's own compiler will refuse too.

**Right after opening a project, only module-level features for a while.** A project whose
build system was found gets clangd once its build tool has described it — as long as the build tool
takes, up to its deadline (5 minutes the first time, then three times the last run, between 1 and 10
minutes) — rather than with a guess from its sources that clangd would then have to
unlearn; mcppls's own engine answers module navigation, hover on imports and `import` completion
meanwhile. A second session starts from the cached model at once.

**Preparation is stuck on Windows, or clangd waits forever for a module.** A clangd that was killed
while building a module leaves a lock behind, and the next clangd waited on it (0.0.6 and earlier).
mcppls clears stale module locks before clangd starts, and again whenever clangd logs that it is
waiting on a lock another process holds.

**The server crashed and the restarted one has the workspace at once.** A restarted server takes its
workspace back without waiting: the lease records its owner's process id and start time, so on Linux a
dead owner is recognized at once (elsewhere its lease expires within half a minute).

**Where the server writes down what went wrong.** A crash, a stuck or spinning clangd, a file set
aside, a restart held back and a broken workaround premise each leave an *incident*: a directory
under the workspace's cache (`incidents/<UTC time>-<kind>/`, the newest twenty, for a week) with
what led up to it, clangd's latest log lines (clangd logs at `info` into memory, never to the default
log), each file's lines where the editor's text and the disk's differ, and which of clangd's threads
used the CPU. The diagnostic bundle carries them.

**"clangd stopped making progress; it was restarted".** clangd left a request unanswered, answered
nothing else meanwhile, and used next to no CPU for five seconds: it was waiting for something that
was not coming, not compiling (a long compile keeps a core busy, and is left alone). The `events`
journal has an `engine-stuck` entry with the numbers. clangd 23.1 has been seen to do this after a
module's source changed twice within a second. On Windows, where the server cannot read clangd's CPU
time, this is not detected; files clangd stops answering for are still set aside one by one.
The same words come for the opposite case (0.0.8): clangd busy on every core for minutes while the
files you have open stay "queued" and nothing finishes -- no diagnostics for any file, no answer, no
module, for three minutes, with a file queued for four. That was seen after a module imported by much
of a project was rewritten with autosave; the stuck watch above does not see it, since clangd keeps
the CPU busy. The `events` journal has an `engine-busy-without-progress` entry, and an incident keeps
clangd's log. On Linux the cause is found sooner: a clangd worker thread that has kept a core busy for
half a minute on a file clangd has not said it was building (a build clangd let go of when the file was closed, and
never stopped) restarts clangd at once (`engine-orphan-spin`, with the thread and its CPU in the
incident).

**"The bundled clangd cannot run on this system".** clangd did not start at all: the system's
program loader refused it, and its message is in the status and the log (for example
``version `GLIBCXX_3.4.30' not found``). No restart can change that, so none is tried; mcppls's own
engine answers module navigation, `import` completion and module diagnostics meanwhile. On Linux
arm64 the bundled clangd needs glibc 2.34 and a GCC 12 libstdc++ — the systems it runs on are listed
in [the install guide](00-install.md). Elsewhere it usually means a musl system (Alpine) or a damaged
payload. An editor that starts `mcppls` itself can give it a clangd of your own, 23.1 or later, with
`--clangd PATH`.

**On Termux or PRoot.** `mcppls report` shows the sandbox the server runs in as `server.sandbox`
(`proot`, say). An engine that cannot be started there is the status issue `engine-start-failed`, with
what went wrong; see [Android, under Termux](00-install.md#android-under-termux).

## Resetting a workspace's cache

When preparation never finishes, the engine keeps crashing or a model looks wrong, reset the cache of
the one workspace instead of deleting directories by hand:

- **VS Code**: **C++ Modules: Reset This Workspace's Cache**
- **Neovim**: `:McpplsResetCache`
- **Other clients**: `workspace/executeCommand` `mcppls.resetCache`

It stops the root's engines, removes its cache — the models, the engine database, and clangd's
module cache and locks — and starts again. The logs stay. The first session afterwards is a cold
one.

Old modules are pruned by themselves: each unit keeps the built modules (BMIs) of its two newest
commands, and older ones are removed in the background when clangd starts. `mcppls cache --prune`
does it for every workspace no server has open.

## When mcppls cannot recover by itself

clangd that keeps crashing, an engine that cannot start or cannot run on this machine, a corrupt
installation, or preparation that stopped making progress: mcppls writes a diagnostic bundle at once,
to `<cache>/bundles/auto-<code>-<time>.zip` (redacted like any bundle, the newest five kept, never
uploaded), and logs where it is, with the link for reporting the problem. VS Code shows one
notification for it, once per problem, with:

| Button | What it does |
|---|---|
| **Report Issue…** | Opens a GitHub bug report with the fields filled in, and shows you the bundle to attach |
| **Restart Server** | Restarts the server |
| **Reset This Workspace's Cache** | As [above](#resetting-a-workspaces-cache) |
| **Turn Off in This Workspace** | Sets `mcppls.enable` to `false` for the workspace |
| **Show Logs** | Opens the log |

A server that crashes three times in three minutes is not restarted again; the extension saves a
crash report — a folder with the client log, the end of the server log, its stderr and basic
information — under `crash-reports/` in its global storage, and says where.

## Keeping mcppls off in one workspace

`mcppls.enable` (default `true`, per workspace) is the switch. In VS Code, **C++ Modules: Turn Off in
This Workspace** sets it and **Turn On in This Workspace** sets it back; the status item reads "C++
Modules: off in this workspace" while it is off, and clicking it turns mcppls on. For Zed, CLion and
Neovim, see [10-editors.md](10-editors.md).

## Filing a bug

Use the templates at [New issue](https://github.com/Sunrisepeak/mcpp-language-server/issues/new/choose):
**Bug report** asks for the version, editor, operating system and build system, what happened, what
you expected and the steps, and takes the diagnostic bundle (**C++ Modules: Export Diagnostic
Bundle**, or `mcppls report --bundle`) or, if there is none, at least the diagnostic report.
**Report Issue…** in the notification above fills the form in for you. Both the bundle and the
report have your user name, home directory, host name and secrets replaced, and neither carries the
contents of your files; read them before attaching all the same. A defect in clangd or mcpp that
mcppls works around is collected in [issue #24](https://github.com/Sunrisepeak/mcpp-language-server/issues/24):
read it before filing, and add a comment if yours is new.
