---
name: mcppls-contributing
description: Use when contributing to mcpp-language-server (mcppls) — building it, finding where code belongs, writing tests and conformance fixtures, keeping the specifications traceable, and what a change has to prove before it is submitted.
---

# Contributing to mcppls

## The shape of the repository

| Directory | What is in it |
|---|---|
| `src/` | The server. C++23 modules only, no headers, no macros. `src/<dir>/<name>.cppm` is `mcppls.<dir>.<name>` |
| `tests/` | Unit tests, one `test_<subject>.cpp` each, run by `mcpp test` |
| `modules/testing/` | The in-repo test harness package `mcppls.testing` that `tests/` depends on |
| `modules/os/` | Three platform-constant packages; one is chosen by the build target and used with `if constexpr` |
| `docs/specs/` | The five normative specifications, their schemas and examples |
| `conformance/` | Fixtures that drive a real server over LSP and check the answers |
| `tools/bench/` | Review fixtures with expected findings: precision and recall for the review rules |
| `editors/` | The VS Code extension and the coding-agent integrations |
| `packaging/` | How the shipped payload (server + clangd + semantic kit) is assembled |
| `tools/model-gateway/` | A separate package: the only thing that ever talks to a model endpoint |
| `vendor/` | Upstream data carried verbatim (the LSP meta model) |
| `docs/` | User documentation. `.agents/docs/design.md` is the design record — different audience |

## Build and prove it

```bash
mcpp build                               # and --target aarch64-macos / x86_64-windows-gnu
mcpp test                                # plus --profile release: release-only defects are real here
mcpp run -p devtools -- check all
python3 docs/specs/tools/validate.py          # needs jsonschema
```

Conformance needs a payload; `CONTRIBUTING.md` has the exact command and how to get one.

**What a change has to prove** depends on what it touches:

| Change | Prove it with |
|---|---|
| Server behaviour | A unit test, and a conformance fixture if an editor would see it |
| Anything about how a project is detected or planned | A conformance fixture — they drive the real LSP |
| A review rule | A `bench/review/` fixture with its expected findings |
| A specification | The text, its schema, its examples, the fixtures, and `conformance/traceability.json` together |

## Rules that are not negotiable

- **No headers, no macros in `src/`.** If something seems to need one, it is a design question, not
  a workaround.
- **Every normative rule in `docs/specs/` carries an identifier and evidence.** `validate.py` fails if a
  MUST/SHOULD has no identifier, or an identifier has no evidence, or the evidence names something
  that does not exist.
- **Nothing the server starts may be able to hold it.** External programs go through
  `mcppls.platform.toolrun`: own process unit, bounded reads, a deadline that ends the unit.
- **Runs the server starts are offline by default.** A fixture asserts it rather than trusting it.
- **The version lives in `mcpp.toml`** and is written everywhere else by
  `mcpp run -p devtools -- version --set`, never by hand.

## Upstream defects (clangd, mcpp, …)

**Issue #24 is the single register of upstream defects** (pinned, English). Its body is only an
index; **each comment is one problem**, `UP-<nn>` (clangd/LLVM) or `UP-M<n>` (mcpp): symptom,
affected versions, upstream status, what mcppls does, when that can go, evidence, TODO. Read it
before calling something "a clangd bug", and before working around one.

- **Found a new one?** Post one comment in that shape (`unfiled` is a status) and add its row to
  the index — then decide what mcppls does.
- **Compensating in code?** It is a registered workaround: an entry in
  `src/engine/clangd/workarounds.cpp` (`WA-CLANGD-<n>`: upstream, evidence, `removeWhen`, a canary
  where one can exist), and its #24 comment names the ID. A limit mcppls deliberately does not work around
  goes in `.agents/docs/design.md` §7 and in #24.
- **Filed or fixed upstream?** Put the link in that comment and the index. Bumping the bundled clangd
  (`packaging/payload.lock.json`) means walking #24: run the canaries, remove what they say is gone,
  update the comments.
- **Not upstream:** a defect in how mcppls drives an upstream tool (for example the arguments it
  generates) is an mcppls bug, fixed here; #24 records only the upstream side of it.

## Commit messages

Lowercase `type(scope): a sentence that says what is now true`, not an imperative. The body is
prose: what failed, what changed, why. For example:

```
fix(platform): the bound ends the unit even when the tool itself went quietly
feat(model): the cache is a model, per source and fingerprinted, and clangd is never started without a database
```

`git log --oneline -40` is the convention; match it.

## Submitting

Open an issue first for anything beyond a fix. Fork, branch, and make CI pass — all of it, on
Linux, macOS and Windows. A pull request with failing CI is not reviewed.
