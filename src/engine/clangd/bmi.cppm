// What clangd's module cache calls its files, and what that means.
//
// clangd's module cache holds two names for one module: `<module>.pcm` and, sometimes,
// `<module>-<YYYYMMDD>-<HHMMSS>-<serial>.pcm` beside it. Counting `*.pcm` then reports twice as
// many modules as exist — the number that reaches a bug report.
//
// Both names are written by clangd. mcppls writes no BMI at all (checked: nothing under src/
// writes, copies or links a `.pcm`), and the doubling is not universal — measured 2026-09-17,
// this repository had 64 files for 64 modules and `xlings` 222 for 111. What decides it is not
// known yet.
//
// This module owns the naming rule so counting and reporting read it from one place.
export module mcppls.engine.clangd.bmi;

import std;
export namespace mcppls::engine::clangd {

// The module a BMI file belongs to: `xlings.core.utf8-20260917-014433-869144.pcm`,
// `xlings.core.utf8-20260917-014433-869144.-3fa9c1.pcm` (a copy under a read lease, mcppls-clangd
// 0.0.12 on) and `xlings.core.utf8.pcm` all answer `xlings.core.utf8`. A name that does not carry a
// stamp is returned unchanged, extension removed.
std::string module_of_bmi(std::string_view fileName);

// C-7 (plan 2026-10-03): whether the file `fileName` in `directory` is one of clangd's copy-on-read
// copies. Two halves, both needed: the name ends in the `-YYYYMMDD-HHMMSS-<serial>` stamp that
// `module_of_bmi` strips, AND the canonical BMI it was taken from sits beside it -- clangd always
// copies beside its source. The second half is what keeps a module genuinely named like a stamp
// (`foo-20260101-120000-1`, no `foo.pcm` beside it) from being swept as a copy: deleting it would
// be deleting a published BMI, and a sweep never does that (D2). Report, sweep and budget all ask
// this one predicate, so what the report counts is exactly what a sweep removes.
bool is_versioned_copy(std::string_view fileName, std::string_view directory);

// The directory of the module unit a published BMI belongs to, `<modules>/<unit>-<hash>`: two
// levels above `<command hash>/<module>.pcm`, or three for a module built against prerequisite
// producers (`<command hash>/producer-chain-v2-<hash>/<module>.pcm`, mcppls-clangd 0.0.13 on).
std::string unit_directory_of_bmi(std::string_view path);

// The name clangd gives a module unit's lock under `<modules>/.locks` (and its `<unit>-<name>`
// directory): the digest of the unit's path, dots removed and folded to lower case where the file
// system ignores case, as clangd's FileDigest (XXH3-64, least significant byte first) in upper-case
// hexadecimal. A unit published to the owned cache has no directory of its own, and its lock is
// found by this name.
std::string unit_lock_name(std::string_view sourcePath);

} // namespace mcppls::engine::clangd
