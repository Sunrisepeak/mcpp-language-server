# mcpp fallback formatting review

The upstream configuration is a byte-preserving snapshot from mcpp commit
74bcb859e60afc71ad759540d2de5344dda400e9. Its SHA is recorded in snapshot.json.
The current snapshot parses with clang-format 22; clang 23 parsing and golden
output are pending. This is a review input, not a completed formatting feature.

The maintained engine will register the named `mcpp` C++ preset from this
snapshot. Existing `.clang-format` discovery remains authoritative. The product
may select this fallback only when verified engine metadata declares
`format-style-mcpp`, and only by default for an mcpp project. Explicit user
fallback settings take priority. External or legacy engines receive no unknown
preset, and no configuration file is written into a user's project.

The fixture covers global module fragments, import, constrained templates,
namespaces, lambdas, constructor initializers and long expressions. Final golden
output must be generated and checked with the same clang 23 formatter as the
maintained preset. A named preset and this snapshot must yield identical output;
project overrides and explicit user fallback are separate acceptance cases.
