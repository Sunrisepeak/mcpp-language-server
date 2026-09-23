import std;
import mcppls.arch;
import mcppls.os;

int main() {
    using namespace mcppls::os;
    const std::string_view arch { mcppls::arch::ARCH == mcppls::arch::Arch::aarch64 ? "arm64" : "x64" };
    bool ok { true };
    if constexpr (FAMILY == Family::windows) {
        ok = EXECUTABLE_SUFFIX == ".exe" && PATH_LIST_SEPARATOR == ';' && PLATFORM == std::format("win32-{}", arch);
    } else if constexpr (FAMILY == Family::macos) {
        ok = EXECUTABLE_SUFFIX.empty() && PATH_LIST_SEPARATOR == ':' && PLATFORM == std::format("darwin-{}", arch);
    } else {
        ok = EXECUTABLE_SUFFIX.empty() && PATH_LIST_SEPARATOR == ':' && PLATFORM == std::format("linux-{}", arch);
    }
    std::println("test_os: family={} platform={} ok={}", FAMILY_NAME, PLATFORM, ok);
    return ok ? 0 : 1;
}
