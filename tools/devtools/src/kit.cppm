// Semantic kits (spec S4) by themselves: the openkal kit a payload carries, and the target kits
// published beside it (`mcppls-kit-<target>`, ecosystem design 2026-10-29 §9.2).
//
//   mcppls-devtools kit --openkal --out DIR [--archive DIR] [--compile-commands <target>=FILE ...]
//   mcppls-devtools kit --platform linux-x64|linux-arm64|win32-x64|darwin-arm64 --out DIR [--archive DIR]
//                       [--cache DIR] [--work DIR] [--jobs N]
//   mcppls-devtools kit --verify DIR
//
// This is the CLI shell around mcppls.pack.openkal and mcppls.pack.kit: argument parsing, finding
// the repository and its lock file, and reporting the result. Everything that copies headers, runs
// cmake, ninja or a compiler lives in the library (tooling architecture §4), so `devtools payload`
// calls the same functions rather than shelling out to this command.
export module mcppls.devtools.kit;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;

export namespace mcppls::devtools {

// The openkal kit of this repository's platform, written to `out`: mcpp's build database of each
// openkal target (`mcpp emit build-database --spec compile-commands --target <t>`) names what it holds.
base::Result<std::string> openkal_kit(const std::string& root, const std::string& out);

// A kit as the release and xlings carry it: `<outDir>/<name>.tar.gz`, whose one top directory is
// `<name>` holding kit.json. Returns the archive's path.
base::Result<std::string> kit_archive(const std::string& kitDir, const std::string& name, const std::string& outDir);

mcpplibs::cmdline::App kit_command(bool& handled, int& status);

} // namespace mcppls::devtools
