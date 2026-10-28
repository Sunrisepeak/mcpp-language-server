// The names a payload, a VSIX and packaging/payload.lock.json are keyed by (mcppls.base.payload), against what
// mcpplibs.os says of this system and architecture.
import std;
import mcpplibs.os;
import mcppls.base.payload;

int main() {
    namespace os = mcpplibs::os;
    using mcppls::base::payload_arch;
    using mcppls::base::payload_platform;
    const std::string_view arch { payload_arch() };
    bool ok { arch == (os::ARCH == os::architecture::aarch64 ? "arm64" : "x64") };
    const auto conventions = os::conventions();
    switch (os::host()) {
    case os::platform::windows:
        ok = ok && conventions.exe_suffix == ".exe" && conventions.path_list_separator == ';' && payload_platform() == std::format("win32-{}", arch);
        break;
    case os::platform::macos:
        ok = ok && conventions.exe_suffix.empty() && conventions.path_list_separator == ':' && payload_platform() == std::format("darwin-{}", arch);
        break;
    default:
        ok = ok && conventions.exe_suffix.empty() && conventions.path_list_separator == ':' && payload_platform() == std::format("linux-{}", arch);
        break;
    }
    std::println("test_os: system={} platform={} image={} ok={}", os::name(os::host()), payload_platform(), os::is_image, ok);
    return ok ? 0 : 1;
}
