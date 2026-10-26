// The system this program runs upon, in the names mcppls uses: the family that decides a path's
// spelling and a program's suffix, and the platform a payload, a VSIX and packaging/payload.lock.json
// are keyed by. Each answer is mcpplibs.os's `host()`: a constant expression when the program is
// built for a system, so `if (family() == ...)` folds and nothing of another system is compiled in;
// answered when the image is loaded when it is built as an ok image (`<arch>-openkal`), so one image
// follows the system it runs upon.
export module mcppls.os;

import std;
import mcpplibs.os;

export namespace mcppls::os {

enum class Family { linux, macos, windows };
enum class Arch { x86_64, aarch64 };

// Built as an ok image: one artifact per architecture, which okrun runs on every system.
inline constexpr bool IS_IMAGE { mcpplibs::os::is_image };

// The architecture of the build, which an image shares with the system it runs upon.
inline constexpr Arch ARCH { mcpplibs::os::ARCH == mcpplibs::os::architecture::aarch64 ? Arch::aarch64 : Arch::x86_64 };

constexpr Family family() {
    switch (mcpplibs::os::host()) {
    case mcpplibs::os::platform::windows: return Family::windows;
    case mcpplibs::os::platform::macos: return Family::macos;
    default: return Family::linux;
    }
}

constexpr std::string_view family_name() {
    switch (family()) {
    case Family::windows: return "windows";
    case Family::macos: return "macos";
    case Family::linux: break;
    }
    return "linux";
}

constexpr std::string_view executable_suffix() { return mcpplibs::os::conventions_of(mcpplibs::os::host()).exe_suffix; }
constexpr char path_list_separator() { return mcpplibs::os::conventions_of(mcpplibs::os::host()).path_list_separator; }

// Names compared without case. macOS is counted with Windows: its default volume is
// case-insensitive, and two spellings of one file must be one key.
constexpr bool case_insensitive_paths() { return family() != Family::linux; }

// The architecture as VS Code names it, and this system on it the way VS Code names extension
// targets (linux-x64, linux-arm64, darwin-arm64, win32-x64).
constexpr std::string_view arch_name() { return ARCH == Arch::aarch64 ? "arm64" : "x64"; }

constexpr std::string_view platform() {
    const bool arm { ARCH == Arch::aarch64 };
    switch (family()) {
    case Family::windows: return arm ? "win32-arm64" : "win32-x64";
    case Family::macos: return arm ? "darwin-arm64" : "darwin-x64";
    case Family::linux: break;
    }
    return arm ? "linux-arm64" : "linux-x64";
}

} // namespace mcppls::os
