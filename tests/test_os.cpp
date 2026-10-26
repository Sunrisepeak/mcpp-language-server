import std;
import mcppls.os;

int main() {
    using namespace mcppls::os;
    const std::string_view arch { arch_name() };
    bool ok { arch == (ARCH == Arch::aarch64 ? "arm64" : "x64") };
    if (family() == Family::windows) {
        ok = ok && executable_suffix() == ".exe" && path_list_separator() == ';' && platform() == std::format("win32-{}", arch);
    } else if (family() == Family::macos) {
        ok = ok && executable_suffix().empty() && path_list_separator() == ':' && platform() == std::format("darwin-{}", arch);
    } else {
        ok = ok && executable_suffix().empty() && path_list_separator() == ':' && platform() == std::format("linux-{}", arch);
    }
    std::println("test_os: family={} platform={} image={} ok={}", family_name(), platform(), IS_IMAGE, ok);
    return ok ? 0 : 1;
}
