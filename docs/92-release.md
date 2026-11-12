# Making a release

> Releases are published on this repository's GitHub release page. Publisher workflows submit the
> verified archives to Open VSX and JetBrains; public JetBrains availability depends on review.
> The VS Code Marketplace is uploaded to by hand, after the release's
> VSIX files are verified locally; xlings serves `mcppls` through a separately maintained index entry (below).
> [91-naming.md](91-naming.md) has the identifiers each uses.

## Versions

A version is `X.Y.Z`. `X.Y`, the **series**, is shared by the server and every editor plugin (VS Code,
CLion, Zed, the Claude Code plugin); `Z` is each component's own patch. A plugin works with any server of
its series and refuses another, naming both versions and the install that matches: a plugin states its
version in `initializationOptions.plugin`, and the server refuses `initialize` when the series differ;
the VS Code extension and the CLion plugin also check the server they are about to start. The Neovim
plugin and the Copilot CLI fragments have no version of their own and are released with the series.

The server's version lives in `mcpp.toml` and is reported by the binary (`modules/base/src/version.cppm`);
each plugin's lives in its manifest. One command writes or checks every site:

```bash
mcpp run -p devtools -- version --check                  # the server's sites agree, every plugin is of its series
mcpp run -p devtools -- version --print --component all  # one `name=version` line per component
mcpp run -p devtools -- version --series 0.22            # a new series: every component 0.22.0
mcpp run -p devtools -- version --bump vscode            # the VS Code extension's next patch
```

`--check` also compares the clangd and kit versions the server states with the ones the payload is built
from. Three parts is the one shape the VS Code Marketplace, Open VSX, Zed and JetBrains all accept; anything
else, a four-part date version included, is refused. Each component's `Z` only rises, so every marketplace
sees its versions rise, and a version, once published anywhere, is never used again.

| Release | Tag | Carries | CHANGELOG section |
|---|---|---|---|
| a series | `vX.Y.0` | every component at `X.Y.0` | `## [X.Y.0]` |
| a component's patch | `<component>-vX.Y.Z` (`server-`, `vscode-`, `clion-`, `zed-`, `claude-code-`) | that component's files | `## [<component> X.Y.Z]` |

Every release is tested as a whole: the candidate is the whole product, and a component's release publishes
its files out of it, with a MANIFEST.md and SHA256SUMS of their own. Open VSX receives a release that carries
the extension (`v*`, `vscode-v*`), JetBrains one that carries the CLion plugin (`v*`, `clion-v*`). A plugin's
patch carries the newest server patch of its series. The series numbering continues the earlier versions:
0.0.*N* became series 0.*N*, so 0.0.21 was released as 0.21.0.

The versions CI *builds with* are a different thing and live in `.github/versions.env`.

## What a release contains

[`packaging/release.manifest.json`](../packaging/release.manifest.json) declares it: every asset,
what it is, and how a person installs it. That file is not documentation — the release workflow
checks the staged directory against it and refuses to publish when a required asset is missing,
when an asset is empty, or when a file nobody declared is about to be shipped. The release page's
notes are the version's section of `CHANGELOG.md`, under "What changed", followed by the files as
rendered from the same declaration, so the page and the check cannot drift apart. A version with no
`CHANGELOG.md` section is refused before anything is built.

| | Per platform | Required |
|---|---|---|
| `mcppls-<version>.vsix` | no — one for every system | yes |
| `mcppls-payload-<version>.tar.gz` | no — an entry per platform inside | yes |
| `mcppls-zed-<version>.tar.gz` | no | yes |
| `mcppls-clion-<version>.zip` | no — one for every system | yes |
| `mcppls-<version>-openkal.zip` | no — one for every system and architecture | yes |
| `mcppls-<version>.okb` | no — the image of both architectures | yes |
| `mcppls-<version>-<system>-<arch>` (`linux-x86_64`, `linux-aarch64`, `macos-aarch64`, `windows-x86_64.exe`) | one per system and architecture | yes |
| `mcppls-kit-openkal-<platform version>.tar.gz` | no — the kit of every openkal target | yes |
| `mcppls-kit-<target>-<version>.tar.gz` | one per platform's target (`x86_64-linux-gnu`, `aarch64-linux-gnu`, `arm64-apple-darwin`, `x86_64-w64-mingw32`) | yes |
| `SHA256SUMS`, `MANIFEST.md` | no | yes |

To add or remove an asset, edit the manifest; a `*` in a pattern stands for a version the release does
not name (a kit's), and `{kit}` for a platform's kit as `packaging/payload.lock.json` names it. The
workflow needs no change.

**One package for every system** (ecosystem design 2026-10-29 §9). The server is built as an ok image
per architecture (mcpp's `openkal` set, whose platform is openkal-platform) and every file above that
holds a server holds those images: the okb holds both; the openkal tree holds both under `lib/<arch>/`
with the launchers `bin/mcppls` and `bin/mcppls.exe` and the runner of every system it serves; the
payload carries the okb as `bin/mcppls.okb` with those runners under `libexec/okrun/<system>-<arch>/`,
and the VSIX and the CLion plugin carry the payload; a single file is a system's runner with one image
inside. The engine is also one ok image, mcppls-clangd's `clangd.okb`, which each platform's runner starts;
what differs per platform is the runner and the kit of a target, and payload.json's entry for each
platform names its runner and its engine. Native programs are test tools and are not distributed.

| Job (`ci.yml`) | Makes | On |
|---|---|---|
| `build` | every platform's test tools (`stage-<platform>`, release profile); the native server's unit tests, archived (`mcpp test --no-run --archive`), of linux-x64 in both profiles, and of every platform in a run with `native` | Linux |
| `test` | nothing: the archived tests run on their own system (`mcpp test --archive`): linux-x64's, and every platform's in a run with `native` (a manual dispatch of CI) | linux-x64; every platform with `native` |
| `openkal-test` | nothing: the server's unit tests as ok images (`mcpp test --target openkal --no-run --archive`, built by `server`) run on each system through its runner | every platform |
| `server` | `mcppls-<version>-openkal.zip` and `mcppls-<version>.okb` from one build (`mcpp pack --target openkal --mode self-contained --systems all --pkg-format zip,okb`), the tree's `runs_on.self` being every platform; the single files (`--pkg-format single-file --systems all`); the server's unit tests as ok images, archived | Linux |
| `kits` | each platform's target kit, `mcppls-kit-<target>-<version>.tar.gz` | Linux, Linux arm64, macOS |
| `payload` | the openkal kit and the one payload | Linux |
| `vsix` | the one VSIX | Linux |
| `distribution` | nothing: the VSIX carries one okb, the one okb, and this platform's runner starts it; the tree's launcher and the single file start the server | every platform |

## Making one

Everything is one manual run. **Actions → Release → Run workflow**, and give it the version:

| Input | Meaning |
|---|---|
| `version` | e.g. `0.0.2`. The tag `v<version>` is created by the run |
| `draft` | on by default — the release is staged for you to verify before anyone gets it, Open VSX included |
| `prerelease` | on by default; turn it off for a stable release |

Before pressing it:

1. Everything is on `main` and CI is green.
2. `mcpp run -p devtools -- version --series X.Y` (a series) or `--bump <component>` (a patch) is committed. The run checks this first and
   stops before building anything if the manifests disagree — an hour is a long time to wait to
   learn one line is wrong.
3. `CHANGELOG.md` has the entry for it.

The run then runs the pre-release test (below), which re-runs the whole of CI, builds the Zed and
CLion plugins, installs them, measures performance and stability, stages everything, checks it
against the manifest and writes `SHA256SUMS` and `MANIFEST.md`. Only when all of that passes does it
create the tag and publish the staged candidate, file for file.

An existing tag must name the same commit. Existing release assets are not overwritten; an
already published release is sent to an editor registry through its publisher workflow.

A release reaches its users in five steps:

1. The run creates the release as a **draft**.
2. You verify the draft's `mcppls-<version>.vsix` locally ([Verifying a release](#verifying-a-release)).
3. You publish the draft. That triggers `.github/workflows/publish-openvsx.yml`, which puts the same
   files on Open VSX ([Open VSX](#open-vsx)).
4. You upload the same file to the VS Code Marketplace ([below](#the-vs-code-marketplace)).
5. `.github/workflows/publish-jetbrains.yml` uploads the published `mcppls-clion-<version>.zip`
   to the JetBrains Marketplace ([below](#the-jetbrains-marketplace)).

A run with `draft` off skips step 2: the release is published at once, and its `openvsx` and
`jetbrains` jobs invoke the two publisher workflows, since a release created by a workflow
triggers no other workflow.

## The pre-release test

`.github/workflows/prerelease.yml` is a release without the publishing: all of `ci.yml`, then
`.github/workflows/release-checks.yml` on the artifacts that run built. It runs by itself on a
`v<version>-rc<n>` tag and weekly on `main`, and Release runs it first. A pull request that changes
release packaging gets the same release checks from CI itself, after CI's own jobs pass, so CI does
not run twice for it. Its jobs, each on Linux, macOS and Windows unless noted:

| Job | Passes when |
|---|---|
| CI | every job of `ci.yml` passes |
| plugins (Linux) | the Zed extension builds and packages, and the CLion plugin builds one package carrying the payload, with an entry for every platform |
| install | devtools installs the Zed extension and the CLion package; the server Zed falls back to and the server the CLion package carries each answer the `inferred` fixture through this platform's entry; `uninstall` leaves nothing behind |
| performance | the median first navigation of five cold starts is under 12 s and of five warm starts under 5 s, with no failed check |
| performance on a real project (Linux) | the server passes the `self-mcpp` fixture: the mcpp repository at a fixed commit |
| stability | the fixtures of the project shapes that matter (all-`.cppm`, `.cppm`/`.cpp` split, watched edits, a broken module, a hung build tool, several roots) pass three rounds in a row |
| release candidate | every asset is present and checked against the manifest; the result is the `release-candidate` artifact |

`ci.yml` itself also gates a change on the user's editor and platform, beyond those fixtures:

| Job | Passes when |
|---|---|
| `code-oss-e2e` | the extension's end-to-end suite passes in Code - OSS (VSCodium, the version pinned as `VSCODIUM_VERSION` in `.github/versions.env`) |
| `proot` | the server passes its checks under termux's PRoot, in both modes, on x64 and arm64 (the environment of issue #32) |
| `ux` | the user-experience scenarios stay within their response-time budgets, on pinned checkouts of mcpp and xlings (fixtures `ux-mcpp` and `ux-xlings`) |

The nightly run adds a `termux` job: termux's own userland and PRoot, with a Debian installed by
`proot-distro`. It is a second opinion and does not fail the nightly; no real Android device is in CI.

The medians and per-round timings are in each run's summary. To try a candidate by hand, download
`release-candidate` from the run: it is exactly the set of files a release would carry.

Pushing a `v*` tag by hand does the same thing, for anyone who prefers tagging first.

## A new engine

The bundled clangd is mcppls-clangd, released from its own repository and pinned here in
`packaging/payload.lock.json`. A change to both goes in this order, so that what the engine
releases has already passed this repository's CI with it, and the release brings no surprise:

1. **The engine PR's CI builds the packaged part**, the ok image `clangd-openkal` (`clangd.okb`, with its
   `engine.json`), proved like a release's. The engine is released only as this image; its native builds
   are test and comparison builds.
2. **This repository's PR tests with that build before the engine is merged.** Commit
   `packaging/engine-candidate.json`, naming the engine PR's run:

   ```json
   { "repository": "Sunrisepeak/mcppls-clangd", "run-id": 38052537482,
     "engine-commit": "<the engine PR head it was built from>" }
   ```

   Server compilation can run while the upstream builds finish. The `engine candidate parts` job
   admits payload assembly only after the images, the `clangd-openkal` part and its identity check pass, with
   matching run/head metadata and immutable artifact digests; known upstream failures or
   cancellations refuse admission.

   CI's `payload` job then assembles the payload from that run's parts instead of the lock's
   archives (`payload --clangd <platform>=<part> ... --candidate`: a fork version other than the
   lock's is taken for the same LLVM base, its identity still checked), and every later job tests it. The job
   prints the engine the payload carries and stops when the run's artifacts hold no part, or when the payload's
   engine is not the candidate's image: a candidate that is named is tested, or the run is red. Fix what it
   finds in either PR, and point the file at the new run, until both are green.
3. **Merge and release the engine**, then switch the lock to the released archives (their size and
   SHA-256, each checked against two downloads and the published `.sha256`) and **delete the
   candidate file** in the same commit. CI runs once more on the released bytes.
4. Merge this PR and release.

A candidate file never reaches a release: CI refuses it on `main` and in a release's run, and the
Release workflow stops at once if it is there. A manual run of CI or of the nightly on a branch other
than `main` may use it: the nightly's ux rounds are where an engine change to idle memory or typing
latency shows, so they see the engine before it is released. Artifacts are kept 14 days; an older run
is rebuilt by re-running the engine PR's CI.

### Engine performance

The engine's warm-prime gate is the median of four alternating native/image pairs, measured in a
comparison run of mcppls-clangd (its `ci/performance-policy.md`); its release names that run. The
one-release exception of engine 0.0.20 has expired. The
[performance issue](https://github.com/Sunrisepeak/mcpp-language-server/issues/77) records the measured
limits, including the Windows cold-readiness tradeoff.

## Measuring a change

What the nightly measures varies from run to run on the same bytes: a slower runner, a busier hour.
One red round is not a regression, and one green round is not a fix. A manual nightly
(`gh workflow run nightly.yml --ref <branch> -f …`) measures a change without building it again:

| Input | What it does |
|---|---|
| `payload-run` | Tests the payload, test tools and kits a CI run of the branch built (its run id) instead of building them again: about 35 minutes instead of 80, on the bytes the pull request's CI tested |
| `scope` | `all` (every job) or `ux` (only the ux rounds, their budgets and the comparison) |
| `ux-repeat` | Each round runs its edits stage (typing, saving, U5, U6, U7a, U15) this many times, each in a new server session on what the cold and warm stages prepared: 4 over the three rounds is 12 samples of each, built once |
| `ux-baseline` | A release (`v0.0.16`) whose server runs the same stages on the same machines, in its own workspace and cache, alternating with this one (A B B A); `ux budgets` then prints `mcppls-devtools measure compare`, the typing, save, state-change, idle and timeout numbers side by side, with those whose median is worse than the baseline's beyond their spread marked |

The runner, the fixtures and the budgets are the branch's for both servers, so only the server differs.

## Verifying a release

Do not trust what the build said; recompute it from what the public can actually download.

| Check | How |
|---|---|
| Every expected asset is on the release | Against `MANIFEST.md`, which the release itself carries |
| Each asset's hash | **Download it again**, hash it, compare against `SHA256SUMS` |
| The VSIX installs and works | On a machine, in VS Code, opening a real C++ modules project |
| The payload works without VS Code | Unpack it, `mcppls serve` over stdio from another editor |
| A real upgrade works | On a machine that already had the previous version, not only a clean one |

Record the result on the release's tracking issue, including what failed and what had to be
finished by hand. It is how the next person finds out what actually happens.

## Open VSX

Open VSX is where VS Code-compatible editors (Cursor, VSCodium, Windsurf, Trae, ...) find
extensions. `.github/workflows/publish-openvsx.yml` publishes the release's `mcppls-<version>.vsix`
to the namespace `sunrisepeak` when the release is published: from the UI (`release: published`), or
from Release's `openvsx` job when the run published it directly. It downloads the file from the
release, checks it against `SHA256SUMS`, checks that the token may publish to the namespace, then
publishes it; the VSIX names no target platform, so Open VSX lists it as universal. Afterwards it
downloads the file back from Open VSX and compares its hash with `SHA256SUMS`; the job fails when Open
VSX serves something else. A GitHub pre-release is published as
an ordinary version.

A version Open VSX already has is skipped (`--skip-duplicate`), so re-running is safe: **Actions →
Publish to Open VSX → Run workflow** with the tag publishes only what is missing.

The token is the repository secret `OVSX_PAT`, an access token of the Open VSX account that owns
the namespace (its Eclipse account has signed the Open VSX Publisher Agreement). When it expires,
generate a new one under *Settings → Access Tokens* on open-vsx.org and replace the secret; the
workflow's *The token may publish to the namespace* step is what fails first when it has not been.

## The VS Code Marketplace

Uploaded by hand, after the local verification: the published release's `mcppls-<version>.vsix` goes
to the publisher `sunrisepeak`, by `npx @vscode/vsce publish --packagePath <the file>` or on the
publisher's management page with *Update*. A Marketplace listing that had a file per platform
(0.0.19 and earlier) is replaced by the one universal file. A version is published once: the Marketplace takes only a version higher than every one
it has had.

## The JetBrains Marketplace

The repository secret `JETBRAINS_TOKEN` authorizes `.github/workflows/publish-jetbrains.yml`.
Publishing a draft triggers it; a release created already published invokes it from `release.yml`.
It downloads the existing `mcppls-clion-<version>.zip`, checks `SHA256SUMS`, the plugin XML ID and
version, then uploads that archive through the Marketplace update API. The workflow retains the
platform receipt, including the submitted update ID and approval status. Public availability follows
Marketplace approval.

The plugin `io.github.sunrisepeak.mcppls` (vendor Sunrisepeak) carries one package for every system.
Version 0.0.19 used a package per system, selected by `com.intellij.modules.os.*` and
`com.intellij.modules.arch.*` dependencies; the universal package declares neither. `since-build`
stays 261. A new plugin is registered manually before automated updates can use its XML ID.

## The xlings index

The xlings index carries the server as `mcppls` (`pkgs/m/mcppls.lua` in
openxlings/xim-pkgindex). Its full payload archive includes the server, engine and semantic kits.
The entry and its regional mirrors are updated through a separate index pull request; the release
workflow does not open that pull request.

`mcppls-devtools release xlings --release DIR --out DIR` renders proposed descriptors from the
self-contained OpenKAL tree and kit archives. Its server output is named `mcpp-language-server.lua`,
and it also emits `mcppls-kit-<target>.lua` and `mcppls-kit-openkal.lua`. These outputs require their
own index review. Standalone kits are distributed as release archives; standalone kit entries are
not currently present in the published index.

The VS Code Marketplace receives its archive through the manual upload after local verification
described above.
