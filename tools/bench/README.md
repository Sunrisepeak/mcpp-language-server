# Agent task benchmark

Whether mcppls is worth a coding agent's time, not whether it answers LSP requests
correctly (that's [conformance/](../conformance/)). Design:
[.agents/docs/design.md](../../.agents/docs/design.md) §5.

```bash
mcpp run -p devtools -- bench tasks validate               # no agent, no network, no API key — CI runs this
mcpp run -p devtools -- bench tasks run --agent claude-code --arm mcppls-lsp \
    --repeat 5 --out results.json --i-understand-this-uses-api-credits   # spends API credits — see "run", below
```

`mcppls-devtools bench tasks validate|run` is part of the one entry for this repository's own
tooling ([docs/93-devtools.md](../../docs/93-devtools.md)).

## Tasks

Each `bench/tasks/<id>/` is:

| File | Contents |
|---|---|
| `task.json` | `id`, `title`, `prompt`, `project`, `check`, `reference`, `shape`, `toolchain` — see below |
| `project/` | A small mcpp package: the **baseline** state, handed to the agent (or to `validate`) as the starting point |
| `reference.patch` | A unified diff, `project/` → a solved state, that makes every `check` command succeed |

`task.json` fields:

- `id` — matches the directory name.
- `title`, `prompt` — `prompt` is verbatim the instruction an agent gets; `title` is for humans (this README, results).
- `project` — the project directory's name, relative to the task directory (always `"project"` here).
- `shape` — `"split"` (interface units and an interface partition in `.cppm`, implementation units in `.cpp`, an
  implementation partition — see `conformance/fixtures/mcpp-split`) or `"all-cppm"` (every module unit in `.cppm`,
  partitions, a re-exported module — see `conformance/fixtures/mcpp-all-cppm`). Every task starts from one of those
  two fixtures' sources, copied and then modified for the task (never referenced in place).
- `toolchain` — mirrors the `[toolchain]` table in `project/mcpp.toml` (informational: `mcpp build`/`mcpp test`
  already read it from there). `llvm@22.1.8` for most tasks; two tasks (`add-parameter-split`,
  `export-from-partition-all-cppm`) pin `gcc@16.1.0` instead, one per shape, so both toolchains the rest of this
  project targets are exercised here too.
- `check` — a list of checks, run in order like a `&&` chain (stop at the first failure). Each entry is one of:
  - an argv list (`["mcpp", "build"]`) — run with the project copy as the working directory, no shell; must exit 0.
  - a declarative **content check**, `{"file-contains": {"files": [...], "all": [...], "none": [...]}}`: the
    listed files' contents, concatenated in that order (no separator), must contain every string in `all` and
    none of the strings in `none` (both optional, but at least one is required), evaluated in-process by
    the runner. `files` with more than one entry is for a check that spans an interface and its implementation unit (see
    `rename-export-split`'s task.json: it checks `greet.cppm` and `greet.cpp` together, since the rename touches
    both).
- `reference` — `"reference.patch"`, applying cleanly to a fresh copy of `project/`.

**The baseline must fail at least one check; the baseline with `reference.patch` applied must pass every
check.** `validate` (below) enforces exactly that, for every task, on every run.

### The ten tasks

Five kinds of C++23 module work, each done once in the `split` shape and once in `all-cppm`:

| Task | Shape | Toolchain | What the agent must do |
|---|---|---|---|
| `rename-export-split` | split | llvm | Rename exported `hello::greet` to `salute`; fix the importer (`src/main.cpp`) |
| `rename-export-all-cppm` | all-cppm | llvm | Rename exported `calc::length` to `magnitude`; fix the importer |
| `add-parameter-split` | split | **gcc** | Add a `salutation` parameter to `hello::greet`; update the call site |
| `add-parameter-all-cppm` | all-cppm | llvm | Add a `scale` parameter to `calc::length`; update the call site |
| `move-helper-split` | split | llvm | A helper private to one implementation unit is also needed by a second one; move it into a new implementation partition |
| `move-helper-all-cppm` | all-cppm | llvm | Same, across two partitions of the same module instead of two implementation units |
| `export-from-partition-split` | split | llvm | Export a new function (`shout`) from a partition already re-exported by the primary interface |
| `export-from-partition-all-cppm` | all-cppm | **gcc** | Same: export `midpoint` from a re-exported partition |
| `fix-broken-import-split` | split | llvm | `src/main.cpp` illegally imports a module partition from outside its module; fix the import |
| `fix-broken-import-all-cppm` | all-cppm | llvm | Same, in the all-`.cppm` shape |

Every task's oracle is `mcpp build && mcpp test`: the project's `tests/` already exercises the target
(post-fix) behavior — the fix makes the existing test pass (or, for `move-helper-*` and
`fix-broken-import-*`, makes the project build at all) rather than the test itself changing. This is why
`reference.patch` never touches `tests/`. Every task is answer-checkable by a command, per design — none of
them is "tell me where X is used."

### Authoring a task

Build `project/` directly as the baseline you want to ship (edit a copy of a conformance fixture — never
edit the fixture itself). To generate `reference.patch`, make a second, *solved* copy next to it and apply
the fix there, then diff the two into directories literally named `a` and `b` (this is what
`apply_patch()` in `tools/devtools/src/bench_patch.cpp` expects — see its comments):

```bash
mkdir /tmp/diff && cp -r bench/tasks/<id>/project /tmp/diff/a && cp -r <solved-copy> /tmp/diff/b
rm -f /tmp/diff/a/compile_commands.json /tmp/diff/b/compile_commands.json  # mcpp build/test writes this; never commit it
rm -rf /tmp/diff/a/target /tmp/diff/b/target
(cd /tmp/diff && git diff --no-index a b) > bench/tasks/<id>/reference.patch
```

Verify with `mcpp run -p devtools -- bench tasks validate --only <id>` before committing — never build or test
directly inside `bench/tasks/<id>/project/` itself, since that leaves `target/` and `compile_commands.json` behind
(`validate`'s own project copies clean these on the way in, but the committed source should never carry them).

## `validate`

```
mcpp run -p devtools -- bench tasks validate [--tasks DIR] [--only ID] [--timeout SECONDS]
```

For every task: copies `project/` to a scratch directory under `target/bench-validate/` and runs `check`
(expects at least one entry to fail); copies `project/` again, applies `reference.patch` (`git apply -p2`,
falling back to a small unified-diff applier built into the runner if `git` is missing or refuses the patch
— both expect the `a/a/...`, `b/b/...` paths the authoring recipe above produces), and runs `check` again
(expects every entry to succeed). Prints one `PASS`/`FAIL` line per task and exits non-zero if any task
fails either half. This is what `.github/workflows/ci.yml`'s `bench-validate` job runs — no agent, no
network beyond what `mcpp build`/`mcpp test` already need (toolchains are pre-installed by `setup-mcpp`),
no API key. The scratch directory is under `target/`, not the system temp directory, deliberately: it keeps
every scratch write inside this repository's own checkout.

## `run` — spends API credits; never run automatically

```
mcpp run -p devtools -- bench tasks run --agent claude-code|copilot-cli --arm grep|clangd-lsp|mcppls-lsp|mcppls-mcp \
    --repeat N --out results.json --i-understand-this-uses-api-credits [--tasks DIR] [--only ID] [--timeout SECONDS]
```

Points a real coding agent CLI at every task, headless, `--repeat` times per task, under one **arm** (how
much C/C++ tooling the agent gets beyond its default file search and shell):

| Arm | Claude Code | Copilot CLI |
|---|---|---|
| `grep` | `claude -p --bare` (no plugins, no MCP, no CLAUDE.md — file/grep/bash tools only) | no `.github/lsp.json` written |
| `clangd-lsp` | a normal (non-`--bare`) session; **assumes** `clangd-lsp@claude-plugins-official` is already installed at user scope and `clangd` is on `PATH` — this repository's tooling does not install it | `.github/lsp.json` registering plain `clangd --background-index` |
| `mcppls-lsp` | `claude -p --bare --plugin-dir editors/claude-code/mcppls-lsp` (self-contained; needs `mcppls` on `PATH`, see `editors/agents/README.md`) | `editors/copilot-cli/lsp.json`'s `mcppls` entry, copied to `.github/lsp.json` |
| `mcppls-mcp` | `claude -p --bare --mcp-config <mcppls mcp> --strict-mcp-config --allowedTools mcp__mcppls` — the MCP tools alone, no LSP plugin (**unverified**: never run by this repository) | `--additional-mcp-config` with `editors/copilot-cli/mcp-config.json`'s server, no `.github/lsp.json` (**unverified** likewise) |

After the agent exits, `run` runs the task's `check` commands in the (agent-modified) project copy and
records one entry per run in `--out`:

```json
{
  "agent": "claude-code", "arm": "mcppls-lsp", "generated_at": "...", "run_id": "...",
  "repeat": 5, "summary": { "runs": 50, "successes": 41, "success_rate": 0.82 },
  "runs": [
    { "task": "rename-export-split", "shape": "split", "agent": "claude-code", "arm": "mcppls-lsp", "repeat": 1,
      "success": true, "wall_time_s": 38.4, "agent_exit_code": 0,
      "turns": 6, "input_tokens": 18213, "output_tokens": 941, "cost_usd": 0.0412,
      "files_changed_outside_scope": [], "check_detail": "all checks passed", "agent_stderr_tail": "" }
  ]
}
```

`turns`, `input_tokens`, `output_tokens`, `cost_usd` come from the agent's own JSON output when it reports
them (only Claude Code's `--output-format json` does, as of this writing — see "Unverified", below) and are
`null` otherwise. `files_changed_outside_scope` lists every file added, removed or modified under the
project copy, outside `src/` and `tests/` (target/ build output is not counted).

**Refuses to run without `--i-understand-this-uses-api-credits`** — every task/arm/repeat combination is a
real agent invocation. Nothing in this repository's own tooling, including `bench-validate` in CI, passes
that flag or invokes `run`; running it is a deliberate, manual, human decision, made outside of this
implementation.

### Unverified

`run` was written against the current public docs for both CLIs' headless/non-interactive flags — it has
never actually been executed (per the instructions this work was done under), so treat the following as
reviewed-not-run:

- The exact Claude Code `--output-format json` field names read here (`num_turns`, `usage.input_tokens`,
  `usage.output_tokens`, `total_cost_usd`) follow the documented shape (`result`, `session_id`,
  `total_cost_usd` are confirmed by <https://code.claude.com/docs/en/headless>; the per-turn/per-token field
  names are the Agent SDK's usual result-message shape, not individually shown in that page's examples).
- GitHub Copilot CLI's docs describe no JSON output flag at all, so `turns`/`input_tokens`/`output_tokens`/
  `cost_usd` stay `null` for every `copilot-cli` run; `success`, `wall_time_s` and
  `files_changed_outside_scope` still come from the runner's own measurements, not the agent's output.
- The `clangd-lsp` arm for Claude Code has no local plugin source to point `--plugin-dir` at (unlike
  `mcppls-lsp`), so it relies on the operator having already run
  `claude plugin install clangd-lsp@claude-plugins-official` at user scope, and on `clangd` being on `PATH`.
- Both agent CLIs' exact flags may drift; re-check `claude -p --help` / the headless docs and `copilot
  --help` before a nightly run.

## Review fixtures

`bench/review/<id>/` measures the review of changes (design §7.4, §10.3, work item RV5): whether `mcppls review`
reports what a change breaks, with evidence, and nothing on a clean change. No agent and no model are involved;
CI runs every fixture on every host.

```bash
mcpp run -p devtools -- bench review --server target/.../bin/mcppls --payload payload [--fixture ID] [--report review.json]
```

`bench review` builds a git repository per fixture under `target/bench-review/` (inside the
checkout, like `validate`'s scratch space above) and writes the `--report` JSON.

| File | Contents |
|---|---|
| `review.json` | `id`, `title`, `category`, `project` (a conformance fixture the change starts from), `remove`, `must`, `must-not` |
| `change/` | Files written over the project, as they are after the change |

The runner commits the project to a fresh git repository, applies the change, runs `mcppls review --format json`,
and checks that every `must` entry (`rule`, and optionally `file`, `line`, `evidence-file`) is matched by a finding,
that no finding matches a `must-not` entry, and that the review left every file as it was. It prints the precision
and recall of each rule over all fixtures. Categories: `interface-break`, `missed-importer`, `partition-misuse`,
`build-description`, `build-error`, and `clean` changes that measure false positives.
