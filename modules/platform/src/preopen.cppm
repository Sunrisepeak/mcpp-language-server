// Absolute names over openkal's supplied directories. openkal names a file
// relative to a directory the program holds; this finds the supplied directory
// an absolute name begins with and the rest of the name beneath it.
export module mcppls.platform.preopen;

import std;
import openkal.fs;

export namespace mcppls::platform {

struct ResolvedName {
    kal_dir directory {};
    std::string preopenName;   // the supplied directory's own name, normalized
    std::string remainder;     // the rest, without a leading separator; empty for the directory itself
};

// The longest supplied directory `absolutePath` begins with, or nullopt when the
// name is beneath none of them.
std::optional<ResolvedName> resolve_name(std::string_view absolutePath);

// Whether some supplied directory is the file system's root, by name. On a POSIX system openkal
// supplies "/" and every program path resolves beneath it; when none is named "/" every start fails
// as "outside every preopened directory". That is the symptom of an openkal built with input-only
// system-call registers running under PRoot's seccomp mode, which rewrites a register the compiler
// then trusted (vendor/README.md), and it is worth saying at startup rather than at the first spawn.
// A system with no root directory (Windows) is never asked: true.
bool has_root_preopen(std::span<const std::string> names);
// The supplied directories' names, as openkal reports them; the same table resolve_name reads.
std::vector<std::string> preopen_names();

} // namespace mcppls::platform
