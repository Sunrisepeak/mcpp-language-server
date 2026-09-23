// Putting the server, clangd and the semantic kit into the payload layout, and checking that an
// assembled payload is complete and internally consistent.
//
// This ports assemble_payload.py, including its `verify()` (also reachable as `--verify PAYLOAD`
// and, having already verified what it just built, at the end of assembling) and its `--from`
// (copy an already-assembled payload, keeping the executable bits, then verify the copy). The
// layout is the contract the server and the editor extensions rely on:
//
//     <payload>/
//       payload.json                  versions and relative paths of the three parts
//       bin/mcppls[.exe]
//       clangd/bin/clangd[.exe]
//       clangd/lib/clang/<major>/include/...
//       kit/kit.json + kit data        (spec S4)
//       licenses/                      mcppls and LLVM license texts
export module mcppls.pack.payload;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.pack.lock;

export namespace mcppls::pack::payload {

// usable plan W9.4: records size and sha256 of clangd and the kit manifest, so the server can tell
// a corrupt or tampered payload from a working one at startup. overall design 5.6 added `engines`.
inline constexpr int PAYLOAD_VERSION { 3 };

struct AssembleOptions {
    std::string platform;                      // one of the lock's platforms
    std::string serverPath;                    // the mcppls executable built for `platform`
    std::string clangdDirectory;                // produced by mcppls.pack.clangd::trim
    std::string kitDirectory;                   // produced by mcppls.pack.kit (ported separately)
    std::string outDirectory;                   // replaced if it exists
    std::string repositoryRoot;                 // LICENSE and (best-effort) git provenance live here
    std::optional<std::string> serverVersion;   // defaults to mcppls::base::VERSION
};

// Assembles the payload directory; returns its path. Every check assemble_payload.py made before
// writing anything survives here as a base::fail before this function touches the output
// directory: the four inputs must exist and be the right shape, or nothing is written at all.
base::Result<std::string> assemble(const AssembleOptions& options, const lock::Lock& lockData);

// The problems found in an assembled payload; empty means it is complete. Every one of
// assemble_payload.py's `verify()` checks -- the manifest shape, the four parts it names, the
// kit's own rules (spec S4 section 4), the startup integrity files' sha256, and paths that
// collide only in case -- is here, in the same order.
std::vector<std::string> verify(std::string_view payloadDirectory);

// Copies an already-assembled payload from `source` to `out` -- keeping the executable bits,
// which a copy that writes each file by hand does not -- then verifies the copy. This is
// assemble_payload.py's `--from`: a caller that already has a payload (CI, which built one per
// platform in an earlier job) puts it where a packaging step wants it.
base::Result<std::string> copy_from(std::string_view source, std::string_view out);

} // namespace mcppls::pack::payload
