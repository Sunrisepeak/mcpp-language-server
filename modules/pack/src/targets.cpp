module mcppls.pack.targets;

import std;

namespace mcppls::pack::targets {

std::optional<Target> parse(std::string_view name) {
    const auto dash = name.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    const std::string_view os { name.substr(0, dash) };
    const std::string_view arch { name.substr(dash + 1) };
    Target target {};
    if (os == "linux") target.os = Os::linux;
    else if (os == "darwin") target.os = Os::darwin;
    else if (os == "win32") target.os = Os::win32;
    else return std::nullopt;
    if (arch == "x64") target.arch = Arch::x64;
    else if (arch == "arm64") target.arch = Arch::arm64;
    else return std::nullopt;
    return target;
}

std::string_view executable_suffix(Target target) { return target.os == Os::win32 ? ".exe" : ""; }

std::string_view xlings_os(Os os) {
    switch (os) {
    case Os::darwin: return "macosx";
    case Os::win32: return "windows";
    default: return "linux";
    }
}

std::string_view xlings_arch(Arch arch) { return arch == Arch::arm64 ? "aarch64" : "x86_64"; }

std::string_view apple_arch(Arch arch) { return arch == Arch::arm64 ? "arm64" : "x86_64"; }

} // namespace mcppls::pack::targets
