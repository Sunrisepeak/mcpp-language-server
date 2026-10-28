// The platform a payload, a VSIX and packaging/payload.lock.json are keyed by: this system and
// architecture the way VS Code names extension targets (linux-x64, linux-arm64, darwin-arm64,
// win32-x64). mcpplibs.os names the system (`host()`, answered at load in an ok image) and the
// architecture (`ARCH`, which an image shares with the system it runs upon); these are mcppls's
// names for them.
export module mcppls.base.payload;

import std;
import mcpplibs.os;

export namespace mcppls::base {

constexpr std::string_view payload_arch() { return mcpplibs::os::ARCH == mcpplibs::os::architecture::aarch64 ? "arm64" : "x64"; }

constexpr std::string_view payload_platform() {
    const bool arm { mcpplibs::os::ARCH == mcpplibs::os::architecture::aarch64 };
    switch (mcpplibs::os::host()) {
    case mcpplibs::os::platform::windows: return arm ? "win32-arm64" : "win32-x64";
    case mcpplibs::os::platform::macos: return arm ? "darwin-arm64" : "darwin-x64";
    default: break;
    }
    return arm ? "linux-arm64" : "linux-x64";
}

} // namespace mcppls::base
