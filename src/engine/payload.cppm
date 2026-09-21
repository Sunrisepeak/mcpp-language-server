// Where the engine and the semantic kit come from: the payload bundled with an
// editor extension, an xlings installation, or explicit paths (design 15.3).
export module mcppls.engine.payload;

import std;
import mcppls.base.error;

export namespace mcppls::engine {

// The size and sha256 a payload.json "files" entry declares for one of its own files (usable
// plan W9.4, design 18 "payload integrity"): `mcppls-devtools payload` (mcppls.pack.payload) writes these; the server checks
// them at startup.
struct PayloadFileIntegrity {
    std::uint64_t size { 0 };
    std::string sha256;
};

struct PayloadPaths {
    std::string directory;     // empty when no payload is used
    std::string clangd;        // absolute executable path, or empty
    std::string clangdVersion;
    std::string kit;           // the kit root that matches the core engine (S4-4-5), or empty
    std::string kitNotice;     // why the payload's kit was not taken: its libc++ is not the engine's version
    std::string platform;
    // Path relative to `directory` -> expected size/sha256. Empty when payload.json declares none
    // (an older payload, or no payload at all): nothing is checked.
    std::map<std::string, PayloadFileIntegrity> files;
};

struct PayloadRequest {
    std::string payloadDirectory;   // --payload
    std::string clangd;             // --clangd
    std::string kit;                // --kit
    std::string engine { "clangd" };   // the core engine, whose version decides the kit; none: any kit
};

// Explicit paths win; then the payload named or enclosing this executable (payload.json's
// `engines`, or its parts in versions 1 and 2, else the conventional layout); then clangd on PATH.
// The kit is the one whose libc++ is the core engine's version (overall design 5.6): the payload's,
// else that version installed by xlings.
PayloadPaths resolve_payload(const PayloadRequest& request);
// The macOS SDK path for kits that need one, or empty.
std::string macos_sdk_path();

// One of `payload.files`'s entries did not match on disk.
struct PayloadIntegrityIssue {
    std::string path;      // relative to the payload directory, as in `payload.files`
    std::string reason;    // human-readable, e.g. "is 512 bytes, expected 91234567"
};

// Checks every file `payload.files` declares: size first (cheap, catches a truncated file
// outright), then sha256. A hash is computed once per (path, size, modification time) and kept in
// `cacheFile` (JSON), so an untouched payload costs a startup no more than the size checks and a
// handful of stamp lookups. Empty when the payload is intact or declares no files to check.
std::vector<PayloadIntegrityIssue> verify_payload_integrity(const PayloadPaths& payload, std::string_view cacheFile);

} // namespace mcppls::engine
