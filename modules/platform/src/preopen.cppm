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

} // namespace mcppls::platform
