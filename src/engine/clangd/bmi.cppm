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
import mcppls.platform.fs;

export namespace mcppls::engine::clangd {

// The module a BMI file belongs to: `xlings.core.utf8-20260917-014433-869144.pcm` and
// `xlings.core.utf8.pcm` both answer `xlings.core.utf8`. A name that does not carry a stamp is
// returned unchanged, extension removed.
std::string module_of_bmi(std::string_view fileName);

// C-7 (plan 2026-10-03): whether the file `fileName` in `directory` is one of clangd's copy-on-read
// copies. Two halves, both needed: the name ends in the `-YYYYMMDD-HHMMSS-<serial>` stamp that
// `module_of_bmi` strips, AND the canonical BMI it was taken from sits beside it -- clangd always
// copies beside its source. The second half is what keeps a module genuinely named like a stamp
// (`foo-20260101-120000-1`, no `foo.pcm` beside it) from being swept as a copy: deleting it would
// be deleting a published BMI, and a sweep never does that (D2). Report, sweep and budget all ask
// this one predicate, so what the report counts is exactly what a sweep removes.
bool is_versioned_copy(std::string_view fileName, std::string_view directory);

} // namespace mcppls::engine::clangd
