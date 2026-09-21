// Lexical path handling that works the same for POSIX and Windows spellings.
// Paths are kept with '/' separators; Windows paths keep their drive letter
// ("C:/Users/x"). std::filesystem is not used for these rules because the
// C library beneath this program spells names the POSIX way on every system.
export module mcppls.base.path;

import std;
import mcppls.os;

export namespace mcppls::base {

enum class PathStyle { posix, windows };

inline constexpr PathStyle NATIVE_PATH_STYLE {
    mcppls::os::FAMILY == mcppls::os::Family::windows ? PathStyle::windows : PathStyle::posix
};

bool is_absolute_path(std::string_view path, PathStyle style = NATIVE_PATH_STYLE);
std::string normalize_path(std::string_view path, PathStyle style = NATIVE_PATH_STYLE);
std::string join_path(std::string_view base, std::string_view child, PathStyle style = NATIVE_PATH_STYLE);
std::string parent_path(std::string_view path, PathStyle style = NATIVE_PATH_STYLE);
std::string_view file_name(std::string_view path);
std::string_view extension(std::string_view path);
std::optional<std::string> relative_path(std::string_view path, std::string_view base, PathStyle style = NATIVE_PATH_STYLE);
std::string path_key(std::string_view path, bool caseInsensitive = mcppls::os::CASE_INSENSITIVE_PATHS);
bool same_path(std::string_view a, std::string_view b, bool caseInsensitive = mcppls::os::CASE_INSENSITIVE_PATHS);
bool is_within(std::string_view path, std::string_view directory, bool caseInsensitive = mcppls::os::CASE_INSENSITIVE_PATHS);

} // namespace mcppls::base
