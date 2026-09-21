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

// The module a BMI file belongs to: `xlings.core.utf8-20260917-014433-869144.pcm` and
// `xlings.core.utf8.pcm` both answer `xlings.core.utf8`. A name that does not carry a stamp is
// returned unchanged, extension removed.
std::string module_of_bmi(std::string_view fileName);

} // namespace mcppls::engine::clangd
