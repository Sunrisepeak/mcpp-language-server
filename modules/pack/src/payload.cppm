// Putting the server, the engine of each platform and the openkal kit into the payload layout,
// and checking that an assembled payload is complete and internally consistent.
//
// ONE PAYLOAD FOR EVERY PLATFORM (payload version 5; ecosystem design 2026-10-29 §9): the server is
// one okb of both architectures, started on each system by the platform's runner, which the
// payload carries for every system it serves; the engine (clangd) is native, one per platform; the
// kit is the openkal kit, whose standard library reads code for any target. The layout is the
// contract the server and the editor extensions rely on:
//
//     <payload>/
//       payload.json                          version, the server, the kit, and per platform its runner,
//                                             engine and integrity-checked files
//       bin/mcppls.okb                        the server: the image of x86_64 and of aarch64
//       libexec/okrun/<system>/okrun[.exe]    the runner of each system (linux-x86_64, macos-aarch64, ...)
//       clangd/<platform>/bin/clangd[.exe]    the engine of each platform (linux-x64, darwin-arm64, ...)
//       clangd/<platform>/lib/clang/<major>/include/...
//       clangd/<platform>/engine.json
//       kit/kit.json + kit data               the openkal kit (spec S4, kit-version 2)
//       licenses/                             mcppls, LLVM and the runner's license texts
export module mcppls.pack.payload;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.pack.lock;

export namespace mcppls::pack::payload {

// Version 5: one payload for every platform. Every engine carries its immutable identity
// (S4-4-10), as version 4 did for its one platform.
inline constexpr int PAYLOAD_VERSION { 5 };

// The engine of one platform: mcppls.pack.clangd::trim's output (or a maintained engine part).
struct PlatformEngine {
    std::string platform;          // one of the lock's platforms
    std::string clangdDirectory;
};

struct AssembleOptions {
    std::string serverImage;                  // the okb of every architecture (`mcpp pack --target openkal --pkg-format okb`)
    // The tree `mcpp pack --target openkal --mode self-contained --systems all` wrote, unpacked: its
    // `libexec/okrun/<system>/` holds the runner of each system, and its mcpp-package.json says
    // where it runs (`runs_on.self`).
    std::string serverTree;
    std::vector<PlatformEngine> engines;      // one per platform the payload serves
    std::string kitDirectory;                 // the openkal kit (mcppls.pack.openkal)
    std::string outDirectory;                 // replaced if it exists
    std::string repositoryRoot;               // LICENSE and (best-effort) git provenance live here
    std::optional<std::string> serverVersion; // defaults to mcppls::base::VERSION
    // A new engine under test (docs/92-release.md, "A new engine"): another fork version of the locked LLVM base is taken,
    // its identity still checked. A release never sets it: its engine is the lock's.
    bool candidateEngine { false };
};

// Assembles the payload directory; returns its path. The inputs are checked before the output
// directory is touched: every server part, engine and the kit must exist and be the right shape,
// and the server tree must run on every platform's system by itself, or nothing is written.
base::Result<std::string> assemble(const AssembleOptions& options, const lock::Lock& lockData);

// The problems found in an assembled payload; empty means it is complete: the manifest's shape,
// the server and every platform's runner and engine, the kit's own rules (spec S4 section 4), the
// integrity files' sha256, and paths that collide only in case.
std::vector<std::string> verify(std::string_view payloadDirectory);

// The problems of a kit by spec S4's rules (data only, std provided, paths present, libc++ the
// version its headers carry, a macOS kit's SDK requirement): a payload's kit and a target kit alike.
std::vector<std::string> verify_kit(std::string_view kitDirectory);

// Copies an already-assembled payload from `source` to `out` -- keeping the executable bits,
// which a copy that writes each file by hand does not -- then verifies the copy.
base::Result<std::string> copy_from(std::string_view source, std::string_view out);

} // namespace mcppls::pack::payload
