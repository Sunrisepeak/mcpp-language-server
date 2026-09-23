// Platform facts for one target. Exactly one of the three os packages is in the
// dependency graph of a build, chosen by the target, so code branches on these
// constants with `if constexpr` instead of the preprocessor.
export module mcppls.os;

import std;
import mcppls.arch;

export namespace mcppls::os {

enum class Family { linux, macos, windows };

inline constexpr Family FAMILY { Family::windows };
inline constexpr std::string_view FAMILY_NAME { "windows" };
inline constexpr std::string_view EXECUTABLE_SUFFIX { ".exe" };
inline constexpr char PATH_LIST_SEPARATOR { ';' };
// This operating system on this architecture, named the way VS Code names extension targets
// (linux-x64, linux-arm64, darwin-arm64, win32-x64): what a payload, a VSIX and the platforms of
// packaging/payload.lock.json are keyed by.
inline constexpr std::string_view PLATFORM { mcppls::arch::ARCH == mcppls::arch::Arch::aarch64 ? "win32-arm64" : "win32-x64" };
inline constexpr bool CASE_INSENSITIVE_PATHS { true };


} // namespace mcppls::os
