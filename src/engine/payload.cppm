// Where the engine and the semantic kits come from: the payload bundled with an editor extension,
// an xlings installation, or explicit paths (design 15.3; ecosystem design 2026-10-29 §9).
//
// One payload serves every platform (payload version 5): the server image, the runner of each
// system, the engine built for each platform and the openkal kit. The server takes its own
// platform's entry. The kit is chosen per project target (S4 section 5): the target's own kit when
// one is installed, else the openkal kit the payload carries, with the kit that would serve the
// target named so the person can install it.
export module mcppls.engine.payload;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.engineidentity;
import mcppls.spec.kit;

export namespace mcppls::engine {

// The size and sha256 a payload.json "files" entry declares for one of its own files (usable
// plan W9.4, design 18 "payload integrity"): `mcppls-devtools payload` (mcppls.pack.payload) writes these; the server checks
// them at startup.
struct PayloadFileIntegrity {
    std::uint64_t size { 0 };
    std::string sha256;
};

// What a payload.json says for one platform: payload version 5's `platforms.<platform>`, or the one
// platform of versions 1 to 4. Paths are relative to the payload directory.
struct PayloadEntry {
    std::string platform;                                   // linux-x64, linux-arm64, darwin-arm64, win32-x64
    std::string runner;                                     // the program that starts the server image here; empty for a program server
    std::string clangd;                                     // the engine built for this platform
    std::string clangdRunner;                               // the program that starts it, for an ok image (clangd.okb); empty for a program
    std::string clangdVersion;
    nlohmann::json identity;                                // engines.clangd.identity; null when not stated
    std::map<std::string, PayloadFileIntegrity> files;      // this platform's own integrity-checked files
};

// payload.json read without the file system.
struct PayloadManifest {
    int version { 0 };
    std::string serverPath;                                 // relative; bin/mcppls.okb, or a program
    std::string kitPath;                                    // relative; the kit the payload carries (the openkal kit from version 5)
    std::vector<PayloadEntry> entries;                      // one per platform, in the manifest's order
    std::map<std::string, PayloadFileIntegrity> files;      // integrity-checked files every platform shares
};

// Version 5 is one payload for every platform; 4 binds an engine identity; 3 adds `engines`; 2 `files`.
inline constexpr int HIGHEST_PAYLOAD_VERSION { 5 };

// payload.json's text read; an error names what is malformed.
base::Result<PayloadManifest> read_payload_manifest(std::string_view text);

// The entry of `platform`, or a sentence that says which platforms the payload carries instead.
std::expected<PayloadEntry, std::string> payload_entry(const PayloadManifest& manifest, std::string_view platform);

struct PayloadPaths {
    std::string directory;     // empty when no payload is used
    std::string clangd;        // absolute executable path, or empty
    // The program that starts the engine when it is an ok image (`okrun clangd.okb`, ecosystem design
    // 2026-10-30 part 2 §25.3): absolute, or empty for an engine that is a program of this system.
    std::string clangdRunner;
    std::string clangdVersion;
    std::string kit;           // the kit for this host's own target (S4 section 5), or empty
    std::vector<std::string> kits;   // every kit root to choose from, in order: --kit, the payload's, the installed ones
    bool explicitKit { false };      // kits.front() is --kit, which serves every target
    std::string payloadKit;          // the kit the payload carries (the openkal kit), or empty
    std::string kitVersion;          // the version an installed target kit is taken at: the engine's LLVM base; empty: any
    std::string kitNotice;     // why an installed kit was not taken: its libc++ is not the engine's version
    std::string platform;
    std::vector<std::string> platforms;   // every platform the payload carries an entry for
    // Path relative to `directory` -> expected size/sha256: the shared files and this platform's own.
    // Empty when payload.json declares none (an older payload, or no payload at all): nothing is checked.
    std::map<std::string, PayloadFileIntegrity> files;
    std::optional<base::EngineIdentity> engineIdentity;
    std::string identityProblem;
};

struct PayloadRequest {
    std::string payloadDirectory;   // --payload
    std::string clangd;             // --clangd
    std::string kit;                // --kit
    std::string engine { "clangd" };   // the core engine, whose version decides the installed kits taken; none: any
};

// How the engine is started: the runner and the engine as its first argument, or the engine itself.
struct EngineCommand {
    std::string program;
    std::vector<std::string> arguments;   // before the engine's own
};
EngineCommand engine_command(std::string_view clangd, std::string_view runner);

// Explicit paths win; then the payload named or enclosing this executable (payload.json's entry
// for this platform, else the conventional layout); then clangd on PATH. The kits are --kit, the
// payload's and those installed by xlings whose libc++ is the core engine's version.
PayloadPaths resolve_payload(const PayloadRequest& request);
// The macOS SDK path for kits that need one, or empty.
std::string macos_sdk_path();

// ---- the kit of a target (S4 section 5) -------------------------------------------------------

// A target as a kit names it: `x86_64-linux-gnu`, `aarch64-linux-gnu`, `x86_64-w64-mingw32` (every
// Windows target), `arm64-apple-darwin`, `<arch>-openkal`; any other triple with its architecture
// spelled as mcpp spells it.
std::string kit_target_name(std::string_view triple);
// This host's own target, as a kit names it.
std::string host_kit_target();
// The kit packages published for targets (xlings `mcppls-kit-<target>` and the release's
// `mcppls-kit-<target>-<version>.tar.gz`): the target names they serve.
std::span<const std::string_view> published_kit_targets();

struct KitChoice {
    std::optional<spec::Kit> kit;   // the kit as it serves the target (a variant flattened), or none
    std::string target;             // the target the choice was made for, as a kit names it
    // When the kit stands in for the target's own (the openkal kit, or the host's for another
    // target): the published kit that would serve it, `x86_64-linux-gnu`. Empty when the kit is the
    // target's own, or when no kit is published for it.
    std::string missingTarget;
};

// The kit for `target` (empty: the host's) among `kits`, in their order: --kit when `explicitKit`,
// for every target; else the target's own kit (a variant of a kit of several targets, or a kit
// of that one target); else the host's own kit; else the openkal kit of the host's architecture.
KitChoice choose_kit(std::span<const spec::Kit> kits, bool explicitKit, std::string_view target, std::string_view host);

// `payload.kits` as they are now: --kit alone, else the payload's and those xlings installed by now
// (a kit installed while the server runs is found).
std::vector<std::string> kit_roots(const PayloadPaths& payload);
// Every kit of `payload.kits` that loads, in order; a kit that does not is logged and left out.
std::vector<spec::Kit> load_kits(const PayloadPaths& payload);
// load_kits, then choose_kit for `target` on this host.
KitChoice kit_for_target(const PayloadPaths& payload, std::string_view target);

// The kit roots xlings installed: `mcppls-kit-<target>` at `wantedVersion` (empty: the newest),
// the openkal kit at its newest, and the kit of 0.0.19 and earlier (`mcppls-kit`).
std::vector<std::string> installed_kits(std::string_view wantedVersion);

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
