// Fetching a file named by a lock entry and proving it is the right one.
//
// This is the only module in this repository that reaches another machine, and it exists for
// packaging: the payload's inputs (a clangd build, the libc++ sources) are downloaded here rather
// than by a Python script that every developer and every CI runner had to have an interpreter for.
//
// `mcppls.platform.net` is the deliberate opposite — loopback only, because the server must not
// reach the network. That is enforced by the package boundary, not by this comment: this lives in
// mcppls-pack, which only mcppls-devtools depends on, and `devtools check binary` fails if the
// server's symbol table ever carries a TLS or archive symbol.
export module mcppls.pack.fetch;

import std;
import mcppls.base.error;

export namespace mcppls::pack::fetch {

// One row of packaging/payload.lock.json: what to get, and what proves it arrived intact.
struct Entry {
    std::string file;                        // the name it is cached under
    std::string url;
    std::string sha256;                      // lowercase hex
    std::optional<std::uint64_t> size {};    // checked as well when the lock records it
};

// The lowercase hex digest of a file, read in blocks so a 400 MB toolchain does not have to fit
// in memory to be verified.
base::Result<std::string> digest_of(std::string_view path);

// The absolute path of a verified copy of `entry` under `cacheDir`.
//
// A cached file whose digest matches is not fetched again; one that does not match is replaced,
// because a truncated or tampered cache entry is the failure this function exists to survive. The
// download lands on a `.partial` name and is renamed only after it verifies, so an interrupted run
// cannot leave something that looks cached.
//
// Network errors and verification failures are retried alike: both are transient often enough that
// distinguishing them costs more than repeating the request.
base::Result<std::string> get(const Entry& entry, std::string_view cacheDir, int retries = 4,
                              bool quiet = false);

} // namespace mcppls::pack::fetch
