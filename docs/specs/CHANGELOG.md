# Changelog

Changes to the specifications in this directory. Each specification is versioned independently.

## 2026-10-01 — S3: how a download the client asked for ended

`CxxModulesStatusParams` gains `onlineRun` (optional): the outcome (`fetched` or `failed`), a sentence
for the person, and when it ended, for the last description of the root that `mcppls.describeOnline`
asked for (S3-4-26). A client tells each run once without blocking (S3-4-27), and may fetch every
offered download of a workspace without asking once the person has said so, with a way back
(S3-4-28). Additive: protocol version stays 1.

## 2026-09-30 — S3: a bundle for what cannot be recovered from, and resetting a root's cache

`CxxModulesIssue` gains `bundle` (optional): the absolute path of a diagnostic bundle the server wrote
by itself when an issue it cannot recover from without the person first appeared (S3-4-22). The bundle
is redacted like a report and never leaves the machine (S3-4-23); a server keeps only the newest few
(S3-4-24); a client offers, once and without blocking, to report the problem with the file attached, to
restart, and to leave the server off for the workspace (S3-4-25). The issue codes `engine-crash-loop`,
`engine-start-failed`, `preparation-stalled` and `payload-corrupt` are named. Section 5.6 adds the
command `mcppls.resetCache` (S3-5.6-1 to S3-5.6-3), and `cxxModules/report`'s `server` carries
`sandbox` and `eventLoop`. All additive: protocol version stays 1.

## 2026-09-27 — S1 0.3.0: generated files

A set's `ide` object gains `generated` (optional, section 7.2): the files and directories that the
build generates and that the set's units compile or include, each with the path this document names
(`path`), the path the build of the same configuration writes (`build-path`), its `kind` (`source`,
`header` or `directory`) and, for a file, the step that writes it (`generator`: `id`, `inputs`,
`arguments`, `work-directory`). A consumer does not report a reference to a generated file that does
not exist as an error in the referring source; it may read the file at `build-path` read-only, and it
runs a step only with its user's consent (S1-7.2-10 to S1-7.2-13). A producer that plans without
building, such as `mcpp emit build-database`, plans in a directory of its own; before this, a header a
rule generates was missing there, and every source that included it lost its semantics
(mcpp-community/mcpp#724). Additive: a 0.2.0 consumer ignores the field (S1-11.2-1). mcpp writes it
from mcpp-community/mcpp's release that closes #724.

## 2026-09-27 — S3: a download the client may offer, and two more build systems

`CxxModulesIssue` gains `askOnline` (optional): on a `producer-needs-download` issue, the server will
describe the project once with the network when the client asks with `workspace/executeCommand`
`mcppls.describeOnline` (S3-4-16). Until then, and while the download runs, the server serves the root
from what it has and reaches no network on its own (S3-4-17, S3-4-18); a client that offers the
download never blocks on the question, asks at most once per root and set of missing things, and
ignores an answer that comes after the need is gone (S3-4-19 to S3-4-21). `project.source` gains
`xmake` and `meson` (tier 3), the issue codes `producer-online`, `generated-files-missing` and
`implementation-unreadable` are named, and `cxxModules/report` carries `settings`. All additive: protocol version stays 1.

## 2026-09-26 — S3: the standard a profile reads with

`SemanticProfile` gains `standard` (optional): the C++ standard the context's module units are read
with, for example `"c++26"`. A BMI can only be imported under the standard it was built with, so a
server reads the module units of one context with one standard; this field says which. Additive:
protocol version stays 1.

## 2026-09-26 — S3: completion of module syntax

Added section 6.2. A server may make a space a completion trigger character, for the module names
after `import`; if it does, a space-triggered request anywhere but right after `import ` or
`export import ` is answered at once, empty, without the semantic engine (S3-6.2-1), and the space is
advertised only to a client that asks for it (`initializationOptions.completion.triggerOnSpace`) or
that the server knows drops the other spaces itself; never to one that declared `false` (S3-6.2-2,
S3-6.2-3). Servers offer the module-syntax keywords where each can begin a declaration, merged with
the semantic engine's result, and without it when it does not answer in time (S3-6.2-4, S3-6.2-5).
All additive: protocol version stays 1.

## 2026-09-26 — S3: a report names no one

`cxxModules/report` takes `redact` (optional, default `true`): a server replaces the user's home
directory, the user's and the machine's names and recognizable secrets in its report with
placeholders, the same placeholder for the same original, and keeps the project's own paths
(S3-5.5-3). `redact: false` gets the report as before. Additive: protocol version stays 1.

## 2026-09-26 — S2 0.3.0: a partial single-document answer

In single-document mode a document may carry `data` together with `error` diagnostics. It describes
everything except what those diagnostics name; the producer names each part it could not describe
(a workspace member or a package) in the diagnostic's new optional `path` field, and a consumer uses
the rest of the document (S2-3.4-12, S2-3.4-13). `data` is present when the command described the
workspace in whole or in part; a command without `data` has still failed (S2-3.4-11). A consumer
written for 0.2.0 reads such a document as a success with errors, which is the intended reading.
mcpp implements it with mcpp-community/mcpp#699 (in #702); before, one workspace member's planning
failure used to remove every member's sets.

## 2026-09-25 — S3: issue categories, the degraded hold, module syntax in semantic tokens

Added `category` (optional) to `CxxModulesIssue`: `code`, `engine`, `environment` or `project`,
whose problem an issue is. Issues of category `code` — the user's own source being wrong — never
make a root `degraded` or `error`; they are reported as diagnostics where they are, and a client
treats an issue without a category as before (S3-4-10 to S3-4-14). A change to `degraded` is held
back until it has lasted a short interval, so a condition that passes by itself never reaches a
client (S3-4-15). Listed `modules-doomed` among the codes.

Added section 6.1: servers add semantic tokens for module syntax (keywords as `keyword`, module
names as `module` for clients that declare `initializationOptions.semanticTokens.moduleType`, else
`namespace`), and `initializationOptions.semanticTokens.modules: false` turns them off (S3-6.1-1
to S3-6.1-3). All additive: protocol version stays 1.

## 2026-09-24 — S3: issue `engine-incompatible`

Listed `engine-incompatible` among `CxxModulesIssue` codes: the core engine cannot run on this
machine at all, because the system's program loader refused it (a shared library, or a version of
one, it needs is missing). Unlike `engine-crashed`, nothing restarts it, and the root's state is
`error` with module-level features only. Informative only: the code set was already open
(`| string`), so protocol version stays 1.

## 2026-09-22 — S3: `project.tier`

Added `project.tier` (optional) to `cxxModules/status`: which kind of source described the project
(the README's and design §2.1's L1..L4 — mcpp or an explicit build database, CMake's own database, a
bare `compile_commands.json`, sources only), independent of `project.level` (S1's own document
conformance number, which answers a different question and happened to share the same 1..4 range).
An incident against a real project showed the confusion this caused directly: a plain project's
`level 2` and an mcpp project's `level 3` read, to someone who did not track the difference between
the two numbers, as if the second were "more done" than the first in the sense the README's L1..L4
promises, when in fact both could be any tier. This is a backward-compatible, additive change (S3-4-8,
S3-4-9): protocol version stays 1, and a client from before it existed sees no field and nothing else
changes.

## 2026-09-22 — first publication

All five are published as drafts, at these versions:

| Spec | Version |
|---|---|
| S1 — C++ Build Database: IDE Profile | profile-version 0.2.0 |
| S2 — Build Database Discovery Protocol | 0.2.0 |
| S3 — Language Server Protocol Extensions for C++ Modules | protocol version 1 |
| S4 — Semantic Kit | kit-version 1 |
| S5 — Semantic Queries for C++ Code | 0.1.0 |

Every requirement carries a rule identifier (`S<n>-<section>-<ordinal>`), and
`conformance/traceability.json` maps each one to its evidence: a test, a conformance check, a schema
validation or a line of code.
