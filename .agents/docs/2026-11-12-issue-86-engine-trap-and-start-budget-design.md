# Issue 86 after 0.22.0: the engine's trap, and a session that cannot run out of engine starts

Status: proposal for review · not implemented · evidence: the reporter's four diagnostic bundles
(0.0.20, 0.21.0, 0.22.0 twice — comments 6758603816, 6758928222, 6812093165, 6816991554 of #86),
and the temporary harness of draft PR #87 (branch `tmp/issue86-systop-repro`, workflow
`issue86-repro`), which runs the released 0.22.0 payload on the reporter's project
(`yizhinailong/systop` at `b506132`) on hosted Windows runners; its results (runs 43175887538,
43178642084, 43181924438) are the assets of the temporary pre-release `issue86-evidence`. Target:
the next series release (0.23.0) for the server's part; an engine release (mcppls-clangd,
`23.1.0-mcppls.11`) and a platform pin for the other two.

The goal this plan serves: **a file that makes clangd trap costs the session that file and one
engine start — not the engine, and never the session's ability to start one; and a trap leaves
enough on the machine that the next report names the function it happened in.**

| # | Item | Decision | Owner | Section |
|---|---|---|---|---|
| E1 | clangd ends by `0xC000001D` while building `src/ui/monitor_view.cppm`: an unchecked `*HeaderID` in its include analysis | check the optional at both sites (engine patch, upstreamable); until the engine ships, the server turns include analysis off where the defect can occur | engine + server | §1.2, §1.3 |
| P1 | On Windows 11 24H2 builds (26100, 26200) the ok image gives an open file another identity than its name | one identity for a file, however it was reached (platform); the engine's CI gets a runner of that build | platform + CI | §1.4 |
| E2 | A trap inside the ok image says nothing: no code, no file, no frame | the runner reports a trap (condition, image offset, frame-pointer chain) on stderr; the server keeps it | platform + server | §2 |
| S1 | Every abnormal exit is recorded as `-1` | keep the condition's number the platform already gives; name it in the log, the issue and the incident | server | §3.1 |
| S2 | The file set aside is guessed from requests and recent edits, and guessed wrong | the file whose AST build was in flight when the engine ended is the suspect; a named suspect needs no second exit | server | §3.2 |
| S3 | A file set aside comes back on every edit, on a rebuilt closure, and in every new session | a crash ledger keyed by file and engine, kept in the workspace cache; one retry per term, whatever the reason | server | §3.3 |
| S4 | Crash restarts spend the sandbox's grant table to its last slot | a start budget the engine knows: automatic restarts stop while a reserve is left; the refused start offers *Restart Server* | server | §3.4 |
| W4 | The grant table never returns a slot (64 a process, ever) | one slot per distinct grant for the life of the process, in the platform; a pin bump | platform | §4 |
| D | What a report carries, and what the logs say | the reproducer command in the bundle; true counts in messages; the release payload not `dirty`; the kit line names the project's target | server | §5 |
| T | Tests and CI | a Windows module-unit fixture, the restart loop past the table's capacity, a trap fixture | server + engine | §6 |

## 0. What the evidence says

### 0.1 The reports

All four bundles are one machine, one project: Windows 10.0.26200, VS Code, `systop` (target
`x86_64-pc-windows-msvc`, clang 23.1.3, MSVC STL 14.44.35207). The last two are the released 0.22.0
(payload commit `9e31bf6`, clangd `23.1.0-mcppls.10`, sha256 `47a09679…`), so the reporter is not on
a stale build. (The report's "target x86_64-w64-mingw32" is the log's semantic-kit line, not the
project's target — §5.)

- **The engine exits, on every version.** 38 engine exits across the bundles: 11 on 0.0.20 (clangd
  `mcppls.9`), 7 on 0.21.0, 20 on 0.22.0 (`mcppls.10`). One signature: clangd's log ends without an
  error, one to three seconds after `Built preamble of size ~257 KB` for a file that imports
  modules (the size of a skipped preamble: the module path, UP-23). In 31 of the 38,
  `src/ui/monitor_view.cppm` was being built in the last eight seconds; the other seven are
  `main.cpp`, `test_monitor_view.cpp` and other units on the same path. The reporter's own
  `okrun clangd.okb --check=src/ui/monitor_view.cppm` ends in `Building AST...` with exit
  `0xC000001D` (illegal instruction), every time.
- **The exit is silent by construction.** The image is built with libc++'s fast hardening, a global
  stack guard and frame pointers (mcppls-clangd `openkal/clangd/mcpp.toml`): a failed hardening
  check and a smashed guard both end in a trap instruction, with no message. Nothing in the image
  catches it, so clangd's own crash context ("crashed while building X", LLVM's stack dump) is
  never printed, and the server's `crashContexts_` stays empty: `crashFile: null`, `stack: []` in
  all 38 incidents.
- **0.22.0's part held, as far as it went.** 0.0.20/0.21.0: 13 directories a start, 9 read-only,
  7 starts fit and the 8th is refused. 0.22.0: 8 directories, 4 read-only (Windows Kits, the MSVC
  tools, the payload, mcpp's data), 16 starts fit (16 × 4 = 64, the table) and the 17th is refused,
  26 minutes into a session whose 15 exits were all the trap above. The refusal carries 0.22.0's
  sentence, with a wrong count: "this session's 33 engine starts" is `generation_`, which advances
  at a start and at a stop (2n − 1); the session had 17.
- **So #86 was never the grant table first.** The table is what a crash loop runs into; W1–W3 of
  0.22.0 moved the wall from the 8th start to the 17th, and W4 (recycling) was deferred. The loop
  itself — the trap — was not looked at, because no bundle could say where it was.

Three more things the bundles show, each a defect of the server's own:

1. **The exit code is thrown away.** `Connection::exit_code()` returns `by_environment ? -1 : code`
   (`src/lsp/connection.cpp:144`); mcpplibs.os gives the condition's number in `code`. The
   incidents say `exitCode: -1`; the reporter found `0xC000001D` by hand.
2. **The wrong file is set aside.** 01:21:32 on 11-12, 0.22.0: the one file clangd was building
   when it ended was `monitor_view.cppm`; the suspect recorded is `main.cpp`, and at 01:21:47
   `main.cpp` is set aside while `monitor_view.cppm` stays. Suspects are the files of unanswered
   requests and of edits in the last ten seconds (`clangd.cpp:2944`), which in an editor at rest
   is every visible file.
3. **Setting aside does not hold.** In that session `monitor_view.cppm` was set aside five times
   and was back with clangd before each next exit: on an edit (`Quarantine::release`, "the file
   changed"), on "the modules it imports were built again since clangd exited on it" (06:48:53 — built again *by the restart*,
   identical; five seconds later the engine ended on it again), and with each new session. Every
   return cost one engine start, four table slots.

### 0.2 The harness (draft PR #87)

The harness replays the report on a clean machine: the reporter's mcpp (2026.11.10.2) and LLVM
(23.1.3) build and test `systop`; the released payload's server is started as the extension starts
it (`okrun mcppls.okb serve --payload …`, the reporter's initialization options) and
`drive_lsp.py` keeps up an editor's requests on the files the reporter had open; then
`check_matrix.py` runs `clangd --check` on `src/ui/monitor_view.cppm` as a matrix, any run that
ends by an exception again under `cdb`, and `symbolize.py` names the frames (it finds the image's
base by matching the bytes at the instruction pointer against the image's ELF — the image is a
static-pie ELF inside the okb, not stripped — and reads the ELF's own symbol table).

**The trap reproduces, on one of the two runners.**

| | `windows-2025` (Server 2025, build 26100, MSVC 14.51) | `windows-2022` (Server 2022, build 20348, MSVC 14.44) |
|---|---|---|
| `--check` on the project's file, 3 runs | **`0xC000001D` ×3**, in `Building AST...` | exit 3 ×3 (one error, below) |
| one ftxui header, `import std;`, its type used in the purview | **`0xC000001D`** | exit 3 |
| the same, the type not used | exit 3 | exit 3 |
| a one-struct header beside the file (guarded, unguarded, `#pragma once`), used | exit 3 | exit 3 |
| the same header included as `"../ui/<name>"` (another spelling of its path), used | exit 3 | exit 3 |
| the trapping variant with `.clangd`: `UnusedIncludes: None`, `MissingIncludes: None` | **exit 3 — no trap** | exit 3 |
| … the same fragment as clangd's *user* configuration, its directory named by `XDG_CONFIG_HOME` or by `HOME` | **exit 3 — no trap** | exit 3 |
| … named by `LOCALAPPDATA` | `0xC000001D` (not read) | exit 3 |
| … without `--experimental-modules-support` | exit 3 — no trap | exit 3 |
| … `--clang-tidy=0`, `--enable-config=0`, `--pch-storage=memory`/`disk`, in-process module builds, `OKRUN_SNAPSHOT=0`, `OKRUN_PLACE=0`, a fresh module cache, the server's own arguments | `0xC000001D`, each | exit 3 |
| `src/main.cpp`, `tests/test_monitor_view.cpp`, `src/systop.cppm`, `src/core/refresh_clock.cppm` | exit 0 | exit 0 |
| `src/core/sampler.cppm` (Windows SDK headers in its global module fragment) | exit 3 | exit 3 |
| clangd's own line about its module cache | "…the owned cache is not used here: **an open file is not the node its name refers to**" | "owned, 1024 MiB of payloads" |
| the replayed session (7 minutes) | 7 engine exits, each recorded `-1`; 9 files set aside | 0 exits |

(Exit 3 is `--check`'s "one error": `module_decl_not_at_start`, which `--check` reports for a
module unit with a global module fragment on the skipped-preamble path and the LSP path does not;
it is a defect of `--check` alone, noted for the engine's list, and not this issue.)

The reporter's own logs carry the same line as the trapping runner — "an open file is not the node
its name refers to", 155 times in the last bundle — and the reporter's Windows is build 26200. The
split is the Windows build, not the MSVC version (the reporter and the runner that does *not* trap
share 14.44.35207), not the processor (AMD EPYC and Intel Xeon here), and not the machine.

**The trap, named** (windows-2025, the project's own file; `matrix/original.stack.txt`):

```
exception c000001d  rip image+0x188e505   bytes at rip: 0f 0b (ud2)
TRAP  function_ref<…>::callback_fn<clang::clangd::computeIncludeCleanerFindings(ParsedAST&, bool)::$_0>+0x665
      function_ref<…>::callback_fn<clang::include_cleaner::walkUsed(…)::$_0>
      RecursiveASTVisitor<include_cleaner::ASTWalker>::TraverseTypedefTypeLoc
      RecursiveASTVisitor<include_cleaner::ASTWalker>::TraverseFunctionProtoTypeLoc
      RecursiveASTVisitor<include_cleaner::ASTWalker>::TraverseFunctionHelper
      RecursiveASTVisitor<include_cleaner::ASTWalker>::TraverseDecl
      clang::include_cleaner::walkUsed
      clang::clangd::computeIncludeCleanerFindings
      clang::clangd::ParsedAST::build
      clang::clangd::check → clangdMain → ok_main
```

and the instructions before it, in the image (`llvm-objdump` at `image+0x188df50`):

```
callq  include_cleaner::Includes::match(Header) const
…
callq  clang::clangd::IncludeStructure::getID(const FileEntry*) const
movq   %rax, %r14
shrq   $0x20, %rax          ; the optional's "has a value"
je     188e505              ; → ud2
```

**The session shows the server's three defects on a clean machine.** In six minutes the server
set aside, among others, `refresh_clock.cppm` ("clangd was idle" — a unit with no `#include` at
all) and `systop.cppm` ("clangd was file is queued" — eight lines of imports), neither of which
can reach the trap, and handed `monitor_view.cppm` back
three times ("the modules it imports were built again since clangd exited on it"), each return an
exit. Every exit is `-1` in the log and the incidents. Windows Error Reporting, asked for a dump of
every `okrun.exe` that ends by an exception, wrote none: nothing on the machine records the trap
unless a debugger is attached.

**The grant table reproduces without any crash, to the slot.** Three sessions on the two runners
(three read-only grants a start: Windows Kits, the MSVC tools, the runner's temporary directory)
were restarted in a loop with `mcppls.restartEngine`: in each, 21 starts fit and the 22nd was
refused, `granting …/VC/Tools/MSVC/<version>: out of memory … (3 read-only grants each)` — 21 × 3 =
63, the 22nd start's first grant took the 64th slot and its second was refused; the reporter's is
16 × 4 = 64. The wall is at start `floor(64 / r) + 1` on any Windows machine, crash or no crash.
Two details of the refusal: its sentence says "43 engine starts" (22 were made), and the server's
log has no line for it at all — the engine logs `is given 8 directories` and nothing after; the
sentence exists only in the status issue and the bundle. Every later restart the person asks for
logs `is given 8 directories` again and fails as silently.

**What CI could not have seen.** Every Windows job of `ci.yml` runs on `windows-2022`; the defect
needs a 26100-or-later build. No Windows 11 24H2 user's first module unit with a third-party
header in its global module fragment was ever exercised before a release.

## 1. E1 — the trap

### 1.1 The cause

Three facts, each established by §0.2, in a chain:

1. **The same file has two identities in one clangd process.** On Windows builds 26100 and 26200
   the ok image's runtime reports another node identity (device and file number — what LLVM's
   `UniqueID` is made of) for a file that was *opened* than for the same file asked about *by
   name*; on build 20348 they agree. The engine's own probe says so at every start (patch 0039,
   UP-03: "an open file is not the node its name refers to"), and for its module cache alone it
   already steps aside. Where exactly the two numbers come apart in openkal's Windows
   implementation is not established here (§1.4).
2. **clangd's include analysis identifies a header both ways.** `IncludeStructure` gives a header
   its ID by the `UniqueID` of the file the preprocessor entered (`getOrCreateID`, `Headers.cpp`).
   `computeIncludeCleanerFindings` later takes each include of the main file, gets a `FileEntry`
   for its *canonical resolved path* (`convertIncludes`: `FileManager::getFileRef(Inc.Resolved)`),
   and asks for that entry's ID (`IncludeStructure::getID`, by `UniqueID`). When the second lookup
   yields another identity than the first — which (1) makes possible — `getID` answers
   `std::nullopt`. The disassembly of §0.2 is this call and the test of its result.
3. **Upstream dereferences that optional behind an `assert` alone** (`IncludeCleaner.cpp:411–415`
   and `:434–438`: `assert(HeaderID.has_value() && "ConvertedIncludes only contains resolved
   includes."); Used.insert(*HeaderID);`). A release build has no assert; an unhardened one reads
   an indeterminate ID (wrong, silent); the image's hardened libc++ checks `optional::operator*`
   and traps. The trap is the hardening doing its work.

Why these files, as far as the matrix says — and where it stops saying:

- The callback runs only for a reference in the main file to a symbol that a main-file include
  provides: a purview *use* of a type from the header. `using namespace ftxui;` alone, an unused
  include, a `std::` type, a type of an imported module do not reach it (the used/not-used rows;
  the reporter's reduction says the same).
- The header must have been *entered in this parse*: the skipped-preamble path of a file that
  imports modules (UP-23), where the global module fragment's headers are parsed with the main
  file. Without `--experimental-modules-support`, or with no import, there is a textual preamble
  and no trap; `main.cpp` in `--check` has one too (its includes precede its imports) and ends 0.
  In a session `main.cpp` can be on the skipped path as well (the reporter's logs show it with a
  258 KB preamble), which is how exits came to be attributed to it.
- **Which headers get two identities is not established.** On the trapping runner the ftxui header
  (in mcpp's store, `D:\a\_temp\…\xpkgs\compat-x-ftxui\…`, found through `-I`) does; a
  one-struct header beside the source (`D:\a\<repo>\…`) does not, by its own name or through
  `../ui/` (so "a path that is not the canonical one" is not the condition — the fourth run was
  made to test exactly that, and refuted it); `sampler.cppm`'s Windows SDK headers do not. The
  engine's probe, which reports the split, runs in the server's cache (`D:\a\_temp\…`). The
  reporter's reduction found a one-struct header that traps, on their paths. What the affected
  locations have in common is the platform's question (§1.4); the engine's fix (§1.2) does not
  depend on the answer, and the server's stopgap (§1.3) keys on the engine's own probe line, which
  both trapping machines print and the non-trapping one does not.

Who is affected: any Windows 11 24H2-or-later user whose module unit includes a library header in
its global module fragment, imports a module, and uses a type from that header — the ordinary
shape of an mcpp module unit over a C or C++ library. The engine ends at the first build of such a
file, every time.

### 1.2 The fix, in the engine

At both sites the optional is checked; an include whose entry has no ID is not marked used by ID:

```cpp
for (auto *Inc : ConvertedIncludes.match(H)) {
  Satisfied = true;
  if (auto HeaderID = AST.getIncludeStructure().getID(&Inc->Resolved->getFileEntry()))
    Used.insert(*HeaderID);
  else
    UsedUnidentified = true;   // the reference is satisfied; which include it was is unknown
}
```

and, so that the unchecked path's *wrong answer* does not replace the trap, an include structure in
which some include could not be identified reports no unused includes for this build
(`getUnused` is skipped when `UsedUnidentified`): a missing "unused include" hint is the honest
result when the analysis cannot tell headers apart; a false "unused" on a header in use would be a
diagnostic that offers to delete a needed line. Missing-include findings are unaffected (they are
computed from providers, not IDs).

One patch in mcppls-clangd's series (`UP-41`), written to go upstream as it is: the assert states an
invariant a file system can break (two paths, two identities — a network share, a substituted
drive, a case-insensitive volume do it without openkal), and a release build must not have
undefined behavior there. Engine version `23.1.0-mcppls.11`.

Rejected: resolving the include's entry by the path it was entered by (it repairs this site and
leaves every other by-name lookup of the analysis as it is); turning the image's hardening off (it
found a real defect on its first outing; §2 makes its report readable instead).

### 1.3 Until the engine ships: the server keeps the analysis off where it traps

A workaround the matrix proves (the `.clangd` row of §0.2): a `.clangd` beside the project with

```yaml
Diagnostics:
  UnusedIncludes: None
  MissingIncludes: None
```

ends the trap; the reporter can use it today, and it goes into the issue and
`docs/50-troubleshooting.md` now. For everyone who has not read the issue, the server does the
same thing itself, as a workaround of the engine's version (`WA-CLANGD-015`, in
`src/engine/clangd/workarounds.cppm`; off for an engine whose identity lists the fix's feature):

- **How.** The image determines no user configuration directory on Windows ("Couldn't determine
  user config file, not loading", in every log); it reads one where `XDG_CONFIG_HOME` or `HOME`
  points (the user-configuration rows of §0.2 — verified: the trap stops; `LOCALAPPDATA` is not read). The server
  starts the engine with `XDG_CONFIG_HOME=<context>/engine-config` and writes
  `<context>/engine-config/clangd/config.yaml` with the fragment above. Nothing in the project is
  touched. In clangd's documented order a user fragment takes precedence over a project's
  `.clangd`, so a project that sets `UnusedIncludes: Strict` does not bring the trap back; the
  fixture of §6 asserts that order rather than trusting it.
- **When.** Only where the defect can occur: Windows, an engine without the fix, and the engine's
  start-up line saying an open file is not the node its name refers to. The line is known only
  after the first start, so the first session on such a machine writes the fragment and restarts
  the engine once (one start of the budget, §3.4); the fact is kept in the workspace cache with
  the engine's sha256, and later sessions start with the fragment in place. A simpler rule —
  always on Windows until `mcppls.11` — costs include hints on machines that are not affected
  (Windows 10, Server 2022); the plan takes the probe's rule and keeps the simpler one as the
  fallback if the restart proves awkward in review.
- **What the person sees.** One log line and one notice in `mcppls report`: "include hints
  (unused/missing) are off on this system: clangd 23.1.0-mcppls.10 ends on them here (#86); they
  return with the next engine". No status issue: nothing is broken that the person can act on.
- **What it does not do.** A person whose own `HOME/.config/clangd/config.yaml` exists is not
  reading it through the image today either (no `HOME` on Windows), so nothing is taken away; on
  Linux and macOS the workaround is never on.

It costs the unused-include hint on affected machines for one release and is removed with the pin
of `mcppls.11`. This is a server patch release (`server 0.22.1`), independent of everything else
in this plan, and the first thing to ship.

### 1.4 P1 — the identity itself

The trap is one consequence of (1); clang uses a file's identity wherever it must know that two
paths are one file — `FileManager`'s table of entries, `#pragma once` and include guards'
"already entered", module input validation, the preamble's stat cache. A header reached by two
spellings is, to all of them, two files on an affected machine. The engine's patch 0039 already
treats the condition as a property of the system to step around; it is a defect of the platform to
fix:

- **Platform (openkal-windows).** One identity per file, the same from an open handle and from a
  name, on every Windows build — in the runtime's `stat`/`fstat` family. The harness gives the
  test: the image's probe must print "owned" on a `windows-2025` runner. What differs between the
  two builds (a by-name query that exists only on 24H2-era builds is the first place to look) is
  for the platform's own analysis; this plan does not guess it into a design.
- **Engine CI.** `mcppls-clangd`'s Windows rows, and this repository's `conformance`,
  `vscode-e2e` and `clean-machine` Windows rows, run on `windows-2025` as well as `windows-2022`
  until the platform fix is pinned, then on `windows-2025`. The image's probe line is asserted
  ("owned") once it is.
- **Server.** `mcppls report` and the bundle say which it is (`engine.fileIdentity: "consistent" |
  "split"`, from the engine's start-up line), so a report from an affected machine says so in its
  first screen.

## 2. E2 — a trap that says where it was

A native clangd that crashes prints its crash context and a stack; the server reads both
(`crash-context` events, K-3) and the incident carries them. The ok image prints nothing, Windows
Error Reporting writes nothing for it (§0.2), and from 2026-11-10 to 2026-11-12 four bundles of 38
exits could say only *when*. The function was found in one CI run once a debugger was attached and
the frames were named — work the runner can do itself. Three layers, each sufficient
for the next report to be better than this one:

1. **The runner reports a trap** (openkal-image / okrun, every system). When the image's code ends
   by a condition the runner can see — an exception on Windows (a vectored handler, first in line),
   a signal on Linux and macOS — it writes one block to stderr before the process ends as it would
   have:

   ```
   okrun: trap: illegal instruction (0xC000001D) at image+0x1dc42cf
   okrun: build-id ed8cd464126f670098a4b710c8908a5579c0a5ac x86_64
   okrun: frames: image+0x1dc42cf image+0xa93c67 image+0x2b10f4 …   (frame-pointer chain, at most 64)
   ```

   The image is built with frame pointers, so the chain is a walk of `rbp`, bounded and
   async-signal-safe: no allocation, no symbol lookup, offsets relative to the image's base, which
   the runner knows. The harness of PR #87 proves the method from outside (cdb's registers,
   `symbolize.py` finds the base from the bytes at the instruction pointer and names frames from
   the image's own symbol table); the runner doing it from inside needs no debugger on the
   reporter's machine. Rejected: unwinding with `.eh_frame` in the handler (too much code to run in
   a process that just trapped), and symbolizing in the runner (the symbol table is 2.8 MB the
   runner would have to map; the server or a developer does it offline).
2. **The server keeps it.** The engine's stderr reader recognizes the `okrun: trap:` block as it
   recognizes LLVM's stack dump today: `lastExit_.exception` becomes the condition
   (`illegal instruction (0xC000001D)`), `lastExit_.stack` the offsets, and the incident and the
   bundle carry them with the image's build-id. A developer names the frames with the image of that
   build-id (`symbolize.py`, promoted from the harness to `tools/`); the report is actionable
   without the reporter's project.
3. **A failed hardening check says which.** With libc++'s hardening in a mode that reports before
   it ends the program (an assertion handler that writes `file:line: assertion … failed` and then
   traps), the block above is preceded by the check that failed. Its cost is code size in the
   image; to be measured on the engine's side before it is taken (the image's cold start is a
   budgeted number there). Layer 1 does not wait for it.

clangd's own notion of "which file" (its `ThreadCrashReporter`) runs in a signal handler the image
does not have; §3.2 gives the server that knowledge from what clangd already logs, so layer 1 does
not need to call back into the image.

## 3. S — the server

### 3.1 S1 — the exit keeps its number

`Connection::exit_code()` returns the platform's `exit_status` whole (`code`, `by_environment`)
instead of folding it to `-1`. From it the engine derives, once, an `ExitCondition { number, kind,
name }`: on Windows an NTSTATUS (`0xC000001D`, "illegal instruction"; `0xC0000005`, "access
violation"; `0xC00000FD`, "stack overflow"; `0xC0000409`, "stack buffer overrun"), elsewhere a
signal (`SIGILL`, `SIGSEGV`, `SIGABRT`, `SIGKILL`). A table of a dozen names in
`src/engine/clangd/`, unit-tested; an unknown number is printed as it is.

- Log: `clangd ended by an illegal instruction (0xC000001D) while building src/ui/monitor_view.cppm`
  — not `clangd exited with -1`.
- Incident and `engine-exit` event: `exitCode` (the number, unsigned on Windows), `exitCondition`
  (the name), `endedByEnvironment` (bool). `exitCode: -1` is no longer written.
- Issue text (`engine-crashed`, `engine-crash-loop`): the condition and the file, e.g. "clangd ended
  by an illegal instruction while building monitor_view.cppm; that file is answered by mcppls's own
  engine meanwhile".
- `SIGKILL` / a console interrupt stay what they are today (W5: a killed clangd keeps the file):
  the condition's kind says "ended from outside", and such an exit names no suspect.

### 3.2 S2 — the suspect is the file being built

The server already reads clangd's `ASTWorker building file <path> version <n>` lines (the
`ast-build` events, `clangd.cpp:2035`) and its file-status notifications. From them it keeps, per
file, whether a build is in flight: begun by `ast-build`, ended by the file's next diagnostics or
an idle file status. At an exit:

- **Named**: clangd printed a crash context (native engines) or the runner's trap block is
  attributed (§2) — as today, that file alone.
- **In flight**: exactly the files whose build was in flight. One file: it is set aside at once —
  the second-exit strike (W5, #59 U9) exists because a guess deserves a second chance, and this is
  not a guess. Several (the engine runs builds in parallel): each gets a strike, and the ledger of
  §3.3 separates them by the next exit, since only the guilty one is in flight both times.
- **Neither** (no build in flight, an exit while idle or during indexing): today's rule — files of
  unanswered requests and recent edits, a strike each.

Measured against the bundles: the in-flight rule names `monitor_view.cppm` in the 31 exits where it
was building, and never `main.cpp` alone at 06:29:16. An exit ended from outside (§3.1) names
nobody under any rule.

Background builds count too: a unit the server opens to index its definitions
(`primeImplementationUnits`) is in flight like any other, and one that ends the engine is kept
from priming by the same ledger (§3.3) instead of being opened again by the next start.

### 3.3 S3 — set aside means set aside: the crash ledger

One record per file that ended the engine, in the workspace's cache
(`workspaces/<key>/crash-ledger.json`), keyed by the file's path key and holding:

```
{ "file": "…/src/ui/monitor_view.cppm",
  "engine": "47a09679…",              // the image's sha256: another engine starts clean
  "condition": "illegal instruction (0xC000001D)",
  "exits": 5, "firstAt": "…", "lastAt": "…",
  "term": 16, "retryAt": "…",          // minutes, doubled each time: 2, 4, 8, 16, then 60, then 24 h
  "contentAtLastExit": "<sha256 of the text clangd had>",
  "closureAtLastExit": "<identity of the BMIs it imported: their input hashes, not build counts>" }
```

Rules, replacing the three ways back of §0.1:

- **One retry per term, whatever the reason.** An edit, a rebuilt closure, a new session, a
  changed command: each makes the file *eligible*; it goes back to clangd only when `retryAt` has
  passed, and an exit on it doubles the term. A person typing in a file that traps clangd costs
  one engine start per term, not one per keystroke burst. Until then the file is answered by
  mcppls's own engine, as a file set aside is today.
- **Unchanged means not eligible.** Same content, same closure, same engine: nothing has changed
  that could change the outcome, and the term's end alone does not send it back after the second
  exit (`exits >= 2`): it waits for a change or for the person.
- **The closure is what it was built from, not how often.** `closure_identity_` counts module
  builds (`moduleBuilds_`), so a restart that rebuilds the same BMIs looks like a change. The
  ledger's identity is the modules' input identity (the source hash and command the engine already
  reports for a rebuild, UP-32/UP-35), which a restart does not move.
- **A new session reads the ledger.** A file with a live record is not handed to clangd at open;
  the status says so once ("monitor_view.cppm made clangd end 5 times (illegal instruction); it is
  answered by mcppls's engine. Try again"). Today every editor restart starts the loop over — the
  third bundle holds six sessions between 01:20 and 01:53 on 11-12, each ended by the same exits.
- **The person decides otherwise in one click.** The quarantine issue's action (`Try again`) clears
  the record's term for that file; `mcppls.restartEngine` does not (a restart is not a claim that
  the file is fine). A record is dropped when the engine's sha256 changes, when the file is deleted,
  or after 30 days.

The existing `Quarantine` keeps the stall cases (timeouts, a build that will not finish) as they
are; the ledger is the crash case's own state, and `set_aside_` consults it. `crashRetried_` and
`aside.crashClosure` go.

### 3.4 S4 — the start budget

The engine knows what a start costs and what is left, on every system whose sandbox counts
(§4 makes the number large; until every pinned platform has it, and for whatever fixed table comes
next, the server does not walk off the edge):

- `startsMade_` (a counter of its own — the refusal's sentence says "17 engine starts", not
  `generation_`) and `slotsSpent_` (read-only grants summed over starts); the capacity is the
  platform's (`os::process::grant_capacity()` when the pin has it, else the measured 64).
- **A reserve.** While fewer than `RESERVE = 3` starts' worth of slots is left, *automatic*
  restarts — a crash recovery, a crash-loop retry, a plan that names an ungranted directory — are
  not made. The engine goes to a state of its own, `paused`: mcppls's engine answers, the status
  says "clangd is paused: it ended N times this session and M starts are left before the server
  must be restarted", and the reserve belongs to the person (`Restart clangd`).
- **The crash loop ends at its cause.** With §3.2 and §3.3 a file that traps clangd is out after
  one or two starts, so a loop of fifteen is not expected again; the budget is the backstop for the
  loop nobody attributed (an exit while idle, a trap in the background index).
- **The refused start offers the way out.** `engine-start-failed` from a spent table gets the
  action `mcppls.restartServer` (it has `mcppls.showLogs` today), and its sentence says "Restart
  Server restores it" — the table is the server process's, and the extension's restart of the
  server takes a second, where "restarting the editor" loses the person's windows. No automatic
  server restart (0.22.0's §3.3 stands: a server that restarts itself is harder to trust than a
  sentence that says what to press).
- A plan change that needs a new grant mid-session (`uncovered`) prefers, when the budget is in
  its reserve, to keep the running engine and report the directory as ungranted (the file that
  needs it degrades) over spending a start on it.

## 4. W4 — the table

0.22.0 left it to the platform; the reproduction on a clean runner (§0.2) says it is not specific
to the reporter's machine, so every Windows user who restarts the engine twenty-odd times in a
session — a toolchain switch, a branch change, a long day — meets it without any crash. Two changes
in openkal's rights table, either sufficient, both wanted:

1. **One slot per distinct grant.** `attenuate()` looks up a live root with the same directory and
   rights before taking a slot, and counts references. The engine's grant set is the same eight
   directories at every start; the table then holds four entries for the life of the server, and
   a plan change adds the one directory it names. No caller changes.
2. **`forget()` returns the slot** when the last handle closes (0.22.0 §3.4's reading of
   `rights.cpp:395`: `names`/`listed`/`base` are released there, `live` is not cleared).

mcppls takes it as a pin bump (openkal-platform, mcpplibs.os), with the `engine-restarts` fixture
raised past the old capacity as the proof (§6). Until the pin moves, §3.4 holds the line. macOS
gets the same fix and loses its exemption from the fixture.

## 5. D — what a report carries

- **The reproducer, in the bundle.** `engine/root-<n>/reproduce.json`: the runner, the image and
  the arguments of `clangd --check=<the file of the last exit>` against the bundle's engine
  database, as a command line for the reporter's shell — what the reporter of #86 worked out by
  hand. `mcppls report --reproduce <file>` runs it and prints how it ended, with §3.1's name.
- **The exit, named** (§3.1), and **the trap's block** (§2) in `incident.json`.
- **Counts that are counts.** The refusal's "N engine starts" (§3.4).
- **A refused start is in the log.** `clangd could not be started: …` is written as a warning when
  it happens, and once more for each later attempt — today it reaches only the status issue and
  the bundle, and the log's last word is `is given 8 directories` (§0.2).
- **The release payload is not `dirty`.** Every bundle says `"dirty": true` for the payload's
  build, the released ones included; the release job packs from a tree its own steps have written
  into. The stamp is taken before those steps (or ignores the files they write), so `dirty` means
  what a reader takes it to mean.
- **The kit line names what it is for.** `semantic kit mcppls-kit-openkal-0.24.4 … for
  x86_64-w64-mingw32, standing in for mcppls-kit-x86_64-w64-mingw32` is written before the model
  is known and names the host's default; the reporter copied it into the report as the project's
  target. The line is written when the model's target is known, or says "for this host's default
  target, until the project's is known".

## 6. T — tests and CI

- **Engine (mcppls-clangd).** A unit test of the include findings over a file system that gives a
  header one identity when opened and another by name (the test VFS can; no Windows needed): no
  crash, no "unused" finding for the header in use, the missing-include findings unchanged. An e2e
  case shaped like `ftxui-used` (a module unit, a header under `-I` in its global module fragment,
  an import, a purview use) run with `--check` on `windows-2025`: it ends by no exception.
  `--check`'s own `module_decl_not_at_start` on the skipped-preamble path gets its case too.
- **Unit (server).** `ExitCondition` from an `exit_status` (NTSTATUS and signal tables, unknown
  numbers); the in-flight set from a sequence of `ast-build` and diagnostics events, and the
  suspects each exit yields (one in flight, several, none, ended from outside); the ledger's rules
  (one retry per term across an edit, a rebuilt closure and a reload; unchanged is not eligible; a
  new engine sha256 clears; `Try again` clears); the start budget (reserve, paused, a person's
  restart inside the reserve, the refusal's sentence and action).
- **Conformance.** `engine-trap` (every system): a file whose opening makes the engine end —
  the conformance client ends the engine's process while that file's build is in flight (its
  `kill-clangd` action, given a condition to end it by and the `ast-build` event to wait for) — asserting: the file is set aside after the
  *first* exit, the other files are answered by clangd again, the incident names the condition,
  a second session does not hand the file to clangd, and the session's starts stay under three.
  `engine-restarts`: raised from 8 restarts to 40 once W4's pin is in (past the old capacity on
  every system, macOS included); until then it asserts `paused` with the reserve intact instead of
  a refused start.
- **The stopgap (`server 0.22.1`).** On `windows-2025`: the systop-shaped fixture below, opened
  through the server with engine `mcppls.10` — the engine does not end, the log carries the
  workaround's line, `<context>/engine-config/clangd/config.yaml` exists, and a project `.clangd`
  that turns the analysis on does not change the outcome. On `windows-2022` and Linux: the
  workaround is off and unused-include hints are still published.
- **Windows runners.** `conformance`, `vscode-e2e`, `clean-machine` and `bench` get a
  `windows-2025` row beside `windows-2022` (§1.4): the eleven Windows rows of `ci.yml` are all
  build 20348 today.
- **Windows module corpus (CI, win32-x64, both runners).** A fixture shaped like the reporter's unit — a module
  interface unit with a global module fragment that includes a third-party header, `import std;`
  with the MSVC STL, a purview use of the header's types — under the real MSVC toolchain of the
  runner (`mcpp-target-windows-real` already has one). The harness of PR #87 becomes its seed:
  `drive_lsp.py`'s session and `check_matrix.py`'s rows are what the fixture asserts.
- **The harness itself** stays out of main: its scripts move to `tools/` only as far as §2 and §5
  use them (`symbolize.py`).

## 7. Order, and what each step buys

0. **Today**: the `.clangd` workaround in the issue (§1.3) — the reporter's editor works again.
1. **`server 0.22.1`** — WA-CLANGD-015 (§1.3): every affected Windows 11 machine stops trapping
   without anyone reading an issue. Small, self-contained, testable on `windows-2025`.
2. **E1** (engine, `mcppls.11`) — the trap is gone for good and include hints come back.
   **S1 + S2** (server, small) ride the same series release — the next crash report names the
   condition and the file; one wrongly-set-aside file fewer per exit.
3. **S3 + S4** (server) — any future file that ends the engine costs one or two starts and never
   the session.
4. **W4 + P1** (platform pin) — the table stops being a limit anyone meets, and a file is one file.
5. **E2** (platform + server) — the report after next names the function.
6. **D, T** with the steps they belong to — the `windows-2025` rows first, with step 1; the PR #87
   branch and the `issue86-evidence` pre-release are deleted when E1 is fixed.

## 8. Not in this plan

- No change to what a start grants (0.22.0's W1/W2 stand).
- No automatic restart of the server.
- No general "crash budget" setting: the ledger's terms and the reserve are constants until a
  report says otherwise.
