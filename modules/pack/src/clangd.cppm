// Reducing an official clangd release archive to what the payload ships.
//
// This ports trim_clangd.py. Keeps bin/clangd[.exe], lib/clang/<major>/include (clang's builtin
// headers, which clangd finds relative to its own executable) and LICENSE.TXT; everything else in
// the release -- sanitizer runtimes, lib/clang/<major>/lib and share -- is never used by clangd and
// is dropped by never selecting it in the first place (mcppls.pack.archive does the writing).
export module mcppls.pack.clangd;

import std;
import mcppls.base.error;
import mcppls.pack.lock;

export namespace mcppls::pack::clangd {

struct Options {
    std::string platform;                 // one of the lock's platform names (linux-x64, ...)
    std::string outDirectory;             // replaced if it exists
    std::optional<std::string> zip;       // an already-downloaded release archive; skips fetch/lock
    std::string cacheDirectory;           // where a fetched archive is cached (only used without `zip`)
    bool strip { true };                  // false is trim_clangd.py's --no-strip
};

struct Result {
    std::string directory;                // outDirectory, canonicalized
    std::string binary;                    // <directory>/bin/clangd[.exe]
    std::string clangMajor;               // "23" from lib/clang/23/...
    std::size_t filesKept { 0 };
    std::uint64_t totalBytes { 0 };
    bool stripped { false };              // a strip/thin tool actually ran
    // The first line of `clangd --version`, read only when this host can run the binary it just
    // produced (trim_clangd.py's `host_runs_it`); absent otherwise, never a stale guess.
    std::optional<std::string> versionLine;
};

// linux-x64: stripped with llvm-strip or strip, when one is on PATH (a missing tool is not a
// failure -- the binary just keeps its symbols, as trim_clangd.py leaves it).
// darwin-arm64: the universal binary is thinned to arm64 with lipo, stripped with `strip -x` and
// re-signed ad hoc (modifying a Mach-O invalidates its signature); this needs a macOS host.
// win32-x64: files are selected only, matching trim_clangd.py.
base::Result<Result> trim(const Options& options, const lock::Lock& lockData);

} // namespace mcppls::pack::clangd
