# Measurements

What the server costs a person and an agent, as test cases: how soon a session is usable, how soon a
large file's semantic tokens are the compiler's, whether a review of a change finds what the change
breaks, whether a coding agent does module work with mcppls, and how a real project feels. Whether
the server answers correctly is [tests/cts](../cts/README.md). Design:
[.agents/docs/design.md](../../.agents/docs/design.md) §5, §7.4, §10.

## The program

`mcppls-bench` (this directory's package) is one program; its cases are the objects of its files,
each stating where it runs, what it holds and the bounds of the metrics it reports:

| File | Case | Kind | What it measures |
|---|---|---|---|
| `startup.cpp` | `startup` | Bench | the timing fixture cold, then warm on the cold start's cache; the first navigation of each start is bounded (15 s cold, 5 s warm) |
| | `startup, the median of five` | Bench, `release` | five of each; the medians are bounded (12 s cold, 5 s warm) |
| | `a large module project` | Bench, `release` | thirty module units with `<windows.h>`, built by MSVC, cold and warm |
| `tokens.cpp` | `tokens` | Bench | tokens-large-file's cold and warm stages: the first completion, the compiler's tokens, the bytes of the whole and of an edit's answer, the refreshes |
| `review.cpp` | `review/<fixture>` | Bench | each fixture of `review/`; `review/every rule's precision and recall` is bounded at 1 |
| `tasks.cpp` | `every task's baseline fails and its reference passes/<task>` | UTest | the agent task benchmark's data |
| | `agent/<task>` | Bench | an agent's runs of each task, when the environment names the agent |
| `ux.cpp` | `ux-mcpp/<stage>`, `ux-xlings/<stage>`, `ux-heavy-headers/edits` | Bench, `ux` | a real project's stages: cold, warm, edits, faults |

A case that runs a fixture of [tests/cts/fixtures](../cts/fixtures) does so through mcppls.harness
([tests/harness](../harness)), as the conformance cases do: every check the runner reports failed is a
failure of the case, and what it measured is the case's metrics. The program speaks the mtest
protocol, so [mtest](https://github.com/mcpp-community/mtest) lists, selects, schedules and records
the cases, and its store keeps each metric across runs:

```bash
mcpp build && mcpp build -p mcppls-bench
bench=$(find target -type f -name mcppls-bench -path '*/bin/*' | head -1)
MCPPLS_CTS_SERVER=$bin/mcppls MCPPLS_CTS_PAYLOAD=target/pack/payload mtest run --case startup "$bench"
mtest analyze trend          # each metric over the stored runs, beside its bounds
mtest explain                # a failure: where, why, and its diagnosis
```

The program runs from the repository's root. The environment names what it runs against: the
conformance cases' (`MCPPLS_CTS_SERVER`, `MCPPLS_CTS_PAYLOAD`, `MCPPLS_CTS_RUNNER`,
`MCPPLS_CTS_FIXTURES`, `MCPPLS_CTS_MSVC_ENV`, `MCPPLS_CTS_MEASURES`, `MCPPLS_CTS_EVIDENCE`; the head
of `tests/harness/src/harness.cppm`), and `MCPPLS_BENCH_MOCK_MODEL`, `MCPPLS_BENCH_AGENT`,
`MCPPLS_BENCH_ARM`, `MCPPLS_BENCH_REPEAT`, `MCPPLS_BENCH_RUNS` (the head of `main.cpp`).

CI's `bench` job runs a platform's cases (`--tag on:<platform>`) without the release's and the
scenarios'; release-checks runs the release's (`--tag 'on:<platform>&release'`); the `ux` job runs a
scenario's stages (`--case ux-mcpp`). Each keeps its store, and the next run takes main's last one, so
the step summary shows each metric beside its history.

## Tasks

The agent task benchmark asks whether mcppls is worth a coding agent's time. Each `tasks/<id>/` is:

| File | Contents |
|---|---|
| `task.json` | `id`, `title`, `prompt`, `project`, `check`, `reference`, `shape`, `toolchain` |
| `project/` | a small mcpp package: the **baseline**, the agent's starting point |
| `reference.patch` | a unified diff from `project/` to a solved state, under which every check passes |

`task.json`:

- `id` is the directory's name; `prompt` is the instruction an agent receives verbatim; `title` is for people.
- `project` is the project's directory, relative to the task's (`"project"`).
- `shape` is `"split"` (interface units and an interface partition in `.cppm`, implementation units in `.cpp`, an
  implementation partition; `tests/cts/fixtures/mcpp-split`) or `"all-cppm"` (every module unit in `.cppm`,
  partitions, a re-exported module; `tests/cts/fixtures/mcpp-all-cppm`). Every task starts from a copy of one of
  the two fixtures' sources.
- `toolchain` mirrors `[toolchain]` of `project/mcpp.toml`: `llvm@23.1.3`, except `add-parameter-split` and
  `export-from-partition-all-cppm`, which pin `gcc@16.1.0`, one per shape.
- `check` is a list run in order, stopping at the first that fails. An entry is an argv list (`["mcpp", "build"]`),
  run in the project's copy with no shell, which must exit 0; or a content check,
  `{"file-contains": {"files": [...], "all": [...], "none": [...]}}`: the files' contents, concatenated in order,
  contain every string of `all` and none of `none` (at least one of the two is given).
- `reference` is `"reference.patch"`.

The baseline fails at least one check, and the baseline with the reference applied passes every check: the case
`every task's baseline fails and its reference passes` holds each task to it, with mcpp and no agent.

### The ten tasks

Five kinds of C++23 module work, each in both shapes:

| Task | Shape | Toolchain | What the agent does |
|---|---|---|---|
| `rename-export-split` | split | llvm | renames exported `hello::greet` to `salute` and fixes the importer (`src/main.cpp`) |
| `rename-export-all-cppm` | all-cppm | llvm | renames exported `calc::length` to `magnitude` and fixes the importer |
| `add-parameter-split` | split | **gcc** | adds a `salutation` parameter to `hello::greet` and updates the call |
| `add-parameter-all-cppm` | all-cppm | llvm | adds a `scale` parameter to `calc::length` and updates the call |
| `move-helper-split` | split | llvm | moves a helper one implementation unit keeps to itself into a new implementation partition, for a second unit |
| `move-helper-all-cppm` | all-cppm | llvm | the same, across two partitions of one module |
| `export-from-partition-split` | split | llvm | exports a new function (`shout`) from a partition the primary interface re-exports |
| `export-from-partition-all-cppm` | all-cppm | **gcc** | the same: exports `midpoint` |
| `fix-broken-import-split` | split | llvm | `src/main.cpp` imports a module partition from outside its module; fixes the import |
| `fix-broken-import-all-cppm` | all-cppm | llvm | the same, in the all-`.cppm` shape |

Every task's oracle is `mcpp build && mcpp test`: the project's `tests/` exercises the solved behaviour, and the fix
makes the existing test pass (or, for `move-helper-*` and `fix-broken-import-*`, makes the project build); the
reference never touches `tests/`.

### Authoring a task

`project/` is the baseline as it ships (an edited copy of a conformance fixture, never the fixture). The reference
is the difference between it and a solved copy, made between directories named `a` and `b` (the convention
`tests/harness/src/patch.cppm` states):

```bash
mkdir /tmp/diff && cp -r tests/bench/tasks/<id>/project /tmp/diff/a && cp -r <solved-copy> /tmp/diff/b
rm -rf /tmp/diff/{a,b}/compile_commands.json /tmp/diff/{a,b}/target
(cd /tmp/diff && git diff --no-index a b) > tests/bench/tasks/<id>/reference.patch
```

`mtest run --case "every task's baseline fails and its reference passes/<id>" mcppls-bench` checks it. A task's
project is never built in place: the case copies it, without `target/` and `compile_commands.json`, before each
check.

### Agent runs

`agent/<task>` runs a real coding agent CLI on a task, headless, `MCPPLS_BENCH_REPEAT` times (default 5), with the
arm `MCPPLS_BENCH_ARM` names -- how much C/C++ tooling the agent has beyond its file search and shell -- and then
the task's checks in the copy the agent changed. A run spends API credits or model usage: the case is skipped
unless `MCPPLS_BENCH_AGENT` names the agent, and no workflow of this repository names one.

| Arm | Claude Code (`claude-code`) | Copilot CLI (`copilot-cli`) |
|---|---|---|
| `grep` | `claude -p --bare`: no plugins, no MCP, no CLAUDE.md | no `.github/lsp.json` |
| `clangd-lsp` | a session that is not `--bare`, with `clangd-lsp@claude-plugins-official` installed at user scope and `clangd` on `PATH` (neither is installed by this repository) | `.github/lsp.json` naming `clangd --background-index` |
| `mcppls-lsp` | `claude -p --bare --plugin-dir editors/claude-code/mcppls-lsp` (`mcppls` on `PATH`; `editors/agents/README.md`) | `editors/copilot-cli/lsp.json`'s `mcppls` entry as `.github/lsp.json` |
| `mcppls-mcp` | `claude -p --bare --mcp-config <mcppls mcp> --strict-mcp-config --allowedTools mcp__mcppls` (not yet run) | `--additional-mcp-config` with `editors/copilot-cli/mcp-config.json`'s server (not yet run) |

Each task's success rate is a metric, and each run's time, turns, tokens and cost where the agent reports them;
every run is a record of `MCPPLS_BENCH_RUNS` (default `target/bench/agent-runs.json`): its task, shape, agent, arm,
success, time, the files it changed outside `src/` and `tests/`, the checks' detail and the end of the agent's
error output. The turns, tokens and cost are read from Claude Code's `--output-format json` (`num_turns`,
`usage.input_tokens`, `usage.output_tokens`, `total_cost_usd`); Copilot CLI documents no JSON output, so its runs
have none. Both CLIs' flags follow their public documentation and may change; `claude -p --help` and
`copilot --help` are the reference before a run.

## Review fixtures

`review/<id>/` measures the review of changes (design §7.4, §10.3, work item RV5): whether `mcppls review` reports
what a change breaks, with evidence, and nothing on a clean change. No agent is involved; a fixture whose
`review.json` scripts a model runs with `mcppls-mock-model`.

| File | Contents |
|---|---|
| `review.json` | `id`, `title`, `category`, `project` (the conformance fixture the change starts from), `remove`, `must`, `must-not`, and optionally `hosts`, `toolchains`, `model`, `incomplete` |
| `change/` | files written over the project, as they are after the change |

The case commits the project to a fresh git repository, applies the change, runs `mcppls review --format json`, and
fails for a `must` entry (`rule`, and optionally `origin`, `file`, `line`, `evidence-file`, `fixed`) no finding
matches, for a finding a `must-not` entry matches, and for a file the review changed. Each rule's precision and
recall over the fixtures are metrics. Categories: `interface-break`, `missed-importer`, `partition-misuse`,
`build-description`, `build-error`, and `clean` changes, which measure false positives.
