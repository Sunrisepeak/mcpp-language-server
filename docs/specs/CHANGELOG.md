# Changelog

Changes to the specifications in this directory. Each specification is versioned independently.

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
