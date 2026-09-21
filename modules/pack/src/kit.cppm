// Assembling an `mcppls-kit` semantic kit (S4, docs/specs/s4-semantic-kit.md) for one platform.
//
// A kit is data only: the standard library headers, the `std` module sources, a P3286-shaped
// module manifest, C library headers where they may be shipped, the upstream license texts and
// kit.json. clangd needs nothing else to give full module semantics on a machine without a
// compiler.
//
// Ported line for line from packaging/scripts/build_kit.py (tooling architecture §4): the two
// recipes it chose by payload.lock.json --
//
//   libcxx-source  LLVM's runtimes build from llvm-project-<v>.src.tar.xz, configured for libc++
//                  and libc++abi and installed as headers and module sources only (nothing is
//                  compiled). Needs cmake, ninja and a working C/C++ compiler for the configure
//                  checks. linux-x64 adds glibc and Linux kernel headers from the host as the
//                  sysroot; darwin-arm64 declares that it requires the macOS SDK instead, which
//                  Apple's license does not allow a kit to carry.
//   llvm-mingw     generic-w64-mingw32/include (without *.idl, *.tlb, *.def), share/libc++/v1 and
//                  the x86_64-w64-mingw32 module manifest, extracted from the llvm-mingw release
//                  built on the same LLVM.
//
// This lives in mcppls-pack, not the server: it runs cmake, ninja and a compiler as external
// programs (mcppls.platform.toolrun), and only mcppls-devtools depends on this package at all
// (modules/pack/mcpp.toml).
export module mcppls.pack.kit;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::pack::kit {

// Kept in step with mcppls.spec.kit's KIT_VERSION (src/spec/kit.cppm), which reads what this
// writes; `mcppls-devtools version --check` compares the two. It is redeclared here, not
// imported, because the boundary this package exists for (tooling architecture §3.1: pack is depended on only by devtools) runs the
// other way from spec: the server must not import anything from mcppls-pack, and mcppls-pack must
// not import server code either, so a `mcpp test -p pack` never risks the server domain.
inline constexpr int KIT_VERSION { 1 };

inline constexpr std::array<std::string_view, 3> PLATFORMS { "linux-x64", "win32-x64", "darwin-arm64" };

// Everything build_kit.py took from argv, plus `lockPath` (which the script found next to itself;
// a library takes it explicitly because it does not know where the repository root is).
struct Options {
    std::string platform;                      // one of PLATFORMS
    std::string outDir;                        // kit directory to create (replaced if it exists)
    std::string cacheDir;                      // download cache (packaging/scripts/fetch.py's DEFAULT_CACHE, here chosen by the caller)
    std::string lockPath;                      // packaging/payload.lock.json
    std::string workDir;                       // scratch directory for sources and the configure tree; empty creates and removes a temporary one
    std::string sourceArchive;                 // --source: use this archive instead of fetching the lock entry
    std::string sysrootIncludeDir;             // --sysroot-include: linux-x64 only, C library headers instead of the host's dpkg packages
    std::vector<std::string> sysrootLicenses;  // --sysroot-license: license file(s) for --sysroot-include (repeatable)
    // --jobs: parallelism given to ninja's install step. build_kit.py's docstring advertised this
    // option but its argparse never defined it, so the Python build always ran ninja without -j
    // (ninja then makes its own default choice). This port makes the advertised option real; the
    // parity check that matters is the default (no --jobs on either side), which is unaffected.
    std::optional<int> jobs;
};

struct BuiltKit {
    std::string kitDir;
    std::string name;
    std::uint64_t fileCount { 0 };
    std::uint64_t totalBytes { 0 };
    // `ordered_json`, not `json`: build_kit.py writes kit.json with `json.dump(data, indent=2)`,
    // which keeps a Python dict's insertion order. Plain nlohmann::json is a std::map underneath
    // and would alphabetize the keys on dump, which the parity check (identical sha256 per file,
    // tooling architecture M2) would then fail on -- the content would be the same JSON, but not
    // the same bytes.
    nlohmann::ordered_json manifest;   // exactly what was written to kit.json
};

// Runs the recipe payload.lock.json names for `options.platform` and writes the kit. Every check
// build_kit.py made is made here too: an unresolvable path in the module manifest, a missing
// include directory, sysroot or license file, a manifest with no `std` module, and the two
// host-platform guards (darwin-arm64 needs a macOS host, linux-x64 takes its C library headers
// from a Linux host) all fail this the same way build_kit.py raised SystemExit for them.
base::Result<BuiltKit> build(const Options& options);

// ---- pieces exposed for whitebox testing (mcpp test -p pack, modules/pack/tests/test_kit.cpp) --
//
// None of these run a process or reach the network, so a test can call them directly rather than
// through a fixture that waits on cmake and ninja: the archive selections build_kit.py's two
// recipes used (SOURCE_SUBTREES's prefix test and the llvm-mingw recipe's `select`), the argv
// each recipe built for cmake and ninja, and the manifest and kit.json checks write_kit_json and
// check_manifest made. Precedent for exporting a module's internals this way is
// mcppls.platform.process's `match_preopen` and mcppls.platform.toolenv's merge and marker
// helpers, both "exposed for tests" the same way.
namespace testing {

// build_kit.py's SOURCE_SUBTREES prefix test (the libcxx-source recipe's archive selection).
bool wants_source_subtree(std::string_view relativePath);

// build_kit.py's llvm-mingw recipe's `select()`, exactly.
bool wants_mingw_member(std::string_view relativePath);

// fnmatch.fnmatch's '*' / '?' subset -- all three of MINGW_EXCLUDED's patterns ever needed.
bool glob_match(std::string_view name, std::string_view pattern);

// What recipe_libcxx_source's `configure` list needed, once the tools and directories were
// already resolved -- everything after cmake itself, which run_tool takes as the program to run.
struct ConfigureInputs {
    std::string platform;    // options.platform: chooses the per-target-runtime-dir vs. OSX_ARCHITECTURES tail
    std::string target;
    std::string runtimesDir;   // <source>/runtimes
    std::string buildDir;
    std::string stageDir;
    std::string ninja;         // resolved path, for -DCMAKE_MAKE_PROGRAM
    std::string cc;
    std::string cxx;
};
std::vector<std::string> configure_arguments(const ConfigureInputs& inputs);

// The ninja install argv: `-C buildDir [-j jobs] install-cxx-headers install-cxxabi-headers
// install-cxx-modules`. build_kit.py's own ninja call never had `-j` (see Options::jobs).
std::vector<std::string> install_arguments(const std::string& buildDir, std::optional<int> jobs);

// build_kit.py's write_kit_json: every include directory, the sysroot and every license must
// exist under kitDir, and the module manifest named by data["stdlib"]["module-metadata"] must
// resolve inside the kit (relocating a path that does not to share/libc++/v1 when one of the same
// name is there) and provide a `std` module. Writes kit.json on success.
base::Result<void> write_kit_json(const std::string& kitDir, nlohmann::ordered_json& data);

} // namespace testing

} // namespace mcppls::pack::kit
