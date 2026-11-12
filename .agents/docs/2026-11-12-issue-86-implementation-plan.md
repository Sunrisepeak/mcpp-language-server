# Issue 86: implementation plan and task dependencies

Implements `2026-11-12-issue-86-engine-trap-and-start-budget-design.md` (the design). One pull
request per repository, each by that repository's own process; this file is the split, the order
and what moved against the design while implementing.

## 1. Repositories and pull requests

| | Repository | Pull request | Carries | Released as |
|---|---|---|---|---|
| A | `Sunrisepeak/mcppls-clangd` | #22 `mcppls-clangd-0.0.22` | E1, the engine's part of T | `v0.0.22`, engine `23.1.0-mcppls.11` |
| B | `mcpplibs/openkal-windows` | #47 `fix/one-identity-and-returned-grants` | P1, W4 (the platform's code) | `0.22.0`; reaches mcppls with a later platform pin |
| C | `Sunrisepeak/mcpp-language-server` | `fix/issue-86-engine-exits` | S1–S4, D, the server's part of T, the pin of A | `0.23.0` |

## 2. What was found while implementing, and what it changes

1. **P1's cause is established** (the design left it open, §1.4). openkal-windows writes the
   volume's serial number into a node's identity from two places: by handle
   `FileFsVolumeInformation` (32 bits), by name — where the system has Windows 11 24H2's
   `FileStatBasicInformation`, which builds 26100 and later do — that record's
   `VolumeSerialNumber` (64 bits). Every file on such a system has two identities. Which headers
   trapped (§1.1's open question) follows: those whose resolved path is another *string* than the
   path they were entered by (a header under `-I` with mixed separators or `..`), since clang's
   `FileManager` answers a path it has seen from its own table and asks the system only for a new
   spelling. B corrects it in `info_by_name`.
2. **E1 is fixed more precisely than §1.2 proposed.** An include is marked used by the ID it was
   entered with — the `HeaderID` the include itself carries — not by the reverse lookup through its
   resolved entry; "report no unused include" remains only for an include with neither. No hint is
   lost on an affected machine.
3. **WA-CLANGD-015 (§1.3, the stopgap) is not built.** It exists for the time between a server
   release and the engine's; A and C ship together, C pinning A's release, so the stopgap would
   never run. The `.clangd` fragment stays documented in `docs/50-troubleshooting.md` for anyone on
   0.22.0 or earlier.
4. **E2 (§2, the runner's trap block) is not in this round.** It is okrun's code (openkal-image)
   and a new platform release train; S1 and S2 already give a report the condition's name and the
   file. Follow-up, with the platform pin below.
5. **The platform pin does not move in C.** mcppls builds over openkal-platform 0.24.4; B's
   correction reaches a payload through openkal-image (okrun), a platform release and the index —
   a train that is at 0.27.0 and moving on its own schedule. Until C's pin takes a platform with
   openkal-windows ≥ 0.22.0: E1 makes the two identities harmless to the include analysis, and S4
   holds the line on the grant table (§3.4, as the design says). `engine-restarts` therefore
   asserts `paused` with the reserve intact, not 40 restarts (§6).

6. **"Try again" is Restart clangd.** §3.3 gives the quarantine issue an action of its own that
   clears a file's term, and says `mcppls.restartEngine` does not. A new command is three editors'
   work and a protocol addition; the status issue of a file that ended clangd (`file-ended-engine`)
   carries **Restart clangd**, and the person's restart asks the ledger to try every such file
   again -- except inside the start reserve, where a file that ends clangd again would spend what
   is left: there they stay with mcppls's engine and clangd comes back for everything else.
7. **The runner already reports a fault on Linux and macOS** (found running the trap fixture):
   okrun prints `openkal: fault: an instruction the program may not execute (SIGILL) at …` and a
   line a frame (`clangd.okb+0x5b3fb2f`) before the process ends. The server now reads that block
   into `lastExit.exception` and `lastExit.stack` -- layer 2 of §2, done; layer 1 is missing on
   Windows only, which is the system the reports came from.
8. **A server defect the fixture found.** `Workspace::serve_kept` detached the core engine's
   request before finishing the job; a clangd between an exit and its restart had not sent the
   request yet and answered the detachment at once, as cancelled, which finished the job under the
   function: the editor was told `cancelled` instead of the kept answer and the job was written to
   after it was erased (SIGSEGV under the debugger, first run). The job is finished first.
9. **`clangd-crash-closure` changes with the rule it tested.** 0.0.17's W5 sent a file back the
   moment its modules were rebuilt; §3.3 replaces that ("one retry per term, whatever the reason").
   The fixture now asserts the file is still aside after the rebuild and back when the term ends.
10. **The grant table is every system's.** openkal-linux and openkal-macos carry the same 64-root
    table and the same `forget()`; B corrects Windows alone. The budget (S4) therefore counts on
    every system with a sandboxed engine. The same correction for the other two is follow-up.
11. **`windows-2025` rows: conformance only.** §6 names four jobs; this round adds the conformance
    row (every `on:win32-x64` case on build 26100) and the engine's own gate with its control. The
    `vscode-e2e`, `clean-machine` and `bench` rows follow once that row has a history.
12. **D, as built.** The reproducer is `lastExit.reproduce` (report, incident, bundle), not a
    separate `reproduce.json`, and there is no `mcppls report --reproduce` yet. The payload's
    `dirty` ignores `mcpp.lock` (what the build tool rewrites for its host) and names what does
    differ. The kit line says when its target is the host's default.

13. **The budget counts a workspace's own starts.** The table belongs to the server process, and a
    server with several workspace roots has an engine for each; each counts what it started. With
    one root -- every report so far -- the count is the table's. Where it is not, a refused start
    still says what it is (`engine-start-failed`, with Restart Server). One count for the process
    is follow-up, and unnecessary once the platform returns its slots.
14. **`windows-2025` is another toolchain too.** The image has CMake 4.4.5 and MSVC 14.51 where
    `windows-2022` has 3.31.6 and Visual Studio 2022's MSVC. `cmake-msvc-std`'s own project is written for 3.31.6's
    experimental `import std`: on the new row its `prepare` failed twice over -- the gate is another
    UUID, and with the right one CMake 4.4.5 builds the standard module's library and leaves it
    off the program's link line (LNK2001). No server runs before that. The row leaves that one
    case out by a tag (74 of 75 cases run there); writing the project for both CMakes is
    follow-up.

15. **The candidate was named and not tested.** Since mcppls-clangd 0.0.21 the engine's run has one
    artifact; `download-artifact` puts one match straight into its path, the payload job looked
    for it a directory deeper, found no part and assembled the payload from the lock's engine
    under `--candidate`, saying nothing. Three runs of this pull request were green on
    `23.1.0-mcppls.10`. It showed in the status a failing case printed (`"features": 15`). The
    job now finds the part wherever it is, prints the engine of every platform, and stops when a
    named candidate is not what the payload carries; `mcppls-devtools payload --candidate` refuses
    to run without an engine. A7 waits for a run that is green on the candidate in fact.
16. **One case is not steady on the new row.** `mcpp-polling-watch-inputs@polling` failed once in
    three runs on `windows-2025` (a deleted external input not seen within its 30 s) and passed
    when run again at once. It is this repository's case and polling's timing, not this change;
    a failed job is run again (attempt 2 runs the failed cases), as on every row.

17. **macOS has the fewest starts.** There the engine is given the system's conventional roots
    besides the project's own: 14 directories in CI's sessions, of which nine to eleven are
    read-only (counted from the grant rules, not read from a session). The table then holds five
    to seven starts and the reserve of three leaves two to four made automatically, after which
    a restart is withheld (`engine-restart-withheld`) and the person has three. That is §3.4 as
    written, and tight; `engine-restarts` does not run on macOS for the same table. Narrowing
    macOS's grants as 0.22.0 narrowed Windows' and Linux's, or the platform returning its slots,
    is what widens it.
18. **A's comparison run (A6).** Linux ARM64 and Linux x64 pass the warm-prime gate (x64 on its
    repetition), Windows records its ratios, and darwin-arm64 is invalid three times running, as
    it was for 0.0.21. The engine's policy admits a release then on its comparison with the one
    before: the two images alternated in eight pairs on one runner of each system (a temporary
    workflow, run 43245970422) -- on macOS 0.0.22's cold readiness and processor time are below
    0.0.21's beside it, on Linux x64 the two are within 2% in every gated quantity. The engine's
    changelog states it.
19. **The engine's owned cache, once.** One cold session of the image on darwin-arm64 (1 of 24 in
    the comparison run) did not become ready: a module's lock anchor could not be opened while
    three files' prerequisites were built at once. The code is 0.0.20's; recorded as known in the
    engine's changelog, and follow-up there.

## 3. Tasks

### A — mcppls-clangd (engine)

| Task | What | Depends on |
|---|---|---|
| A1 | patch 0048 (UP-41): mark by entered ID; `mayConsiderUnused` checks its lookup | — |
| A2 | unit test `IncludeCleaner.HeaderWithAnotherIdentityByName` (a VFS with two identities) | A1 |
| A3 | e2e `gmf_include_analysis.py` in `e2e_suite.py` and `build-test` | A1 |
| A4 | `openkal.yml` job on `windows-2025`: the case as a gate, 0.0.21's image as its control | A3 |
| A5 | ledger row, `PATCHES.md` section, changelog, `RELEASE_VERSION` 0.0.22, `-mcppls.11`, feature `include-ids-by-entry` | A1 |
| A6 | comparison run (`build-test` dispatch, `all`); the row becomes `steady`, the changelog names the run | A1–A5 green |
| A7 | squash-merge, tag `v0.0.22`, release workflow | A6, **C's CI green on A's candidate** |

### B — openkal-windows (platform)

| Task | What | Depends on |
|---|---|---|
| B1 | tests `one_identity_for_a_node`, `attenuation_returns_its_place`; a `windows-2025` row | — |
| B2 | negative control: B1's run fails where it should (first test on 26100 only, second everywhere) | B1 |
| B3 | `info_by_name`: the record's volume word translated to the handle's, learned per volume | B2 |
| B4 | `rights::forget`: a root without a record gives its place back | B2 |
| B5 | README, 0.22.0; squash-merge (done: `6a2da47`, release `0.22.0`) | B3, B4 green |

### C — mcpp-language-server (server)

| Task | What | Depends on |
|---|---|---|
| C1 | S1: `Connection::exit_status()`; `ExitCondition` (NTSTATUS and signal names); log, incident, event, issue text | — |
| C2 | S2: in-flight AST builds from `ast-build` events; suspects by rule (named / in flight / neither); an exit ended from outside names nobody | C1 |
| C3 | S3: crash ledger in the workspace cache; one retry per term; unchanged is not eligible; a new session reads it; `Try again` clears | C2 |
| C4 | S4: `startsMade_`, slots spent, reserve, `paused`; refused start logged, counted truly, offers `mcppls.restartServer` | C1 |
| C5 | D: `reproduce.json` in the bundle; `engine.fileIdentity`; the kit line; the payload's `dirty` stamp | C1 |
| C6 | T: unit tests of C1–C4 (the pure rules, and the engine with a process that ends by a signal: set aside at the first exit, across an edit and a session; paused at the reserve); conformance `engine-trap`; a `windows-2025` conformance row | C1–C4 |
| C7 | pin A: `packaging/engine-candidate.json` → A's CI artifact; after A7, `payload.lock.json` → the release, candidate file dropped | A's CI artifact; A7 |
| C8 | docs (`50-troubleshooting.md`, spec/schema where an event or issue changes), changelog, version 0.23.0 | C1–C7 |
| C9 | squash-merge; main CI cancelled, release workflow first; main CI re-run after the release; the release verified locally | C8, A7 |

## 4. Order across repositories

```
A1..A5 ──► A's CI (image + acceptance, windows-2025 gate) ──► artifact clangd-openkal
   │                                                              │
   └─► A6 comparison run                                          ▼
B1 ─► B2 ─► B3,B4 ─► B5 (merge)              C1..C6 ──► C7 candidate ──► C's CI green
                                                                  │
                         A6 green + C green ──► A7 merge, tag v0.0.22, release
                                                                  │
                                              C7 pin of the release ──► C's CI green ──► C9
```

A's image build is the long pole (hours), so A goes first and B and C are written while it runs.
B is independent of A and C. C cannot merge before A is released; A is not merged before C's CI has
run on A's candidate (the integration rule of both repositories).

## 5. After this round

- The platform pin (openkal-platform with openkal-windows ≥ 0.22.0) in mcppls and in the engine's
  `openkal/clangd/mcpp.toml`: W4 and P1 reach users; `engine-restarts` goes to 40; the
  `windows-2025` control in the engine's CI turns to "one identity".
- E2: the runner's trap block, and the server's reading of it.
- The same correction of `forget()` in openkal-linux and openkal-macos; one start count for the
  server process (§2.13).
- macOS's grant set (§2.17); the engine's lock anchor at a cold start (§2.19); `cmake-msvc-std`
  for CMake 4.4 and `mcpp-polling-watch-inputs@polling` on `windows-2025` (§2.14, §2.16).
- Housekeeping: draft PR #87, branch `tmp/issue86-systop-repro`, pre-release `issue86-evidence`.
