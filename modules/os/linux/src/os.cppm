// Platform facts for one target. Exactly one of the three os packages is in the
// dependency graph of a build, chosen by the target, so code branches on these
// constants with `if constexpr` instead of the preprocessor.
export module mcppls.os;

import std;

export namespace mcppls::os {

enum class Family { linux, macos, windows };

inline constexpr Family FAMILY { Family::linux };
inline constexpr std::string_view FAMILY_NAME { "linux" };
inline constexpr std::string_view EXECUTABLE_SUFFIX { "" };
inline constexpr char PATH_LIST_SEPARATOR { ':' };
inline constexpr std::string_view VSCODE_TARGET { "linux-x64" };
inline constexpr bool CASE_INSENSITIVE_PATHS { false };


} // namespace mcppls::os
