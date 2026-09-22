# Making a release

> Releases are published on this repository's GitHub release page. The VS Code Marketplace, Open VSX
> and the xlings index are separate channels with their own credentials; the Marketplace is published
> to by hand (below), the other two not yet. [91-naming.md](91-naming.md) has the identifiers each
> uses.

## Versions

The product's version lives in `mcpp.toml` and nowhere else. Everything derived from it is written
or checked by one command:

```bash
mcpp run -p devtools -- version --check          # every site agrees
mcpp run -p devtools -- version --set 0.0.2
```

It keeps these in step: `mcpp.toml`, the constant the running binary reports
(`modules/base/src/version.cppm`), the version of every editor plugin (VS Code, Zed, CLion and the
Claude Code plugin with its marketplace entry), and the clangd and kit versions the server states
against the ones the payload is actually built from.

The version is a three-part semantic version, `MAJOR.MINOR.PATCH`, starting at `0.0.1`, and every
plugin carries it unchanged — three parts is the one shape the VS Code Marketplace, Open VSX, Zed
and JetBrains all accept. `version --set` refuses anything else, a four-part date version included.
A marketplace only takes a version higher than every one it has seen, so a version, once published
anywhere, is never used again.

The versions CI *builds with* are a different thing and live in `.github/versions.env`.

## What a release contains

[`packaging/release.manifest.json`](../packaging/release.manifest.json) declares it: every asset,
what it is, and how a person installs it. That file is not documentation — the release workflow
checks the staged directory against it and refuses to publish when a required asset is missing,
when an asset is empty, or when a file nobody declared is about to be shipped. The release page's
notes are rendered from the same declaration, so the page and the check cannot drift apart.

| | Per platform | Required |
|---|---|---|
| `mcppls-<platform>.vsix` | yes | yes |
| `payload-<platform>.tar.gz` | yes | yes |
| `mcppls-zed-<version>.tar.gz` | no | yes |
| `mcppls-clion-<version>.zip` | no | yes |
| `SHA256SUMS`, `MANIFEST.md` | no | yes |

To add or remove an asset, edit the manifest. The workflow needs no change.

## Making one

Everything is one manual run. **Actions → Release → Run workflow**, and give it the version:

| Input | Meaning |
|---|---|
| `version` | e.g. `0.0.2`. The tag `v<version>` is created by the run |
| `draft` | on by default — the release is staged for you to look at before anyone sees it |
| `prerelease` | on by default; turn it off for a stable release |

Before pressing it:

1. Everything is on `main` and CI is green.
2. `mcpp run -p devtools -- version --set <version>` is committed. The run checks this first and
   stops before building anything if the manifests disagree — an hour is a long time to wait to
   learn one line is wrong.
3. `CHANGELOG.md` has the entry for it.

The run then runs the pre-release test (below), which re-runs the whole of CI, builds the Zed and
CLion plugins, installs them, measures performance and stability, stages everything, checks it
against the manifest and writes `SHA256SUMS` and `MANIFEST.md`. Only when all of that passes does it
create the tag and publish the staged candidate, file for file.

## The pre-release test

`.github/workflows/prerelease.yml` is a release without the publishing: all of `ci.yml`, then
`.github/workflows/release-checks.yml` on the artifacts that run built. It runs by itself on a
`v<version>-rc<n>` tag and weekly on `main`, and Release runs it first. A pull request that changes
release packaging gets the same release checks from CI itself, after CI's own jobs pass, so CI does
not run twice for it. Its jobs, each on Linux, macOS and Windows unless noted:

| Job | Passes when |
|---|---|
| CI | every job of `ci.yml` passes |
| plugins (Linux) | the Zed and CLion plugins build and package |
| install | devtools installs both plugins from the release archives, the installed server answers the `inferred` fixture, and `uninstall` leaves nothing behind |
| performance | the median first navigation of five cold starts is under 12 s and of five warm starts under 5 s, with no failed check |
| performance on a real project (Linux) | the server passes the `self-mcpp` fixture: the mcpp repository at a fixed commit |
| stability | the fixtures of the project shapes that matter (all-`.cppm`, `.cppm`/`.cpp` split, watched edits, a broken module, a hung build tool, several roots) pass three rounds in a row |
| release candidate | every asset is present and checked against the manifest; the result is the `release-candidate` artifact |

The medians and per-round timings are in each run's summary. To try a candidate by hand, download
`release-candidate` from the run: it is exactly the set of files a release would carry.

Pushing a `v*` tag by hand does the same thing, for anyone who prefers tagging first.

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

## The VS Code Marketplace

Not part of the workflow yet: after the release is published, the three `mcppls-<platform>.vsix`
files are uploaded to the publisher `sunrisepeak`, by `npx @vscode/vsce publish --packagePath
<the three files>` or one at a time on the publisher's management page (the first as a new
extension, the others with *Update*). A version is published once: the Marketplace takes only a
version higher than every one it has had.

## Not here yet

Open VSX and the xlings index each need their own credentials and a publish step in the release
workflow, and so would publishing to the Marketplace from the workflow. `mcppls-devtools release xlings` already produces the xlings package
descriptors; the index pull request and the other two channels are still to be added.
