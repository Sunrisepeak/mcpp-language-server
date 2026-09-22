# Contributing

## Prerequisites

The server is built with [mcpp](https://github.com/mcpp-community/mcpp) and a pinned LLVM
toolchain. The pins CI uses live in [`.github/versions.env`](.github/versions.env)
(`MCPP_VERSION`, `LLVM_VERSION`); match them locally so you build what CI builds:

```bash
xlings install mcpp@<MCPP_VERSION> -y -g   # or however you already have mcpp installed
mcpp toolchain install llvm <LLVM_VERSION>
mcpp toolchain default llvm@<LLVM_VERSION>
```

`.github/scripts/install-mcpp.sh` is the exact recipe CI runs, if you want to follow it literally.

## Building and testing

```bash
mcpp build                              # native host
mcpp build --target aarch64-macos       # cross-build for macOS (from Linux, in CI)
mcpp build --target x86_64-windows-gnu  # cross-build for Windows (from Linux, in CI)
mcpp test                               # dev profile
mcpp test --profile release             # release profile — ship-only defects have been found here (design 12.9)
```

Executables land under `target/<triple>/<fingerprint>/bin/`. After changing anything under
`vendor/lsp-metamodel/metaModel-3.18.json` or `src/lsp`, regenerate and check nothing drifted:

```bash
mcpp run mcppls-lspgen -- generate --meta-model vendor/lsp-metamodel/metaModel-3.18.json --out src/lsp
git diff --exit-code src/lsp
```

## Conformance fixtures

Conformance fixtures drive a real server through LSP and check what comes back
([`conformance/README.md`](conformance/README.md) has the full fixture list and scenario format).
Running them needs a **payload** — the server alone isn't enough, since fixtures also exercise
clangd and the semantic kit:

```bash
mcpp build
bin=target/<triple>/<fingerprint>/bin
$bin/mcppls-conformance run --server $bin/mcppls --fixture conformance/fixtures/inferred \
    --payload editors/vscode/payload            # or --clangd PATH --kit DIR
```

Every contributor command — build, test, debug, package, release — is listed in
[`docs/93-devtools.md`](docs/93-devtools.md).

If you don't have an assembled payload, make one with `mcpp run -p devtools -- payload`
(see [`docs/00-install.md`](docs/00-install.md)), or point `--clangd` at a clangd on your machine
and `--kit` at a kit directory
([`packaging/kits/README.md`](packaging/kits/README.md)) instead of `--payload`.

## VS Code end-to-end suite

```bash
mcpp run -p devtools -- extension          # payload + compiled extension + a .vsix
cd editors/vscode && npm test              # Linux without a display: xvfb-run -a npm test
```

The suite needs the payload and the compiled extension, which the first command produces.
`MCPPLS_E2E_VSIX=<path>` runs it against the packaged `.vsix` rather than the source tree (what CI
does); see the header of `editors/vscode/test/runTest.ts` for every mode.

## Specifications

Changes to `docs/specs/s1`–`s5` must update the spec text, its schema, its examples and the affected
conformance fixtures together. Every normative rule (MUST, SHOULD, ...) carries an identifier
(`S<n>-<section>-<ordinal>`) with evidence recorded in `conformance/traceability.json`. Check both:

```bash
python3 -m venv .venv && .venv/bin/pip install --quiet 'jsonschema>=4.18'
.venv/bin/python docs/specs/tools/validate.py
```

## Versions

The product version lives in `mcpp.toml`; every other site (`modules/base/src/version.cppm`, every editor
plugin's manifest, the clangd and kit versions the server expects) is derived or checked from it, never
edited by hand:

```bash
mcpp run -p devtools -- version --check       # CI runs this (as part of `check all`)
mcpp run -p devtools -- version --set 0.0.2
```

## Code shape

The server is **C++23 named modules only** — no headers, no macros. One module per
`src/<dir>/<name>.cppm`, named `mcppls.<dir>.<name>` (for example `src/orchestrator/workspace.cppm`
is `export module mcppls.orchestrator.workspace;`). Keep new code inside that shape rather than
introducing a header or a macro to work around it.

## Contributions developed with an AI agent

They are welcome, and they are held to the same bar as any other: the change has to prove itself
(above), and CI has to pass on all three platforms.

[`.agents/skills/mcppls-contributing/SKILL.md`](.agents/skills/mcppls-contributing/SKILL.md) is
written for the agent rather than for you — where code belongs, what a change of each kind has to
prove, and the rules that are not negotiable (no headers or macros in `src/`, every normative rule
in `docs/specs/` carries evidence, nothing the server starts may be able to hold it, the version is set
by script). Point your assistant at it:

```
Read .agents/skills/mcppls-contributing/SKILL.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then follow it to help me submit a contribution.
```

Two things an agent gets wrong here more than anywhere else, so check them yourself before
submitting: a fix with no test that fails without it, and a conformance fixture whose checks pass
whether or not the change is present. `.agents/skills/mcppls-usage/SKILL.md` is the other half —
for an agent using mcppls rather than changing it.

## Commit messages

Lowercase `type(scope): a sentence`, where the sentence states what is now true or what failure is
fixed — not an imperative like "add X". Scope names the subsystem (`model`, `engine`, `platform`,
`observability`, `conformance`, `version`, `ai`, `kernel`, `status`, `spec`, ...); `docs` and `chore`
commits often drop it. For example:

```
fix(model): an untrusted workspace does not start from a cached model either — a cache is
the build tool's word, and that is what a workspace nobody trusts does not get

feat(model): the cache is a model, per source and fingerprinted, and clangd is never
started without a database
```

When the change is more than the subject line explains, add a body in full prose paragraphs
describing the failure being fixed, what changed, and why — not a bullet list of file names.
`git log --oneline -40` is the best source of the actual convention; match what is there rather
than a generic style.
