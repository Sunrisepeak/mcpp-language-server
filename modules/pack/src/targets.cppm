// What a platform name says: `<os>-<arch>`, spelled the way VS Code names extension targets
// (linux-x64, linux-arm64, darwin-arm64, win32-x64).
//
// Which platforms exist is packaging/payload.lock.json's `platforms` and nothing else: every
// consumer that asks "is this a platform?" asks the lock. This module only reads a name, so the
// facts that follow from it -- an .exe suffix, a Linux C library, xlings' spelling of the pair --
// are derived in one place instead of being compared against one literal name at a time.
export module mcppls.pack.targets;

import std;

export namespace mcppls::pack::targets {

enum class Os { linux, darwin, win32 };
enum class Arch { x64, arm64 };

struct Target {
    Os os;
    Arch arch;
};

// `name` read as `<os>-<arch>`; nullopt for anything else.
std::optional<Target> parse(std::string_view name);

// ".exe" on win32, "" elsewhere.
std::string_view executable_suffix(Target target);

// xlings' spelling of the pair (payload.lock.json's history and the .lua.in templates):
// linux / macosx / windows, x86_64 / aarch64.
std::string_view xlings_os(Os os);
std::string_view xlings_arch(Arch arch);

// The architecture as Apple's tools name it (lipo -thin, CMAKE_OSX_ARCHITECTURES).
std::string_view apple_arch(Arch arch);

} // namespace mcppls::pack::targets
