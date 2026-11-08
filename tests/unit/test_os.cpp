// The names a payload, a VSIX and packaging/payload.lock.json are keyed by (mcppls.base.payload), against what
// mcpplibs.os says of this system and architecture.
import std;
import mcpplibs.os;
import mcppls.base.payload;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;
namespace os = mcpplibs::os;

UTest os_tests{"the payload's names of this system", [] {
    using mcppls::base::payload_arch;
    using mcppls::base::payload_platform;
    const std::string_view arch { payload_arch() };
    expect(arch == (os::ARCH == os::architecture::aarch64 ? "arm64" : "x64")) << arch;
    const auto conventions = os::conventions();
    switch (os::host()) {
    case os::platform::windows:
        expect(conventions.exe_suffix == ".exe" && conventions.path_list_separator == ';');
        expect(payload_platform() == std::format("win32-{}", arch)) << payload_platform();
        break;
    case os::platform::macos:
        expect(conventions.exe_suffix.empty() && conventions.path_list_separator == ':');
        expect(payload_platform() == std::format("darwin-{}", arch)) << payload_platform();
        break;
    default:
        expect(conventions.exe_suffix.empty() && conventions.path_list_separator == ':');
        expect(payload_platform() == std::format("linux-{}", arch)) << payload_platform();
        break;
    }
}};
