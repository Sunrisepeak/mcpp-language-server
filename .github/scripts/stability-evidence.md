# Per-round stability evidence

Stock-payload Windows run 37772700872, job 113308243293, failed round 1
`module-faults` STRESS1 after 180.7s: maximum stall 59.0776s exceeded the
existing 45s budget; documentSymbol reached 60.009s and final state was
`preparing`. The job printed a server-log tail but deleted the scratch
workspace/cache. This evidence change does not attribute or repair that
runtime failure.

The release stability job still runs the same fixtures, three rounds, polling
variant, assertions and 180s timeout. Each invocation gets distinct owned
workspace/cache paths, `--keep` and the existing `--measure` option. A wrapper
collects evidence after conformance returns, preserves its exit code and
removes only its newly created scratch in `finally`. No cache is shared across
rounds. Cleanup failure is diagnostic and cannot turn a runner failure into a
pass or change its verdict. Collection does not extend a conformance check's
deadline. The wallclock table explicitly includes runner **and diagnostics**;
actual check durations remain in the measurement.

Always-upload artifacts contain only bounded text, never scratch directories,
BMI files or payload binaries:

- At most eight direct regular cache logs, each with a 128KiB prefix/tail
  allowance plus an omission marker (upper bound 128KiB + 32 bytes), using the
  existing timing collector. No log-directory symlink is followed.
- At most eight `compile_commands.json` / `build_database.json` excerpts,
  each at most 128KiB, from workspace/cache only. The breadth-first walk
  examines at most 512 entries, 64 directories and six levels, with a five
  second deadline checked between operations. Known binary/cache subtrees are
  excluded; symlink/reparse components are rejected. This is an operation
  time budget, not a hard bound on an individual OS filesystem call.
- Measurement and manifest are each at most 1MiB. CDB/measurement truncation is
  explicitly recorded with original size, metadata, prefix/tail offsets and
  retained hash; a truncated `.json` is not complete JSON. Walk stop reason,
  limit reached and collection errors explicitly mark incomplete evidence.

The maximum retained text per round is below 4MiB plus 256 bytes of log
markers. The manifest records the command, paths, runner exit and binary
identities; binary hashes are computed without copying binaries. Scratch is
not uploaded, including if a fixture creates BMI files or external links.
Missing measurements still leave a manifest and the original runner verdict.
A collector cannot survive a whole job/process termination; always-upload
preserves whatever evidence already exists without calling an interrupted
round a pass.

Local controls exercised success, exit 7 failure, exit 9 missing measurement,
log-tail retention, oversized CDB truncation, external symlink/BMI exclusion,
owned scratch cleanup and the final-directory 512-entry exhaustion edge.
Python/YAML checks and the existing script inventory check pass. These are
collector controls, not native Windows runtime qualification.
