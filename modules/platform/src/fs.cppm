// Files and directories. Paths are strings in the normalized '/' form of
// mcppls.base.path; Windows paths keep their drive ("C:/Users/x").
export module mcppls.platform.fs;

import std;
import mcppls.base.error;

export namespace mcppls::platform::fs {

struct FileStamp {
    std::uint64_t size { 0 };
    std::int64_t modified { 0 };   // nanoseconds since the file-clock epoch; only compared for equality
    auto operator<=>(const FileStamp&) const = default;
};

bool exists(std::string_view path);
bool is_directory(std::string_view path);
bool is_regular_file(std::string_view path);
std::optional<FileStamp> stamp(std::string_view path);

base::Result<std::string> read_file(std::string_view path);
base::Result<void> write_file(std::string_view path, std::string_view content);
// Writes a sibling temporary file and renames it over `path`.
base::Result<void> write_file_atomic(std::string_view path, std::string_view content);
base::Result<void> create_directories(std::string_view path);

// Sets the executable bit on `paths`, and does nothing on a platform where a file has none.
//
// It sets all three execute bits together because that is the only request openkal-musl can grant:
// it reports one read triple and one write triple, each all-set or all-clear, and refuses with
// ENOSYS any mode it could not report back rather than rounding it. So asking for the owner's bit
// alone -- the narrower, more careful-looking request -- is the one that fails
// (mcpplibs/openkal#31, answered in openkal 0.13 / openkal-musl 0.14).
base::Result<void> make_executable(std::span<const std::string> paths);
void remove_all(std::string_view path);

// Regular files under `root` whose extension (".cppm") is in `extensions`,
// sorted. Directories named in `skipDirectories` and directories starting with
// '.' are not entered. An empty `extensions` accepts every file.
std::vector<std::string> list_files(std::string_view root, std::span<const std::string_view> extensions,
                                    std::span<const std::string_view> skipDirectories);
// Immediate children (files and directories) of a directory, sorted.
std::vector<std::string> list_directory(std::string_view path);

std::string current_directory();

// What the file system itself calls a file: a volume and an index on it. Two
// names reach one file exactly when their identities are equal.
struct FileIdentity {
    std::uint64_t volume { 0 };
    std::uint64_t index { 0 };
    auto operator<=>(const FileIdentity&) const = default;
};
std::optional<FileIdentity> file_identity(std::string_view path);

// One name for a file that several names reach, for comparing names: every
// symbolic link on the way is followed (/var is /private/var on macOS), and on
// Windows every short alias is replaced by its long name (RUNNER~1 by
// runneradmin). The part of a path that does not exist is kept as written.
std::string canonical_path(std::string_view path);

} // namespace mcppls::platform::fs
