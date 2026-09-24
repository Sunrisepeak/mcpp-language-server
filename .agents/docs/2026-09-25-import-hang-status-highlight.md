# Typing `import a.` freezes the editor; what the status means; highlighting `import`

Status: proposal for review · measured 2026-09-25 on mcppls 0.0.3 (linux-x64 payload, bundled clangd
23.1.0 `ea7d852a`), `main` at c508e62, project `~/test/mcpp/hello`

| # | Item | Proposal | Section |
|---|---|---|---|
| H1 | clangd 23.1 spins forever on a module name that ends in `.` at end of line | mcppls never sends that text to clangd: a same-line `;` placeholder that keeps positions | §1, §3 |
| H2 | The guards added in 0.0.2/0.0.3 did not recover | a busy clangd is not always a compiling clangd: bound it, and stop treating an edit to the file itself as a rebuild | §2, §4 |
| H3 | Half-typed imports churn the plan: a stand-in for `hello.`, clangd restarts | validate module names; don't create a stand-in for an import that is still being typed | §5 |
| S | "Some features are limited" for a missing `;` | separate *your code has an error* from *the server is working* and from *the server lost a feature* | §6 |
| K | `import` is not highlighted | mcppls fixes it in two layers: an injected grammar in the extension, and module semantic tokens from the server (a custom `module` type with fallbacks); colors stay with themes | §7 |
| W | Temporary fixes are scattered and unlabeled | one workaround module; each entry names its upstream bug, how to remove it, and a canary test | §9 |
| C | Other C++ extensions on the same files | keep asking before any change; add commands, re-checks and a notice; semantic tokens follow the spec | §7, §10 |

## 1. H1: root cause

**What was seen (live, the user's session).** clangd pid 1661254 at 87% CPU for minutes. Its thread
`Worker:main.cpp` was in state R with about 200 s of CPU; every other thread was asleep. ptrace is
restricted (`yama/ptrace_scope=1`), so no live stack. The server log
(`~/.cache/mcppls/logs/server-20260924-211510.237-be62.log`) shows the edit, then nothing answered:

```
21:15:23 restarting clangd: units are compiled with other arguments
21:15:26 clangd could not find module hello                     <- while typing `import hello`
21:15:27 module hello has a stand-in ... trying module hello again
21:15:56 clangd: main.cpp:3:14: error: expected identifier after '.' in module name
21:15:58 module hello. has a stand-in                           <- a stand-in for an invalid name
21:16:18 ... did not answer textDocument/documentHighlight in time
21:16:55 .. 21:20:08  did not answer inlayHint / codeAction / semanticTokens / documentSymbol ... (every request)
```

**Reproduced without mcppls.** The bundled clangd, the same compile database, and a small LSP driver
that erases `greet;` from `import hello.greet;` and types it back one key at a time. Every request
is answered until the buffer holds `import hello.`. From then on nothing is answered, even after the
text is fixed, and `Worker:main.cpp` stays at 100% of a core. The verbose log shows
`Reusing preamble version 1 for version 7`, then "building AST" never finishes.

**Minimal case.** `clangd --check=FILE` with `-std=c++23`, no project and no modules support, gives
the same result:

Each case was given 8–20 s. A blank cell was not run.

| File contents | clangd 23.1.0 (bundled) | 22.1.8 | 24.0.0git `510126255` |
|---|---|---|---|
| `import hello.⏎`, `import hello.⏎int x;⏎`, `import std;⏎import hello.⏎…` | **hangs** | error, 0.1 s | error, 0.1 s |
| `import a.b.⏎`, `export import hello.⏎` | **hangs** | | |
| `export module a.⏎`, `module a.⏎`, `module;⏎export module m.⏎` | **hangs** | | |
| `import hello. ⏎`, `import hello.\t⏎`, `import hello.// c⏎` | **hangs** | | |
| `import hello.⏎;⏎` (the `;` is on the next line) | **hangs** | | |
| `import hello.;`, `import hello. ;`, `import hello.;// c` | ok, error | | |
| `import hello`, `import hello.greet` (no `;`), `import hello:`, `import hello:p.` | ok, error | | |

**Cause.** In clang 23.1, a module name in an `import` or `module` directive that ends in `.`,
with nothing after the dot on that line except whitespace or a comment, sends the preprocessor into a
loop. This is the new P1857 code, which lexes these lines as directives. It is not a modules-support
problem in clangd: the same file hangs without `--experimental-modules-support`.

- **Why it happens every time.** Typing any dotted name at the end of a line goes through
  `import a.`, and so does `export module a.b;` in an interface unit.
- **Why nothing gets it back.** `$/cancelRequest` cannot interrupt a parse. clangd builds a file's
  versions one after another on one worker (ASTWorker), so the fixed text queues behind the spin
  forever. Only killing clangd frees it.
- **Upstream.**
  - LLVM 23.1.1 and 23.1.2: none of their 74 commits has a title about the lexer or `import`. They
    were not run.
  - clangd/clangd has published no release after 23.1.0, only snapshots from main
    (`snapshot_20260913` is the latest).
  - Main is fixed: the 24.0.0git build above passes.
  - A likely fix, not yet confirmed by bisect: `6dcfc17b1b` "[clang] Move the diagnostic of
    unexpected token after module name from phase 4 to phase 7 (#187846)", 2026-08-21. It is on main,
    in the 24git build, and not in 23.1.2.
  - I found no upstream issue for this hang.

## 2. Why the stability work did not help

Each guard did what it was designed to do. The design assumed that a busy clangd is a clangd making
progress.

1. **StuckWatch detects only an idle clangd** (`clangd.cpp:1108-1180`, `guard.cpp` `StuckWatch::check`,
   `IDLE_SHARE = 0.05`). "A long compile keeps a core busy, and is left alone"
   (`docs/50-troubleshooting.md`). A spin keeps a core busy too, so the verdict is always "busy, not
   stuck". There is no upper bound on how long busy may last.
2. **The set-aside path is switched off by the edit that causes the hang.**
   - `Quarantine::timed_out` (`clangd.cpp:839-848`) needs unanswered requests from at least 2 files
     to call clangd *stalled*. With only `main.cpp` open, every unanswered request is for that one
     file.
   - The per-file path is gated by `changed_recently_(path)`, which is true for
     `GENERAL_PATIENCE` = **120 s after any edit to the file itself**.
   - The hang is caused by an edit, and the user keeps editing to fix it, so the 120 s restarts on
     every key. The verdict stays `wait`.
3. **Recovery is slow and can bring the hang back.** This is read from the code, not measured.
   - Once typing stops, recovery takes 120 s, then 2 more timeouts, then set-aside, then
     `Reclaim::if_busy` restarts clangd (`clangd.cpp:1820-1835`, the case the code itself calls
     "the spin in experiment S17"). That is about 2.5–3 minutes with every feature blank.
   - When the set-aside term ends (`Quarantine::due`, 2 minutes the first time), the file goes back
     to clangd with whatever text it has. If that text still has `import hello.`, clangd spins
     again. The earlier hand-back on an edit (`clangd.cpp:582`) compares import *structure*, and
     mcppls's scanner does not count `hello.` as an import, so it cannot tell that text is the
     problem either.
   - Three restarts in ten minutes reach the cap, and clangd is then left spinning.
4. **Timed-out requests get empty answers** (`request.reply(Answer {})`). Semantic tokens, hover,
   symbols and code actions all go blank, which is the "plugin unusable" the user saw.

## 3. H1 fix: do not send clangd text that hangs it (first, small)

In the clangd engine, before `didOpen` and each `didChange` goes to clangd:

- **Detect.** Use `project::scan`'s lexer on lines that start with `[export] import` or
  `[export] module`. A module name (or partition) whose last token is `.` and is followed only by
  whitespace or a comment up to the end of the line is a hazard.
- **Rewrite what clangd sees.** Insert `;` immediately after that dot, on the same line. Measured on
  23.1.0: every hazard in §1 then finishes at once with the proper `pp_module_expected_ident` error.
  - **It never changes valid code.** Under P1857 an `import` or `module` directive ends at the end
    of its line, so a dot there is always an error. The rewrite only turns a hang into that error.
  - **Positions.** Every other line is unchanged, and so is the edited line up to and including the
    dot. Only the trailing whitespace or comment after it moves right by one column. Nothing is
    there to request, and a result that clangd places there is moved back by one.
  - **Do not delete the dot.** `import hello` makes clangd look for module `hello`, which starts the
    stand-in churn of §5.
- **Sync.** While a file's clangd text differs from the editor's text, or did at the last change,
  send clangd full-text `didChange` built from `document.text`. Today the engine forwards the
  client's message as is (`clangd.cpp:589`).
- **Diagnostics.** Keep clangd's `pp_module_expected_ident` ("expected identifier after '.' in module
  name"). Its wording is already right; only its range moves to the dot.
- **A workaround, registered as `WA-CLANGD-001` (§9).** The code lives in the workaround module.
  - Its registry entry turns it on for 23.1.x, through a trait `hangsOnTrailingDotModuleName`.
  - A clangd given with `--clangd PATH` is matched by its version.
  - A canary test tells us when it can go.
- **Upstream.**
  1. Bisect and confirm the fix commit.
  2. File the LLVM issue and ask for a backport to 23.1.x.
  3. Update `packaging/payload.lock.json` once clangd/clangd publishes a fixed release.
  - Not recommended as the default: bundling a clangd/clangd snapshot from main.
    `snapshot_20260913` postdates the candidate fix, so it should contain it, but that was not run.
    It would swap one known bug for an unreleased build; at most, offer it as an opt-in.

## 4. H2 fix: a clangd that stays busy without progress is also stuck

A defence against the next spin nobody has found yet. Target: **features back within about 15 s,
even while the user keeps typing.**

- **Budget the work that should be short.** clangd reports what each file is doing (`fileStatus_`).
  - Building a preamble or modules can take minutes. It keeps today's patience and progress checks.
  - A main-file AST build on a reused preamble is short: milliseconds for `hello`. clangd is spinning
    on the file when all of these hold:
    - the file's state has not changed for its budget;
    - the CPU is busy;
    - a newer version of the same file is waiting.
  - **Budget.** `max(20 s, 5 × the file's last successful AST build)`. A heavy template file that
    really takes 15 s is not called stuck. The numbers are a starting point, to tune on the
    real-project stress runs (0.0.2 plan).
  - **Cost of a false positive.** A restart, plus rebuilding module BMIs. That is why the budget
    follows each file's own history instead of a fixed number.
- **An edit to the file itself is not a rebuild.** Split `changed_recently_`:
  - Edits to module sources in the file's import closure keep `GENERAL_PATIENCE`.
  - An edit to the file itself gets a short grace, about 10 s.
- **One file is enough.** If every unanswered request is for one file, clangd answers nothing else
  for `STALL_WINDOW`, and the CPU is busy, that file is stuck. Today this needs 2 files.
- **Recovery.**
  - Set the file aside and restart at once (no deferral).
  - Remember a hash of the text clangd hung on. That exact text never goes back to clangd; any other
    text goes back immediately, without waiting out a term.
  - **These restarts still count toward the cap**; a new text can hang again. What changes is what
    happens at the cap:
    - the file that caused the restarts stays with mcppls's engine until its text changes;
    - clangd is restarted once more, without that file;
    - clangd is never left spinning.
- **Meanwhile, native answers.** While the file is aside, the native engine answers (module
  navigation, `import` completion, module diagnostics). Timed-out requests fall back to it instead
  of an empty answer where it can answer.

## 5. H3: plan hygiene for half-typed imports

- **Validate module names** from the model's `requires` and from scanning: `ident(.ident)*` with an
  optional `:ident(.ident)*` partition. An invalid name gets no stand-in, only a diagnostic.
  - The `hello.` stand-in came from mcpp's rescan after autosave (`files.autoSave: afterDelay`).
    mcppls's own scanner rejects a trailing dot (`scan.cpp:190-205`).
  - Report this to mcpp.
- **No stand-in for an import that is still being typed.** A name that does not resolve in a file
  the user is editing is usually a typo in progress.
  - Report it as a diagnostic on the import, e.g. "module `hello` not found; did you mean
    `hello.greet`?".
  - Create a stand-in only after the text has been stable for a while, or for imports in files that
    are not open.
  - **Exception: a unit that provides a module** (`.cppm`, partitions). Building such a unit with an
    import that does not resolve is what deadlocks clangd 23.1 (`plan.cpp`, "building it is what
    deadlocks"). It keeps today's rule, a stand-in or leaving it out (`WA-CLANGD-002`, §9). A plain
    source file such as `main.cpp` does not need one: without a stand-in, clangd just reports the
    module as not found.
- **To investigate: the 21:15:23 restart during an import edit** ("units are compiled with other
  arguments"). Changing a file's imports should not restart clangd. Add a test once the cause is
  found; this analysis did not confirm it.

## 6. S: the status must say *whose* problem it is

**Today.**
- `compute_state()` (`workspace.cpp:1107-1122`) returns `degraded` for any engine issue, model issue
  or plan issue.
- `plan.issues` includes `unresolved-module` and `module-build-failed`, which a typo or an import
  still being typed produces.
- The extension shows every `degraded` state as "Some features are limited" on a warning background
  (`editors/vscode/src/status.ts:117`).

So a missing `;`, a slow rebuild, and a clangd that has lost the file all look the same. With
autosave, each half-typed state reaches the plan, so the warning flickers while typing.

**Measured.**
- On a copy of the project with `import hello.greet` (no `;`) on disk, `mcppls report` gives state
  `ready` and no issues. The final text alone is fine; clangd reports
  `expected_semi_after_module_or_import`, as it does `expected_semi_after_expr` for a statement.
- In the live session, the intermediate texts `import hello` and `import hello.` each put a plan
  issue in place (log at 05:15:27, 05:15:58, 05:20:02). Each one turned the status to "limited".
- These are code problems reported as a lost feature.

**Rule: attribute every problem to where it comes from.**
- Each plan or engine issue carries the file and range that caused it.
- If the cause is text in a document the user has open (a syntax error, an unterminated `import`, an
  import name that resolves to nothing, a module that fails because of its own code), it is a
  **code** problem.
  - It becomes a diagnostic on that range, with a clear source (clang, or mcppls for module names).
  - It never changes the status.
- Only causes outside the user's text are **limited**: toolchain, environment, a dependency's
  missing generated module, clangd failing.
- The model layer has to follow the same rule. mcpp's scan result and the plan's `unresolved-module`
  or `module-build-failed` issues have to say which import of which file caused them, so they can
  be shown as code problems.

**Proposal: three categories, each shown in its own place.**

| Category | Examples | Where | Status bar |
|---|---|---|---|
| **Code**: the user's source is wrong | `import hello.`, `import helo;`, a module that does not compile because of its code | a diagnostic on the line (Problems panel) | stays ✓ ready |
| **Working**: temporary | rebuilding after an edit, preparing modules, restarting clangd | spinner, "Analyzing main.cpp", shown only after about 1 s | spinner |
| **Limited**: a feature is actually lost | clangd hung and restarted, file set aside, restart cap reached, clangd cannot run, kit or SDK missing, stale model | warning with the scope and the loss: "main.cpp: basic features only; clangd stopped responding" | ⚠ / ✖ |

- **A code problem with a consequence.** A module that does not compile because of its own code
  sends its importers to mcppls's engine (existing containment, RP1.1).
  - It stays a **code** problem: the fix is in the user's code, not in the setup.
  - The `module-failed` diagnostic on each importer's `import` says what it costs: "module-level
    features only until `hello.greet` compiles".
  - The status bar stays ✓. It does not also warn.
- **Restarting clangd because it hung** is **working** while the restart runs. The cause stays
  afterwards as a *limited* notice naming the file, until that file answers again.
- **Protocol (S3 extension, backward compatible).**
  - `ModuleIssue` gets an optional `category` (`code` | `engine` | `environment` | `project`) and
    `files`.
  - `compute_state` ignores `code` issues, which become diagnostics instead.
  - The status text comes from the top issue, not from a generic phrase.
  - Document this in `docs/specs/s3-lsp-extensions.md`.
- **Hysteresis.** Show `degraded` only after it has lasted about 3 s. Hard failures such as
  `engine-incompatible` or `payload-corrupt` show at once.
- **Wording.**
  - "Some features are limited" goes away.
  - `error` ("Only module-level features are available") stays, but names why.

## 7. K: highlighting `import` — mcppls fixes it, in two layers

**Measured.**
- **Grammar.** VS Code's built-in C++ grammar (better-cpp-syntax `071dd6e`) defines a
  `module_import` rule, but nothing includes it (0 references), so `import std;` gets no keyword
  scope. `export` and `module` are colored, as storage modifiers.
- **Semantic tokens.** clangd 23.1 on `main.cpp`, with modules built: tokens start at line 4
  (`main`). The `import` lines get **no tokens at all**, neither the keyword nor `std` or
  `hello.greet`. clangd's legend has `namespace` but no `keyword` type.
- **This machine.** `import` is colored here only because the separate mcpp-vscode extension
  injects `source.cpp.mcpp-modules`. Without it, a user of mcppls sees `import` uncolored.
- **Earlier claim.** The 0.0.3 plan (§7) said highlighting "needs nothing from us". That is wrong for
  module syntax.

**Who owns what.**

| Layer | What it knows | Owner | In mcppls |
|---|---|---|---|
| Lexical/syntax (keywords, strings, comments) | the text only; must work on every key and with no server | the editor: TextMate (VS Code), tree-sitter (Neovim, Zed, Helix) | layer 1, and only for the module syntax the editor's grammar misses |
| Semantic (what a name *is*: class, function, module; declaration; imported) | the whole project | the language server, through `semanticTokens` | layer 2: mcppls's own module feature |
| Colors | nothing about code | the theme and the user's settings; LSP carries no colors, by design | defaults and documentation only |

Both layers are mcppls's own code: the VS Code extension and the server.

**Layer 1: an injection grammar in the VS Code extension.** It colors at once, while typing, and
even when the server is down.
- `source.cpp.mcppls-modules`, `injectTo: source.cpp`, selector `L:source.cpp`.
- It covers:
  - `module;`
  - `[export] module name[:part];`
  - `module :private;`
  - `[export] import name | :part | <header> | "header";`
- Anchored at line start, and it accepts incomplete names, so `import hello.` is colored while it is
  typed.
- Scopes are the same as mcpp-vscode (`keyword.control.import.cpp`, `keyword.control.module.cpp`,
  `keyword.control.export.cpp`, `entity.name.namespace.module.cpp`), so installing both is harmless.
- This layer makes up for VS Code's grammar, so it is registered in §9 as `WA-VSCODE-001`. Remove it
  when the built-in grammar colors `import std;`; a canary grammar test checks that.

**Layer 2: semantic tokens from the server.** This covers every editor (Neovim included), module
names too, and still answers when clangd is out. It stays within LSP 3.17:
- **Legend owned by mcppls.**
  - mcppls declares its own fixed legend in `initialize`: clangd's types, plus `keyword`, a
    predefined `SemanticTokenTypes` value.
  - It maps clangd's token-type indices into that legend on every response. It does not rely on
    appending to clangd's legend, so a clangd restart, a clangd given with `--clangd`, or no clangd
    at all cannot shift the indices.
- **What the native engine tokenizes.** From its own scan of the document (`project::scan_source`):
  - `export`, `module` and `import` as `keyword`;
  - module and partition names as a custom type **`module`**, with the `declaration` modifier in
    `export module` and a custom `partition` modifier on partitions.
- **Custom types need a fallback in every client.** LSP allows custom types, but a theme only colors
  what it knows.
  - The VS Code extension declares `contributes.semanticTokenTypes`: `module` with
    `superType: namespace`. Any theme then colors it as a namespace until the user picks a color.
  - The Neovim plugin sets `@lsp.type.module` with `default = true`, linked to `@module`.
  - A client that does not say it knows `module` (in `initializationOptions`, sent by mcppls's own
    plugins) gets `namespace` instead. Zed, Helix and the rest then still get a sensible color.
- **Later, and separately designed: `imported` and `exported` modifiers** on the names clangd
  colors (for example, "this function comes from module `hello.greet`"). This is what only a
  modules-aware server can do. It needs mapping each name to its module through the native index
  (`engine/native/exports.cpp`), and is not part of this change.
- **clangd wins every position it covers.** mcppls adds tokens only where clangd has none. Tokens
  stay sorted and never overlap (overlap is only allowed with the client's
  `overlappingTokenSupport`). Measured: clangd has nothing on the `import` lines, so today nothing is
  ever dropped.
- **Requests.**
  - Advertise `full` without `delta`. mcppls re-encodes every answer, so it cannot pass on clangd's
    `resultId`s.
  - A `full/delta` request from an older client gets a full result, which LSP allows
    (`SemanticTokens | SemanticTokensDelta`).
  - `range`: filter to the range.
- **Refresh.** Send `workspace/semanticTokens/refresh`, if the client has `refreshSupport`, when
  clangd restarts, when a file is set aside or handed back, and when modules finish preparing.
- **The same color in both layers (VS Code).**
  - As far as I recall Dark+, the grammar's `keyword.control.*` is purple and semantic `keyword`
    maps to scope `keyword`, which is blue. If so, `import` would change color once the tokens
    arrive. To confirm in the test below.
  - The extension contributes `semanticTokenScopes` for `cpp`: `keyword` →
    `keyword.control.cpp`. clangd emits no `keyword` tokens (its legend has none), so nothing
    else changes.
  - Themes with their own `semanticTokenColors.keyword` still decide. Check Dark+, Light+ and one
    third-party theme, looking for no color change between the grammar and the tokens.
- **Switch and customization.**
  - `mcppls.semanticTokens.modules` (default on), also an `initializationOptions` field and a
    Neovim option. It is for people who prefer their own grammar or tree-sitter colors.
  - Colors are customized the standard way, and the docs show how:
    - VS Code: `editor.semanticTokenColorCustomizations.rules` (`"module": …`,
      `"*.partition": …`);
    - Neovim: `vim.api.nvim_set_hl(0, '@lsp.type.module', …)`.
- **No bundled color themes.**
  - A theme replaces the colors of the whole editor, and people choose theirs on purpose.
  - Themes would need dark, light and high-contrast variants to maintain.
  - They cannot be shared across editors (Neovim colorschemes are another system).
  - If ever wanted, a theme is a separate optional extension, not part of the language server.
- **Neovim.** Tokens show through the `@lsp.type.keyword` and `@lsp.type.namespace` groups. The
  smoke test on Neovim 0.10 to 0.12 checks they are linked by default.
- **clangd missing or hung, or the file set aside.** Answer the native tokens alone, instead of
  today's `null`.
- **This is not a workaround.** It is a feature, permanent: clangd does not tokenize module syntax at
  all.

**Tests.**
- A `vscode-tmgrammar-test` fixture for layer 1.
- A conformance case for layer 2, with clangd both present and absent:
  - `import`/`module`/`export` come back as `keyword`;
  - module names come back as `module`, or as `namespace` for a client that did not ask for
    `module`;
  - clangd's token types come back mapped to the right legend entries;
  - no two tokens overlap.
- A VS Code check that `import` keeps one color from the grammar through the semantic tokens, in
  Dark+ and Light+.
- This also fills the gap the 0.0.3 plan recorded: no test covered semantic tokens.

## 8. Acceptance

- **Conformance `typing-import`, with the real bundled clangd.**
  - Type `import hello.greet;` in `main.cpp` and `export module a.b;` in a `.cppm`, one key at a
    time.
  - Every request is answered within `INTERACTIVE_LIMIT`.
  - clangd is idle within 5 s after typing stops.
  - No restart and no stand-in.
- **Sanitizer unit tests.** Every hazard in §1 is rewritten, and nothing else is. Positions are
  preserved.
- **Guard test.** A fault-injected clangd that spins on a marker text.
  - Features are back within about 15 s while edits continue.
  - The text it hung on is not resent.
  - At the restart cap, the file stays with mcppls's engine and clangd is not left spinning.
  - A file that really takes 15 s to build, while it is being edited, is not called stuck.
- **Status tests.**
  - `import helo;` gives a diagnostic and the state stays `ready`.
  - Typing `import hello.greet;` one key at a time, with autosave on, keeps the state `ready`
    through every intermediate text. A missing `;` is only a syntax diagnostic.
  - A spinning clangd gives `degraded` with text that names the file.
  - Changes shorter than the hysteresis window do not flicker.
- **Highlighting**: the grammar fixture and the semantic-token case (§7).
- **Canary tests** for every workaround in §9.

## 9. W: workarounds in one module, each one labeled

**Why.** Some of these fixes exist only because of a bug in someone else's code (clangd 23.1, VS
Code's grammar, mcpp's scanner). They should go away when that bug does. Today such code is spread
across `clangd.cpp` and `plan.cpp`, gated by `EngineTraits` bools (`clangd.cpp:28-45`). Nothing
records why each one exists or when it can go.

**Design.**
- **Where.**
  - `src/engine/clangd/workarounds.cppm` / `workarounds.cpp`, module
    `mcppls.engine.clangd.workarounds`, for engine workarounds.
  - `editors/vscode/src/workarounds.ts`, plus the grammar file, for the extension.
- **A registry, one entry per workaround.**

  ```cpp
  struct Workaround {
      std::string_view id;           // "WA-CLANGD-001": grep-able, used in comments, logs, report
      std::string_view title;        // what it works around, one line
      std::string_view engine;       // "clangd"
      VersionRange affects;          // data, e.g. [23.1.0, 24.0.0); compared with the running engine's version
      std::string_view upstream;     // issue / fix commit URL, or "unfiled" (then filing is a to-do)
      std::string_view evidence;     // doc section and fixture, e.g. ".agents/docs/2026-09-25-...#1"
      std::string_view added;        // mcppls version and date
      std::string_view removeWhen;   // the condition, e.g. "bundled and minimum clangd contain 6dcfc17b1b"
      std::string_view canary;       // the test that fails once the upstream bug is gone
  };
  ```

- **Gating comes from the registry.** `traits_for_version` sets its bools from the entries that apply
  to the engine's version, so the registry is the only list.
- **One code path per workaround.** The code lives in the module, for example
  `sanitize_module_names(text) -> {text, insertions}`. The call site carries one comment:
  `// WA-CLANGD-001, see workarounds.cppm`.
- **Visible.**
  - At startup, one log line lists the active workarounds for the engine version.
  - `mcppls report` lists them under `engines[].details.workarounds`.
  - Bug reports then show which ones were on.
- **Canary tests drive removal.** Each entry has a test that runs the upstream bug with the
  workaround off, and expects the bug.
  - Example for `WA-CLANGD-001`: `clangd --check` on `import hello.` must time out.
  - When a clangd update in `payload.lock.json` fixes the bug, the canary fails with "WA-CLANGD-001
    is no longer needed: remove it".
  - Removing a workaround is then a failing test, not someone's memory.

**Initial entries.**

| ID | What | Affects | Remove when | Canary |
|---|---|---|---|---|
| WA-CLANGD-001 | Same-line `;` after a trailing-dot module name (§3) | clangd [23.1.0, 24): 23.1.0 measured, .1/.2 assumed from commit titles | the bundled and minimum supported clangd contain the fix (bisect; candidate `6dcfc17b1b`) | `--check` on `import hello.` times out |
| WA-VSCODE-001 | Module-syntax injection grammar (§7 layer 1) | VS Code built-in cpp grammar `071dd6e` | the built-in grammar colors `import std;` | tmgrammar test without the injection |
| WA-CLANGD-002..005 | Existing: stand-ins for unresolved imports (`hangsOnUnresolvedImports`), module preparation, module hints, MSVC STL aligned allocation | clangd 23.1.x | to be written when moved: each needs its upstream reference and a canary | to be written |

**Not workarounds, kept out of the registry.** These stay whatever clangd does:
- H2, the busy-without-progress guard (§4). It is the defense for the next unknown bug; its numbers
  are tuning, not a workaround.
- H3, module-name validation and no stand-in for an import being typed (§5). This is input
  checking; reporting the mcpp scanner bug is a separate to-do.
- S, status categories (§6).
- K layer 2, semantic tokens (§7).

## 10. Coexistence with other C++ language extensions

**What exists today.**
- **VS Code** (`editors/vscode/src/conflicts.ts`). Once per workspace, it asks to turn off the
  language features of cpptools (`C_Cpp.intelliSenseEngine: "disabled"`) and vscode-clangd
  (`clangd.enable: false`). The change goes in the *workspace* settings, and an e2e "conflicts"
  scenario tests it.
- **Neovim** (`editors/nvim/lua/mcppls/init.lua:242-280`). It says once when `clangd` or `ccls` also
  attaches to a buffer.
- **Other editors.** The server itself sends one `showMessage` explaining how to disable the
  editor's own clangd (`workspace.cpp:1138-1170`). Zed gets the exact settings. A client that
  detects conflicts itself turns this off with `conflictArbitration: "client"`.

**How semantic tokens behave next to another server.** VS Code does not merge semantic tokens
from several extensions: it uses one provider's result for a document. With vscode-clangd also
active, either its tokens or mcppls's are shown, never both. Layer 1 (the grammar) does not depend
on this, which is one more reason to keep it. Colors do not get mixed up, but mcppls's module tokens
may not show while another C++ server is active.

**Gaps.**
1. **The question is asked once.** After "Keep" or closing the message, duplicates remain and nothing
   says so again. There is also no way to do it later.
2. **Checked only at activation.** Installing or enabling vscode-clangd, or turning IntelliSense back
   on, is not noticed.
3. **Only settings can be changed.** The VS Code API cannot disable another extension; changing its
   settings is the only lever. An extension with no "enable" setting (ccls, for example) can only be
   pointed out.
4. **Observed on this machine.** vscode-clangd is installed and enabled, and `clangd.enable` is not
   set. No second clangd runs only because there is no `clangd` on `PATH`. Once there is one, two
   engines serve the same files, and the status says nothing.

**Proposal.**
- **Keep the rule: never change another extension without asking.**
  - Only settings, in the workspace by default.
  - Reversible, and written to the log.
  - cpptools: turn off IntelliSense only, not the extension. Its debugger keeps working.
- **Commands.**
  - `mcppls: Turn Off Other C++ Language Features`, with a choice of this workspace or everywhere
    (user settings).
  - `mcppls: Restore Other C++ Language Features`, which puts back the previous values.
  - The first-run question stays as a shortcut to the first command.
- **Check again when things change.** Listen to `vscode.extensions.onDidChange` and
  `workspace.onDidChangeConfiguration`. A new conflict gives a status notice (category
  `environment`, §6): "clangd extension also active: results may appear twice". It has the "Turn
  off" action and no warning background; this is a notice, not a lost feature.
- **Candidates.**
  - cpptools: `C_Cpp.intelliSenseEngine`.
  - vscode-clangd: `clangd.enable`.
  - Extensions with no enable setting (ccls): a notice whose action opens the extension in the
    Extensions view (`@id:`), so the user can pick "Disable (Workspace)".
  - mcpp-vscode is not a conflict: it has no language server, and its grammar uses the same scopes.
- **Neovim.** Keep the notice, and add an option `disable_conflicting = true`, off by default. It
  stops `clangd`/`ccls` clients on buffers mcppls serves, with a message saying which one it
  stopped.
- **Tests.**
  - Extend the e2e conflicts scenario: installing or enabling the clangd stub later gives the notice.
  - Restore puts back the previous values.
  - The command writes to the scope the user chose.

## 11. Decisions for review

| # | Question | Recommendation | Alternative |
|---|---|---|---|
| D1 | How to keep clangd 23.1 away from `import a.` | rewrite what clangd sees (§3) | hold the version back from clangd: simpler, but diagnostics and tokens go stale while typing |
| D2 | Bundle a clangd snapshot from main? | no; keep 23.1.0 plus WA-CLANGD-001 | opt-in snapshot for people willing to test |
| D3 | Budget for a main-file AST build | `max(20 s, 5 × last build)`, then tune on stress runs | a fixed number: simpler, and it misjudges heavy files |
| D4 | Restarts caused by a hung text | count toward the cap; at the cap, keep the file out and restart without it | exempt them from the cap: faster, with a risk of restart storms |
| D5 | Status categories | `code` / `working` / `limited`, status from the top non-code issue | keep `degraded` and only reword it |
| D6 | Module names as a custom `module` type | yes, with `superType: namespace` and a `namespace` fallback | `namespace` only: no customization hook |
| D7 | Bundled color themes | no | a separate optional theme extension, later |
| D8 | "Turn off other C++ features" scope | this workspace by default, "everywhere" on request | global by default |
| D9 | Neovim `disable_conflicting` | available, off by default | on by default |

Order: H1 (inside the workaround module, the module created with it) and K layer 1, which are small
and fix what the user sees; then H2, then S, then K layer 2 and H3, and C. Moving the existing
workarounds into the registry comes after. Upstream work (bisect, LLVM issue, backport, clangd
update, the mcpp scanner report) runs in parallel.

## 12. Implementation plan (0.0.4)

One pull request, one version: **0.0.4**. Every item in §3–§10 is in it.

**Measured while planning** (so the design follows the facts):
- **The spin's signature.** A normal rebuild of `main.cpp` shows `parsing includes, parsing main
  file` for about 10 ms, then `idle`. During the spin that state never changes again, while newer
  versions keep arriving.
- **No stand-in needed for a plain `.cpp`.** With an import nothing provides and no stand-in,
  clangd answers normally: one `Module 'nosuch' not found` diagnostic, hover and tokens work. A
  stand-in is therefore needed only when an importer itself provides a module.
- **Unresolved imports already have a diagnostic.** The native index publishes `unresolved-module`
  ("module 'x' not found") on the import. A code-category issue needs no new diagnostic.
- **Timed-out requests already fall back.** `Answer {}` is "unavailable", which hands the request
  to the next engine. The native engine answers what it can (module navigation, hover, completion,
  outline).

**Shared contracts** (fixed before the parallel work starts):

| Contract | Shape |
|---|---|
| `initializationOptions.semanticTokens` | `{ "modules": bool = true, "moduleType": bool = false }`. `modules`: the server adds module-syntax tokens. `moduleType`: the client knows the custom type `module` and modifier `partition`; otherwise module names are sent as `namespace`. |
| Semantic token legend | Owned by the server: a fixed base (LSP standard types and modifiers, clangd's own extras, `module`, `partition`), with any other name clangd declares appended at `initialize`. clangd's indices are mapped by name on every response. `full` and `range`, no `delta`. |
| Status issue `category` | `"code"`, `"engine"`, `"environment"` or `"project"`, optional. Only non-`code` issues can make the state `degraded`. A client with no category treats the issue as non-code, as before. |
| Status hysteresis | The server holds a `ready` → `degraded` change for 3 s, so a passing condition never reaches any editor. `error` is sent at once. |
| VS Code contributions | Grammar `source.cpp.mcppls-modules`; `semanticTokenTypes` `module` (superType `namespace`); `semanticTokenModifiers` `partition`; `semanticTokenScopes` for `cpp`: `keyword` → `keyword.control.cpp`, `module` → `entity.name.namespace.module.cpp`. Setting `mcppls.semanticTokens.modules`. |
| Neovim | `semantic_tokens_modules` (default true) and `moduleType = true` sent. `@lsp.type.module` linked to `@module` by default. Option `disable_conflicting` (default false). |

**Tasks and dependencies.**

```
T0 contracts (above)
 ├─ A server core (main branch of work)
 │   A1 workaround module: registry, WA-CLANGD-001 sanitizer, 002–005 registered, report/log ─┐
 │   A2 H2: self-edit grace, spin detection, poisoned text, restart at the cap  (after A1) ────┤
 │   A3 H3: stand-ins only for module-providing importers, module-name validation ─────────────┤
 │   A4 S: issue categories, state from non-code issues, hysteresis ───────────────────────────┤
 │   A5 conformance: `type-text` and `clangd-check` kinds; `typing-import`, `workaround-canaries` fixtures ─┤
 ├─ B semantic tokens in the server (parallel, own worktree) ───────────────────────────────────┤
 ├─ C VS Code: grammar, token contributions, setting, conflicts, status rendering (parallel) ───┤
 └─ D Neovim: options, highlight link, disable_conflicting (parallel) ──────────────────────────┤
                                                                                                ▼
 E specs (S3), user docs (en, zh-CN), CHANGELOG, version 0.0.4, design record ── F integrate, test all, PR, CI, review, merge, release, verify
```

**How each goal dimension is met.**
- **Architecture.** Workarounds live in one module, driven by a registry. Semantic tokens use the
  existing `merge` role. Categories are decided where issues are made, not in the client.
- **Stability.** The spin can no longer be triggered (H1). Any future spin is found in about 20 s
  and recovered even while the user types (H2). Churn from half-typed imports is gone (H3).
- **Simplicity.** H3 needs no timers (the provider rule). H2 needs no CPU reading (history and a
  waiting newer version decide).
- **User experience.** Nothing freezes. The status says whose problem it is. `import` is colored.
  Conflicts can be turned off or restored at any time.
- **Compatibility and seamless upgrade.**
  - Every new protocol field is optional.
  - A 0.0.3 extension with a 0.0.4 server sees fewer false `degraded` states; a 0.0.4 extension with
    an older server behaves as before.
  - No cache or setting has to change.
  - The sanitizer turns off by clangd version.
- **Cross-platform.** Nothing platform-specific: text rewriting, timers, JSON. Conformance runs on
  Linux x64 and arm64, macOS and Windows as before.
- **Consistency.** State, hysteresis and token types are decided by the server, so every editor
  sees the same thing.
