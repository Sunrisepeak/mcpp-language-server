// Lexical path handling that works the same for POSIX and Windows spellings.
// Paths are kept with '/' separators; Windows paths keep their drive letter
// ("C:/Users/x"). std::filesystem is not used for these rules because the
// C library beneath this program spells names the POSIX way on every system.
// The last section asks the file system, through std::filesystem and
// mcpplibs.os, and gives the answer in this spelling.
export module mcppls.base.path;

import std;
import mcpplibs.os;
import mcppls.base.error;

export namespace mcppls::base {

enum class PathStyle { posix, windows };

// The spelling of the system this program runs upon (mcpplibs.os `host()`: a constant expression
// when it is built for a system, answered at load when it is built as an ok image).
constexpr PathStyle native_path_style() {
    return mcpplibs::os::host() == mcpplibs::os::platform::windows ? PathStyle::windows : PathStyle::posix;
}

bool is_absolute_path(std::string_view path, PathStyle style = native_path_style());
std::string normalize_path(std::string_view path, PathStyle style = native_path_style());
std::string join_path(std::string_view base, std::string_view child, PathStyle style = native_path_style());
std::string parent_path(std::string_view path, PathStyle style = native_path_style());
std::string_view file_name(std::string_view path);
std::string_view extension(std::string_view path);
std::optional<std::string> relative_path(std::string_view path, std::string_view base, PathStyle style = native_path_style());
// Names compared without case where the host's names are (macOS counted with Windows: its default
// volume is case-insensitive, and two spellings of one file must be one key).
std::string path_key(std::string_view path, bool caseInsensitive = mcpplibs::os::conventions().case_insensitive_names);
bool same_path(std::string_view a, std::string_view b, bool caseInsensitive = mcpplibs::os::conventions().case_insensitive_names);
bool is_within(std::string_view path, std::string_view directory, bool caseInsensitive = mcpplibs::os::conventions().case_insensitive_names);

// ---- asking the file system, in this spelling ----------------------------------------------------

// A name std::filesystem or mcpplibs.os gave, in this spelling: UTF-8, '/' separators, normalized.
std::string spelled(const std::filesystem::path& path);
// The entries of a directory, joined to it and sorted; none when it cannot be read.
std::vector<std::string> children(std::string_view directory);
// The regular files under `root` whose extension (".cppm") is in `extensions` (any, when empty),
// sorted. Directories named in `skipDirectories`, directories whose name starts with '.', and links
// to directories are not entered.
std::vector<std::string> files_under(std::string_view root, std::span<const std::string_view> extensions,
                                     std::span<const std::string_view> skipDirectories);
// The working directory; empty when it cannot be read.
std::string current_directory();
// mcpplibs.os `fs::canonical`: one name for a file several names reach (links followed; on Windows
// short components replaced by their long names). The part that does not exist is kept as written.
std::string canonical_path(std::string_view path);
// Each file may be started (mcpplibs.os `fs::set_executable`). A volume that does not record it (Windows decides by
// the extension) has nothing to grant and nothing that could have been withheld: that answer is success.
Result<void> make_executable(std::span<const std::string> paths);
// mcpplibs.os `fs::stamp_of`, as a value that is there or not: what the caches here compare and keep.
std::optional<mcpplibs::os::fs::stamp> file_stamp(std::string_view path);
// The file clock's reading of now, in the unit mcpplibs.os `fs::stamp_of` gives a file's modification in: what a
// file written at this moment would carry.
std::int64_t file_time_now();

} // namespace mcppls::base
