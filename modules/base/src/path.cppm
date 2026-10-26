// Lexical path handling that works the same for POSIX and Windows spellings.
// Paths are kept with '/' separators; Windows paths keep their drive letter
// ("C:/Users/x"). std::filesystem is not used for these rules because the
// C library beneath this program spells names the POSIX way on every system.
export module mcppls.base.path;

import std;
import mcppls.os;

export namespace mcppls::base {

enum class PathStyle { posix, windows };

// The spelling of the system this program runs upon (mcppls.os: a constant expression when it is
// built for a system, answered at load when it is built as an ok image).
constexpr PathStyle native_path_style() {
    return mcppls::os::family() == mcppls::os::Family::windows ? PathStyle::windows : PathStyle::posix;
}

bool is_absolute_path(std::string_view path, PathStyle style = native_path_style());
std::string normalize_path(std::string_view path, PathStyle style = native_path_style());
std::string join_path(std::string_view base, std::string_view child, PathStyle style = native_path_style());
std::string parent_path(std::string_view path, PathStyle style = native_path_style());
std::string_view file_name(std::string_view path);
std::string_view extension(std::string_view path);
std::optional<std::string> relative_path(std::string_view path, std::string_view base, PathStyle style = native_path_style());
std::string path_key(std::string_view path, bool caseInsensitive = mcppls::os::case_insensitive_paths());
bool same_path(std::string_view a, std::string_view b, bool caseInsensitive = mcppls::os::case_insensitive_paths());
bool is_within(std::string_view path, std::string_view directory, bool caseInsensitive = mcppls::os::case_insensitive_paths());

} // namespace mcppls::base
